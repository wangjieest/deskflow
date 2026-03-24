/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboardFileConverter.h"

#include "base/Log.h"
#include "deskflow/ClipboardTransferThread.h"
#include "deskflow/FileTransfer.h"
#include "platform/OSXPasteboardPeeker.h"

#include <CoreFoundation/CoreFoundation.h>
#include <ctime>
#include <sstream>

// Static members
std::vector<FileMetadata> OSXClipboardFileConverter::s_pendingFiles;
std::string OSXClipboardFileConverter::s_destinationFolder;
ClipboardTransferThread *OSXClipboardFileConverter::s_transferThread = nullptr;

IClipboard::Format OSXClipboardFileConverter::getFormat() const
{
  return IClipboard::Format::FileList;
}

CFStringRef OSXClipboardFileConverter::getOSXFormat() const
{
  // Use the standard file URL pasteboard type
  // This is the type used when files are copied in Finder
  return kUTTypeFileURL;
}

std::string OSXClipboardFileConverter::fromIClipboard(const std::string &data) const
{
  // Convert from our JSON format to macOS format
  // For now, we store the file metadata and return a placeholder
  // The actual file transfer happens when the paste is triggered

  auto files = parseFileList(data);
  if (files.empty()) {
    LOG_DEBUG("no files to convert for clipboard");
    return std::string();
  }

  // Store files for later retrieval
  setPendingFiles(files);

  LOG_INFO("stored %zu pending files for clipboard transfer", files.size());
  for (const auto &file : files) {
    LOG_DEBUG("pending file: %s (size=%llu, isDir=%d)", file.name.c_str(), file.size, file.isDir);
  }

  // Return a marker string that indicates files are pending
  // The actual paste handling will check for pending files
  return "deskflow-pending-files";
}

std::string OSXClipboardFileConverter::toIClipboard(const std::string &data) const
{
  // Convert from macOS file URL format to our JSON format
  // This is called when Mac is the source machine (user copies files on Mac)

  // The 'data' parameter contains raw pasteboard data for a single item,
  // but we need to read ALL file URLs from the clipboard.
  // Use the Objective-C helper function to get all files as JSON.

  const char* json = getClipboardFilesAsJson();
  if (json == nullptr) {
    LOG_DEBUG("toIClipboard: no files found on clipboard");
    return std::string();
  }

  std::string result(json);
  freeClipboardFilesJson(json);

  LOG_INFO("toIClipboard: converted macOS clipboard to JSON (%zu bytes)", result.size());
  return result;
}

std::vector<FileMetadata> OSXClipboardFileConverter::parseFileList(const std::string &json)
{
  std::vector<FileMetadata> files;

  // Simple JSON parser for our file list format
  // Format: [{"path":"...","name":"...","size":123,"isDir":false},...]

  if (json.empty() || json[0] != '[') {
    LOG_DEBUG("invalid file list JSON: doesn't start with [");
    return files;
  }

  // Parse each object in the array
  size_t pos = 1; // Skip opening [
  while (pos < json.size()) {
    // Skip whitespace and commas
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\n' || json[pos] == '\r')) {
      pos++;
    }

    if (pos >= json.size() || json[pos] == ']') {
      break;
    }

    if (json[pos] != '{') {
      LOG_DEBUG("expected { at position %zu", pos);
      break;
    }

    FileMetadata file;
    file.size = 0;
    file.isDir = false;
    file.relativePath = "";

    // Parse object
    pos++; // Skip {
    while (pos < json.size() && json[pos] != '}') {
      // Skip whitespace
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
      }

      if (json[pos] == '}') {
        break;
      }

      // Parse key
      if (json[pos] != '"') {
        break;
      }
      pos++; // Skip opening "

      std::string key;
      while (pos < json.size() && json[pos] != '"') {
        key += json[pos++];
      }
      pos++; // Skip closing "

      // Skip :
      while (pos < json.size() && json[pos] != ':') {
        pos++;
      }
      pos++; // Skip :

      // Skip whitespace
      while (pos < json.size() && json[pos] == ' ') {
        pos++;
      }

      // Parse value
      if (json[pos] == '"') {
        // String value
        pos++; // Skip opening "
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
          if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
            case 'n':
              value += '\n';
              break;
            case 'r':
              value += '\r';
              break;
            case 't':
              value += '\t';
              break;
            case '\\':
              value += '\\';
              break;
            case '"':
              value += '"';
              break;
            default:
              value += json[pos];
            }
          } else {
            value += json[pos];
          }
          pos++;
        }
        pos++; // Skip closing "

        if (key == "path") {
          file.path = value;
        } else if (key == "name") {
          file.name = value;
        } else if (key == "relativePath") {
          file.relativePath = value;
        }
      } else if (json[pos] == 't' || json[pos] == 'f') {
        // Boolean value
        bool boolValue = (json[pos] == 't');
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}') {
          pos++;
        }
        if (key == "isDir") {
          file.isDir = boolValue;
        }
      } else if (json[pos] >= '0' && json[pos] <= '9') {
        // Number value
        uint64_t numValue = 0;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
          numValue = numValue * 10 + (json[pos] - '0');
          pos++;
        }
        if (key == "size") {
          file.size = numValue;
        }
      }
    }

    if (json[pos] == '}') {
      pos++; // Skip closing }
    }

    if (!file.path.empty() && !file.name.empty()) {
      files.push_back(file);
    }
  }

  return files;
}

void OSXClipboardFileConverter::setPendingFiles(const std::vector<FileMetadata> &files)
{
  s_pendingFiles = files;
}

const std::vector<FileMetadata> &OSXClipboardFileConverter::getPendingFiles()
{
  return s_pendingFiles;
}

bool OSXClipboardFileConverter::hasPendingFiles()
{
  return !s_pendingFiles.empty();
}

void OSXClipboardFileConverter::clearPendingFiles()
{
  s_pendingFiles.clear();
}

bool OSXClipboardFileConverter::triggerFileTransfer(const std::string &destFolder)
{
  LOG_DEBUG("[FileTrigger] triggerFileTransfer called, destFolder: %s", destFolder.c_str());

  if (!hasPendingFiles()) {
    LOG_WARN("[FileTrigger] No pending files to transfer");
    return false;
  }

  LOG_INFO("[FileTrigger] Has %zu pending files", s_pendingFiles.size());

  // Check if already triggered - avoid duplicate transfers for same clipboard event
  // This can happen when both clipboard 0 and 1 are updated
  static uint32_t s_lastBatchId = 0;
  static int64_t s_lastTriggerTime = 0;
  int64_t now = std::time(nullptr);

  // If triggered within 1 second, it's likely a duplicate
  if (now - s_lastTriggerTime < 1) {
    LOG_WARN("[FileTrigger] Duplicate trigger detected (within 1 second), skipping to avoid double transfer");
    return true;  // Return true to not show error
  }

  s_lastTriggerTime = now;

  if (!FileTransfer::hasFileRequestCallback()) {
    LOG_ERR("[FileTrigger] CRITICAL: No file request callback registered in FileTransfer!");
    LOG_ERR("[FileTrigger] This means the Client instance hasn't set up the callback");
    return false;
  }

  LOG_INFO("[FileTrigger] ✅ File request callback is registered");
  LOG_INFO("[FileTrigger] Triggering file transfer for %zu files to: %s", s_pendingFiles.size(), destFolder.c_str());

  // Generate batch ID for this transfer
  uint32_t batchId = FileTransfer::generateRequestId();

  bool success = true;
  for (size_t i = 0; i < s_pendingFiles.size(); i++) {
    const auto &file = s_pendingFiles[i];
    std::string relativePath = file.relativePath.empty() ? file.name : file.relativePath;

    LOG_INFO("[FileTrigger] Requesting file %zu/%zu: path=%s, relativePath=%s, isDir=%d, size=%llu",
             i + 1, s_pendingFiles.size(), file.path.c_str(), relativePath.c_str(), file.isDir, file.size);

    uint32_t requestId =
        FileTransfer::requestFileForPaste(file.path, relativePath, file.isDir, batchId, destFolder);

    if (requestId == 0) {
      LOG_ERR("[FileTrigger] ✗ Failed to request file: %s", file.path.c_str());
      success = false;
    } else {
      LOG_INFO("[FileTrigger] ✓ File request sent, requestId=%u", requestId);
    }
  }

  LOG_INFO("[FileTrigger] Transfer trigger complete, success=%d", success);
  return success;
}

void OSXClipboardFileConverter::setDestinationFolder(const std::string &folder)
{
  s_destinationFolder = folder;
  LOG_DEBUG("destination folder set to: %s", folder.c_str());
}

const std::string &OSXClipboardFileConverter::getDestinationFolder()
{
  return s_destinationFolder;
}

// Static member definitions for synchronous wait mechanism
std::vector<std::string> OSXClipboardFileConverter::s_completedFilePaths;
bool OSXClipboardFileConverter::s_transferInProgress = false;
bool OSXClipboardFileConverter::s_transferComplete = false;

bool OSXClipboardFileConverter::triggerFileTransferAndWait(const std::string &destFolder, uint32_t timeoutMs)
{
  // First try to use ClipboardTransferThread for point-to-point transfer
  if (s_transferThread && s_transferThread->isRunning() && s_transferThread->hasPendingFilesForPaste()) {
    LOG_INFO("[FileTrigger] using ClipboardTransferThread for point-to-point transfer");

    // Use the transfer thread's blocking wait mechanism
    std::vector<std::string> completedPaths = s_transferThread->requestFilesAndWait(destFolder, timeoutMs);

    if (!completedPaths.empty()) {
      s_completedFilePaths = completedPaths;
      s_transferComplete = true;
      LOG_INFO("[FileTrigger] point-to-point transfer completed, %zu files ready", completedPaths.size());
      return true;
    } else {
      LOG_WARN("[FileTrigger] point-to-point transfer failed or timed out, trying fallback");
      // Fall through to legacy transfer
    }
  }

  // Fallback to legacy FileTransfer callback mechanism
  if (!hasPendingFiles()) {
    LOG_DEBUG("[FileTrigger] no pending files to transfer");
    return false;
  }

  if (!FileTransfer::hasFileRequestCallback()) {
    LOG_ERR("[FileTrigger] no file request callback registered in FileTransfer");
    return false;
  }

  // Reset state
  s_transferInProgress = true;
  s_transferComplete = false;
  s_completedFilePaths.clear();

  LOG_INFO("[FileTrigger] triggering file transfer (legacy) and waiting for %zu files (timeout=%u ms)",
           s_pendingFiles.size(), timeoutMs);

  // Generate batch ID for this transfer
  uint32_t batchId = FileTransfer::generateRequestId();
  size_t expectedFiles = 0;

  for (const auto &file : s_pendingFiles) {
    std::string relativePath = file.relativePath.empty() ? file.name : file.relativePath;
    LOG_DEBUG("[FileTrigger] requesting file: %s -> %s", file.path.c_str(), relativePath.c_str());

    uint32_t requestId = FileTransfer::requestFileForPaste(file.path, relativePath, file.isDir, batchId, destFolder);
    if (requestId != 0) {
      expectedFiles++;
    } else {
      LOG_ERR("[FileTrigger] failed to request file: %s", file.path.c_str());
    }
  }

  if (expectedFiles == 0) {
    s_transferInProgress = false;
    return false;
  }

  LOG_INFO("[FileTrigger] waiting for %zu file(s) to transfer", expectedFiles);

  // Wait for transfer to complete using run loop
  // This keeps the system responsive while waiting
  uint32_t elapsed = 0;
  const uint32_t pollInterval = 100; // 100ms

  while (s_transferInProgress && !s_transferComplete && (timeoutMs == 0 || elapsed < timeoutMs)) {
    // Run the run loop briefly to allow network events to be processed
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, pollInterval / 1000.0, false);
    elapsed += pollInterval;

    // Check every second
    if (elapsed % 1000 == 0) {
      LOG_DEBUG("[FileTrigger] still waiting... elapsed=%u ms, completed=%zu files",
                elapsed, s_completedFilePaths.size());
    }
  }

  s_transferInProgress = false;

  if (s_transferComplete && hasCompletedFiles()) {
    LOG_INFO("[FileTrigger] file transfer completed successfully, %zu files ready",
             s_completedFilePaths.size());
    return true;
  } else if (elapsed >= timeoutMs && timeoutMs > 0) {
    LOG_WARN("[FileTrigger] file transfer timed out after %u ms", timeoutMs);
    return false;
  } else {
    LOG_ERR("[FileTrigger] file transfer failed");
    return false;
  }
}

void OSXClipboardFileConverter::signalTransferComplete()
{
  LOG_INFO("[FileTrigger] signaling file transfer complete");
  s_transferComplete = true;
  s_transferInProgress = false;
}

bool OSXClipboardFileConverter::isTransferInProgress()
{
  return s_transferInProgress;
}

void OSXClipboardFileConverter::setCompletedFilePaths(const std::vector<std::string> &paths)
{
  s_completedFilePaths = paths;
  LOG_INFO("[FileTrigger] set %zu completed file paths", paths.size());
}

const std::vector<std::string> &OSXClipboardFileConverter::getCompletedFilePaths()
{
  return s_completedFilePaths;
}

bool OSXClipboardFileConverter::hasCompletedFiles()
{
  return !s_completedFilePaths.empty();
}

void OSXClipboardFileConverter::setClipboardTransferThread(ClipboardTransferThread *thread)
{
  s_transferThread = thread;
  LOG_DEBUG("ClipboardTransferThread set for OSXClipboardFileConverter: %p", thread);
}

ClipboardTransferThread *OSXClipboardFileConverter::getClipboardTransferThread()
{
  return s_transferThread;
}
