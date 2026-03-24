/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#import "platform/OSXPasteboardPeeker.h"

#import <Cocoa/Cocoa.h>
#import <CoreData/CoreData.h>
#import <Foundation/Foundation.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

CFStringRef getDraggedFileURL()
{
  NSString *pbName = NSDragPboard;
  NSPasteboard *pboard = [NSPasteboard pasteboardWithName:pbName];

  NSMutableString *string;
  string = [[NSMutableString alloc] initWithCapacity:0];

  NSArray *files = [pboard propertyListForType:NSFilenamesPboardType];
  for (id file in files) {
    [string appendString:(NSString *)file];
    [string appendString:@"\0"];
  }

  return (CFStringRef)string;
}

void updatePasteboardWithFiles(const char **filePaths, int count)
{
  @autoreleasepool {
    NSLog(@"[Clipboard] updatePasteboardWithFiles called with %d file(s)", count);

    NSPasteboard *pboard = [NSPasteboard generalPasteboard];
    [pboard clearContents];
    NSLog(@"[Clipboard] Cleared pasteboard contents");

    NSMutableArray *fileURLs = [NSMutableArray arrayWithCapacity:count];

    for (int i = 0; i < count; i++) {
      NSString *path = [NSString stringWithUTF8String:filePaths[i]];
      NSLog(@"[Clipboard] Processing file %d: %@", i, path);

      // Check if file exists
      NSFileManager *fileManager = [NSFileManager defaultManager];
      BOOL fileExists = [fileManager fileExistsAtPath:path];
      NSLog(@"[Clipboard] File exists: %@", fileExists ? @"YES" : @"NO");

      NSURL *url = [NSURL fileURLWithPath:path];
      if (url) {
        NSLog(@"[Clipboard] Created URL: %@", [url absoluteString]);
        [fileURLs addObject:url];
      } else {
        NSLog(@"[Clipboard] WARNING: Failed to create URL for path: %@", path);
      }
    }

    if ([fileURLs count] > 0) {
      NSLog(@"[Clipboard] Writing %lu URL(s) to pasteboard", (unsigned long)[fileURLs count]);
      BOOL success = [pboard writeObjects:fileURLs];
      NSLog(@"[Clipboard] writeObjects returned: %@", success ? @"YES" : @"NO");

      if (success) {
        // Verify what was written
        NSArray *types = [pboard types];
        NSLog(@"[Clipboard] Pasteboard types after write: %@", types);

        // Check for file URL type
        if ([types containsObject:NSPasteboardTypeFileURL]) {
          NSLog(@"[Clipboard] ✅ NSPasteboardTypeFileURL is present");
        } else {
          NSLog(@"[Clipboard] ⚠️ NSPasteboardTypeFileURL is NOT present");
        }
      }
    } else {
      NSLog(@"[Clipboard] ERROR: No valid file URLs created");
    }
  }
}

// Helper function to escape JSON strings
static NSString* escapeJsonString(NSString *str) {
  NSMutableString *result = [NSMutableString stringWithCapacity:[str length]];
  for (NSUInteger i = 0; i < [str length]; i++) {
    unichar c = [str characterAtIndex:i];
    switch (c) {
      case '"': [result appendString:@"\\\""]; break;
      case '\\': [result appendString:@"\\\\"]; break;
      case '\n': [result appendString:@"\\n"]; break;
      case '\r': [result appendString:@"\\r"]; break;
      case '\t': [result appendString:@"\\t"]; break;
      default: [result appendFormat:@"%C", c]; break;
    }
  }
  return result;
}

// Recursively scan directory and collect all files
static void scanDirectoryRecursive(NSString *dirPath, NSString *baseName, NSString *relativePath,
                                   NSMutableArray *allFiles, NSFileManager *fileManager) {
  NSError *error = nil;
  NSArray *contents = [fileManager contentsOfDirectoryAtPath:dirPath error:&error];
  if (error) {
    NSLog(@"[Clipboard] Failed to list directory %@: %@", dirPath, error);
    return;
  }

  for (NSString *item in contents) {
    NSString *fullPath = [dirPath stringByAppendingPathComponent:item];
    NSString *itemRelativePath = [relativePath stringByAppendingPathComponent:item];

    BOOL isDir = NO;
    if ([fileManager fileExistsAtPath:fullPath isDirectory:&isDir]) {
      NSDictionary *attrs = [fileManager attributesOfItemAtPath:fullPath error:nil];
      unsigned long long fileSize = isDir ? 0 : [attrs fileSize];

      NSDictionary *fileInfo = @{
        @"path": fullPath,
        @"name": item,
        @"relativePath": itemRelativePath,
        @"size": @(fileSize),
        @"isDir": @(isDir)
      };
      [allFiles addObject:fileInfo];

      if (isDir) {
        scanDirectoryRecursive(fullPath, baseName, itemRelativePath, allFiles, fileManager);
      }
    }
  }
}

const char* getClipboardFilesAsJson(void) {
  @autoreleasepool {
    NSPasteboard *pboard = [NSPasteboard generalPasteboard];

    // Try to read file URLs from pasteboard
    NSArray *classes = @[[NSURL class]];
    NSDictionary *options = @{NSPasteboardURLReadingFileURLsOnlyKey: @YES};

    if (![pboard canReadObjectForClasses:classes options:options]) {
      NSLog(@"[Clipboard] No file URLs available on clipboard");
      return NULL;
    }

    NSArray *fileURLs = [pboard readObjectsForClasses:classes options:options];
    if (!fileURLs || [fileURLs count] == 0) {
      NSLog(@"[Clipboard] No file URLs read from clipboard");
      return NULL;
    }

    NSLog(@"[Clipboard] Found %lu file(s) on clipboard", (unsigned long)[fileURLs count]);

    NSFileManager *fileManager = [NSFileManager defaultManager];
    NSMutableArray *allFiles = [NSMutableArray array];

    for (NSURL *url in fileURLs) {
      NSString *path = [url path];
      NSString *fileName = [path lastPathComponent];

      BOOL isDir = NO;
      if (![fileManager fileExistsAtPath:path isDirectory:&isDir]) {
        NSLog(@"[Clipboard] File does not exist: %@", path);
        continue;
      }

      NSDictionary *attrs = [fileManager attributesOfItemAtPath:path error:nil];
      unsigned long long fileSize = isDir ? 0 : [attrs fileSize];

      // Add top-level item
      NSDictionary *fileInfo = @{
        @"path": path,
        @"name": fileName,
        @"relativePath": fileName,
        @"size": @(fileSize),
        @"isDir": @(isDir)
      };
      [allFiles addObject:fileInfo];

      NSLog(@"[Clipboard] File: %@ (size=%llu, isDir=%d)", fileName, fileSize, isDir);

      // If directory, recursively scan contents
      if (isDir) {
        scanDirectoryRecursive(path, fileName, fileName, allFiles, fileManager);
      }
    }

    if ([allFiles count] == 0) {
      return NULL;
    }

    // Build JSON array
    NSMutableString *json = [NSMutableString stringWithString:@"["];
    BOOL first = YES;

    for (NSDictionary *file in allFiles) {
      if (!first) {
        [json appendString:@","];
      }
      first = NO;

      [json appendString:@"{"];
      [json appendFormat:@"\"path\":\"%@\",", escapeJsonString(file[@"path"])];
      [json appendFormat:@"\"name\":\"%@\",", escapeJsonString(file[@"name"])];
      [json appendFormat:@"\"relativePath\":\"%@\",", escapeJsonString(file[@"relativePath"])];
      [json appendFormat:@"\"size\":%@,", file[@"size"]];
      [json appendFormat:@"\"isDir\":%@", [file[@"isDir"] boolValue] ? @"true" : @"false"];
      [json appendString:@"}"];
    }

    [json appendString:@"]"];

    NSLog(@"[Clipboard] Generated JSON with %lu files", (unsigned long)[allFiles count]);

    // Return a copy that the caller must free
    const char *utf8 = [json UTF8String];
    char *result = strdup(utf8);
    return result;
  }
}

void freeClipboardFilesJson(const char* json) {
  if (json) {
    free((void*)json);
  }
}
