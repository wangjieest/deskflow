/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <functional>
#include <string>

/// Bridge between the main Deskflow process and the Finder Sync Extension.
///
/// All communication uses NSDistributedNotificationCenter (Mach IPC),
/// no files involved:
///   Main app -> Extension:  "org.deskflow.clipboardUpdate"  (file metadata)
///   Extension -> Main app:  "org.deskflow.pasteRequest"     (target dir)
///   Extension -> Main app:  "org.deskflow.requestClipboardState" (cold start)
class OSXPasteboardBridge
{
public:
  using PasteCallback = std::function<void(const std::string &targetDirectory)>;

  /// Start listening for paste requests from the Finder extension.
  static void startListening(PasteCallback callback);

  /// Stop listening.
  static void stopListening();

  /// Broadcast pending file metadata via notification so the extension
  /// can show its menu item. Pure in-memory IPC.
  static void publishPendingFiles(const std::string &filesJson, int fileCount);

  /// Broadcast that there are no more pending files.
  static void clearPendingFiles();
};
