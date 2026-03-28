/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/ServerProxy.h"
#include "client/FileTransferConnection.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "client/Client.h"
#include "net/SocketMultiplexer.h"
#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/OptionTypes.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "deskflow/ipc/CoreIpc.h"
#include "io/IStream.h"

#ifdef _WIN32
#include "deskflow/ClipboardTransferThread.h"
#include "platform/MSWindowsClipboardFileConverter.h"
#endif

#if WINAPI_CARBON
#include "deskflow/ClipboardTransferThread.h"
#include "platform/OSXClipboardFileConverter.h"
#include "platform/OSXPasteboardBridge.h"
#endif

#include <cstring>
#include <fstream>
#include <sstream>

//
// ServerProxy
//

ServerProxy::ServerProxy(Client *client, deskflow::IStream *stream, IEventQueue *events, SocketMultiplexer *socketMultiplexer)
    : m_client(client),
      m_stream(stream),
      m_events(events),
      m_socketMultiplexer(socketMultiplexer)
{
  assert(m_client != nullptr);
  assert(m_stream != nullptr);

  // initialize modifier translation table
  for (KeyModifierID id = 0; id < kKeyModifierIDLast; ++id)
    m_modifierTranslationTable[id] = id;

  // handle data on stream
  m_events->addHandler(EventTypes::StreamInputReady, m_stream->getEventTarget(), [this](const auto &) {
    handleData();
  });
  m_events->addHandler(EventTypes::ClipboardSending, this, [this](const auto &e) {
    ClipboardChunk::send(m_stream, e.getDataObject());
  });

  // send heartbeat
  setKeepAliveRate(kKeepAliveRate);
}

ServerProxy::~ServerProxy()
{
  setKeepAliveRate(-1.0);
  m_events->removeHandler(EventTypes::StreamInputReady, m_stream->getEventTarget());

  // Clean up file transfer connection
  if (m_fileTransferConn) {
    m_fileTransferConn->close();
    delete m_fileTransferConn;
    m_fileTransferConn = nullptr;
  }
}

void ServerProxy::resetKeepAliveAlarm()
{
  if (m_keepAliveAlarmTimer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_keepAliveAlarmTimer);
    m_events->deleteTimer(m_keepAliveAlarmTimer);
    m_keepAliveAlarmTimer = nullptr;
  }
  if (m_keepAliveAlarm > 0.0) {
    m_keepAliveAlarmTimer = m_events->newOneShotTimer(m_keepAliveAlarm, nullptr);
    m_events->addHandler(EventTypes::Timer, m_keepAliveAlarmTimer, [this](const auto &) { handleKeepAliveAlarm(); });
  }
}

void ServerProxy::setKeepAliveRate(double rate)
{
  m_keepAliveAlarm = rate * kKeepAlivesUntilDeath;
  resetKeepAliveAlarm();
}

void ServerProxy::handleData()
{
  // handle messages until there are no more.  first read message code.
  uint8_t code[4];
  uint32_t n = m_stream->read(code, 4);
  while (n != 0) {
    // verify we got an entire code
    if (n != 4) {
      LOG_ERR("incomplete message from server: %d bytes", n);
      m_client->disconnect("incomplete message from server");
      return;
    }

    // parse message
    LOG_DEBUG2("msg from server: %c%c%c%c", code[0], code[1], code[2], code[3]);
    try {
      switch ((this->*m_parser)(code)) {
        using enum ConnectionResult;
      case Okay:
        break;

      case Unknown:
        LOG_ERR("invalid message from server: %c%c%c%c", code[0], code[1], code[2], code[3]);
        // not possible to determine message boundaries
        // read the whole stream to discard unkonwn data
        while (m_stream->read(nullptr, 4))
          ;
        break;

      case Disconnect:
        return;
      }
    } catch (const BadClientException &e) {
      LOG_ERR("protocol error from server: %s", e.what());
      ProtocolUtil::writef(m_stream, kMsgEBad);
      m_client->disconnect("invalid message from server");
      return;
    }

    // next message
    n = m_stream->read(code, 4);
  }

  flushCompressedMouse();
}

ServerProxy::ConnectionResult ServerProxy::parseHandshakeMessage(const uint8_t *code)
{
  using enum ConnectionResult;
  using enum deskflow::core::ConnectionRefusal;

  if (memcmp(code, kMsgQInfo, 4) == 0) {
    queryInfo();
  }

  else if (memcmp(code, kMsgCInfoAck, 4) == 0) {
    infoAcknowledgment();
  }

  else if (memcmp(code, kMsgDSetOptions, 4) == 0) {
    setOptions();

    // handshake is complete
    m_parser = &ServerProxy::parseMessage;

    if (const auto missedKeyboardLayouts = m_layoutManager.getMissedLayouts(); !missedKeyboardLayouts.empty()) {
      LOG_WARN("server layouts missing on this computer: %s", missedKeyboardLayouts.c_str());
      ipcSendToClient("missingKeyboardLayouts", QString::fromStdString(missedKeyboardLayouts));
    }

    m_client->handshakeComplete();
  }

  else if (memcmp(code, kMsgCResetOptions, 4) == 0) {
    resetOptions();
  }

  else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
    // echo keep alives and reset alarm
    ProtocolUtil::writef(m_stream, kMsgCKeepAlive);
    resetKeepAliveAlarm();
  }

  else if (memcmp(code, kMsgCNoop, 4) == 0) {
    // accept and discard no-op
  }

  else if (memcmp(code, kMsgCClose, 4) == 0) {
    // server wants us to hangup
    LOG_DEBUG1("recv close");
    m_client->disconnect(nullptr);
    return Disconnect;
  }

  else if (memcmp(code, kMsgEIncompatible, 4) == 0) {
    int32_t major;
    int32_t minor;
    ProtocolUtil::readf(m_stream, kMsgEIncompatible + 4, &major, &minor);
    LOG_ERR("server has incompatible version %d.%d", major, minor);
    m_client->refuseConnection(IncompatibleVersion, "server has incompatible version");
    return Disconnect;
  }

  else if (memcmp(code, kMsgEBusy, 4) == 0) {
    LOG_ERR("server already has a connected client with name \"%s\"", m_client->getName().c_str());
    m_client->refuseConnection(AlreadyConnected, "server already has a connected client with our name");
    return Disconnect;
  }

  else if (memcmp(code, kMsgEUnknown, 4) == 0) {
    LOG_ERR("server refused client with name \"%s\"", m_client->getName().c_str());
    m_client->refuseConnection(UnknownClient, "server refused client with our name");
    return Disconnect;
  }

  else if (memcmp(code, kMsgEBad, 4) == 0) {
    LOG_ERR("server disconnected due to a protocol error");
    m_client->refuseConnection(ProtocolError, "server reported a protocol error");
    return Disconnect;
  } else if (memcmp(code, kMsgDLanguageSynchronisation, 4) == 0) {
    setServerLanguages();
  } else {
    return Unknown;
  }

  return Okay;
}

ServerProxy::ConnectionResult ServerProxy::parseMessage(const uint8_t *code)
{
  using enum ConnectionResult;

  if (memcmp(code, kMsgDMouseMove, 4) == 0) {
    mouseMove();
  }

  else if (memcmp(code, kMsgDMouseRelMove, 4) == 0) {
    mouseRelativeMove();
  }

  else if (memcmp(code, kMsgDMouseWheel, 4) == 0) {
    mouseWheel();
  }

  else if (memcmp(code, kMsgDKeyDown, 4) == 0) {
    uint16_t id = 0;
    uint16_t mask = 0;
    uint16_t button = 0;
    ProtocolUtil::readf(m_stream, kMsgDKeyDown + 4, &id, &mask, &button);
    LOG_DEBUG1("recv key down id=0x%08x, mask=0x%04x, button=0x%04x", id, mask, button);

    keyDown(id, mask, button, "");
  }

  else if (memcmp(code, kMsgDKeyDownLang, 4) == 0) {
    std::string lang;
    uint16_t id = 0;
    uint16_t mask = 0;
    uint16_t button = 0;

    ProtocolUtil::readf(m_stream, kMsgDKeyDownLang + 4, &id, &mask, &button, &lang);
    LOG_DEBUG1("recv key down id=0x%08x, mask=0x%04x, button=0x%04x, lang=\"%s\"", id, mask, button, lang.c_str());

    keyDown(id, mask, button, lang);
  }

  else if (memcmp(code, kMsgDKeyUp, 4) == 0) {
    keyUp();
  }

  else if (memcmp(code, kMsgDMouseDown, 4) == 0) {
    mouseDown();
  }

  else if (memcmp(code, kMsgDMouseUp, 4) == 0) {
    mouseUp();
  }

  else if (memcmp(code, kMsgDKeyRepeat, 4) == 0) {
    keyRepeat();
  }

  else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
    // echo keep alives and reset alarm
    ProtocolUtil::writef(m_stream, kMsgCKeepAlive);
    resetKeepAliveAlarm();
  }

  else if (memcmp(code, kMsgCNoop, 4) == 0) {
    // accept and discard no-op
  }

  else if (memcmp(code, kMsgCEnter, 4) == 0) {
    enter();
  }

  else if (memcmp(code, kMsgCLeave, 4) == 0) {
    leave();
  }

  else if (memcmp(code, kMsgCClipboard, 4) == 0) {
    grabClipboard();
  }

  else if (memcmp(code, kMsgCScreenSaver, 4) == 0) {
    screensaver();
  }

  else if (memcmp(code, kMsgQInfo, 4) == 0) {
    queryInfo();
  }

  else if (memcmp(code, kMsgCInfoAck, 4) == 0) {
    infoAcknowledgment();
  }

  else if (memcmp(code, kMsgDClipboard, 4) == 0) {
    setClipboard();
  }

  else if (memcmp(code, kMsgCResetOptions, 4) == 0) {
    resetOptions();
  }

  else if (memcmp(code, kMsgDSetOptions, 4) == 0) {
    setOptions();
  }

  else if (memcmp(code, kMsgDSecureInputNotification, 4) == 0) {
    secureInputNotification();
  }

  else if (memcmp(code, kMsgDFileChunk, 4) == 0) {
    fileChunkReceived();
  }

  else if (memcmp(code, kMsgDFileTransferPort, 4) == 0) {
    handleFileTransferPort();
  }

  else if (memcmp(code, kMsgDClipboardMeta, 4) == 0) {
    setClipboardMeta();
  }

  else if (memcmp(code, kMsgSFileRequest, 4) == 0) {
    handleServerFileRequest();
  }

  else if (memcmp(code, kMsgCClose, 4) == 0) {
    // server wants us to hangup
    LOG_DEBUG1("recv close");
    m_client->disconnect(nullptr);
    return Disconnect;
  } else if (memcmp(code, kMsgEBad, 4) == 0) {
    LOG_ERR("server disconnected due to a protocol error");
    m_client->disconnect("server reported a protocol error");
    return Disconnect;
  } else {
    return Unknown;
  }

  // send a reply.  this is intended to work around a delay when
  // running a linux server and an OS X (any BSD?) client.  the
  // client waits to send an ACK (if the system control flag
  // net.inet.tcp.delayed_ack is 1) in hopes of piggybacking it
  // on a data packet.  we provide that packet here.  i don't
  // know why a delayed ACK should cause the server to wait since
  // TCP_NODELAY is enabled.
  ProtocolUtil::writef(m_stream, kMsgCNoop);

  return Okay;
}

void ServerProxy::handleKeepAliveAlarm()
{
  LOG_NOTE("server is dead");
  m_client->disconnect("server is not responding");
}

void ServerProxy::onInfoChanged()
{
  // ignore mouse motion until we receive acknowledgment of our info
  // change message.
  m_ignoreMouse = true;

  // send info update
  queryInfo();
}

bool ServerProxy::onGrabClipboard(ClipboardID id)
{
  LOG_DEBUG1("sending clipboard %d changed", id);
  ProtocolUtil::writef(m_stream, kMsgCClipboard, id, m_seqNum);
  return true;
}

void ServerProxy::onClipboardChanged(ClipboardID id, const IClipboard *clipboard)
{
  std::string data = IClipboard::marshall(clipboard);
  LOG_DEBUG("sending clipboard %d seqnum=%d", id, m_seqNum);

  StreamChunker::sendClipboard(data, data.size(), id, m_seqNum, m_events, this);
}

void ServerProxy::flushCompressedMouse()
{
  if (m_compressMouse) {
    m_compressMouse = false;
    m_client->mouseMove(m_xMouse, m_yMouse);
  }
  if (m_compressMouseRelative) {
    m_compressMouseRelative = false;
    m_client->mouseRelativeMove(m_dxMouse, m_dyMouse);
    m_dxMouse = 0;
    m_dyMouse = 0;
  }
}

void ServerProxy::sendInfo(const ClientInfo &info)
{
  LOG_DEBUG1("sending info shape=%d,%d %dx%d", info.m_x, info.m_y, info.m_w, info.m_h);
  ProtocolUtil::writef(m_stream, kMsgDInfo, info.m_x, info.m_y, info.m_w, info.m_h, 0, info.m_mx, info.m_my);
}

KeyID ServerProxy::translateKey(KeyID id) const
{
  static const KeyID s_translationTable[kKeyModifierIDLast][2] = {
      {kKeyNone, kKeyNone},     {kKeyShift_L, kKeyShift_R}, {kKeyControl_L, kKeyControl_R}, {kKeyAlt_L, kKeyAlt_R},
      {kKeyMeta_L, kKeyMeta_R}, {kKeySuper_L, kKeySuper_R}, {kKeyAltGr, kKeyAltGr}
  };

  KeyModifierID id2 = kKeyModifierIDNull;
  uint32_t side = 0;
  switch (id) {
  case kKeyShift_L:
    id2 = kKeyModifierIDShift;
    side = 0;
    break;

  case kKeyShift_R:
    id2 = kKeyModifierIDShift;
    side = 1;
    break;

  case kKeyControl_L:
    id2 = kKeyModifierIDControl;
    side = 0;
    break;

  case kKeyControl_R:
    id2 = kKeyModifierIDControl;
    side = 1;
    break;

  case kKeyAlt_L:
    id2 = kKeyModifierIDAlt;
    side = 0;
    break;

  case kKeyAlt_R:
    id2 = kKeyModifierIDAlt;
    side = 1;
    break;

  case kKeyAltGr:
    id2 = kKeyModifierIDAltGr;
    side = 1; // there is only one alt gr key on the right side
    break;

  case kKeyMeta_L:
    id2 = kKeyModifierIDMeta;
    side = 0;
    break;

  case kKeyMeta_R:
    id2 = kKeyModifierIDMeta;
    side = 1;
    break;

  case kKeySuper_L:
    id2 = kKeyModifierIDSuper;
    side = 0;
    break;

  case kKeySuper_R:
    id2 = kKeyModifierIDSuper;
    side = 1;
    break;

  default:
    break;
  }

  if (id2 != kKeyModifierIDNull) {
    return s_translationTable[m_modifierTranslationTable[id2]][side];
  } else {
    return id;
  }
}

KeyModifierMask ServerProxy::translateModifierMask(KeyModifierMask mask) const
{
  static const KeyModifierMask s_masks[kKeyModifierIDLast] = {0x0000,          KeyModifierShift, KeyModifierControl,
                                                              KeyModifierAlt,  KeyModifierMeta,  KeyModifierSuper,
                                                              KeyModifierAltGr};

  KeyModifierMask newMask = mask & ~(KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierMeta |
                                     KeyModifierSuper | KeyModifierAltGr);
  if ((mask & KeyModifierShift) != 0) {
    newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDShift]];
  }
  if ((mask & KeyModifierControl) != 0) {
    newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDControl]];
  }
  if ((mask & KeyModifierAlt) != 0) {
    newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDAlt]];
  }
  if ((mask & KeyModifierAltGr) != 0) {
    newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDAltGr]];
  }
  if ((mask & KeyModifierMeta) != 0) {
    newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDMeta]];
  }
  if ((mask & KeyModifierSuper) != 0) {
    newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDSuper]];
  }
  return newMask;
}

void ServerProxy::enter()
{
  // parse
  int16_t x;
  int16_t y;
  uint16_t mask;
  uint32_t seqNum;
  ProtocolUtil::readf(m_stream, kMsgCEnter + 4, &x, &y, &seqNum, &mask);
  LOG_DEBUG1("recv enter, %d,%d %d %04x", x, y, seqNum, mask);
  LOG_INFO("[ServerProxy] entering screen at (%d,%d) seq=%d - mouse from server", x, y, seqNum);

  // discard old compressed mouse motion, if any
  m_compressMouse = false;
  m_compressMouseRelative = false;
  m_dxMouse = 0;
  m_dyMouse = 0;
  m_seqNum = seqNum;
  m_serverLayout = "";
  m_isUserNotifiedAboutLayoutSyncError = false;

  // forward
  m_client->enter(x, y, seqNum, static_cast<KeyModifierMask>(mask), false);

  LOG_DEBUG("[ServerProxy] enter complete, client screen active");
}

void ServerProxy::leave()
{
  // parse
  LOG_DEBUG1("recv leave");
  LOG_INFO("[ServerProxy] leaving screen - mouse moving back to server");

  // send last mouse motion
  flushCompressedMouse();

  // forward
  m_client->leave();

  LOG_DEBUG("[ServerProxy] leave complete");
}

void ServerProxy::setClipboard()
{
  // parse
  static std::string dataCached;
  ClipboardID id;
  uint32_t seq;

  auto r = ClipboardChunk::assemble(m_stream, dataCached, id, seq);

  if (r == TransferState::Started) {
    size_t size = ClipboardChunk::getExpectedSize();
    LOG_DEBUG("receiving clipboard %d size=%d", id, size);

    // Mark as transferring if this was a deferred clipboard
    if (id < kClipboardEnd && m_deferredClipboard[id].isActive()) {
      m_deferredClipboard[id].isTransferring = true;
    }
  } else if (r == TransferState::Finished) {
    LOG_DEBUG("received clipboard %d size=%d", id, dataCached.size());

    // Check if this deferred clipboard should be auto-deleted after transfer
    bool shouldClear = true;
    if (id < kClipboardEnd && m_deferredClipboard[id].deleteOnComplete) {
      LOG_DEBUG("clearing deferred clipboard %d after completed transfer", id);
      shouldClear = true;
    }

    // Clear deferred state since we're receiving full data
    if (shouldClear) {
      clearDeferredClipboard(id);
    }

    // forward
    Clipboard clipboard;
    clipboard.unmarshall(dataCached, 0);
    m_client->setClipboard(id, &clipboard);

    LOG_INFO("clipboard was updated");
  }
}

void ServerProxy::setClipboardMeta()
{
  // Parse message: kMsgDClipboardMeta = "DCMT%1i%s"
  // %1i = clipboard ID (1 byte)
  // %s = JSON metadata string
  ClipboardID id;
  std::string metaJson;

  if (!ProtocolUtil::readf(m_stream, kMsgDClipboardMeta + 4, &id, &metaJson)) {
    LOG_ERR("failed to parse clipboard metadata message");
    return;
  }

  // Validate clipboard ID
  if (id >= kClipboardEnd) {
    LOG_ERR("invalid clipboard ID in metadata: %d", id);
    return;
  }

  // Parse metadata
  ClipboardMeta meta = ClipboardMeta::deserialize(metaJson);

  LOG_INFO(
      "received clipboard %d metadata (size=%llu, deferred=%s, sessionId=%llu)", id, meta.totalSize,
      meta.deferred ? "true" : "false", meta.sessionId
  );

  // New clipboard content arrived - cleanup old deferred clipboards
  cleanupDeferredClipboards(id);

  // Store new deferred metadata
  m_deferredClipboard[id].meta = meta;
  m_deferredClipboard[id].isTransferring = false;
  m_deferredClipboard[id].deleteOnComplete = false;

  if (meta.deferred) {
    // For deferred mode, we'll request data when user pastes
    // For now, notify client that clipboard is available but in deferred mode
    LOG_DEBUG("clipboard %d is in deferred mode - will request data on paste", id);

#ifdef _WIN32
    // On Windows, set up ClipboardTransferThread for point-to-point transfer
    // This moves WM_RENDERFORMAT handling to a dedicated thread, preventing main thread blocking
    if (meta.contentType == static_cast<uint32_t>(IClipboard::Format::FileList) &&
        !meta.sourceAddress.empty() && meta.sourcePort > 0) {
      LOG_INFO(
          "[ServerProxy] Windows: setting up point-to-point file transfer from %s:%u",
          meta.sourceAddress.c_str(), meta.sourcePort
      );

      // Get ClipboardTransferThread from client
      ClipboardTransferThread *transferThread = m_client->getClipboardTransferThread();
      if (transferThread && transferThread->isRunning()) {
        // Parse file list from metadata
        std::vector<ClipboardTransferFileInfo> files;
        auto parsedFiles = MSWindowsClipboardFileConverter::parseFileList(meta.metadata);
        files.reserve(parsedFiles.size());
        for (const auto &f : parsedFiles) {
          ClipboardTransferFileInfo info;
          info.path = f.path;
          info.relativePath = f.relativePath.empty() ? f.name : f.relativePath;
          info.size = f.size;
          info.isDir = f.isDir;
          files.push_back(std::move(info));
        }

        // Use setDelayedRenderingFiles to set up delayed rendering in the transfer thread
        // This makes the ClipboardTransferThread's window the clipboard owner,
        // so WM_RENDERFORMAT will be sent to that thread instead of the main thread
        transferThread->setDelayedRenderingFiles(
            files, meta.sourceAddress, meta.sourcePort, meta.sessionId
        );

        LOG_INFO(
            "[ServerProxy] Windows: ClipboardTransferThread configured with %zu files from %s:%u (non-blocking)",
            files.size(), meta.sourceAddress.c_str(), meta.sourcePort
        );
      } else {
        LOG_WARN("[ServerProxy] ClipboardTransferThread not available, will use legacy (blocking) transfer");
        // Fall back to legacy behavior - store pending files for MSWindowsScreen to handle
        MSWindowsClipboardFileConverter::setPendingFiles(
            MSWindowsClipboardFileConverter::parseFileList(meta.metadata)
        );
        MSWindowsClipboardFileConverter::setDelayedRenderingActive(true);
      }
    }
#endif

#if WINAPI_CARBON
    // On macOS, set up ClipboardTransferThread for point-to-point transfer
    if (meta.contentType == static_cast<uint32_t>(IClipboard::Format::FileList) &&
        !meta.sourceAddress.empty() && meta.sourcePort > 0) {
      LOG_INFO(
          "[ServerProxy] setting up point-to-point file transfer from %s:%u",
          meta.sourceAddress.c_str(), meta.sourcePort
      );

      // Get ClipboardTransferThread from client
      ClipboardTransferThread *transferThread = m_client->getClipboardTransferThread();
      if (transferThread && transferThread->isRunning()) {
        // Parse file list from metadata
        std::vector<ClipboardTransferFileInfo> files;
        auto parsedFiles = OSXClipboardFileConverter::parseFileList(meta.metadata);
        files.reserve(parsedFiles.size());
        for (const auto &f : parsedFiles) {
          ClipboardTransferFileInfo info;
          info.path = f.path;
          info.relativePath = f.relativePath.empty() ? f.name : f.relativePath;
          info.size = f.size;
          info.isDir = f.isDir;
          files.push_back(std::move(info));
        }

        // Set pending files for paste
        transferThread->setPendingFilesForPaste(
            files, meta.sourceAddress, meta.sourcePort, meta.sessionId
        );

        // Also set the transfer thread in OSXClipboardFileConverter for static access
        OSXClipboardFileConverter::setClipboardTransferThread(transferThread);

        LOG_INFO(
            "[ServerProxy] ClipboardTransferThread configured with %zu files from %s:%u",
            files.size(), meta.sourceAddress.c_str(), meta.sourcePort
        );

        // Publish pending files to Finder Sync Extension via shared state file
        OSXPasteboardBridge::publishPendingFiles(meta.metadata, static_cast<int>(parsedFiles.size()));
      } else {
        LOG_WARN("[ServerProxy] ClipboardTransferThread not available, will use legacy transfer");
      }
    }
#endif
  }
}

void ServerProxy::requestClipboardData(ClipboardID id)
{
  // Validate
  if (id >= kClipboardEnd) {
    LOG_ERR("invalid clipboard ID for data request: %d", id);
    return;
  }

  DeferredClipboardState &state = m_deferredClipboard[id];
  if (!state.isActive()) {
    LOG_DEBUG("no deferred clipboard data to request for id=%d", id);
    return;
  }

  // Build data request
  ClipboardDataRequest request;
  request.sessionId = state.meta.sessionId;
  request.contentType = state.meta.contentType;

  std::string requestJson = request.serialize();

  LOG_INFO("requesting clipboard %d data (sessionId=%llu)", id, state.meta.sessionId);

  // Mark as transferring
  state.isTransferring = true;

  // Send request to server
  ProtocolUtil::writef(m_stream, kMsgQClipboardData, id, &requestJson);
}

void ServerProxy::clearDeferredClipboard(ClipboardID id)
{
  if (id >= kClipboardEnd) {
    return;
  }

  if (m_deferredClipboard[id].isActive()) {
    LOG_DEBUG("clearing deferred clipboard %d (sessionId=%llu)", id, m_deferredClipboard[id].meta.sessionId);
  }

  m_deferredClipboard[id].clear();
}

void ServerProxy::cleanupDeferredClipboards(ClipboardID newClipboardId)
{
  // When new clipboard content arrives, we need to:
  // 1. Clear all non-transferring deferred clipboards (they're now stale)
  // 2. Mark transferring ones for auto-delete when transfer completes

  for (ClipboardID i = 0; i < kClipboardEnd; ++i) {
    DeferredClipboardState &state = m_deferredClipboard[i];

    if (!state.isActive()) {
      continue;
    }

    // Skip the clipboard that's being updated (will be replaced with new content)
    if (i == newClipboardId) {
      continue;
    }

    if (state.isTransferring) {
      // Transfer in progress - mark for auto-delete when complete
      LOG_DEBUG(
          "marking deferred clipboard %d for auto-delete (sessionId=%llu, transfer in progress)", i,
          state.meta.sessionId
      );
      state.deleteOnComplete = true;
    } else {
      // Not transferring - can safely clear now
      LOG_DEBUG("clearing stale deferred clipboard %d (sessionId=%llu)", i, state.meta.sessionId);
      state.clear();
    }
  }
}

void ServerProxy::grabClipboard()
{
  // parse
  ClipboardID id;
  uint32_t seqNum;
  ProtocolUtil::readf(m_stream, kMsgCClipboard + 4, &id, &seqNum);
  LOG_DEBUG("recv grab clipboard %d", id);

  // validate
  if (id >= kClipboardEnd) {
    return;
  }

  // New clipboard content is coming - cleanup old deferred state
  cleanupDeferredClipboards(id);

  // forward
  m_client->grabClipboard(id);
}

void ServerProxy::keyDown(uint16_t id, uint16_t mask, uint16_t button, const std::string &lang)
{
  // get mouse up to date
  flushCompressedMouse();
  setActiveServerLanguage(lang);

  // translate
  KeyID id2 = translateKey(static_cast<KeyID>(id));
  KeyModifierMask mask2 = translateModifierMask(static_cast<KeyModifierMask>(mask));
  if (id2 != static_cast<KeyID>(id) || mask2 != static_cast<KeyModifierMask>(mask))
    LOG_DEBUG1("key down translated to id=0x%08x, mask=0x%04x", id2, mask2);

  // forward
  m_client->keyDown(id2, mask2, button, lang);
}

void ServerProxy::keyRepeat()
{
  // get mouse up to date
  flushCompressedMouse();

  // parse
  uint16_t id;
  uint16_t mask;
  uint16_t count;
  uint16_t button;
  std::string lang;
  ProtocolUtil::readf(m_stream, kMsgDKeyRepeat + 4, &id, &mask, &count, &button, &lang);
  LOG(
      (CLOG_DEBUG1 "recv key repeat id=0x%08x, mask=0x%04x, count=%d, "
                   "button=0x%04x, lang=\"%s\"",
       id, mask, count, button, lang.c_str())
  );

  // translate
  KeyID id2 = translateKey(static_cast<KeyID>(id));
  KeyModifierMask mask2 = translateModifierMask(static_cast<KeyModifierMask>(mask));
  if (id2 != static_cast<KeyID>(id) || mask2 != static_cast<KeyModifierMask>(mask))
    LOG_DEBUG1("key repeat translated to id=0x%08x, mask=0x%04x", id2, mask2);

  // forward
  m_client->keyRepeat(id2, mask2, count, button, lang);
}

void ServerProxy::keyUp()
{
  // get mouse up to date
  flushCompressedMouse();

  // parse
  uint16_t id;
  uint16_t mask;
  uint16_t button;
  ProtocolUtil::readf(m_stream, kMsgDKeyUp + 4, &id, &mask, &button);
  LOG_DEBUG1("recv key up id=0x%08x, mask=0x%04x, button=0x%04x", id, mask, button);

  // translate
  KeyID id2 = translateKey(static_cast<KeyID>(id));
  KeyModifierMask mask2 = translateModifierMask(static_cast<KeyModifierMask>(mask));
  if (id2 != static_cast<KeyID>(id) || mask2 != static_cast<KeyModifierMask>(mask))
    LOG_DEBUG1("key up translated to id=0x%08x, mask=0x%04x", id2, mask2);

  // forward
  m_client->keyUp(id2, mask2, button);
}

void ServerProxy::mouseDown()
{
  // get mouse up to date
  flushCompressedMouse();

  // parse
  int8_t id;
  ProtocolUtil::readf(m_stream, kMsgDMouseDown + 4, &id);
  LOG_DEBUG1("recv mouse down id=%d", id);

  // forward
  m_client->mouseDown(static_cast<ButtonID>(id));
}

void ServerProxy::mouseUp()
{
  // get mouse up to date
  flushCompressedMouse();

  // parse
  int8_t id;
  ProtocolUtil::readf(m_stream, kMsgDMouseUp + 4, &id);
  LOG_DEBUG1("recv mouse up id=%d", id);

  // forward
  m_client->mouseUp(static_cast<ButtonID>(id));
}

void ServerProxy::mouseMove()
{
  // parse
  bool ignore;
  int16_t x;
  int16_t y;
  ProtocolUtil::readf(m_stream, kMsgDMouseMove + 4, &x, &y);

  // note if we should ignore the move
  ignore = m_ignoreMouse;

  // compress mouse motion events if more input follows
  if (!ignore && !m_compressMouse && m_stream->isReady()) {
    m_compressMouse = true;
  }

  // if compressing then ignore the motion but record it
  if (m_compressMouse) {
    m_compressMouseRelative = false;
    ignore = true;
    m_xMouse = x;
    m_yMouse = y;
    m_dxMouse = 0;
    m_dyMouse = 0;
  }
  LOG_DEBUG2("recv mouse move %d,%d", x, y);

  // forward
  if (!ignore) {
    m_client->mouseMove(x, y);
  }
}

void ServerProxy::mouseRelativeMove()
{
  // parse
  bool ignore;
  int16_t dx;
  int16_t dy;
  ProtocolUtil::readf(m_stream, kMsgDMouseRelMove + 4, &dx, &dy);

  // note if we should ignore the move
  ignore = m_ignoreMouse;

  // compress mouse motion events if more input follows
  if (!ignore && !m_compressMouseRelative && m_stream->isReady()) {
    m_compressMouseRelative = true;
  }

  // if compressing then ignore the motion but record it
  if (m_compressMouseRelative) {
    ignore = true;
    m_dxMouse += dx;
    m_dyMouse += dy;
  }
  LOG_DEBUG2("recv mouse relative move %d,%d", dx, dy);

  // forward
  if (!ignore) {
    m_client->mouseRelativeMove(dx, dy);
  }
}

void ServerProxy::mouseWheel()
{
  // get mouse up to date
  flushCompressedMouse();

  // parse
  int16_t xDelta;
  int16_t yDelta;
  ProtocolUtil::readf(m_stream, kMsgDMouseWheel + 4, &xDelta, &yDelta);
  LOG_DEBUG2("recv mouse wheel %+d,%+d", xDelta, yDelta);

  // forward
  m_client->mouseWheel(xDelta, yDelta);
}

void ServerProxy::screensaver()
{
  // parse
  int8_t on;
  ProtocolUtil::readf(m_stream, kMsgCScreenSaver + 4, &on);
  LOG_DEBUG1("recv screen saver on=%d", on);

  // forward
  m_client->screensaver(on != 0);
}

void ServerProxy::resetOptions()
{
  // parse
  LOG_DEBUG1("recv reset options");

  // forward
  m_client->resetOptions();

  // reset keep alive
  setKeepAliveRate(kKeepAliveRate);

  // reset modifier translation table
  for (KeyModifierID id = 0; id < kKeyModifierIDLast; ++id) {
    m_modifierTranslationTable[id] = id;
  }
}

void ServerProxy::setOptions()
{
  // parse
  OptionsList options;
  ProtocolUtil::readf(m_stream, kMsgDSetOptions + 4, &options);
  LOG_DEBUG1("recv set options size=%d", options.size());

  // forward
  m_client->setOptions(options);

  // update modifier table
  for (uint32_t i = 0, n = (uint32_t)options.size(); i < n; i += 2) {
    KeyModifierID id = kKeyModifierIDNull;
    if (options[i] == kOptionModifierMapForShift) {
      id = kKeyModifierIDShift;
    } else if (options[i] == kOptionModifierMapForControl) {
      id = kKeyModifierIDControl;
    } else if (options[i] == kOptionModifierMapForAlt) {
      id = kKeyModifierIDAlt;
    } else if (options[i] == kOptionModifierMapForAltGr) {
      id = kKeyModifierIDAltGr;
    } else if (options[i] == kOptionModifierMapForMeta) {
      id = kKeyModifierIDMeta;
    } else if (options[i] == kOptionModifierMapForSuper) {
      id = kKeyModifierIDSuper;
    } else if (options[i] == kOptionHeartbeat) {
      // update keep alive
      setKeepAliveRate(1.0e-3 * static_cast<double>(options[i + 1]));
    }

    if (id != kKeyModifierIDNull) {
      m_modifierTranslationTable[id] = options[i + 1];
      LOG_DEBUG1("modifier %d mapped to %d", id, m_modifierTranslationTable[id]);
    }
  }
}

void ServerProxy::queryInfo()
{
  ClientInfo info;
  m_client->getShape(info.m_x, info.m_y, info.m_w, info.m_h);
  m_client->getCursorPos(info.m_mx, info.m_my);
  sendInfo(info);
}

void ServerProxy::infoAcknowledgment()
{
  LOG_DEBUG1("recv info acknowledgment");
  m_ignoreMouse = false;
}

void ServerProxy::secureInputNotification()
{
  std::string app;
  ProtocolUtil::readf(m_stream, kMsgDSecureInputNotification + 4, &app);
  LOG_INFO("application \"%s\" is blocking the keyboard", app.c_str());
}

void ServerProxy::setServerLanguages()
{
  std::string serverLayout;
  ProtocolUtil::readf(m_stream, kMsgDLanguageSynchronisation + 4, &serverLayout);
  m_layoutManager.setRemoteLayouts(serverLayout);
}

void ServerProxy::setActiveServerLanguage(const std::string_view &language)
{
  if (!language.empty() && (language.size() > 0)) {
    if (m_serverLayout != language) {
      m_isUserNotifiedAboutLayoutSyncError = false;
      m_serverLayout = language;
    }

    if (!m_layoutManager.isLayoutInstalled(m_serverLayout)) {
      if (!m_isUserNotifiedAboutLayoutSyncError) {
        LOG_WARN("current server layout is not installed on client");
        m_isUserNotifiedAboutLayoutSyncError = true;
      }
    } else {
      m_isUserNotifiedAboutLayoutSyncError = false;
    }
  } else {
    LOG_DEBUG1("active server layout is empty");
  }
}

uint32_t ServerProxy::requestFile(
    const std::string &filePath, const std::string &relativePath, bool isDir, uint32_t batchTransferId
)
{
  uint32_t requestId = FileTransfer::generateRequestId();

  // Use batch ID if provided, otherwise use this request's ID
  uint32_t effectiveBatchId = (batchTransferId != 0) ? batchTransferId : requestId;

  LOG_INFO(
      "requesting file: id=%u, batchId=%u, path=%s, relativePath=%s, isDir=%d", requestId, effectiveBatchId,
      filePath.c_str(), relativePath.c_str(), isDir
  );

  // Store request info for when we receive the response
  FileTransferRequest request;
  request.requestId = requestId;
  request.batchTransferId = effectiveBatchId;
  request.filePath = filePath;
  request.relativePath = relativePath.empty() ? filePath : relativePath;
  request.isDir = isDir;
  request.bytesTransferred = 0;
  request.isComplete = false;
  request.hasError = false;
  m_fileTransfers[requestId] = std::move(request);

  // Check if we have point-to-point source info in the clipboard metadata
  // We check clipboard 0 (primary) first, since file transfers are typically from there
  const DeferredClipboardState &clipState = m_deferredClipboard[kClipboardClipboard];
  if (!clipState.meta.sourceAddress.empty() && clipState.meta.sourcePort > 0) {
    // Point-to-point transfer: connect directly to source machine
    LOG_INFO(
        "[P2P] Using point-to-point transfer: source=%s:%u, sessionId=%llu", clipState.meta.sourceAddress.c_str(),
        clipState.meta.sourcePort, clipState.meta.sessionId
    );

    // Create or reuse file transfer connection
    if (!m_fileTransferConn) {
      SocketMultiplexer *multiplexer = new SocketMultiplexer();
      m_fileTransferConn = new FileTransferConnection(m_events, multiplexer);
    }

    // Set up callback for receiving data
    m_fileTransferConn->setDataCallback([this, requestId](FileChunkType type, const std::string &data) {
      handleFileChunkFromP2P(requestId, type, data);
    });

    // Connect point-to-point
    if (m_fileTransferConn->connectPointToPoint(
            clipState.meta.sourceAddress, clipState.meta.sourcePort, requestId, clipState.meta.sessionId, filePath
        )) {
      LOG_INFO("[P2P] Point-to-point connection established for requestId=%u", requestId);
      return requestId;
    } else {
      LOG_WARN("[P2P] Point-to-point connection failed, falling back to server relay");
      // Fall through to server relay method
    }
  }

  // Fallback: send file request to server via main connection
  // Build JSON request with extended file info
  auto escapeJson = [](const std::string &s) -> std::string {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
      }
    }
    return result;
  };

  std::ostringstream requestJson;
  requestJson << "{";
  requestJson << "\"path\":\"" << escapeJson(filePath) << "\",";
  requestJson << "\"relativePath\":\"" << escapeJson(relativePath.empty() ? filePath : relativePath) << "\",";
  requestJson << "\"isDir\":" << (isDir ? "true" : "false");
  requestJson << "}";

  std::string requestData = requestJson.str();

  // Send file request to server with JSON payload
  ProtocolUtil::writef(m_stream, kMsgQFileRequest, requestId, &requestData);

  return requestId;
}

void ServerProxy::fileChunkReceived()
{
  // Parse file chunk message: kMsgDFileChunk = "DFCH%4i%1i%s"
  // %4i = request ID (4 bytes)
  // %1i = chunk type (1 byte)
  // %s = data (string)

  uint32_t requestId = 0;
  uint8_t chunkType = 0;
  std::string data;

  if (!ProtocolUtil::readf(m_stream, kMsgDFileChunk + 4, &requestId, &chunkType, &data)) {
    LOG_ERR("failed to parse file chunk message");
    return;
  }

  FileChunkType type = static_cast<FileChunkType>(chunkType);

  switch (type) {
  case FileChunkType::Start: {
    // Parse file metadata from JSON (try extended format first, fall back to basic)
    std::string fileName;
    std::string relativePath;
    uint64_t fileSize = 0;
    bool isDir = false;

    if (!FileTransfer::parseStartChunkEx(data, fileName, relativePath, fileSize, isDir)) {
      // Try basic format for backward compatibility
      if (!FileTransfer::parseStartChunk(data, fileName, fileSize)) {
        LOG_ERR("failed to parse file start chunk");
        return;
      }
      relativePath = fileName; // Use fileName as relativePath if not provided
    }

    LOG_INFO(
        "file transfer started: id=%u, name=%s, relativePath=%s, size=%llu, isDir=%d", requestId, fileName.c_str(),
        relativePath.c_str(), fileSize, isDir
    );

    // Log for GUI monitoring
    LOG_NOTE("FILE_TRANSFER_START: id=%u, name=%s, size=%llu", requestId, fileName.c_str(), fileSize);

    // Check if we already have a pending request (created during requestFile)
    auto it = m_fileTransfers.find(requestId);
    if (it != m_fileTransfers.end()) {
      // Update existing request with server-provided metadata
      // Keep batchTransferId from client request
      it->second.fileName = fileName;
      it->second.fileSize = fileSize;
      // Use server's relativePath if available, otherwise keep client's
      if (!relativePath.empty()) {
        it->second.relativePath = relativePath;
      }
      it->second.isDir = isDir;
      if (!isDir) {
        it->second.data.reserve(static_cast<size_t>(fileSize));
      }
    } else {
      // Create new transfer request (unexpected but handle gracefully)
      FileTransferRequest request;
      request.requestId = requestId;
      request.batchTransferId = requestId; // Use requestId as batchId for unsolicited transfers
      request.fileName = fileName;
      request.relativePath = relativePath;
      request.fileSize = fileSize;
      request.bytesTransferred = 0;
      request.isComplete = false;
      request.hasError = false;
      request.isDir = isDir;
      if (!isDir) {
        request.data.reserve(static_cast<size_t>(fileSize));
      }
      m_fileTransfers[requestId] = std::move(request);
    }
    break;
  }

  case FileChunkType::Data: {
    auto it = m_fileTransfers.find(requestId);
    if (it == m_fileTransfers.end()) {
      LOG_ERR("received data chunk for unknown request: %u", requestId);
      return;
    }

    // Append data
    it->second.data.insert(it->second.data.end(), data.begin(), data.end());
    it->second.bytesTransferred += data.size();

    LOG_DEBUG1(
        "file data chunk: id=%u, chunk=%zu, total=%llu/%llu", requestId, data.size(), it->second.bytesTransferred,
        it->second.fileSize
    );

    // Log progress at 10% intervals for GUI monitoring
    if (it->second.fileSize > 0) {
      int currentPercent = static_cast<int>((it->second.bytesTransferred * 100) / it->second.fileSize);
      int prevPercent =
          static_cast<int>(((it->second.bytesTransferred - data.size()) * 100) / it->second.fileSize);

      // Log every 10%
      if ((currentPercent / 10) > (prevPercent / 10)) {
        LOG_NOTE(
            "FILE_TRANSFER_PROGRESS: id=%u, percent=%d, bytes=%llu, total=%llu", requestId, currentPercent,
            it->second.bytesTransferred, it->second.fileSize
        );
      }
    }
    break;
  }

  case FileChunkType::End: {
    auto it = m_fileTransfers.find(requestId);
    if (it == m_fileTransfers.end()) {
      LOG_ERR("received end chunk for unknown request: %u", requestId);
      return;
    }

    it->second.isComplete = true;

    // Use relativePath to preserve directory structure if available
    std::string tempPath;
    if (!it->second.relativePath.empty() && it->second.relativePath != it->second.fileName) {
      // Use batch transfer ID as session identifier for directory structure
      // This ensures all files from the same batch go into the same directory
      uint32_t transferId = (it->second.batchTransferId != 0) ? it->second.batchTransferId : requestId;
      tempPath = FileTransfer::createTempFilePathWithRelative(it->second.relativePath, transferId);
    } else {
      tempPath = FileTransfer::createTempFilePath(it->second.fileName);
    }

    // Handle directory entries
    if (it->second.isDir) {
      // Create directory
      if (FileTransfer::createDirectoryPath(tempPath)) {
        it->second.filePath = tempPath;
        LOG_INFO("directory created: id=%u, path=%s", requestId, tempPath.c_str());

        // Store completed directory path
        m_completedFilePaths.push_back(tempPath);

        LOG_NOTE(
            "FILE_TRANSFER_COMPLETE: id=%u, file=%s, path=%s", requestId, it->second.fileName.c_str(), tempPath.c_str()
        );

        // Update macOS clipboard with completed file
        updateClipboardWithCompletedFiles();
      } else {
        LOG_ERR("failed to create directory: %s", tempPath.c_str());
        it->second.hasError = true;
        it->second.errorMessage = "Failed to create directory";

        LOG_NOTE("FILE_TRANSFER_ERROR: id=%u, error=Failed to create directory", requestId);
      }
    } else {
      // Save file
      std::ofstream outFile(tempPath, std::ios::binary);
      if (outFile.is_open()) {
        outFile.write(reinterpret_cast<const char *>(it->second.data.data()), it->second.data.size());
        outFile.close();

        it->second.filePath = tempPath;
        LOG_INFO("file transfer complete: id=%u, saved to %s", requestId, tempPath.c_str());

        // Store completed file path for later retrieval
        m_completedFilePaths.push_back(tempPath);

        // Log progress for GUI monitoring
        LOG_NOTE(
            "FILE_TRANSFER_COMPLETE: id=%u, file=%s, path=%s", requestId, it->second.fileName.c_str(), tempPath.c_str()
        );

        // Update macOS clipboard with completed file
        updateClipboardWithCompletedFiles();
      } else {
        LOG_ERR("failed to save file: %s", tempPath.c_str());
        it->second.hasError = true;
        it->second.errorMessage = "Failed to save file";

        LOG_NOTE("FILE_TRANSFER_ERROR: id=%u, error=Failed to save file", requestId);
      }
    }

    // Clean up transfer state (keep for a short time in case of retry)
    // For now, we remove it immediately
    m_fileTransfers.erase(it);
    break;
  }

  case FileChunkType::Error: {
    auto it = m_fileTransfers.find(requestId);
    if (it != m_fileTransfers.end()) {
      it->second.hasError = true;
      it->second.errorMessage = data;
      m_fileTransfers.erase(it);
    }
    LOG_ERR("file transfer error: id=%u, message=%s", requestId, data.c_str());
    break;
  }
  }
}

#ifdef __APPLE__
#include "platform/OSXClipboardFileConverter.h"
#include "platform/OSXPasteboardBridge.h"
#include "platform/OSXPasteboardPeeker.h"
#endif

void ServerProxy::updateClipboardWithCompletedFiles()
{
  if (m_completedFilePaths.empty()) {
    return;
  }

#ifdef __APPLE__
  // Convert std::vector<std::string> to const char**
  std::vector<const char*> cPaths;
  cPaths.reserve(m_completedFilePaths.size());
  for (const auto& path : m_completedFilePaths) {
    cPaths.push_back(path.c_str());
  }

  LOG_INFO("macOS: storing %zu completed file path(s) for clipboard", m_completedFilePaths.size());
  OSXClipboardFileConverter::setCompletedFilePaths(m_completedFilePaths);

  // Signal that transfer is complete (wakes up promise keeper callback if waiting)
  OSXClipboardFileConverter::signalTransferComplete();
  LOG_INFO("macOS: signaled file transfer complete");

  // Clear Finder Extension pending state now that files are transferred
  OSXPasteboardBridge::clearPendingFiles();

  // Also update the pasteboard directly with file URLs
  updatePasteboardWithFiles(cPaths.data(), static_cast<int>(cPaths.size()));
#elif defined(_WIN32)
  // Update Windows clipboard file converter with completed paths
  LOG_INFO("Windows: storing %zu completed file path(s) for clipboard", m_completedFilePaths.size());
  MSWindowsClipboardFileConverter::setCompletedFilePaths(m_completedFilePaths);

  // Signal that transfer is complete (wakes up WM_RENDERFORMAT handler if waiting)
  MSWindowsClipboardFileConverter::signalTransferComplete();
  LOG_INFO("Windows: signaled file transfer complete");
#endif
}

void ServerProxy::handleFileTransferPort()
{
  uint32_t requestId;
  uint16_t transferPort;

  if (!ProtocolUtil::readf(m_stream, kMsgDFileTransferPort + 4, &requestId, &transferPort)) {
    LOG_ERR("[FileTransfer] Failed to parse file transfer port message");
    return;
  }

  LOG_INFO("[FileTransfer] Received transfer port %u for requestId %u", transferPort, requestId);

  // Find the file transfer request
  auto it = m_fileTransfers.find(requestId);
  if (it == m_fileTransfers.end()) {
    LOG_ERR("[FileTransfer] Unknown requestId %u in port notification", requestId);
    return;
  }

  // Create file transfer connection if needed
  if (!m_fileTransferConn) {
    // Create a SocketMultiplexer for file transfer if not provided
    // Each file transfer connection can have its own multiplexer
    SocketMultiplexer *multiplexer = m_socketMultiplexer;
    if (!multiplexer) {
      multiplexer = new SocketMultiplexer();
      LOG_INFO("[FileTransfer] Created dedicated SocketMultiplexer for file transfers");
    }

    LOG_INFO("[FileTransfer] Creating file transfer connection");
    m_fileTransferConn = new FileTransferConnection(m_events, multiplexer);
  }

  // Get server address from client
  NetworkAddress serverAddr(m_client->getServerAddress());

  // Connect to file transfer port
  if (m_fileTransferConn->connect(serverAddr, requestId)) {
    LOG_INFO("[FileTransfer] ✅ Connected to file transfer channel, requestId=%u", requestId);

    // Set callback to handle received file chunks
    m_fileTransferConn->setDataCallback([this, requestId](FileChunkType type, const std::string &data) {
      LOG_DEBUG1("[FileTransfer] Received chunk via dedicated channel: type=%d, size=%zu", static_cast<int>(type), data.size());

      // Simulate receiving via main stream for existing logic compatibility
      // We manually update the file transfer state
      auto it = m_fileTransfers.find(requestId);
      if (it == m_fileTransfers.end()) {
        LOG_WARN("[FileTransfer] Received chunk for unknown request %u", requestId);
        return;
      }

      // Process chunk based on type (reusing logic from fileChunkReceived)
      switch (type) {
        case FileChunkType::Start: {
          // Parse metadata
          std::string fileName, relativePath;
          uint64_t fileSize = 0;
          bool isDir = false;

          if (FileTransfer::parseStartChunkEx(data, fileName, relativePath, fileSize, isDir)) {
            it->second.fileName = fileName;
            it->second.fileSize = fileSize;
            it->second.relativePath = relativePath;
            it->second.isDir = isDir;
            if (!isDir) {
              it->second.data.reserve(static_cast<size_t>(fileSize));
            }
            LOG_INFO("[FileTransfer] Start chunk received: name=%s, size=%llu", fileName.c_str(), fileSize);
          }
          break;
        }

        case FileChunkType::Data: {
          it->second.data.insert(it->second.data.end(), data.begin(), data.end());
          it->second.bytesTransferred += data.size();
          LOG_DEBUG1("[FileTransfer] Data chunk: %zu bytes, total=%llu/%llu",
                     data.size(), it->second.bytesTransferred, it->second.fileSize);
          break;
        }

        case FileChunkType::End: {
          LOG_INFO("[FileTransfer] End chunk received, saving file");
          // Save file (reuse logic from fileChunkReceived)
          std::string tempPath = FileTransfer::createTempFilePath(it->second.fileName);
          std::ofstream outFile(tempPath, std::ios::binary);
          if (outFile.is_open()) {
            outFile.write(reinterpret_cast<const char*>(it->second.data.data()), it->second.data.size());
            outFile.close();
            it->second.filePath = tempPath;
            m_completedFilePaths.push_back(tempPath);
            LOG_INFO("[FileTransfer] ✅ File saved: %s", tempPath.c_str());

            // Update clipboard on macOS
            updateClipboardWithCompletedFiles();
          }
          m_fileTransfers.erase(it);
          break;
        }

        case FileChunkType::Error: {
          LOG_ERR("[FileTransfer] Transfer error: %s", data.c_str());
          it->second.hasError = true;
          it->second.errorMessage = data;
          m_fileTransfers.erase(it);
          break;
        }
      }
    });
  } else {
    LOG_ERR("[FileTransfer] Failed to connect to file transfer channel");
  }
}

void ServerProxy::handleServerFileRequest()
{
  // Parse file request message from server: kMsgSFileRequest = "SFIL%4i%s"
  // Server is requesting a file from this client (Client → Host transfer)

  uint32_t requestId = 0;
  std::string requestData;

  if (!ProtocolUtil::readf(m_stream, kMsgSFileRequest + 4, &requestId, &requestData)) {
    LOG_ERR("[FileTransfer] failed to parse server file request message");
    return;
  }

  LOG_INFO("[FileTransfer] received server file request: id=%u, data=%s", requestId, requestData.c_str());

  // Parse JSON request data to extract file path
  // Format: {"path":"...", "sessionId":123, "relativePath":"...", "isDir":false}
  std::string filePath;
  bool isDir = false;

  // Simple JSON parsing for required fields
  size_t pathPos = requestData.find("\"path\":\"");
  if (pathPos != std::string::npos) {
    size_t start = pathPos + 8;
    size_t end = requestData.find("\"", start);
    if (end != std::string::npos) {
      filePath = requestData.substr(start, end - start);
      // Unescape basic JSON escapes
      size_t pos = 0;
      while ((pos = filePath.find("\\\\", pos)) != std::string::npos) {
        filePath.replace(pos, 2, "\\");
        pos += 1;
      }
      pos = 0;
      while ((pos = filePath.find("\\/", pos)) != std::string::npos) {
        filePath.replace(pos, 2, "/");
        pos += 1;
      }
    }
  }

  size_t isDirPos = requestData.find("\"isDir\":true");
  if (isDirPos != std::string::npos) {
    isDir = true;
  }

  if (filePath.empty()) {
    LOG_ERR("[FileTransfer] invalid server file request: missing path");
    sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::Error), "invalid request: missing path");
    return;
  }

  LOG_INFO("[FileTransfer] server requesting file: %s (isDir=%d)", filePath.c_str(), isDir);

  // Check if file exists
  std::ifstream file(filePath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_ERR("[FileTransfer] file not found: %s", filePath.c_str());
    sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::Error), "file not found");
    return;
  }

  if (isDir) {
    // For directories, just send success (directory creation is handled by metadata)
    LOG_INFO("[FileTransfer] directory request, sending success");
    std::string metadata = "{\"name\":\"" + filePath + "\",\"size\":0,\"isDir\":true}";
    sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::Start), metadata);
    sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::End), "");
    return;
  }

  // Get file size
  auto fileSize = file.tellg();
  file.seekg(0, std::ios::beg);

  LOG_INFO("[FileTransfer] reading file: %s (size=%lld)", filePath.c_str(), static_cast<long long>(fileSize));

  // Extract filename from path
  std::string fileName = filePath;
  size_t lastSlash = filePath.find_last_of("/\\");
  if (lastSlash != std::string::npos) {
    fileName = filePath.substr(lastSlash + 1);
  }

  // Send start chunk with metadata
  std::ostringstream metadata;
  metadata << "{\"name\":\"" << fileName << "\",\"size\":" << fileSize << ",\"isDir\":false}";
  sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::Start), metadata.str());

  // Send file data in chunks
  const size_t chunkSize = 64 * 1024; // 64KB chunks
  std::vector<char> buffer(chunkSize);

  while (file) {
    file.read(buffer.data(), chunkSize);
    std::streamsize bytesRead = file.gcount();
    if (bytesRead > 0) {
      std::string chunk(buffer.data(), static_cast<size_t>(bytesRead));
      sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::Data), chunk);
    }
  }

  file.close();

  // Send end chunk
  sendFileChunkToServer(requestId, static_cast<uint8_t>(FileChunkType::End), "");
  LOG_INFO("[FileTransfer] file sent successfully: %s", filePath.c_str());
}

void ServerProxy::sendFileChunkToServer(uint32_t requestId, uint8_t chunkType, const std::string &data)
{
  LOG_DEBUG("[FileTransfer] sending chunk to server: id=%u, type=%u, size=%zu", requestId, chunkType, data.size());
  ProtocolUtil::writef(m_stream, kMsgDFileChunk, requestId, chunkType, &data);
}

void ServerProxy::handleFileChunkFromP2P(uint32_t requestId, FileChunkType type, const std::string &data)
{
  LOG_DEBUG1("[FileTransfer] P2P chunk received: type=%d, size=%zu, requestId=%u", static_cast<int>(type), data.size(), requestId);

  auto it = m_fileTransfers.find(requestId);
  if (it == m_fileTransfers.end()) {
    LOG_WARN("[FileTransfer] P2P chunk for unknown request %u", requestId);
    return;
  }

  switch (type) {
    case FileChunkType::Start: {
      // Parse metadata
      std::string fileName, relativePath;
      uint64_t fileSize = 0;
      bool isDir = false;

      if (FileTransfer::parseStartChunkEx(data, fileName, relativePath, fileSize, isDir)) {
        it->second.fileName = fileName;
        it->second.fileSize = fileSize;
        it->second.relativePath = relativePath;
        it->second.isDir = isDir;
        if (!isDir) {
          it->second.data.reserve(static_cast<size_t>(fileSize));
        }
        LOG_INFO("[FileTransfer] P2P start chunk: name=%s, size=%llu", fileName.c_str(), fileSize);
      }
      break;
    }

    case FileChunkType::Data: {
      it->second.data.insert(it->second.data.end(), data.begin(), data.end());
      it->second.bytesTransferred += data.size();
      LOG_DEBUG1("[FileTransfer] P2P data chunk: %zu bytes, total=%llu/%llu", data.size(), it->second.bytesTransferred, it->second.fileSize);
      break;
    }

    case FileChunkType::End: {
      LOG_INFO("[FileTransfer] P2P end chunk received, saving file");
      std::string tempPath = FileTransfer::createTempFilePath(it->second.fileName);
      std::ofstream outFile(tempPath, std::ios::binary);
      if (outFile.is_open()) {
        outFile.write(reinterpret_cast<const char *>(it->second.data.data()), it->second.data.size());
        outFile.close();
        it->second.filePath = tempPath;
        m_completedFilePaths.push_back(tempPath);
        LOG_INFO("[FileTransfer] P2P file saved: %s", tempPath.c_str());

        // Update clipboard on macOS
        updateClipboardWithCompletedFiles();
      } else {
        LOG_ERR("[FileTransfer] P2P failed to open file for writing: %s", tempPath.c_str());
      }
      m_fileTransfers.erase(it);
      break;
    }

    case FileChunkType::Error: {
      LOG_ERR("[FileTransfer] P2P transfer error: %s", data.c_str());
      it->second.hasError = true;
      it->second.errorMessage = data;
      m_fileTransfers.erase(it);
      break;
    }
  }
}
