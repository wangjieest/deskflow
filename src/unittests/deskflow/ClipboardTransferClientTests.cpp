/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardTransferClientTests.h"

#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardMeta.h"
#include "deskflow/ClipboardTransferClient.h"
#include "deskflow/ClipboardTransferThread.h"
#include "deskflow/FileTransfer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>

#include <future>
#include <limits>

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
std::string start(const std::string &relativePath, uint64_t size, bool isDir = false, uint64_t offset = 0)
{
  return FileTransfer::createStartChunkEx(1, "ignored", relativePath, size, isDir, offset);
}

QByteArray wholeFile(const std::string &relativePath, const std::string &content)
{
  return chunk(FileChunkType::Start, start(relativePath, content.size())) + chunk(FileChunkType::Data, content) +
         chunk(FileChunkType::End, {});
}

struct Step
{
  QByteArray bytes;
  int delayMs = 0;
};
using Script = std::vector<Step>;

FileDownloadOptions singleAttempt()
{
  FileDownloadOptions options;
  options.maxAttempts = 1;
  return options;
}

// Plays one script per incoming connection while the real client downloads, recording each request
Result downloadScripted(
    const std::vector<Script> &connections, const std::string &remotePath, const QString &destDir,
    const FileDownloadOptions &options, bool keep = false, QList<QByteArray> *requests = nullptr
)
{
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, 0))
    return {false, {}, "listen failed"};

  const auto port = server.serverPort();
  auto pending = std::async(std::launch::async, [port, remotePath, dest = destDir.toStdString(), keep, options] {
    return ClipboardTransferClient::downloadFile("127.0.0.1", port, 42, remotePath, dest, keep, options);
  });

  for (const auto &script : connections) {
    if (!server.waitForNewConnection(10000))
      break;
    std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
    const auto requestSize = static_cast<qsizetype>(20 + remotePath.size());
    QByteArray request;
    while (request.size() < requestSize && socket->waitForReadyRead(5000))
      request += socket->readAll();
    if (socket->waitForReadyRead(50)) // a resume trailer split into its own segment
      request += socket->readAll();
    if (requests)
      requests->append(request);
    for (const auto &step : script) {
      if (step.delayMs > 0)
        QThread::msleep(static_cast<unsigned long>(step.delayMs));
      if (socket->state() != QAbstractSocket::ConnectedState)
        break;
      socket->write(step.bytes);
      socket->waitForBytesWritten(5000);
    }
    socket->disconnectFromHost();
    if (socket->state() != QAbstractSocket::UnconnectedState)
      socket->waitForDisconnected(5000);
  }
  return pending.get();
}

Result download(const QByteArray &response, const std::string &remotePath, const QString &destDir, bool keep = false)
{
  return downloadScripted({{{response}}}, remotePath, destDir, singleAttempt(), keep);
}

Script trickle(const QByteArray &bytes, int delayMs)
{
  Script script;
  for (const char c : bytes)
    script.push_back({QByteArray(1, c), delayMs});
  return script;
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

void ClipboardTransferClientTests::initTestCase()
{
  // The transfer thread's event queue and multiplexer use the global Arch
  m_arch.init();
}

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

void ClipboardTransferClientTests::copyWithoutFileListDropsOnlyFiles()
{
  Clipboard src;
  src.open(0);
  src.empty();
  src.add(IClipboard::Format::Text, "note");
  src.add(IClipboard::Format::FileList, R"([{"path":"/secret/a.txt"}])");

  Clipboard dst;
  copyWithoutFileList(src, dst);
  src.close();

  QVERIFY(dst.open(0));
  QVERIFY(dst.has(IClipboard::Format::Text));
  QCOMPARE(QString::fromStdString(dst.get(IClipboard::Format::Text)), QStringLiteral("note"));
  QVERIFY(!dst.has(IClipboard::Format::FileList));
  dst.close();
}

void ClipboardTransferClientTests::refusedConnectionFailsFast()
{
  quint16 closedPort = 0;
  {
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    closedPort = probe.serverPort();
  }

  QElapsedTimer timer;
  timer.start();
  const auto result =
      ClipboardTransferClient::downloadFile("127.0.0.1", closedPort, 42, "/src/a.txt", m_dir->path().toStdString(), false);

  QVERIFY(!result.success);
  QVERIFY2(result.error.rfind("cannot connect", 0) == 0, result.error.c_str());
  QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
}

void ClipboardTransferClientTests::relayTempPathsStayInTempFolder()
{
  const auto root = FileTransfer::pathFromUtf8(FileTransfer::getTempDirectory()).lexically_normal();
  const auto inside = [&root](const std::string &path) {
    const auto rel = FileTransfer::pathFromUtf8(path).lexically_normal().lexically_relative(root);
    return !rel.empty() && *rel.begin() != "..";
  };

  QVERIFY(inside(FileTransfer::createTempFilePath("../../escape.txt")));
  QVERIFY(inside(FileTransfer::createTempFilePath("..")));
  QVERIFY(inside(FileTransfer::createTempFilePathWithRelative("folder/../../../escape.txt", 7)));
  QVERIFY(inside(FileTransfer::createTempFilePathWithRelative("C:\\Windows\\evil.txt", 7)));
  QCOMPARE(
      FileTransfer::pathFromUtf8(FileTransfer::createTempFilePathWithRelative("folder/sub/a.txt", 7)).filename(),
      std::filesystem::path("a.txt")
  );
}

void ClipboardTransferClientTests::appendChunkRefusesMoreThanAnnounced()
{
  FileTransferRequest request{};
  request.fileSize = 4;
  FileTransfer::reserveFor(request);
  QVERIFY(FileTransfer::appendChunk(request, "abc"));
  QVERIFY(FileTransfer::appendChunk(request, "d"));
  QVERIFY(!FileTransfer::appendChunk(request, "e"));
  QCOMPARE(request.bytesTransferred, uint64_t{4});

  FileTransferRequest huge{};
  huge.fileSize = std::numeric_limits<uint64_t>::max();
  FileTransfer::reserveFor(huge);
  QVERIFY(huge.data.capacity() <= 64u * 1024 * 1024);
}

void ClipboardTransferClientTests::resumesAfterDroppedConnection()
{
  const Script first{{chunk(FileChunkType::Start, start("a.txt", 10)) + chunk(FileChunkType::Data, "hello")}};
  const Script second{
      {chunk(FileChunkType::Start, start("a.txt", 10, false, 5)) + chunk(FileChunkType::Data, "world") +
       chunk(FileChunkType::End, {})}
  };
  FileDownloadOptions options;
  options.retryDelay = std::chrono::milliseconds(10);
  QList<QByteArray> requests;

  const auto result = downloadScripted({first, second}, "/src/a.txt", m_dir->path(), options, false, &requests);

  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(m_dir->filePath("a.txt")), QByteArray("helloworld"));
  QCOMPARE(requests.size(), 2);
  QVERIFY(!requests[0].contains("OFS1"));
  QVERIFY(requests[1].endsWith(QByteArray("OFS1") + QByteArray(7, '\0') + QByteArray(1, '\x05')));
}

void ClipboardTransferClientTests::restartsWhenSourceIgnoresOffset()
{
  // A source that predates resuming sends the whole file again without echoing the offset
  const Script first{{chunk(FileChunkType::Start, start("a.txt", 10)) + chunk(FileChunkType::Data, "hello")}};
  const Script second{{wholeFile("a.txt", "helloworld")}};
  FileDownloadOptions options;
  options.retryDelay = std::chrono::milliseconds(10);

  const auto result = downloadScripted({first, second}, "/src/a.txt", m_dir->path(), options);

  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(m_dir->filePath("a.txt")), QByteArray("helloworld"));
}

void ClipboardTransferClientTests::stalledSourceIsDropped()
{
  FileDownloadOptions options = singleAttempt();
  options.stallWindow = std::chrono::milliseconds(300);
  options.minBytesPerWindow = 64;
  Script script{{chunk(FileChunkType::Start, start("a.txt", 20))}};
  const auto dribble = trickle(chunk(FileChunkType::Data, std::string(20, 'x')), 100);
  script.insert(script.end(), dribble.begin(), dribble.end());

  QElapsedTimer timer;
  timer.start();
  const auto result = downloadScripted({script}, "/src/a.txt", m_dir->path(), options);

  QVERIFY(!result.success);
  QCOMPARE(QString::fromStdString(result.error), QStringLiteral("source is too slow"));
  QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
  QVERIFY2(allEntries(m_dir->path()).isEmpty(), qPrintable(allEntries(m_dir->path()).join(", ")));
}

void ClipboardTransferClientTests::slowButSteadySourceIsKept()
{
  FileDownloadOptions options = singleAttempt();
  options.stallWindow = std::chrono::milliseconds(300);
  options.minBytesPerWindow = 16;
  std::string content;
  Script script{{chunk(FileChunkType::Start, start("a.txt", 120))}};
  for (int i = 0; i < 12; ++i) {
    const std::string piece(10, static_cast<char>('a' + i));
    content += piece;
    script.push_back({chunk(FileChunkType::Data, piece), 100});
  }
  script.push_back({chunk(FileChunkType::End, {})});

  const auto result = downloadScripted({script}, "/src/a.txt", m_dir->path(), options);

  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(m_dir->filePath("a.txt")), QByteArray::fromStdString(content));
}

void ClipboardTransferClientTests::realServerServesAndResumes()
{
  QTemporaryDir sourceDir;
  const QString sourcePath = sourceDir.filePath("source.bin");
  QByteArray content;
  for (int i = 0; i < 100000; ++i)
    content.append(static_cast<char>(i % 251));
  QFile source(sourcePath);
  QVERIFY(source.open(QIODevice::WriteOnly));
  source.write(content);
  source.close();

  ClipboardTransferThread thread;
  QVERIFY(thread.start());
  const uint64_t session = newTransferSessionId();
  ClipboardTransferFileInfo info;
  info.path = sourcePath.toStdString();
  info.relativePath = "source.bin";
  info.size = static_cast<uint64_t>(content.size());
  thread.setAvailableFiles(session, {info});
  const auto port = thread.getServerPort();

  // The session is registered asynchronously on the transfer thread
  Result result;
  for (int i = 0; i < 30; ++i) {
    result = ClipboardTransferClient::downloadFile(
        "127.0.0.1", port, session, info.path, m_dir->path().toStdString(), false, singleAttempt()
    );
    if (result.success || result.error.find("not allowed") == std::string::npos)
      break;
    QThread::msleep(100);
  }
  QVERIFY2(result.success, result.error.c_str());
  QCOMPARE(readFile(m_dir->filePath("source.bin")), content);

  // Raw resume request: the Start chunk must echo the offset and only the tail may follow
  const uint64_t offset = 60000;
  QByteArray request("P2P ");
  request += be32(static_cast<uint32_t>(session >> 32)) + be32(static_cast<uint32_t>(session));
  request += be32(7) + be32(static_cast<uint32_t>(info.path.size())) + QByteArray::fromStdString(info.path);
  request += QByteArray("OFS1") + be32(0) + be32(static_cast<uint32_t>(offset));
  QTcpSocket socket;
  socket.connectToHost(QHostAddress::LocalHost, port);
  QVERIFY(socket.waitForConnected(5000));
  socket.write(request);
  QVERIFY(socket.waitForBytesWritten(5000));

  QByteArray buffer;
  QByteArray tail;
  QString startJson;
  bool ended = false;
  while (!ended && socket.waitForReadyRead(5000)) {
    buffer += socket.readAll();
    while (true) {
      qsizetype at = 0;
      if (!buffer.startsWith("DFCH")) {
        if (buffer.size() < 4)
          break;
        at = 4; // ProtocolUtil::writef length prefix
      }
      if (buffer.size() < at + 13)
        break;
      const auto *h = reinterpret_cast<const uint8_t *>(buffer.constData() + at);
      const uint32_t len = (uint32_t(h[9]) << 24) | (uint32_t(h[10]) << 16) | (uint32_t(h[11]) << 8) | h[12];
      if (buffer.size() < at + 13 + len)
        break;
      const auto type = static_cast<FileChunkType>(h[8]);
      const QByteArray payload = buffer.mid(at + 13, len);
      buffer.remove(0, at + 13 + len);
      if (type == FileChunkType::Start)
        startJson = QString::fromUtf8(payload.mid(payload.indexOf('{')));
      else if (type == FileChunkType::Data)
        tail += payload;
      else if (type == FileChunkType::End)
        ended = true;
    }
  }
  QVERIFY(ended);
  QVERIFY2(startJson.contains(QStringLiteral("\"offset\":60000")), qPrintable(startJson));
  QCOMPARE(tail, content.mid(static_cast<qsizetype>(offset)));
  thread.stop();
}

void ClipboardTransferClientTests::sourceErrorIsReported()
{
  const auto result = download(chunk(FileChunkType::Error, "File not found"), "/src/gone.txt", m_dir->path());

  QVERIFY(!result.success);
  QVERIFY2(result.error.find("File not found") != std::string::npos, result.error.c_str());
}

QTEST_MAIN(ClipboardTransferClientTests)
