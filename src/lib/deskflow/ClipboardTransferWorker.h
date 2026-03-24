/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class IEventQueue;
class SocketMultiplexer;
class ISocketFactory;
class ClipboardTransferServer;
class ClipboardTransferClient;
class ClipboardTransferThread;

/**
 * @brief Worker object that runs in the QThread
 *
 * This object runs in the QThread and performs all the actual work.
 * Using QThread ensures proper thread-local storage initialization on POSIX
 * platforms, which is required by SocketMultiplexer.
 *
 * Note: This class doesn't use Q_OBJECT/signals/slots to avoid forcing
 * all consumers to enable AUTOMOC. Instead, it uses callbacks and
 * condition variables for synchronization.
 */
class ClipboardTransferWorker
{
public:
  using InitCallback = std::function<void(bool success)>;

  explicit ClipboardTransferWorker(ClipboardTransferThread *owner);
  ~ClipboardTransferWorker();

  // Called to start initialization and processing (runs in QThread context)
  void run();

  void requestStop();
  uint16_t getServerPort() const;
  std::string getLocalAddress() const;
  ClipboardTransferServer *getServer() const;
  ClipboardTransferClient *getClient() const;
  IEventQueue *getEvents() const;

  // Set callback for initialization completion
  void setInitCallback(InitCallback callback);

private:
  void processLoop();
  void cleanup();

  ClipboardTransferThread *m_owner = nullptr;
  std::atomic<bool> m_stopping{false};
  bool m_initialized = false;

  InitCallback m_initCallback;

  std::unique_ptr<IEventQueue> m_events;
  std::unique_ptr<SocketMultiplexer> m_socketMultiplexer;
  std::unique_ptr<ISocketFactory> m_socketFactory;
  std::unique_ptr<ClipboardTransferServer> m_server;
  std::unique_ptr<ClipboardTransferClient> m_client;
};
