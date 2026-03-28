/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardMeta.h"
#include "deskflow/FileTransfer.h"
#include "deskflow/ClipboardTransferWorker.h"

#include <QThread>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

class IEventQueue;
class SocketMultiplexer;
class ISocketFactory;
class ClipboardTransferServer;
class ClipboardTransferClient;

/**
 * @brief Callback for data received from remote
 * @param success True if transfer completed successfully
 * @param localPath Path to downloaded file (or empty on failure)
 * @param errorMessage Error message if failed
 */
using DataReceivedCallback = std::function<void(bool success, const std::string &localPath, const std::string &errorMessage)>;

/**
 * @brief Callback for all files in a batch completed
 * @param success True if all files transferred successfully
 * @param localPaths Paths to all downloaded files
 */
using BatchCompleteCallback = std::function<void(bool success, const std::vector<std::string> &localPaths)>;

/**
 * @brief File info for transfer
 */
struct ClipboardTransferFileInfo
{
  std::string path;         //!< Full path on source machine
  std::string relativePath; //!< Relative path for directory structure
  uint64_t size = 0;        //!< File size in bytes
  bool isDir = false;       //!< True if this is a directory
};

/**
 * @brief Independent thread for clipboard data transfer
 *
 * Handles file and large data transfers in a separate thread to avoid
 * blocking the main Deskflow event loop. This ensures mouse/keyboard
 * handling remains responsive during file transfers.
 *
 * Features:
 * - Independent event loop and socket multiplexer
 * - ClipboardTransferServer for serving files when this machine is copy source
 * - ClipboardTransferClient for downloading files when this machine is paste target
 * - Windows: Owns the clipboard window for delayed rendering
 *
 * Small clipboard data (text < threshold) still goes through the main thread
 * for efficiency.
 */
class ClipboardTransferThread
{
public:
  //! Size threshold for using transfer thread (1MB)
  static constexpr size_t kLargeDataThreshold = 1024 * 1024;

  ClipboardTransferThread();
  ~ClipboardTransferThread();

  ClipboardTransferThread(const ClipboardTransferThread &) = delete;
  ClipboardTransferThread &operator=(const ClipboardTransferThread &) = delete;

  /**
   * @brief Start the transfer thread
   * @return true if started successfully
   */
  bool start();

  /**
   * @brief Stop the transfer thread
   *
   * Waits for pending transfers to complete (with timeout), then stops.
   */
  void stop();

  /**
   * @brief Check if thread is running
   */
  bool isRunning() const
  {
    return m_running.load();
  }

  //
  // Server-side (copy source) API
  //

  /**
   * @brief Set files available for transfer (called when this machine copies)
   *
   * Thread-safe. Can be called from main thread.
   *
   * @param sessionId Unique session identifier
   * @param files List of files available for transfer
   */
  void setAvailableFiles(uint64_t sessionId, const std::vector<ClipboardTransferFileInfo> &files);

  /**
   * @brief Clear files for a session
   * @param sessionId Session to clear
   */
  void clearSession(uint64_t sessionId);

  /**
   * @brief Get the server's listening port
   * @return Port number, or 0 if not running
   */
  uint16_t getServerPort() const;

  /**
   * @brief Get the local IP address for point-to-point transfer
   * @return IP address string
   */
  std::string getLocalAddress() const;

  //
  // Client-side (paste target) API
  //

  /**
   * @brief Request a file from remote source
   *
   * Thread-safe. Can be called from main thread.
   * The callback will be invoked on the transfer thread.
   *
   * @param sourceAddr IP address of source machine
   * @param port Port of source machine's transfer server
   * @param sessionId Session ID for validation
   * @param remotePath Path of file on source machine
   * @param callback Callback when transfer completes
   */
  void requestFile(
      const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
      DataReceivedCallback callback
  );

  /**
   * @brief Request multiple files from remote source
   *
   * Thread-safe. Can be called from main thread.
   *
   * @param sourceAddr IP address of source machine
   * @param port Port of source machine's transfer server
   * @param sessionId Session ID for validation
   * @param files List of files to request
   * @param callback Callback when all transfers complete
   */
  void requestFiles(
      const std::string &sourceAddr, uint16_t port, uint64_t sessionId,
      const std::vector<ClipboardTransferFileInfo> &files, BatchCompleteCallback callback,
      const std::string &destFolder = ""
  );

  /**
   * @brief Check if there are pending file requests
   */
  bool hasPendingRequests() const;

  /**
   * @brief Wait for all pending requests to complete
   * @param timeoutMs Maximum time to wait (0 = infinite)
   * @return true if all completed, false if timeout
   */
  bool waitForPendingRequests(uint32_t timeoutMs);

  //
  // Platform-specific clipboard integration
  //

  /**
   * @brief Set pending files for deferred paste
   *
   * Called from main thread when clipboard meta is received.
   * On Windows: The transfer thread will handle WM_RENDERFORMAT.
   * On macOS: Call requestFilesAndWait() when paste is triggered.
   *
   * @param files Files available for paste
   * @param sourceAddr Source machine address
   * @param sourcePort Source machine port
   * @param sessionId Session ID
   */
  void setPendingFilesForPaste(
      const std::vector<ClipboardTransferFileInfo> &files, const std::string &sourceAddr, uint16_t sourcePort,
      uint64_t sessionId
  );

  /**
   * @brief Request files and wait for completion (blocking)
   *
   * This is for macOS paste operation where we need to synchronously
   * wait for files to be downloaded before returning to the system.
   * The actual transfer runs in the transfer thread, but this method
   * blocks the calling thread until completion.
   *
   * @param destFolder Destination folder for downloaded files
   * @param timeoutMs Timeout in milliseconds (0 = infinite)
   * @return Paths to downloaded files (empty on failure)
   */
  std::vector<std::string> requestFilesAndWait(const std::string &destFolder, uint32_t timeoutMs = 60000);

  /**
   * @brief Check if there are pending files for paste
   */
  bool hasPendingFilesForPaste() const;

  /**
   * @brief Clear pending files
   */
  void clearPendingFilesForPaste();

#ifdef _WIN32
  /**
   * @brief Set pending files for delayed rendering (Windows-specific)
   *
   * Wrapper for setPendingFilesForPaste that also sets up
   * delayed rendering in the clipboard window.
   */
  void setDelayedRenderingFiles(
      const std::vector<ClipboardTransferFileInfo> &files, const std::string &sourceAddr, uint16_t sourcePort,
      uint64_t sessionId
  );

  /**
   * @brief Get the clipboard window handle
   *
   * This window is created in the transfer thread and handles
   * WM_RENDERFORMAT messages.
   */
  void *getClipboardWindow() const;
#endif

private:
  //
  // Internal message types for thread communication
  //
  enum class MessageType
  {
    Stop,
    SetAvailableFiles,
    ClearSession,
    RequestFile,
    RequestFiles,
#ifdef _WIN32
    SetDelayedRendering,
#endif
  };

  struct Message
  {
    MessageType type;
    uint64_t sessionId = 0;
    std::string sourceAddr;
    uint16_t port = 0;
    std::string remotePath;
    std::string destFolder;
    std::vector<ClipboardTransferFileInfo> files;
    DataReceivedCallback callback;
    BatchCompleteCallback batchCallback;
  };

  // Process messages from queue
  void processMessages();

  // Message handlers
  void handleSetAvailableFiles(const Message &msg);
  void handleClearSession(const Message &msg);
  void handleRequestFile(const Message &msg);
  void handleRequestFiles(const Message &msg);

#ifdef _WIN32
  void handleSetDelayedRendering(const Message &msg);
  void createClipboardWindow();
  void destroyClipboardWindow();
  static LRESULT CALLBACK clipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT handleClipboardMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

  // Post message to transfer thread
  void postMessage(Message &&msg);

  // Friend class for Worker access
  friend class ClipboardTransferWorker;

  // Thread state
  QThread *m_thread = nullptr;
  ClipboardTransferWorker *m_worker = nullptr;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stopping{false};

  // Message queue
  std::queue<Message> m_messageQueue;
  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCondition;

  // Note: Transfer components (IEventQueue, SocketMultiplexer, ISocketFactory,
  // ClipboardTransferServer, ClipboardTransferClient) are now owned by Worker

  // Pending request tracking
  std::atomic<int> m_pendingRequests{0};
  mutable std::mutex m_pendingMutex;
  std::condition_variable m_pendingCondition;

  // Pending files for paste (cross-platform)
  std::vector<ClipboardTransferFileInfo> m_pendingPasteFiles;
  std::string m_pendingSourceAddr;
  uint16_t m_pendingSourcePort = 0;
  uint64_t m_pendingSessionId = 0;
  std::vector<std::string> m_completedFilePaths;
  mutable std::mutex m_pendingFilesMutex;

#ifdef _WIN32
  // Windows clipboard window
  HWND m_clipboardWindow = nullptr;
#endif
};
