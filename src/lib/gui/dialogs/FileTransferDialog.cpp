/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferDialog.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QStyle>

FileTransferDialog::FileTransferDialog(QWidget *parent)
    : QDialog(parent)
{
  setupUI();

  m_updateTimer = new QTimer(this);
  connect(m_updateTimer, &QTimer::timeout, this, &FileTransferDialog::updateUI);
  m_updateTimer->start(100); // Update UI every 100ms
}

void FileTransferDialog::setupUI()
{
  setWindowTitle(tr("File Transfer"));
  setMinimumWidth(400);
  setModal(false); // Non-modal so user can continue working

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setSpacing(10);
  mainLayout->setContentsMargins(15, 15, 15, 15);

  // Title
  m_titleLabel = new QLabel(tr("Transferring files..."), this);
  QFont titleFont = m_titleLabel->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 2);
  m_titleLabel->setFont(titleFont);
  mainLayout->addWidget(m_titleLabel);

  // Current file
  m_currentFileLabel = new QLabel(this);
  m_currentFileLabel->setWordWrap(true);
  mainLayout->addWidget(m_currentFileLabel);

  // File progress
  auto *fileProgressLayout = new QHBoxLayout();
  auto *fileLabel = new QLabel(tr("Current file:"), this);
  m_fileProgressBar = new QProgressBar(this);
  m_fileProgressBar->setRange(0, 100);
  m_fileProgressBar->setValue(0);
  fileProgressLayout->addWidget(fileLabel);
  fileProgressLayout->addWidget(m_fileProgressBar, 1);
  mainLayout->addLayout(fileProgressLayout);

  // Overall progress
  auto *overallProgressLayout = new QHBoxLayout();
  auto *overallLabel = new QLabel(tr("Overall:"), this);
  m_overallProgressBar = new QProgressBar(this);
  m_overallProgressBar->setRange(0, 100);
  m_overallProgressBar->setValue(0);
  overallProgressLayout->addWidget(overallLabel);
  overallProgressLayout->addWidget(m_overallProgressBar, 1);
  mainLayout->addLayout(overallProgressLayout);

  // Status
  m_statusLabel = new QLabel(this);
  m_statusLabel->setStyleSheet("color: gray;");
  mainLayout->addWidget(m_statusLabel);

  // Buttons
  auto *buttonLayout = new QHBoxLayout();
  buttonLayout->addStretch();

  m_cancelButton = new QPushButton(tr("Cancel"), this);
  connect(m_cancelButton, &QPushButton::clicked, this, &FileTransferDialog::onCancelClicked);
  buttonLayout->addWidget(m_cancelButton);

  m_closeButton = new QPushButton(tr("Close"), this);
  m_closeButton->setEnabled(false);
  connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
  buttonLayout->addWidget(m_closeButton);

  mainLayout->addLayout(buttonLayout);

  setLayout(mainLayout);
}

void FileTransferDialog::addFile(const QString &fileName, const QString &sourcePath, uint64_t fileSize)
{
  FileTransferInfo info;
  info.fileName = fileName;
  info.sourcePath = sourcePath;
  info.fileSize = fileSize;
  info.bytesTransferred = 0;
  info.isComplete = false;
  info.hasError = false;

  m_files.push_back(info);
  updateUI();

  if (!isVisible()) {
    show();
    raise();
    activateWindow();
  }
}

void FileTransferDialog::updateProgress(const QString &fileName, uint64_t bytesTransferred)
{
  auto *file = findFile(fileName);
  if (file) {
    file->bytesTransferred = bytesTransferred;
  }
}

void FileTransferDialog::markComplete(const QString &fileName, const QString &localPath)
{
  auto *file = findFile(fileName);
  if (file) {
    file->isComplete = true;
    file->bytesTransferred = file->fileSize;
  }

  if (isAllComplete()) {
    m_titleLabel->setText(tr("Transfer complete"));
    m_cancelButton->setEnabled(false);
    m_closeButton->setEnabled(true);
    m_updateTimer->stop();
    Q_EMIT allTransfersComplete();
  }
}

void FileTransferDialog::markFailed(const QString &fileName, const QString &errorMessage)
{
  auto *file = findFile(fileName);
  if (file) {
    file->hasError = true;
    file->errorMessage = errorMessage;
  }

  m_statusLabel->setStyleSheet("color: red;");
  m_statusLabel->setText(tr("Error: %1").arg(errorMessage));
}

bool FileTransferDialog::isAllComplete() const
{
  if (m_files.empty()) {
    return false;
  }

  for (const auto &file : m_files) {
    if (!file.isComplete && !file.hasError) {
      return false;
    }
  }
  return true;
}

QStringList FileTransferDialog::getCompletedFiles() const
{
  QStringList result;
  for (const auto &file : m_files) {
    if (file.isComplete && !file.hasError) {
      result.append(file.fileName);
    }
  }
  return result;
}

void FileTransferDialog::onCancelClicked()
{
  m_cancelled = true;
  m_updateTimer->stop();
  Q_EMIT transferCancelled();
  reject();
}

void FileTransferDialog::updateUI()
{
  if (m_files.empty()) {
    return;
  }

  // Find current file (first non-complete file)
  int currentIndex = -1;
  for (size_t i = 0; i < m_files.size(); ++i) {
    if (!m_files[i].isComplete && !m_files[i].hasError) {
      currentIndex = static_cast<int>(i);
      break;
    }
  }

  if (currentIndex >= 0) {
    const auto &current = m_files[currentIndex];
    m_currentFileLabel->setText(tr("File: %1").arg(current.fileName));

    // File progress
    if (current.fileSize > 0) {
      int percent = static_cast<int>((current.bytesTransferred * 100) / current.fileSize);
      m_fileProgressBar->setValue(percent);
    }

    // Status
    QString sizeStr;
    if (current.fileSize >= 1024 * 1024) {
      sizeStr = tr("%1 / %2 MB")
                    .arg(current.bytesTransferred / (1024.0 * 1024.0), 0, 'f', 1)
                    .arg(current.fileSize / (1024.0 * 1024.0), 0, 'f', 1);
    } else if (current.fileSize >= 1024) {
      sizeStr = tr("%1 / %2 KB")
                    .arg(current.bytesTransferred / 1024.0, 0, 'f', 1)
                    .arg(current.fileSize / 1024.0, 0, 'f', 1);
    } else {
      sizeStr = tr("%1 / %2 bytes")
                    .arg(current.bytesTransferred)
                    .arg(current.fileSize);
    }
    m_statusLabel->setText(sizeStr);
  }

  updateOverallProgress();
}

void FileTransferDialog::updateOverallProgress()
{
  if (m_files.empty()) {
    m_overallProgressBar->setValue(0);
    return;
  }

  uint64_t totalSize = 0;
  uint64_t totalTransferred = 0;

  for (const auto &file : m_files) {
    totalSize += file.fileSize;
    totalTransferred += file.bytesTransferred;
  }

  if (totalSize > 0) {
    int percent = static_cast<int>((totalTransferred * 100) / totalSize);
    m_overallProgressBar->setValue(percent);
  }

  // Update title with count
  int completedCount = 0;
  for (const auto &file : m_files) {
    if (file.isComplete) {
      completedCount++;
    }
  }
  m_titleLabel->setText(tr("Transferring files (%1/%2)...")
                            .arg(completedCount)
                            .arg(m_files.size()));
}

FileTransferInfo* FileTransferDialog::findFile(const QString &fileName)
{
  for (auto &file : m_files) {
    if (file.fileName == fileName) {
      return &file;
    }
  }
  return nullptr;
}
