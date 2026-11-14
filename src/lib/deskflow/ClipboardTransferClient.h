/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/FileTransfer.h"

#include <functional>
#include <string>

/**
 * @brief Callback for file transfer completion
 * @param success True if transfer completed successfully
 * @param localPath Path to downloaded file
 * @param errorMessage Error message if failed
 */
using FileTransferCallback = std::function<void(bool success, const std::string &localPath, const std::string &errorMessage)>;

/**
 * @brief Client-side clipboard transfer connection
 *
 * Downloads files point-to-point from the source machine's ClipboardTransferServer.
 * Holds no per-download state, so downloads may outlive the object.
 */
class ClipboardTransferClient
{
public:
  ClipboardTransferClient() = default;
  ~ClipboardTransferClient() = default;

  ClipboardTransferClient(const ClipboardTransferClient &) = delete;
  ClipboardTransferClient &operator=(const ClipboardTransferClient &) = delete;

  /**
   * @brief Request a file from remote source
   *
   * @param sourceAddr IP address of source machine
   * @param port Port of source machine's transfer server
   * @param sessionId Session ID for validation
   * @param remotePath Path of file on source machine
   * @param callback Callback when transfer completes
   */
  void requestFile(
      const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
      FileTransferCallback callback
  );

  struct DownloadResult
  {
    bool success = false;
    std::string localPath;
    std::string error;
  };

  /**
   * @brief Download one entry into destDir, blocking the calling thread
   *
   * Touches no member state, so detached threads may call it after the client is gone.
   * The entry lands at destDir/<sanitized relative path from the source>.
   *
   * @param keepExisting When true an existing file is kept and the download gets a "name 2.ext" style name
   */
  static DownloadResult downloadFile(
      const std::string &sourceAddr, uint16_t port, uint64_t sessionId, const std::string &remotePath,
      const std::string &destDir, bool keepExisting
  );

  /**
   * @brief Folder that downloads without an explicit destination go to (created on demand)
   */
  std::string getTempDirectory() const;
};
