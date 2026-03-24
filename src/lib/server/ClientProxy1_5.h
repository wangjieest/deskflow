/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "server/ClientProxy1_4.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.5
class ClientProxy1_5 : public ClientProxy1_4
{
public:
  ClientProxy1_5(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ClientProxy1_5(ClientProxy1_5 const &) = delete;
  ClientProxy1_5(ClientProxy1_5 &&) = delete;
  ~ClientProxy1_5() override = default;

  ClientProxy1_5 &operator=(ClientProxy1_5 const &) = delete;
  ClientProxy1_5 &operator=(ClientProxy1_5 &&) = delete;

  void sendDragInfo(uint32_t fileCount, const char *info, size_t size) override;
  void fileChunkSending(uint8_t mark, char *data, size_t dataSize) override;
  bool parseMessage(const uint8_t *code) override;
  void fileChunkReceived() const;
  void dragInfoReceived() const;

  //! Handle file request from client (Client → Server direction)
  void handleFileRequest();

  //! Request a file from the client (Server → Client direction)
  /*!
  Sends a file request to the client. Used when the host (server) pastes
  files that were copied on a client machine.
  \param filePath the path of the file on the client
  \param relativePath the relative path for preserving directory structure
  \param isDir true if this is a directory entry
  \param sessionId the clipboard session ID for validation
  \return the request ID for tracking the transfer
  */
  uint32_t requestFileFromClient(
      const std::string &filePath, const std::string &relativePath, bool isDir, uint64_t sessionId
  );

  //! Handle file chunk received from client (for Client → Server transfer)
  void handleFileChunkFromClient();

  //! Check if there are pending file transfers
  bool hasPendingTransfers() const;

  //! Get completed file paths
  const std::vector<std::string> &getCompletedFilePaths() const;

  //! Clear completed file paths
  void clearCompletedFilePaths();

  //! Set transfer complete callback
  using TransferCompleteCallback = std::function<void()>;
  void setTransferCompleteCallback(const TransferCompleteCallback &callback);

private:
  //! Send directory response to client
  void sendDirectoryResponse(uint32_t requestId, const std::string &relativePath);

  //! Send file with extended metadata
  bool sendFileWithMetadata(
      deskflow::IStream *stream, uint32_t requestId, const std::string &filePath, const std::string &relativePath
  );

  //! File transfer state for receiving files from client
  struct FileReceiveState
  {
    uint32_t requestId = 0;
    std::string fileName;
    std::string relativePath;
    std::string destPath;
    uint64_t fileSize = 0;
    uint64_t bytesReceived = 0;
    bool isDir = false;
    std::vector<uint8_t> data;
  };

  std::map<uint32_t, FileReceiveState> m_fileReceives;
  std::vector<std::string> m_completedFilePaths;
  uint32_t m_pendingTransferCount = 0;
  std::string m_transferDestFolder;
  TransferCompleteCallback m_transferCompleteCallback;
};
