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

  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
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

  std::ostringstream metaJson;
  metaJson << "{\"name\":\"" << fileName << "\",\"size\":" << fileSize << "}";
  std::string metaStr = metaJson.str();
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

std::string FileTransfer::getTempDirectory()
{
#ifdef _WIN32
  char tempPath[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, tempPath);
  if (len > 0 && len < MAX_PATH) {
    return std::string(tempPath) + "autodeskflow-files" + PATH_SEPARATOR;
  }
  return "C:\\Temp\\autodeskflow-files\\";
#else
  const char *tmpDir = std::getenv("TMPDIR");
  if (tmpDir == nullptr)
    tmpDir = "/tmp";
  return std::string(tmpDir) + "/autodeskflow-files/";
#endif
}

std::string FileTransfer::createTempFilePath(const std::string &fileName)
{
  std::string tempDir = getTempDirectory();
#ifdef _WIN32
  _mkdir(tempDir.c_str());
#else
  mkdir(tempDir.c_str(), 0755);
#endif

  std::ostringstream path;
  path << tempDir << fileName;

  std::string finalPath = path.str();
  FILE *test = fopen(finalPath.c_str(), "r");
  if (test) {
    fclose(test);
    std::remove(finalPath.c_str());
  }

  return finalPath;
}

std::string FileTransfer::createTempFilePathWithRelative(const std::string &relativePath,
                                                          uint32_t transferId)
{
  std::string tempDir = getTempDirectory();

  std::ostringstream sessionDir;
  sessionDir << tempDir << "transfer_" << transferId << PATH_SEPARATOR;
  createDirectoryPath(sessionDir.str());

  std::string normalizedPath = relativePath;
  for (char &c : normalizedPath) {
    if (c == '/' || c == '\\')
      c = PATH_SEPARATOR[0];
  }

  std::string fullPath = sessionDir.str() + normalizedPath;

  size_t lastSep = fullPath.find_last_of(PATH_SEPARATOR);
  if (lastSep != std::string::npos) {
    std::string parentDir = fullPath.substr(0, lastSep);
    createDirectoryPath(parentDir);
  }

  return fullPath;
}

bool FileTransfer::createDirectoryPath(const std::string &path)
{
  if (path.empty())
    return false;

  std::string normalizedPath = path;
  for (char &c : normalizedPath) {
    if (c == '/' || c == '\\')
      c = PATH_SEPARATOR[0];
  }

  while (!normalizedPath.empty() && normalizedPath.back() == PATH_SEPARATOR[0])
    normalizedPath.pop_back();

  if (normalizedPath.empty())
    return false;

#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(normalizedPath.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
    return true;
  if (CreateDirectoryA(normalizedPath.c_str(), nullptr))
    return true;
  if (GetLastError() == ERROR_PATH_NOT_FOUND) {
    size_t lastSep = normalizedPath.find_last_of(PATH_SEPARATOR);
    if (lastSep != std::string::npos && lastSep > 0) {
      std::string parentPath = normalizedPath.substr(0, lastSep);
      if (parentPath.length() > 2 || parentPath[1] != ':') {
        if (!createDirectoryPath(parentPath))
          return false;
        return CreateDirectoryA(normalizedPath.c_str(), nullptr) != 0;
      }
    }
  }
  return false;
#else
  struct stat st;
  if (stat(normalizedPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    return true;
  if (mkdir(normalizedPath.c_str(), 0755) == 0)
    return true;
  if (errno == ENOENT) {
    size_t lastSep = normalizedPath.find_last_of(PATH_SEPARATOR);
    if (lastSep != std::string::npos && lastSep > 0) {
      std::string parentPath = normalizedPath.substr(0, lastSep);
      if (!createDirectoryPath(parentPath))
        return false;
      return mkdir(normalizedPath.c_str(), 0755) == 0;
    }
  }
  return false;
#endif
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
