/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardTransferWorker.h"

#include "base/EventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardTransferClient.h"
#include "deskflow/ClipboardTransferServer.h"
#include "deskflow/ClipboardTransferThread.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocketFactory.h"

#ifdef _WIN32
#include <objbase.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

ClipboardTransferWorker::ClipboardTransferWorker(ClipboardTransferThread *owner)
    : m_owner(owner)
{
}

ClipboardTransferWorker::~ClipboardTransferWorker()
{
  cleanup();
}

void ClipboardTransferWorker::setInitCallback(InitCallback callback)
{
  m_initCallback = std::move(callback);
}

void ClipboardTransferWorker::run()
{
  LOG_DEBUG("[ClipboardTransfer] Worker::run() in QThread");

#ifdef _WIN32
  // Initialize OLE for this thread (required for OleSetClipboard)
  HRESULT oleHr = OleInitialize(nullptr);
  if (FAILED(oleHr)) {
    LOG_WARN("[ClipboardTransfer] OleInitialize failed: 0x%08X, falling back to CoInitialize", oleHr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  }
#endif

  try {
    // Create our own event queue for this thread
    m_events = std::make_unique<EventQueue>();

    // Create socket multiplexer
    m_socketMultiplexer = std::make_unique<SocketMultiplexer>();

    // Create socket factory
    m_socketFactory = std::make_unique<TCPSocketFactory>(m_events.get(), m_socketMultiplexer.get());

    // Create server and client
    m_server = std::make_unique<ClipboardTransferServer>(m_events.get(), m_socketMultiplexer.get(), m_socketFactory.get());
    m_client = std::make_unique<ClipboardTransferClient>(m_events.get(), m_socketMultiplexer.get());

    // Start the server
    if (!m_server->start()) {
      LOG_ERR("[ClipboardTransfer] failed to start server");
      if (m_initCallback) {
        m_initCallback(false);
      }
      return;
    }

    LOG_INFO("[ClipboardTransfer] server started on port %u", m_server->getPort());

#ifdef _WIN32
    // Create clipboard window in this thread
    m_owner->createClipboardWindow();
#endif

    m_initialized = true;
    if (m_initCallback) {
      m_initCallback(true);
    }

    // Start the processing loop
    processLoop();

  } catch (const std::exception &e) {
    LOG_ERR("[ClipboardTransfer] Worker exception: %s", e.what());
    if (m_initCallback) {
      m_initCallback(false);
    }
  }
}

void ClipboardTransferWorker::requestStop()
{
  m_stopping = true;
}

uint16_t ClipboardTransferWorker::getServerPort() const
{
  return m_server ? m_server->getPort() : 0;
}

std::string ClipboardTransferWorker::getLocalAddress() const
{
  return m_server ? m_server->getLocalAddress() : "";
}

ClipboardTransferServer *ClipboardTransferWorker::getServer() const
{
  return m_server.get();
}

ClipboardTransferClient *ClipboardTransferWorker::getClient() const
{
  return m_client.get();
}

IEventQueue *ClipboardTransferWorker::getEvents() const
{
  return m_events.get();
}

void ClipboardTransferWorker::processLoop()
{
  LOG_DEBUG("[ClipboardTransfer] entering processing loop");

  // Track idle iterations to detect stuck state
  int idleIterations = 0;
  const int maxIdleIterations = 1000; // About 10 seconds at 10ms per iteration

  while (!m_stopping) {
    bool hadActivity = false;

    // Process pending messages from main thread
    m_owner->processMessages();

    // Process event queue with short timeout
    if (m_events) {
      Event event;
      if (m_events->getEvent(event, 0.01)) { // 10ms timeout
        m_events->dispatchEvent(event);
        hadActivity = true;
      }
    }

#ifdef _WIN32
    // Process ALL Windows messages (not just clipboard window)
    // OLE clipboard requires a full message pump for COM marshalling
    {
      MSG winMsg;
      while (PeekMessage(&winMsg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&winMsg);
        DispatchMessage(&winMsg);
        hadActivity = true;
      }
    }
#endif

    // Track idle state for debugging stuck threads
    if (hadActivity) {
      idleIterations = 0;
    } else {
      idleIterations++;
      if (idleIterations == maxIdleIterations) {
        LOG_WARN("[ClipboardTransfer] processLoop has been idle for %d iterations", maxIdleIterations);
      }
    }

    // No QCoreApplication dependency - the event processing in m_events is sufficient
  }

  LOG_DEBUG("[ClipboardTransfer] exiting processing loop (stopping requested)");
  cleanup();
}

void ClipboardTransferWorker::cleanup()
{
  if (!m_initialized) {
    return;
  }

  LOG_DEBUG("[ClipboardTransfer] Worker cleanup starting");

#ifdef _WIN32
  m_owner->destroyClipboardWindow();
#endif

  // Stop server first to close all connections
  if (m_server) {
    LOG_DEBUG("[ClipboardTransfer] stopping server");
    m_server->stop();
  }

  // Reset in reverse order of creation to ensure proper cleanup
  LOG_DEBUG("[ClipboardTransfer] releasing server");
  m_server.reset();

  LOG_DEBUG("[ClipboardTransfer] releasing client");
  m_client.reset();

  LOG_DEBUG("[ClipboardTransfer] releasing socket factory");
  m_socketFactory.reset();

  LOG_DEBUG("[ClipboardTransfer] releasing socket multiplexer");
  m_socketMultiplexer.reset();

  LOG_DEBUG("[ClipboardTransfer] releasing event queue");
  m_events.reset();

  m_initialized = false;

#ifdef _WIN32
  OleUninitialize();
#endif

  LOG_DEBUG("[ClipboardTransfer] Worker cleanup complete");
}
