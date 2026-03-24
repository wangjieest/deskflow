/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objidl.h>
#include <shlobj.h>

#include <string>
#include <vector>

class ClipboardTransferClient;

/**
 * @brief Format enumerator for IDataObject::EnumFormatEtc
 */
class MSWindowsFormatEnumerator : public IEnumFORMATETC
{
public:
  explicit MSWindowsFormatEnumerator(const std::vector<FORMATETC> &formats);

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  // IEnumFORMATETC
  STDMETHOD(Next)(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) override;
  STDMETHOD(Skip)(ULONG celt) override;
  STDMETHOD(Reset)() override;
  STDMETHOD(Clone)(IEnumFORMATETC **ppenum) override;

private:
  ~MSWindowsFormatEnumerator();

  LONG m_refCount;
  std::vector<FORMATETC> m_formats;
  size_t m_current;
};

//! File metadata for P2P transfer
struct FileMetadata {
  std::wstring name;         // Display file name
  std::wstring relativePath; // Relative path for subdirectories
  std::string remotePath;    // Remote file path on source machine
  uint64_t size;             // File size in bytes
  bool isDir;                // Is directory flag
  std::string sourceAddr;    // P2P source IP address
  uint16_t sourcePort;       // P2P source port
  uint64_t sessionId;        // P2P session ID for authentication
};

/**
 * @brief Windows IDataObject implementation for delayed clipboard rendering
 *
 * Implements IDataObject + IDataObjectAsyncCapability to provide delayed rendering of:
 * - Files (CFSTR_FILEDESCRIPTOR + CFSTR_FILECONTENTS)
 * - Text (CF_UNICODETEXT)
 * - Rich Text (CF_RTF)
 *
 * Features:
 * - Async capability support (IDataObjectAsyncCapability)
 * - Instant paste response (no waiting for download)
 * - Standard Windows progress UI with real-time updates
 * - Streaming transfer (low memory usage, direct to destination)
 * - Cancel support (ESC key)
 * - E_PENDING support for async operations
 *
 * Usage:
 *   std::vector<FileMetadata> files = ...;
 *   auto *pDataObject = new MSWindowsDataObject(files, transferClient);
 *   OleSetClipboard(pDataObject);
 *   pDataObject->Release();
 */
class MSWindowsDataObject : public IDataObject, public IDataObjectAsyncCapability {
public:
  /**
   * @brief Construct IDataObject with file metadata
   *
   * @param files List of file metadata
   * @param transferClient P2P transfer client for downloading (not owned)
   */
  MSWindowsDataObject(const std::vector<FileMetadata> &files, ClipboardTransferClient *transferClient);

  virtual ~MSWindowsDataObject();

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  // IDataObject
  STDMETHOD(GetData)(FORMATETC *pFormatEtc, STGMEDIUM *pMedium) override;
  STDMETHOD(GetDataHere)(FORMATETC *pFormatEtc, STGMEDIUM *pMedium) override;
  STDMETHOD(QueryGetData)(FORMATETC *pFormatEtc) override;
  STDMETHOD(GetCanonicalFormatEtc)(FORMATETC *pFormatIn, FORMATETC *pFormatOut) override;
  STDMETHOD(SetData)(FORMATETC *pFormatEtc, STGMEDIUM *pMedium, BOOL fRelease) override;
  STDMETHOD(EnumFormatEtc)(DWORD dwDirection, IEnumFORMATETC **ppEnum) override;
  STDMETHOD(DAdvise)(FORMATETC *pFormatEtc, DWORD advf, IAdviseSink *pAdvSink, DWORD *pdwConnection) override;
  STDMETHOD(DUnadvise)(DWORD dwConnection) override;
  STDMETHOD(EnumDAdvise)(IEnumSTATDATA **ppEnum) override;

  // IDataObjectAsyncCapability
  STDMETHOD(SetAsyncMode)(BOOL fDoOpAsync) override;
  STDMETHOD(GetAsyncMode)(BOOL *pfIsOpAsync) override;
  STDMETHOD(StartOperation)(IBindCtx *pbcReserved) override;
  STDMETHOD(EndOperation)(HRESULT hResult, IBindCtx *pbcReserved, DWORD dwEffects) override;
  STDMETHOD(InOperation)(BOOL *pfInAsyncOp) override;

private:
  /**
   * @brief Create FILEGROUPDESCRIPTOR for file list
   *
   * This is called when Explorer requests CFSTR_FILEDESCRIPTOR format.
   * Returns file metadata (names, sizes, attributes) without actual content.
   *
   * @param pMedium Output medium to store HGLOBAL with FILEGROUPDESCRIPTOR
   * @return S_OK on success
   */
  HRESULT createFileGroupDescriptor(STGMEDIUM *pMedium);

  /**
   * @brief Create IStream for file contents
   *
   * This is called when Explorer requests CFSTR_FILECONTENTS format.
   * Returns a lazy-loading IStream that downloads file on-demand.
   *
   * @param fileIndex Index of file in metadata list
   * @param pMedium Output medium to store IStream pointer
   * @return S_OK on success
   */
  HRESULT createFileContents(int fileIndex, STGMEDIUM *pMedium);

  /**
   * @brief Get text data (delayed)
   */
  HRESULT getTextData(STGMEDIUM *pMedium);

  /**
   * @brief Get RTF data (delayed)
   */
  HRESULT getRtfData(STGMEDIUM *pMedium);

  /**
   * @brief Register supported formats
   */
  void registerFormats();

private:
  LONG m_refCount;
  std::vector<FileMetadata> m_files;
  std::vector<FORMATETC> m_formats; // Supported formats for enumeration
  ClipboardTransferClient *m_transferClient; // Not owned

  // Clipboard data (for delayed rendering)
  std::string m_textData;
  std::string m_rtfData;

  // Clipboard format IDs
  static UINT s_cfFileDescriptor;
  static UINT s_cfFileContents;
  static UINT s_cfRtf;

  // Async operation state
  BOOL m_asyncMode;
  BOOL m_inOperation;
};
