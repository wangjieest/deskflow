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
#include <thread>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define mkdir(dir, mode) _mkdir(dir)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define closesocket close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
typedef int SOCKET;
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

// Blocking helper: read exactly n bytes
static bool recvAll(SOCKET fd, void *buf, size_t n)
{
  char *p = static_cast<char *>(buf);
  size_t got = 0;
  while (got < n) {
    auto r = recv(fd, p + got, static_cast<int>(n - got), 0);
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

void ClipboardTransferClient::requestFile(
    const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
    FileTransferCallback callback
)
{
  LOG_INFO(
      "[ClipboardTransferClient] requesting file (blocking): %s from %s:%u (session=%llu)",
      remotePath.c_str(), sourceAddr.c_str(), port, sessionId
  );

  // Capture everything by value for the thread
  std::string destFolder = m_destFolder;
  uint32_t requestId = generateRequestId();

  std::thread([sourceAddr, port, sessionId, remotePath, requestId, callback, destFolder, this]() {
    // --- plain POSIX blocking connect ---
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(sourceAddr.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
      LOG_ERR("[ClipboardTransferClient] DNS resolve failed for %s", sourceAddr.c_str());
      if (callback) callback(false, "", "DNS resolve failed");
      return;
    }
    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) {
      freeaddrinfo(res);
      if (callback) callback(false, "", "socket() failed");
      return;
    }
    // 10-second connect timeout
    struct timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    if (connect(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0) {
      freeaddrinfo(res);
      closesocket(fd);
      LOG_ERR("[ClipboardTransferClient] connect to %s:%u failed", sourceAddr.c_str(), port);
      if (callback) callback(false, "", "connect failed");
      return;
    }
    freeaddrinfo(res);
    LOG_INFO("[ClipboardTransferClient] connected to %s:%u", sourceAddr.c_str(), port);

    // --- send P2P request ---
    // Format: "P2P " + sessionId(8BE) + requestId(4BE) + pathLen(4BE) + path
    std::vector<uint8_t> req;
    req.insert(req.end(), {'P','2','P',' '});
    for (int i = 7; i >= 0; --i) req.push_back((sessionId >> (8*i)) & 0xFF);
    for (int i = 3; i >= 0; --i) req.push_back((requestId >> (8*i)) & 0xFF);
    uint32_t plen = static_cast<uint32_t>(remotePath.size());
    for (int i = 3; i >= 0; --i) req.push_back((plen >> (8*i)) & 0xFF);
    req.insert(req.end(), remotePath.begin(), remotePath.end());
    send(fd, reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0);
    LOG_DEBUG("[ClipboardTransferClient] sent P2P req: session=%llu reqId=%u path=%s",
              sessionId, requestId, remotePath.c_str());

    // --- read response packets (length-prefixed DFCH) ---
    // Server uses ProtocolUtil::writef which adds 4-byte BE packet length prefix
    // Determine destination path
    std::string fileName = remotePath;
    auto slash = fileName.find_last_of("/\\");
    if (slash != std::string::npos) fileName = fileName.substr(slash + 1);
    std::string localPath;

    bool success = false;
    while (true) {
      // Read 4-byte packet length
      uint8_t lenBuf[4];
      if (!recvAll(fd, lenBuf, 4)) {
        LOG_ERR("[ClipboardTransferClient] failed to read packet length");
        break;
      }
      uint32_t pktLen = (uint32_t(lenBuf[0])<<24)|(uint32_t(lenBuf[1])<<16)|(uint32_t(lenBuf[2])<<8)|lenBuf[3];
      if (pktLen < 9 || pktLen > 64*1024*1024) {
        LOG_ERR("[ClipboardTransferClient] bad packet length %u", pktLen);
        break;
      }
      std::vector<uint8_t> pkt(pktLen);
      if (!recvAll(fd, pkt.data(), pktLen)) {
        LOG_ERR("[ClipboardTransferClient] failed to read packet body");
        break;
      }
      // Verify DFCH header
      if (pkt[0]!='D'||pkt[1]!='F'||pkt[2]!='C'||pkt[3]!='H') {
        LOG_ERR("[ClipboardTransferClient] bad chunk magic");
        break;
      }
      uint32_t rId = (uint32_t(pkt[4])<<24)|(uint32_t(pkt[5])<<16)|(uint32_t(pkt[6])<<8)|pkt[7];
      uint8_t  chunkType = pkt[8];
      uint32_t dataLen = (uint32_t(pkt[9])<<24)|(uint32_t(pkt[10])<<16)|(uint32_t(pkt[11])<<8)|pkt[12];
      const uint8_t *data = pkt.data() + 13;

      LOG_DEBUG("[ClipboardTransferClient] chunk: reqId=%u type=%u dataLen=%u", rId, chunkType, dataLen);

      if (chunkType == static_cast<uint8_t>(FileChunkType::Error)) {
        std::string errMsg(reinterpret_cast<const char*>(data), dataLen);
        LOG_ERR("[ClipboardTransferClient] server error: %s", errMsg.c_str());
        break;
      }
      if (chunkType == static_cast<uint8_t>(FileChunkType::Start)) {
        // data = relativePath string (4-byte len + bytes)
        uint32_t relLen = (uint32_t(data[0])<<24)|(uint32_t(data[1])<<16)|(uint32_t(data[2])<<8)|data[3];
        std::string relPath(reinterpret_cast<const char*>(data+4), relLen);
        // Skip optional extra fields (fileName etc.) — just use fileName from path
        std::string dir = destFolder.empty() ? getTempDirectory() : destFolder;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        // Write to .pasting temp file, rename on completion
        localPath = dir + fileName + ".pasting";
        // Remove any leftover temp file
        ::unlink(localPath.c_str());
        LOG_INFO("[ClipboardTransferClient] receiving file to: %s", localPath.c_str());
        continue;
      }
      if (chunkType == static_cast<uint8_t>(FileChunkType::Data)) {
        std::ofstream f(localPath, std::ios::binary | std::ios::app);
        f.write(reinterpret_cast<const char*>(data), dataLen);
        continue;
      }
      if (chunkType == static_cast<uint8_t>(FileChunkType::End)) {
        // Rename .pasting → final name
        std::string finalPath = localPath.substr(0, localPath.size() - 8); // strip ".pasting"
        ::rename(localPath.c_str(), finalPath.c_str());
        localPath = finalPath;
        LOG_INFO("[ClipboardTransferClient] file received: %s", localPath.c_str());
        success = true;
        break;
      }
    }
    closesocket(fd);
    if (callback) callback(success, localPath, success ? "" : "transfer failed");
  }).detach();
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

void ClipboardTransferClient::setDestinationFolder(const std::string &folder)
{
  m_destFolder = folder;
  // Reset cached temp dir so getTempDirectory() picks up the new folder
  m_tempDir.clear();

  if (!folder.empty()) {
    LOG_INFO("[ClipboardTransferClient] destination folder set: %s", folder.c_str());
  } else {
    LOG_DEBUG("[ClipboardTransferClient] destination folder cleared, using temp");
  }
}

std::string ClipboardTransferClient::getTempDirectory() const
{
  // Use explicit destination folder if set
  if (!m_destFolder.empty()) {
    std::string dir = m_destFolder;
    if (dir.back() != '/' && dir.back() != '\\') {
      dir += '/';
    }
    mkdir(dir.c_str(), 0755);
    return dir;
  }

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

    // kMsgDFileChunk = "DFCH%4i%1i%s" - readf must consume the "DFCH" header too
    if (!ProtocolUtil::readf(conn->stream, kMsgDFileChunk, &requestId, &chunkType, &data)) {
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
