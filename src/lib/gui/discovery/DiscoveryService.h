/*
 * AutoDeskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QMap>
#include <QHostAddress>
#include <QCryptographicHash>

namespace deskflow::gui::discovery {

//! Node role in the network
enum class Role {
  Idle,    //!< Not connected, waiting
  Master,  //!< Controlling other nodes (keyboard/mouse source)
  Slave    //!< Being controlled by master
};

//! Information about a discovered peer node
struct PeerNode {
  QString nodeId;       //!< Unique node identifier (UUID)
  QString nodeName;     //!< Human-readable name (hostname)
  QString ip;           //!< IP address
  uint16_t port;        //!< TCP port for connection
  Role role;            //!< Current role
  qint64 lastSeen;      //!< Timestamp of last heartbeat

  //! Check if node is still online (seen within 15 seconds)
  bool isOnline() const;
};

//! UDP-based discovery service for finding peer nodes
/*!
This service handles:
- Periodic ANNOUNCE broadcasts to advertise presence
- Receiving and filtering announcements from peers with matching token
- MASTER_CLAIM broadcasts when becoming the controller
- Automatic role switching when another node claims master

Protocol messages (pipe-separated):
- DESKFLOW|ANNOUNCE|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>|<role>
- DESKFLOW|MASTER_CLAIM|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>|<timestamp>
  (Request to become master - larger timestamp wins)
- DESKFLOW|MASTER_NOTIFY|<ver>|<tokenHash>|<nodeId>|<nodeName>|<ip>|<port>
  (Server is ready - clients should connect now)
- DESKFLOW|MASTER_RELEASE|<ver>|<tokenHash>|<nodeId>
*/
class DiscoveryService : public QObject
{
  Q_OBJECT

public:
  //! Discovery UDP port (default TCP port + 1)
  static constexpr uint16_t kDiscoveryPort = 24801;

  //! Protocol version
  static constexpr int kProtocolVersion = 1;

  //! Announce interval in milliseconds
  static constexpr int kAnnounceInterval = 5000;

  //! Node timeout in seconds (no heartbeat = offline)
  static constexpr int kNodeTimeout = 15;

  explicit DiscoveryService(QObject *parent = nullptr);
  ~DiscoveryService() override;

  //! Set the group token for filtering peers
  void setToken(const QString &token);
  QString token() const { return m_token; }

  //! Set this node's display name
  void setNodeName(const QString &name);
  QString nodeName() const { return m_nodeName; }

  //! Set the TCP port this node listens on
  void setListenPort(uint16_t port);
  uint16_t listenPort() const { return m_listenPort; }

  //! Start discovery service
  bool start();

  //! Stop discovery service
  void stop();

  //! Check if service is running
  bool isRunning() const { return m_running; }

  //! Claim master role (broadcast intent to become the controller)
  //! This just announces intent - actual master status is set when server starts
  void claimMaster();

  //! Notify that we are now the master and ready for connections
  //! Call this when server is listening and ready
  void notifyMasterReady();

  //! Release master role
  void releaseMaster();

  //! Get current role
  Role currentRole() const { return m_role; }

  //! Check if this node is currently the master
  bool isMaster() const { return m_role == Role::Master; }

  //! Get list of discovered peers
  QList<PeerNode> peers() const;

  //! Get peer by node ID
  PeerNode* peer(const QString &nodeId);

  //! Get this node's unique ID
  QString nodeId() const { return m_nodeId; }

Q_SIGNALS:
  //! Emitted when a new peer is discovered
  void peerDiscovered(const PeerNode &peer);

  //! Emitted when a peer goes offline
  void peerLost(const QString &nodeId);

  //! Emitted when a peer's information is updated
  void peerUpdated(const PeerNode &peer);

  //! Emitted when another node claims master
  void masterClaimed(const PeerNode &master);

  //! Emitted when we should yield master role (another node's claim wins)
  //! The current host should stop server and prepare to become client
  void shouldYieldMaster(const QString &winnerNodeId, const QString &winnerName);

  //! Emitted when this node should connect to a new master
  //! (received MASTER_NOTIFY from the new host)
  void shouldConnectToMaster(const QString &ip, uint16_t port);

  //! Emitted when role changes
  void roleChanged(Role newRole);

  //! Emitted on errors
  void error(const QString &message);

private Q_SLOTS:
  void onReadyRead();
  void onAnnounceTimer();
  void onCleanupTimer();

private:
  void sendAnnounce();
  void sendMasterClaim();
  void sendMasterNotify();
  void sendMasterRelease();

  void handleMessage(const QByteArray &data, const QHostAddress &sender);
  void handleAnnounce(const QStringList &parts, const QHostAddress &sender);
  void handleMasterClaim(const QStringList &parts);
  void handleMasterNotify(const QStringList &parts);
  void handleMasterRelease(const QStringList &parts);

  QString computeTokenHash(const QString &token) const;
  QString roleToString(Role role) const;
  Role stringToRole(const QString &str) const;

  void setRole(Role role);

  QString getLocalIpAddress() const;

  QUdpSocket *m_socket = nullptr;
  QTimer *m_announceTimer = nullptr;
  QTimer *m_cleanupTimer = nullptr;

  QString m_token;
  QString m_tokenHash;
  QString m_nodeId;
  QString m_nodeName;
  uint16_t m_listenPort = 24800;
  Role m_role = Role::Idle;
  qint64 m_claimTimestamp = 0;
  bool m_running = false;
  QString m_currentMasterNodeId;  //!< NodeId of current master (when we are slave)
  QString m_pendingMasterNodeId;  //!< NodeId of pending master (waiting for NOTIFY)
  QString m_pendingMasterIp;      //!< IP of pending master
  uint16_t m_pendingMasterPort = 0;  //!< Port of pending master

  QMap<QString, PeerNode> m_peers;
};

} // namespace deskflow::gui::discovery
