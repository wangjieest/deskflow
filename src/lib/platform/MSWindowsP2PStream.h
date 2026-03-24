/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objidl.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

class ClipboardTransferClient;
struct FileMetadata;

/**
 * @brief Lazy-loading IStream implementation for P2P file transfer
 *
 * Implements the IStream interface to provide on-demand file download
 * from a remote Deskflow P2P connection. The actual download begins when
 * Explorer first calls Read(), not when the stream is created.
 *
 * Features:
 * - Lazy initialization: connection established on first Read()
 * - Buffered streaming: downloads data in chunks
 * - Thread-safe: can be called from multiple threads
 * - Progress support: reports size for Explorer progress UI
 *
 * Flow:
 *   1. Explorer calls IStream::Read()
 *   2. First call: establish P2P connection and start download
 *   3. Subsequent calls: return buffered data
 *   4. Download completes: return S_FALSE to signal EOF
 */
class MSWindowsP2PStream : public IStream
{
public:
  /**
   * @brief Construct stream for file download
   *
   * @param file File metadata (size, name, P2P info)
   * @param transferClient P2P transfer client (not owned)
   */
  MSWindowsP2PStream(const FileMetadata &file, ClipboardTransferClient *transferClient);

  virtual ~MSWindowsP2PStream();

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  // ISequentialStream
  STDMETHOD(Read)(void *pv, ULONG cb, ULONG *pcbRead) override;
  STDMETHOD(Write)(const void *pv, ULONG cb, ULONG *pcbWritten) override;

  // IStream
  STDMETHOD(Seek)(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override;
  STDMETHOD(SetSize)(ULARGE_INTEGER libNewSize) override;
  STDMETHOD(CopyTo)(IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten) override;
  STDMETHOD(Commit)(DWORD grfCommitFlags) override;
  STDMETHOD(Revert)() override;
  STDMETHOD(LockRegion)(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override;
  STDMETHOD(UnlockRegion)(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override;
  STDMETHOD(Stat)(STATSTG *pstatstg, DWORD grfStatFlag) override;
  STDMETHOD(Clone)(IStream **ppstm) override;

private:
  /**
   * @brief Initialize P2P connection and start download
   *
   * Called on first Read(). Starts async download that fills the buffer.
   *
   * @return true if connection succeeded
   */
  bool initializeTransfer();

  /**
   * @brief Callback for receiving downloaded data
   *
   * Called by ClipboardTransferClient when data arrives.
   * Adds data to buffer and signals waiting Read() calls.
   *
   * @param data Downloaded chunk
   * @param size Chunk size
   */
  void onDataReceived(const void *data, size_t size);

  /**
   * @brief Callback for transfer completion
   *
   * @param success true if transfer completed successfully
   * @param error Error message if failed
   */
  void onTransferComplete(bool success, const std::string &error);

  /**
   * @brief Read from buffer (blocking if empty)
   *
   * Waits for data to arrive if buffer is empty and transfer not complete.
   *
   * @param pv Output buffer
   * @param cb Bytes requested
   * @param timeout Timeout in milliseconds (0 = infinite)
   * @return Bytes actually read
   */
  ULONG readFromBuffer(void *pv, ULONG cb, DWORD timeout = 5000);

private:
  LONG m_refCount;

  // File metadata
  std::string m_remotePath;
  std::string m_sourceAddr;
  uint16_t m_sourcePort;
  uint64_t m_sessionId;
  uint64_t m_fileSize;
  std::wstring m_fileName;

  // Transfer client
  ClipboardTransferClient *m_transferClient; // Not owned

  // Transfer state
  std::atomic<bool> m_initialized{false};
  std::atomic<bool> m_transferComplete{false};
  std::atomic<bool> m_transferSuccess{false};
  std::string m_errorMessage;

  // Data buffer
  std::queue<std::string> m_dataChunks;
  size_t m_currentChunkOffset = 0;
  uint64_t m_totalBytesRead = 0;
  std::mutex m_bufferMutex;
  std::condition_variable m_bufferCV;
};
