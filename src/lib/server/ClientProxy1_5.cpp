/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_5.h"

#include "base/Log.h"
#include "deskflow/ClipboardMeta.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "io/IStream.h"
#include "server/Server.h"

#ifdef _WIN32
#include "platform/MSWindowsClipboardFileConverter.h"
#elif defined(__APPLE__)
#include "platform/OSXClipboardFileConverter.h"
#endif

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

//
// ClientProxy1_5
//

ClientProxy1_5::ClientProxy1_5(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_4(name, stream, server, events)
{
  // do nothing
}

void ClientProxy1_5::sendDragInfo(uint32_t fileCount, const char *info, size_t size)
{
  // do nothing
}

void ClientProxy1_5::fileChunkSending(uint8_t mark, char *data, size_t dataSize)
{
  // do nothing
}

bool ClientProxy1_5::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgDFileTransfer, 4) == 0) {
    fileChunkReceived();
  } else if (memcmp(code, kMsgDDragInfo, 4) == 0) {
    dragInfoReceived();
  } else if (memcmp(code, kMsgQFileRequest, 4) == 0) {
    handleFileRequest();
  } else if (memcmp(code, kMsgDFileChunk, 4) == 0) {
    // File chunk from client (for Client → Server transfer)
    handleFileChunkFromClient();
  } else {
    return ClientProxy1_4::parseMessage(code);
  }

  return true;
}

void ClientProxy1_5::fileChunkReceived() const
{
  // do nothing
}

void ClientProxy1_5::dragInfoReceived() const
{
  // do nothing
}

void ClientProxy1_5::handleFileRequest()
{
  // Parse file request message: kMsgQFileRequest = "QFIL%4i%s"
  // %4i = request ID (4 bytes)
  // %s = request data (JSON string with path, relativePath, isDir, sessionId)

  uint32_t requestId = 0;
  std::string requestData;

  if (!ProtocolUtil::readf(getStream(), kMsgQFileRequest + 4, &requestId, &requestData)) {
    LOG_ERR("failed to parse file request message");
    return;
  }

  // Parse JSON request data
  std::string filePath;
  std::string relativePath;
  bool isDir = false;
  uint64_t sessionId = 0;

  // Helper lambda to unescape JSON strings
  auto unescapeJson = [](const std::string &s) -> std::string {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        ++i;
        switch (s[i]) {
        case 'n':
          result += '\n';
          break;
        case 'r':
          result += '\r';
          break;
        case 't':
          result += '\t';
          break;
        default:
          result += s[i];
        }
      } else {
        result += s[i];
      }
    }
    return result;
  };

  // Helper lambda to extract string field from JSON
  auto extractStringField = [&unescapeJson](const std::string &json, const std::string &fieldName)
      -> std::pair<std::string, bool> {
    std::string searchStr = "\"" + fieldName + "\":\"";
    size_t pos = json.find(searchStr);
    if (pos == std::string::npos) {
      return {"", false};
    }
    pos += searchStr.size();
    size_t endPos = pos;
    while (endPos < json.size() && json[endPos] != '"') {
      if (json[endPos] == '\\' && endPos + 1 < json.size()) {
        endPos += 2;
      } else {
        endPos++;
      }
    }
    return {unescapeJson(json.substr(pos, endPos - pos)), true};
  };

  // Helper lambda to extract uint64 field from JSON
  auto extractUint64Field = [](const std::string &json, const std::string &fieldName) -> uint64_t {
    std::string searchStr = "\"" + fieldName + "\":";
    size_t pos = json.find(searchStr);
    if (pos == std::string::npos) {
      return 0;
    }
    pos += searchStr.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
      pos++;
    }
    uint64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
      value = value * 10 + (json[pos] - '0');
      pos++;
    }
    return value;
  };

  // Check if requestData is JSON (starts with '{')
  if (!requestData.empty() && requestData[0] == '{') {
    // Parse JSON format
    auto [path, hasPath] = extractStringField(requestData, "path");
    auto [relPath, hasRelPath] = extractStringField(requestData, "relativePath");

    if (hasPath) {
      filePath = path;
    }
    if (hasRelPath) {
      relativePath = relPath;
    }

    // Parse sessionId field
    sessionId = extractUint64Field(requestData, "sessionId");

    // Parse isDir field
    size_t isDirPos = requestData.find("\"isDir\":");
    if (isDirPos != std::string::npos) {
      isDirPos += 8;
      while (isDirPos < requestData.size() && requestData[isDirPos] == ' ') {
        isDirPos++;
      }
      if (isDirPos < requestData.size() && requestData[isDirPos] == 't') {
        isDir = true;
      }
    }
  } else {
    // Legacy format: requestData is just the file path
    filePath = requestData;
    // Extract file name for relativePath
    size_t lastSlash = filePath.find_last_of("/\\");
    relativePath = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;
  }

  LOG_INFO(
      "received file request: id=%u, sessionId=%llu, path=%s, relativePath=%s, isDir=%d", requestId, sessionId,
      filePath.c_str(), relativePath.c_str(), isDir
  );

  // Validate the request against current clipboard session
  // Use clipboard 0 (primary clipboard) for file transfers
  ClipboardDataStatus status = m_server->validateFileRequest(0, sessionId, filePath);

  if (status != ClipboardDataStatus::Success) {
    // Send error response
    LOG_WARN(
        "file request rejected: status=%s, path=%s", clipboardDataStatusToString(status), filePath.c_str()
    );
    std::string errorMsg = clipboardDataStatusToString(status);
    ProtocolUtil::writef(
        getStream(), kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Error), &errorMsg
    );
    return;
  }

  // Handle directory entries
  if (isDir) {
    // For directories, just send start and end chunks with metadata
    // The directory will be created on the client side
    sendDirectoryResponse(requestId, relativePath);
  } else {
    // Send the file back to the client with extended metadata
    if (!sendFileWithMetadata(getStream(), requestId, filePath, relativePath)) {
      LOG_ERR("failed to send file: %s", filePath.c_str());
    }
  }
}

void ClientProxy1_5::sendDirectoryResponse(uint32_t requestId, const std::string &relativePath)
{
  // Extract directory name from relativePath
  std::string dirName = relativePath;
  size_t lastSlash = relativePath.find_last_of("/\\");
  if (lastSlash != std::string::npos) {
    dirName = relativePath.substr(lastSlash + 1);
  }

  // Send start chunk with directory metadata
  std::string startChunk = FileTransfer::createStartChunkEx(requestId, dirName, relativePath, 0, true);

  // The createStartChunkEx already includes request ID and chunk type
  // We need to write it as a kMsgDFileChunk
  std::string dirMetadata = "{\"name\":\"" + dirName + "\",\"relativePath\":\"" + relativePath + "\",\"size\":0,\"isDir\":true}";
  ProtocolUtil::writef(
      getStream(), kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Start),
      &dirMetadata
  );

  // Send end chunk
  std::string emptyStr;
  ProtocolUtil::writef(getStream(), kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::End), &emptyStr);

  LOG_INFO("sent directory response: id=%u, path=%s", requestId, relativePath.c_str());
}

bool ClientProxy1_5::sendFileWithMetadata(
    deskflow::IStream *stream, uint32_t requestId, const std::string &filePath, const std::string &relativePath
)
{
  LOG_INFO("sending file with metadata: %s (requestId=%u, relativePath=%s)", filePath.c_str(), requestId, relativePath.c_str());

  // Open file
  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_ERR("failed to open file for transfer: %s", filePath.c_str());
    std::string errorMsg = "File not found";
    ProtocolUtil::writef(
        stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Error), &errorMsg
    );
    return false;
  }

  // Get file size
  uint64_t fileSize = static_cast<uint64_t>(file.tellg());
  file.seekg(0, std::ios::beg);

  // Extract file name from path
  std::string fileName = filePath;
  size_t lastSlash = filePath.find_last_of("/\\");
  if (lastSlash != std::string::npos) {
    fileName = filePath.substr(lastSlash + 1);
  }

  // Helper lambda to escape JSON strings
  auto escapeJson = [](const std::string &s) -> std::string {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
      }
    }
    return result;
  };

  // Build extended metadata JSON
  std::ostringstream metaJson;
  metaJson << "{";
  metaJson << "\"name\":\"" << escapeJson(fileName) << "\",";
  metaJson << "\"relativePath\":\"" << escapeJson(relativePath) << "\",";
  metaJson << "\"size\":" << fileSize << ",";
  metaJson << "\"isDir\":false";
  metaJson << "}";

  // Send start chunk with extended metadata
  std::string metaStr = metaJson.str();
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Start), &metaStr);

  LOG_DEBUG("sent file start chunk: name=%s, relativePath=%s, size=%llu", fileName.c_str(), relativePath.c_str(), fileSize);

  // Send data chunks
  const size_t chunkSize = 32768;
  std::vector<char> buffer(chunkSize);
  uint64_t totalSent = 0;

  while (file.good() && totalSent < fileSize) {
    file.read(buffer.data(), chunkSize);
    size_t bytesRead = static_cast<size_t>(file.gcount());

    if (bytesRead > 0) {
      std::string chunkData(buffer.data(), bytesRead);
      ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Data), &chunkData);
      totalSent += bytesRead;

      LOG_DEBUG1("sent file data chunk: %zu bytes (total: %llu/%llu)", bytesRead, totalSent, fileSize);
    }
  }

  file.close();

  // Send end chunk
  std::string emptyStr;
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::End), &emptyStr);

  LOG_INFO("file transfer complete: %s (%llu bytes)", filePath.c_str(), totalSent);
  return true;
}

uint32_t ClientProxy1_5::requestFileFromClient(
    const std::string &filePath, const std::string &relativePath, bool isDir, uint64_t sessionId
)
{
  uint32_t requestId = FileTransfer::generateRequestId();

  // Build JSON request data
  auto escapeJson = [](const std::string &s) -> std::string {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
      }
    }
    return result;
  };

  std::ostringstream json;
  json << "{";
  json << "\"path\":\"" << escapeJson(filePath) << "\",";
  json << "\"relativePath\":\"" << escapeJson(relativePath) << "\",";
  json << "\"isDir\":" << (isDir ? "true" : "false") << ",";
  json << "\"sessionId\":" << sessionId;
  json << "}";

  std::string requestData = json.str();

  LOG_INFO(
      "requesting file from client \"%s\": id=%u, path=%s, relativePath=%s, isDir=%d", getName().c_str(), requestId,
      filePath.c_str(), relativePath.c_str(), isDir
  );

  // Send request to client using kMsgSFileRequest
  ProtocolUtil::writef(getStream(), kMsgSFileRequest, requestId, &requestData);

  // Track this pending transfer
  FileReceiveState state;
  state.requestId = requestId;
  state.relativePath = relativePath;
  state.isDir = isDir;
  m_fileReceives[requestId] = state;
  m_pendingTransferCount++;

  return requestId;
}

void ClientProxy1_5::handleFileChunkFromClient()
{
  // Parse file chunk message: kMsgDFileChunk = "DFCH%4i%1i%s"
  // %4i = request ID (4 bytes)
  // %1i = chunk type (1 byte)
  // %s = data (string)

  uint32_t requestId = 0;
  uint8_t chunkType = 0;
  std::string data;

  if (!ProtocolUtil::readf(getStream(), kMsgDFileChunk + 4, &requestId, &chunkType, &data)) {
    LOG_ERR("failed to parse file chunk message from client");
    return;
  }

  LOG_DEBUG(
      "[FileTransfer] received chunk from client: requestId=%u, type=%u, size=%zu", requestId, chunkType, data.size()
  );

  auto it = m_fileReceives.find(requestId);
  if (it == m_fileReceives.end()) {
    LOG_WARN("[FileTransfer] received chunk for unknown request: %u", requestId);
    return;
  }

  FileReceiveState &state = it->second;

  switch (static_cast<FileChunkType>(chunkType)) {
  case FileChunkType::Start: {
    // Parse metadata from JSON
    std::string fileName;
    std::string relativePath;
    uint64_t fileSize = 0;
    bool isDir = false;

    if (!FileTransfer::parseStartChunkEx(data, fileName, relativePath, fileSize, isDir)) {
      LOG_ERR("[FileTransfer] failed to parse start chunk metadata");
      m_fileReceives.erase(it);
      m_pendingTransferCount--;
      return;
    }

    state.fileName = fileName;
    if (!relativePath.empty()) {
      state.relativePath = relativePath;
    }
    state.fileSize = fileSize;
    state.isDir = isDir;
    state.bytesReceived = 0;
    state.data.clear();

    // Create destination path
    if (isDir) {
      state.destPath = FileTransfer::createTempFilePathWithRelative(state.relativePath, requestId);
      FileTransfer::createDirectoryPath(state.destPath);
      LOG_INFO("[FileTransfer] created directory: %s", state.destPath.c_str());
    } else {
      state.destPath = FileTransfer::createTempFilePathWithRelative(state.relativePath, requestId);
      // Ensure parent directory exists
      size_t lastSlash = state.destPath.find_last_of("/\\");
      if (lastSlash != std::string::npos) {
        FileTransfer::createDirectoryPath(state.destPath.substr(0, lastSlash));
      }
      state.data.reserve(static_cast<size_t>(fileSize));
    }

    LOG_INFO(
        "[FileTransfer] starting receive: id=%u, name=%s, size=%llu, isDir=%d, dest=%s", requestId, fileName.c_str(),
        fileSize, isDir, state.destPath.c_str()
    );
    break;
  }

  case FileChunkType::Data: {
    // Append data
    state.data.insert(state.data.end(), data.begin(), data.end());
    state.bytesReceived += data.size();

    LOG_DEBUG1(
        "[FileTransfer] received data chunk: %zu bytes (total: %llu/%llu)", data.size(), state.bytesReceived,
        state.fileSize
    );
    break;
  }

  case FileChunkType::End: {
    if (!state.isDir && !state.destPath.empty()) {
      // Write file to disk
      std::ofstream file(state.destPath, std::ios::binary);
      if (file.is_open()) {
        file.write(reinterpret_cast<const char *>(state.data.data()), state.data.size());
        file.close();
        LOG_INFO("[FileTransfer] file saved: %s (%llu bytes)", state.destPath.c_str(), state.bytesReceived);
        m_completedFilePaths.push_back(state.destPath);
      } else {
        LOG_ERR("[FileTransfer] failed to write file: %s", state.destPath.c_str());
      }
    } else if (state.isDir) {
      // Directory already created
      m_completedFilePaths.push_back(state.destPath);
    }

    m_fileReceives.erase(it);
    m_pendingTransferCount--;

    // Check if all transfers are complete
    if (m_pendingTransferCount == 0) {
      LOG_INFO("[FileTransfer] all file transfers complete");

      // Signal platform-specific transfer complete
#ifdef _WIN32
      LOG_INFO("[FileTransfer] Windows: storing %zu completed file path(s)", m_completedFilePaths.size());
      MSWindowsClipboardFileConverter::setCompletedFilePaths(m_completedFilePaths);
      MSWindowsClipboardFileConverter::signalTransferComplete();
#elif defined(__APPLE__)
      LOG_INFO("[FileTransfer] macOS: storing %zu completed file path(s)", m_completedFilePaths.size());
      OSXClipboardFileConverter::setCompletedFilePaths(m_completedFilePaths);
      OSXClipboardFileConverter::signalTransferComplete();
#endif

      if (m_transferCompleteCallback) {
        m_transferCompleteCallback();
      }
    }
    break;
  }

  case FileChunkType::Error: {
    LOG_ERR("[FileTransfer] transfer error for request %u: %s", requestId, data.c_str());
    m_fileReceives.erase(it);
    m_pendingTransferCount--;

    // Still signal complete on error so waiting code doesn't hang
    if (m_pendingTransferCount == 0) {
#ifdef _WIN32
      MSWindowsClipboardFileConverter::signalTransferComplete();
#elif defined(__APPLE__)
      OSXClipboardFileConverter::signalTransferComplete();
#endif

      if (m_transferCompleteCallback) {
        m_transferCompleteCallback();
      }
    }
    break;
  }
  }
}

bool ClientProxy1_5::hasPendingTransfers() const
{
  return m_pendingTransferCount > 0;
}

const std::vector<std::string> &ClientProxy1_5::getCompletedFilePaths() const
{
  return m_completedFilePaths;
}

void ClientProxy1_5::clearCompletedFilePaths()
{
  m_completedFilePaths.clear();
}

void ClientProxy1_5::setTransferCompleteCallback(const TransferCompleteCallback &callback)
{
  m_transferCompleteCallback = callback;
}
