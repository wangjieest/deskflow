/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_6.h"

#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ClipboardMeta.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "io/IStream.h"
#include "server/Server.h"

#include <cstring>

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_5(name, stream, server, events),
      m_events(events)
{
  m_events->addHandler(EventTypes::ClipboardSending, this, [this](const auto &e) {
    ClipboardChunk::send(getStream(), e.getDataObject());
  });
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

    std::string data = m_clipboard[id].m_clipboard.marshall();

    size_t size = data.size();
    LOG_DEBUG("sending clipboard %d to \"%s\"", id, getName().c_str());

    StreamChunker::sendClipboard(data, size, id, 0, m_events, this);
  }
}

void ClientProxy1_6::setClipboardMeta(ClipboardID id, const ClipboardMeta &meta)
{
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;

    // Send metadata instead of full data for deferred transfer
    std::string metaJson = meta.serialize();
    LOG_DEBUG(
        "sending clipboard %d metadata to \"%s\" (size=%llu, deferred=%s)", id, getName().c_str(), meta.totalSize,
        meta.deferred ? "true" : "false"
    );

    ProtocolUtil::writef(getStream(), kMsgDClipboardMeta, id, &metaJson);
  }
}

bool ClientProxy1_6::recvClipboard()
{
  // parse message
  static std::string dataCached;
  ClipboardID id;
  uint32_t seq;

  if (auto r = ClipboardChunk::assemble(getStream(), dataCached, id, seq); r == TransferState::Started) {
    size_t size = ClipboardChunk::getExpectedSize();
    LOG_DEBUG("receiving clipboard %d size=%d", id, size);
  } else if (r == TransferState::Finished) {
    LOG(
        (CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%d", getName().c_str(), id, seq,
         dataCached.size())
    );
    // save clipboard
    m_clipboard[id].m_clipboard.unmarshall(dataCached, 0);
    m_clipboard[id].m_sequenceNumber = seq;

    // notify
    auto *info = new ClipboardInfo;
    info->m_id = id;
    info->m_sequenceNumber = seq;
    m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
  }

  return true;
}

bool ClientProxy1_6::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgQClipboardData, 4) == 0) {
    handleClipboardDataRequest();
    return true;
  }
  return ClientProxy1_5::parseMessage(code);
}

void ClientProxy1_6::handleClipboardDataRequest()
{
  // Parse message: kMsgQClipboardData = "QCLD%1i%s"
  // %1i = clipboard ID (1 byte)
  // %s = JSON request string

  ClipboardID id;
  std::string requestJson;

  if (!ProtocolUtil::readf(getStream(), kMsgQClipboardData + 4, &id, &requestJson)) {
    LOG_ERR("failed to parse clipboard data request message");
    return;
  }

  // Validate clipboard ID
  if (id >= kClipboardEnd) {
    LOG_ERR("invalid clipboard ID in data request: %d", id);
    return;
  }

  // Deserialize the request
  ClipboardDataRequest request = ClipboardDataRequest::deserialize(requestJson);

  LOG_INFO(
      "received clipboard %d data request from \"%s\" (sessionId=%llu)", id, getName().c_str(), request.sessionId
  );

  // Validate the session ID matches current clipboard
  uint64_t currentSessionId = m_server->getClipboardSessionId(id);
  if (request.sessionId != currentSessionId) {
    LOG_WARN(
        "clipboard data request rejected: session mismatch (requested=%llu, current=%llu)", request.sessionId,
        currentSessionId
    );
    // Note: We could send an error response here if needed
    return;
  }

  // Session is valid - send the full clipboard data
  // This uses the existing setClipboard mechanism which sends chunked data
  LOG_DEBUG("sending clipboard %d data for deferred request", id);

  // Mark this clipboard as dirty to force resend
  m_clipboard[id].m_dirty = true;

  // Get the clipboard from server and send it
  m_server->sendClipboardToClient(this, id);
}
