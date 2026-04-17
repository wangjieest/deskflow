/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/Client.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "client/ServerProxy.h"
#include "common/Settings.h"
#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardMeta.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/ClipboardTransferThread.h"
#include "deskflow/IPlatformScreen.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/Screen.h"
#include "deskflow/StreamChunker.h"
#include "deskflow/ipc/CoreIpc.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"
#include "net/TCPSocketFactory.h"

#if defined(__APPLE__)
#include "platform/OSXClipboardFileConverter.h"
#include "platform/OSXPasteboardBridge.h"
#include "platform/OSXPasteboardPeeker.h"
#endif

#include <QMetaEnum>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

using namespace deskflow::client;

//
// Client
//

Client::Client(
    IEventQueue *events, const std::string &name, const NetworkAddress &address, ISocketFactory *socketFactory,
    deskflow::Screen *screen, SocketMultiplexer *socketMultiplexer
)
    : m_name(name),
      m_serverAddress(address),
      m_socketFactory(socketFactory),
      m_screen(screen),
      m_events(events),
      m_useSecureNetwork(Settings::value(Settings::Security::TlsEnabled).toBool())
{
  assert(m_socketFactory != nullptr);
  assert(m_screen != nullptr);

  // register suspend/resume event handlers
  m_events->addHandler(EventTypes::ScreenSuspend, getEventTarget(), [this](const auto &) { handleSuspend(); });
  m_events->addHandler(EventTypes::ScreenResume, getEventTarget(), [this](const auto &) { handleResume(); });

  m_pHelloBack = std::make_unique<HelloBack>(std::make_shared<HelloBack::Deps>(
      [this]() {
        sendConnectionFailedEvent("got invalid hello message from server");
        cleanupTimer();
        cleanupConnection();
      },
      [this](int major, int minor) {
        sendConnectionFailedEvent(IncompatibleClientException(major, minor).what());
        cleanupTimer();
        cleanupConnection();
      }
  ));

#if defined(__APPLE__)
  // Pre-initialize ClipboardTransferThread so it's ready when receiving
  // file clipboard meta from server (Mac is the paste destination).
  m_clipboardTransferThread = new ClipboardTransferThread();
  if (!m_clipboardTransferThread->start()) {
    LOG_WARN("[Client] failed to pre-start clipboard transfer thread");
    delete m_clipboardTransferThread;
    m_clipboardTransferThread = nullptr;
  }

  // Start listening for paste requests from the Finder Sync Extension.
  // When user right-clicks in Finder and selects "Deskflow Paste",
  // the extension posts a notification with the target directory.
  OSXPasteboardBridge::startListening([this](const std::string &targetDir) {
    LOG_INFO("[FinderPaste] paste request received, target: %s", targetDir.c_str());

    if (!m_clipboardTransferThread || !m_clipboardTransferThread->isRunning()) {
      LOG_WARN("[FinderPaste] clipboard transfer thread not available");
      return;
    }

    if (!m_clipboardTransferThread->hasPendingFilesForPaste()) {
      LOG_WARN("[FinderPaste] no pending files in transfer thread");
      return;
    }

    LOG_INFO("[FinderPaste] starting async file transfer to: %s", targetDir.c_str());

    // Snapshot pending files immediately (before clearing state)
    OSXPasteboardBridge::clearPendingFiles();
    OSXClipboardFileConverter::clearPendingFiles();

    // Run transfer in background thread so socket server returns "OK" immediately
    // (prevents mouse lag from blocking the socket server thread)
    ClipboardTransferThread *transferThread = m_clipboardTransferThread;
    std::thread([this, transferThread, targetDir]() {
      std::vector<std::string> paths =
          transferThread->requestFilesAndWait(targetDir, 300000); // 5 min timeout

      if (!paths.empty()) {
        LOG_INFO("[FinderPaste] %zu file(s) transferred to %s", paths.size(), targetDir.c_str());
        std::vector<const char *> cPaths;
        cPaths.reserve(paths.size());
        for (const auto &p : paths) cPaths.push_back(p.c_str());
        updatePasteboardWithFiles(cPaths.data(), static_cast<int>(cPaths.size()));
      } else {
        LOG_ERR("[FinderPaste] file transfer failed or timed out");
      }
    }).detach();
  });
#endif
}

Client::~Client()
{
  m_events->removeHandler(EventTypes::ScreenSuspend, getEventTarget());
  m_events->removeHandler(EventTypes::ScreenResume, getEventTarget());

  cleanupTimer();
  cleanupScreen();
  cleanupConnecting();
  cleanupConnection();
  delete m_socketFactory;

#if defined(__APPLE__)
  OSXPasteboardBridge::stopListening();
  OSXPasteboardBridge::clearPendingFiles();
#endif

  // Clean up clipboard transfer thread
  if (m_clipboardTransferThread) {
    m_clipboardTransferThread->stop();
    delete m_clipboardTransferThread;
    m_clipboardTransferThread = nullptr;
  }
}

void Client::connect(size_t addressIndex)
{
  if (m_stream != nullptr) {
    return;
  }
  if (m_suspended) {
    m_connectOnResume = true;
    return;
  }

  auto securityLevel = m_useSecureNetwork ? SecurityLevel::PeerAuth : SecurityLevel::PlainText;

  try {
    // resolve the server hostname.  do this every time we connect
    // in case we couldn't resolve the address earlier or the address
    // has changed (which can happen frequently if this is a laptop
    // being shuttled between various networks).  patch by Brent
    // Priddy.
    m_resolvedAddressesCount = m_serverAddress.resolve(addressIndex);

    // m_serverAddress will be null if the hostname address is not reolved
    if (m_serverAddress.getAddress() != nullptr) {
      // to help users troubleshoot, show server host name (issue: 60)
      LOG_DEBUG(
          "connecting to '%s': %s:%i", m_serverAddress.getHostname().c_str(),
          ARCH->addrToString(m_serverAddress.getAddress()).c_str(), m_serverAddress.getPort()
      );
      ipcSendConnectionState(deskflow::core::ConnectionState::Connecting);
    }

    // create the socket
    IDataSocket *socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()), securityLevel);
    bindNetworkInterface(socket);

    // filter socket messages, including a packetizing filter
    m_stream = new PacketStreamFilter(m_events, socket, true);

    // connect
    LOG_DEBUG1("connecting to server");
    setupConnecting();
    setupTimer();
    socket->connect(m_serverAddress);
  } catch (BaseException &e) {
    cleanupTimer();
    cleanupConnecting();
    cleanupStream();
    LOG_DEBUG1("connection failed");
    sendConnectionFailedEvent(e.what());
    return;
  }
}

void Client::disconnect(const char *msg)
{
  cleanup();

  if (msg) {
    sendConnectionFailedEvent(msg);
  } else {
    sendEvent(EventTypes::ClientDisconnected);
  }
}

void Client::refuseConnection(deskflow::core::ConnectionRefusal reason, const char *msg)
{
  const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionRefusal>();
  ipcSendToClient("connectionRefused", metaEnum.valueToKey(static_cast<int>(reason)));

  cleanup();

  if (msg) {
    auto info = new FailInfo(msg);
    info->m_retry = true;
    Event event(EventTypes::ClientConnectionRefused, getEventTarget(), info, Event::EventFlags::DontFreeData);
    m_events->addEvent(std::move(event));
  }
}

void Client::handshakeComplete()
{
  m_ready = true;
  m_screen->enable();
  sendEvent(EventTypes::ClientConnected);

  // Register file request callback for clipboard paste operations
  // This allows platform-specific clipboard implementations (e.g., Mac Promise keeper)
  // to request files when the user pastes
  FileTransfer::setFileRequestCallback(
      [this](const std::string &filePath, const std::string &relativePath, bool isDir, uint32_t batchId,
             const std::string & /*destFolder*/) -> uint32_t {
        // Note: destFolder is currently ignored; files are saved to temp directory
        // TODO: Support custom destination folder in future
        return this->requestFile(filePath, relativePath, isDir, batchId);
      }
  );
  LOG_DEBUG("file request callback registered for clipboard paste");
}

uint32_t Client::requestFile(
    const std::string &filePath, const std::string &relativePath, bool isDir, uint32_t batchTransferId
)
{
  if (m_server == nullptr) {
    LOG_ERR("cannot request file: not connected to server");
    return 0;
  }
  return m_server->requestFile(filePath, relativePath, isDir, batchTransferId);
}

bool Client::isConnected() const
{
  return (m_server != nullptr);
}

bool Client::isConnecting() const
{
  return (m_timer != nullptr);
}

NetworkAddress Client::getServerAddress() const
{
  return m_serverAddress;
}

void *Client::getEventTarget() const
{
  return m_screen->getEventTarget();
}

bool Client::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  return m_screen->getClipboard(id, clipboard);
}

void Client::getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const
{
  m_screen->getShape(x, y, w, h);
}

void Client::getCursorPos(int32_t &x, int32_t &y) const
{
  m_screen->getCursorPos(x, y);
}

void Client::enter(int32_t xAbs, int32_t yAbs, uint32_t, KeyModifierMask mask, bool)
{
  m_active = true;
  m_screen->mouseMove(xAbs, yAbs);
  m_screen->enter(mask);
}

bool Client::leave()
{
  m_active = false;

  m_screen->leave();

  if (m_enableClipboard) {
    // send clipboards that we own and that have changed
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
      if (m_ownClipboard[id]) {
        sendClipboard(id);
      }
    }
  }

  return true;
}

void Client::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // Open clipboard before checking formats (it may have been closed after unmarshall)
  clipboard->open(0);

  // Check if clipboard contains file list that needs to be transferred
  if (clipboard->has(IClipboard::Format::FileList)) {
    std::string fileListJson = clipboard->get(IClipboard::Format::FileList);
    LOG_DEBUG("received file list clipboard: %s", fileListJson.c_str());

#if defined(__APPLE__)
    // On macOS, we use the Promise mechanism:
    // - File metadata is stored in the clipboard via OSXClipboard::addFilePromise()
    // - When user pastes, the Promise keeper callback triggers file transfer
    // - This provides "pull on paste" semantics - files are transferred only when needed
    LOG_INFO("macOS: file list stored for promise-based paste (pull on paste)");
#else
    // On other platforms (Windows, Linux), use immediate transfer (push model)
    // Parse file entries from JSON and request each file
    // JSON format: [{"path":"...","name":"...","relativePath":"...","size":123,"isDir":false},...]

    // Helper lambda to unescape JSON strings
    auto unescapeJson = [](const std::string &s) -> std::string {
      std::string result;
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
          ++i;
          switch (s[i]) {
          case 'n':
            result += '\n';
            break;
          case 'r':
            result += '\r';
            break;
          case 't':
            result += '\t';
            break;
          default:
            result += s[i];
          }
        } else {
          result += s[i];
        }
      }
      return result;
    };

    // Helper lambda to extract string field from JSON
    auto extractStringField = [&unescapeJson](const std::string &json, size_t startPos, const std::string &fieldName)
        -> std::pair<std::string, bool> {
      std::string searchStr = "\"" + fieldName + "\":\"";
      size_t pos = json.find(searchStr, startPos);
      if (pos == std::string::npos || pos > json.find("}", startPos)) {
        return {"", false};
      }
      pos += searchStr.size();
      size_t endPos = pos;
      while (endPos < json.size() && json[endPos] != '"') {
        if (json[endPos] == '\\' && endPos + 1 < json.size()) {
          endPos += 2;
        } else {
          endPos++;
        }
      }
      return {unescapeJson(json.substr(pos, endPos - pos)), true};
    };

    // Helper lambda to extract bool field from JSON
    auto extractBoolField = [](const std::string &json, size_t startPos, const std::string &fieldName) -> bool {
      std::string searchStr = "\"" + fieldName + "\":";
      size_t pos = json.find(searchStr, startPos);
      if (pos == std::string::npos || pos > json.find("}", startPos)) {
        return false;
      }
      pos += searchStr.size();
      while (pos < json.size() && json[pos] == ' ') {
        pos++;
      }
      return pos < json.size() && json[pos] == 't';
    };

    // Generate a single transfer ID for this batch of files
    // This ensures all files from the same copy operation share the same directory structure
    uint32_t batchTransferId = FileTransfer::generateRequestId();
    LOG_INFO("starting batch file transfer: batchId=%u", batchTransferId);

    // Parse each file entry in the JSON array
    size_t entryStart = 0;
    while ((entryStart = fileListJson.find("{", entryStart)) != std::string::npos) {
      size_t entryEnd = fileListJson.find("}", entryStart);
      if (entryEnd == std::string::npos) {
        break;
      }

      // Extract fields from this entry
      auto [path, hasPath] = extractStringField(fileListJson, entryStart, "path");
      auto [relativePath, hasRelPath] = extractStringField(fileListJson, entryStart, "relativePath");
      bool isDir = extractBoolField(fileListJson, entryStart, "isDir");

      if (hasPath) {
        // If no relativePath, use the file name as relativePath
        if (!hasRelPath || relativePath.empty()) {
          size_t lastSlash = path.find_last_of("/\\");
          relativePath = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
        }

        LOG_INFO("requesting file transfer: path=%s, relativePath=%s, isDir=%d", path.c_str(), relativePath.c_str(), isDir);
        requestFile(path, relativePath, isDir, batchTransferId);
      }

      entryStart = entryEnd + 1;
    }
#endif
  }

  // Close clipboard before passing to screen (screen will reopen it)
  clipboard->close();

  m_screen->setClipboard(id, clipboard);
  m_ownClipboard[id] = false;
  m_sentClipboard[id] = false;
}

void Client::grabClipboard(ClipboardID id)
{
  m_screen->grabClipboard(id);
  m_ownClipboard[id] = false;
  m_sentClipboard[id] = false;
}

void Client::setClipboardDirty(ClipboardID, bool)
{
  assert(0 && "shouldn't be called");
}

void Client::keyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang)
{
  m_screen->keyDown(id, mask, button, lang);
}

void Client::keyRepeat(KeyID id, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &lang)
{
  m_screen->keyRepeat(id, mask, count, button, lang);
}

void Client::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
  m_screen->keyUp(id, mask, button);
}

void Client::mouseDown(ButtonID id)
{
  m_screen->mouseDown(id);
}

void Client::mouseUp(ButtonID id)
{
  m_screen->mouseUp(id);
}

void Client::mouseMove(int32_t x, int32_t y)
{
  m_screen->mouseMove(x, y);
}

void Client::mouseRelativeMove(int32_t dx, int32_t dy)
{
  m_screen->mouseRelativeMove(dx, dy);
}

void Client::mouseWheel(int32_t xDelta, int32_t yDelta)
{
  m_screen->mouseWheel(xDelta, yDelta);
}

void Client::screensaver(bool activate)
{
  m_screen->screensaver(activate);
}

void Client::resetOptions()
{
  m_screen->resetOptions();
}

void Client::setOptions(const OptionsList &options)
{
  for (auto index = options.begin(); index != options.end(); ++index) {
    const OptionID id = *index;
    if (id == kOptionClipboardSharing) {
      index++;
      if (index != options.end()) {
        if (!*index) {
          LOG_NOTE("clipboard sharing disabled by server");
        }
        m_enableClipboard = *index;
      }
    } else if (id == kOptionClipboardSharingSize) {
      index++;
      if (index != options.end()) {
        m_maximumClipboardSize = *index;
      }
    }
  }

  // Note: m_maximumClipboardSize == 0 now means "use P2P for all transfers", not "disable"
  if (!m_maximumClipboardSize) {
    LOG_NOTE("clipboard size threshold is 0, all supported formats will use P2P mode");
  }

  m_screen->setOptions(options);
}

std::string Client::getName() const
{
  return m_name;
}

void Client::sendClipboard(ClipboardID id)
{
  // note -- m_mutex must be locked on entry
  assert(m_screen != nullptr);
  assert(m_server != nullptr);

  // get clipboard data.  set the clipboard time to the last
  // clipboard time before getting the data from the screen
  // as the screen may detect an unchanged clipboard and
  // avoid copying the data.
  Clipboard clipboard;
  if (clipboard.open(m_timeClipboard[id])) {
    clipboard.close();
  }
  m_screen->getClipboard(id, &clipboard);

  // check time
  if (m_timeClipboard[id] == 0 || clipboard.getTime() != m_timeClipboard[id]) {
    // marshall the data
    std::string data = clipboard.marshall();
    // When m_maximumClipboardSize == 0, all supported formats use P2P (deferred mode)
    // so we don't skip based on size - the server will handle routing
    if (m_maximumClipboardSize > 0 && data.size() >= m_maximumClipboardSize * 1024) {
      LOG(
          (CLOG_NOTE "skipping clipboard transfer because the clipboard"
                     " contents exceeds the %i MB size limit set by the server",
           m_maximumClipboardSize / 1024)
      );
      return;
    }

    // save new time
    m_timeClipboard[id] = clipboard.getTime();
    // save and send data if different or not yet sent
    if (!m_sentClipboard[id] || data != m_dataClipboard[id]) {
      m_sentClipboard[id] = true;
      m_dataClipboard[id] = data;

      // Check if clipboard contains file list - start file transfer server for point-to-point transfer
      // Need to reopen clipboard since getClipboard() closes it after copying
      clipboard.open(clipboard.getTime());
      bool hasFileList = clipboard.has(IClipboard::Format::FileList);
      LOG_INFO("[Client] sendClipboard: hasFileList=%d", hasFileList);
      if (hasFileList) {
        // Start file transfer server and inject source info into FileList
        std::string fileListData = clipboard.get(IClipboard::Format::FileList);
        LOG_INFO("[Client] sendClipboard: FileList data (len=%zu): %.100s", fileListData.size(), fileListData.c_str());

        Clipboard modifiedClipboard;
        if (injectSourceInfoToClipboard(clipboard, modifiedClipboard)) {
          LOG_INFO("[Client] sendClipboard: using modified clipboard with source info");
          clipboard.close();  // Close before sending
          m_server->onClipboardChanged(id, &modifiedClipboard);
          return;
        }
        LOG_WARN("[Client] sendClipboard: injectSourceInfoToClipboard failed, using original");
      }
      clipboard.close();  // Close after checking FileList

      m_server->onClipboardChanged(id, &clipboard);
    }
  }
}

void Client::sendEvent(EventTypes type)
{
  m_events->addEvent(Event(type, getEventTarget()));
}

void Client::sendConnectionFailedEvent(const char *msg)
{
  auto *info = new FailInfo(msg);
  info->m_retry = true;
  Event event(EventTypes::ClientConnectionFailed, getEventTarget(), info, Event::EventFlags::DontFreeData);
  m_events->addEvent(std::move(event));
}

void Client::setupConnecting()
{
  assert(m_stream != nullptr);

  if (Settings::value(Settings::Security::TlsEnabled).toBool()) {
    m_events->addHandler(EventTypes::DataSocketSecureConnected, m_stream->getEventTarget(), [this](const auto &) {
      handleConnected();
    });
  } else {
    m_events->addHandler(EventTypes::DataSocketConnected, m_stream->getEventTarget(), [this](const auto &) {
      handleConnected();
    });
  }
  m_events->addHandler(EventTypes::DataSocketConnectionFailed, m_stream->getEventTarget(), [this](const auto &e) {
    handleConnectionFailed(e);
  });
}

void Client::setupConnection()
{
  assert(m_stream != nullptr);

  m_events->addHandler(EventTypes::SocketDisconnected, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
  m_events->addHandler(EventTypes::StreamInputReady, m_stream->getEventTarget(), [this](const auto &) {
    handleHello();
  });
  m_events->addHandler(EventTypes::StreamOutputError, m_stream->getEventTarget(), [this](const auto &) {
    handleOutputError();
  });
  m_events->addHandler(EventTypes::StreamInputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
  m_events->addHandler(EventTypes::StreamOutputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
}

void Client::setupScreen()
{
  assert(m_server == nullptr);

  m_ready = false;
  m_server = new ServerProxy(this, m_stream, m_events);
  m_events->addHandler(EventTypes::ScreenShapeChanged, getEventTarget(), [this](const auto &) {
    handleShapeChanged();
  });
  m_events->addHandler(EventTypes::ClipboardGrabbed, getEventTarget(), [this](const auto &e) {
    handleClipboardGrabbed(e);
  });
}

void Client::setupTimer()
{
  assert(m_timer == nullptr);
  m_timer = m_events->newOneShotTimer(2.0, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const auto &) { handleConnectTimeout(); });
}

void Client::cleanup()
{
  m_connectOnResume = false;
  cleanupTimer();
  cleanupScreen();
  cleanupConnecting();
  cleanupConnection();

  // Stop clipboard transfer thread when disconnecting
  // This prevents the thread from continuing to run when connection is lost
  if (m_clipboardTransferThread && m_clipboardTransferThread->isRunning()) {
    LOG_INFO("[Client] stopping clipboard transfer thread due to disconnect");
    // Don't wait for pending transfers - connection is already lost
    m_clipboardTransferThread->stop();
  }
}

void Client::cleanupConnecting()
{
  if (m_stream != nullptr) {
    m_events->removeHandler(EventTypes::DataSocketConnected, m_stream->getEventTarget());
    m_events->removeHandler(EventTypes::DataSocketConnectionFailed, m_stream->getEventTarget());
  }
}

void Client::cleanupConnection()
{
  if (m_stream != nullptr) {
    using enum EventTypes;
    m_events->removeHandler(StreamInputReady, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputError, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(SocketDisconnected, m_stream->getEventTarget());
    cleanupStream();
  }
}

void Client::cleanupScreen()
{
  if (m_server != nullptr) {
    if (m_ready) {
      m_screen->disable();
      m_ready = false;
    }
    m_events->removeHandler(EventTypes::ScreenShapeChanged, getEventTarget());
    m_events->removeHandler(EventTypes::ClipboardGrabbed, getEventTarget());
    delete m_server;
    m_server = nullptr;
  }
}

void Client::cleanupTimer()
{
  if (m_timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
    m_timer = nullptr;
  }
}

void Client::cleanupStream()
{
  delete m_stream;
  m_stream = nullptr;
}

void Client::handleConnected()
{
  LOG_DEBUG1("connected, waiting for hello");
  cleanupConnecting();
  setupConnection();

  // reset clipboard state
  for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
    m_ownClipboard[id] = false;
    m_sentClipboard[id] = false;
    m_timeClipboard[id] = 0;
  }
}

void Client::handleConnectionFailed(const Event &event)
{
  auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(event.getData());

  cleanupTimer();
  cleanupConnecting();
  cleanupStream();
  LOG_DEBUG1("connection failed");
  sendConnectionFailedEvent(info->m_what.c_str());
  delete info;
}

void Client::handleConnectTimeout()
{
  cleanupTimer();
  cleanupConnecting();
  cleanupConnection();
  cleanupStream();
  LOG_DEBUG1("connection timed out");
  sendConnectionFailedEvent("Timed out");
}

void Client::handleOutputError()
{
  cleanupTimer();
  cleanupScreen();
  cleanupConnection();
  LOG_WARN("error sending to server");
  sendEvent(EventTypes::ClientDisconnected);
}

void Client::handleDisconnected()
{
  LOG_WARN("[Client] handleDisconnected: connection to server lost, cleaning up");

  // Check if ClipboardTransferThread is running when disconnecting
  if (m_clipboardTransferThread && m_clipboardTransferThread->isRunning()) {
    LOG_WARN("[Client] handleDisconnected: ClipboardTransferThread was running, will be stopped in cleanup()");
  }

  cleanupTimer();
  cleanupScreen();
  cleanupConnection();
  LOG_DEBUG1("disconnected");
  sendEvent(EventTypes::ClientDisconnected);

  LOG_INFO("[Client] handleDisconnected: cleanup complete, ready for reconnection");
}

void Client::handleShapeChanged()
{
  LOG_DEBUG("resolution changed");
  m_server->onInfoChanged();
}

void Client::handleClipboardGrabbed(const Event &event)
{
  // Note: m_maximumClipboardSize == 0 means use P2P for all transfers, not "disable"
  if (!m_enableClipboard) {
    return;
  }

  const auto *info = static_cast<const IScreen::ClipboardInfo *>(event.getData());

  // grab ownership
  m_server->onGrabClipboard(info->m_id);

  // we now own the clipboard and it has not been sent to the server
  m_ownClipboard[info->m_id] = true;
  m_sentClipboard[info->m_id] = false;
  m_timeClipboard[info->m_id] = 0;

  // if we're not the active screen then send the clipboard now,
  // otherwise we'll wait until we leave.
  if (!m_active) {
    sendClipboard(info->m_id);
  }
}

void Client::handleHello()
{
  m_pHelloBack->handleHello(m_stream, m_name);

  // now connected but waiting to complete handshake
  setupScreen();
  cleanupTimer();

  // make sure we process any remaining messages later.  we won't
  // receive another event for already pending messages so we fake
  // one.
  if (m_stream->isReady()) {
    m_events->addEvent(Event(EventTypes::StreamInputReady, m_stream->getEventTarget()));
  }
}

void Client::handleSuspend()
{
  if (!m_suspended) {
    LOG_INFO("suspend");
    m_suspended = true;
    bool wasConnected = isConnected();
    disconnect(nullptr);
    m_connectOnResume = wasConnected;
  }
}

void Client::handleResume()
{
  if (m_suspended) {
    LOG_INFO("resume");
    m_suspended = false;
    if (m_connectOnResume) {
      m_connectOnResume = false;
      connect();
    }
  }
}

void Client::bindNetworkInterface(IDataSocket *socket) const
{
  try {
    if (const auto address = Settings::value(Settings::Core::Interface).toString(); !address.isEmpty()) {
      LOG_DEBUG1("bind to network interface: %s", qPrintable(address));

      NetworkAddress bindAddress(address.toStdString());
      bindAddress.resolve();

      socket->bind(bindAddress);
    }
  } catch (BaseException &e) {
    LOG_WARN("%s", e.what());
    LOG_WARN("operating system will select network interface automatically");
  }
}

void Client::startFileTransferServer(const std::string &fileListJson)
{
  if (fileListJson.empty()) {
    LOG_DEBUG("[Client] no file list data");
    return;
  }

  // Generate new session ID only if we don't have a recent one
  // This prevents creating multiple sessions for the same clipboard operation
  // (macOS often triggers clipboard updates multiple times)
  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  uint64_t lastSessionTime = m_currentFileSessionId >> 32;

  bool isNewSession = false;
  if (m_currentFileSessionId == 0 || (now - lastSessionTime) > 1) {
    // No session or session is older than 1 second - create new one
    m_currentFileSessionId = now << 32 | static_cast<uint64_t>(std::rand());
    LOG_INFO("[Client] starting clipboard transfer thread with new session (sessionId=%llu)", m_currentFileSessionId);
    isNewSession = true;
  } else {
    // Reuse existing session - likely a duplicate clipboard notification
    // No need to re-send file list to server
    LOG_INFO("[Client] reusing recent clipboard transfer session (sessionId=%llu, age=%llu sec) - skipping duplicate",
             m_currentFileSessionId, now - lastSessionTime);
    return;  // Early return - session already configured
  }

  // Create and start ClipboardTransferThread if needed
  // ClipboardTransferThread uses QThread for cross-platform compatibility
  // This fixes POSIX thread-local storage issues with SocketMultiplexer
  if (!m_clipboardTransferThread) {
    m_clipboardTransferThread = new ClipboardTransferThread();
  }

  if (!m_clipboardTransferThread->isRunning()) {
    if (!m_clipboardTransferThread->start()) {
      LOG_ERR("[Client] failed to start clipboard transfer thread");
      return;
    }
  }

  // Parse file list to set session files
  std::vector<ClipboardTransferFileInfo> files;

  // Helper to extract string field from JSON object
  auto extractString = [](const std::string &json, size_t start, size_t end, const std::string &field) -> std::string {
    std::string search = "\"" + field + "\":\"";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos || pos >= end) return "";
    pos += search.size();
    size_t endPos = pos;
    while (endPos < end && json[endPos] != '"') {
      if (json[endPos] == '\\' && endPos + 1 < end) endPos += 2;
      else endPos++;
    }
    // Unescape
    std::string result;
    for (size_t i = pos; i < endPos; ++i) {
      if (json[i] == '\\' && i + 1 < endPos) {
        ++i;
        if (json[i] == 'n') result += '\n';
        else if (json[i] == 'r') result += '\r';
        else if (json[i] == 't') result += '\t';
        else result += json[i];
      } else {
        result += json[i];
      }
    }
    return result;
  };

  auto extractUint64 = [](const std::string &json, size_t start, size_t end, const std::string &field) -> uint64_t {
    std::string search = "\"" + field + "\":";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos || pos >= end) return 0;
    pos += search.size();
    while (pos < end && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    uint64_t val = 0;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
      val = val * 10 + (json[pos] - '0');
      pos++;
    }
    return val;
  };

  auto extractBool = [](const std::string &json, size_t start, size_t end, const std::string &field) -> bool {
    std::string search = "\"" + field + "\":";
    size_t pos = json.find(search, start);
    if (pos == std::string::npos || pos >= end) return false;
    pos += search.size();
    while (pos < end && json[pos] == ' ') pos++;
    return pos < end && json[pos] == 't';
  };

  // Parse each file entry (skip __source entries)
  size_t entryStart = 0;
  while ((entryStart = fileListJson.find("{", entryStart)) != std::string::npos) {
    size_t entryEnd = fileListJson.find("}", entryStart);
    if (entryEnd == std::string::npos) break;

    // Skip __source metadata entry
    if (fileListJson.find("\"__source\"", entryStart) < entryEnd) {
      entryStart = entryEnd + 1;
      continue;
    }

    std::string path = extractString(fileListJson, entryStart, entryEnd, "path");
    std::string relativePath = extractString(fileListJson, entryStart, entryEnd, "relativePath");
    uint64_t size = extractUint64(fileListJson, entryStart, entryEnd, "size");
    bool isDir = extractBool(fileListJson, entryStart, entryEnd, "isDir");

    if (!path.empty()) {
      ClipboardTransferFileInfo info;
      info.path = path;
      info.relativePath = relativePath.empty() ? path : relativePath;
      info.size = size;
      info.isDir = isDir;
      files.push_back(std::move(info));
      LOG_DEBUG("[Client] file for transfer: path=%s, size=%llu, isDir=%d", path.c_str(), size, isDir);
    }

    entryStart = entryEnd + 1;
  }

  // Set files on the clipboard transfer thread
  m_clipboardTransferThread->setAvailableFiles(m_currentFileSessionId, files);

  LOG_INFO(
      "[Client] clipboard transfer thread ready: port=%u, address=%s, files=%zu",
      m_clipboardTransferThread->getServerPort(), m_clipboardTransferThread->getLocalAddress().c_str(), files.size()
  );
}

bool Client::injectSourceInfoToClipboard(const IClipboard &src, Clipboard &dst)
{
  // Get original file list
  std::string fileListJson = src.get(IClipboard::Format::FileList);
  if (fileListJson.empty()) {
    return false;
  }

  // Start file transfer server first
  startFileTransferServer(fileListJson);

  // Use ClipboardTransferThread for P2P file transfer
  // ClipboardTransferThread runs in QThread with proper TLS initialization
  std::string sourceAddress;
  uint16_t sourcePort = 0;

  if (m_clipboardTransferThread && m_clipboardTransferThread->isRunning()) {
    sourceAddress = m_clipboardTransferThread->getLocalAddress();
    sourcePort = m_clipboardTransferThread->getServerPort();
    LOG_DEBUG("[Client] using ClipboardTransferThread for P2P: %s:%u", sourceAddress.c_str(), sourcePort);
  } else {
    LOG_WARN("[Client] clipboard transfer thread not running, sending clipboard without source info");
    return false;
  }

  // Build source metadata JSON object
  std::ostringstream sourceJson;
  sourceJson << "{\"__source\":{";
  sourceJson << "\"address\":\"" << sourceAddress << "\",";
  sourceJson << "\"port\":" << sourcePort << ",";
  sourceJson << "\"sessionId\":" << m_currentFileSessionId;
  sourceJson << "}}";

  // Insert source metadata at the beginning of the array
  // Original: [{...}, {...}]
  // Modified: [{"__source":{...}}, {...}, {...}]
  std::string modifiedJson;
  if (fileListJson.size() > 1 && fileListJson[0] == '[') {
    modifiedJson = "[" + sourceJson.str();
    if (fileListJson.size() > 2 && fileListJson[1] != ']') {
      modifiedJson += ",";
    }
    modifiedJson += fileListJson.substr(1);
  } else {
    // Fallback: wrap in array
    modifiedJson = "[" + sourceJson.str() + "]";
  }

  LOG_INFO("[Client] injected source info into FileList: address=%s, port=%u, sessionId=%llu",
           sourceAddress.c_str(), sourcePort, m_currentFileSessionId);

  // Copy all formats from source to destination, replacing FileList
  dst.open(src.getTime());
  dst.empty();

  // Copy non-FileList formats
  if (src.has(IClipboard::Format::Text)) {
    dst.add(IClipboard::Format::Text, src.get(IClipboard::Format::Text));
  }
  if (src.has(IClipboard::Format::HTML)) {
    dst.add(IClipboard::Format::HTML, src.get(IClipboard::Format::HTML));
  }
  if (src.has(IClipboard::Format::Bitmap)) {
    dst.add(IClipboard::Format::Bitmap, src.get(IClipboard::Format::Bitmap));
  }

  // Add modified FileList
  dst.add(IClipboard::Format::FileList, modifiedJson);

  dst.close();

  return true;
}
