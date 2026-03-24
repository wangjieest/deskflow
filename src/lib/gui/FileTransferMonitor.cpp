/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferMonitor.h"
#include "FileTail.h"

namespace deskflow::gui {

FileTransferMonitor::FileTransferMonitor(QObject *parent)
    : QObject(parent)
{
  // Initialize regex patterns
  m_startPattern = QRegularExpression(
      R"(FILE_TRANSFER_START:\s*id=(\d+),\s*name=([^,]+),\s*size=(\d+))");
  m_progressPattern = QRegularExpression(
      R"(FILE_TRANSFER_PROGRESS:\s*id=(\d+),\s*percent=(\d+),\s*bytes=(\d+),\s*total=(\d+))");
  m_completePattern = QRegularExpression(
      R"(FILE_TRANSFER_COMPLETE:\s*id=(\d+),\s*file=([^,]+),\s*path=(.+))");
  m_errorPattern = QRegularExpression(
      R"(FILE_TRANSFER_ERROR:\s*id=(\d+),\s*error=(.+))");
}

void FileTransferMonitor::startMonitoring(const QString &logFilePath)
{
  stopMonitoring();

  m_fileTail = new FileTail(logFilePath, this);
  connect(m_fileTail, &FileTail::newLine, this, &FileTransferMonitor::handleLogLine);
}

void FileTransferMonitor::stopMonitoring()
{
  if (m_fileTail) {
    m_fileTail->deleteLater();
    m_fileTail = nullptr;
  }
}

void FileTransferMonitor::handleLogLine(const QString &line)
{
  // Check for file transfer messages
  if (line.contains("FILE_TRANSFER_START:")) {
    parseStartMessage(line);
  } else if (line.contains("FILE_TRANSFER_PROGRESS:")) {
    parseProgressMessage(line);
  } else if (line.contains("FILE_TRANSFER_COMPLETE:")) {
    parseCompleteMessage(line);
  } else if (line.contains("FILE_TRANSFER_ERROR:")) {
    parseErrorMessage(line);
  }
}

void FileTransferMonitor::parseStartMessage(const QString &line)
{
  QRegularExpressionMatch match = m_startPattern.match(line);
  if (match.hasMatch()) {
    uint32_t requestId = match.captured(1).toUInt();
    QString fileName = match.captured(2).trimmed();
    uint64_t fileSize = match.captured(3).toULongLong();

    Q_EMIT transferStarted(requestId, fileName, fileSize);
  }
}

void FileTransferMonitor::parseProgressMessage(const QString &line)
{
  QRegularExpressionMatch match = m_progressPattern.match(line);
  if (match.hasMatch()) {
    uint32_t requestId = match.captured(1).toUInt();
    int percent = match.captured(2).toInt();
    uint64_t bytesTransferred = match.captured(3).toULongLong();
    uint64_t totalSize = match.captured(4).toULongLong();

    Q_EMIT transferProgress(requestId, percent, bytesTransferred, totalSize);
  }
}

void FileTransferMonitor::parseCompleteMessage(const QString &line)
{
  QRegularExpressionMatch match = m_completePattern.match(line);
  if (match.hasMatch()) {
    uint32_t requestId = match.captured(1).toUInt();
    QString fileName = match.captured(2).trimmed();
    QString localPath = match.captured(3).trimmed();

    Q_EMIT transferCompleted(requestId, fileName, localPath);
  }
}

void FileTransferMonitor::parseErrorMessage(const QString &line)
{
  QRegularExpressionMatch match = m_errorPattern.match(line);
  if (match.hasMatch()) {
    uint32_t requestId = match.captured(1).toUInt();
    QString errorMessage = match.captured(2).trimmed();

    Q_EMIT transferFailed(requestId, errorMessage);
  }
}

} // namespace deskflow::gui
