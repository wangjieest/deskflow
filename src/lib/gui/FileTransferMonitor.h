/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QRegularExpression>

class FileTransferDialog;

namespace deskflow::gui {

class FileTail;

//! Monitors log output for file transfer messages
/*!
This class monitors the core process log output and parses
file transfer messages to update the GUI progress dialog.

Log message formats:
- FILE_TRANSFER_START: id=%u, name=%s, size=%llu
- FILE_TRANSFER_PROGRESS: id=%u, percent=%d, bytes=%llu, total=%llu
- FILE_TRANSFER_COMPLETE: id=%u, file=%s, path=%s
- FILE_TRANSFER_ERROR: id=%u, error=%s
*/
class FileTransferMonitor : public QObject
{
  Q_OBJECT

public:
  explicit FileTransferMonitor(QObject *parent = nullptr);
  ~FileTransferMonitor() override = default;

  //! Start monitoring the specified log file
  void startMonitoring(const QString &logFilePath);

  //! Stop monitoring
  void stopMonitoring();

  //! Check if actively monitoring
  bool isMonitoring() const { return m_fileTail != nullptr; }

Q_SIGNALS:
  //! Emitted when a file transfer starts
  void transferStarted(uint32_t requestId, const QString &fileName, uint64_t fileSize);

  //! Emitted when transfer progress is updated
  void transferProgress(uint32_t requestId, int percent, uint64_t bytesTransferred, uint64_t totalSize);

  //! Emitted when a file transfer completes
  void transferCompleted(uint32_t requestId, const QString &fileName, const QString &localPath);

  //! Emitted when a file transfer fails
  void transferFailed(uint32_t requestId, const QString &errorMessage);

private Q_SLOTS:
  void handleLogLine(const QString &line);

private:
  void parseStartMessage(const QString &line);
  void parseProgressMessage(const QString &line);
  void parseCompleteMessage(const QString &line);
  void parseErrorMessage(const QString &line);

  FileTail *m_fileTail = nullptr;

  // Regex patterns for parsing log messages
  QRegularExpression m_startPattern;
  QRegularExpression m_progressPattern;
  QRegularExpression m_completePattern;
  QRegularExpression m_errorPattern;
};

} // namespace deskflow::gui
