/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoAutoAutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/FileTransferConnection.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/StreamBuffer.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"
#include "net/SocketException.h"
#include "net/TCPSocket.h"

FileTransferConnection::FileTransferConnection(IEventQueue *events, SocketMultiplexer *socketMultiplexer)
    : m_events(events),
      m_socketMultiplexer(socketMultiplexer)
{
  LOG_DEBUG("[FileTransfer] FileTransferConnection created");
}

FileTransferConnection::~FileTransferConnection()
{
  close();
}

bool FileTransferConnection::connect(const NetworkAddress &serverAddress, uint32_t requestId)
{
  if (m_socket) {
    LOG_WARN("[FileTransfer] Already connected");
    return true;
  }

  m_requestId = requestId;

  try {
    // Create socket to file transfer port (24802)
    NetworkAddress fileTransferAddr(serverAddress.getHostname(), 24802);

    LOG_INFO("[FileTransfer] Connecting to %s:24802 for requestId=%u", serverAddress.getHostname().c_str(), requestId);

    m_socket = new TCPSocket(m_events, m_socketMultiplexer, ARCH->getAddrFamily(fileTransferAddr.getAddress()));
    m_socket->connect(fileTransferAddr);

    // Create stream
    m_stream = new PacketStreamFilter(m_events, m_socket, m_socketMultiplexer);

    LOG_INFO("[FileTransfer] ✅ Connected to file transfer channel");

    // Send handshake: requestId (4 bytes, big-endian)
    uint8_t handshake[4];
    handshake[0] = (requestId >> 24) & 0xFF;
    handshake[1] = (requestId >> 16) & 0xFF;
    handshake[2] = (requestId >> 8) & 0xFF;
    handshake[3] = requestId & 0xFF;

    m_stream->write(handshake, 4);
    LOG_INFO("[FileTransfer] Handshake sent, requestId=%u", requestId);

    // Setup data ready handler
    m_events->addHandler(EventTypes::StreamInputReady, m_stream, [this](const auto &) {
      handleDataReady();
    });

    // Setup disconnect handler
    m_events->addHandler(EventTypes::SocketDisconnected, m_socket, [this](const auto &) {
      handleDisconnected();
    });

    return true;

  } catch (SocketConnectException &e) {
    LOG_ERR("[FileTransfer] Failed to connect to file transfer port: %s", e.what());
    if (m_stream) {
      delete m_stream;
      m_stream = nullptr;
    }
    if (m_socket) {
      delete m_socket;
      m_socket = nullptr;
    }
    return false;
  } catch (BaseException &e) {
    LOG_ERR("[FileTransfer] Connection error: %s", e.what());
    if (m_stream) {
      delete m_stream;
      m_stream = nullptr;
    }
    if (m_socket) {
      delete m_socket;
      m_socket = nullptr;
    }
    return false;
  }
}

bool FileTransferConnection::connectPointToPoint(
    const std::string &address, uint16_t port, uint32_t requestId, uint64_t sessionId, const std::string &filePath
)
{
  if (m_socket && m_stream) {
    // Already connected - send new file request on existing connection
    LOG_INFO("[FileTransfer] Already connected, sending new file request on existing connection");
    m_requestId = requestId;
    return sendFileRequest(sessionId, requestId, filePath);
  }

  m_requestId = requestId;

  try {
    // Create socket to specified address and port (point-to-point)
    NetworkAddress p2pAddr(address, port);

    LOG_INFO(
        "[FileTransfer] Point-to-point connecting to %s:%u for requestId=%u, sessionId=%llu", address.c_str(), port,
        requestId, sessionId
    );

    // Must call resolve() to convert hostname to address
    LOG_DEBUG("[FileTransfer] resolving address...");
    p2pAddr.resolve();
    LOG_DEBUG("[FileTransfer] address resolved");

    auto addrHandle = p2pAddr.getAddress();
    LOG_DEBUG("[FileTransfer] got address handle=%p", addrHandle);

    if (!addrHandle) {
      LOG_ERR("[FileTransfer] NetworkAddress::getAddress() returned null after resolve");
      return false;
    }

    LOG_DEBUG("[FileTransfer] getting addr family...");
    auto addrFamily = ARCH->getAddrFamily(addrHandle);
    LOG_DEBUG("[FileTransfer] addr family=%d", static_cast<int>(addrFamily));

    LOG_DEBUG("[FileTransfer] creating TCPSocket (events=%p, multiplexer=%p)...", m_events, m_socketMultiplexer);
    m_socket = new TCPSocket(m_events, m_socketMultiplexer, addrFamily);
    LOG_DEBUG("[FileTransfer] TCPSocket created, connecting...");
    m_socket->connect(p2pAddr);

    // For P2P, we use raw socket instead of PacketStreamFilter
    // because ClipboardTransferServer reads raw bytes without packet framing
    m_stream = m_socket;  // Use socket directly as stream

    LOG_INFO("[FileTransfer] Point-to-point connection established");

    // Send point-to-point handshake directly to socket (no PacketStreamFilter framing):
    // Protocol: "P2P " (4 bytes) + sessionId (8 bytes) + requestId (4 bytes) + pathLength (4 bytes) + path
    std::string handshake = "P2P ";

    // Append sessionId (8 bytes, big-endian)
    handshake.push_back(static_cast<char>((sessionId >> 56) & 0xFF));
    handshake.push_back(static_cast<char>((sessionId >> 48) & 0xFF));
    handshake.push_back(static_cast<char>((sessionId >> 40) & 0xFF));
    handshake.push_back(static_cast<char>((sessionId >> 32) & 0xFF));
    handshake.push_back(static_cast<char>((sessionId >> 24) & 0xFF));
    handshake.push_back(static_cast<char>((sessionId >> 16) & 0xFF));
    handshake.push_back(static_cast<char>((sessionId >> 8) & 0xFF));
    handshake.push_back(static_cast<char>(sessionId & 0xFF));

    // Append requestId (4 bytes, big-endian)
    handshake.push_back(static_cast<char>((requestId >> 24) & 0xFF));
    handshake.push_back(static_cast<char>((requestId >> 16) & 0xFF));
    handshake.push_back(static_cast<char>((requestId >> 8) & 0xFF));
    handshake.push_back(static_cast<char>(requestId & 0xFF));

    // Append path length (4 bytes, big-endian)
    uint32_t pathLen = static_cast<uint32_t>(filePath.size());
    handshake.push_back(static_cast<char>((pathLen >> 24) & 0xFF));
    handshake.push_back(static_cast<char>((pathLen >> 16) & 0xFF));
    handshake.push_back(static_cast<char>((pathLen >> 8) & 0xFF));
    handshake.push_back(static_cast<char>(pathLen & 0xFF));

    // Append file path
    handshake.append(filePath);

    // Write directly to socket (raw bytes, no packet framing)
    m_socket->write(handshake.data(), static_cast<uint32_t>(handshake.size()));
    LOG_INFO("[FileTransfer] P2P handshake sent (raw), sessionId=%llu, requestId=%u, path=%s", sessionId, requestId, filePath.c_str());

    // Setup data ready handler
    m_events->addHandler(EventTypes::StreamInputReady, m_stream, [this](const auto &) { handleDataReady(); });

    // Setup disconnect handler
    m_events->addHandler(EventTypes::SocketDisconnected, m_socket, [this](const auto &) { handleDisconnected(); });

    return true;

  } catch (SocketConnectException &e) {
    LOG_ERR("[FileTransfer] Point-to-point connection failed: %s", e.what());
    if (m_stream) {
      delete m_stream;
      m_stream = nullptr;
    }
    if (m_socket) {
      delete m_socket;
      m_socket = nullptr;
    }
    return false;
  } catch (BaseException &e) {
    LOG_ERR("[FileTransfer] Point-to-point error: %s", e.what());
    if (m_stream) {
      delete m_stream;
      m_stream = nullptr;
    }
    if (m_socket) {
      delete m_socket;
      m_socket = nullptr;
    }
    return false;
  }
}

bool FileTransferConnection::sendFileRequest(uint64_t sessionId, uint32_t requestId, const std::string &filePath)
{
  if (!m_stream) {
    LOG_ERR("[FileTransfer] Cannot send file request - no stream");
    return false;
  }

  // Build and send file request (same format as initial handshake)
  // Protocol: "P2P " (4 bytes) + sessionId (8 bytes) + requestId (4 bytes) + pathLength (4 bytes) + path
  std::string request = "P2P ";

  // Append sessionId (8 bytes, big-endian)
  request.push_back(static_cast<char>((sessionId >> 56) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 48) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 40) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 32) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 24) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 16) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 8) & 0xFF));
  request.push_back(static_cast<char>(sessionId & 0xFF));

  // Append requestId (4 bytes, big-endian)
  request.push_back(static_cast<char>((requestId >> 24) & 0xFF));
  request.push_back(static_cast<char>((requestId >> 16) & 0xFF));
  request.push_back(static_cast<char>((requestId >> 8) & 0xFF));
  request.push_back(static_cast<char>(requestId & 0xFF));

  // Append path length (4 bytes, big-endian)
  uint32_t pathLen = static_cast<uint32_t>(filePath.size());
  request.push_back(static_cast<char>((pathLen >> 24) & 0xFF));
  request.push_back(static_cast<char>((pathLen >> 16) & 0xFF));
  request.push_back(static_cast<char>((pathLen >> 8) & 0xFF));
  request.push_back(static_cast<char>(pathLen & 0xFF));

  // Append file path
  request.append(filePath);

  m_stream->write(request.data(), static_cast<uint32_t>(request.size()));
  LOG_INFO("[FileTransfer] P2P file request sent, sessionId=%llu, requestId=%u, path=%s", sessionId, requestId, filePath.c_str());

  return true;
}

void FileTransferConnection::setDataCallback(DataCallback callback)
{
  m_dataCallback = callback;
}

void FileTransferConnection::handleDataReady()
{
  if (!m_stream || !m_dataCallback) {
    return;
  }

  try {
    // Read file chunk message: same format as on main connection
    // Format: chunk_type (1 byte) + data_length (4 bytes) + data
    uint8_t chunkType;
    uint32_t dataLength;
    std::string data;

    // This reuses the protocol from kMsgDFileChunk but without the requestId prefix
    // Actually, let's keep it simple and use full kMsgDFileChunk format
    uint32_t requestId;
    if (!ProtocolUtil::readf(m_stream, kMsgDFileChunk + 4, &requestId, &chunkType, &data)) {
      LOG_ERR("[FileTransfer] Failed to read file chunk");
      return;
    }

    LOG_DEBUG1("[FileTransfer] Received chunk: type=%u, size=%zu", chunkType, data.size());

    // Call callback with chunk data
    m_dataCallback(static_cast<FileChunkType>(chunkType), data);

    // If this is an End or Error chunk, close connection
    if (chunkType == static_cast<uint8_t>(FileChunkType::End) ||
        chunkType == static_cast<uint8_t>(FileChunkType::Error)) {
      LOG_INFO("[FileTransfer] Transfer complete, closing connection");
      close();
    }

  } catch (BaseException &e) {
    LOG_ERR("[FileTransfer] Error reading data: %s", e.what());
    close();
  }
}

void FileTransferConnection::handleDisconnected()
{
  LOG_INFO("[FileTransfer] Connection disconnected");
  close();
}

void FileTransferConnection::close()
{
  if (!m_socket) {
    return;
  }

  LOG_DEBUG("[FileTransfer] Closing file transfer connection");

  if (m_stream) {
    m_events->removeHandler(EventTypes::StreamInputReady, m_stream);
  }
  if (m_socket) {
    m_events->removeHandler(EventTypes::SocketDisconnected, m_socket);
  }

  // Note: For P2P connections, m_stream == m_socket (no PacketStreamFilter)
  // Only delete if they are different objects
  if (m_stream != m_socket) {
    delete m_stream;
  }
  delete m_socket;

  m_stream = nullptr;
  m_socket = nullptr;
}
