/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardTransferClientTests.h"

#include "deskflow/ClipboardTransferClient.h"
#include "deskflow/FileTransfer.h"

#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>

#include <future>

namespace {

using Result = ClipboardTransferClient::DownloadResult;

QByteArray be32(uint32_t v)
{
  QByteArray b(4, '\0');
  b[0] = static_cast<char>(v >> 24);
  b[1] = static_cast<char>(v >> 16);
  b[2] = static_cast<char>(v >> 8);
  b[3] = static_cast<char>(v);
  return b;
}

// Same bytes as ProtocolUtil::writef(stream, kMsgDFileChunk, requestId, type, &payload)
QByteArray chunk(FileChunkType type, const std::string &payload)
{
  QByteArray out("DFCH");
  out += be32(1);
  out += static_cast<char>(type);
  out += be32(static_cast<uint32_t>(payload.size()));
  out += QByteArray(payload.data(), static_cast<qsizetype>(payload.size()));
  return out;
}

QByteArray prefixed(const QByteArray &packet)
{
  return be32(static_cast<uint32_t>(packet.size())) + packet;
}

// The real sender's Start payload
std::string start(const std::string &relativePath, uint64_t size, bool isDir = false)
{
  return FileTransfer::createStartChunkEx(1, "ignored", relativePath, size, isDir);
}

QByteArray wholeFile(const std::string &relativePath, const std::string &content)
{
  return chunk(FileChunkType::Start, start(relativePath, content.size())) + chunk(FileChunkType::Data, content) +
         chunk(FileChunkType::End, {});
}

// Serves one canned response to the first connection while the real client downloads from it
Result download(const QByteArray &response, const std::string &remotePath, const QString &destDir, bool keep = false)
{
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, 0))
    return {false, {}, "listen failed"};

  const auto port = server.serverPort();
  auto pending = std::async(std::launch::async, [port, remotePath, dest = destDir.toStdString(), keep] {
    return ClipboardTransferClient::downloadFile("127.0.0.1", port, 42, remotePath, dest, keep);
  });

  if (server.waitForNewConnection(5000)) {
    std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
    const auto requestSize = static_cast<qsizetype>(20 + remotePath.size());
    QByteArray request;
    while (request.size() < requestSize && socket->waitForReadyRead(5000))
      request += socket->readAll();
    socket->write(response);
    socket->waitForBytesWritten(5000);
    socket->disconnectFromHost();
    if (socket->state() != QAbstractSocket::UnconnectedState)
      socket->waitForDisconnected(5000);
  }
  return pending.get();
}

QByteArray readFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}

QStringList allEntries(const QString &dir)
{
  QStringList out;
  QDir root(dir);
  for (const auto &info : root.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
    out << info.fileName();
  }
  return out;
}

} // namespace

void ClipboardTransferClientTests::init()
{
  m_dir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_dir->isValid());
}

void ClipboardTransferClientTests::nestedFileKeepsFolderStructure()
{
  const auto result = download(wholeFile("folder/sub/a.txt", "hello"), "/src/folder/sub/a.txt", m_dir->path());

  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(QString::fromStdString(result.localPath), m_dir->filePath("folder/sub/a.txt"));
  QCOMPARE(readFile(m_dir->filePath("folder/sub/a.txt")), QByteArray("hello"));
  QVERIFY(!QFile::exists(m_dir->filePath("folder/sub/a.txt.pasting")));
}

void ClipboardTransferClientTests::lengthPrefixedPacketsAreAccepted()
{
  const auto response = prefixed(chunk(FileChunkType::Start, start("b.txt", 5))) +
                        prefixed(chunk(FileChunkType::Data, "hel")) + prefixed(chunk(FileChunkType::Data, "lo")) +
                        prefixed(chunk(FileChunkType::End, {}));

  const auto result = download(response, "/src/b.txt", m_dir->path());

  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(m_dir->filePath("b.txt")), QByteArray("hello"));
}

void ClipboardTransferClientTests::directoryEntryCreatesFolder()
{
  const auto response = chunk(FileChunkType::Start, start("folder/empty", 0, true)) + chunk(FileChunkType::End, {});

  const auto result = download(response, "/src/folder/empty", m_dir->path());

  QVERIFY2(result.success, result.error.c_str());
  QVERIFY(QDir(m_dir->filePath("folder/empty")).exists());
}

void ClipboardTransferClientTests::pathTraversalFallsBackToBaseName()
{
  QDir parent(m_dir->path());
  parent.cdUp();

  auto result = download(wholeFile("../../escape.txt", "x"), "/src/report.txt", m_dir->path());
  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(QString::fromStdString(result.localPath), m_dir->filePath("report.txt"));
  QVERIFY(!QFile::exists(parent.filePath("escape.txt")));

  result = download(wholeFile("C:\\Windows\\evil.txt", "x"), "/src/second.txt", m_dir->path());
  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(QString::fromStdString(result.localPath), m_dir->filePath("second.txt"));
}

void ClipboardTransferClientTests::keepExistingPicksFreeName()
{
  QFile existing(m_dir->filePath("a.txt"));
  QVERIFY(existing.open(QIODevice::WriteOnly));
  existing.write("old");
  existing.close();

  auto result = download(wholeFile("a.txt", "new"), "/src/a.txt", m_dir->path(), true);
  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(QString::fromStdString(result.localPath), m_dir->filePath("a 2.txt"));
  QCOMPARE(readFile(m_dir->filePath("a.txt")), QByteArray("old"));
  QCOMPARE(readFile(m_dir->filePath("a 2.txt")), QByteArray("new"));

  result = download(wholeFile("a.txt", "newer"), "/src/a.txt", m_dir->path(), false);
  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(m_dir->filePath("a.txt")), QByteArray("newer"));
}

void ClipboardTransferClientTests::oversizedChunkFailsWithoutLeftovers()
{
  // Claims ~4 GB of payload; the old parser wrapped 13 + len and wrote past a tiny buffer
  QByteArray hostile("DFCH");
  hostile += be32(1);
  hostile += static_cast<char>(FileChunkType::Data);
  hostile += be32(0xFFFFFFF8u);

  auto result = download(chunk(FileChunkType::Start, start("a.txt", 10)) + hostile, "/src/a.txt", m_dir->path());
  QVERIFY(!result.success);
  QCOMPARE(QString::fromStdString(result.error), QStringLiteral("chunk too large"));
  QVERIFY2(allEntries(m_dir->path()).isEmpty(), qPrintable(allEntries(m_dir->path()).join(", ")));

  // Length prefix shorter than a chunk header
  result = download(be32(9) + QByteArray(9, 'x'), "/src/a.txt", m_dir->path());
  QVERIFY(!result.success);
  QCOMPARE(QString::fromStdString(result.error), QStringLiteral("bad packet length"));
}

void ClipboardTransferClientTests::truncatedTransferRemovesPartialFile()
{
  const auto response = chunk(FileChunkType::Start, start("big.bin", 100)) + chunk(FileChunkType::Data, "partial");

  const auto result = download(response, "/src/big.bin", m_dir->path());

  QVERIFY(!result.success);
  QVERIFY2(allEntries(m_dir->path()).isEmpty(), qPrintable(allEntries(m_dir->path()).join(", ")));
}

void ClipboardTransferClientTests::moreDataThanAnnouncedFails()
{
  const auto response = chunk(FileChunkType::Start, start("a.txt", 3)) + chunk(FileChunkType::Data, "abc") +
                        chunk(FileChunkType::Data, "d") + chunk(FileChunkType::End, {});

  const auto result = download(response, "/src/a.txt", m_dir->path());

  QVERIFY(!result.success);
  QCOMPARE(QString::fromStdString(result.error), QStringLiteral("source sent more data than announced"));
  QVERIFY2(allEntries(m_dir->path()).isEmpty(), qPrintable(allEntries(m_dir->path()).join(", ")));
}

void ClipboardTransferClientTests::plantedSymlinkIsNotFollowed()
{
#ifdef Q_OS_WIN
  QSKIP("creating symlinks needs extra privileges on Windows");
#else
  QTemporaryDir victimDir;
  const auto victim = victimDir.filePath("victim.txt");
  QFile victimFile(victim);
  QVERIFY(victimFile.open(QIODevice::WriteOnly));
  victimFile.write("keep");
  victimFile.close();
  QVERIFY(QFile::link(victim, m_dir->filePath("a.txt.pasting")));

  const auto result = download(wholeFile("a.txt", "new"), "/src/a.txt", m_dir->path());

  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(victim), QByteArray("keep"));
  QCOMPARE(readFile(m_dir->filePath("a.txt")), QByteArray("new"));
#endif
}

void ClipboardTransferClientTests::sourceErrorIsReported()
{
  const auto result = download(chunk(FileChunkType::Error, "File not found"), "/src/gone.txt", m_dir->path());

  QVERIFY(!result.success);
  QVERIFY2(result.error.find("File not found") != std::string::npos, result.error.c_str());
}

QTEST_MAIN(ClipboardTransferClientTests)
