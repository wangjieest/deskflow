/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>
#include <memory>
#include <vector>

//! File transfer progress information
struct FileTransferInfo
{
  QString fileName;
  QString sourcePath;
  uint64_t fileSize = 0;
  uint64_t bytesTransferred = 0;
  bool isComplete = false;
  bool hasError = false;
  QString errorMessage;
};

//! Dialog for showing file transfer progress
/*!
This dialog shows the progress of file transfers between machines.
It can handle multiple files and shows overall progress.
*/
class FileTransferDialog : public QDialog
{
  Q_OBJECT

public:
  explicit FileTransferDialog(QWidget *parent = nullptr);
  ~FileTransferDialog() override = default;

  //! Add a file to the transfer queue
  void addFile(const QString &fileName, const QString &sourcePath, uint64_t fileSize);

  //! Update progress for a specific file
  void updateProgress(const QString &fileName, uint64_t bytesTransferred);

  //! Mark a file as complete
  void markComplete(const QString &fileName, const QString &localPath);

  //! Mark a file as failed
  void markFailed(const QString &fileName, const QString &errorMessage);

  //! Check if all transfers are complete
  bool isAllComplete() const;

  //! Get list of successfully transferred files
  QStringList getCompletedFiles() const;

Q_SIGNALS:
  //! Emitted when all transfers are complete
  void allTransfersComplete();

  //! Emitted when user cancels the transfer
  void transferCancelled();

private Q_SLOTS:
  void onCancelClicked();
  void updateUI();

private:
  void setupUI();
  void updateOverallProgress();
  FileTransferInfo* findFile(const QString &fileName);

  QLabel *m_titleLabel = nullptr;
  QLabel *m_currentFileLabel = nullptr;
  QLabel *m_statusLabel = nullptr;
  QProgressBar *m_fileProgressBar = nullptr;
  QProgressBar *m_overallProgressBar = nullptr;
  QPushButton *m_cancelButton = nullptr;
  QPushButton *m_closeButton = nullptr;
  QTimer *m_updateTimer = nullptr;

  std::vector<FileTransferInfo> m_files;
  int m_currentFileIndex = 0;
  bool m_cancelled = false;
};
