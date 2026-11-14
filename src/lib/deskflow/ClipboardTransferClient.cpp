/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardTransferClient.h"

#include "base/Log.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#define INVALID_SOCKET (-1)
typedef int SOCKET;
#endif

namespace {

constexpr size_t kChunkHeaderSize = 13; // "DFCH" + requestId(4) + type(1) + dataLen(4)
constexpr uint32_t kMaxChunkPayload = 64 * 1024 * 1024;
constexpr int kConnectTimeoutMs = 10000;

// A legitimate LAN transfer is far faster; this only stops a source that trickles data to hold us open
constexpr auto kTransferGrace = std::chrono::seconds(60);
constexpr uint64_t kMinBytesPerSecond = 256 * 1024;
constexpr auto kUnknownSizeAllowance = std::chrono::hours(1);

std::atomic<uint32_t> s_nextDownloadId{1};

bool recvAll(SOCKET fd, void *buf, size_t n)
{
  char *p = static_cast<char *>(buf);
  size_t got = 0;
  while (got < n) {
    auto r = recv(fd, p + got, static_cast<int>(n - got), 0);
    if (r <= 0)
      return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

bool sendAll(SOCKET fd, const void *buf, size_t n)
{
  const char *p = static_cast<const char *>(buf);
  size_t sent = 0;
  while (sent < n) {
    auto r = send(fd, p + sent, static_cast<int>(n - sent), 0);
    if (r <= 0)
      return false;
    sent += static_cast<size_t>(r);
  }
  return true;
}

bool setNonBlocking(SOCKET fd, bool enable)
{
#ifdef _WIN32
  u_long mode = enable ? 1 : 0;
  return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return false;
  return fcntl(fd, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

// A plain connect() to an unreachable peer blocks for the OS default, which can exceed a minute
bool connectWithTimeout(SOCKET fd, const sockaddr *addr, socklen_t addrLen, int timeoutMs)
{
  if (!setNonBlocking(fd, true))
    return connect(fd, addr, addrLen) == 0;

  bool ok = connect(fd, addr, addrLen) == 0;
  if (!ok) {
#ifdef _WIN32
    const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
    const bool pending = errno == EINPROGRESS;
#endif
    if (pending) {
#ifdef _WIN32
      // Windows reports a failed connect through the exception set rather than the write set
      fd_set writable;
      fd_set failed;
      FD_ZERO(&writable);
      FD_ZERO(&failed);
      FD_SET(fd, &writable);
      FD_SET(fd, &failed);
      timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
      const bool ready = select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(fd, &writable);
#else
      pollfd pfd{fd, POLLOUT, 0};
      const bool ready = poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLOUT);
#endif
      if (ready) {
        int error = 0;
        socklen_t errorLen = sizeof(error);
        ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &errorLen) == 0 && error == 0;
      }
    }
  }
  setNonBlocking(fd, false);
  return ok;
}

uint32_t readBE32(const uint8_t *p)
{
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string baseName(const std::string &path)
{
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct StartInfo
{
  std::string relativePath;
  bool isDir = false;
  bool hasSize = false;
  uint64_t size = 0;
};

// Start payload is requestId(4) + type(1) + JSON; locate the JSON rather than trusting the prefix size
StartInfo parseStartPayload(const uint8_t *data, uint32_t len)
{
  StartInfo info;
  const auto *begin = reinterpret_cast<const char *>(data);
  const auto *brace = static_cast<const char *>(std::memchr(begin, '{', std::min<uint32_t>(len, 16)));
  if (brace == nullptr)
    return info;
  const auto doc = QJsonDocument::fromJson(QByteArray(brace, static_cast<qsizetype>(begin + len - brace)));
  if (!doc.isObject())
    return info;
  const auto obj = doc.object();
  info.relativePath = obj.value(QStringLiteral("relativePath")).toString().toStdString();
  if (info.relativePath.empty())
    info.relativePath = obj.value(QStringLiteral("name")).toString().toStdString();
  info.isDir = obj.value(QStringLiteral("isDir")).toBool();
  if (const auto size = obj.value(QStringLiteral("size")); size.isDouble() && size.toDouble() >= 0) {
    info.hasSize = true;
    info.size = static_cast<uint64_t>(size.toDouble());
  }
  return info;
}

std::filesystem::path uniquePath(const std::filesystem::path &path)
{
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return path;
  const auto stem = path.stem().u8string();
  const auto ext = path.extension().u8string();
  for (int i = 2; i < 10000; ++i) {
    const auto n = std::to_string(i);
    auto candidate = path.parent_path() / (stem + u8" " + std::u8string(n.begin(), n.end()) + ext);
    if (!std::filesystem::exists(candidate, ec))
      return candidate;
  }
  return path;
}

} // namespace

void ClipboardTransferClient::requestFile(
    const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
    FileTransferCallback callback
)
{
  const std::string destDir = getTempDirectory();
  std::thread([sourceAddr, port, sessionId, remotePath, destDir, callback = std::move(callback)]() {
    const auto result = downloadFile(sourceAddr, port, sessionId, remotePath, destDir, false);
    if (callback)
      callback(result.success, result.localPath, result.error);
  }).detach();
}

ClipboardTransferClient::DownloadResult ClipboardTransferClient::downloadFile(
    const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
    const std::string &destDir, bool keepExisting
)
{
  namespace fs = std::filesystem;
  using Clock = std::chrono::steady_clock;

  DownloadResult result;
  if (destDir.empty()) {
    result.error = "no destination folder";
    return result;
  }

  const uint32_t requestId = s_nextDownloadId++;
  LOG_DEBUG(
      "[ClipboardTransferClient] downloading %s from %s:%u (session=%llu, request=%u)", remotePath.c_str(),
      sourceAddr.c_str(), port, sessionId, requestId
  );

  addrinfo hints{};
  addrinfo *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const auto portStr = std::to_string(port);
  if (getaddrinfo(sourceAddr.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
    result.error = "cannot resolve " + sourceAddr;
    return result;
  }
  const SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd == INVALID_SOCKET) {
    freeaddrinfo(res);
    result.error = "cannot create socket";
    return result;
  }
#ifdef _WIN32
  DWORD timeoutMs = 30000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
#else
  timeval tv{30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
  const bool connected =
      connectWithTimeout(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen), kConnectTimeoutMs);
  freeaddrinfo(res);
  if (!connected) {
    closesocket(fd);
    result.error = "cannot connect to " + sourceAddr + ":" + portStr;
    return result;
  }

  // "P2P " + sessionId(8) + requestId(4) + pathLen(4) + path, all big-endian
  std::vector<uint8_t> request{'P', '2', 'P', ' '};
  for (int i = 7; i >= 0; --i)
    request.push_back(static_cast<uint8_t>(sessionId >> (8 * i)));
  for (int i = 3; i >= 0; --i)
    request.push_back(static_cast<uint8_t>(requestId >> (8 * i)));
  const auto pathLen = static_cast<uint32_t>(remotePath.size());
  for (int i = 3; i >= 0; --i)
    request.push_back(static_cast<uint8_t>(pathLen >> (8 * i)));
  request.insert(request.end(), remotePath.begin(), remotePath.end());
  if (!sendAll(fd, request.data(), request.size())) {
    closesocket(fd);
    result.error = "cannot send request";
    return result;
  }

  fs::path finalPath;
  fs::path tempPath;
  std::ofstream out;
  bool started = false;
  bool isDir = false;
  bool limitToAnnounced = false;
  uint64_t announcedSize = 0;
  uint64_t written = 0;
  auto deadline = Clock::now() + kTransferGrace;
  std::vector<uint8_t> packet;

  const auto fail = [&](const std::string &error) {
    out.close();
    if (!tempPath.empty()) {
      std::error_code ec;
      fs::remove(tempPath, ec);
    }
    result.error = error;
  };

  // Each chunk is "DFCH" + requestId + type + dataLen + payload, optionally preceded by a 4-byte length
  while (true) {
    if (Clock::now() > deadline) {
      fail("source is too slow");
      break;
    }

    uint8_t first4[4];
    if (!recvAll(fd, first4, sizeof(first4))) {
      fail("connection lost");
      break;
    }
    if (std::memcmp(first4, "DFCH", 4) == 0) {
      uint8_t rest[kChunkHeaderSize - 4];
      if (!recvAll(fd, rest, sizeof(rest))) {
        fail("connection lost");
        break;
      }
      const uint32_t payloadLen = readBE32(rest + 5);
      if (payloadLen > kMaxChunkPayload) {
        fail("chunk too large");
        break;
      }
      packet.resize(kChunkHeaderSize + payloadLen);
      std::memcpy(packet.data(), first4, sizeof(first4));
      std::memcpy(packet.data() + sizeof(first4), rest, sizeof(rest));
      if (payloadLen > 0 && !recvAll(fd, packet.data() + kChunkHeaderSize, payloadLen)) {
        fail("connection lost");
        break;
      }
    } else {
      const uint32_t packetLen = readBE32(first4);
      if (packetLen < kChunkHeaderSize || packetLen > kChunkHeaderSize + kMaxChunkPayload) {
        fail("bad packet length");
        break;
      }
      packet.resize(packetLen);
      if (!recvAll(fd, packet.data(), packetLen)) {
        fail("connection lost");
        break;
      }
      if (std::memcmp(packet.data(), "DFCH", 4) != 0) {
        fail("bad chunk magic");
        break;
      }
    }

    const auto type = static_cast<FileChunkType>(packet[8]);
    const uint32_t dataLen = readBE32(packet.data() + 9);
    if (dataLen != packet.size() - kChunkHeaderSize) {
      fail("chunk length mismatch");
      break;
    }
    const uint8_t *data = packet.data() + kChunkHeaderSize;

    if (type == FileChunkType::Error) {
      fail("source refused: " + std::string(reinterpret_cast<const char *>(data), dataLen));
      break;
    }

    if (type == FileChunkType::Start) {
      if (started) {
        fail("duplicate start chunk");
        break;
      }
      started = true;
      const auto info = parseStartPayload(data, dataLen);
      auto relativePath = FileTransfer::sanitizeRelativePath(info.relativePath);
      if (relativePath.empty())
        relativePath = FileTransfer::sanitizeRelativePath(baseName(remotePath));
      if (relativePath.empty()) {
        fail("invalid file name");
        break;
      }
      isDir = info.isDir;
      limitToAnnounced = info.hasSize;
      announcedSize = info.size;
      deadline = Clock::now() + kTransferGrace +
                 (info.hasSize ? std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::seconds(announcedSize / kMinBytesPerSecond)
                                 )
                               : std::chrono::duration_cast<Clock::duration>(kUnknownSizeAllowance));
      finalPath = FileTransfer::pathFromUtf8(destDir) / FileTransfer::pathFromUtf8(relativePath);
      std::error_code ec;
      if (isDir) {
        fs::create_directories(finalPath, ec);
        if (ec) {
          fail("cannot create folder " + FileTransfer::pathToUtf8(finalPath));
          break;
        }
        continue;
      }
      fs::create_directories(finalPath.parent_path(), ec);
      if (keepExisting)
        finalPath = uniquePath(finalPath);
      tempPath = finalPath;
      tempPath += ".pasting";
      // Removes a planted symlink itself rather than writing through it
      fs::remove(tempPath, ec);
      out.open(tempPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        fail("cannot write " + FileTransfer::pathToUtf8(tempPath));
        break;
      }
      continue;
    }

    if (type == FileChunkType::Data) {
      if (!out.is_open()) {
        fail("data before start chunk");
        break;
      }
      written += dataLen;
      if (limitToAnnounced && written > announcedSize) {
        fail("source sent more data than announced");
        break;
      }
      out.write(reinterpret_cast<const char *>(data), dataLen);
      if (!out) {
        fail("cannot write " + FileTransfer::pathToUtf8(tempPath));
        break;
      }
      continue;
    }

    if (type == FileChunkType::End) {
      if (!started) {
        fail("end before start chunk");
        break;
      }
      if (!isDir) {
        out.close();
        if (!out) {
          fail("cannot write " + FileTransfer::pathToUtf8(tempPath));
          break;
        }
        std::error_code ec;
        fs::rename(tempPath, finalPath, ec);
        if (ec) {
          fail("cannot rename to " + FileTransfer::pathToUtf8(finalPath));
          break;
        }
        tempPath.clear();
      }
      result.success = true;
      result.localPath = FileTransfer::pathToUtf8(finalPath);
      break;
    }
  }

  closesocket(fd);

  if (result.success)
    LOG_INFO("[ClipboardTransferClient] received %s", result.localPath.c_str());
  else
    LOG_WARN("[ClipboardTransferClient] download of %s failed: %s", remotePath.c_str(), result.error.c_str());
  return result;
}

std::string ClipboardTransferClient::getTempDirectory() const
{
  // temp_directory_path() is Unicode-aware on Windows, unlike GetTempPathA
  std::error_code ec;
  auto dir = std::filesystem::temp_directory_path(ec);
  if (ec)
    dir = std::filesystem::current_path(ec);
  dir /= "deskflow_transfer";
  std::filesystem::create_directories(dir, ec);
  return FileTransfer::pathToUtf8(dir);
}
