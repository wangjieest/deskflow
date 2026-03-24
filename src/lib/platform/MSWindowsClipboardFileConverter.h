/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "platform/MSWindowsClipboard.h"

#include <string>
#include <vector>

//! File metadata for pending transfer
struct PendingFileInfo
{
  std::string path;         // Full path on source machine
  std::string name;         // File name only
  std::string relativePath; // Relative path for directory structure
  uint64_t size;            // File size in bytes
  bool isDir;               // Is directory
};

//! Convert to/from file list (CF_HDROP) format
/*!
This converter handles the CF_HDROP clipboard format which contains
a list of file paths. The data is converted to/from a JSON array
containing file metadata for transfer between machines.

For incoming file lists (from remote machine), it uses Windows delayed
rendering mechanism where actual file content is pulled when the user pastes.
*/
class MSWindowsClipboardFileConverter : public IMSWindowsClipboardConverter
{
public:
  MSWindowsClipboardFileConverter() = default;
  ~MSWindowsClipboardFileConverter() override = default;

  // IMSWindowsClipboardConverter overrides
  IClipboard::Format getFormat() const override;
  UINT getWin32Format() const override;
  HANDLE fromIClipboard(const std::string &) const override;
  std::string toIClipboard(HANDLE) const override;

  //! Parse file metadata from JSON
  static std::vector<PendingFileInfo> parseFileList(const std::string &json);

  //! Store pending files for delayed rendering paste
  static void setPendingFiles(const std::vector<PendingFileInfo> &files);

  //! Get pending files
  static const std::vector<PendingFileInfo> &getPendingFiles();

  //! Check if there are pending files
  static bool hasPendingFiles();

  //! Clear pending files
  static void clearPendingFiles();

  //! Set completed file paths (after transfer finishes)
  static void setCompletedFilePaths(const std::vector<std::string> &paths);

  //! Get completed file paths
  static const std::vector<std::string> &getCompletedFilePaths();

  //! Check if there are completed files ready
  static bool hasCompletedFiles();

  //! Create CF_HDROP handle from completed file paths
  static HANDLE createHDropFromPaths(const std::vector<std::string> &paths);

  //! Trigger file transfer for pending files
  /*!
  Called when WM_RENDERFORMAT is received.
  Uses the FileRequestCallback registered in FileTransfer to request files.
  \return true if transfer was initiated
  */
  static bool triggerFileTransfer();

  //! Trigger file transfer and wait for completion
  /*!
  Called when WM_RENDERFORMAT is received. Initiates file transfer and
  blocks until all files are received or timeout occurs.
  Uses Windows message pumping to keep the UI responsive while waiting.
  \param timeoutMs maximum time to wait in milliseconds (0 = no timeout)
  \return true if all files were transferred successfully
  */
  static bool triggerFileTransferAndWait(DWORD timeoutMs = 30000);

  //! Signal that transfer is complete (called from network thread)
  static void signalTransferComplete();

  //! Set the flag indicating delayed rendering is active
  static void setDelayedRenderingActive(bool active);

  //! Check if delayed rendering is active
  static bool isDelayedRenderingActive();

  //! Check if transfer is currently in progress
  static bool isTransferInProgress();

private:
  static std::vector<PendingFileInfo> s_pendingFiles;
  static std::vector<std::string> s_completedFilePaths;
  static bool s_delayedRenderingActive;
  static bool s_transferInProgress;
  static HANDLE s_transferCompleteEvent;
};
