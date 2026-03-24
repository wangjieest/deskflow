/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "DiscoveryService.h"

#include <QHostInfo>
#include <QNetworkInterface>
#include <QUuid>
#include <QDateTime>

namespace deskflow::gui::discovery {

bool PeerNode::isOnline() const
{
  return QDateTime::currentSecsSinceEpoch() - lastSeen < DiscoveryService::kNodeTimeout;
}

DiscoveryService::DiscoveryService(QObject *parent)
    : QObject(parent)
{
  // Generate unique node ID
  m_nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);

  // Default node name is hostname
  m_nodeName = QHostInfo::localHostName();

  // Setup timers
  m_announceTimer = new QTimer(this);
  m_announceTimer->setInterval(kAnnounceInterval);
  connect(m_announceTimer, &QTimer::timeout, this, &DiscoveryService::onAnnounceTimer);

  m_cleanupTimer = new QTimer(this);
  m_cleanupTimer->setInterval(kAnnounceInterval);
  connect(m_cleanupTimer, &QTimer::timeout, this, &DiscoveryService::onCleanupTimer);
}

DiscoveryService::~DiscoveryService()
{
  stop();
}

void DiscoveryService::setToken(const QString &token)
{
  m_token = token;
  m_tokenHash = computeTokenHash(token);
}

void DiscoveryService::setNodeName(const QString &name)
{
  m_nodeName = name;
}

void DiscoveryService::setListenPort(uint16_t port)
{
  m_listenPort = port;
}

bool DiscoveryService::start()
{
  if (m_running) {
    return true;
  }

  if (m_token.isEmpty()) {
    Q_EMIT error(tr("Token not set"));
    return false;
  }

  qDebug() << "[DISCOVERY] Starting discovery service, token:" << m_token << "nodeId:" << m_nodeId;

  // Create and bind UDP socket
  m_socket = new QUdpSocket(this);

  // Enable broadcast
  m_socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);

  // Bind to discovery port
  if (!m_socket->bind(QHostAddress::Any, kDiscoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
    Q_EMIT error(tr("Failed to bind to discovery port %1: %2")
                     .arg(kDiscoveryPort)
                     .arg(m_socket->errorString()));
    delete m_socket;
    m_socket = nullptr;
    return false;
  }

  connect(m_socket, &QUdpSocket::readyRead, this, &DiscoveryService::onReadyRead);

  // Start timers
  m_announceTimer->start();
  m_cleanupTimer->start();

  m_running = true;

  // Send initial announce
  sendAnnounce();

  return true;
}

void DiscoveryService::stop()
{
  if (!m_running) {
    return;
  }

  m_running = false;

  // Stop timers
  m_announceTimer->stop();
  m_cleanupTimer->stop();

  // Release master if we are one
  if (m_role == Role::Master) {
    sendMasterRelease();
  }

  // Close socket
  if (m_socket) {
    m_socket->close();
    delete m_socket;
    m_socket = nullptr;
  }

  // Clear peers
  m_peers.clear();

  setRole(Role::Idle);
}

void DiscoveryService::claimMaster()
{
  if (!m_running) {
    qDebug() << "[DISCOVERY] Cannot claim master - service not running";
    return;
  }

  m_claimTimestamp = QDateTime::currentSecsSinceEpoch();
  // Don't set role to Master yet - wait until server is ready and we send NOTIFY
  qDebug() << "[DISCOVERY] Broadcasting MASTER_CLAIM (intent to become master), timestamp:" << m_claimTimestamp;
  sendMasterClaim();
}

void DiscoveryService::notifyMasterReady()
{
  if (!m_running) {
    qDebug() << "[DISCOVERY] Cannot notify master ready - service not running";
    return;
  }

  // Now we are truly the master
  setRole(Role::Master);
  qDebug() << "[DISCOVERY] Server is ready, broadcasting MASTER_NOTIFY to peers";
  sendMasterNotify();
}

void DiscoveryService::releaseMaster()
{
  if (!m_running || m_role != Role::Master) {
    return;
  }

  qDebug() << "[DISCOVERY] Releasing master role, broadcasting MASTER_RELEASE";
  sendMasterRelease();
  setRole(Role::Idle);
}

QList<PeerNode> DiscoveryService::peers() const
{
  return m_peers.values();
}

PeerNode* DiscoveryService::peer(const QString &nodeId)
{
  auto it = m_peers.find(nodeId);
  if (it != m_peers.end()) {
    return &it.value();
  }
  return nullptr;
}

void DiscoveryService::onReadyRead()
{
  while (m_socket->hasPendingDatagrams()) {
    QByteArray data;
    data.resize(m_socket->pendingDatagramSize());
    QHostAddress sender;
    quint16 senderPort;

    m_socket->readDatagram(data.data(), data.size(), &sender, &senderPort);
    handleMessage(data, sender);
  }
}

void DiscoveryService::onAnnounceTimer()
{
  sendAnnounce();
}

void DiscoveryService::onCleanupTimer()
{
  qint64 now = QDateTime::currentSecsSinceEpoch();
  QStringList lostNodes;

  for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
    if (now - it->lastSeen >= kNodeTimeout) {
      lostNodes.append(it.key());
    }
  }

  for (const QString &nodeId : lostNodes) {
    QString nodeName = m_peers[nodeId].nodeName;
    qWarning() << "[DISCOVERY] Peer lost (timeout) - Name:" << nodeName
               << "NodeID:" << nodeId << "Last seen:"
               << (now - m_peers[nodeId].lastSeen) << "seconds ago";
    m_peers.remove(nodeId);
    Q_EMIT peerLost(nodeId);
  }

  if (!lostNodes.isEmpty()) {
    qDebug() << "[DISCOVERY] Remaining peers:" << m_peers.size();
  }
}

QString DiscoveryService::getLocalIpAddress() const
{
  const auto interfaces = QNetworkInterface::allInterfaces();
  for (const auto &iface : interfaces) {
    if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
        iface.flags().testFlag(QNetworkInterface::IsRunning) &&
        !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
      const auto addresses = iface.addressEntries();
      for (const auto &addr : addresses) {
        if (addr.ip().protocol() == QAbstractSocket::IPv4Protocol) {
          return addr.ip().toString();
        }
      }
    }
  }
  return QStringLiteral("127.0.0.1");
}

void DiscoveryService::sendAnnounce()
{
  if (!m_socket) {
    return;
  }

  QString localIp = getLocalIpAddress();

  // Build message: DESKFLOW|ANNOUNCE|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>|<role>
  QString message = QString("DESKFLOW|ANNOUNCE|%1|%2|%3|%4|%5|%6|%7")
                        .arg(kProtocolVersion)
                        .arg(m_tokenHash)
                        .arg(m_nodeId)
                        .arg(m_nodeName)
                        .arg(localIp)
                        .arg(m_listenPort)
                        .arg(roleToString(m_role));

  qDebug() << "[DISCOVERY] Broadcasting ANNOUNCE - Name:" << m_nodeName
           << "IP:" << localIp << "Port:" << m_listenPort
           << "Role:" << roleToString(m_role) << "Peers:" << m_peers.size();

  QByteArray data = message.toUtf8();
  m_socket->writeDatagram(data, QHostAddress::Broadcast, kDiscoveryPort);
}

void DiscoveryService::sendMasterClaim()
{
  if (!m_socket) {
    return;
  }

  QString localIp = getLocalIpAddress();

  // Build message: DESKFLOW|MASTER_CLAIM|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>|<timestamp>
  QString message = QString("DESKFLOW|MASTER_CLAIM|%1|%2|%3|%4|%5|%6|%7")
                        .arg(kProtocolVersion)
                        .arg(m_tokenHash)
                        .arg(m_nodeId)
                        .arg(m_nodeName)
                        .arg(localIp)
                        .arg(m_listenPort)
                        .arg(m_claimTimestamp);

  qInfo() << "[DISCOVERY] Broadcasting MASTER_CLAIM - Name:" << m_nodeName
          << "IP:" << localIp << "Port:" << m_listenPort
          << "Timestamp:" << m_claimTimestamp << "Peers will connect to us";

  QByteArray data = message.toUtf8();
  m_socket->writeDatagram(data, QHostAddress::Broadcast, kDiscoveryPort);
}

void DiscoveryService::sendMasterNotify()
{
  if (!m_socket) {
    return;
  }

  QString localIp = getLocalIpAddress();

  // Build message: DESKFLOW|MASTER_NOTIFY|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>
  QString message = QString("DESKFLOW|MASTER_NOTIFY|%1|%2|%3|%4|%5|%6")
                        .arg(kProtocolVersion)
                        .arg(m_tokenHash)
                        .arg(m_nodeId)
                        .arg(m_nodeName)
                        .arg(localIp)
                        .arg(m_listenPort);

  qInfo() << "[DISCOVERY] Broadcasting MASTER_NOTIFY - Name:" << m_nodeName
          << "IP:" << localIp << "Port:" << m_listenPort
          << "Server is ready, peers should connect now";

  QByteArray data = message.toUtf8();
  m_socket->writeDatagram(data, QHostAddress::Broadcast, kDiscoveryPort);
}

void DiscoveryService::sendMasterRelease()
{
  if (!m_socket) {
    return;
  }

  // Build message: DESKFLOW|MASTER_RELEASE|<ver>|<tokenHash>|<nodeId>
  QString message = QString("DESKFLOW|MASTER_RELEASE|%1|%2|%3")
                        .arg(kProtocolVersion)
                        .arg(m_tokenHash)
                        .arg(m_nodeId);

  qInfo() << "[DISCOVERY] Broadcasting MASTER_RELEASE - NodeID:" << m_nodeId
          << "Stepping down as master, peers are now free";

  QByteArray data = message.toUtf8();
  m_socket->writeDatagram(data, QHostAddress::Broadcast, kDiscoveryPort);
}

void DiscoveryService::handleMessage(const QByteArray &data, const QHostAddress &sender)
{
  QString message = QString::fromUtf8(data);
  QStringList parts = message.split('|');

  if (parts.size() < 4 || parts[0] != "DESKFLOW") {
    return;  // Invalid message
  }

  QString type = parts[1];
  int version = parts[2].toInt();
  QString tokenHash = parts[3];

  // Check protocol version
  if (version != kProtocolVersion) {
    return;  // Incompatible version
  }

  // Check token hash - ignore messages from different groups
  if (tokenHash != m_tokenHash) {
    return;
  }

  if (type == "ANNOUNCE") {
    handleAnnounce(parts, sender);
  } else if (type == "MASTER_CLAIM") {
    handleMasterClaim(parts);
  } else if (type == "MASTER_NOTIFY") {
    handleMasterNotify(parts);
  } else if (type == "MASTER_RELEASE") {
    handleMasterRelease(parts);
  }
}

void DiscoveryService::handleAnnounce(const QStringList &parts, const QHostAddress &sender)
{
  // DESKFLOW|ANNOUNCE|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>|<role>
  if (parts.size() < 9) {
    return;
  }

  QString nodeId = parts[4];
  QString nodeName = parts[5];
  QString ip = parts[6];
  uint16_t port = parts[7].toUShort();
  Role role = stringToRole(parts[8]);

  // Ignore our own announcements
  if (nodeId == m_nodeId) {
    return;
  }

  qint64 now = QDateTime::currentSecsSinceEpoch();
  bool isNew = !m_peers.contains(nodeId);

  PeerNode &peer = m_peers[nodeId];
  bool changed = isNew || peer.role != role || peer.ip != ip || peer.port != port;

  peer.nodeId = nodeId;
  peer.nodeName = nodeName;
  peer.ip = ip;
  peer.port = port;
  peer.role = role;
  peer.lastSeen = now;

  if (isNew) {
    qInfo() << "[DISCOVERY] New peer discovered - Name:" << nodeName
            << "NodeID:" << nodeId << "IP:" << ip << ":" << port
            << "Role:" << roleToString(role) << "Total peers:" << m_peers.size();
    Q_EMIT peerDiscovered(peer);
  } else if (changed) {
    qDebug() << "[DISCOVERY] Peer updated - Name:" << nodeName
             << "IP:" << ip << ":" << port << "Role:" << roleToString(role);
    Q_EMIT peerUpdated(peer);
  }
}

void DiscoveryService::handleMasterClaim(const QStringList &parts)
{
  // DESKFLOW|MASTER_CLAIM|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>|<timestamp>
  if (parts.size() < 9) {
    return;
  }

  QString nodeId = parts[4];
  QString nodeName = parts[5];
  QString ip = parts[6];
  uint16_t port = parts[7].toUShort();
  qint64 timestamp = parts[8].toLongLong();

  // Ignore our own claims
  if (nodeId == m_nodeId) {
    return;
  }

  // Update peer info
  qint64 now = QDateTime::currentSecsSinceEpoch();
  PeerNode &peer = m_peers[nodeId];
  peer.nodeId = nodeId;
  peer.nodeName = nodeName;
  peer.ip = ip;
  peer.port = port;
  peer.lastSeen = now;

  // Helper lambda: check if other node wins the conflict
  // Later timestamp wins; if equal, smaller nodeId wins (deterministic tie-breaker)
  auto otherWins = [&]() {
    if (timestamp > m_claimTimestamp) return true;
    if (timestamp < m_claimTimestamp) return false;
    return nodeId < m_nodeId;
  };

  // If we are currently master, check if we should yield
  if (m_role == Role::Master) {
    if (otherWins()) {
      qWarning() << "[DISCOVERY] Master conflict - Other node wins"
                 << "(Their ts:" << timestamp << "Ours:" << m_claimTimestamp
                 << "Their id:" << nodeId << "Ours:" << m_nodeId << ")"
                 << "- Yielding to" << nodeName;

      m_pendingMasterNodeId = nodeId;
      m_pendingMasterIp = ip;
      m_pendingMasterPort = port;

      Q_EMIT shouldYieldMaster(nodeId, nodeName);
    } else {
      qDebug() << "[DISCOVERY] Master conflict - We win, keeping master role";
    }
    return;
  }

  // If we also claimed but not yet master
  if (m_claimTimestamp > 0) {
    if (otherWins()) {
      qDebug() << "[DISCOVERY] Concurrent CLAIM conflict - Other node wins"
               << "- Aborting our claim, waiting for their NOTIFY";
      m_claimTimestamp = 0;
      m_pendingMasterNodeId = nodeId;
      m_pendingMasterIp = ip;
      m_pendingMasterPort = port;
      Q_EMIT shouldYieldMaster(nodeId, nodeName);
    } else {
      qDebug() << "[DISCOVERY] Concurrent CLAIM conflict - We win"
               << "- Continuing with our claim";
    }
    return;
  }

  // We are not master and have no pending claim - just wait for NOTIFY
  qDebug() << "[DISCOVERY] Received MASTER_CLAIM from" << nodeName
           << "- Waiting for MASTER_NOTIFY before connecting";
  m_pendingMasterNodeId = nodeId;
  m_pendingMasterIp = ip;
  m_pendingMasterPort = port;
}

void DiscoveryService::handleMasterNotify(const QStringList &parts)
{
  // DESKFLOW|MASTER_NOTIFY|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>
  if (parts.size() < 8) {
    return;
  }

  QString nodeId = parts[4];
  QString nodeName = parts[5];
  QString ip = parts[6];
  uint16_t port = parts[7].toUShort();

  // Ignore our own notify
  if (nodeId == m_nodeId) {
    return;
  }

  // Update peer info
  qint64 now = QDateTime::currentSecsSinceEpoch();
  PeerNode &peer = m_peers[nodeId];
  peer.nodeId = nodeId;
  peer.nodeName = nodeName;
  peer.ip = ip;
  peer.port = port;
  peer.role = Role::Master;
  peer.lastSeen = now;

  // If we are master, we should have already yielded from CLAIM
  // But if we're still master (maybe missed the CLAIM), yield now
  if (m_role == Role::Master) {
    qWarning() << "[DISCOVERY] Received MASTER_NOTIFY while still master - yielding now";
    Q_EMIT shouldYieldMaster(nodeId, nodeName);
    return;
  }

  // If we already have a current master and it's the same node, ignore
  if (m_role == Role::Slave && m_currentMasterNodeId == nodeId) {
    qDebug() << "[DISCOVERY] Ignoring MASTER_NOTIFY from current master" << nodeName;
    return;
  }

  // Now connect to the new master
  qInfo() << "[DISCOVERY] Received MASTER_NOTIFY - Connecting to new master"
          << nodeName << "at" << ip << ":" << port;

  m_currentMasterNodeId = nodeId;
  m_pendingMasterNodeId.clear();
  m_pendingMasterIp.clear();
  m_pendingMasterPort = 0;

  setRole(Role::Slave);
  Q_EMIT masterClaimed(peer);
  Q_EMIT shouldConnectToMaster(ip, port);
}

void DiscoveryService::handleMasterRelease(const QStringList &parts)
{
  // DESKFLOW|MASTER_RELEASE|<ver>|<tokenHash>|<nodeId>
  if (parts.size() < 5) {
    return;
  }

  QString nodeId = parts[4];

  // Ignore our own release
  if (nodeId == m_nodeId) {
    return;
  }

  // Update peer role
  if (m_peers.contains(nodeId)) {
    m_peers[nodeId].role = Role::Idle;
    Q_EMIT peerUpdated(m_peers[nodeId]);
  }

  // If we were slave to this master, become idle
  if (m_role == Role::Slave) {
    setRole(Role::Idle);
  }
}

QString DiscoveryService::computeTokenHash(const QString &token) const
{
  QByteArray hash = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
  return hash.toHex().left(16);
}

QString DiscoveryService::roleToString(Role role) const
{
  switch (role) {
    case Role::Idle:   return "idle";
    case Role::Master: return "master";
    case Role::Slave:  return "slave";
  }
  return "idle";
}

Role DiscoveryService::stringToRole(const QString &str) const
{
  if (str == "master") return Role::Master;
  if (str == "slave")  return Role::Slave;
  return Role::Idle;
}

void DiscoveryService::setRole(Role role)
{
  if (m_role != role) {
    m_role = role;
    // Clear current master tracking when we become non-slave
    if (role != Role::Slave) {
      m_currentMasterNodeId.clear();
    }
    qInfo() << "[DISCOVERY] Role changed to:" << roleToString(role);
    Q_EMIT roleChanged(role);
  }
}

} // namespace deskflow::gui::discovery
