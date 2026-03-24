/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QMap>
#include <QMutex>
#include <memory>

class FileTransferDialog;

//! Pending file transfer request
struct PendingFileRequest
{
  uint32_t requestId = 0;
  QString fileName;
  QString sourcePath;
  uint64_t fileSize = 0;
  uint64_t bytesReceived = 0;
  QByteArray data;
  bool isComplete = false;
  bool hasError = false;
  QString localPath;
};

//! Manager for file transfers in the GUI
/*!
This class manages file transfers between the core process and the GUI.
It shows progress dialogs and handles completion/error states.
*/
class FileTransferManager : public QObject
{
  Q_OBJECT

public:
  static FileTransferManager &instance();

  //! Start a file transfer request
  /*!
  Called when the clipboard contains file references that need to be transferred.
  \param requestId unique identifier for this transfer
  \param fileName the file name
  \param sourcePath the path on the source machine
  \param fileSize expected file size
  */
  void startTransfer(uint32_t requestId, const QString &fileName,
                     const QString &sourcePath, uint64_t fileSize);

  //! Update transfer progress
  /*!
  Called when file data is received.
  \param requestId the transfer identifier
  \param bytesReceived total bytes received so far
  */
  void updateProgress(uint32_t requestId, uint64_t bytesReceived);

  //! Complete a transfer
  /*!
  Called when file transfer is complete.
  \param requestId the transfer identifier
  \param localPath where the file was saved
  */
  void completeTransfer(uint32_t requestId, const QString &localPath);

  //! Fail a transfer
  /*!
  Called when file transfer fails.
  \param requestId the transfer identifier
  \param errorMessage description of the error
  */
  void failTransfer(uint32_t requestId, const QString &errorMessage);

  //! Check if there are active transfers
  bool hasActiveTransfers() const;

  //! Get list of completed file paths
  QStringList getCompletedFilePaths() const;

Q_SIGNALS:
  //! Emitted when all pending transfers are complete
  void allTransfersComplete(const QStringList &localPaths);

  //! Emitted when a transfer fails
  void transferFailed(uint32_t requestId, const QString &errorMessage);

private:
  FileTransferManager();
  ~FileTransferManager() override;

  FileTransferManager(const FileTransferManager &) = delete;
  FileTransferManager &operator=(const FileTransferManager &) = delete;

  void showDialog();
  void checkAllComplete();
  PendingFileRequest *findRequest(uint32_t requestId);

  std::unique_ptr<FileTransferDialog> m_dialog;
  QMap<uint32_t, PendingFileRequest> m_requests;
  mutable QMutex m_mutex;
};
