/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardMeta.h"

#include "base/Log.h"

#include <sstream>

namespace {

std::string extractStringField(const std::string &json, const std::string &fieldName)
{
  std::string searchStr = "\"" + fieldName + "\":\"";
  size_t pos = json.find(searchStr);
  if (pos == std::string::npos) {
    return "";
  }
  pos += searchStr.size();
  size_t endPos = pos;
  while (endPos < json.size() && json[endPos] != '"') {
    if (json[endPos] == '\\' && endPos + 1 < json.size()) {
      endPos += 2;
    } else {
      endPos++;
    }
  }

  std::string value;
  for (size_t i = pos; i < endPos; ++i) {
    if (json[i] == '\\' && i + 1 < endPos) {
      ++i;
      switch (json[i]) {
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      default:  value += json[i];
      }
    } else {
      value += json[i];
    }
  }
  return value;
}

uint64_t extractUint64Field(const std::string &json, const std::string &fieldName)
{
  std::string searchStr = "\"" + fieldName + "\":";
  size_t pos = json.find(searchStr);
  if (pos == std::string::npos) {
    return 0;
  }
  pos += searchStr.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
    pos++;
  }
  uint64_t value = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    value = value * 10 + (json[pos] - '0');
    pos++;
  }
  return value;
}

bool extractBoolField(const std::string &json, const std::string &fieldName)
{
  std::string searchStr = "\"" + fieldName + "\":";
  size_t pos = json.find(searchStr);
  if (pos == std::string::npos) {
    return false;
  }
  pos += searchStr.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
    pos++;
  }
  return (pos < json.size() && json[pos] == 't');
}

std::string extractJsonValue(const std::string &json, const std::string &fieldName)
{
  std::string searchStr = "\"" + fieldName + "\":";
  size_t pos = json.find(searchStr);
  if (pos == std::string::npos) {
    return "";
  }
  pos += searchStr.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
    pos++;
  }
  if (pos >= json.size()) {
    return "";
  }
  if (json.substr(pos, 4) == "null") {
    return "";
  }
  if (json[pos] == '"') {
    return extractStringField(json, fieldName);
  }

  char openBracket = json[pos];
  char closeBracket = (openBracket == '{') ? '}' : (openBracket == '[') ? ']' : '\0';

  if (closeBracket == '\0') {
    size_t endPos = pos;
    while (endPos < json.size() && json[endPos] != ',' && json[endPos] != '}' && json[endPos] != ']') {
      endPos++;
    }
    return json.substr(pos, endPos - pos);
  }

  size_t depth = 1;
  size_t endPos = pos + 1;
  while (endPos < json.size() && depth > 0) {
    if (json[endPos] == openBracket) {
      depth++;
    } else if (json[endPos] == closeBracket) {
      depth--;
    } else if (json[endPos] == '"') {
      endPos++;
      while (endPos < json.size() && json[endPos] != '"') {
        if (json[endPos] == '\\') {
          endPos++;
        }
        endPos++;
      }
    }
    endPos++;
  }

  return json.substr(pos, endPos - pos);
}

} // namespace

ClipboardMeta ClipboardMeta::deserialize(const std::string &json)
{
  ClipboardMeta meta;

  if (json.empty() || json[0] != '{') {
    LOG_DEBUG("ClipboardMeta::deserialize: invalid JSON");
    return meta;
  }

  meta.sessionId = extractUint64Field(json, "sessionId");
  meta.contentType = static_cast<uint32_t>(extractUint64Field(json, "contentType"));
  meta.totalSize = extractUint64Field(json, "totalSize");
  meta.itemCount = static_cast<uint32_t>(extractUint64Field(json, "itemCount"));
  meta.checksum = extractStringField(json, "checksum");
  meta.deferred = extractBoolField(json, "deferred");
  meta.sourceAddress = extractStringField(json, "sourceAddress");
  meta.sourcePort = static_cast<uint16_t>(extractUint64Field(json, "sourcePort"));
  meta.metadata = extractJsonValue(json, "metadata");

  return meta;
}

ClipboardMeta ClipboardMeta::createForFileList(
    uint64_t sessionId, const std::string &fileListJson,
    uint64_t totalSize, uint32_t fileCount)
{
  ClipboardMeta meta;
  meta.sessionId = sessionId;
  meta.contentType = static_cast<uint32_t>(IClipboard::Format::FileList);
  meta.totalSize = totalSize;
  meta.itemCount = fileCount;
  meta.metadata = fileListJson;
  return meta;
}

ClipboardMeta ClipboardMeta::createForText(
    uint64_t sessionId, uint64_t textSize, const std::string &preview)
{
  ClipboardMeta meta;
  meta.sessionId = sessionId;
  meta.contentType = static_cast<uint32_t>(IClipboard::Format::Text);
  meta.totalSize = textSize;
  meta.itemCount = 1;
  if (!preview.empty()) {
    std::ostringstream ss;
    ss << "{\"preview\":\"" << escapeJson(preview) << "\"}";
    meta.metadata = ss.str();
  }
  return meta;
}

ClipboardMeta ClipboardMeta::createForBitmap(
    uint64_t sessionId, uint64_t imageSize, uint32_t width, uint32_t height)
{
  ClipboardMeta meta;
  meta.sessionId = sessionId;
  meta.contentType = static_cast<uint32_t>(IClipboard::Format::Bitmap);
  meta.totalSize = imageSize;
  meta.itemCount = 1;
  if (width > 0 && height > 0) {
    std::ostringstream ss;
    ss << "{\"width\":" << width << ",\"height\":" << height << "}";
    meta.metadata = ss.str();
  }
  return meta;
}

ClipboardMeta ClipboardMeta::createForHtml(
    uint64_t sessionId, uint64_t htmlSize, const std::string &preview)
{
  ClipboardMeta meta;
  meta.sessionId = sessionId;
  meta.contentType = static_cast<uint32_t>(IClipboard::Format::HTML);
  meta.totalSize = htmlSize;
  meta.itemCount = 1;
  if (!preview.empty()) {
    std::ostringstream ss;
    ss << "{\"preview\":\"" << escapeJson(preview) << "\"}";
    meta.metadata = ss.str();
  }
  return meta;
}

ClipboardDataRequest ClipboardDataRequest::deserialize(const std::string &json)
{
  ClipboardDataRequest request;

  if (json.empty() || json[0] != '{') {
    LOG_DEBUG("ClipboardDataRequest::deserialize: invalid JSON");
    return request;
  }

  request.sessionId = extractUint64Field(json, "sessionId");
  request.contentType = static_cast<uint32_t>(extractUint64Field(json, "contentType"));
  request.filePath = extractStringField(json, "filePath");
  request.relativePath = extractStringField(json, "relativePath");
  request.isDir = extractBoolField(json, "isDir");
  request.batchId = static_cast<uint32_t>(extractUint64Field(json, "batchId"));

  return request;
}
