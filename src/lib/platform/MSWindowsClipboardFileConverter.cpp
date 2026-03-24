/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardFileConverter.h"

#include "base/Log.h"
#include "base/Unicode.h"
#include "deskflow/FileTransfer.h"

#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <sys/stat.h>
#include <vector>

// Static members
std::vector<PendingFileInfo> MSWindowsClipboardFileConverter::s_pendingFiles;
std::vector<std::string> MSWindowsClipboardFileConverter::s_completedFilePaths;
bool MSWindowsClipboardFileConverter::s_delayedRenderingActive = false;
bool MSWindowsClipboardFileConverter::s_transferInProgress = false;
HANDLE MSWindowsClipboardFileConverter::s_transferCompleteEvent = nullptr;

namespace {

// File info structure for collecting files
struct FileInfo
{
  std::string path;
  std::string name;
  std::string relativePath; // Path relative to the root directory being copied
  uint64_t size;
  bool isDir;
};

// Escape JSON strings
std::string escapeJson(const std::string &s)
{
  std::string result;
  for (char c : s) {
    switch (c) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result += c;
    }
  }
  return result;
}

// Recursively scan directory and collect all files
void scanDirectory(
    const std::wstring &dirPath, const std::string &basePath, const std::string &relativePath,
    std::vector<FileInfo> &files
)
{
  std::wstring searchPath = dirPath + L"\\*";

  WIN32_FIND_DATAW findData;
  HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

  if (hFind == INVALID_HANDLE_VALUE) {
    LOG_DEBUG("failed to open directory: %s", basePath.c_str());
    return;
  }

  do {
    // Skip . and ..
    if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
      continue;
    }

    std::wstring fullPath = dirPath + L"\\" + findData.cFileName;

    // Convert filename to UTF-8
    std::string fileName = Unicode::UTF16ToUTF8(
        std::string(reinterpret_cast<const char *>(findData.cFileName), wcslen(findData.cFileName) * sizeof(wchar_t))
    );

    // Build relative path
    std::string itemRelativePath = relativePath.empty() ? fileName : relativePath + "/" + fileName;

    // Convert full path to UTF-8
    std::string fullPathUtf8 =
        Unicode::UTF16ToUTF8(std::string(reinterpret_cast<const char *>(fullPath.c_str()), fullPath.size() * sizeof(wchar_t)));

    FileInfo info;
    info.path = fullPathUtf8;
    info.name = fileName;
    info.relativePath = itemRelativePath;
    info.isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (info.isDir) {
      info.size = 0;
      files.push_back(info);

      // Recursively scan subdirectory
      scanDirectory(fullPath, basePath, itemRelativePath, files);
    } else {
      // Get file size
      LARGE_INTEGER fileSize;
      fileSize.LowPart = findData.nFileSizeLow;
      fileSize.HighPart = findData.nFileSizeHigh;
      info.size = static_cast<uint64_t>(fileSize.QuadPart);
      files.push_back(info);
    }
  } while (FindNextFileW(hFind, &findData));

  FindClose(hFind);
}

} // namespace

//
// MSWindowsClipboardFileConverter
//

IClipboard::Format MSWindowsClipboardFileConverter::getFormat() const
{
  return IClipboard::Format::FileList;
}

UINT MSWindowsClipboardFileConverter::getWin32Format() const
{
  return CF_HDROP;
}

HANDLE MSWindowsClipboardFileConverter::fromIClipboard(const std::string &data) const
{
  // Parse JSON array of file metadata
  // Use Windows delayed rendering mechanism:
  // 1. Parse and store file metadata as pending files
  // 2. Return nullptr to signal delayed rendering (caller will use SetClipboardData with NULL)
  // 3. When user pastes, WM_RENDERFORMAT triggers file transfer
  // 4. After files are downloaded, we provide actual CF_HDROP data

  if (data.empty()) {
    LOG_DEBUG("fromIClipboard: empty data");
    return nullptr;
  }

  LOG_INFO("fromIClipboard: parsing FileList JSON (len=%zu): %.200s", data.size(), data.c_str());

  // Parse file list JSON
  auto files = parseFileList(data);
  LOG_INFO("fromIClipboard: parseFileList returned %zu files", files.size());
  if (files.empty()) {
    LOG_WARN("fromIClipboard: no valid files in JSON, delayed rendering NOT activated");
    return nullptr;
  }

  // Store pending files for later transfer
  setPendingFiles(files);

  // Activate delayed rendering mode
  setDelayedRenderingActive(true);

  LOG_INFO("fromIClipboard: stored %zu pending files for delayed rendering", files.size());

  // Return special marker - MSWindowsClipboard::add() will check isDelayedRenderingActive()
  // and call SetClipboardData(CF_HDROP, NULL) instead of passing this handle
  return nullptr;
}

std::string MSWindowsClipboardFileConverter::toIClipboard(HANDLE data) const
{
  // Convert CF_HDROP to JSON array of file metadata
  HDROP hDrop = static_cast<HDROP>(data);
  if (hDrop == nullptr) {
    return std::string();
  }

  // Get number of top-level files/directories
  UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
  if (fileCount == 0) {
    LOG_DEBUG("CF_HDROP contains no files");
    return std::string();
  }

  LOG_DEBUG("CF_HDROP contains %u top-level items", fileCount);

  // Collect all files (including directory contents)
  std::vector<FileInfo> allFiles;

  for (UINT i = 0; i < fileCount; ++i) {
    // Get required buffer size
    UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
    if (pathLen == 0) {
      continue;
    }

    // Get file path (wide string)
    std::wstring wpath(pathLen + 1, L'\0');
    DragQueryFileW(hDrop, i, &wpath[0], pathLen + 1);
    wpath.resize(pathLen); // Remove extra null

    // Convert to UTF-8
    std::string path =
        Unicode::UTF16ToUTF8(std::string(reinterpret_cast<const char *>(wpath.c_str()), wpath.size() * sizeof(wchar_t)));

    // Get file info
    struct _stat64 fileStat;
    bool isDir = false;
    uint64_t fileSize = 0;

    if (_wstat64(wpath.c_str(), &fileStat) == 0) {
      isDir = (fileStat.st_mode & _S_IFDIR) != 0;
      fileSize = isDir ? 0 : fileStat.st_size;
    }

    // Extract file name from path
    std::string fileName = path;
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
      fileName = path.substr(lastSlash + 1);
    }

    FileInfo info;
    info.path = path;
    info.name = fileName;
    info.relativePath = fileName; // Top-level item uses just the name
    info.size = fileSize;
    info.isDir = isDir;

    allFiles.push_back(info);

    // If it's a directory, recursively scan contents
    if (isDir) {
      scanDirectory(wpath, path, fileName, allFiles);
    }

    LOG_DEBUG("item[%u]: %s (size=%llu, isDir=%d)", i, path.c_str(), fileSize, isDir);
  }

  LOG_INFO("collected %zu files/directories for transfer", allFiles.size());

  // Build JSON array
  std::ostringstream json;
  json << "[";

  bool first = true;
  for (const auto &file : allFiles) {
    if (!first) {
      json << ",";
    }
    first = false;

    json << "{";
    json << "\"path\":\"" << escapeJson(file.path) << "\",";
    json << "\"name\":\"" << escapeJson(file.name) << "\",";
    json << "\"relativePath\":\"" << escapeJson(file.relativePath) << "\",";
    json << "\"size\":" << file.size << ",";
    json << "\"isDir\":" << (file.isDir ? "true" : "false");
    json << "}";
  }

  json << "]";

  return json.str();
}

std::vector<PendingFileInfo> MSWindowsClipboardFileConverter::parseFileList(const std::string &json)
{
  std::vector<PendingFileInfo> files;

  // Simple JSON parser for file list format
  // Format: [{"path":"...","name":"...","relativePath":"...","size":123,"isDir":false},...]

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

    PendingFileInfo file;
    file.size = 0;
    file.isDir = false;

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
      if (json[pos] == '{') {
        // Nested object value (e.g., __source) - skip it entirely
        int braceCount = 1;
        pos++; // Skip opening {
        while (pos < json.size() && braceCount > 0) {
          if (json[pos] == '{') {
            braceCount++;
          } else if (json[pos] == '}') {
            braceCount--;
          } else if (json[pos] == '"') {
            // Skip string content (may contain braces)
            pos++;
            while (pos < json.size() && json[pos] != '"') {
              if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++; // Skip escaped char
              }
              pos++;
            }
          }
          pos++;
        }
        // Key like __source is skipped, continue to next key
      } else if (json[pos] == '"') {
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

void MSWindowsClipboardFileConverter::setPendingFiles(const std::vector<PendingFileInfo> &files)
{
  s_pendingFiles = files;
  LOG_INFO("stored %zu pending files for delayed rendering", files.size());
}

const std::vector<PendingFileInfo> &MSWindowsClipboardFileConverter::getPendingFiles()
{
  return s_pendingFiles;
}

bool MSWindowsClipboardFileConverter::hasPendingFiles()
{
  return !s_pendingFiles.empty();
}

void MSWindowsClipboardFileConverter::clearPendingFiles()
{
  s_pendingFiles.clear();
}

void MSWindowsClipboardFileConverter::setCompletedFilePaths(const std::vector<std::string> &paths)
{
  s_completedFilePaths = paths;
  LOG_INFO("set %zu completed file paths", paths.size());
}

const std::vector<std::string> &MSWindowsClipboardFileConverter::getCompletedFilePaths()
{
  return s_completedFilePaths;
}

bool MSWindowsClipboardFileConverter::hasCompletedFiles()
{
  return !s_completedFilePaths.empty();
}

HANDLE MSWindowsClipboardFileConverter::createHDropFromPaths(const std::vector<std::string> &paths)
{
  if (paths.empty()) {
    return nullptr;
  }

  // Convert paths to wide strings and calculate total size
  std::vector<std::wstring> widePaths;
  size_t totalChars = 0;

  for (const auto &path : paths) {
    // Convert UTF-8 to UTF-16 using Windows API
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLen > 0) {
      std::wstring wpath(wideLen - 1, L'\0'); // -1 because wideLen includes null terminator
      MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wideLen);
      widePaths.push_back(wpath);
      totalChars += wpath.size() + 1; // +1 for null terminator
    }
  }
  totalChars++; // Extra null for double-null termination

  // Calculate total size needed
  size_t totalSize = sizeof(DROPFILES) + (totalChars * sizeof(wchar_t));

  // Allocate global memory
  HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalSize);
  if (hGlobal == nullptr) {
    LOG_ERR("failed to allocate memory for CF_HDROP");
    return nullptr;
  }

  // Lock and fill the memory
  DROPFILES *pDropFiles = static_cast<DROPFILES *>(GlobalLock(hGlobal));
  if (pDropFiles == nullptr) {
    GlobalFree(hGlobal);
    return nullptr;
  }

  // Fill DROPFILES structure
  pDropFiles->pFiles = sizeof(DROPFILES);
  pDropFiles->pt.x = 0;
  pDropFiles->pt.y = 0;
  pDropFiles->fNC = FALSE;
  pDropFiles->fWide = TRUE; // Using wide chars

  // Copy file paths after DROPFILES structure
  wchar_t *pData = reinterpret_cast<wchar_t *>(reinterpret_cast<char *>(pDropFiles) + sizeof(DROPFILES));
  for (const auto &wpath : widePaths) {
    wcscpy(pData, wpath.c_str());
    pData += wpath.size() + 1;
  }
  *pData = L'\0'; // Double-null termination

  GlobalUnlock(hGlobal);

  LOG_INFO("created CF_HDROP with %zu files", paths.size());
  return hGlobal;
}

bool MSWindowsClipboardFileConverter::triggerFileTransfer()
{
  if (!hasPendingFiles()) {
    LOG_DEBUG("no pending files to transfer");
    return false;
  }

  if (!FileTransfer::hasFileRequestCallback()) {
    LOG_ERR("no file request callback registered in FileTransfer");
    return false;
  }

  LOG_INFO("triggering file transfer for %zu files (Windows delayed rendering)", s_pendingFiles.size());

  // Generate batch ID for this transfer
  uint32_t batchId = FileTransfer::generateRequestId();

  bool success = true;
  for (const auto &file : s_pendingFiles) {
    std::string relativePath = file.relativePath.empty() ? file.name : file.relativePath;
    LOG_INFO("requesting file: %s -> %s", file.path.c_str(), relativePath.c_str());

    // Empty destFolder - files go to temp directory
    uint32_t requestId = FileTransfer::requestFileForPaste(file.path, relativePath, file.isDir, batchId, "");
    if (requestId == 0) {
      LOG_ERR("failed to request file: %s", file.path.c_str());
      success = false;
    }
  }

  return success;
}

void MSWindowsClipboardFileConverter::setDelayedRenderingActive(bool active)
{
  s_delayedRenderingActive = active;
  LOG_DEBUG("delayed rendering active: %s", active ? "true" : "false");
}

bool MSWindowsClipboardFileConverter::isDelayedRenderingActive()
{
  return s_delayedRenderingActive;
}

bool MSWindowsClipboardFileConverter::isTransferInProgress()
{
  return s_transferInProgress;
}

void MSWindowsClipboardFileConverter::signalTransferComplete()
{
  LOG_INFO("signaling file transfer complete");
  s_transferInProgress = false;

  if (s_transferCompleteEvent != nullptr) {
    SetEvent(s_transferCompleteEvent);
  }
}

bool MSWindowsClipboardFileConverter::triggerFileTransferAndWait(DWORD timeoutMs)
{
  if (!hasPendingFiles()) {
    LOG_DEBUG("no pending files to transfer");
    return false;
  }

  if (!FileTransfer::hasFileRequestCallback()) {
    LOG_ERR("no file request callback registered in FileTransfer");
    return false;
  }

  // Create event if not exists
  if (s_transferCompleteEvent == nullptr) {
    s_transferCompleteEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (s_transferCompleteEvent == nullptr) {
      LOG_ERR("failed to create transfer complete event");
      return triggerFileTransfer(); // Fall back to async
    }
  }

  // Reset event and mark transfer in progress
  ResetEvent(s_transferCompleteEvent);
  s_transferInProgress = true;
  s_completedFilePaths.clear();

  LOG_INFO("triggering file transfer for %zu files and waiting for completion", s_pendingFiles.size());

  // Generate batch ID for this transfer
  uint32_t batchId = FileTransfer::generateRequestId();
  size_t expectedFiles = 0;

  for (const auto &file : s_pendingFiles) {
    std::string relativePath = file.relativePath.empty() ? file.name : file.relativePath;
    LOG_DEBUG("requesting file: %s -> %s", file.path.c_str(), relativePath.c_str());

    uint32_t requestId = FileTransfer::requestFileForPaste(file.path, relativePath, file.isDir, batchId, "");
    if (requestId != 0) {
      expectedFiles++;
    } else {
      LOG_ERR("failed to request file: %s", file.path.c_str());
    }
  }

  if (expectedFiles == 0) {
    s_transferInProgress = false;
    return false;
  }

  LOG_INFO("waiting for %zu file(s) to transfer (timeout=%lu ms)", expectedFiles, timeoutMs);

  // Wait for transfer to complete, pumping messages to keep UI responsive
  DWORD startTime = GetTickCount();
  DWORD elapsed = 0;

  while (s_transferInProgress && (timeoutMs == 0 || elapsed < timeoutMs)) {
    // Wait with message pumping
    DWORD waitResult = MsgWaitForMultipleObjects(
        1, &s_transferCompleteEvent,
        FALSE,
        timeoutMs == 0 ? INFINITE : (timeoutMs - elapsed),
        QS_ALLINPUT
    );

    if (waitResult == WAIT_OBJECT_0) {
      // Event signaled - transfer complete
      LOG_INFO("file transfer completed successfully");
      break;
    } else if (waitResult == WAIT_OBJECT_0 + 1) {
      // Messages available - pump them
      MSG msg;
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    } else if (waitResult == WAIT_TIMEOUT) {
      LOG_WARN("file transfer timed out after %lu ms", timeoutMs);
      s_transferInProgress = false;
      return false;
    } else {
      LOG_ERR("MsgWaitForMultipleObjects failed: %lu", GetLastError());
      s_transferInProgress = false;
      return false;
    }

    elapsed = GetTickCount() - startTime;
  }

  s_transferInProgress = false;

  // Check if we got the files
  bool success = hasCompletedFiles();
  LOG_INFO("file transfer wait complete: success=%d, files=%zu", success, s_completedFilePaths.size());
  return success;
}
