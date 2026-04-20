/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXPasteboardBridge.h"
#include "platform/OSXPasteboardPeeker.h"
#include "deskflow/ClipboardTransferThread.h"
#include "base/Log.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

// ---------------------------------------------------------------------------
// Deferred pasteboard provider — declares types immediately, downloads on demand
// ---------------------------------------------------------------------------
@interface DeferredPasteProvider : NSObject
@property (nonatomic) ClipboardTransferThread *transferThread;
@property (nonatomic, copy) NSString *tempDir;
+ (instancetype)providerWithThread:(ClipboardTransferThread *)thread;
- (void)declarePasteboard;
@end

@implementation DeferredPasteProvider

+ (instancetype)providerWithThread:(ClipboardTransferThread *)thread {
  DeferredPasteProvider *p = [DeferredPasteProvider new];
  p.transferThread = thread;
  // Create temp dir for this session
  char tmpl[] = "/tmp/autodeskflow-paste-XXXXXX";
  char *dir = mkdtemp(tmpl);
  p.tempDir = dir ? [NSString stringWithUTF8String:dir] : NSTemporaryDirectory();
  return p;
}

- (void)declarePasteboard {
  NSPasteboard *pb = [NSPasteboard generalPasteboard];
  // Declare deferred types — AppKit will call provideDataForType: on demand
  [pb declareTypes:@[NSPasteboardTypeFileURL, @"NSFilenamesPboardType"] owner:self];
  LOG_INFO("[DeferredPaste] declared deferred pasteboard types — Cmd+V ready (download on paste)");
}

// Called synchronously by AppKit when the user actually pastes (Cmd+V / Edit→Paste)
- (void)pasteboard:(NSPasteboard *)sender provideDataForType:(NSPasteboardType)type {
  LOG_INFO("[DeferredPaste] provideDataForType: %s — downloading now...", [type UTF8String]);

  if (!self.transferThread || !self.transferThread->hasPendingFilesForPaste()) {
    LOG_WARN("[DeferredPaste] no pending files when paste was triggered");
    return;
  }

  std::string destFolder = self.tempDir ? std::string([self.tempDir UTF8String]) : "/tmp";
  auto paths = self.transferThread->requestFilesAndWait(destFolder, 30000);

  if (paths.empty()) {
    LOG_ERR("[DeferredPaste] download failed or timed out");
    return;
  }

  LOG_INFO("[DeferredPaste] downloaded %zu file(s) to %s", paths.size(), destFolder.c_str());

  if ([type isEqualToString:NSPasteboardTypeFileURL]) {
    NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:paths.size()];
    for (const auto &p : paths) {
      NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]];
      if (url) [urls addObject:url];
    }
    // Write file URLs to pasteboard so Finder copies them to the paste destination
    [sender clearContents];
    [sender writeObjects:urls];
  } else if ([type isEqualToString:@"NSFilenamesPboardType"]) {
    NSMutableArray<NSString *> *filePaths = [NSMutableArray arrayWithCapacity:paths.size()];
    for (const auto &p : paths)
      [filePaths addObject:[NSString stringWithUTF8String:p.c_str()]];
    [sender setPropertyList:filePaths forType:@"NSFilenamesPboardType"];
  }
}

// Called when another app takes ownership of the pasteboard
- (void)pasteboardChangedOwner:(NSPasteboard *)sender {
  LOG_DEBUG("[DeferredPaste] pasteboard ownership lost");
  self.transferThread = nullptr;
}
@end

static DeferredPasteProvider *s_deferredProvider = nil;
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>

static std::string resolveHostname(const std::string &ipAddr)
{
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  if (inet_pton(AF_INET, ipAddr.c_str(), &sa.sin_addr) != 1) {
    return ipAddr;
  }
  char host[NI_MAXHOST];
  if (getnameinfo((struct sockaddr *)&sa, sizeof(sa), host, sizeof(host),
                  nullptr, 0, NI_NAMEREQD) == 0) {
    // Use only the short hostname (before first dot)
    std::string full(host);
    auto dot = full.find('.');
    return (dot != std::string::npos) ? full.substr(0, dot) : full;
  }
  return ipAddr;
}

// Notification names (kept for state broadcast)
static NSString *const kClipboardUpdateNotification = @"org.deskflow.clipboardUpdate";
static NSString *const kPasteRequestNotification = @"org.deskflow.pasteRequest";
static NSString *const kRequestStateNotification = @"org.deskflow.requestClipboardState";

// Unix domain socket path for low-latency IPC
static const char *kSocketPath = "/tmp/autodeskflow-paste.sock";

// Debug state file (optional, written alongside socket communication)
static NSString *const kDebugStateDir = @"AutoDeskflow";
static NSString *const kDebugStateFile = @"clipboard-state.json";

// Internal state
static OSXPasteboardBridge::PasteCallback s_pasteCallback;
static id s_pasteObserver = nil;
static id s_stateRequestObserver = nil;
static std::string s_lastFilesJson;
static int s_lastFileCount = 0;
static std::string s_lastSourceAddress;
static uint16_t s_lastSourcePort = 0;
static uint64_t s_lastSessionId = 0;
static int s_serverSocket = -1;
static std::thread s_socketThread;
static bool s_running = false;
static dispatch_source_t s_cmdFileSource = nil;
// Command file written by the sandboxed extension (can't connect to socket due to EPERM)
static const char *kCmdFilePath = "/tmp/autodeskflow-paste-cmd.txt";

#pragma mark - Debug State File

static void writeDebugStateFile(const std::string &filesJson, int fileCount, bool hasPending)
{
  @autoreleasepool {
    NSArray *paths = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *dir = [paths.firstObject stringByAppendingPathComponent:kDebugStateDir];

    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:dir]) {
      [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    }

    NSString *path = [dir stringByAppendingPathComponent:kDebugStateFile];
    NSString *json = [NSString stringWithFormat:
        @"{\"hasPendingFiles\":%@,\"fileCount\":%d,\"files\":%s,\"timestamp\":%f}",
        hasPending ? @"true" : @"false",
        fileCount,
        filesJson.c_str(),
        [[NSDate date] timeIntervalSince1970]];

    [json writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
  }
}

#pragma mark - Unix Domain Socket Server

static void socketServerLoop()
{
  // Clean up old socket
  unlink(kSocketPath);

  s_serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s_serverSocket < 0) {
    LOG_ERR("OSXPasteboardBridge: failed to create socket");
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

  if (bind(s_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG_ERR("OSXPasteboardBridge: failed to bind socket");
    close(s_serverSocket);
    s_serverSocket = -1;
    return;
  }

  // Allow extension process to connect
  chmod(kSocketPath, 0666);

  if (listen(s_serverSocket, 5) < 0) {
    LOG_ERR("OSXPasteboardBridge: failed to listen on socket");
    close(s_serverSocket);
    s_serverSocket = -1;
    return;
  }

  LOG_INFO("OSXPasteboardBridge: socket server listening on %s", kSocketPath);

  while (s_running) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(s_serverSocket, &readfds);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int ret = select(s_serverSocket + 1, &readfds, nullptr, nullptr, &tv);
    if (ret <= 0) continue;

    int clientFd = accept(s_serverSocket, nullptr, nullptr);
    { FILE *f = fopen("/tmp/autodeskflow-socket.log","a");
      if (f) { fprintf(f, "accept: fd=%d err=%d\n", clientFd, errno); fclose(f); } }
    if (clientFd < 0) continue;

    // Read command from extension
    char buf[4096];
    ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      std::string cmd(buf);

      // File log every command for debugging
      { FILE *f = fopen("/tmp/autodeskflow-socket.log","a");
        if (f) { fprintf(f, "cmd(%zd): %s\n", n, cmd.substr(0,40).c_str()); fclose(f); } }

      if (cmd == "GET_STATE") {
        // Respond with current clipboard state
        std::string response;
        if (s_lastFileCount > 0) {
          response = "{\"hasPendingFiles\":true,\"fileCount\":" +
                     std::to_string(s_lastFileCount) +
                     ",\"source\":\"" + s_lastSourceAddress + "\"" +
                     ",\"sourcePort\":" + std::to_string(s_lastSourcePort) +
                     ",\"sessionId\":" + std::to_string(s_lastSessionId) +
                     ",\"files\":" + s_lastFilesJson + "}";
        } else {
          response = "{\"hasPendingFiles\":false,\"fileCount\":0,\"source\":\"\",\"sourcePort\":0,\"files\":[]}";
        }
        write(clientFd, response.c_str(), response.size());
        LOG_DEBUG("OSXPasteboardBridge: served state to extension (%zu bytes)", response.size());
      } else if (cmd.rfind("PASTE:", 0) == 0) {
        // Format: PASTE:/path/to/target/dir
        std::string targetDir = cmd.substr(6);
        LOG_INFO("[FinderPaste] socket received PASTE request, target: %s", targetDir.c_str());
        if (s_pasteCallback) {
          LOG_INFO("[FinderPaste] invoking paste callback...");
          s_pasteCallback(targetDir);
          LOG_INFO("[FinderPaste] paste callback returned");
        } else {
          LOG_WARN("[FinderPaste] no paste callback registered!");
        }
        const char *ack = "OK";
        write(clientFd, ack, 2);
      }
    }
    close(clientFd);
  }

  close(s_serverSocket);
  s_serverSocket = -1;
  unlink(kSocketPath);
}

#pragma mark - Public API

void OSXPasteboardBridge::startListening(PasteCallback callback)
{
  s_pasteCallback = callback;

  // Start socket server thread
  s_running = true;
  s_socketThread = std::thread(socketServerLoop);
  s_socketThread.detach();

  // Also listen via NSDistributedNotificationCenter as fallback
  if (s_pasteObserver) {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:s_pasteObserver];
  }
  // Use a dedicated background queue instead of main queue to avoid
  // Qt event loop integration issues with NSDistributedNotificationCenter.
  NSOperationQueue *pasteQueue = [[NSOperationQueue alloc] init];
  pasteQueue.name = @"AutoDeskflow.PasteNotification";
  s_pasteObserver = [[NSDistributedNotificationCenter defaultCenter]
      addObserverForName:kPasteRequestNotification
                  object:nil
                   queue:pasteQueue
              usingBlock:^(NSNotification *note) {
                // Target dir is in note.object (sandboxed apps can't send userInfo)
                NSString *targetDir = note.object ?: note.userInfo[@"targetDirectory"];
                LOG_INFO("[FinderPaste] notification received: target=%s", targetDir ? [targetDir UTF8String] : "(nil)");
                FILE *f = fopen("/tmp/autodeskflow-paste.log", "a");
                if (f) { fprintf(f, "[FinderPaste] notification: target=%s\n", targetDir ? [targetDir UTF8String] : "(nil)"); fclose(f); }
                if (targetDir && s_pasteCallback) {
                  s_pasteCallback(std::string([targetDir UTF8String]));
                }
              }];

  // Listen for state requests from extension (cold start recovery)
  if (s_stateRequestObserver) {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:s_stateRequestObserver];
  }
  s_stateRequestObserver = [[NSDistributedNotificationCenter defaultCenter]
      addObserverForName:kRequestStateNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification *) {
                // Re-broadcast current state
                if (s_lastFileCount > 0) {
                  publishPendingFiles(s_lastFilesJson, s_lastFileCount, s_lastSourceAddress, s_lastSourcePort, s_lastSessionId);
                }
              }];

  LOG_INFO("OSXPasteboardBridge: listening (socket + notification)");
}

void OSXPasteboardBridge::stopListening()
{
  s_running = false;

  if (s_pasteObserver) {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:s_pasteObserver];
    s_pasteObserver = nil;
  }
  if (s_stateRequestObserver) {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:s_stateRequestObserver];
    s_stateRequestObserver = nil;
  }
  s_pasteCallback = nullptr;

  // Socket thread will exit on next select() timeout
  if (s_serverSocket >= 0) {
    close(s_serverSocket);
    s_serverSocket = -1;
  }
  unlink(kSocketPath);

  LOG_INFO("OSXPasteboardBridge: stopped listening");
}

void OSXPasteboardBridge::publishPendingFiles(
    const std::string &filesJson, int fileCount, const std::string &sourceAddress,
    uint16_t sourcePort, uint64_t sessionId)
{
  // Cache for socket server and state re-broadcast
  s_lastFilesJson = filesJson;
  s_lastFileCount = fileCount;
  s_lastSourcePort = sourcePort;
  s_lastSessionId = sessionId;
  // Resolve hostname once on publish (blocking, but only on clipboard change)
  s_lastSourceAddress = sourceAddress.empty() ? sourceAddress : resolveHostname(sourceAddress);

  // Broadcast via notification using 'object' field (consistent with paste direction).
  // Both directions use object for sandbox compatibility.
  // Format: "1|<fileCount>|<source>" (1=hasPending)
  @autoreleasepool {
    NSString *payload = [NSString stringWithFormat:@"1|%d|%s", fileCount, sourceAddress.c_str()];
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kClipboardUpdateNotification
                      object:payload
                    userInfo:nil
         deliverImmediately:YES];
  }

  // Write debug state file
  writeDebugStateFile(filesJson, fileCount, true);

  LOG_INFO("OSXPasteboardBridge: published %d pending file(s)", fileCount);
}

void OSXPasteboardBridge::clearPendingFiles()
{
  s_lastFilesJson = "[]";
  s_lastFileCount = 0;

  @autoreleasepool {
    // "0" = no pending files
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kClipboardUpdateNotification
                      object:@"0"
                    userInfo:nil
         deliverImmediately:YES];
  }

  // Write debug state file
  writeDebugStateFile("[]", 0, false);

  LOG_DEBUG("OSXPasteboardBridge: cleared pending files");
}

void OSXPasteboardBridge::updatePasteboardForCmdV(const std::vector<std::string> &localPaths)
{
  LOG_INFO("OSXPasteboardBridge: updatePasteboardForCmdV called with %zu file(s)", localPaths.size());
  { FILE *f = fopen("/tmp/autodeskflow-transfer.log","a");
    if (f) { fprintf(f,"[AutoDownload] updatePasteboardForCmdV: %zu files\n", localPaths.size()); fclose(f); } }

  // IMPORTANT: copy the vector — localPaths is a const-ref to a caller-stack variable
  // that will be destroyed when the calling thread returns. The block must own the data.
  std::vector<std::string> pathsCopy(localPaths);

  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    @autoreleasepool {
      std::vector<const char *> cPaths;
      cPaths.reserve(pathsCopy.size());
      for (const auto &p : pathsCopy) cPaths.push_back(p.c_str());
      updatePasteboardWithFiles(cPaths.data(), static_cast<int>(cPaths.size()));
      LOG_INFO("OSXPasteboardBridge: NSPasteboard updated with %zu file(s) — Cmd+V ready",
               pathsCopy.size());
      FILE *f = fopen("/tmp/autodeskflow-transfer.log","a");
      if (f) { fprintf(f,"[AutoDownload] pasteboard updated OK, count=%zu\n", pathsCopy.size()); fclose(f); }
    }
  });
}

void OSXPasteboardBridge::setupDeferredPaste(void *transferThreadPtr)
{
  ClipboardTransferThread *thread = static_cast<ClipboardTransferThread *>(transferThreadPtr);
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    @autoreleasepool {
      s_deferredProvider = [DeferredPasteProvider providerWithThread:thread];
      [s_deferredProvider declarePasteboard];
    }
  });
}
