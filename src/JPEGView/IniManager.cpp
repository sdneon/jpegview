# include "IniReader.h"

static const TCHAR* SECTION_NAME_RECENTS = _T("JPEGView_Recents");

using namespace stdext;

CIniManager::CIniManager(CString& sIniFilePath) :
    m_sIniFilePath(sIniFilePath), m_pUserKeys(NULL), m_pIniUserSectionBuffer(NULL)
{
	EnsureUnicodeIniFile();
}

void CIniManager::EnsureUnicodeIniFile() {
	bool hasUnicodeBOM = false;
	HANDLE hFile = ::CreateFile(m_sIniFilePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE) {
		BYTE bom[2] = { 0, 0 };
		DWORD nRead = 0;
		::ReadFile(hFile, bom, sizeof(bom), &nRead, NULL);
		hasUnicodeBOM = (nRead == sizeof(bom) && bom[0] == 0xFF && bom[1] == 0xFE);
		::CloseHandle(hFile);
	}
	if (!hasUnicodeBOM) {
		// (Re-)create the file with just a UTF-16LE BOM so GetPrivateProfileSection/WritePrivateProfileString
		// read/write it as Unicode instead of falling back to the ANSI codepage
		hFile = ::CreateFile(m_sIniFilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			BYTE bom[2] = { 0xFF, 0xFE };
			DWORD nWritten = 0;
			::WriteFile(hFile, bom, sizeof(bom), &nWritten, NULL);
			::CloseHandle(hFile);
		}
	}
}

void CIniManager::Invalidate() {
	delete m_pUserKeys;
	m_pUserKeys = NULL;
	delete[] m_pIniUserSectionBuffer;
	m_pIniUserSectionBuffer = NULL;
}

void CIniManager::ReadIniFile(LPCTSTR fileName, IniHashMap* keyMap, TCHAR*& pBuffer) {
	int bufferSize = 1024 * 2;

	pBuffer = NULL;
	int actualSize;
	do {
		delete[] pBuffer;
		bufferSize = bufferSize * 2;
		pBuffer = new TCHAR[bufferSize];
		actualSize = ::GetPrivateProfileSection(SECTION_NAME_RECENTS, pBuffer, bufferSize, fileName);
	} while (actualSize == bufferSize - 2);

	int index = 0;
	LPTSTR current = pBuffer;
	// TODO not sure why the original author manually parses the file instead of using GetPrivateProfileString/Int APIs
	while (*current != 0) {
		while (*current != 0 && _istspace(*current)) current++;
		LPCTSTR key = current;
		while (*current != 0 && !_istspace(*current) && *current != _T('=')) current++;
		LPCTSTR value = current;
		if (*current != 0) {
			*current++ = 0;
			while (*current != 0 && _istspace(*current)) current++;
			value = current;
		}
		if (*key != 0) {
			(*keyMap)[key] = value;
		}
		current += _tcslen(value) + 1;
	}
}

LPCTSTR CIniManager::ReadIniString(LPCTSTR key, LPCTSTR fileName, IniHashMap*& keyMap, TCHAR*& pBuffer) {
	if (keyMap == NULL) {
		keyMap = new IniHashMap();
		ReadIniFile(m_sIniFilePath, keyMap, pBuffer);
	}
	hash_map<LPCTSTR, LPCTSTR, CHashCompareLPCTSTR>::const_iterator iter;
	iter = keyMap->find(key);
	if (iter == keyMap->end()) {
		return NULL; // not found
	}
	else {
		return iter->second;
	}
}

CString CIniManager::GetString(LPCTSTR sKey, LPCTSTR sDefault) {
	// finally global path if not found in user path
	LPCTSTR value = ReadIniString(sKey, m_sIniFilePath, m_pUserKeys, m_pIniUserSectionBuffer);
	if (value == NULL) {
		return CString(sDefault);
	}
	return CString(value);
}

CString CIniManager::GetString(int nKey, LPCTSTR sDefault) {
	TCHAR buf[2] = { TCHAR('0' + nKey), 0 };
	CString key(buf);
	LPCTSTR pKey = key;
	return GetString(key, sDefault);
}

bool CIniManager::HasKey(LPCTSTR sKey) {
	LPCTSTR value = ReadIniString(sKey, m_sIniFilePath, m_pUserKeys, m_pIniUserSectionBuffer);
	return value != NULL;
}

bool CIniManager::HasKey(int nKey) {
	// finally global path if not found in user path
	TCHAR buf[2] = { TCHAR('0' + nKey), 0 };
	CString key(buf);
	LPCTSTR pKey = key;
	return HasKey(key);
}

int CIniManager::GetInt(LPCTSTR sKey, int nDefault, int nMin, int nMax) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return nDefault;
	}
	int nValue = (int)_wtof((LPCTSTR)s);
	return min(nMax, max(nMin, nValue));
}

double CIniManager::GetDouble(LPCTSTR sKey, double dDefault, double dMin, double dMax) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return dDefault;
	}
	double dValue = _wtof((LPCTSTR)s);
	return min(dMax, max(dMin, dValue));
}

bool CIniManager::GetBool(LPCTSTR sKey, bool bDefault) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return bDefault;
	}
	if (s.CompareNoCase(_T("true")) == 0) {
		return true;
	}
	else if (s.CompareNoCase(_T("false")) == 0) {
		return false;
	}
	else {
		return bDefault;
	}
}

CRect CIniManager::GetRect(LPCTSTR sKey, const CRect& defaultRect) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return defaultRect;
	}
	int nLeft, nTop, nRight, nBottom;
	if (_stscanf((LPCTSTR)s, _T(" %d %d %d %d "), &nLeft, &nTop, &nRight, &nBottom) == 4) {
		CRect newRect = CRect(nLeft, nTop, nRight, nBottom);
		newRect.NormalizeRect();
		return newRect;
	}
	else {
		return defaultRect;
	}
}

CSize CIniManager::GetSize(LPCTSTR sKey, const CSize& defaultSize) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return defaultSize;
	}
	int nWidth, nHeight;
	if (_stscanf((LPCTSTR)s, _T(" %d %d "), &nWidth, &nHeight) == 2) {
		nWidth = max(1, nWidth);
		nHeight = max(1, nHeight);
		return CSize(nWidth, nHeight);
	}
	else {
		return defaultSize;
	}
}

COLORREF CIniManager::GetColor(LPCTSTR sKey, COLORREF defaultColor, bool bReverse) {
	int nRed, nGreen, nBlue;
	CString sColor = GetString(sKey, _T(""));
	if (sColor.IsEmpty()) {
		return defaultColor;
	}
	if (_stscanf(sColor, _T(" %d %d %d"), &nRed, &nGreen, &nBlue) == 3) {
		if (!bReverse)
			return RGB(nRed, nGreen, nBlue);
		else
			return RGB(nBlue, nGreen, nRed);
	}
	else {
		return defaultColor;
	}
}

void CIniManager::WriteString(LPCTSTR sKey, LPCTSTR sString) {
	::WritePrivateProfileString(SECTION_NAME_RECENTS, sKey, sString, m_sIniFilePath);
}

void CIniManager::WriteDouble(LPCTSTR sKey, double dValue) {
	TCHAR buff[32];
	_stprintf_s(buff, 32, _T("%.2f"), dValue);
	WriteString(sKey, buff);
}

void CIniManager::WriteBool(LPCTSTR sKey, bool bValue) {
	WriteString(sKey, bValue ? _T("true") : _T("false"));
}

void CIniManager::WriteInt(LPCTSTR sKey, int nValue) {
	TCHAR buff[32];
	_stprintf_s(buff, 32, _T("%d"), nValue);
	WriteString(sKey, buff);
}

void CIniManager::ClearSection() {
	// passing a NULL key name deletes the whole section
	::WritePrivateProfileString(SECTION_NAME_RECENTS, NULL, NULL, m_sIniFilePath);
	Invalidate();
}

double CIniManager::ParseTimeInterval(CString& strInterval) {
	if (strInterval.GetLength() <= 0)
		return NAN;
	double dMultiplier = 1.0;
	int nLastChIndex = strInterval.GetLength() - 1;
	wchar_t chUnits = strInterval[nLastChIndex];
	if (chUnits == 'm')
	{
		dMultiplier = 60.0;
		strInterval = strInterval.Left(nLastChIndex);
	}
	else if (chUnits == 'h')
	{
		dMultiplier = 3600.0;
		strInterval = strInterval.Left(nLastChIndex);
	}
	else if (chUnits == 's')
	{
		strInterval = strInterval.Left(nLastChIndex);
	}
	return _wtof((LPCTSTR)strInterval) * dMultiplier;
}