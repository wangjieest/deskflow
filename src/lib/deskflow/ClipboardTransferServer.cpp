/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardTransferServer.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"
#include "net/SocketException.h"

#include <ctime>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <iphlpapi.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

ClipboardTransferServer::ClipboardTransferServer(
    IEventQueue *events, SocketMultiplexer *socketMultiplexer, ISocketFactory *socketFactory
)
    : m_events(events),
      m_socketMultiplexer(socketMultiplexer),
      m_socketFactory(socketFactory)
{
  LOG_DEBUG("[ClipboardTransferServer] created");
}

ClipboardTransferServer::~ClipboardTransferServer()
{
  stop();
}

bool ClipboardTransferServer::start()
{
  if (m_listen) {
    LOG_WARN("[ClipboardTransferServer] already started on port %u", m_port);
    return true;
  }

  // Try ports starting from 24803
  const uint16_t startPort = 24803;
  const uint16_t endPort = 24899;

  for (uint16_t port = startPort; port <= endPort; ++port) {
    try {
      NetworkAddress addr("0.0.0.0", port);
      addr.resolve();

      m_listen = m_socketFactory->createListen(ARCH->getAddrFamily(addr.getAddress()), SecurityLevel::PlainText);

      m_events->addHandler(EventTypes::ListenSocketConnecting, m_listen, [this](const auto &) {
        handleClientConnecting();
      });

      m_listen->bind(addr);

      m_port = port;
      LOG_INFO("[ClipboardTransferServer] started on port %u", m_port);
      return true;
    } catch (SocketAddressInUseException &) {
      if (m_listen) {
        delete m_listen;
        m_listen = nullptr;
      }
      continue;
    } catch (BaseException &e) {
      LOG_ERR("[ClipboardTransferServer] failed to start on port %u: %s", port, e.what());
      if (m_listen) {
        delete m_listen;
        m_listen = nullptr;
      }
      return false;
    }
  }

  LOG_ERR("[ClipboardTransferServer] no available port in range %u-%u", startPort, endPort);
  return false;
}

void ClipboardTransferServer::stop()
{
  if (!m_listen) {
    return;
  }

  LOG_INFO("[ClipboardTransferServer] stopping (port %u)", m_port);

  for (auto &pair : m_connections) {
    IDataSocket *socket = pair.first;
    m_events->removeHandler(EventTypes::SocketDisconnected, socket);
    m_events->removeHandler(EventTypes::StreamInputReady, socket);
    delete pair.second.stream;
    delete socket;
  }
  m_connections.clear();

  m_events->removeHandler(EventTypes::ListenSocketConnecting, m_listen);

  delete m_listen;
  m_listen = nullptr;
  m_port = 0;

  LOG_INFO("[ClipboardTransferServer] stopped");
}

std::string ClipboardTransferServer::getLocalAddress() const
{
  if (!m_localAddress.empty()) {
    return m_localAddress;
  }

#ifdef _WIN32
  ULONG bufferSize = 15000;
  std::vector<uint8_t> buffer(bufferSize);
  PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &bufferSize);

  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(bufferSize);
    addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &bufferSize);
  }

  if (result == NO_ERROR) {
    for (PIP_ADAPTER_ADDRESSES adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp) {
        continue;
      }

      if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
        continue;
      }

      for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress; unicast != nullptr;
           unicast = unicast->Next) {
        sockaddr *addr = unicast->Address.lpSockaddr;
        if (addr->sa_family == AF_INET) {
          char ipStr[INET_ADDRSTRLEN];
          sockaddr_in *ipv4 = reinterpret_cast<sockaddr_in *>(addr);
          inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);

          if (strncmp(ipStr, "127.", 4) != 0) {
            m_localAddress = ipStr;
            LOG_DEBUG("[ClipboardTransferServer] using local address: %s", m_localAddress.c_str());
            return m_localAddress;
          }
        }
      }
    }
  }
#else
  struct ifaddrs *ifaddrs = nullptr;
  if (getifaddrs(&ifaddrs) == 0) {
    for (struct ifaddrs *ifa = ifaddrs; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr) {
        continue;
      }

      if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_RUNNING)) {
        continue;
      }

      if (ifa->ifa_addr->sa_family == AF_INET) {
        char ipStr[INET_ADDRSTRLEN];
        struct sockaddr_in *ipv4 = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
        inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);

        if (strncmp(ipStr, "127.", 4) != 0) {
          m_localAddress = ipStr;
          freeifaddrs(ifaddrs);
          LOG_DEBUG("[ClipboardTransferServer] using local address: %s", m_localAddress.c_str());
          return m_localAddress;
        }
      }
    }
    freeifaddrs(ifaddrs);
  }
#endif

  LOG_WARN("[ClipboardTransferServer] could not determine local address");
  return "";
}

void ClipboardTransferServer::setSessionFiles(uint64_t sessionId, const std::vector<FileTransferFileInfo> &files)
{
  Session session;
  session.sessionId = sessionId;
  session.files = files;

  for (size_t i = 0; i < files.size(); ++i) {
    session.pathToIndex[files[i].path] = i;
  }

  m_sessions[sessionId] = std::move(session);

  LOG_INFO("[ClipboardTransferServer] session %llu: %zu files available", sessionId, files.size());
}

void ClipboardTransferServer::clearSession(uint64_t sessionId)
{
  auto it = m_sessions.find(sessionId);
  if (it != m_sessions.end()) {
    LOG_DEBUG("[ClipboardTransferServer] cleared session %llu", sessionId);
    m_sessions.erase(it);
  }
}

void ClipboardTransferServer::clearAllSessions()
{
  m_sessions.clear();
  LOG_DEBUG("[ClipboardTransferServer] cleared all sessions");
}

void ClipboardTransferServer::handleClientConnecting()
{
  if (!m_listen) {
    return;
  }

  LOG_DEBUG("[ClipboardTransferServer] client connecting");

  std::unique_ptr<IDataSocket> socket = m_listen->accept();
  if (!socket) {
    LOG_ERR("[ClipboardTransferServer] failed to accept connection");
    return;
  }

  IDataSocket *rawSocket = socket.release();

  // Register handlers on the raw socket
  m_events->addHandler(EventTypes::SocketDisconnected, rawSocket, [this, rawSocket](const auto &) {
    handleClientDisconnected(rawSocket);
  });

  m_events->addHandler(EventTypes::StreamInputReady, rawSocket, [this, rawSocket](const auto &) {
    handleClientData(rawSocket);
  });

  // Store connection (no PacketStreamFilter wrapper for reading - we read raw bytes)
  ClientConnection conn;
  conn.socket = rawSocket;
  conn.stream = rawSocket; // Use socket directly for both read and write
  conn.lastActivity = std::time(nullptr);
  m_connections[rawSocket] = conn;

  LOG_INFO("[ClipboardTransferServer] client connected (total: %zu)", m_connections.size());
}

void ClipboardTransferServer::handleClientData(IDataSocket *socket)
{
  auto it = m_connections.find(socket);
  if (it == m_connections.end()) {
    return;
  }

  ClientConnection &conn = it->second;
  conn.lastActivity = std::time(nullptr);

  // Read request message directly from socket (raw bytes)
  // P2P format: "P2P " (4 bytes) + sessionId (8 bytes) + requestId (4 bytes) + pathLength (4 bytes) + path
  try {
    uint32_t requestId = 0;
    uint64_t sessionId = 0;
    std::string filePath;

    // Read first 4 bytes
    uint8_t headerBuf[4];
    uint32_t bytesRead = socket->read(headerBuf, 4);
    if (bytesRead != 4) {
      LOG_DEBUG("[ClipboardTransferServer] waiting for more data (got %u bytes)", bytesRead);
      return;
    }

    // Check for P2P magic header
    bool isP2P = (headerBuf[0] == 'P' && headerBuf[1] == '2' && headerBuf[2] == 'P' && headerBuf[3] == ' ');

    if (isP2P) {
      LOG_DEBUG("[ClipboardTransferServer] P2P protocol detected");

      // Read session ID: 8 bytes
      uint8_t sessionIdBuf[8];
      if (socket->read(sessionIdBuf, 8) != 8) {
        LOG_ERR("[ClipboardTransferServer] failed to read P2P session ID");
        return;
      }
      sessionId = (static_cast<uint64_t>(sessionIdBuf[0]) << 56) | (static_cast<uint64_t>(sessionIdBuf[1]) << 48) |
                  (static_cast<uint64_t>(sessionIdBuf[2]) << 40) | (static_cast<uint64_t>(sessionIdBuf[3]) << 32) |
                  (static_cast<uint64_t>(sessionIdBuf[4]) << 24) | (static_cast<uint64_t>(sessionIdBuf[5]) << 16) |
                  (static_cast<uint64_t>(sessionIdBuf[6]) << 8) | static_cast<uint64_t>(sessionIdBuf[7]);

      // Read request ID: 4 bytes
      uint8_t reqIdBuf[4];
      if (socket->read(reqIdBuf, 4) != 4) {
        LOG_ERR("[ClipboardTransferServer] failed to read P2P request ID");
        return;
      }
      requestId = (reqIdBuf[0] << 24) | (reqIdBuf[1] << 16) | (reqIdBuf[2] << 8) | reqIdBuf[3];

    } else {
      // Legacy format: first 4 bytes are requestId
      requestId = (headerBuf[0] << 24) | (headerBuf[1] << 16) | (headerBuf[2] << 8) | headerBuf[3];

      // Read session ID: 8 bytes
      uint8_t sessionIdBuf[8];
      if (socket->read(sessionIdBuf, 8) != 8) {
        LOG_ERR("[ClipboardTransferServer] failed to read session ID");
        return;
      }
      sessionId = (static_cast<uint64_t>(sessionIdBuf[0]) << 56) | (static_cast<uint64_t>(sessionIdBuf[1]) << 48) |
                  (static_cast<uint64_t>(sessionIdBuf[2]) << 40) | (static_cast<uint64_t>(sessionIdBuf[3]) << 32) |
                  (static_cast<uint64_t>(sessionIdBuf[4]) << 24) | (static_cast<uint64_t>(sessionIdBuf[5]) << 16) |
                  (static_cast<uint64_t>(sessionIdBuf[6]) << 8) | static_cast<uint64_t>(sessionIdBuf[7]);
    }

    // Read file path length: 4 bytes
    uint8_t pathLenBuf[4];
    if (socket->read(pathLenBuf, 4) != 4) {
      LOG_ERR("[ClipboardTransferServer] failed to read path length");
      return;
    }
    uint32_t pathLen = (pathLenBuf[0] << 24) | (pathLenBuf[1] << 16) | (pathLenBuf[2] << 8) | pathLenBuf[3];

    if (pathLen > 0 && pathLen < 65536) {
      std::vector<char> pathBuf(pathLen);
      if (socket->read(pathBuf.data(), pathLen) == static_cast<int>(pathLen)) {
        filePath.assign(pathBuf.data(), pathLen);
      }
    }

    LOG_INFO("[ClipboardTransferServer] request: id=%u, session=%llu, path=%s", requestId, sessionId, filePath.c_str());

    conn.sessionId = sessionId;
    processFileRequest(socket, requestId, sessionId, filePath);

  } catch (const std::exception &e) {
    LOG_ERR("[ClipboardTransferServer] error reading request: %s", e.what());
  }
}

void ClipboardTransferServer::handleClientDisconnected(IDataSocket *socket)
{
  auto it = m_connections.find(socket);
  if (it == m_connections.end()) {
    return;
  }

  LOG_INFO("[ClipboardTransferServer] client disconnected");

  m_events->removeHandler(EventTypes::SocketDisconnected, socket);
  m_events->removeHandler(EventTypes::StreamInputReady, socket);

  // Note: stream == socket in our case, so only delete socket
  delete socket;

  m_connections.erase(it);
}

void ClipboardTransferServer::processFileRequest(
    IDataSocket *socket, uint32_t requestId, uint64_t sessionId, const std::string &filePath
)
{
  auto connIt = m_connections.find(socket);
  if (connIt == m_connections.end()) {
    return;
  }

  deskflow::IStream *stream = connIt->second.stream;

  const FileTransferFileInfo *fileInfo = findFileInfo(sessionId, filePath);
  if (!fileInfo) {
    LOG_WARN("[ClipboardTransferServer] file not allowed: session=%llu, path=%s", sessionId, filePath.c_str());
    sendError(stream, requestId, "File not allowed or session expired");
    return;
  }

  if (fileInfo->isDir) {
    sendDirectory(stream, requestId, fileInfo->relativePath);
  } else {
    sendFile(stream, requestId, fileInfo->path, fileInfo->relativePath);
  }
}

bool ClipboardTransferServer::sendFile(
    deskflow::IStream *stream, uint32_t requestId, const std::string &filePath, const std::string &relativePath
)
{
  LOG_INFO("[ClipboardTransferServer] sending file: %s (requestId=%u)", filePath.c_str(), requestId);

  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_ERR("[ClipboardTransferServer] failed to open file: %s", filePath.c_str());
    sendError(stream, requestId, "File not found");
    return false;
  }

  uint64_t fileSize = static_cast<uint64_t>(file.tellg());
  file.seekg(0, std::ios::beg);

  std::string fileName = filePath;
  size_t lastSlash = filePath.find_last_of("/\\");
  if (lastSlash != std::string::npos) {
    fileName = filePath.substr(lastSlash + 1);
  }

  // Send start chunk
  std::string startData = FileTransfer::createStartChunkEx(requestId, fileName, relativePath, fileSize, false);
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Start), &startData);

  LOG_DEBUG("[ClipboardTransferServer] sent start chunk: name=%s, size=%llu", fileName.c_str(), fileSize);

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
    }
  }

  file.close();

  // Send end chunk
  std::string emptyStr;
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::End), &emptyStr);

  LOG_INFO("[ClipboardTransferServer] file transfer complete: %s (%llu bytes)", filePath.c_str(), totalSent);
  return true;
}

void ClipboardTransferServer::sendDirectory(deskflow::IStream *stream, uint32_t requestId, const std::string &relativePath)
{
  LOG_INFO("[ClipboardTransferServer] sending directory: %s (requestId=%u)", relativePath.c_str(), requestId);

  std::string dirName = relativePath;
  size_t lastSlash = relativePath.find_last_of("/\\");
  if (lastSlash != std::string::npos) {
    dirName = relativePath.substr(lastSlash + 1);
  }

  std::string startData = FileTransfer::createStartChunkEx(requestId, dirName, relativePath, 0, true);
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Start), &startData);

  std::string emptyStr;
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::End), &emptyStr);

  LOG_INFO("[ClipboardTransferServer] directory sent: %s", relativePath.c_str());
}

void ClipboardTransferServer::sendError(deskflow::IStream *stream, uint32_t requestId, const std::string &message)
{
  LOG_ERR("[ClipboardTransferServer] sending error for request %u: %s", requestId, message.c_str());
  ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, static_cast<uint8_t>(FileChunkType::Error), &message);
}

bool ClipboardTransferServer::isFileAllowed(uint64_t sessionId, const std::string &filePath) const
{
  return findFileInfo(sessionId, filePath) != nullptr;
}

const FileTransferFileInfo *ClipboardTransferServer::findFileInfo(uint64_t sessionId, const std::string &filePath) const
{
  auto sessionIt = m_sessions.find(sessionId);
  if (sessionIt == m_sessions.end()) {
    return nullptr;
  }

  const Session &session = sessionIt->second;
  auto pathIt = session.pathToIndex.find(filePath);
  if (pathIt == session.pathToIndex.end()) {
    return nullptr;
  }

  return &session.files[pathIt->second];
}

void ClipboardTransferServer::checkIdleConnections()
{
  int64_t now = std::time(nullptr);
  const int64_t idleTimeout = 30;

  for (auto it = m_connections.begin(); it != m_connections.end();) {
    if (now - it->second.lastActivity > idleTimeout) {
      LOG_INFO("[ClipboardTransferServer] closing idle connection");

      IDataSocket *socket = it->first;
      m_events->removeHandler(EventTypes::SocketDisconnected, socket);
      m_events->removeHandler(EventTypes::StreamInputReady, socket);
      delete socket;

      it = m_connections.erase(it);
    } else {
      ++it;
    }
  }
}
