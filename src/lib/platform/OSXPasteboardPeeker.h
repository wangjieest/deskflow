/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#import <CoreFoundation/CoreFoundation.h>

#if defined(__cplusplus)
extern "C"
{
#endif

  CFStringRef getDraggedFileURL();

  //! Write file URLs to the general pasteboard
  /*!
  Updates the macOS clipboard with actual file URLs so that
  paste operations work correctly in Finder and other apps.
  \param filePaths array of file paths to add to clipboard
  \param count number of file paths
  */
  void updatePasteboardWithFiles(const char **filePaths, int count);

  //! Get all file paths from the general pasteboard
  /*!
  Reads all file URLs from the macOS clipboard and returns them as
  a JSON array suitable for network transfer.
  \return JSON string with file metadata, or empty string if no files
  */
  const char* getClipboardFilesAsJson(void);

  //! Free the JSON string returned by getClipboardFilesAsJson
  void freeClipboardFilesJson(const char* json);

#if defined(__cplusplus)
}
#endif
