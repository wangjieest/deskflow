/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class ClipboardTransferClientTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void init();
  void nestedFileKeepsFolderStructure();
  void lengthPrefixedPacketsAreAccepted();
  void directoryEntryCreatesFolder();
  void pathTraversalFallsBackToBaseName();
  void keepExistingPicksFreeName();
  void oversizedChunkFailsWithoutLeftovers();
  void truncatedTransferRemovesPartialFile();
  void moreDataThanAnnouncedFails();
  void plantedSymlinkIsNotFollowed();
  void copyWithoutFileListDropsOnlyFiles();
  void sourceErrorIsReported();

private:
  Log m_log;
  std::unique_ptr<QTemporaryDir> m_dir;
};
