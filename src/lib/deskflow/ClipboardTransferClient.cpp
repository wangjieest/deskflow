/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardTransferClient.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"
#include "net/SocketException.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(dir, mode) _mkdir(dir)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

ClipboardTransferClient::ClipboardTransferClient(IEventQueue *events, SocketMultiplexer *socketMultiplexer)
    : m_events(events),
      m_socketMultiplexer(socketMultiplexer)
{
  LOG_DEBUG("[ClipboardTransferClient] created");

  // Initialize random seed for request IDs
  std::srand(static_cast<unsigned>(std::time(nullptr)));
  m_nextRequestId = static_cast<uint32_t>(std::rand()) & 0xFFFF;
}

ClipboardTransferClient::~ClipboardTransferClient()
{
  closeAll();
}

void ClipboardTransferClient::requestFile(
    const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
    FileTransferCallback callback
)
{
  LOG_INFO(
      "[ClipboardTransferClient] requesting file: %s from %s:%u (session=%llu)", remotePath.c_str(), sourceAddr.c_str(),
      port, sessionId
  );

  // Get or create connection
  Connection *conn = getConnection(sourceAddr, port, sessionId);
  if (!conn) {
    LOG_ERR("[ClipboardTransferClient] failed to connect to %s:%u", sourceAddr.c_str(), port);
    if (callback) {
      callback(false, "", "Failed to connect to source");
    }
    return;
  }

  // Generate request ID
  uint32_t requestId = generateRequestId();

  // Create pending request
  PendingRequest req;
  req.requestId = requestId;
  req.remotePath = remotePath;
  req.callback = callback;

  conn->pendingRequests[requestId] = std::move(req);

  // Send request
  if (!sendFileRequest(conn, requestId, remotePath)) {
    LOG_ERR("[ClipboardTransferClient] failed to send request");
    completeRequest(conn, requestId, false, "Failed to send request");
  }
}

void ClipboardTransferClient::closeAll()
{
  LOG_DEBUG("[ClipboardTransferClient] closing all connections");

  for (auto &pair : m_connections) {
    Connection *conn = pair.second.get();

    // Cancel pending requests
    for (auto &reqPair : conn->pendingRequests) {
      if (reqPair.second.file) {
        reqPair.second.file->close();
        delete reqPair.second.file;
      }
      if (reqPair.second.callback) {
        reqPair.second.callback(false, "", "Connection closed");
      }
    }

    // Remove handlers and close
    if (conn->socket) {
      m_events->removeHandler(EventTypes::StreamInputReady, conn->socket);
      m_events->removeHandler(EventTypes::SocketDisconnected, conn->socket);
      delete conn->stream;
      delete conn->socket;
    }
  }

  m_connections.clear();
}

std::string ClipboardTransferClient::getTempDirectory() const
{
  if (!m_tempDir.empty()) {
    return m_tempDir;
  }

#ifdef _WIN32
  char tempPath[MAX_PATH];
  if (GetTempPathA(MAX_PATH, tempPath) > 0) {
    m_tempDir = std::string(tempPath) + "deskflow_transfer\\";
  } else {
    m_tempDir = "C:\\Temp\\deskflow_transfer\\";
  }
#else
  const char *tmpdir = std::getenv("TMPDIR");
  if (!tmpdir) {
    tmpdir = "/tmp";
  }
  m_tempDir = std::string(tmpdir) + "/deskflow_transfer/";
#endif

  // Create directory if it doesn't exist
  mkdir(m_tempDir.c_str(), 0755);

  LOG_DEBUG("[ClipboardTransferClient] temp directory: %s", m_tempDir.c_str());
  return m_tempDir;
}

ClipboardTransferClient::Connection *ClipboardTransferClient::getConnection(
    const std::string &addr, uint16_t port, uint64_t sessionId
)
{
  std::string key = addr + ":" + std::to_string(port);

  auto it = m_connections.find(key);
  if (it != m_connections.end() && it->second->socket) {
    // Existing connection
    LOG_DEBUG("[ClipboardTransferClient] reusing connection to %s", key.c_str());
    return it->second.get();
  }

  // Create new connection
  LOG_INFO("[ClipboardTransferClient] connecting to %s:%u", addr.c_str(), port);

  try {
    NetworkAddress netAddr(addr, port);
    netAddr.resolve();

    auto socket = new TCPSocket(m_events, m_socketMultiplexer, ARCH->getAddrFamily(netAddr.getAddress()));
    socket->connect(netAddr);

    // Register handlers
    m_events->addHandler(EventTypes::StreamInputReady, socket, [this, socket](const auto &) {
      handleDataReady(socket);
    });

    m_events->addHandler(EventTypes::SocketDisconnected, socket, [this, socket](const auto &) {
      handleDisconnected(socket);
    });

    // Create connection object
    auto conn = std::make_unique<Connection>();
    conn->socket = socket;
    conn->stream = socket; // Use socket directly (no PacketStreamFilter for reading)
    conn->address = addr;
    conn->port = port;
    conn->sessionId = sessionId;

    Connection *result = conn.get();
    m_connections[key] = std::move(conn);

    LOG_INFO("[ClipboardTransferClient] connected to %s", key.c_str());
    return result;

  } catch (const std::exception &e) {
    LOG_ERR("[ClipboardTransferClient] connection failed: %s", e.what());
    return nullptr;
  }
}

bool ClipboardTransferClient::sendFileRequest(Connection *conn, uint32_t requestId, const std::string &filePath)
{
  if (!conn || !conn->socket) {
    return false;
  }

  // Build P2P request: "P2P " (4) + sessionId (8) + requestId (4) + pathLen (4) + path
  std::string request = "P2P ";

  // Session ID (8 bytes, big-endian)
  uint64_t sessionId = conn->sessionId;
  request.push_back(static_cast<char>((sessionId >> 56) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 48) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 40) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 32) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 24) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 16) & 0xFF));
  request.push_back(static_cast<char>((sessionId >> 8) & 0xFF));
  request.push_back(static_cast<char>(sessionId & 0xFF));

  // Request ID (4 bytes, big-endian)
  request.push_back(static_cast<char>((requestId >> 24) & 0xFF));
  request.push_back(static_cast<char>((requestId >> 16) & 0xFF));
  request.push_back(static_cast<char>((requestId >> 8) & 0xFF));
  request.push_back(static_cast<char>(requestId & 0xFF));

  // Path length (4 bytes, big-endian)
  uint32_t pathLen = static_cast<uint32_t>(filePath.size());
  request.push_back(static_cast<char>((pathLen >> 24) & 0xFF));
  request.push_back(static_cast<char>((pathLen >> 16) & 0xFF));
  request.push_back(static_cast<char>((pathLen >> 8) & 0xFF));
  request.push_back(static_cast<char>(pathLen & 0xFF));

  // Path
  request.append(filePath);

  try {
    conn->socket->write(request.data(), static_cast<uint32_t>(request.size()));
    LOG_DEBUG(
        "[ClipboardTransferClient] sent request: id=%u, session=%llu, path=%s", requestId, sessionId, filePath.c_str()
    );
    return true;
  } catch (const std::exception &e) {
    LOG_ERR("[ClipboardTransferClient] failed to send request: %s", e.what());
    return false;
  }
}

void ClipboardTransferClient::handleDataReady(IDataSocket *socket)
{
  // Find connection
  Connection *conn = nullptr;
  for (auto &pair : m_connections) {
    if (pair.second->socket == socket) {
      conn = pair.second.get();
      break;
    }
  }

  if (!conn) {
    return;
  }

  // Read response using ProtocolUtil (server sends in kMsgDFileChunk format)
  try {
    uint32_t requestId;
    uint8_t chunkType;
    std::string data;

    // kMsgDFileChunk format: "DFCH" + requestId(4) + chunkType(1) + data
    if (!ProtocolUtil::readf(conn->stream, kMsgDFileChunk + 4, &requestId, &chunkType, &data)) {
      LOG_DEBUG("[ClipboardTransferClient] waiting for more data");
      return;
    }

    LOG_DEBUG("[ClipboardTransferClient] received chunk: request=%u, type=%u, size=%zu", requestId, chunkType, data.size());

    processChunk(conn, requestId, static_cast<FileChunkType>(chunkType), data);

  } catch (const std::exception &e) {
    LOG_ERR("[ClipboardTransferClient] error reading response: %s", e.what());
  }
}

void ClipboardTransferClient::handleDisconnected(IDataSocket *socket)
{
  LOG_INFO("[ClipboardTransferClient] connection disconnected");

  // Find and remove connection
  for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
    if (it->second->socket == socket) {
      Connection *conn = it->second.get();

      // Fail pending requests
      for (auto &reqPair : conn->pendingRequests) {
        if (reqPair.second.file) {
          reqPair.second.file->close();
          delete reqPair.second.file;
        }
        if (reqPair.second.callback) {
          reqPair.second.callback(false, "", "Connection lost");
        }
      }

      m_events->removeHandler(EventTypes::StreamInputReady, socket);
      m_events->removeHandler(EventTypes::SocketDisconnected, socket);
      delete socket;

      m_connections.erase(it);
      break;
    }
  }
}

void ClipboardTransferClient::processChunk(
    Connection *conn, uint32_t requestId, FileChunkType chunkType, const std::string &data
)
{
  auto it = conn->pendingRequests.find(requestId);
  if (it == conn->pendingRequests.end()) {
    LOG_WARN("[ClipboardTransferClient] received chunk for unknown request %u", requestId);
    return;
  }

  PendingRequest &req = it->second;

  switch (chunkType) {
  case FileChunkType::Start: {
    // Parse start chunk to get file info
    // Format: requestId(4) + nameLen(4) + name + relPathLen(4) + relPath + size(8) + isDir(1)
    if (data.size() < 4) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }

    size_t pos = 4; // Skip requestId in data

    // Name length and name
    if (pos + 4 > data.size()) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }
    uint32_t nameLen = (static_cast<uint8_t>(data[pos]) << 24) | (static_cast<uint8_t>(data[pos + 1]) << 16) |
                       (static_cast<uint8_t>(data[pos + 2]) << 8) | static_cast<uint8_t>(data[pos + 3]);
    pos += 4;

    if (pos + nameLen > data.size()) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }
    std::string fileName(data.data() + pos, nameLen);
    pos += nameLen;

    // Relative path length and path
    if (pos + 4 > data.size()) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }
    uint32_t relPathLen = (static_cast<uint8_t>(data[pos]) << 24) | (static_cast<uint8_t>(data[pos + 1]) << 16) |
                          (static_cast<uint8_t>(data[pos + 2]) << 8) | static_cast<uint8_t>(data[pos + 3]);
    pos += 4;

    if (pos + relPathLen > data.size()) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }
    req.relativePath = std::string(data.data() + pos, relPathLen);
    pos += relPathLen;

    // Size (8 bytes)
    if (pos + 8 > data.size()) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }
    req.expectedSize = (static_cast<uint64_t>(static_cast<uint8_t>(data[pos])) << 56) |
                       (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 1])) << 48) |
                       (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 2])) << 40) |
                       (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 3])) << 32) |
                       (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 4])) << 24) |
                       (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 5])) << 16) |
                       (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 6])) << 8) |
                       static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 7]));
    pos += 8;

    // Is directory (1 byte)
    if (pos + 1 > data.size()) {
      completeRequest(conn, requestId, false, "Invalid start chunk");
      return;
    }
    req.isDir = (data[pos] != 0);

    LOG_INFO(
        "[ClipboardTransferClient] start: name=%s, relPath=%s, size=%llu, isDir=%d", fileName.c_str(),
        req.relativePath.c_str(), req.expectedSize, req.isDir
    );

    // Create local path
    req.localPath = createLocalPath(req.relativePath.empty() ? fileName : req.relativePath);

    if (req.isDir) {
      // Create directory
      mkdir(req.localPath.c_str(), 0755);
    } else {
      // Open file for writing
      req.file = new std::ofstream(req.localPath, std::ios::binary);
      if (!req.file->is_open()) {
        LOG_ERR("[ClipboardTransferClient] failed to create file: %s", req.localPath.c_str());
        delete req.file;
        req.file = nullptr;
        completeRequest(conn, requestId, false, "Failed to create local file");
        return;
      }
    }

    req.receivedSize = 0;
    break;
  }

  case FileChunkType::Data: {
    if (req.file && req.file->is_open()) {
      req.file->write(data.data(), data.size());
      req.receivedSize += data.size();
      LOG_DEBUG("[ClipboardTransferClient] data: %zu bytes (total: %llu/%llu)", data.size(), req.receivedSize, req.expectedSize);
    }
    break;
  }

  case FileChunkType::End: {
    LOG_INFO("[ClipboardTransferClient] end: %s (%llu bytes)", req.localPath.c_str(), req.receivedSize);
    completeRequest(conn, requestId, true);
    break;
  }

  case FileChunkType::Error: {
    LOG_ERR("[ClipboardTransferClient] error from server: %s", data.c_str());
    completeRequest(conn, requestId, false, data);
    break;
  }
  }
}

void ClipboardTransferClient::completeRequest(Connection *conn, uint32_t requestId, bool success, const std::string &error)
{
  auto it = conn->pendingRequests.find(requestId);
  if (it == conn->pendingRequests.end()) {
    return;
  }

  PendingRequest &req = it->second;

  // Close file if open
  if (req.file) {
    req.file->close();
    delete req.file;
    req.file = nullptr;
  }

  // Call callback
  if (req.callback) {
    if (success) {
      req.callback(true, req.localPath, "");
    } else {
      req.callback(false, "", error.empty() ? "Unknown error" : error);
    }
  }

  // Remove request
  conn->pendingRequests.erase(it);
}

uint32_t ClipboardTransferClient::generateRequestId()
{
  return m_nextRequestId++;
}

std::string ClipboardTransferClient::createLocalPath(const std::string &relativePath)
{
  std::string basePath = getTempDirectory();

  // Handle subdirectories in relative path
  std::string fullPath = basePath + relativePath;

  // Create parent directories if needed
  size_t pos = 0;
  while ((pos = fullPath.find_first_of("/\\", pos + 1)) != std::string::npos) {
    std::string dir = fullPath.substr(0, pos);
    mkdir(dir.c_str(), 0755);
  }

  return fullPath;
}
