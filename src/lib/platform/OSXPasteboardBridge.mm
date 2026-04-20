/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXPasteboardBridge.h"
#include "base/Log.h"

#import <Foundation/Foundation.h>
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

  // Broadcast via notification (for extensions that use notification path)
  @autoreleasepool {
    NSDictionary *userInfo = @{
      @"hasPendingFiles" : @YES,
      @"fileCount" : @(fileCount),
      @"source" : [NSString stringWithUTF8String:sourceAddress.c_str()],
      @"filesJson" : [NSString stringWithUTF8String:filesJson.c_str()],
      @"timestamp" : @([[NSDate date] timeIntervalSince1970])
    };

    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kClipboardUpdateNotification
                      object:nil
                    userInfo:userInfo
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
    NSDictionary *userInfo = @{
      @"hasPendingFiles" : @NO,
      @"fileCount" : @0,
      @"timestamp" : @([[NSDate date] timeIntervalSince1970])
    };

    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kClipboardUpdateNotification
                      object:nil
                    userInfo:userInfo
         deliverImmediately:YES];
  }

  // Write debug state file
  writeDebugStateFile("[]", 0, false);

  LOG_DEBUG("OSXPasteboardBridge: cleared pending files");
}
