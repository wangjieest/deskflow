/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/FileTransfer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class IDataSocket;
class IEventQueue;
class IListenSocket;
class ISocketFactory;
class SocketMultiplexer;

namespace deskflow {
class IStream;
}

/**
 * @brief File info for pending transfer
 */
struct FileTransferFileInfo
{
  std::string path;         //!< Full path on this machine
  std::string relativePath; //!< Relative path for directory structure
  uint64_t size = 0;        //!< File size in bytes
  bool isDir = false;       //!< True if this is a directory
};

/**
 * @brief Point-to-point clipboard transfer server
 *
 * Runs in the ClipboardTransfer thread. Listens on a dynamic port
 * and serves files/data to other machines.
 *
 * Features:
 * - Dynamic port allocation (OS assigns available port)
 * - Session-based file access control
 * - Multiple concurrent connections
 * - 30-second idle timeout per connection
 */
class ClipboardTransferServer
{
public:
  ClipboardTransferServer(IEventQueue *events, SocketMultiplexer *socketMultiplexer, ISocketFactory *socketFactory);
  ~ClipboardTransferServer();

  ClipboardTransferServer(const ClipboardTransferServer &) = delete;
  ClipboardTransferServer &operator=(const ClipboardTransferServer &) = delete;

  /**
   * @brief Start listening on a dynamic port
   * @return true if listening started successfully
   */
  bool start();

  /**
   * @brief Stop listening and close all connections
   */
  void stop();

  /**
   * @brief Check if server is running
   */
  bool isRunning() const
  {
    return m_listen != nullptr;
  }

  /**
   * @brief Get the port this server is listening on
   * @return port number, or 0 if not running
   */
  uint16_t getPort() const
  {
    return m_port;
  }

  /**
   * @brief Get the local IP address for point-to-point transfer
   * @return IP address string, or empty if not available
   */
  std::string getLocalAddress() const;

  /**
   * @brief Set files available for transfer in a session
   *
   * @param sessionId Unique session identifier (from ClipboardMeta)
   * @param files List of files available for transfer
   */
  void setSessionFiles(uint64_t sessionId, const std::vector<FileTransferFileInfo> &files);

  /**
   * @brief Clear files for a session
   * @param sessionId Session to clear
   */
  void clearSession(uint64_t sessionId);

  /**
   * @brief Clear all sessions
   */
  void clearAllSessions();

private:
  struct Session
  {
    uint64_t sessionId = 0;
    std::vector<FileTransferFileInfo> files;
    std::map<std::string, size_t> pathToIndex;
  };

  struct ClientConnection
  {
    IDataSocket *socket = nullptr;
    deskflow::IStream *stream = nullptr;
    int64_t lastActivity = 0;
    uint64_t sessionId = 0;
  };

  void handleClientConnecting();
  void handleClientData(IDataSocket *socket);
  void handleClientDisconnected(IDataSocket *socket);

  void processFileRequest(IDataSocket *socket, uint32_t requestId, uint64_t sessionId, const std::string &filePath);
  bool sendFile(deskflow::IStream *stream, uint32_t requestId, const std::string &filePath, const std::string &relativePath);
  void sendDirectory(deskflow::IStream *stream, uint32_t requestId, const std::string &relativePath);
  void sendError(deskflow::IStream *stream, uint32_t requestId, const std::string &message);

  bool isFileAllowed(uint64_t sessionId, const std::string &filePath) const;
  const FileTransferFileInfo *findFileInfo(uint64_t sessionId, const std::string &filePath) const;

  void checkIdleConnections();

  IListenSocket *m_listen = nullptr;
  IEventQueue *m_events = nullptr;
  SocketMultiplexer *m_socketMultiplexer = nullptr;
  ISocketFactory *m_socketFactory = nullptr;

  uint16_t m_port = 0;

  std::map<uint64_t, Session> m_sessions;
  std::map<IDataSocket *, ClientConnection> m_connections;

  mutable std::string m_localAddress;
};
