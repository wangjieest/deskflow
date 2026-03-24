/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboard.h"

#include "arch/ArchException.h"
#include "base/Log.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardFileConverter.h"
#include "platform/OSXClipboardHTMLConverter.h"
#include "platform/OSXClipboardTextConverter.h"
#include "platform/OSXClipboardUTF16Converter.h"
#include "platform/OSXClipboardUTF8Converter.h"
#include "platform/OSXPasteboardPeeker.h"

// Static members
bool OSXClipboard::s_promiseKeeperSet = false;

//
// OSXClipboard
//

OSXClipboard::OSXClipboard() : m_time(0), m_pboard(nullptr)
{
  m_converters.push_back(new OSXClipboardHTMLConverter);
  m_converters.push_back(new OSXClipboardBMPConverter);
  m_converters.push_back(new OSXClipboardUTF8Converter);
  m_converters.push_back(new OSXClipboardUTF16Converter);
  m_converters.push_back(new OSXClipboardTextConverter);
  m_converters.push_back(new OSXClipboardFileConverter);

  OSStatus createErr = PasteboardCreate(kPasteboardClipboard, &m_pboard);
  if (createErr != noErr) {
    LOG_WARN("failed to create clipboard reference: error %i", createErr);
    LOG_ERR("unable to connect to pasteboard, clipboard sharing disabled", createErr);
    m_pboard = nullptr;
    return;
  }

  OSStatus syncErr = PasteboardSynchronize(m_pboard);
  if (syncErr != noErr) {
    LOG_WARN("failed to syncronize clipboard: error %i", syncErr);
  }
}

OSXClipboard::~OSXClipboard()
{
  clearConverters();
}

bool OSXClipboard::empty()
{
  LOG_DEBUG("emptying clipboard");
  if (m_pboard == nullptr)
    return false;

  OSStatus err = PasteboardClear(m_pboard);
  if (err != noErr) {
    LOG_WARN("failed to clear clipboard: error %i", err);
    return false;
  }

  return true;
}

bool OSXClipboard::synchronize()
{
  if (m_pboard == nullptr)
    return false;

  PasteboardSyncFlags flags = PasteboardSynchronize(m_pboard);
  LOG_DEBUG2("flags: %x", flags);

  if (flags & kPasteboardModified) {
    return true;
  }
  return false;
}

void OSXClipboard::add(Format format, const std::string &data)
{
  if (m_pboard == nullptr)
    return;

  LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
  if (format == IClipboard::Format::Text) {
    LOG_DEBUG("format of data to be added to clipboard was kText");
  } else if (format == IClipboard::Format::Bitmap) {
    LOG_DEBUG("format of data to be added to clipboard was kBitmap");
  } else if (format == IClipboard::Format::HTML) {
    LOG_DEBUG("format of data to be added to clipboard was kHTML");
  } else if (format == IClipboard::Format::FileList) {
    LOG_DEBUG("format of data to be added to clipboard was kFileList");
    // Use special handling for file lists with promise mechanism
    addFilePromise(data);
    return;
  }

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    IOSXClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      std::string osXData = converter->fromIClipboard(data);
      CFStringRef flavorType = converter->getOSXFormat();
      CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (uint8_t *)osXData.data(), osXData.size());
      PasteboardItemID itemID = 0;

      if (dataRef) {
        PasteboardPutItemFlavor(m_pboard, itemID, flavorType, dataRef, kPasteboardFlavorNoFlags);

        CFRelease(dataRef);
        LOG_DEBUG("added %d bytes to clipboard format: %d", data.size(), format);
      }
    }
  }
}

void OSXClipboard::addFilePromise(const std::string &data)
{
  // Parse the file list JSON and store pending files
  auto files = OSXClipboardFileConverter::parseFileList(data);
  if (files.empty()) {
    LOG_DEBUG("no files in file list, skipping");
    return;
  }

  OSXClipboardFileConverter::setPendingFiles(files);
  LOG_INFO("macOS: stored %zu pending files for lazy transfer (Promise mechanism)", files.size());

  // Set destination folder
  OSXClipboardFileConverter::setDestinationFolder("/tmp/autodeskflow-pending");

  // Register promise keeper callback
  if (!s_promiseKeeperSet) {
    OSStatus err = PasteboardSetPromiseKeeper(m_pboard, promiseKeeperCallback, this);
    if (err == noErr) {
      s_promiseKeeperSet = true;
      LOG_INFO("✅ Promise keeper registered");
    } else {
      LOG_WARN("Failed to register promise keeper: %d", err);
    }
  }

  // DON'T clear clipboard - we need existing items
  // Clear would delete all items and make itemID invalid

  // Get first item (or create if empty)
  PasteboardItemID itemID;
  OSStatus getItemErr = PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &itemID);

  if (getItemErr != noErr) {
    // No items exist, need to add data first to create an item
    // Add a dummy flavor to create the item, then add promise
    LOG_DEBUG("No existing item, creating with dummy data");

    // Add a text representation first (creates the item)
    std::string fileNames;
    for (const auto &file : files) {
      if (!fileNames.empty()) fileNames += "\n";
      fileNames += file.name + " (pending transfer)";
    }

    CFDataRef textData = CFDataCreate(kCFAllocatorDefault, (uint8_t*)fileNames.c_str(), fileNames.size());
    if (textData) {
      // This creates item ID 1
      OSStatus textErr = PasteboardPutItemFlavor(m_pboard, (PasteboardItemID)1, kUTTypeUTF8PlainText, textData, kPasteboardFlavorNoFlags);
      CFRelease(textData);

      if (textErr == noErr) {
        itemID = (PasteboardItemID)1;
        LOG_DEBUG("Item created with text flavor");
      } else {
        LOG_WARN("Failed to create item: %d - files stored as pending, will transfer on paste", textErr);
        // Don't trigger immediate transfer - files are stored as pending
        // and will be transferred when user actually pastes
        return;
      }
    }
  } else {
    LOG_DEBUG("Using existing item ID: %p", itemID);
  }

  // Now add promise flavor to the existing item
  CFDataRef emptyData = CFDataCreate(kCFAllocatorDefault, nullptr, 0);
  OSStatus err = PasteboardPutItemFlavor(
      m_pboard, itemID,
      kPasteboardTypeFileURLPromise,
      emptyData,
      kPasteboardFlavorPromised | kPasteboardFlavorSenderOnly);
  CFRelease(emptyData);

  if (err == noErr) {
    LOG_INFO("✅ Promise added successfully - NO transfer yet, callback on paste");
  } else {
    LOG_WARN("❌ Promise mechanism failed (error %d) - files stored as pending, will transfer on paste", err);
    // Don't trigger immediate transfer here either - files are already stored as pending
    // The transfer will happen when user actually pastes through alternative mechanism
  }
}

OSStatus OSXClipboard::promiseKeeperCallback(
    PasteboardRef pasteboard, PasteboardItemID item,
    CFStringRef flavorType, void *context)
{
  LOG_INFO("Promise keeper callback INVOKED - USER IS PASTING!");

  // Check if this is file URL promise request
  if (CFStringCompare(flavorType, kPasteboardTypeFileURLPromise, 0) != kCFCompareEqualTo) {
    LOG_WARN("Promise keeper called for non-file flavor");
    return noErr;
  }

  // Check if files are already transferred
  if (OSXClipboardFileConverter::hasCompletedFiles()) {
    LOG_INFO("Files already transferred, providing file URLs");
    // Files are ready, we could provide the file URLs here
    // For now, just return success
    return noErr;
  }

  // NOW trigger file transfer (lazy - only when user pastes)
  // Use Downloads folder as destination
  std::string destFolder = std::string(getenv("HOME")) + "/Downloads";
  OSXClipboardFileConverter::setDestinationFolder(destFolder);

  LOG_INFO("Starting synchronous file transfer (triggered by paste operation)");
  LOG_INFO("Destination: %s", destFolder.c_str());

  // Use synchronous transfer and wait for completion
  // This blocks until files are received or timeout occurs (60 seconds)
  if (!OSXClipboardFileConverter::triggerFileTransferAndWait(destFolder, 60000)) {
    LOG_ERR("File transfer failed or timed out in promise callback");
    return badPasteboardFlavorErr;
  }

  // Transfer completed successfully
  const auto &completedFiles = OSXClipboardFileConverter::getCompletedFilePaths();
  LOG_INFO("File transfer completed, %zu files received:", completedFiles.size());
  for (size_t i = 0; i < completedFiles.size(); ++i) {
    LOG_INFO("  completed[%zu]: %s", i, completedFiles[i].c_str());
  }

  // Update the pasteboard with actual file URLs
  // This allows the paste operation to succeed with the real files
  if (!completedFiles.empty()) {
    std::vector<const char*> cPaths;
    cPaths.reserve(completedFiles.size());
    for (const auto& path : completedFiles) {
      cPaths.push_back(path.c_str());
    }
    updatePasteboardWithFiles(cPaths.data(), static_cast<int>(cPaths.size()));
  }

  LOG_INFO("File transfer completed successfully from promise callback");
  return noErr;
}

bool OSXClipboard::open(Time time) const
{
  if (m_pboard == nullptr)
    return false;

  LOG_DEBUG("opening clipboard");
  m_time = time;
  return true;
}

void OSXClipboard::close() const
{
  LOG_DEBUG("closing clipboard");
  /* not needed */
}

IClipboard::Time OSXClipboard::getTime() const
{
  return m_time;
}

bool OSXClipboard::has(Format format) const
{
  if (m_pboard == nullptr)
    return false;

  PasteboardItemID item;
  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IOSXClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      PasteboardFlavorFlags flags;
      CFStringRef type = converter->getOSXFormat();

      OSStatus res;

      if ((res = PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags)) == noErr) {
        return true;
      }
    }
  }

  return false;
}

std::string OSXClipboard::get(Format format) const
{
  CFStringRef type;
  PasteboardItemID item;
  std::string result;

  if (m_pboard == nullptr)
    return result;

  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  // find the converter for the first clipboard format we can handle
  IOSXClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    converter = *index;

    PasteboardFlavorFlags flags;
    type = converter->getOSXFormat();

    if (converter->getFormat() == format && PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags) == noErr) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_DEBUG("unable to find converter for data");
    return result;
  }

  // get the clipboard data.
  CFDataRef buffer = nullptr;
  try {
    OSStatus err = PasteboardCopyItemFlavorData(m_pboard, item, type, &buffer);

    if (err != noErr) {
      throw err;
    }

    result = std::string((char *)CFDataGetBytePtr(buffer), CFDataGetLength(buffer));
  } catch (OSStatus err) {
    LOG_DEBUG("exception thrown in OSXClipboard::get MacError (%d)", err);
  } catch (...) {
    LOG_DEBUG("unknown exception in OSXClipboard::get");
    RETHROW_THREADEXCEPTION
  }

  if (buffer != nullptr)
    CFRelease(buffer);

  return converter->toIClipboard(result);
}

void OSXClipboard::clearConverters()
{
  if (m_pboard == nullptr)
    return;

  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}
