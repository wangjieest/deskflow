/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoAutoAutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/FileTransferListener.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/StreamBuffer.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"
#include "arch/Arch.h"
#include "net/SocketException.h"

#include <ctime>

FileTransferListener::FileTransferListener(IEventQueue *events, SocketMultiplexer *socketMultiplexer, ISocketFactory *socketFactory)
    : m_events(events),
      m_socketMultiplexer(socketMultiplexer),
      m_socketFactory(socketFactory)
{
  LOG_DEBUG("FileTransferListener created");
}

FileTransferListener::~FileTransferListener()
{
  stop();
}

bool FileTransferListener::start(const NetworkAddress &address)
{
  LOG_INFO("[FileTransfer] FileTransferListener::start() called");

  if (m_listen) {
    LOG_WARN("FileTransferListener already started");
    return true;
  }

  try {
    LOG_INFO("[FileTransfer] Creating NetworkAddress for port 24802, hostname=%s", address.getHostname().c_str());
    // Create listen socket on port 24802
    NetworkAddress fileTransferAddr(address.getHostname(), 24802);

    LOG_INFO("[FileTransfer] Resolving address");
    fileTransferAddr.resolve();
    LOG_INFO("[FileTransfer] Address resolved, isValid=%d", fileTransferAddr.isValid());

    LOG_INFO("[FileTransfer] Getting address family from ARCH");
    auto addrFamily = ARCH->getAddrFamily(fileTransferAddr.getAddress());
    LOG_INFO("[FileTransfer] Address family=%d, creating listen socket", static_cast<int>(addrFamily));

    m_listen = m_socketFactory->createListen(addrFamily, SecurityLevel::Encrypted);
    LOG_INFO("[FileTransfer] Listen socket created: %p", m_listen);

    // Setup event handler for incoming connections
    m_events->addHandler(EventTypes::ListenSocketConnecting, m_listen, [this](const auto &) {
      handleClientConnecting();
    });

    // Bind and start listening
    LOG_INFO("FileTransferListener binding to port 24802");
    m_listen->bind(fileTransferAddr);

    LOG_INFO("✅ FileTransferListener started on port 24802");
    return true;
  } catch (SocketAddressInUseException &e) {
    LOG_ERR("Failed to bind file transfer port 24802 (address in use): %s", e.what());
    if (m_listen) {
      delete m_listen;
      m_listen = nullptr;
    }
    return false;
  } catch (BaseException &e) {
    LOG_ERR("Failed to start FileTransferListener: %s", e.what());
    if (m_listen) {
      delete m_listen;
      m_listen = nullptr;
    }
    return false;
  }
}

void FileTransferListener::stop()
{
  if (!m_listen) {
    return;
  }

  LOG_INFO("Stopping FileTransferListener");

  // Close all active sessions
  for (auto &pair : m_activeSessions) {
    IDataSocket *socket = pair.first;
    m_events->removeHandler(EventTypes::SocketDisconnected, socket);
    delete pair.second.stream;
    delete socket;
  }
  m_activeSessions.clear();

  // Remove listen handler
  m_events->removeHandler(EventTypes::ListenSocketConnecting, m_listen);

  // Close listen socket
  delete m_listen;
  m_listen = nullptr;

  // Clear pending requests
  m_pendingRequests.clear();

  LOG_INFO("FileTransferListener stopped");
}

void FileTransferListener::addPendingRequest(
    uint32_t requestId, const std::string &clientName, const std::string &filePath, const std::string &relativePath,
    bool isDir
)
{
  PendingRequest request;
  request.requestId = requestId;
  request.clientName = clientName;
  request.filePath = filePath;
  request.relativePath = relativePath;
  request.isDir = isDir;

  m_pendingRequests[requestId] = request;

  LOG_INFO("[FileTransfer] Added pending request: id=%u, client=%s, file=%s", requestId, clientName.c_str(),
           filePath.c_str());
}

void FileTransferListener::handleClientConnecting()
{
  if (!m_listen) {
    return;
  }

  LOG_DEBUG("[FileTransfer] Client connecting to port 24802");

  // Accept the connection
  std::unique_ptr<IDataSocket> socket = m_listen->accept();
  if (!socket) {
    LOG_ERR("[FileTransfer] Failed to accept client connection");
    return;
  }

  handleClientAccepted(socket.release());
}

void FileTransferListener::handleClientAccepted(IDataSocket *socket)
{
  LOG_INFO("[FileTransfer] Client connected to file transfer port");

  // Create stream for the socket
  deskflow::IStream *stream = new PacketStreamFilter(m_events, socket, m_socketMultiplexer);

  // Register disconnect handler
  m_events->addHandler(EventTypes::SocketDisconnected, socket, [this, socket](const auto &) {
    handleClientDisconnected(socket);
  });

  // Read handshake: client should send requestId
  uint32_t requestId = 0;

  try {
    // Read 4 bytes for requestId
    uint8_t buffer[4];
    stream->read(buffer, 4);
    requestId = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];

    LOG_INFO("[FileTransfer] Handshake received, requestId=%u", requestId);

    // Find pending request
    auto it = m_pendingRequests.find(requestId);
    if (it == m_pendingRequests.end()) {
      LOG_ERR("[FileTransfer] Unknown requestId=%u, closing connection", requestId);
      delete stream;
      delete socket;
      return;
    }

    // Store active session
    ActiveSession session;
    session.requestId = requestId;
    session.socket = socket;
    session.stream = stream;
    session.lastActivity = std::time(nullptr);

    m_activeSessions[socket] = session;

    // Start sending file
    sendFileOnConnection(socket, it->second);

    // Remove from pending
    m_pendingRequests.erase(it);

  } catch (BaseException &e) {
    LOG_ERR("[FileTransfer] Handshake failed: %s", e.what());
    delete stream;
    delete socket;
  }
}

void FileTransferListener::sendFileOnConnection(IDataSocket *socket, const PendingRequest &request)
{
  auto it = m_activeSessions.find(socket);
  if (it == m_activeSessions.end()) {
    LOG_ERR("[FileTransfer] Socket not in active sessions");
    return;
  }

  deskflow::IStream *stream = it->second.stream;

  LOG_INFO("[FileTransfer] Sending file on dedicated connection: requestId=%u, file=%s", request.requestId,
           request.filePath.c_str());

  // Use existing FileTransfer::sendFile or sendFileWithMetadata logic
  // For now, use the simplified sendFile
  bool success = FileTransfer::sendFile(stream, request.requestId, request.filePath);

  if (success) {
    LOG_INFO("[FileTransfer] File sent successfully, requestId=%u", request.requestId);
  } else {
    LOG_ERR("[FileTransfer] Failed to send file, requestId=%u", request.requestId);
  }

  // Update activity time
  it->second.lastActivity = std::time(nullptr);

  // Close connection after transfer
  // Note: In real implementation, could keep alive for next transfer
  LOG_INFO("[FileTransfer] Closing connection after transfer, requestId=%u", request.requestId);
  handleClientDisconnected(socket);
}

void FileTransferListener::handleClientDisconnected(IDataSocket *socket)
{
  auto it = m_activeSessions.find(socket);
  if (it == m_activeSessions.end()) {
    return;
  }

  LOG_INFO("[FileTransfer] Client disconnected, requestId=%u", it->second.requestId);

  m_events->removeHandler(EventTypes::SocketDisconnected, socket);
  delete it->second.stream;
  delete socket;

  m_activeSessions.erase(it);
}

void FileTransferListener::checkIdleConnections()
{
  int64_t now = std::time(nullptr);

  for (auto it = m_activeSessions.begin(); it != m_activeSessions.end();) {
    if (now - it->second.lastActivity > 30) {
      LOG_INFO("[FileTransfer] Closing idle connection (30s timeout), requestId=%u", it->second.requestId);

      IDataSocket *socket = it->first;
      m_events->removeHandler(EventTypes::SocketDisconnected, socket);
      delete it->second.stream;
      delete socket;

      it = m_activeSessions.erase(it);
    } else {
      ++it;
    }
  }
}
