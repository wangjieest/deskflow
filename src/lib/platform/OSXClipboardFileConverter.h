/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "platform/OSXClipboard.h"

#include <string>
#include <vector>

class ClipboardTransferThread;

//! File metadata for transfer
struct FileMetadata
{
  std::string path;         // Full path on source machine
  std::string name;         // File name only
  std::string relativePath; // Relative path for directory structure
  uint64_t size;            // File size in bytes
  bool isDir;               // Is directory
};

//! Convert to/from file list format
/*!
This converter handles the file list clipboard format. It converts between
JSON file metadata and macOS file URL pasteboard format.

For incoming file lists (from remote machine), it sets up a promise-based
mechanism where actual file content is pulled when the user pastes.
*/
class OSXClipboardFileConverter : public IOSXClipboardConverter
{
public:
  OSXClipboardFileConverter() = default;
  ~OSXClipboardFileConverter() override = default;

  // IOSXClipboardConverter overrides
  IClipboard::Format getFormat() const override;
  CFStringRef getOSXFormat() const override;
  std::string fromIClipboard(const std::string &) const override;
  std::string toIClipboard(const std::string &) const override;

  //! Parse file metadata from JSON
  static std::vector<FileMetadata> parseFileList(const std::string &json);

  //! Store pending files for promise-based paste
  static void setPendingFiles(const std::vector<FileMetadata> &files);

  //! Get pending files
  static const std::vector<FileMetadata> &getPendingFiles();

  //! Check if there are pending files
  static bool hasPendingFiles();

  //! Clear pending files
  static void clearPendingFiles();

  //! Trigger file transfer for pending files to specified destination
  /*!
  Uses the FileRequestCallback registered in FileTransfer to request files.
  */
  static bool triggerFileTransfer(const std::string &destFolder);

  //! Trigger file transfer and wait for completion
  /*!
  Called from the promise keeper callback. Initiates file transfer and
  blocks until all files are received or timeout occurs.
  Uses a run loop to keep the system responsive while waiting.
  \param destFolder destination folder for files
  \param timeoutMs maximum time to wait in milliseconds (0 = no timeout)
  \return true if all files were transferred successfully
  */
  static bool triggerFileTransferAndWait(const std::string &destFolder, uint32_t timeoutMs = 60000);

  //! Signal that transfer is complete (called from network thread)
  static void signalTransferComplete();

  //! Check if transfer is currently in progress
  static bool isTransferInProgress();

  //! Set the destination folder for paste operation
  static void setDestinationFolder(const std::string &folder);

  //! Get the destination folder
  static const std::string &getDestinationFolder();

  //! Set completed file paths (after transfer finishes)
  static void setCompletedFilePaths(const std::vector<std::string> &paths);

  //! Get completed file paths
  static const std::vector<std::string> &getCompletedFilePaths();

  //! Check if there are completed files ready
  static bool hasCompletedFiles();

  //! Set ClipboardTransferThread for point-to-point transfer
  /*!
  This should be called by Client when it initializes the transfer thread.
  */
  static void setClipboardTransferThread(ClipboardTransferThread *thread);

  //! Get ClipboardTransferThread
  static ClipboardTransferThread *getClipboardTransferThread();

private:
  static std::vector<FileMetadata> s_pendingFiles;
  static std::string s_destinationFolder;
  static std::vector<std::string> s_completedFilePaths;
  static bool s_transferInProgress;
  static bool s_transferComplete;
  static ClipboardTransferThread *s_transferThread;
};
