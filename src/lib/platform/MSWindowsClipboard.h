/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 AutoDeskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class IMSWindowsClipboardConverter;
class IMSWindowsClipboardFacade;
class ClipboardTransferClient;

//! Microsoft windows clipboard implementation
class MSWindowsClipboard : public IClipboard
{
public:
  MSWindowsClipboard(HWND window);
  MSWindowsClipboard(HWND window, IMSWindowsClipboardFacade &facade);
  ~MSWindowsClipboard() override;

  //! Empty clipboard without ownership
  /*!
  Take ownership of the clipboard and clear all data from it.
  This must be called between a successful open() and close().
  Return false if the clipboard ownership could not be taken;
  the clipboard should not be emptied in this case.  Unlike
  empty(), isOwnedByDeskflow() will return false when emptied
  this way.  This is useful when deskflow wants to put data on
  clipboard but pretend (to itself) that some other app did it.
  When using empty(), deskflow assumes the data came from the
  server and doesn't need to be sent back.  emptyUnowned()
  makes deskflow send the data to the server.
  */
  bool emptyUnowned();

  //! Test if clipboard is owned by deskflow
  static bool isOwnedByDeskflow();

  //! Set transfer client for P2P file streaming
  void setTransferClient(ClipboardTransferClient *client) { m_transferClient = client; }

  // IClipboard overrides
  bool empty() override;
  void add(Format, const std::string &data) override;
  bool open(Time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format) const override;
  std::string get(Format) const override;

  void setFacade(IMSWindowsClipboardFacade &facade);

private:
  void clearConverters();

  /**
   * @brief Add file list using IDataObject streaming
   *
   * Parses JSON file list and creates IDataObject for delayed rendering.
   * Falls back to standard conversion if P2P info is missing.
   *
   * @param jsonData JSON array of file metadata
   * @return true if IDataObject was successfully set
   */
  bool addFileListAsIDataObject(const std::string &jsonData);

  /**
   * @brief Add text using IDataObject delayed rendering
   *
   * @param text Text data
   * @return true if IDataObject was successfully set
   */
  bool addTextAsIDataObject(const std::string &text);

  /**
   * @brief Add HTML/RTF using IDataObject delayed rendering
   *
   * @param html HTML data
   * @return true if IDataObject was successfully set
   */
  bool addHtmlAsIDataObject(const std::string &html);

  UINT convertFormatToWin32(Format) const;
  HANDLE convertTextToWin32(const std::string &data) const;
  std::string convertTextFromWin32(HANDLE) const;

  static UINT getOwnershipFormat();

private:
  using ConverterList = std::vector<IMSWindowsClipboardConverter *>;

  HWND m_window;
  mutable Time m_time;
  ConverterList m_converters;
  static UINT s_ownershipFormat;
  IMSWindowsClipboardFacade *m_facade;
  bool m_deleteFacade;
  ClipboardTransferClient *m_transferClient; // Not owned
};

//! Clipboard format converter interface
/*!
This interface defines the methods common to all win32 clipboard format
converters.
*/
class IMSWindowsClipboardConverter
{
public:
  virtual ~IMSWindowsClipboardConverter() = default;
  // accessors

  // return the clipboard format this object converts from/to
  virtual IClipboard::Format getFormat() const = 0;

  // return the atom representing the win32 clipboard format that
  // this object converts from/to
  virtual UINT getWin32Format() const = 0;

  // convert from the IClipboard format to the win32 clipboard format.
  // the input data must be in the IClipboard format returned by
  // getFormat().  the return data will be in the win32 clipboard
  // format returned by getWin32Format(), allocated by GlobalAlloc().
  virtual HANDLE fromIClipboard(const std::string &) const = 0;

  // convert from the win32 clipboard format to the IClipboard format
  // (i.e., the reverse of fromIClipboard()).
  virtual std::string toIClipboard(HANDLE data) const = 0;
};
