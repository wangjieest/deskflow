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

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr size_t kChunkHeaderSize = 13; // "DFCH" + requestId(4) + type(1) + dataLen(4)
constexpr uint32_t kMaxChunkPayload = 64 * 1024 * 1024;
constexpr int kConnectTimeoutMs = 10000;
constexpr char kResumeTag[4] = {'O', 'F', 'S', '1'};

std::atomic<uint32_t> s_nextDownloadId{1};

// Drops a source that keeps the connection open while delivering almost nothing
class StallGuard
{
public:
  StallGuard(std::chrono::milliseconds window, uint64_t minBytes) : m_window(window), m_minBytes(minBytes)
  {
  }

  bool onData(size_t n)
  {
    m_bytes += n;
    return healthy();
  }

  bool healthy()
  {
    const auto now = Clock::now();
    if (now - m_windowStart < m_window)
      return true;
    if (m_bytes < m_minBytes)
      return false;
    m_windowStart = now;
    m_bytes = 0;
    return true;
  }

private:
  std::chrono::milliseconds m_window;
  uint64_t m_minBytes;
  Clock::time_point m_windowStart = Clock::now();
  uint64_t m_bytes = 0;
};

enum class RecvResult
{
  Ok,
  Lost,
  Stalled
};

// Checks the guard after every recv, so a source dribbling single bytes is caught mid-chunk
RecvResult recvAll(SOCKET fd, void *buf, size_t n, StallGuard &guard)
{
  char *p = static_cast<char *>(buf);
  size_t got = 0;
  while (got < n) {
    auto r = recv(fd, p + got, static_cast<int>(n - got), 0);
    if (r <= 0)
      return RecvResult::Lost;
    got += static_cast<size_t>(r);
    if (!guard.onData(static_cast<size_t>(r)))
      return RecvResult::Stalled;
  }
  return RecvResult::Ok;
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

SOCKET openConnection(const std::string &sourceAddr, uint16_t port, std::string &error)
{
  addrinfo hints{};
  addrinfo *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  const auto portStr = std::to_string(port);
  if (getaddrinfo(sourceAddr.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
    error = "cannot resolve " + sourceAddr;
    return INVALID_SOCKET;
  }
  const SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd == INVALID_SOCKET) {
    freeaddrinfo(res);
    error = "cannot create socket";
    return INVALID_SOCKET;
  }
  // A recv that waits this long means the connection is dead, independent of the stall window
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
    error = "cannot connect to " + sourceAddr + ":" + portStr;
    return INVALID_SOCKET;
  }
  return fd;
}

void appendBE(std::vector<uint8_t> &out, uint64_t value, int bytes)
{
  for (int i = bytes - 1; i >= 0; --i)
    out.push_back(static_cast<uint8_t>(value >> (8 * i)));
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
  uint64_t offset = 0;
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
  if (const auto offset = obj.value(QStringLiteral("offset")); offset.isDouble() && offset.toDouble() > 0)
    info.offset = static_cast<uint64_t>(offset.toDouble());
  return info;
}

fs::path uniquePath(const fs::path &path)
{
  std::error_code ec;
  if (!fs::exists(path, ec))
    return path;
  const auto stem = path.stem().u8string();
  const auto ext = path.extension().u8string();
  for (int i = 2; i < 10000; ++i) {
    const auto n = std::to_string(i);
    auto candidate = path.parent_path() / (stem + u8" " + std::u8string(n.begin(), n.end()) + ext);
    if (!fs::exists(candidate, ec))
      return candidate;
  }
  return path;
}

// What survives between attempts of one download
struct Transfer
{
  bool started = false;
  bool isDir = false;
  std::string relativePath;
  fs::path finalPath;
  fs::path tempPath;
  bool limitToAnnounced = false;
  uint64_t announcedSize = 0;
  uint64_t written = 0; // bytes already in tempPath
};

enum class Outcome
{
  Done,
  Retry, // connection dropped or stalled; the partial file is kept for resuming
  Fatal
};

struct AttemptContext
{
  const std::string &sourceAddr;
  uint16_t port;
  uint64_t sessionId;
  const std::string &remotePath;
  const std::string &destDir;
  bool keepExisting;
  const FileDownloadOptions &options;
};

Outcome attemptDownload(const AttemptContext &ctx, uint32_t requestId, Transfer &t, std::string &error)
{
  const SOCKET fd = openConnection(ctx.sourceAddr, ctx.port, error);
  if (fd == INVALID_SOCKET)
    return t.started ? Outcome::Retry : Outcome::Fatal;

  struct SocketCloser
  {
    SOCKET fd;
    ~SocketCloser()
    {
      closesocket(fd);
    }
  } closer{fd};

  // What is on disk is the truth for where to resume, not what we think we wrote
  uint64_t resumeFrom = 0;
  if (t.started && !t.isDir) {
    std::error_code ec;
    const auto onDisk = fs::file_size(t.tempPath, ec);
    t.written = ec ? 0 : onDisk;
    resumeFrom = t.written;
  }

  // "P2P " + sessionId(8) + requestId(4) + pathLen(4) + path [+ "OFS1" + offset(8)], all big-endian.
  // Sources that predate resuming ignore the trailer and do not echo an offset.
  std::vector<uint8_t> request{'P', '2', 'P', ' '};
  appendBE(request, ctx.sessionId, 8);
  appendBE(request, requestId, 4);
  appendBE(request, ctx.remotePath.size(), 4);
  request.insert(request.end(), ctx.remotePath.begin(), ctx.remotePath.end());
  if (resumeFrom > 0) {
    request.insert(request.end(), kResumeTag, kResumeTag + sizeof(kResumeTag));
    appendBE(request, resumeFrom, 8);
  }
  if (!sendAll(fd, request.data(), request.size())) {
    error = "cannot send request";
    return Outcome::Retry;
  }

  StallGuard guard(ctx.options.stallWindow, ctx.options.minBytesPerWindow);
  std::ofstream out;
  std::vector<uint8_t> packet;
  bool startedThisAttempt = false;

  const auto receive = [&](void *buf, size_t n) {
    switch (recvAll(fd, buf, n, guard)) {
    case RecvResult::Ok:
      return true;
    case RecvResult::Lost:
      error = "connection lost";
      return false;
    case RecvResult::Stalled:
      error = "source is too slow";
      return false;
    }
    return false;
  };

  // Each chunk is "DFCH" + requestId + type + dataLen + payload, optionally preceded by a 4-byte length
  while (true) {
    if (!guard.healthy()) {
      error = "source is too slow";
      return Outcome::Retry;
    }

    uint8_t first4[4];
    if (!receive(first4, sizeof(first4)))
      return Outcome::Retry;
    if (std::memcmp(first4, "DFCH", 4) == 0) {
      uint8_t rest[kChunkHeaderSize - 4];
      if (!receive(rest, sizeof(rest)))
        return Outcome::Retry;
      const uint32_t payloadLen = readBE32(rest + 5);
      if (payloadLen > kMaxChunkPayload) {
        error = "chunk too large";
        return Outcome::Fatal;
      }
      packet.resize(kChunkHeaderSize + payloadLen);
      std::memcpy(packet.data(), first4, sizeof(first4));
      std::memcpy(packet.data() + sizeof(first4), rest, sizeof(rest));
      if (payloadLen > 0 && !receive(packet.data() + kChunkHeaderSize, payloadLen))
        return Outcome::Retry;
    } else {
      const uint32_t packetLen = readBE32(first4);
      if (packetLen < kChunkHeaderSize || packetLen > kChunkHeaderSize + kMaxChunkPayload) {
        error = "bad packet length";
        return Outcome::Fatal;
      }
      packet.resize(packetLen);
      if (!receive(packet.data(), packetLen))
        return Outcome::Retry;
      if (std::memcmp(packet.data(), "DFCH", 4) != 0) {
        error = "bad chunk magic";
        return Outcome::Fatal;
      }
    }

    const auto type = static_cast<FileChunkType>(packet[8]);
    const uint32_t dataLen = readBE32(packet.data() + 9);
    if (dataLen != packet.size() - kChunkHeaderSize) {
      error = "chunk length mismatch";
      return Outcome::Fatal;
    }
    const uint8_t *data = packet.data() + kChunkHeaderSize;

    if (type == FileChunkType::Error) {
      error = "source refused: " + std::string(reinterpret_cast<const char *>(data), dataLen);
      return Outcome::Fatal;
    }

    if (type == FileChunkType::Start) {
      if (startedThisAttempt) {
        error = "duplicate start chunk";
        return Outcome::Fatal;
      }
      startedThisAttempt = true;
      const auto info = parseStartPayload(data, dataLen);
      auto relativePath = FileTransfer::sanitizeRelativePath(info.relativePath);
      if (relativePath.empty())
        relativePath = FileTransfer::sanitizeRelativePath(baseName(ctx.remotePath));
      if (relativePath.empty()) {
        error = "invalid file name";
        return Outcome::Fatal;
      }

      std::error_code ec;
      if (!t.started) {
        t.started = true;
        t.isDir = info.isDir;
        t.relativePath = relativePath;
        t.limitToAnnounced = info.hasSize;
        t.announcedSize = info.size;
        t.finalPath = FileTransfer::pathFromUtf8(ctx.destDir) / FileTransfer::pathFromUtf8(relativePath);
        if (t.isDir) {
          fs::create_directories(t.finalPath, ec);
          if (ec) {
            error = "cannot create folder " + FileTransfer::pathToUtf8(t.finalPath);
            return Outcome::Fatal;
          }
          continue;
        }
        fs::create_directories(t.finalPath.parent_path(), ec);
        if (ctx.keepExisting)
          t.finalPath = uniquePath(t.finalPath);
        t.tempPath = t.finalPath;
        t.tempPath += ".pasting";
        // Removes a planted symlink itself rather than writing through it
        fs::remove(t.tempPath, ec);
      } else if (relativePath != t.relativePath || info.isDir != t.isDir || info.size != t.announcedSize) {
        error = "source file changed while resuming";
        return Outcome::Fatal;
      }

      if (t.isDir)
        continue;
      const bool resumed = resumeFrom > 0 && info.offset == resumeFrom;
      if (!resumed)
        t.written = 0;
      out.open(t.tempPath, std::ios::binary | (resumed ? std::ios::app : std::ios::trunc));
      if (!out) {
        error = "cannot write " + FileTransfer::pathToUtf8(t.tempPath);
        return Outcome::Fatal;
      }
      if (resumed)
        LOG_INFO("[ClipboardTransferClient] resuming %s at byte %llu", t.relativePath.c_str(), resumeFrom);
      continue;
    }

    if (type == FileChunkType::Data) {
      if (!out.is_open()) {
        error = "data before start chunk";
        return Outcome::Fatal;
      }
      if (t.limitToAnnounced && t.written + dataLen > t.announcedSize) {
        error = "source sent more data than announced";
        return Outcome::Fatal;
      }
      out.write(reinterpret_cast<const char *>(data), dataLen);
      if (!out) {
        error = "cannot write " + FileTransfer::pathToUtf8(t.tempPath);
        return Outcome::Fatal;
      }
      t.written += dataLen;
      continue;
    }

    if (type == FileChunkType::End) {
      if (!startedThisAttempt) {
        error = "end before start chunk";
        return Outcome::Fatal;
      }
      if (t.isDir)
        return Outcome::Done;
      out.close();
      if (!out) {
        error = "cannot write " + FileTransfer::pathToUtf8(t.tempPath);
        return Outcome::Fatal;
      }
      std::error_code ec;
      fs::rename(t.tempPath, t.finalPath, ec);
      if (ec) {
        error = "cannot rename to " + FileTransfer::pathToUtf8(t.finalPath);
        return Outcome::Fatal;
      }
      t.tempPath.clear();
      return Outcome::Done;
    }
  }
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
    const std::string &destDir, bool keepExisting, const DownloadOptions &options
)
{
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

  const AttemptContext ctx{sourceAddr, port, sessionId, remotePath, destDir, keepExisting, options};
  Transfer transfer;
  Outcome outcome = Outcome::Fatal;
  const int attempts = std::max(1, options.maxAttempts);
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    if (attempt > 1) {
      LOG_INFO(
          "[ClipboardTransferClient] %s: %s, retrying (%d/%d) from byte %llu", remotePath.c_str(), result.error.c_str(),
          attempt, attempts, transfer.written
      );
      std::this_thread::sleep_for(options.retryDelay * (attempt - 1));
    }
    outcome = attemptDownload(ctx, requestId, transfer, result.error);
    if (outcome != Outcome::Retry)
      break;
  }

  if (outcome == Outcome::Done) {
    result.success = true;
    result.error.clear();
    result.localPath = FileTransfer::pathToUtf8(transfer.finalPath);
    LOG_INFO("[ClipboardTransferClient] received %s", result.localPath.c_str());
    return result;
  }

  if (!transfer.tempPath.empty()) {
    std::error_code ec;
    fs::remove(transfer.tempPath, ec);
  }
  LOG_WARN("[ClipboardTransferClient] download of %s failed: %s", remotePath.c_str(), result.error.c_str());
  return result;
}

std::string ClipboardTransferClient::getTempDirectory() const
{
  // temp_directory_path() is Unicode-aware on Windows, unlike GetTempPathA
  std::error_code ec;
  auto dir = fs::temp_directory_path(ec);
  if (ec)
    dir = fs::current_path(ec);
  dir /= "deskflow_transfer";
  fs::create_directories(dir, ec);
  return FileTransfer::pathToUtf8(dir);
}
