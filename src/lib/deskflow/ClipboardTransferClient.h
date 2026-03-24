/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/FileTransfer.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

class IEventQueue;
class IDataSocket;
class SocketMultiplexer;
class NetworkAddress;

namespace deskflow {
class IStream;
}

/**
 * @brief Callback for file transfer completion
 * @param success True if transfer completed successfully
 * @param localPath Path to downloaded file
 * @param errorMessage Error message if failed
 */
using FileTransferCallback = std::function<void(bool success, const std::string &localPath, const std::string &errorMessage)>;

/**
 * @brief Client-side clipboard transfer connection
 *
 * Runs in the ClipboardTransfer thread. Connects to remote
 * ClipboardTransferServer to download files/data.
 *
 * Features:
 * - Point-to-point connection to source machine
 * - Multiple concurrent file requests
 * - Automatic connection reuse
 */
class ClipboardTransferClient
{
public:
  ClipboardTransferClient(IEventQueue *events, SocketMultiplexer *socketMultiplexer);
  ~ClipboardTransferClient();

  ClipboardTransferClient(const ClipboardTransferClient &) = delete;
  ClipboardTransferClient &operator=(const ClipboardTransferClient &) = delete;

  /**
   * @brief Request a file from remote source
   *
   * @param sourceAddr IP address of source machine
   * @param port Port of source machine's transfer server
   * @param sessionId Session ID for validation
   * @param remotePath Path of file on source machine
   * @param callback Callback when transfer completes
   */
  void requestFile(
      const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
      FileTransferCallback callback
  );

  /**
   * @brief Close all connections
   */
  void closeAll();

  /**
   * @brief Get the temporary directory for downloaded files
   */
  std::string getTempDirectory() const;

private:
  struct PendingRequest
  {
    uint32_t requestId = 0;
    std::string remotePath;
    std::string localPath;
    std::string relativePath;
    uint64_t expectedSize = 0;
    uint64_t receivedSize = 0;
    bool isDir = false;
    FileTransferCallback callback;
    std::ofstream *file = nullptr;
  };

  struct Connection
  {
    IDataSocket *socket = nullptr;
    deskflow::IStream *stream = nullptr;
    std::string address;
    uint16_t port = 0;
    uint64_t sessionId = 0;
    std::map<uint32_t, PendingRequest> pendingRequests;
  };

  // Get or create connection to remote server
  Connection *getConnection(const std::string &addr, uint16_t port, uint64_t sessionId);

  // Send file request on connection
  bool sendFileRequest(Connection *conn, uint32_t requestId, const std::string &filePath);

  // Event handlers
  void handleDataReady(IDataSocket *socket);
  void handleDisconnected(IDataSocket *socket);

  // Process received chunk
  void processChunk(Connection *conn, uint32_t requestId, FileChunkType chunkType, const std::string &data);

  // Complete a request
  void completeRequest(Connection *conn, uint32_t requestId, bool success, const std::string &error = "");

  // Generate unique request ID
  uint32_t generateRequestId();

  // Create local path for file
  std::string createLocalPath(const std::string &relativePath);

  IEventQueue *m_events;
  SocketMultiplexer *m_socketMultiplexer;

  // Connections by "addr:port"
  std::map<std::string, std::unique_ptr<Connection>> m_connections;

  // Request ID counter
  uint32_t m_nextRequestId = 1;

  // Temp directory
  mutable std::string m_tempDir;
};
