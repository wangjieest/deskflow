/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace deskflow {
class IStream;
}

//! File transfer chunk types
enum class FileChunkType : uint8_t
{
  Start = 0, //!< Transfer starting, data contains file metadata JSON
  Data = 1,  //!< File data chunk
  End = 2,   //!< Transfer complete
  Error = 3  //!< Transfer failed
};

//! File transfer request info
struct FileTransferRequest
{
  uint32_t requestId;
  uint32_t batchTransferId;
  std::string filePath;
  std::string fileName;
  std::string relativePath;
  uint64_t fileSize;
  uint64_t bytesTransferred;
  bool isComplete;
  bool hasError;
  bool isDir;
  std::string errorMessage;
  std::vector<uint8_t> data;
};

using FileTransferCompleteCallback =
    std::function<void(uint32_t requestId, bool success, const std::string &localPath)>;

using FileDataProviderCallback =
    std::function<bool(const std::string &path, std::vector<uint8_t> &data)>;

using FileRequestCallback = std::function<uint32_t(
    const std::string &filePath, const std::string &relativePath,
    bool isDir, uint32_t batchId, const std::string &destFolder)>;

//! File transfer manager
class FileTransfer
{
public:
  FileTransfer() = default;
  ~FileTransfer() = default;

  static uint32_t generateRequestId();
  static std::string createFileRequest(uint32_t requestId, const std::string &filePath);
  static std::string createFileChunk(uint32_t requestId, FileChunkType chunkType,
                                      const std::string &data);
  static std::string createStartChunk(uint32_t requestId, const std::string &fileName,
                                       uint64_t fileSize);
  static std::string createErrorChunk(uint32_t requestId, const std::string &errorMessage);
  static bool parseStartChunk(const std::string &data, std::string &fileName, uint64_t &fileSize);

  static bool sendFile(deskflow::IStream *stream, uint32_t requestId,
                       const std::string &filePath, size_t chunkSize = 32768);

  static std::string getTempDirectory();
  static std::string createTempFilePath(const std::string &fileName);
  static std::string createTempFilePathWithRelative(const std::string &relativePath,
                                                     uint32_t transferId);
  static bool createDirectoryPath(const std::string &path);

  static bool parseStartChunkEx(const std::string &data, std::string &fileName,
                                 std::string &relativePath, uint64_t &fileSize, bool &isDir);
  static std::string createStartChunkEx(uint32_t requestId, const std::string &fileName,
                                         const std::string &relativePath, uint64_t fileSize,
                                         bool isDir);

  static void setFileRequestCallback(const FileRequestCallback &callback);
  static const FileRequestCallback &getFileRequestCallback();
  static bool hasFileRequestCallback();
  static uint32_t requestFileForPaste(const std::string &filePath,
                                       const std::string &relativePath, bool isDir,
                                       uint32_t batchId, const std::string &destFolder);

  static void setSourceInfo(const std::string &address, uint16_t port, uint64_t sessionId);
  static const std::string &getSourceAddress();
  static uint16_t getSourcePort();
  static uint64_t getSourceSessionId();
  static bool hasSourceInfo();
  static void clearSourceInfo();

private:
  static uint32_t s_nextRequestId;
  static FileRequestCallback s_fileRequestCallback;
  static std::string s_sourceAddress;
  static uint16_t s_sourcePort;
  static uint64_t s_sourceSessionId;
};
