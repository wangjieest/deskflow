/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferManager.h"
#include "dialogs/FileTransferDialog.h"

#include "common/Settings.h"

#include <QApplication>
#include <QMutexLocker>

FileTransferManager &FileTransferManager::instance()
{
  static FileTransferManager instance;
  return instance;
}

FileTransferManager::FileTransferManager()
    : QObject(nullptr)
{
}

FileTransferManager::~FileTransferManager() = default;

void FileTransferManager::startTransfer(uint32_t requestId, const QString &fileName,
                                        const QString &sourcePath, uint64_t fileSize)
{
  QMutexLocker locker(&m_mutex);

  qDebug() << "[FileTransfer] Starting transfer - RequestID:" << requestId
           << "FileName:" << fileName << "Source:" << sourcePath
           << "Size:" << fileSize << "bytes";

  PendingFileRequest request;
  request.requestId = requestId;
  request.fileName = fileName;
  request.sourcePath = sourcePath;
  request.fileSize = fileSize;
  request.bytesReceived = 0;
  request.isComplete = false;
  request.hasError = false;

  m_requests.insert(requestId, request);

  qDebug() << "[FileTransfer] Total active requests:" << m_requests.size();

  // Show dialog on the main thread
  QMetaObject::invokeMethod(this, [this, fileName, sourcePath, fileSize]() {
    showDialog();
    if (m_dialog) {
      m_dialog->addFile(fileName, sourcePath, fileSize);
    }
  }, Qt::QueuedConnection);
}

void FileTransferManager::updateProgress(uint32_t requestId, uint64_t bytesReceived)
{
  QMutexLocker locker(&m_mutex);

  auto *request = findRequest(requestId);
  if (!request) {
    qWarning() << "[FileTransfer] Update progress failed - Unknown requestId:" << requestId;
    return;
  }

  request->bytesReceived = bytesReceived;

  double progress = request->fileSize > 0 ? (double)bytesReceived / request->fileSize * 100.0 : 0.0;
  qDebug() << "[FileTransfer] Progress update - RequestID:" << requestId
           << "File:" << request->fileName
           << "Received:" << bytesReceived << "/" << request->fileSize
           << QString("(%1%)").arg(progress, 0, 'f', 1);

  // Update dialog on main thread
  QString fileName = request->fileName;
  QMetaObject::invokeMethod(this, [this, fileName, bytesReceived]() {
    if (m_dialog) {
      m_dialog->updateProgress(fileName, bytesReceived);
    }
  }, Qt::QueuedConnection);
}

void FileTransferManager::completeTransfer(uint32_t requestId, const QString &localPath)
{
  QMutexLocker locker(&m_mutex);

  auto *request = findRequest(requestId);
  if (!request) {
    qWarning() << "[FileTransfer] Complete transfer failed - Unknown requestId:" << requestId;
    return;
  }

  request->isComplete = true;
  request->localPath = localPath;

  QString fileName = request->fileName;

  qInfo() << "[FileTransfer] Transfer completed successfully - RequestID:" << requestId
          << "File:" << fileName << "Size:" << request->fileSize << "bytes"
          << "Saved to:" << localPath;

  // Update dialog on main thread
  QMetaObject::invokeMethod(this, [this, fileName, localPath]() {
    if (m_dialog) {
      m_dialog->markComplete(fileName, localPath);
    }
    checkAllComplete();
  }, Qt::QueuedConnection);
}

void FileTransferManager::failTransfer(uint32_t requestId, const QString &errorMessage)
{
  QMutexLocker locker(&m_mutex);

  auto *request = findRequest(requestId);
  if (!request) {
    qWarning() << "[FileTransfer] Fail transfer called for unknown requestId:" << requestId;
    return;
  }

  request->hasError = true;

  QString fileName = request->fileName;

  qWarning() << "[FileTransfer] Transfer FAILED - RequestID:" << requestId
             << "File:" << fileName << "Error:" << errorMessage
             << "Bytes received:" << request->bytesReceived << "/" << request->fileSize;

  // Update dialog on main thread
  QMetaObject::invokeMethod(this, [this, fileName, errorMessage, requestId]() {
    if (m_dialog) {
      m_dialog->markFailed(fileName, errorMessage);
    }
    Q_EMIT transferFailed(requestId, errorMessage);
  }, Qt::QueuedConnection);
}

bool FileTransferManager::hasActiveTransfers() const
{
  QMutexLocker locker(&m_mutex);

  for (const auto &request : m_requests) {
    if (!request.isComplete && !request.hasError) {
      return true;
    }
  }
  return false;
}

QStringList FileTransferManager::getCompletedFilePaths() const
{
  QMutexLocker locker(&m_mutex);

  QStringList paths;
  for (const auto &request : m_requests) {
    if (request.isComplete && !request.hasError) {
      paths.append(request.localPath);
    }
  }
  return paths;
}

void FileTransferManager::showDialog()
{
  // Use clipboard sharing size setting to determine if dialog should be shown
  // Default: don't show dialog (silent background transfer)
  // Show only if file size exceeds clipboard sharing size limit
  QMutexLocker locker(&m_mutex);

  // Get clipboard sharing size from settings (in KB, default 3MB = 3072KB)
  QVariant sizeValue = Settings::value("server/clipboardSharingSize");
  size_t clipboardSizeLimit = (sizeValue.isNull() ? 3072 : sizeValue.toULongLong()) * 1024;

  bool hasLargeFile = false;
  for (const auto &request : m_requests) {
    if (request.fileSize > clipboardSizeLimit) {
      hasLargeFile = true;
      qDebug() << "[FileTransfer] File size" << request.fileSize
               << "exceeds limit" << clipboardSizeLimit
               << "- will show dialog";
      break;
    }
  }

  if (!hasLargeFile) {
    qDebug() << "[FileTransfer] All files below size limit - silent transfer";
    return;
  }

  locker.unlock();

  if (!m_dialog) {
    m_dialog = std::make_unique<FileTransferDialog>();

    connect(m_dialog.get(), &FileTransferDialog::transferCancelled, this, [this]() {
      QMutexLocker locker(&m_mutex);
      m_requests.clear();
    });

    connect(m_dialog.get(), &FileTransferDialog::allTransfersComplete, this, [this]() {
      checkAllComplete();
    });
  }

  if (!m_dialog->isVisible()) {
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
  }
}

void FileTransferManager::checkAllComplete()
{
  QMutexLocker locker(&m_mutex);

  bool allComplete = true;
  QStringList completedPaths;
  int totalRequests = m_requests.size();
  int completedCount = 0;
  int failedCount = 0;

  for (const auto &request : m_requests) {
    if (!request.isComplete && !request.hasError) {
      allComplete = false;
      break;
    }
    if (request.isComplete) {
      completedPaths.append(request.localPath);
      completedCount++;
    } else if (request.hasError) {
      failedCount++;
    }
  }

  if (allComplete && !m_requests.isEmpty()) {
    qInfo() << "[FileTransfer] All transfers complete - Total:" << totalRequests
            << "Completed:" << completedCount << "Failed:" << failedCount;

    // Auto-close dialog after completion
    QMetaObject::invokeMethod(this, [this]() {
      if (m_dialog && m_dialog->isVisible()) {
        qInfo() << "[FileTransfer] Auto-closing dialog after completion";
        m_dialog->close();
      }
    }, Qt::QueuedConnection);

    locker.unlock();
    Q_EMIT allTransfersComplete(completedPaths);
  }
}

PendingFileRequest *FileTransferManager::findRequest(uint32_t requestId)
{
  auto it = m_requests.find(requestId);
  if (it != m_requests.end()) {
    return &it.value();
  }
  return nullptr;
}
