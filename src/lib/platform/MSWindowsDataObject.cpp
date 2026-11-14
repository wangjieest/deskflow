/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsDataObject.h"

#include "base/Log.h"
#include "deskflow/ClipboardTransferClient.h"

#include <shlwapi.h>
#include <strsafe.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

// Initialize static members
UINT MSWindowsDataObject::s_cfFileDescriptor = 0;
UINT MSWindowsDataObject::s_cfFileContents = 0;
UINT MSWindowsDataObject::s_cfPreferredDropEffect = 0;
UINT MSWindowsDataObject::s_cfDeskflowOwnership = 0;
UINT MSWindowsDataObject::s_cfRtf = 0;

MSWindowsDataObject::MSWindowsDataObject(const std::vector<FileMetadata> &files, ClipboardTransferClient *transferClient)
    : m_refCount(1),
      m_files(files),
      m_transferClient(transferClient),
      m_asyncMode(TRUE),
      m_inOperation(FALSE)
{
  // Register clipboard formats on first use
  if (s_cfFileDescriptor == 0) {
    s_cfFileDescriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    s_cfFileContents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    s_cfPreferredDropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    s_cfDeskflowOwnership = RegisterClipboardFormatW(L"Deskflow Ownership");
    s_cfRtf = RegisterClipboardFormatW(L"Rich Text Format");
  }

  // Register supported formats for enumeration
  registerFormats();

  LOG_DEBUG("MSWindowsDataObject created with %zu files, async=%d", m_files.size(), m_asyncMode);
}

MSWindowsDataObject::~MSWindowsDataObject()
{
  LOG_DEBUG("MSWindowsDataObject destroyed");
}

//
// IUnknown implementation
//

STDMETHODIMP MSWindowsDataObject::QueryInterface(REFIID riid, void **ppv)
{
  if (!ppv) {
    return E_INVALIDARG;
  }

  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
    *ppv = static_cast<IDataObject *>(this);
    AddRef();
    return S_OK;
  }

  if (IsEqualIID(riid, IID_IDataObjectAsyncCapability)) {
    *ppv = static_cast<IDataObjectAsyncCapability *>(this);
    AddRef();
    return S_OK;
  }

  *ppv = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MSWindowsDataObject::AddRef()
{
  LONG count = InterlockedIncrement(&m_refCount);
  LOG_VERBOSE("MSWindowsDataObject::AddRef() -> %d", count);
  return count;
}

STDMETHODIMP_(ULONG) MSWindowsDataObject::Release()
{
  LONG count = InterlockedDecrement(&m_refCount);
  LOG_VERBOSE("MSWindowsDataObject::Release() -> %d", count);

  if (count == 0) {
    delete this;
  }

  return count;
}

//
// IDataObject implementation
//

STDMETHODIMP MSWindowsDataObject::GetData(FORMATETC *pFormatEtc, STGMEDIUM *pMedium)
{
  if (!pFormatEtc || !pMedium) {
    return E_INVALIDARG;
  }

  LOG_DEBUG("MSWindowsDataObject::GetData() format=%u, tymed=%u, lindex=%d", pFormatEtc->cfFormat, pFormatEtc->tymed, pFormatEtc->lindex);

  // FILEDESCRIPTOR: Return file metadata list
  if (pFormatEtc->cfFormat == s_cfFileDescriptor && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    LOG_DEBUG("GetData: returning FILEDESCRIPTOR for %zu files", m_files.size());
    return createFileGroupDescriptor(pMedium);
  }

  // FILECONTENTS: Return IStream for file content
  if (pFormatEtc->cfFormat == s_cfFileContents && (pFormatEtc->tymed & TYMED_ISTREAM)) {
    int fileIndex = pFormatEtc->lindex;

    if (fileIndex < 0 || fileIndex >= static_cast<int>(m_files.size())) {
      LOG_ERR("GetData: invalid file index %d (total: %zu)", fileIndex, m_files.size());
      return DV_E_LINDEX;
    }

    // Async mode: return E_PENDING if not in operation
    if (m_asyncMode && !m_inOperation) {
      LOG_DEBUG("GetData(FILECONTENTS): returning E_PENDING (async mode, not in operation)");
      return E_PENDING;
    }

    LOG_DEBUG("GetData: returning FILECONTENTS IStream for file index %d (%S)", fileIndex, m_files[fileIndex].name.c_str());
    return createFileContents(fileIndex, pMedium);
  }

  // CF_UNICODETEXT: Return text data
  if (pFormatEtc->cfFormat == CF_UNICODETEXT && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    if (!m_textData.empty()) {
      LOG_DEBUG("GetData: returning CF_UNICODETEXT");
      return getTextData(pMedium);
    }
  }

  // CF_RTF: Return RTF data
  if (pFormatEtc->cfFormat == s_cfRtf && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    if (!m_rtfData.empty()) {
      LOG_DEBUG("GetData: returning CF_RTF");
      return getRtfData(pMedium);
    }
  }

  // Preferred drop effect
  if (pFormatEtc->cfFormat == s_cfPreferredDropEffect && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    HGLOBAL hGlobal = GlobalAlloc(GHND, sizeof(DWORD));
    if (!hGlobal) return E_OUTOFMEMORY;
    auto *pEffect = static_cast<DWORD *>(GlobalLock(hGlobal));
    *pEffect = DROPEFFECT_COPY;
    GlobalUnlock(hGlobal);
    pMedium->tymed = TYMED_HGLOBAL;
    pMedium->hGlobal = hGlobal;
    pMedium->pUnkForRelease = nullptr;
    return S_OK;
  }

  // Deskflow Ownership marker
  if (pFormatEtc->cfFormat == s_cfDeskflowOwnership && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    HGLOBAL hGlobal = GlobalAlloc(GHND, 1);
    if (!hGlobal) return E_OUTOFMEMORY;
    pMedium->tymed = TYMED_HGLOBAL;
    pMedium->hGlobal = hGlobal;
    pMedium->pUnkForRelease = nullptr;
    return S_OK;
  }

  LOG_DEBUG("GetData: format not supported (cfFormat=%u)", pFormatEtc->cfFormat);
  return DV_E_FORMATETC;
}

STDMETHODIMP MSWindowsDataObject::GetDataHere(FORMATETC *pFormatEtc, STGMEDIUM *pMedium)
{
  // Not implemented - we provide data via GetData()
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsDataObject::QueryGetData(FORMATETC *pFormatEtc)
{
  if (!pFormatEtc) {
    return E_INVALIDARG;
  }

  // Support FILEDESCRIPTOR with HGLOBAL
  if (pFormatEtc->cfFormat == s_cfFileDescriptor && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    return !m_files.empty() ? S_OK : DV_E_FORMATETC;
  }

  // Support FILECONTENTS with ISTREAM
  if (pFormatEtc->cfFormat == s_cfFileContents && (pFormatEtc->tymed & TYMED_ISTREAM)) {
    int fileIndex = pFormatEtc->lindex;
    if (fileIndex >= 0 && fileIndex < static_cast<int>(m_files.size())) {
      // Skip directories
      if (m_files[fileIndex].isDir) {
        return DV_E_FORMATETC;
      }
      return S_OK;
    }
    return DV_E_LINDEX;
  }

  // Support CF_UNICODETEXT
  if (pFormatEtc->cfFormat == CF_UNICODETEXT && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    return !m_textData.empty() ? S_OK : DV_E_FORMATETC;
  }

  // Support CF_RTF
  if (pFormatEtc->cfFormat == s_cfRtf && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    return !m_rtfData.empty() ? S_OK : DV_E_FORMATETC;
  }

  // Support Preferred DropEffect
  if (pFormatEtc->cfFormat == s_cfPreferredDropEffect && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    return !m_files.empty() ? S_OK : DV_E_FORMATETC;
  }

  // Support Deskflow Ownership marker
  if (pFormatEtc->cfFormat == s_cfDeskflowOwnership && (pFormatEtc->tymed & TYMED_HGLOBAL)) {
    return S_OK;
  }

  return DV_E_FORMATETC;
}

STDMETHODIMP MSWindowsDataObject::GetCanonicalFormatEtc(FORMATETC *pFormatIn, FORMATETC *pFormatOut)
{
  if (!pFormatOut) {
    return E_INVALIDARG;
  }

  // No canonical format - return same as input
  pFormatOut->ptd = nullptr;
  return DATA_S_SAMEFORMATETC;
}

STDMETHODIMP MSWindowsDataObject::SetData(FORMATETC *pFormatEtc, STGMEDIUM *pMedium, BOOL fRelease)
{
  // Read-only data object
  return E_NOTIMPL;
}

STDMETHODIMP MSWindowsDataObject::EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppEnum)
{
  if (!ppEnum) {
    return E_INVALIDARG;
  }

  if (dwDirection != DATADIR_GET) {
    return E_NOTIMPL;
  }

  // Return custom enumerator with all registered formats
  *ppEnum = new MSWindowsFormatEnumerator(m_formats);
  LOG_DEBUG("EnumFormatEtc: returning enumerator with %zu formats", m_formats.size());

  return S_OK;
}

STDMETHODIMP MSWindowsDataObject::DAdvise(FORMATETC *pFormatEtc, DWORD advf, IAdviseSink *pAdvSink, DWORD *pdwConnection)
{
  // No advise support
  return OLE_E_ADVISENOTSUPPORTED;
}

STDMETHODIMP MSWindowsDataObject::DUnadvise(DWORD dwConnection)
{
  return OLE_E_ADVISENOTSUPPORTED;
}

STDMETHODIMP MSWindowsDataObject::EnumDAdvise(IEnumSTATDATA **ppEnum)
{
  return OLE_E_ADVISENOTSUPPORTED;
}

//
// Private helper methods
//

HRESULT MSWindowsDataObject::createFileGroupDescriptor(STGMEDIUM *pMedium)
{
  size_t fileCount = m_files.size();

  // Calculate size needed for FILEGROUPDESCRIPTOR structure
  size_t structSize = sizeof(FILEGROUPDESCRIPTORW) + (fileCount > 0 ? (fileCount - 1) : 0) * sizeof(FILEDESCRIPTORW);

  // Allocate global memory
  HGLOBAL hGlobal = GlobalAlloc(GHND, structSize);
  if (!hGlobal) {
    LOG_ERR("Failed to allocate memory for FILEGROUPDESCRIPTOR");
    return E_OUTOFMEMORY;
  }

  FILEGROUPDESCRIPTORW *pFGD = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(hGlobal));
  if (!pFGD) {
    GlobalFree(hGlobal);
    return E_OUTOFMEMORY;
  }

  // Fill in file count
  pFGD->cItems = static_cast<UINT>(fileCount);

  // Fill in each file descriptor
  for (size_t i = 0; i < fileCount; i++) {
    FILEDESCRIPTORW &fd = pFGD->fgd[i];
    const FileMetadata &file = m_files[i];

    // Initialize to zero
    ZeroMemory(&fd, sizeof(FILEDESCRIPTORW));

    // Set file name (use relative path if available, otherwise just name)
    // Explorer requires backslash separators for nested paths (e.g., "folder\file.txt")
    std::wstring displayName = file.relativePath.empty() ? file.name : file.relativePath;
    std::replace(displayName.begin(), displayName.end(), L'/', L'\\');
    StringCchCopyW(fd.cFileName, MAX_PATH, displayName.c_str());

    // Set file size
    fd.dwFlags |= FD_FILESIZE;
    fd.nFileSizeLow = static_cast<DWORD>(file.size & 0xFFFFFFFF);
    fd.nFileSizeHigh = static_cast<DWORD>((file.size >> 32) & 0xFFFFFFFF);

    // Enable progress UI
    fd.dwFlags |= FD_PROGRESSUI;

    // Set file attributes
    if (file.isDir) {
      fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
      fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }

    LOG_DEBUG("  File %zu: name='%S', size=%llu, isDir=%d", i, displayName.c_str(), file.size, file.isDir);
  }

  GlobalUnlock(hGlobal);

  // Fill in STGMEDIUM
  pMedium->tymed = TYMED_HGLOBAL;
  pMedium->hGlobal = hGlobal;
  pMedium->pUnkForRelease = nullptr;

  LOG_DEBUG("Created FILEGROUPDESCRIPTOR with %zu files", fileCount);
  return S_OK;
}

HRESULT MSWindowsDataObject::createFileContents(int fileIndex, STGMEDIUM *pMedium)
{
  const FileMetadata &file = m_files[fileIndex];

  if (file.isDir) {
    LOG_DEBUG("Skipping directory: %S", file.name.c_str());
    return DV_E_FORMATETC;
  }

  // The first GetData(FILECONTENTS) starts the batch; later and re-entrant calls wait for it
  downloadAllFiles();

  // Check if this file was downloaded successfully
  if (fileIndex >= static_cast<int>(m_localPaths.size()) || m_localPaths[fileIndex].empty()) {
    LOG_ERR("createFileContents[%d]: file not downloaded", fileIndex);
    return E_FAIL;
  }

  const std::string localPath = m_localPaths[fileIndex];
  LOG_DEBUG("createFileContents[%d]: returning IStream for %s", fileIndex, localPath.c_str());

  // Open the downloaded temp file as IStream
  int wideLen = MultiByteToWideChar(CP_UTF8, 0, localPath.c_str(), -1, nullptr, 0);
  if (wideLen <= 0) {
    LOG_ERR("createFileContents[%d]: invalid UTF-8 path", fileIndex);
    return E_FAIL;
  }
  std::wstring widePath(wideLen - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, localPath.c_str(), -1, widePath.data(), wideLen);

  IStream *pStream = nullptr;
  HRESULT hr = SHCreateStreamOnFileW(widePath.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, &pStream);
  if (FAILED(hr) || !pStream) {
    LOG_ERR("createFileContents[%d]: SHCreateStreamOnFile failed: 0x%08X", fileIndex, hr);
    return hr;
  }

  pMedium->tymed = TYMED_ISTREAM;
  pMedium->pstm = pStream;
  pMedium->pUnkForRelease = nullptr;
  return S_OK;
}

// Owned jointly with the download thread, so the event outlives whichever side finishes first
struct MSWindowsDataObject::BatchDownload
{
  HANDLE done = nullptr;
  std::mutex mutex;
  std::vector<std::string> paths; // index matches m_files

  ~BatchDownload()
  {
    if (done)
      CloseHandle(done);
  }
};

void MSWindowsDataObject::downloadAllFiles()
{
  // Explorer may re-enter GetData while the loop below dispatches messages; every caller waits on one batch
  if (!m_download) {
    if (!m_transferClient) {
      LOG_ERR("downloadAllFiles: no transfer client");
      return;
    }
    auto batch = std::make_shared<BatchDownload>();
    batch->done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!batch->done) {
      LOG_ERR("downloadAllFiles: CreateEvent failed: %lu", GetLastError());
      return;
    }
    batch->paths.resize(m_files.size());
    m_download = batch;

    const std::string destDir = m_transferClient->getTempDirectory();
    LOG_DEBUG("downloadAllFiles: downloading %zu item(s) into %s", m_files.size(), destDir.c_str());

    // One sequential thread; each download ends on its own once the source stalls
    std::thread([batch, files = m_files, destDir]() {
      for (size_t i = 0; i < files.size(); ++i) {
        const FileMetadata &file = files[i];
        if (file.isDir)
          continue;
        const auto result = ClipboardTransferClient::downloadFile(
            file.sourceAddr, file.sourcePort, file.sessionId, file.remotePath, destDir, false
        );
        if (!result.success) {
          LOG_ERR("downloadAllFiles[%zu]: %s", i, result.error.c_str());
          continue;
        }
        std::lock_guard<std::mutex> lock(batch->mutex);
        batch->paths[i] = result.localPath;
      }
      SetEvent(batch->done);
    }).detach();
  }

  const auto batch = m_download;
  constexpr ULONGLONG kTimeoutMs = 300000;
  const ULONGLONG start = GetTickCount64();
  while (true) {
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed >= kTimeoutMs) {
      LOG_ERR("downloadAllFiles: timed out, handing over what has arrived");
      break;
    }
    // Keep pumping so COM calls into this thread are served while we wait
    const DWORD wait =
        MsgWaitForMultipleObjects(1, &batch->done, FALSE, static_cast<DWORD>(kTimeoutMs - elapsed), QS_ALLINPUT);
    if (wait != WAIT_OBJECT_0 + 1)
      break;
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  std::lock_guard<std::mutex> lock(batch->mutex);
  m_localPaths = batch->paths;
}

//
// IDataObjectAsyncCapability implementation
//

STDMETHODIMP MSWindowsDataObject::SetAsyncMode(BOOL fDoOpAsync)
{
  LOG_DEBUG("MSWindowsDataObject::SetAsyncMode(%d)", fDoOpAsync);
  m_asyncMode = fDoOpAsync;
  return S_OK;
}

STDMETHODIMP MSWindowsDataObject::GetAsyncMode(BOOL *pfIsOpAsync)
{
  if (!pfIsOpAsync) {
    return E_INVALIDARG;
  }

  *pfIsOpAsync = m_asyncMode;
  LOG_VERBOSE("MSWindowsDataObject::GetAsyncMode() -> %d", m_asyncMode);
  return S_OK;
}

STDMETHODIMP MSWindowsDataObject::StartOperation(IBindCtx *pbcReserved)
{
  LOG_DEBUG("MSWindowsDataObject::StartOperation - Begin async operation");
  m_inOperation = TRUE;
  return S_OK;
}

STDMETHODIMP MSWindowsDataObject::EndOperation(HRESULT hResult, IBindCtx *pbcReserved, DWORD dwEffects)
{
  LOG_DEBUG("MSWindowsDataObject::EndOperation - End async operation (result=0x%08X)", hResult);
  m_inOperation = FALSE;
  return S_OK;
}

STDMETHODIMP MSWindowsDataObject::InOperation(BOOL *pfInAsyncOp)
{
  if (!pfInAsyncOp) {
    return E_INVALIDARG;
  }

  *pfInAsyncOp = m_inOperation;
  LOG_VERBOSE("MSWindowsDataObject::InOperation() -> %d", m_inOperation);
  return S_OK;
}

//
// Helper methods
//

void MSWindowsDataObject::registerFormats()
{
  LOG_DEBUG("MSWindowsDataObject::registerFormats");

  if (!m_files.empty()) {
    // FILEDESCRIPTOR format
    FORMATETC descriptor = {};
    descriptor.cfFormat = s_cfFileDescriptor;
    descriptor.dwAspect = DVASPECT_CONTENT;
    descriptor.lindex = -1;
    descriptor.tymed = TYMED_HGLOBAL;
    m_formats.push_back(descriptor);

    // FILECONTENTS formats (one per file, skip directories)
    for (LONG i = 0; i < static_cast<LONG>(m_files.size()); ++i) {
      if (m_files[i].isDir) {
        continue; // Skip directories
      }

      FORMATETC contents = {};
      contents.cfFormat = s_cfFileContents;
      contents.dwAspect = DVASPECT_CONTENT;
      contents.lindex = i;
      contents.tymed = TYMED_ISTREAM;
      m_formats.push_back(contents);
    }
  }

  // Preferred drop effect (copy)
  if (!m_files.empty()) {
    FORMATETC dropEffect = {};
    dropEffect.cfFormat = static_cast<CLIPFORMAT>(s_cfPreferredDropEffect);
    dropEffect.dwAspect = DVASPECT_CONTENT;
    dropEffect.lindex = -1;
    dropEffect.tymed = TYMED_HGLOBAL;
    m_formats.push_back(dropEffect);
  }

  // Deskflow Ownership marker - prevents main thread clipboard re-grab
  {
    FORMATETC ownership = {};
    ownership.cfFormat = static_cast<CLIPFORMAT>(s_cfDeskflowOwnership);
    ownership.dwAspect = DVASPECT_CONTENT;
    ownership.lindex = -1;
    ownership.tymed = TYMED_HGLOBAL;
    m_formats.push_back(ownership);
  }

  // Add text format if text data is available
  if (!m_textData.empty()) {
    FORMATETC text = {};
    text.cfFormat = CF_UNICODETEXT;
    text.dwAspect = DVASPECT_CONTENT;
    text.lindex = -1;
    text.tymed = TYMED_HGLOBAL;
    m_formats.push_back(text);
  }

  // Add RTF format if RTF data is available
  if (!m_rtfData.empty()) {
    FORMATETC rtf = {};
    rtf.cfFormat = s_cfRtf;
    rtf.dwAspect = DVASPECT_CONTENT;
    rtf.lindex = -1;
    rtf.tymed = TYMED_HGLOBAL;
    m_formats.push_back(rtf);
  }

  LOG_DEBUG("Registered %zu formats", m_formats.size());
}

HRESULT MSWindowsDataObject::getTextData(STGMEDIUM *pMedium)
{
  // Async mode: return E_PENDING if not in operation
  if (m_asyncMode && !m_inOperation) {
    LOG_DEBUG("getTextData: returning E_PENDING (async mode, not in operation)");
    return E_PENDING;
  }

  // Convert std::string to wide string
  std::wstring wtext(m_textData.begin(), m_textData.end());

  SIZE_T bytes = (wtext.size() + 1) * sizeof(wchar_t);
  HGLOBAL hGlobal = GlobalAlloc(GHND, bytes);
  if (!hGlobal) {
    return E_OUTOFMEMORY;
  }

  wchar_t *buffer = static_cast<wchar_t *>(GlobalLock(hGlobal));
  if (!buffer) {
    GlobalFree(hGlobal);
    return E_OUTOFMEMORY;
  }

  memcpy(buffer, wtext.c_str(), bytes);
  GlobalUnlock(hGlobal);

  pMedium->tymed = TYMED_HGLOBAL;
  pMedium->hGlobal = hGlobal;
  pMedium->pUnkForRelease = nullptr;

  LOG_INFO("Created text data: %zu characters", wtext.size());
  return S_OK;
}

HRESULT MSWindowsDataObject::getRtfData(STGMEDIUM *pMedium)
{
  // Async mode: return E_PENDING if not in operation
  if (m_asyncMode && !m_inOperation) {
    LOG_DEBUG("getRtfData: returning E_PENDING (async mode, not in operation)");
    return E_PENDING;
  }

  SIZE_T bytes = m_rtfData.size() + 1;
  HGLOBAL hGlobal = GlobalAlloc(GHND, bytes);
  if (!hGlobal) {
    return E_OUTOFMEMORY;
  }

  char *buffer = static_cast<char *>(GlobalLock(hGlobal));
  if (!buffer) {
    GlobalFree(hGlobal);
    return E_OUTOFMEMORY;
  }

  memcpy(buffer, m_rtfData.c_str(), bytes);
  GlobalUnlock(hGlobal);

  pMedium->tymed = TYMED_HGLOBAL;
  pMedium->hGlobal = hGlobal;
  pMedium->pUnkForRelease = nullptr;

  LOG_INFO("Created RTF data: %zu bytes", m_rtfData.size());
  return S_OK;
}

//
// MSWindowsFormatEnumerator implementation
//

MSWindowsFormatEnumerator::MSWindowsFormatEnumerator(const std::vector<FORMATETC> &formats)
    : m_refCount(1),
      m_formats(formats),
      m_current(0)
{
  LOG_VERBOSE("MSWindowsFormatEnumerator created with %zu formats", m_formats.size());
}

MSWindowsFormatEnumerator::~MSWindowsFormatEnumerator()
{
  LOG_VERBOSE("MSWindowsFormatEnumerator destroyed");
}

STDMETHODIMP MSWindowsFormatEnumerator::QueryInterface(REFIID riid, void **ppv)
{
  if (!ppv) {
    return E_INVALIDARG;
  }

  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFORMATETC)) {
    *ppv = static_cast<IEnumFORMATETC *>(this);
    AddRef();
    return S_OK;
  }

  *ppv = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MSWindowsFormatEnumerator::AddRef()
{
  return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) MSWindowsFormatEnumerator::Release()
{
  LONG count = InterlockedDecrement(&m_refCount);
  if (count == 0) {
    delete this;
  }
  return count;
}

STDMETHODIMP MSWindowsFormatEnumerator::Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched)
{
  if (!rgelt) {
    return E_INVALIDARG;
  }

  ULONG fetched = 0;
  while (m_current < m_formats.size() && fetched < celt) {
    rgelt[fetched] = m_formats[m_current];
    m_current++;
    fetched++;
  }

  if (pceltFetched) {
    *pceltFetched = fetched;
  }

  return (fetched == celt) ? S_OK : S_FALSE;
}

STDMETHODIMP MSWindowsFormatEnumerator::Skip(ULONG celt)
{
  m_current += celt;
  if (m_current > m_formats.size()) {
    m_current = m_formats.size();
  }
  return (m_current < m_formats.size()) ? S_OK : S_FALSE;
}

STDMETHODIMP MSWindowsFormatEnumerator::Reset()
{
  m_current = 0;
  return S_OK;
}

STDMETHODIMP MSWindowsFormatEnumerator::Clone(IEnumFORMATETC **ppenum)
{
  if (!ppenum) {
    return E_INVALIDARG;
  }

  auto *clone = new MSWindowsFormatEnumerator(m_formats);
  clone->m_current = m_current;
  *ppenum = clone;

  return S_OK;
}
