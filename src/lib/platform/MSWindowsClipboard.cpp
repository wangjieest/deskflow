/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboard.h"

#include "base/Log.h"
#include "deskflow/ClipboardTransferClient.h"
#include "deskflow/IClipboard.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardFacade.h"
#include "platform/MSWindowsClipboardFileConverter.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"
#include "platform/MSWindowsDataObject.h"

#include <objbase.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

//
// MSWindowsClipboard
//

UINT MSWindowsClipboard::s_ownershipFormat = 0;

MSWindowsClipboard::MSWindowsClipboard(HWND window)
    : m_window(window),
      m_time(0),
      m_facade(new MSWindowsClipboardFacade()),
      m_deleteFacade(true),
      m_transferClient(nullptr)
{
  // Initialize OLE for IDataObject support
  OleInitialize(nullptr);

  // add converters, most desired first
  m_converters.push_back(new MSWindowsClipboardUTF16Converter);
  m_converters.push_back(new MSWindowsClipboardBitmapConverter);
  m_converters.push_back(new MSWindowsClipboardHTMLConverter);
  m_converters.push_back(new MSWindowsClipboardFileConverter);
}

MSWindowsClipboard::~MSWindowsClipboard()
{
  clearConverters();

  // dependency injection causes confusion over ownership, so we need
  // logic to decide whether or not we delete the facade. there must
  // be a more elegant way of doing this.
  if (m_deleteFacade)
    delete m_facade;

  // Uninitialize OLE
  OleUninitialize();
}

void MSWindowsClipboard::setFacade(IMSWindowsClipboardFacade &facade)
{
  delete m_facade;
  m_facade = &facade;
  m_deleteFacade = false;
}

bool MSWindowsClipboard::emptyUnowned()
{
  LOG_DEBUG("empty clipboard");

  // empty the clipboard (and take ownership)
  if (!EmptyClipboard()) {
    // unable to cause this in integ tests, but this error has never
    // actually been reported by users.
    LOG_WARN("failed to grab clipboard");
    return false;
  }

  return true;
}

bool MSWindowsClipboard::empty()
{
  if (!emptyUnowned()) {
    return false;
  }

  // mark clipboard as being owned by deskflow
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 1);
  if (nullptr == SetClipboardData(getOwnershipFormat(), data)) {
    LOG_WARN("failed to set clipboard data");
    GlobalFree(data);
    return false;
  }

  return true;
}

void MSWindowsClipboard::add(Format format, const std::string &data)
{
  // exit early if there is no data to prevent spurious "failed to convert clipboard data" errors
  if (data.empty()) {
    LOG_DEBUG("not adding 0 bytes to clipboard format: %d", format);
    return;
  }

  // Special handling for FileList format
  if (format == Format::FileList) {
    // If ClipboardTransferThread already set up delayed rendering, skip entirely.
    // Re-adding FileList here would conflict and cause clipboard ownership feedback loops.
    if (MSWindowsClipboardFileConverter::isDelayedRenderingActive()) {
      LOG_DEBUG("skipping FileList add - delayed rendering already active via ClipboardTransferThread");
      return;
    }
    if (addFileListAsIDataObject(data)) {
      LOG_INFO("Successfully added file list using IDataObject streaming");
      return;
    }
    LOG_WARN("Failed to add file list as IDataObject, falling back to standard conversion");
  }

  // TODO: Special handling for Text format using IDataObject delayed rendering
  // Temporarily disabled for compilation - needs setTextData() implementation in MSWindowsDataObject
  /*
  if (format == Format::Text) {
    // Use IDataObject for large text or when transfer client is available
    if (data.size() > 10240 || m_transferClient != nullptr) { // > 10KB
      if (addTextAsIDataObject(data)) {
        LOG_INFO("Successfully added text using IDataObject delayed rendering");
        return;
      }
      LOG_DEBUG("Failed to add text as IDataObject, falling back to standard conversion");
    }
  }

  // TODO: Special handling for HTML format using IDataObject delayed rendering
  // Temporarily disabled - Format::Html enum value not defined
  if (format == Format::Html) {
    // Use IDataObject for large HTML or when transfer client is available
    if (data.size() > 10240 || m_transferClient != nullptr) { // > 10KB
      if (addHtmlAsIDataObject(data)) {
        LOG_INFO("Successfully added HTML using IDataObject delayed rendering");
        return;
      }
      LOG_DEBUG("Failed to add HTML as IDataObject, falling back to standard conversion");
    }
  }
  */

  bool isSucceeded = false;
  // convert data to win32 form
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      HANDLE win32Data = converter->fromIClipboard(data);

      // Check if this is a FileList format using delayed rendering
      if (format == Format::FileList && MSWindowsClipboardFileConverter::isDelayedRenderingActive()) {
        // Use Windows delayed rendering mechanism:
        // SetClipboardData with NULL handle tells Windows to send WM_RENDERFORMAT
        // when an application actually requests this data (e.g., user pastes)
        LOG_INFO("using delayed rendering for CF_HDROP (file list)");
        if (SetClipboardData(CF_HDROP, nullptr) != nullptr) {
          isSucceeded = true;
        } else {
          LOG_ERR("failed to set delayed rendering for CF_HDROP: %d", GetLastError());
        }
        break;
      }

      if (win32Data != nullptr) {
        LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
        m_facade->write(win32Data, converter->getWin32Format());
        isSucceeded = true;
        break;
      } else {
        LOG_DEBUG("failed to convert clipboard data to platform format");
      }
    }
  }

  if (!isSucceeded) {
    LOG_DEBUG("missed clipboard data convert for format: %d", format);
  }
}

bool MSWindowsClipboard::addFileListAsIDataObject(const std::string &jsonData)
{
  try {
    // Parse JSON file list
    auto json = nlohmann::json::parse(jsonData);

    if (!json.is_array() || json.empty()) {
      LOG_ERR("Invalid file list JSON: not an array or empty");
      return false;
    }

    // Extract P2P connection info from __source entry
    std::string sourceAddr;
    uint16_t sourcePort = 0;
    uint64_t sessionId = 0;

    for (const auto &item : json) {
      if (item.contains("__source")) {
        const auto &src = item["__source"];
        if (src.contains("address")) {
          sourceAddr = src["address"].get<std::string>();
        }
        if (src.contains("port")) {
          sourcePort = src["port"].get<uint16_t>();
        }
        if (src.contains("sessionId")) {
          sessionId = src["sessionId"].get<uint64_t>();
        }
        break;
      }
    }

    // Check if we have P2P info
    if (sourceAddr.empty() || sourcePort == 0) {
      LOG_WARN("File list missing P2P connection info - cannot use IDataObject streaming");
      return false;
    }

    // Convert to FileMetadata
    std::vector<FileMetadata> files;
    for (const auto &item : json) {
      FileMetadata meta;

      // Convert name to wide string
      if (item.contains("name")) {
        std::string name = item["name"].get<std::string>();
        meta.name = std::wstring(name.begin(), name.end());
      }

      // Relative path for subdirectories
      if (item.contains("relativePath")) {
        std::string relPath = item["relativePath"].get<std::string>();
        meta.relativePath = std::wstring(relPath.begin(), relPath.end());
      }

      // Remote path
      if (item.contains("path")) {
        meta.remotePath = item["path"].get<std::string>();
      }

      // Size
      if (item.contains("size")) {
        meta.size = item["size"].get<uint64_t>();
      }

      // Is directory
      if (item.contains("isDir")) {
        meta.isDir = item["isDir"].get<bool>();
      }

      // P2P info
      meta.sourceAddr = sourceAddr;
      meta.sourcePort = sourcePort;
      meta.sessionId = sessionId;

      files.push_back(meta);
    }

    LOG_INFO("Creating IDataObject for %zu files from %s:%u", files.size(), sourceAddr.c_str(), sourcePort);

    // Create IDataObject
    MSWindowsDataObject *pDataObject = new MSWindowsDataObject(files, m_transferClient);

    // Set to clipboard using OleSetClipboard
    HRESULT hr = OleSetClipboard(pDataObject);

    // Release our reference (OleSetClipboard adds its own)
    pDataObject->Release();

    if (FAILED(hr)) {
      LOG_ERR("OleSetClipboard failed: 0x%08X", hr);
      return false;
    }

    LOG_INFO("Successfully set IDataObject to clipboard");
    return true;

  } catch (const std::exception &e) {
    LOG_ERR("Exception parsing file list JSON: %s", e.what());
    return false;
  }
}

bool MSWindowsClipboard::open(Time time) const
{
  LOG_DEBUG("open clipboard");

  if (!OpenClipboard(m_window)) {
    LOG_WARN("failed to open clipboard: %d", GetLastError());
    return false;
  }

  m_time = time;

  return true;
}

void MSWindowsClipboard::close() const
{
  LOG_DEBUG("close clipboard");
  CloseClipboard();
}

IClipboard::Time MSWindowsClipboard::getTime() const
{
  return m_time;
}

bool MSWindowsClipboard::has(Format format) const
{
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      if (IsClipboardFormatAvailable(converter->getWin32Format())) {
        return true;
      }
    }
  }
  return false;
}

std::string MSWindowsClipboard::get(Format format) const
{
  // find the converter for the first clipboard format we can handle
  IMSWindowsClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    converter = *index;
    if (converter->getFormat() == format) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_WARN("no converter for format %d", format);
    return std::string();
  }

  // get a handle to the clipboard data
  HANDLE win32Data = GetClipboardData(converter->getWin32Format());
  if (win32Data == nullptr) {
    // nb: can't cause this using integ tests; this is only caused when
    // the selected converter returns an invalid format -- which you
    // cannot cause using public functions.
    return std::string();
  }

  // convert
  return converter->toIClipboard(win32Data);
}

void MSWindowsClipboard::clearConverters()
{
  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}

bool MSWindowsClipboard::isOwnedByDeskflow()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
  }
  return (IsClipboardFormatAvailable(getOwnershipFormat()) != 0);
}

UINT MSWindowsClipboard::getOwnershipFormat()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
  }

  // return the format
  return s_ownershipFormat;
}

// TODO: Temporarily disabled - needs setTextData() implementation
/*
bool MSWindowsClipboard::addTextAsIDataObject(const std::string &text)
{
  try {
    LOG_INFO("Adding text using IDataObject delayed rendering (%zu bytes)", text.size());

    // Create empty file list
    std::vector<FileMetadata> empty;
    MSWindowsDataObject *pDataObject = new MSWindowsDataObject(empty, m_transferClient);

    // Set text data for delayed rendering
    pDataObject->setTextData(text);

    // Set to clipboard
    HRESULT hr = OleSetClipboard(pDataObject);

    // Release our reference (OleSetClipboard adds its own)
    pDataObject->Release();

    if (FAILED(hr)) {
      LOG_ERR("OleSetClipboard failed for text: 0x%08X", hr);
      return false;
    }

    LOG_INFO("Successfully set text to clipboard using IDataObject");
    return true;

  } catch (const std::exception &e) {
    LOG_ERR("Exception in addTextAsIDataObject: %s", e.what());
    return false;
  }
}

bool MSWindowsClipboard::addHtmlAsIDataObject(const std::string &html)
{
  try {
    LOG_INFO("Adding HTML using IDataObject delayed rendering (%zu bytes)", html.size());

    // Create empty file list
    std::vector<FileMetadata> empty;
    MSWindowsDataObject *pDataObject = new MSWindowsDataObject(empty, m_transferClient);

    // Set HTML as text data (could also convert to RTF)
    pDataObject->setTextData(html);

    // If we have RTF converter, could also set RTF:
    // std::string rtf = convertHtmlToRtf(html);
    // pDataObject->setRtfData(rtf);

    // Set to clipboard
    HRESULT hr = OleSetClipboard(pDataObject);

    // Release our reference
    pDataObject->Release();

    if (FAILED(hr)) {
      LOG_ERR("OleSetClipboard failed for HTML: 0x%08X", hr);
      return false;
    }

    LOG_INFO("Successfully set HTML to clipboard using IDataObject");
    return true;

  } catch (const std::exception &e) {
    LOG_ERR("Exception in addHtmlAsIDataObject: %s", e.what());
    return false;
  }
}
*/

// Temporary stub implementations
bool MSWindowsClipboard::addTextAsIDataObject(const std::string &text)
{
  LOG_DEBUG("addTextAsIDataObject temporarily disabled");
  return false;
}

bool MSWindowsClipboard::addHtmlAsIDataObject(const std::string &html)
{
  LOG_DEBUG("addHtmlAsIDataObject temporarily disabled");
  return false;
}
