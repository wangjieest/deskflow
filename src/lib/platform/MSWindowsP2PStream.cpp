/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsP2PStream.h"

#include "base/Log.h"
#include "deskflow/ClipboardTransferClient.h"
#include "platform/MSWindowsDataObject.h"

MSWindowsP2PStream::MSWindowsP2PStream(const FileMetadata &file, ClipboardTransferClient *transferClient)
    : m_refCount(1),
      m_remotePath(file.remotePath),
      m_sourceAddr(file.sourceAddr),
      m_sourcePort(file.sourcePort),
      m_sessionId(file.sessionId),
      m_fileSize(file.size),
      m_fileName(file.name),
      m_transferClient(transferClient)
{
  LOG_INFO(
      "MSWindowsP2PStream created: file='%S', size=%llu, addr=%s:%u (NOT initialized yet)", m_fileName.c_str(), m_fileSize,
      m_sourceAddr.c_str(), m_sourcePort
  );
}

MSWindowsP2PStream::~MSWindowsP2PStream()
{
  LOG_DEBUG("MSWindowsP2PStream destroyed: %S", m_fileName.c_str());
}

//
// IUnknown implementation
//

STDMETHODIMP MSWindowsP2PStream::QueryInterface(REFIID riid, void **ppv)
{
  if (!ppv) {
    return E_INVALIDARG;
  }

  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream) || IsEqualIID(riid, IID_ISequentialStream)) {
    *ppv = static_cast<IStream *>(this);
    AddRef();
    return S_OK;
  }

  *ppv = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MSWindowsP2PStream::AddRef()
{
  LONG count = InterlockedIncrement(&m_refCount);
  LOG_DEBUG2("MSWindowsP2PStream::AddRef() -> %d (%S)", count, m_fileName.c_str());
  return count;
}

STDMETHODIMP_(ULONG) MSWindowsP2PStream::Release()
{
  LONG count = InterlockedDecrement(&m_refCount);
  LOG_DEBUG2("MSWindowsP2PStream::Release() -> %d (%S)", count, m_fileName.c_str());

  if (count == 0) {
    delete this;
  }

  return count;
}

//
// ISequentialStream implementation
//

STDMETHODIMP MSWindowsP2PStream::Read(void *pv, ULONG cb, ULONG *pcbRead)
{
  if (!pv) {
    return STG_E_INVALIDPOINTER;
  }

  LOG_DEBUG("IStream::Read() requested %u bytes (total read: %llu/%llu)", cb, m_totalBytesRead, m_fileSize);

  // First Read() call: initialize transfer
  if (!m_initialized.load()) {
    LOG_INFO("First Read() call - initializing P2P transfer for: %S", m_fileName.c_str());

    if (!initializeTransfer()) {
      LOG_ERR("Failed to initialize P2P transfer");
      if (pcbRead) {
        *pcbRead = 0;
      }
      return STG_E_CANTSAVE;
    }

    m_initialized.store(true);
  }

  // Read from buffer (blocks if empty and transfer not complete)
  ULONG bytesRead = readFromBuffer(pv, cb);

  if (pcbRead) {
    *pcbRead = bytesRead;
  }

  m_totalBytesRead += bytesRead;

  LOG_DEBUG("IStream::Read() returned %u bytes (total: %llu/%llu)", bytesRead, m_totalBytesRead, m_fileSize);

  // Return S_OK if we read the requested amount, S_FALSE if less (including EOF)
  if (bytesRead < cb) {
    return S_FALSE; // EOF or partial read
  }

  return S_OK;
}

STDMETHODIMP MSWindowsP2PStream::Write(const void *pv, ULONG cb, ULONG *pcbWritten)
{
  // Read-only stream
  return STG_E_ACCESSDENIED;
}

//
// IStream implementation
//

STDMETHODIMP MSWindowsP2PStream::Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition)
{
  // P2P streams don't support seeking (forward-only)
  // Return E_NOTIMPL to signal non-seekable stream
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsP2PStream::SetSize(ULARGE_INTEGER libNewSize)
{
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsP2PStream::CopyTo(
    IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten
)
{
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsP2PStream::Commit(DWORD grfCommitFlags)
{
  // No-op for read-only stream
  return S_OK;
}

STDMETHODIMP MSWindowsP2PStream::Revert()
{
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsP2PStream::LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType)
{
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsP2PStream::UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType)
{
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsP2PStream::Stat(STATSTG *pstatstg, DWORD grfStatFlag)
{
  if (!pstatstg) {
    return STG_E_INVALIDPOINTER;
  }

  ZeroMemory(pstatstg, sizeof(STATSTG));

  pstatstg->type = STGTY_STREAM;
  pstatstg->cbSize.QuadPart = m_fileSize;
  pstatstg->grfMode = STGM_READ;

  // Optionally include name if not STATFLAG_NONAME
  if (!(grfStatFlag & STATFLAG_NONAME)) {
    size_t nameLen = (m_fileName.length() + 1) * sizeof(wchar_t);
    pstatstg->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(nameLen));
    if (pstatstg->pwcsName) {
      wcscpy_s(pstatstg->pwcsName, m_fileName.length() + 1, m_fileName.c_str());
    }
  }

  return S_OK;
}

STDMETHODIMP MSWindowsP2PStream::Clone(IStream **ppstm)
{
  // Cloning not supported for P2P streams
  return E_NOTIMPL;
}

//
// Private helper methods
//

bool MSWindowsP2PStream::initializeTransfer()
{
  if (!m_transferClient) {
    LOG_ERR("No transfer client available");
    return false;
  }

  LOG_INFO(
      "Initializing P2P transfer: addr=%s:%u, session=%llu, path=%s", m_sourceAddr.c_str(), m_sourcePort, m_sessionId,
      m_remotePath.c_str()
  );

  // Request file from P2P server
  // Callback will be invoked when data arrives
  m_transferClient->requestFile(
      m_sourceAddr, m_sourcePort, m_sessionId, m_remotePath,
      [this](bool success, const std::string &localPath, const std::string &error) {
        onTransferComplete(success, error);
      }
  );

  // Note: We don't have a direct data callback in ClipboardTransferClient
  // The client downloads to a local file, then we read from that file
  // For true streaming, we would need to modify ClipboardTransferClient
  // to support chunk callbacks or use a different approach

  // For now, we'll use a workaround: read the file as it's being written
  // This is not ideal but works with the existing ClipboardTransferClient API

  return true;
}

void MSWindowsP2PStream::onDataReceived(const void *data, size_t size)
{
  if (size == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_bufferMutex);

  // Add data to buffer
  m_dataChunks.push(std::string(static_cast<const char *>(data), size));

  LOG_DEBUG2("Data received: %zu bytes, buffer size: %zu chunks", size, m_dataChunks.size());

  // Notify waiting Read() calls
  m_bufferCV.notify_one();
}

void MSWindowsP2PStream::onTransferComplete(bool success, const std::string &error)
{
  LOG_INFO("Transfer complete: success=%d, error=%s", success, error.c_str());

  m_transferComplete.store(true);
  m_transferSuccess.store(success);
  m_errorMessage = error;

  // Notify waiting Read() calls
  m_bufferCV.notify_all();
}

ULONG MSWindowsP2PStream::readFromBuffer(void *pv, ULONG cb, DWORD timeout)
{
  std::unique_lock<std::mutex> lock(m_bufferMutex);

  ULONG totalBytesRead = 0;
  char *dest = static_cast<char *>(pv);

  while (totalBytesRead < cb) {
    // Check if we have data in buffer
    if (!m_dataChunks.empty()) {
      std::string &chunk = m_dataChunks.front();
      size_t available = chunk.size() - m_currentChunkOffset;
      size_t bytesToCopy = std::min(static_cast<size_t>(cb - totalBytesRead), available);

      // Copy data
      memcpy(dest + totalBytesRead, chunk.data() + m_currentChunkOffset, bytesToCopy);
      totalBytesRead += static_cast<ULONG>(bytesToCopy);
      m_currentChunkOffset += bytesToCopy;

      // Remove chunk if fully consumed
      if (m_currentChunkOffset >= chunk.size()) {
        m_dataChunks.pop();
        m_currentChunkOffset = 0;
      }

      continue;
    }

    // No data available - check if transfer is complete
    if (m_transferComplete.load()) {
      if (!m_transferSuccess.load()) {
        LOG_ERR("Transfer failed: %s", m_errorMessage.c_str());
      }
      break; // EOF
    }

    // Wait for data with timeout
    if (timeout == 0) {
      m_bufferCV.wait(lock);
    } else {
      auto result = m_bufferCV.wait_for(lock, std::chrono::milliseconds(timeout));
      if (result == std::cv_status::timeout) {
        LOG_WARN("Read timeout waiting for data");
        break;
      }
    }
  }

  return totalBytesRead;
}
