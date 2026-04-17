/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#import "FinderSync.h"
#include <os/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static NSString *const kClipboardUpdateNotification = @"org.deskflow.clipboardUpdate";
static NSString *const kPasteRequestNotification = @"org.deskflow.pasteRequest";
static NSString *const kRequestStateNotification = @"org.deskflow.requestClipboardState";
static const char *kSocketPath = "/tmp/autodeskflow-paste.sock";

static os_log_t gExtLog;

static void extLog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void extLog(NSString *fmt, ...) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{ gExtLog = os_log_create("org.autodeskflow.paste-ext", "paste"); });
  va_list args;
  va_start(args, fmt);
  NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:args];
  va_end(args);
  os_log(gExtLog, "%{public}s", msg.UTF8String);
  NSLog(@"[AutoDeskflowExt] %@", msg);
}

@implementation DeskflowFinderSync {
  NSImage *_toolbarIcon;
  NSDictionary *_cachedState;
  NSDate *_lastStateCheck;
}

#pragma mark - Lifecycle

- (instancetype)init {
  self = [super init];
  if (self) {
    [FIFinderSyncController defaultController].directoryURLs =
        [NSSet setWithObject:[NSURL fileURLWithPath:@"/"]];

    _toolbarIcon = [NSImage imageNamed:NSImageNameNetwork];

    // Listen for clipboard updates from main app
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(handleClipboardUpdate:)
               name:kClipboardUpdateNotification
             object:nil];

    // Request current state from main app on cold start
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kRequestStateNotification
                      object:nil
                    userInfo:nil
         deliverImmediately:YES];

    extLog(@"[Ext] initialized, monitoring /");
  }
  return self;
}

// ARC automatically inserts [super dealloc]; suppress the false-positive warning.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-missing-super-calls"
- (void)dealloc {
  [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
}
#pragma clang diagnostic pop

#pragma mark - Toolbar Item (FIFinderSync protocol)

- (NSString *)toolbarItemName {
  return @"Deskflow Paste";
}

- (NSImage *)toolbarItemImage {
  return _toolbarIcon;
}

- (NSString *)toolbarItemToolTip {
  return @"Paste files from remote Deskflow machine";
}

#pragma mark - Notification Handler

- (void)handleClipboardUpdate:(NSNotification *)note {
  _cachedState = note.userInfo;
  _lastStateCheck = [NSDate date];
}

- (NSString *)sourceLabel {
  NSDictionary *state = [self loadClipboardState];
  NSString *source = state[@"source"];
  if (source.length > 0) return source;
  return @"remote";
}

#pragma mark - Socket Communication

- (NSDictionary *)queryStateViaSocket {
  // Fast path: if socket file doesn't exist, main app isn't running
  if (access(kSocketPath, F_OK) != 0) {
    return nil;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return nil;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return nil;
  }

  const char *cmd = "GET_STATE";
  write(fd, cmd, strlen(cmd));

  // Short read timeout - this should be very fast for local socket
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000; // 100ms
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char buf[8192];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);

  if (n <= 0) return nil;
  buf[n] = '\0';

  NSData *data = [NSData dataWithBytes:buf length:n];
  NSError *error = nil;
  NSDictionary *state = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  if (error) {
    extLog(@"[Ext] socket state parse error: %@", error);
    return nil;
  }
  return state;
}

- (BOOL)sendPasteViaSocket:(NSString *)targetPath {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    extLog(@"[Ext] sendPaste: socket() failed errno=%d", errno);
    return NO;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    extLog(@"[Ext] sendPaste: connect() failed errno=%d", errno);
    close(fd);
    return NO;
  }

  NSString *cmd = [NSString stringWithFormat:@"PASTE:%@", targetPath];
  const char *cmdStr = [cmd UTF8String];
  ssize_t written = write(fd, cmdStr, strlen(cmdStr));
  extLog(@"[Ext] sendPaste: sent %zd bytes cmd=%@", written, cmd);

  // Wait for ack (main app replies OK after accepting the request)
  char buf[16] = {0};
  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  extLog(@"[Ext] sendPaste: ack read=%zd content='%s'", n, buf);
  close(fd);

  return YES;
}

#pragma mark - State Query

- (NSDictionary *)loadClipboardState {
  // Use notification-pushed cache if fresh (< 2s old)
  // This avoids any IPC on right-click in the common case
  if (_cachedState && _lastStateCheck &&
      [[NSDate date] timeIntervalSinceDate:_lastStateCheck] < 2.0) {
    return _cachedState;
  }

  // Cache stale or missing: query via socket for fresh state
  NSDictionary *state = [self queryStateViaSocket];
  if (state) {
    _cachedState = state;
    _lastStateCheck = [NSDate date];
    return state;
  }

  // Socket unavailable: use last notification-pushed state (may be stale)
  return _cachedState;
}

- (BOOL)hasPendingFiles {
  NSDictionary *state = [self loadClipboardState];
  if (!state) return NO;
  return [state[@"hasPendingFiles"] boolValue];
}

- (NSUInteger)pendingFileCount {
  NSDictionary *state = [self loadClipboardState];
  if (!state) return 0;
  NSArray *files = state[@"files"];
  if (files) return files.count;
  NSNumber *count = state[@"fileCount"];
  return count ? [count unsignedIntegerValue] : 0;
}

#pragma mark - Context Menu

- (NSMenu *)menuForMenuKind:(FIMenuKind)menuKind {
  NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Deskflow"];

  BOOL hasPending = [self hasPendingFiles];
  NSUInteger count = hasPending ? [self pendingFileCount] : 0;

  NSString *title;
  if (!hasPending) {
    title = @"No Files to Paste";
  } else if (count == 1) {
    title = [NSString stringWithFormat:@"Paste 1 File from %@", [self sourceLabel]];
  } else {
    title = [NSString stringWithFormat:@"Paste %lu Files from %@",
                                       (unsigned long)count, [self sourceLabel]];
  }

  NSMenuItem *item =
      [[NSMenuItem alloc] initWithTitle:title
                                 action:hasPending ? @selector(pasteFromDeskflow:) : nil
                          keyEquivalent:@""];
  item.image = _toolbarIcon;
  item.enabled = hasPending;
  [menu addItem:item];

  return menu;
}

#pragma mark - Paste Action

- (void)pasteFromDeskflow:(id)sender {
  NSURL *targetURL = [[FIFinderSyncController defaultController] targetedURL];

  if (!targetURL) {
    NSArray *paths = NSSearchPathForDirectoriesInDomains(
        NSDesktopDirectory, NSUserDomainMask, YES);
    targetURL = [NSURL fileURLWithPath:paths.firstObject];
  }

  NSString *targetPath = [targetURL path];
  extLog(@"[Ext] paste to: %@", targetPath);

  // Primary: send via socket (low latency)
  if ([self sendPasteViaSocket:targetPath]) {
    extLog(@"[Ext] paste request sent via socket");
    return;
  }

  // Fallback: notification
  NSDictionary *userInfo = @{
    @"targetDirectory" : targetPath,
    @"timestamp" : @([[NSDate date] timeIntervalSince1970])
  };
  [[NSDistributedNotificationCenter defaultCenter]
      postNotificationName:kPasteRequestNotification
                    object:nil
                  userInfo:userInfo
       deliverImmediately:YES];
  extLog(@"[Ext] paste request sent via notification (fallback)");
}

@end
