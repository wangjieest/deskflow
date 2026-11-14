/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileTransfer.h"

#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#include <direct.h>
#define PATH_SEPARATOR "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEPARATOR "/"
#endif

uint32_t FileTransfer::s_nextRequestId = 1;
FileRequestCallback FileTransfer::s_fileRequestCallback;
std::string FileTransfer::s_sourceAddress;
uint16_t FileTransfer::s_sourcePort = 0;
uint64_t FileTransfer::s_sourceSessionId = 0;

uint32_t FileTransfer::generateRequestId()
{
  return s_nextRequestId++;
}

std::string FileTransfer::createFileRequest(uint32_t requestId, const std::string &filePath)
{
  std::string data;
  data.push_back(static_cast<char>((requestId >> 24) & 0xFF));
  data.push_back(static_cast<char>((requestId >> 16) & 0xFF));
  data.push_back(static_cast<char>((requestId >> 8) & 0xFF));
  data.push_back(static_cast<char>(requestId & 0xFF));
  data.append(filePath);
  return data;
}

std::string FileTransfer::createFileChunk(uint32_t requestId, FileChunkType chunkType,
                                           const std::string &chunkData)
{
  std::string data;
  data.push_back(static_cast<char>((requestId >> 24) & 0xFF));
  data.push_back(static_cast<char>((requestId >> 16) & 0xFF));
  data.push_back(static_cast<char>((requestId >> 8) & 0xFF));
  data.push_back(static_cast<char>(requestId & 0xFF));
  data.push_back(static_cast<char>(chunkType));
  data.append(chunkData);
  return data;
}

std::string FileTransfer::createStartChunk(uint32_t requestId, const std::string &fileName,
                                            uint64_t fileSize)
{
  std::ostringstream json;
  json << "{\"name\":\"";
  for (char c : fileName) {
    switch (c) {
    case '"':  json << "\\\""; break;
    case '\\': json << "\\\\"; break;
    case '\n': json << "\\n";  break;
    case '\r': json << "\\r";  break;
    case '\t': json << "\\t";  break;
    default:   json << c;
    }
  }
  json << "\",\"size\":" << fileSize << "}";
  return createFileChunk(requestId, FileChunkType::Start, json.str());
}

std::string FileTransfer::createErrorChunk(uint32_t requestId, const std::string &errorMessage)
{
  return createFileChunk(requestId, FileChunkType::Error, errorMessage);
}

bool FileTransfer::parseStartChunk(const std::string &data, std::string &fileName,
                                    uint64_t &fileSize)
{
  fileName.clear();
  fileSize = 0;

  size_t nameStart = data.find("\"name\":\"");
  if (nameStart == std::string::npos)
    return false;
  nameStart += 8;

  size_t nameEnd = nameStart;
  while (nameEnd < data.size()) {
    if (data[nameEnd] == '"' && (nameEnd == 0 || data[nameEnd - 1] != '\\'))
      break;
    nameEnd++;
  }

  for (size_t i = nameStart; i < nameEnd; ++i) {
    if (data[i] == '\\' && i + 1 < nameEnd) {
      i++;
      switch (data[i]) {
      case 'n': fileName += '\n'; break;
      case 'r': fileName += '\r'; break;
      case 't': fileName += '\t'; break;
      default:  fileName += data[i];
      }
    } else {
      fileName += data[i];
    }
  }

  size_t sizeStart = data.find("\"size\":");
  if (sizeStart == std::string::npos)
    return false;
  sizeStart += 7;
  while (sizeStart < data.size() && data[sizeStart] == ' ')
    sizeStart++;
  while (sizeStart < data.size() && data[sizeStart] >= '0' && data[sizeStart] <= '9') {
    fileSize = fileSize * 10 + (data[sizeStart] - '0');
    sizeStart++;
  }

  return !fileName.empty();
}

bool FileTransfer::sendFile(deskflow::IStream *stream, uint32_t requestId,
                             const std::string &filePath, size_t chunkSize)
{
  LOG_INFO("sending file: %s (requestId=%u)", filePath.c_str(), requestId);

  std::ifstream file(pathFromUtf8(filePath), std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_ERR("failed to open file for transfer: %s", filePath.c_str());
    std::string errorMsg = "File not found";
    ProtocolUtil::writef(stream, kMsgDFileChunk, requestId,
                         static_cast<uint8_t>(FileChunkType::Error), &errorMsg);
    return false;
  }

  uint64_t fileSize = static_cast<uint64_t>(file.tellg());
  file.seekg(0, std::ios::beg);

  std::string fileName = filePath;
  size_t lastSlash = filePath.find_last_of("/\\");
  if (lastSlash != std::string::npos)
    fileName = filePath.substr(lastSlash + 1);

  // A quote or backslash in the name used to produce JSON the receiver could not parse
  const QJsonObject meta{
      {QStringLiteral("name"), QString::fromStdString(fileName)}, {QStringLiteral("size"), static_cast<qint64>(fileSize)}
  };
  std::string metaStr = QJsonDocument(meta).toJson(QJsonDocument::Compact).toStdString();
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId,
                       static_cast<uint8_t>(FileChunkType::Start), &metaStr);

  std::vector<char> buffer(chunkSize);
  uint64_t totalSent = 0;

  while (file.good() && totalSent < fileSize) {
    file.read(buffer.data(), chunkSize);
    size_t bytesRead = static_cast<size_t>(file.gcount());
    if (bytesRead > 0) {
      std::string chunkData(buffer.data(), bytesRead);
      ProtocolUtil::writef(stream, kMsgDFileChunk, requestId,
                           static_cast<uint8_t>(FileChunkType::Data), &chunkData);
      totalSent += bytesRead;
    }
  }

  file.close();

  std::string emptyStr;
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId,
                       static_cast<uint8_t>(FileChunkType::End), &emptyStr);

  LOG_INFO("file transfer complete: %s (%llu bytes)", filePath.c_str(), totalSent);
  return true;
}

std::filesystem::path FileTransfer::pathFromUtf8(const std::string &path)
{
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

std::string FileTransfer::pathToUtf8(const std::filesystem::path &path)
{
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

std::string FileTransfer::sanitizeRelativePath(const std::string &path)
{
  std::string result;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find_first_of("/\\", start);
    if (end == std::string::npos)
      end = path.size();
    const auto part = path.substr(start, end - start);
    if (part == ".." || part.find(':') != std::string::npos)
      return {};
    if (!part.empty() && part != ".") {
      if (!result.empty())
        result += '/';
      result += part;
    }
    start = end + 1;
  }
  return result;
}

void FileTransfer::reserveFor(FileTransferRequest &request)
{
  constexpr uint64_t kMaxReserve = 64 * 1024 * 1024;
  if (!request.isDir)
    request.data.reserve(static_cast<size_t>(std::min(request.fileSize, kMaxReserve)));
}

bool FileTransfer::appendChunk(FileTransferRequest &request, const std::string &chunk)
{
  if (request.fileSize > 0 && request.bytesTransferred + chunk.size() > request.fileSize)
    return false;
  request.data.insert(request.data.end(), chunk.begin(), chunk.end());
  request.bytesTransferred += chunk.size();
  return true;
}

std::string FileTransfer::getTempDirectory()
{
  // temp_directory_path() is Unicode-aware on Windows, unlike GetTempPathA
  std::error_code ec;
  auto base = std::filesystem::temp_directory_path(ec);
  if (ec)
    base = pathFromUtf8(PATH_SEPARATOR[0] == '\\' ? "C:\\Temp" : "/tmp");
  return pathToUtf8(base / "autodeskflow-files") + PATH_SEPARATOR;
}

std::string FileTransfer::createTempFilePath(const std::string &fileName)
{
  const std::string tempDir = getTempDirectory();
  createDirectoryPath(tempDir);

  const auto slash = fileName.find_last_of("/\\");
  std::string safeName = sanitizeRelativePath(slash == std::string::npos ? fileName : fileName.substr(slash + 1));
  if (safeName.empty())
    safeName = "file";

  const auto finalPath = pathFromUtf8(tempDir) / pathFromUtf8(safeName);
  std::error_code ec;
  std::filesystem::remove(finalPath, ec);
  return pathToUtf8(finalPath);
}

std::string FileTransfer::createTempFilePathWithRelative(const std::string &relativePath,
                                                          uint32_t transferId)
{
  const auto sessionDir =
      pathFromUtf8(getTempDirectory()) / pathFromUtf8("transfer_" + std::to_string(transferId));

  std::string safePath = sanitizeRelativePath(relativePath);
  if (safePath.empty()) {
    const auto slash = relativePath.find_last_of("/\\");
    safePath = sanitizeRelativePath(slash == std::string::npos ? relativePath : relativePath.substr(slash + 1));
  }
  if (safePath.empty())
    safePath = "file";

  const auto fullPath = sessionDir / pathFromUtf8(safePath);
  std::error_code ec;
  std::filesystem::create_directories(fullPath.parent_path(), ec);
  return pathToUtf8(fullPath);
}

bool FileTransfer::createDirectoryPath(const std::string &path)
{
  if (path.empty())
    return false;
  const auto dir = pathFromUtf8(path);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return std::filesystem::is_directory(dir, ec);
}

bool FileTransfer::parseStartChunkEx(
    const std::string &data, std::string &fileName,
    std::string &relativePath, uint64_t &fileSize, bool &isDir)
{
  fileName.clear();
  relativePath.clear();
  fileSize = 0;
  isDir = false;

  size_t nameStart = data.find("\"name\":\"");
  if (nameStart == std::string::npos)
    return false;
  nameStart += 8;

  size_t nameEnd = nameStart;
  while (nameEnd < data.size()) {
    if (data[nameEnd] == '"' && (nameEnd == 0 || data[nameEnd - 1] != '\\'))
      break;
    nameEnd++;
  }

  for (size_t i = nameStart; i < nameEnd; ++i) {
    if (data[i] == '\\' && i + 1 < nameEnd) {
      i++;
      switch (data[i]) {
      case 'n': fileName += '\n'; break;
      case 'r': fileName += '\r'; break;
      case 't': fileName += '\t'; break;
      default:  fileName += data[i];
      }
    } else {
      fileName += data[i];
    }
  }

  size_t relPathStart = data.find("\"relativePath\":\"");
  if (relPathStart != std::string::npos) {
    relPathStart += 16;
    size_t relPathEnd = relPathStart;
    while (relPathEnd < data.size()) {
      if (data[relPathEnd] == '"' && (relPathEnd == 0 || data[relPathEnd - 1] != '\\'))
        break;
      relPathEnd++;
    }
    for (size_t i = relPathStart; i < relPathEnd; ++i) {
      if (data[i] == '\\' && i + 1 < relPathEnd) {
        i++;
        switch (data[i]) {
        case 'n': relativePath += '\n'; break;
        case 'r': relativePath += '\r'; break;
        case 't': relativePath += '\t'; break;
        default:  relativePath += data[i];
        }
      } else {
        relativePath += data[i];
      }
    }
  }

  size_t sizeStart = data.find("\"size\":");
  if (sizeStart != std::string::npos) {
    sizeStart += 7;
    while (sizeStart < data.size() && data[sizeStart] == ' ')
      sizeStart++;
    while (sizeStart < data.size() && data[sizeStart] >= '0' && data[sizeStart] <= '9') {
      fileSize = fileSize * 10 + (data[sizeStart] - '0');
      sizeStart++;
    }
  }

  size_t isDirPos = data.find("\"isDir\":");
  if (isDirPos != std::string::npos) {
    isDirPos += 8;
    while (isDirPos < data.size() && data[isDirPos] == ' ')
      isDirPos++;
    if (isDirPos < data.size() && data[isDirPos] == 't')
      isDir = true;
  }

  return !fileName.empty();
}

std::string FileTransfer::createStartChunkEx(
    uint32_t requestId, const std::string &fileName,
    const std::string &relativePath, uint64_t fileSize, bool isDir)
{
  auto escapeJson = [](const std::string &s) -> std::string {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n";  break;
      case '\r': result += "\\r";  break;
      case '\t': result += "\\t";  break;
      default:   result += c;
      }
    }
    return result;
  };

  std::ostringstream json;
  json << "{";
  json << "\"name\":\"" << escapeJson(fileName) << "\",";
  json << "\"relativePath\":\"" << escapeJson(relativePath) << "\",";
  json << "\"size\":" << fileSize << ",";
  json << "\"isDir\":" << (isDir ? "true" : "false");
  json << "}";

  return createFileChunk(requestId, FileChunkType::Start, json.str());
}

void FileTransfer::setFileRequestCallback(const FileRequestCallback &callback)
{
  s_fileRequestCallback = callback;
}

const FileRequestCallback &FileTransfer::getFileRequestCallback()
{
  return s_fileRequestCallback;
}

bool FileTransfer::hasFileRequestCallback()
{
  return static_cast<bool>(s_fileRequestCallback);
}

uint32_t FileTransfer::requestFileForPaste(
    const std::string &filePath, const std::string &relativePath,
    bool isDir, uint32_t batchId, const std::string &destFolder)
{
  if (!s_fileRequestCallback) {
    LOG_ERR("no file request callback registered");
    return 0;
  }

  LOG_INFO("requesting file for paste: path=%s", filePath.c_str());
  return s_fileRequestCallback(filePath, relativePath, isDir, batchId, destFolder);
}

void FileTransfer::setSourceInfo(const std::string &address, uint16_t port, uint64_t sessionId)
{
  s_sourceAddress = address;
  s_sourcePort = port;
  s_sourceSessionId = sessionId;
}

const std::string &FileTransfer::getSourceAddress()
{
  return s_sourceAddress;
}

uint16_t FileTransfer::getSourcePort()
{
  return s_sourcePort;
}

uint64_t FileTransfer::getSourceSessionId()
{
  return s_sourceSessionId;
}

bool FileTransfer::hasSourceInfo()
{
  return !s_sourceAddress.empty() && s_sourcePort > 0;
}

void FileTransfer::clearSourceInfo()
{
  s_sourceAddress.clear();
  s_sourcePort = 0;
  s_sourceSessionId = 0;
}
