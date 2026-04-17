/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardTransferThread.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardTransferClient.h"
#include "deskflow/ClipboardTransferServer.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

ClipboardTransferThread::ClipboardTransferThread()
{
  LOG_DEBUG("[ClipboardTransfer] thread object created");
}

ClipboardTransferThread::~ClipboardTransferThread()
{
  stop();
}

bool ClipboardTransferThread::start()
{
  if (m_running.load()) {
    LOG_WARN("[ClipboardTransfer] thread already running");
    return true;
  }

  LOG_INFO("[ClipboardTransfer] starting thread with QThread");

  m_stopping.store(false);

  try {
    // Track initialization result
    bool initSuccess = false;
    bool initDone = false;
    std::mutex initMutex;
    std::condition_variable initCondition;

    // Create Worker with init callback
    m_worker = new ClipboardTransferWorker(this);
    m_worker->setInitCallback([this, &initSuccess, &initDone, &initMutex, &initCondition](bool success) {
      std::lock_guard<std::mutex> lock(initMutex);
      initSuccess = success;
      m_running.store(success);
      initDone = true;
      initCondition.notify_all();
    });

    // Create a custom QThread subclass to run the worker
    class WorkerThread : public QThread
    {
    public:
      explicit WorkerThread(ClipboardTransferWorker *worker)
          : m_worker(worker)
      {
      }

    protected:
      void run() override
      {
        if (m_worker) {
          m_worker->run();
        }
      }

    private:
      ClipboardTransferWorker *m_worker;
    };

    m_thread = new WorkerThread(m_worker);

    // Connect cleanup signals
    // Capture worker pointer by value to avoid accessing 'this' after ClipboardTransferThread is destroyed
    ClipboardTransferWorker *workerPtr = m_worker;
    QObject::connect(m_thread, &QThread::finished, m_thread, [workerPtr]() {
      // Worker cleanup: delete the worker object when thread finishes
      // Don't access 'this' here - ClipboardTransferThread may already be destroyed
      if (workerPtr) {
        LOG_DEBUG("[ClipboardTransfer] deleting worker after thread finished");
        delete workerPtr;
      }
    });
    QObject::connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);

    // Start the thread
    m_thread->start();

    // Wait for initialization to complete
    std::unique_lock<std::mutex> lock(initMutex);
    bool completed = initCondition.wait_for(lock, std::chrono::seconds(5), [&initDone] { return initDone; });

    if (!completed || !initSuccess) {
      LOG_ERR("[ClipboardTransfer] QThread failed to start or initialize");
      if (m_worker) {
        m_worker->requestStop();
      }
      if (m_thread && m_thread->isRunning()) {
        m_thread->wait(3000);
      }
      delete m_worker;
      m_worker = nullptr;
      m_thread = nullptr;
      return false;
    }

    LOG_INFO("[ClipboardTransfer] QThread started successfully");
    return true;

  } catch (const std::exception &e) {
    LOG_ERR("[ClipboardTransfer] failed to create QThread: %s", e.what());
    return false;
  }
}

void ClipboardTransferThread::stop()
{
  if (!m_running.load()) {
    return;
  }

  LOG_INFO("[ClipboardTransfer] stopping QThread");

  m_stopping.store(true);

  // Post stop message
  Message msg;
  msg.type = MessageType::Stop;
  postMessage(std::move(msg));

  // Request worker to stop
  if (m_worker) {
    m_worker->requestStop();
  }

  // Wait for QThread to finish gracefully
  if (m_thread && m_thread->isRunning()) {
    // Don't use quit() since we're using run() override, just wait for it to finish
    // Use longer timeout for graceful shutdown, especially if file transfers are in progress
    const int gracefulTimeoutMs = 10000; // 10 seconds
    const int forceTimeoutMs = 2000;     // Additional 2 seconds before giving up

    if (!m_thread->wait(gracefulTimeoutMs)) {
      LOG_WARN("[ClipboardTransfer] QThread did not finish in %d ms, waiting additional %d ms",
               gracefulTimeoutMs, forceTimeoutMs);

      // Last attempt - give it more time but don't terminate
      // Terminating a thread is extremely dangerous and can cause crashes
      if (!m_thread->wait(forceTimeoutMs)) {
        LOG_ERR("[ClipboardTransfer] QThread still running after %d ms - thread may be stuck",
                gracefulTimeoutMs + forceTimeoutMs);
        LOG_ERR("[ClipboardTransfer] Leaving thread running to avoid crash. Memory leak may occur.");

        // Don't delete worker or thread - let them leak rather than crash
        // The OS will clean up when process exits
        m_running.store(false);
        m_worker = nullptr;  // Just null the pointer, don't delete
        m_thread = nullptr;  // Just null the pointer, don't delete
        return;
      }
    }
  }

  m_running.store(false);

  // Worker will be deleted by QThread::finished signal via deleteLater
  // Don't delete it here to avoid race condition
  m_worker = nullptr;
  m_thread = nullptr; // Will be deleted by QThread::finished signal

  LOG_INFO("[ClipboardTransfer] QThread stopped gracefully");
}

// Note: threadProc() is no longer used - the Worker class handles all thread work

void ClipboardTransferThread::processMessages()
{
  std::queue<Message> localQueue;

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    std::swap(localQueue, m_messageQueue);
  }

  while (!localQueue.empty()) {
    Message &msg = localQueue.front();

    switch (msg.type) {
    case MessageType::Stop:
      LOG_DEBUG("[ClipboardTransfer] received Stop message");
      m_stopping.store(true);
      break;

    case MessageType::SetAvailableFiles:
      handleSetAvailableFiles(msg);
      break;

    case MessageType::ClearSession:
      handleClearSession(msg);
      break;

    case MessageType::RequestFile:
      handleRequestFile(msg);
      break;

    case MessageType::RequestFiles:
      handleRequestFiles(msg);
      break;

#ifdef _WIN32
    case MessageType::SetDelayedRendering:
      handleSetDelayedRendering(msg);
      break;
#endif
    }

    localQueue.pop();
  }
}

void ClipboardTransferThread::handleSetAvailableFiles(const Message &msg)
{
  LOG_INFO("[ClipboardTransfer] setting %zu files for session %llu", msg.files.size(), msg.sessionId);

  ClipboardTransferServer *server = m_worker ? m_worker->getServer() : nullptr;
  if (server) {
    // Convert to server's file info format
    std::vector<FileTransferFileInfo> serverFiles;
    serverFiles.reserve(msg.files.size());
    for (const auto &f : msg.files) {
      FileTransferFileInfo info;
      info.path = f.path;
      info.relativePath = f.relativePath;
      info.size = f.size;
      info.isDir = f.isDir;
      serverFiles.push_back(std::move(info));
    }
    server->setSessionFiles(msg.sessionId, serverFiles);
  }
}

void ClipboardTransferThread::handleClearSession(const Message &msg)
{
  LOG_DEBUG("[ClipboardTransfer] clearing session %llu", msg.sessionId);

  ClipboardTransferServer *server = m_worker ? m_worker->getServer() : nullptr;
  if (server) {
    server->clearSession(msg.sessionId);
  }
}

void ClipboardTransferThread::handleRequestFile(const Message &msg)
{
  LOG_INFO(
      "[ClipboardTransfer] requesting file: %s from %s:%u", msg.remotePath.c_str(), msg.sourceAddr.c_str(), msg.port
  );

  m_pendingRequests++;

  ClipboardTransferClient *client = m_worker ? m_worker->getClient() : nullptr;
  if (client) {
    client->requestFile(
        msg.sourceAddr, msg.port, msg.sessionId, msg.remotePath,
        [this, callback = msg.callback](bool success, const std::string &localPath, const std::string &error) {
          m_pendingRequests--;
          m_pendingCondition.notify_all();

          if (callback) {
            callback(success, localPath, error);
          }
        }
    );
  } else {
    m_pendingRequests--;
    m_pendingCondition.notify_all();
    if (msg.callback) {
      msg.callback(false, "", "Client not initialized");
    }
  }
}

void ClipboardTransferThread::handleRequestFiles(const Message &msg)
{
  LOG_INFO(
      "[ClipboardTransfer] requesting %zu files from %s:%u to %s", msg.files.size(), msg.sourceAddr.c_str(), msg.port,
      msg.destFolder.empty() ? "(temp)" : msg.destFolder.c_str()
  );

  if (msg.files.empty()) {
    if (msg.batchCallback) {
      msg.batchCallback(true, {});
    }
    return;
  }

  m_pendingRequests++;

  // Set destination folder on the client so files go directly there
  ClipboardTransferClient *client = m_worker ? m_worker->getClient() : nullptr;
  if (!client) {
    LOG_ERR("[ClipboardTransfer] handleRequestFiles: client is null (worker=%s)", m_worker ? "ok" : "null");
    if (msg.batchCallback) msg.batchCallback(false, {});
    return;
  }
  if (!msg.destFolder.empty()) {
    client->setDestinationFolder(msg.destFolder);
  }

  // Download files sequentially in a single background thread to avoid
  // spawning hundreds of concurrent connections for large batches.
  auto files = msg.files;
  auto sourceAddr = msg.sourceAddr;
  auto port = msg.port;
  auto sessionId = msg.sessionId;
  auto destFolder = msg.destFolder;
  auto batchCallback = msg.batchCallback;

  std::thread([this, client, files, sourceAddr, port, sessionId, destFolder, batchCallback]() {
    std::vector<std::string> paths;
    bool allOk = true;

    for (const auto &file : files) {
      std::mutex m; std::condition_variable cv; bool done = false;
      bool ok = false; std::string lp;

      client->requestFile(
          sourceAddr, port, sessionId, file.path,
          [&](bool fileSuccess, const std::string &localPath, const std::string &err) {
            std::lock_guard<std::mutex> lock(m);
            ok = fileSuccess; lp = localPath;
            if (!fileSuccess) LOG_ERR("[ClipboardTransfer] file failed: %s", err.c_str());
            done = true; cv.notify_one();
          }
      );

      // Wait for this file to complete before starting next
      std::unique_lock<std::mutex> lock(m);
      cv.wait_for(lock, std::chrono::seconds(120), [&]{ return done; });

      if (ok) paths.push_back(lp);
      else allOk = false;
    }

    if (!destFolder.empty()) client->setDestinationFolder("");
    m_pendingRequests--;
    m_pendingCondition.notify_all();
    if (batchCallback) batchCallback(allOk, paths);
  }).detach();
}

void ClipboardTransferThread::postMessage(Message &&msg)
{
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_messageQueue.push(std::move(msg));
  }
  m_queueCondition.notify_one();
}

void ClipboardTransferThread::setAvailableFiles(uint64_t sessionId, const std::vector<ClipboardTransferFileInfo> &files)
{
  Message msg;
  msg.type = MessageType::SetAvailableFiles;
  msg.sessionId = sessionId;
  msg.files = files;
  postMessage(std::move(msg));
}

void ClipboardTransferThread::clearSession(uint64_t sessionId)
{
  Message msg;
  msg.type = MessageType::ClearSession;
  msg.sessionId = sessionId;
  postMessage(std::move(msg));
}

uint16_t ClipboardTransferThread::getServerPort() const
{
  if (m_worker) {
    return m_worker->getServerPort();
  }
  return 0;
}

std::string ClipboardTransferThread::getLocalAddress() const
{
  if (m_worker) {
    return m_worker->getLocalAddress();
  }
  return "";
}

void ClipboardTransferThread::requestFile(
    const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
    DataReceivedCallback callback
)
{
  Message msg;
  msg.type = MessageType::RequestFile;
  msg.sourceAddr = sourceAddr;
  msg.port = port;
  msg.sessionId = sessionId;
  msg.remotePath = remotePath;
  msg.callback = callback;
  postMessage(std::move(msg));
}

void ClipboardTransferThread::requestFiles(
    const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::vector<ClipboardTransferFileInfo> &files,
    BatchCompleteCallback callback, const std::string &destFolder
)
{
  Message msg;
  msg.type = MessageType::RequestFiles;
  msg.sourceAddr = sourceAddr;
  msg.port = port;
  msg.sessionId = sessionId;
  msg.files = files;
  msg.batchCallback = callback;
  msg.destFolder = destFolder;
  postMessage(std::move(msg));
}

bool ClipboardTransferThread::hasPendingRequests() const
{
  return m_pendingRequests.load() > 0;
}

bool ClipboardTransferThread::waitForPendingRequests(uint32_t timeoutMs)
{
  std::unique_lock<std::mutex> lock(m_pendingMutex);

  if (timeoutMs == 0) {
    m_pendingCondition.wait(lock, [this] { return m_pendingRequests.load() == 0; });
    return true;
  } else {
    return m_pendingCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
      return m_pendingRequests.load() == 0;
    });
  }
}

void ClipboardTransferThread::setPendingFilesForPaste(
    const std::vector<ClipboardTransferFileInfo> &files, const std::string &sourceAddr, uint16_t sourcePort,
    uint64_t sessionId
)
{
  std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
  m_pendingPasteFiles = files;
  m_pendingSourceAddr = sourceAddr;
  m_pendingSourcePort = sourcePort;
  m_pendingSessionId = sessionId;
  m_completedFilePaths.clear();

  LOG_INFO("[ClipboardTransfer] set %zu pending files for paste from %s:%u", files.size(), sourceAddr.c_str(), sourcePort);
}

bool ClipboardTransferThread::hasPendingFilesForPaste() const
{
  std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
  return !m_pendingPasteFiles.empty();
}

void ClipboardTransferThread::clearPendingFilesForPaste()
{
  std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
  m_pendingPasteFiles.clear();
  m_completedFilePaths.clear();
}

std::vector<std::string> ClipboardTransferThread::requestFilesAndWait(const std::string &destFolder, uint32_t timeoutMs)
{
  std::vector<ClipboardTransferFileInfo> files;
  std::string sourceAddr;
  uint16_t sourcePort = 0;
  uint64_t sessionId = 0;

  {
    std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
    if (m_pendingPasteFiles.empty()) {
      LOG_WARN("[ClipboardTransfer] no pending files to request");
      return {};
    }
    files = m_pendingPasteFiles;
    sourceAddr = m_pendingSourceAddr;
    sourcePort = m_pendingSourcePort;
    sessionId = m_pendingSessionId;
  }

  LOG_INFO("[ClipboardTransfer] requesting %zu files from %s:%u to '%s' (sessionId=%llu)",
           files.size(), sourceAddr.c_str(), sourcePort, destFolder.c_str(), sessionId);

  // Use condition variable to wait for completion
  auto completedPaths = std::make_shared<std::vector<std::string>>();
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto success = std::make_shared<std::atomic<bool>>(false);
  std::mutex waitMutex;
  std::condition_variable waitCondition;

  // Request files through the transfer thread, downloading directly to destFolder
  requestFiles(
      sourceAddr, sourcePort, sessionId, files,
      [completedPaths, done, success, &waitCondition](bool transferSuccess, const std::vector<std::string> &paths) {
        if (transferSuccess) {
          *completedPaths = paths;
          success->store(true);
        }
        done->store(true);
        waitCondition.notify_all();
      },
      destFolder
  );

  // Wait for completion
  std::unique_lock<std::mutex> lock(waitMutex);
  bool completed = waitCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [done] { return done->load(); });

  if (!completed) {
    LOG_WARN("[ClipboardTransfer] timed out after %u ms waiting for transfer from %s:%u",
             timeoutMs, sourceAddr.c_str(), sourcePort);
    return {};
  }

  if (!success->load()) {
    LOG_ERR("[ClipboardTransfer] transfer from %s:%u FAILED", sourceAddr.c_str(), sourcePort);
    return {};
  }

  LOG_INFO("[ClipboardTransfer] transfer complete, %zu file(s) received", completedPaths->size());

  // Store completed paths
  {
    std::lock_guard<std::mutex> filesLock(m_pendingFilesMutex);
    m_completedFilePaths = *completedPaths;
  }

  LOG_INFO("[ClipboardTransfer] file transfer complete, %zu files downloaded", completedPaths->size());
  return *completedPaths;
}

#ifdef _WIN32

void ClipboardTransferThread::setDelayedRenderingFiles(
    const std::vector<ClipboardTransferFileInfo> &files, const std::string &sourceAddr, uint16_t sourcePort,
    uint64_t sessionId
)
{
  Message msg;
  msg.type = MessageType::SetDelayedRendering;
  msg.files = files;
  msg.sourceAddr = sourceAddr;
  msg.port = sourcePort;
  msg.sessionId = sessionId;
  postMessage(std::move(msg));
}

void ClipboardTransferThread::handleSetDelayedRendering(const Message &msg)
{
  LOG_INFO("[ClipboardTransfer] setting delayed rendering for %zu files", msg.files.size());

  // Use the cross-platform pending files storage
  {
    std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
    m_pendingPasteFiles = msg.files;
    m_pendingSourceAddr = msg.sourceAddr;
    m_pendingSourcePort = msg.port;
    m_pendingSessionId = msg.sessionId;
    m_completedFilePaths.clear();
  }

  // Open clipboard and set delayed rendering
  if (m_clipboardWindow && OpenClipboard(m_clipboardWindow)) {
    EmptyClipboard();

    // IMPORTANT: Set "Deskflow Ownership" format so MSWindowsClipboard::isOwnedByDeskflow() returns true
    // This prevents the main thread from detecting a "clipboard grab" and interfering with delayed rendering
    static UINT ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
    if (ownershipFormat != 0) {
      // Just need to set the format with any data (or empty) - the presence of the format is what matters
      HGLOBAL hOwner = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 1);
      if (hOwner) {
        SetClipboardData(ownershipFormat, hOwner);
      }
    }

    SetClipboardData(CF_HDROP, nullptr); // Delayed rendering
    CloseClipboard();
    LOG_INFO("[ClipboardTransfer] delayed rendering activated (with ownership marker)");
  } else {
    DWORD err = GetLastError();
    LOG_ERR("[ClipboardTransfer] failed to open clipboard for delayed rendering: %lu", err);
  }
}

void ClipboardTransferThread::createClipboardWindow()
{
  const wchar_t *className = L"DeskflowClipboardTransfer";

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = clipboardWndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = className;

  if (!RegisterClassExW(&wc)) {
    DWORD error = GetLastError();
    if (error != ERROR_CLASS_ALREADY_EXISTS) {
      LOG_ERR("[ClipboardTransfer] failed to register window class: %lu", error);
      return;
    }
  }

  m_clipboardWindow = CreateWindowExW(
      0, className, L"Deskflow Clipboard Transfer", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), this
  );

  if (!m_clipboardWindow) {
    LOG_ERR("[ClipboardTransfer] failed to create clipboard window: %lu", GetLastError());
    return;
  }

  LOG_INFO("[ClipboardTransfer] clipboard window created: %p", m_clipboardWindow);
}

void ClipboardTransferThread::destroyClipboardWindow()
{
  if (m_clipboardWindow) {
    DestroyWindow(m_clipboardWindow);
    m_clipboardWindow = nullptr;
    LOG_DEBUG("[ClipboardTransfer] clipboard window destroyed");
  }
}

void *ClipboardTransferThread::getClipboardWindow() const
{
  return m_clipboardWindow;
}

LRESULT CALLBACK ClipboardTransferThread::clipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  ClipboardTransferThread *self = nullptr;

  if (msg == WM_NCCREATE) {
    CREATESTRUCT *cs = reinterpret_cast<CREATESTRUCT *>(lParam);
    self = static_cast<ClipboardTransferThread *>(cs->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<ClipboardTransferThread *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  }

  if (self) {
    return self->handleClipboardMessage(hwnd, msg, wParam, lParam);
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT ClipboardTransferThread::handleClipboardMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg) {
  case WM_RENDERFORMAT: {
    UINT format = static_cast<UINT>(wParam);
    LOG_INFO("[ClipboardTransfer] WM_RENDERFORMAT for format %u", format);

    // Use cross-platform pending files storage
    std::vector<ClipboardTransferFileInfo> pendingFiles;
    std::string sourceAddr;
    uint16_t sourcePort = 0;
    uint64_t sessionId = 0;
    bool hasCompleted = false;

    {
      std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
      pendingFiles = m_pendingPasteFiles;
      sourceAddr = m_pendingSourceAddr;
      sourcePort = m_pendingSourcePort;
      sessionId = m_pendingSessionId;
      hasCompleted = !m_completedFilePaths.empty();
    }

    if (format == CF_HDROP && !pendingFiles.empty()) {
      LOG_INFO("[ClipboardTransfer] handling delayed rendering for %zu files", pendingFiles.size());

      // Check if files already downloaded
      if (hasCompleted) {
        LOG_INFO("[ClipboardTransfer] using pre-downloaded files");
      } else {
        // Need to download files - this can block in this thread without affecting main thread
        LOG_INFO("[ClipboardTransfer] downloading files from %s:%u", sourceAddr.c_str(), sourcePort);

        // Request all files and wait
        auto completedPaths = std::make_shared<std::vector<std::string>>();
        auto done = std::make_shared<std::atomic<bool>>(false);

        requestFiles(
            sourceAddr, sourcePort, sessionId, pendingFiles,
            [completedPaths, done](bool success, const std::vector<std::string> &paths) {
              if (success) {
                *completedPaths = paths;
              }
              done->store(true);
            }
        );

        // Wait for completion (this blocks the clipboard thread, not main thread)
        while (!done->load() && !m_stopping.load()) {
          processMessages();

          // Process event queue
          IEventQueue *events = m_worker ? m_worker->getEvents() : nullptr;
          if (events) {
            Event event;
            if (events->getEvent(event, 0.01)) {
              events->dispatchEvent(event);
            }
          }
        }

        {
          std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
          m_completedFilePaths = *completedPaths;
        }
      }

      // Create CF_HDROP from completed paths
      std::vector<std::string> completedPaths;
      {
        std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
        completedPaths = m_completedFilePaths;
      }

      if (!completedPaths.empty()) {
        // Calculate required buffer size
        // DROPFILES structure + null-terminated wide strings + final null
        size_t totalSize = sizeof(DROPFILES);
        for (const auto &path : completedPaths) {
          // Convert to wide string to get length
          int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
          totalSize += wideLen * sizeof(wchar_t);
        }
        totalSize += sizeof(wchar_t); // Final null terminator

        // Allocate global memory
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize);
        if (hGlobal) {
          char *pData = static_cast<char *>(GlobalLock(hGlobal));
          if (pData) {
            // Fill DROPFILES structure
            DROPFILES *pDropFiles = reinterpret_cast<DROPFILES *>(pData);
            pDropFiles->pFiles = sizeof(DROPFILES);
            pDropFiles->pt.x = 0;
            pDropFiles->pt.y = 0;
            pDropFiles->fNC = FALSE;
            pDropFiles->fWide = TRUE; // Using wide strings

            // Write file paths
            wchar_t *pDst = reinterpret_cast<wchar_t *>(pData + sizeof(DROPFILES));
            for (const auto &path : completedPaths) {
              int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, pDst,
                  static_cast<int>((totalSize - (reinterpret_cast<char *>(pDst) - pData)) / sizeof(wchar_t)));
              pDst += wideLen; // Includes null terminator
            }
            *pDst = L'\0'; // Final null terminator

            GlobalUnlock(hGlobal);

            // Set clipboard data (clipboard takes ownership of hGlobal)
            HANDLE hResult = SetClipboardData(CF_HDROP, hGlobal);
            if (hResult) {
              LOG_INFO("[ClipboardTransfer] CF_HDROP set successfully with %zu files", completedPaths.size());
            } else {
              LOG_ERR("[ClipboardTransfer] SetClipboardData failed: %lu", GetLastError());
              GlobalFree(hGlobal);
            }
          } else {
            GlobalFree(hGlobal);
            LOG_ERR("[ClipboardTransfer] GlobalLock failed");
          }
        } else {
          LOG_ERR("[ClipboardTransfer] GlobalAlloc failed: %lu", GetLastError());
        }
      }
    }
    return 0;
  }

  case WM_RENDERALLFORMATS: {
    LOG_INFO("[ClipboardTransfer] WM_RENDERALLFORMATS");
    if (OpenClipboard(hwnd)) {
      // Render any pending formats
      std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
      if (!m_pendingPasteFiles.empty() && m_completedFilePaths.empty()) {
        // We're closing without rendering - clear delayed state
        m_pendingPasteFiles.clear();
      }
      CloseClipboard();
    }
    return 0;
  }

  case WM_DESTROYCLIPBOARD: {
    // Note: Do NOT clear pending files here!
    // The main thread may grab clipboard after we set delayed rendering,
    // which triggers WM_DESTROYCLIPBOARD. But we still need the pending files
    // for when the user pastes and we re-establish delayed rendering.
    LOG_DEBUG("[ClipboardTransfer] WM_DESTROYCLIPBOARD (pending files preserved)");
    // Only clear completed paths since they're no longer valid
    std::lock_guard<std::mutex> lock(m_pendingFilesMutex);
    m_completedFilePaths.clear();
    return 0;
  }

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

#endif // _WIN32
