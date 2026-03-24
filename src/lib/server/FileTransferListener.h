/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoAutoAutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Event.h"
#include "net/NetworkAddress.h"

#include <cstdint>
#include <map>
#include <string>

class IDataSocket;
class IEventQueue;
class IListenSocket;
class ISocketFactory;
class SocketMultiplexer;

namespace deskflow {
class IStream;
}

/**
 * @brief Manages dedicated file transfer connections on port 24802
 *
 * This listener handles file data transfer on a separate TCP connection
 * to prevent blocking the main control connection (port 24800).
 *
 * Features:
 * - Fixed port 24802 for file transfers
 * - Multiple concurrent client connections
 * - 30-second idle timeout per connection
 * - Request-based session management
 */
class FileTransferListener
{
public:
  FileTransferListener(IEventQueue *events, SocketMultiplexer *socketMultiplexer, ISocketFactory *socketFactory);
  ~FileTransferListener();

  /**
   * @brief Start listening on port 24802
   * @param address Base address (port will be changed to 24802)
   * @return true if listening started successfully
   */
  bool start(const NetworkAddress &address);

  /**
   * @brief Stop listening and close all connections
   */
  void stop();

  /**
   * @brief Add a pending file transfer request
   * @param requestId Unique request identifier
   * @param clientName Name of requesting client
   * @param filePath Path to file on server
   * @param relativePath Relative path for client
   * @param isDir Whether this is a directory
   */
  void addPendingRequest(
      uint32_t requestId, const std::string &clientName, const std::string &filePath,
      const std::string &relativePath, bool isDir
  );

  /**
   * @brief Check if listener is running
   */
  bool isRunning() const
  {
    return m_listen != nullptr;
  }

private:
  struct PendingRequest
  {
    uint32_t requestId;
    std::string clientName;
    std::string filePath;
    std::string relativePath;
    bool isDir;
  };

  struct ActiveSession
  {
    uint32_t requestId;
    IDataSocket *socket;
    deskflow::IStream *stream;
    int64_t lastActivity;
  };

  // Event handlers
  void handleClientConnecting();
  void handleClientAccepted(IDataSocket *socket);
  void handleClientDisconnected(IDataSocket *socket);

  // Send file on the given socket
  void sendFileOnConnection(IDataSocket *socket, const PendingRequest &request);

  // Idle timeout check
  void checkIdleConnections();

  IListenSocket *m_listen = nullptr;
  IEventQueue *m_events;
  SocketMultiplexer *m_socketMultiplexer;
  ISocketFactory *m_socketFactory;

  // Pending requests waiting for client to connect
  std::map<uint32_t, PendingRequest> m_pendingRequests;

  // Active transfer sessions
  std::map<IDataSocket *, ActiveSession> m_activeSessions;
};
