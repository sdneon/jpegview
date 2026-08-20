#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
#include <atlstr.h>
#include <atltypes.h>
#include <hash_map>
#include "HashCompareLPCTSTR.h"

typedef stdext::hash_map<LPCTSTR, LPCTSTR, CHashCompareLPCTSTR> IniHashMap;

class CIniManager
{
private:
	CString m_sIniFilePath;
	IniHashMap* m_pUserKeys;
	TCHAR* m_pIniUserSectionBuffer;

private:
	// GetPrivateProfileSection/WritePrivateProfileString silently fall back to the ANSI
	// codepage for files without a UTF-16LE BOM, mangling non-Latin-1 characters (e.g. Chinese)
	void EnsureUnicodeIniFile();

public:
	CIniManager(CString& sIniFilePath);

	// Get the file name with path of the user INI file (in AppData path)
	LPCTSTR GetUserINIFileName() { return m_sIniFilePath; }
	LPCTSTR ReadIniString(LPCTSTR key, LPCTSTR fileName, IniHashMap*& keyMap, TCHAR*& pBuffer);

	void ReadIniFile(LPCTSTR fileName, IniHashMap* keyMap, TCHAR*& pBuffer);
	CString GetString(LPCTSTR sKey, LPCTSTR sDefault);
	CString GetString(int nKey, LPCTSTR sDefault);
	bool HasKey(LPCTSTR sKey);
	bool HasKey(int nKey);
	int GetInt(LPCTSTR sKey, int nDefault, int nMin, int nMax);
	double GetDouble(LPCTSTR sKey, double dDefault, double dMin, double dMax);
	bool GetBool(LPCTSTR sKey, bool bDefault);
	CRect GetRect(LPCTSTR sKey, const CRect& defaultRect);
	CSize GetSize(LPCTSTR sKey, const CSize& defaultSize);
	COLORREF GetColor(LPCTSTR sKey, COLORREF defaultColor, bool bReverse = false);
	void WriteString(LPCTSTR sKey, LPCTSTR sString);
	void WriteDouble(LPCTSTR sKey, double dValue);
	void WriteBool(LPCTSTR sKey, bool bValue);
	void WriteInt(LPCTSTR sKey, int nValue);
	// Drops the cached copy of the INI section so the next Get* call re-reads it from disk
	void Invalidate();
	// Removes every key in the section from the INI file
	void ClearSection();

	static double ParseTimeInterval(CString& strInterval);
};