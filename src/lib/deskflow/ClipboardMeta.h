/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

//! Clipboard metadata for deferred/lazy transfer
/*!
This structure contains metadata about clipboard content, allowing clients
to request specific data on-demand rather than receiving everything immediately.

This enables:
1. Deferred file transfer - files are only transferred when user actually pastes
2. Validation - server can verify requests match current clipboard state
3. Bandwidth optimization - large data only transferred when needed
4. Security - server maintains whitelist of allowed paths per session
*/
struct ClipboardMeta
{
  uint64_t sessionId = 0;
  uint32_t contentType = 0;
  uint64_t totalSize = 0;
  uint32_t itemCount = 0;
  std::string checksum;
  std::string metadata;
  bool deferred = false;
  std::string sourceAddress;
  uint16_t sourcePort = 0;

  //! Serialize to string for network transfer
  std::string serialize() const
  {
    std::ostringstream ss;
    ss << "{";
    ss << "\"sessionId\":" << sessionId << ",";
    ss << "\"contentType\":" << contentType << ",";
    ss << "\"totalSize\":" << totalSize << ",";
    ss << "\"itemCount\":" << itemCount << ",";
    ss << "\"checksum\":\"" << escapeJson(checksum) << "\",";
    ss << "\"deferred\":" << (deferred ? "true" : "false") << ",";
    ss << "\"sourceAddress\":\"" << escapeJson(sourceAddress) << "\",";
    ss << "\"sourcePort\":" << sourcePort << ",";
    ss << "\"metadata\":" << (metadata.empty() ? "null" : metadata);
    ss << "}";
    return ss.str();
  }

  static ClipboardMeta deserialize(const std::string &json);
  static ClipboardMeta createForFileList(uint64_t sessionId, const std::string &fileListJson,
                                         uint64_t totalSize, uint32_t fileCount);
  static ClipboardMeta createForText(uint64_t sessionId, uint64_t textSize,
                                      const std::string &preview = "");
  static ClipboardMeta createForBitmap(uint64_t sessionId, uint64_t imageSize,
                                        uint32_t width = 0, uint32_t height = 0);
  static ClipboardMeta createForHtml(uint64_t sessionId, uint64_t htmlSize,
                                      const std::string &preview = "");

private:
  static std::string escapeJson(const std::string &s)
  {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n";  break;
      case '\r': result += "\\r";  break;
      case '\t': result += "\\t";  break;
      default:   result += c;
      }
    }
    return result;
  }
};

//! Data request with session validation
struct ClipboardDataRequest
{
  uint64_t sessionId = 0;
  uint32_t contentType = 0;
  std::string filePath;
  std::string relativePath;
  bool isDir = false;
  uint32_t batchId = 0;

  std::string serialize() const
  {
    std::ostringstream ss;
    ss << "{";
    ss << "\"sessionId\":" << sessionId << ",";
    ss << "\"contentType\":" << contentType << ",";
    ss << "\"filePath\":\"" << escapeJson(filePath) << "\",";
    ss << "\"relativePath\":\"" << escapeJson(relativePath) << "\",";
    ss << "\"isDir\":" << (isDir ? "true" : "false") << ",";
    ss << "\"batchId\":" << batchId;
    ss << "}";
    return ss.str();
  }

  static ClipboardDataRequest deserialize(const std::string &json);

private:
  static std::string escapeJson(const std::string &s)
  {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':  result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n";  break;
      case '\r': result += "\\r";  break;
      case '\t': result += "\\t";  break;
      default:   result += c;
      }
    }
    return result;
  }
};

//! Data response status
enum class ClipboardDataStatus : uint8_t
{
  Success = 0,
  SessionExpired = 1,
  InvalidPath = 2,
  FileNotFound = 3,
  AccessDenied = 4,
  TransferError = 5,
  ContentTooLarge = 6,
  UnsupportedFormat = 7,
};

inline const char *clipboardDataStatusToString(ClipboardDataStatus status)
{
  switch (status) {
  case ClipboardDataStatus::Success:           return "Success";
  case ClipboardDataStatus::SessionExpired:    return "SessionExpired";
  case ClipboardDataStatus::InvalidPath:       return "InvalidPath";
  case ClipboardDataStatus::FileNotFound:      return "FileNotFound";
  case ClipboardDataStatus::AccessDenied:      return "AccessDenied";
  case ClipboardDataStatus::TransferError:     return "TransferError";
  case ClipboardDataStatus::ContentTooLarge:   return "ContentTooLarge";
  case ClipboardDataStatus::UnsupportedFormat: return "UnsupportedFormat";
  default: return "Unknown";
  }
}
