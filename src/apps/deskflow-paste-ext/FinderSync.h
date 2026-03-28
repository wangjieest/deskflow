/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#import <Cocoa/Cocoa.h>
#import <FinderSync/FinderSync.h>

/// Finder Sync Extension that provides a "Deskflow Paste" context menu item.
///
/// Communication with main Deskflow process:
///   Primary:  Unix domain socket at /tmp/autodeskflow-paste.sock
///   Fallback: NSDistributedNotificationCenter
///
/// The extension queries the main app for pending file state via socket
/// (GET_STATE), and sends paste requests via socket (PASTE:<dir>) or
/// notification. Debug state is also written to clipboard-state.json.
@interface DeskflowFinderSync : FIFinderSync

- (BOOL)hasPendingFiles;
- (NSUInteger)pendingFileCount;

@end
