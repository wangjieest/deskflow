/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoAutoAutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/FileTransfer.h"

#include <functional>
#include <string>

class IEventQueue;
class IDataSocket;
class SocketMultiplexer;
class NetworkAddress;

namespace deskflow {
class IStream;
}

/**
 * @brief Client-side dedicated file transfer connection
 *
 * Connects to server's file transfer port (24802) to receive
 * file data on a separate channel from the main control connection.
 */
class FileTransferConnection
{
public:
  using DataCallback = std::function<void(FileChunkType, const std::string &)>;

  FileTransferConnection(IEventQueue *events, SocketMultiplexer *socketMultiplexer);
  ~FileTransferConnection();

  /**
   * @brief Connect to file transfer port and send handshake
   * @param serverAddress Server address (port will be changed to 24802)
   * @param requestId Request ID for handshake
   * @return true if connected successfully
   */
  bool connect(const NetworkAddress &serverAddress, uint32_t requestId);

  /**
   * @brief Connect to specified address and port for point-to-point transfer
   * @param address IP address or hostname
   * @param port TCP port
   * @param requestId Request ID for handshake
   * @param sessionId Session ID for validation (from ClipboardMeta)
   * @param filePath Path of file to request
   * @return true if connected successfully
   */
  bool connectPointToPoint(
      const std::string &address, uint16_t port, uint32_t requestId, uint64_t sessionId, const std::string &filePath
  );

  /**
   * @brief Set callback for received file data
   * @param callback Function to call when data chunk is received
   */
  void setDataCallback(DataCallback callback);

  /**
   * @brief Start receiving file data
   *
   * This will be called asynchronously as data arrives on the socket
   */
  void receiveFileData();

  /**
   * @brief Close the connection
   */
  void close();

  /**
   * @brief Check if connected
   */
  bool isConnected() const
  {
    return m_socket != nullptr;
  }

private:
  void handleDataReady();
  void handleDisconnected();
  bool sendFileRequest(uint64_t sessionId, uint32_t requestId, const std::string &filePath);

  IDataSocket *m_socket = nullptr;
  deskflow::IStream *m_stream = nullptr;
  IEventQueue *m_events;
  SocketMultiplexer *m_socketMultiplexer;
  uint32_t m_requestId = 0;

  DataCallback m_dataCallback;
};
