/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardMeta.h"
#include "deskflow/ClipboardTypes.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/KeyTypes.h"
#include "deskflow/KeyboardLayoutManager.h"

#include <map>

class Client;
class ClientInfo;
class EventQueueTimer;
class IClipboard;
namespace deskflow {
class IStream;
}
class IEventQueue;
class SocketMultiplexer;

//! Proxy for server
/*!
This class acts a proxy for the server, converting calls into messages
to the server and messages from the server to calls on the client.
*/
class ServerProxy
{
public:
  /*!
  Process messages from the server on \p stream and forward to
  \p client.
  */
  ServerProxy(Client *client, deskflow::IStream *stream, IEventQueue *events, SocketMultiplexer *socketMultiplexer = nullptr);
  ServerProxy(ServerProxy const &) = delete;
  ServerProxy(ServerProxy &&) = delete;
  ~ServerProxy();

  ServerProxy &operator=(ServerProxy const &) = delete;
  ServerProxy &operator=(ServerProxy &&) = delete;

  //! @name manipulators
  //@{

  void onInfoChanged();
  bool onGrabClipboard(ClipboardID);
  void onClipboardChanged(ClipboardID, const IClipboard *);

  //! Request a file from the server
  /*!
  Sends a file request to the server. When the file is received,
  it will be saved to a temporary location.
  \param filePath the path of the file on the server
  \param relativePath the relative path for preserving directory structure
  \param isDir true if this is a directory entry
  \param batchTransferId optional batch ID for grouping related files
  \return the request ID for tracking the transfer
  */
  uint32_t requestFile(
      const std::string &filePath, const std::string &relativePath = "", bool isDir = false, uint32_t batchTransferId = 0
  );

  //@}

protected:
  enum class ConnectionResult
  {
    Okay,
    Unknown,
    Disconnect
  };
  ConnectionResult parseHandshakeMessage(const uint8_t *code);
  ConnectionResult parseMessage(const uint8_t *code);

private:
  // if compressing mouse motion then send the last motion now
  void flushCompressedMouse();

  void sendInfo(const ClientInfo &);

  void resetKeepAliveAlarm();
  void setKeepAliveRate(double);

  // modifier key translation
  KeyID translateKey(KeyID) const;
  KeyModifierMask translateModifierMask(KeyModifierMask) const;

  // event handlers
  void handleData();
  void handleKeepAliveAlarm();

  // message handlers
  void enter();
  void leave();
  void setClipboard();
  void grabClipboard();
  void keyDown(uint16_t id, uint16_t mask, uint16_t button, const std::string &lang);
  void keyRepeat();
  void keyUp();
  void mouseDown();
  void mouseUp();
  void mouseMove();
  void mouseRelativeMove();
  void mouseWheel();
  void screensaver();
  void resetOptions();
  void setOptions();
  void queryInfo();
  void infoAcknowledgment();
  void secureInputNotification();
  void setServerLanguages();
  void setActiveServerLanguage(const std::string_view &language);
  void fileChunkReceived();
  void setClipboardMeta();

  //! Handle file request from server (for Client → Host transfer)
  void handleServerFileRequest();

  //! Send file chunk to server (for Client → Host transfer)
  void sendFileChunkToServer(uint32_t requestId, uint8_t chunkType, const std::string &data);

  //! Request clipboard data from server for deferred transfer
  void requestClipboardData(ClipboardID id);

private:
  using MessageParser = ConnectionResult (ServerProxy::*)(const uint8_t *);

  Client *m_client = nullptr;
  deskflow::IStream *m_stream = nullptr;

  uint32_t m_seqNum = 0;

  bool m_compressMouse = false;
  bool m_compressMouseRelative = false;
  int32_t m_xMouse = 0;
  int32_t m_yMouse = 0;
  int32_t m_dxMouse = 0;
  int32_t m_dyMouse = 0;

  bool m_ignoreMouse = false;

  KeyModifierID m_modifierTranslationTable[kKeyModifierIDLast];

  double m_keepAliveAlarm = 0.0;
  EventQueueTimer *m_keepAliveAlarmTimer = nullptr;

  MessageParser m_parser = &ServerProxy::parseHandshakeMessage;
  IEventQueue *m_events = nullptr;
  std::string m_serverLayout = "";
  bool m_isUserNotifiedAboutLayoutSyncError = false;
  deskflow::KeyboardLayoutManager m_layoutManager;
  SocketMultiplexer *m_socketMultiplexer = nullptr;

  // File transfer state
  std::map<uint32_t, FileTransferRequest> m_fileTransfers;
  std::vector<std::string> m_completedFilePaths;

  //! Deferred clipboard state
  struct DeferredClipboardState
  {
    ClipboardMeta meta;           //!< Clipboard metadata
    bool isTransferring = false;  //!< True if data transfer is in progress
    bool deleteOnComplete = false; //!< True if should be cleared when transfer completes

    void clear()
    {
      meta = ClipboardMeta();
      isTransferring = false;
      deleteOnComplete = false;
    }

    bool isActive() const
    {
      return meta.deferred && meta.sessionId != 0;
    }
  };

  // Deferred clipboard state (per clipboard ID)
  DeferredClipboardState m_deferredClipboard[kClipboardEnd];

  //! Clear deferred clipboard state for a specific ID
  void clearDeferredClipboard(ClipboardID id);

  //! Clear all non-transferring deferred clipboards, mark transferring ones for auto-delete
  void cleanupDeferredClipboards(ClipboardID newClipboardId);

  // Update macOS clipboard with completed files (macOS only)
  void updateClipboardWithCompletedFiles();

  // Dedicated file transfer connection
  class FileTransferConnection *m_fileTransferConn = nullptr;

  // Handle file transfer port notification from server
  void handleFileTransferPort();

  // Handle file chunk from point-to-point connection
  void handleFileChunkFromP2P(uint32_t requestId, FileChunkType type, const std::string &data);
};
