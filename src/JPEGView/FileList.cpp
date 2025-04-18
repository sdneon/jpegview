#include "StdAfx.h"
#include "FileList.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "DirectoryWatcher.h"
#include "Shlwapi.h"

///////////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////////

// static initializers
Helpers::ESorting CFileDesc::sm_eSorting = Helpers::FS_LastModTime;
bool CFileDesc::sm_bSortUpcounting = true;
Helpers::ENavigationMode CFileList::sm_eMode = Helpers::NM_LoopDirectory;

// Helper to add the current file of filefind object to the list
static void AddToFileList(std::list<CFileDesc> & fileList, CFindFile &fileFind, LPCTSTR expectedExtension, int nMinFilesize = 1, bool bHideHidden = false) {
	if (!fileFind.IsDirectory()) {
		if (expectedExtension != NULL) {
			// compare if the extension is the expected extension
			// this is required because a strange behavior of CFindFile: If searching for "*.tif", .tiff files are also found
			CString fileName = fileFind.GetFileName();
			int lastDot = fileName.ReverseFind(_T('.'));
			if (_tcsicmp(((LPCTSTR)fileName) + lastDot + 1, expectedExtension) != 0) {
				return;
			}
		}
		FILETIME lastWriteTime, creationTime;
		fileFind.GetLastWriteTime(&lastWriteTime);
		fileFind.GetCreationTime(&creationTime);
		if (fileFind.GetFileSize() >= nMinFilesize)
		{
			if (!bHideHidden || !fileFind.IsHidden())
			{
				CFileDesc thisFile(fileFind.GetFilePath(), &lastWriteTime, &creationTime, fileFind.GetFileSize());
				fileList.push_back(thisFile);
			}
		}
	}
}

static bool s_bUseLogicalStringCompare = true;
static bool s_bUseLogicalStringCompareValid = false;

// Used by method below
static bool IsNoLogicalStrCmpSetInRegistryHive(HKEY hKeyRoot) {
	HKEY key;
	if (RegOpenKeyEx(hKeyRoot, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer"), 0, KEY_READ, &key) == ERROR_SUCCESS) {
		DWORD value;
		DWORD size = sizeof(DWORD);
		DWORD type;
		if (RegQueryValueEx(key, _T("NoStrCmpLogical"), NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
			if (type == REG_DWORD) {
				RegCloseKey(key);
				return value == 1;
			}
		}
		RegCloseKey(key);
	}
	return false;
}

// Queries the registry if logical string comparing (file9.txt before file10.txt) shall be used.
// This is default in Windows XP and later.
static bool UseLogicalStringCompare() {
	if (!s_bUseLogicalStringCompareValid) {
		s_bUseLogicalStringCompareValid = true;
		s_bUseLogicalStringCompare = !(IsNoLogicalStrCmpSetInRegistryHive(HKEY_LOCAL_MACHINE) || 
			IsNoLogicalStrCmpSetInRegistryHive(HKEY_CURRENT_USER));
	}
	return s_bUseLogicalStringCompare;
}

///////////////////////////////////////////////////////////////////////////////////
// CFileDesc
///////////////////////////////////////////////////////////////////////////////////

CFileDesc::CFileDesc(const CString & sName, const FILETIME* lastModTime, const FILETIME* creationTime, __int64 fileSize) {
	m_sName = sName;
	m_sTitle = (LPCTSTR)m_sName + sName.ReverseFind(_T('\\')) + 1;
	memcpy(&m_lastModTime, lastModTime, sizeof(FILETIME));
	memcpy(&m_creationTime, creationTime, sizeof(FILETIME));
	m_nRandomOrderNumber = rand();
	m_fileSize = fileSize;
}

bool CFileDesc::SortUpcounting(const CFileDesc& other) const {
	if (sm_eSorting == Helpers::FS_CreationTime || sm_eSorting == Helpers::FS_LastModTime) {
		const FILETIME* pTime = (sm_eSorting == Helpers::FS_LastModTime) ? &m_lastModTime : &m_creationTime;
		const FILETIME* pTimeOther = (sm_eSorting == Helpers::FS_LastModTime) ? &(other.m_lastModTime) : &(other.m_creationTime);
		if (pTime->dwHighDateTime < pTimeOther->dwHighDateTime) {
			return true;
		} else if (pTime->dwHighDateTime > pTimeOther->dwHighDateTime) {
			return false;
		} else {
			return pTime->dwLowDateTime < pTimeOther->dwLowDateTime;
		}
	} else if (sm_eSorting == Helpers::FS_Random) {
		return m_nRandomOrderNumber < other.m_nRandomOrderNumber;
	} else if (sm_eSorting == Helpers::FS_FileSize) {
		return m_fileSize < other.m_fileSize;
	} else {
		if (UseLogicalStringCompare()) {
			// If the filename contains numbers, we want to sort the files
			// according to the numbers to place 'File9' before 'File10'
			return StrCmpLogicalW(m_sTitle, other.m_sTitle) < 0;
		} else {
			return _tcsicoll(m_sTitle, other.m_sTitle) < 0;
		}
	}
}

bool CFileDesc::operator < (const CFileDesc& other) const {
	return SortUpcounting(other) ^ (!sm_bSortUpcounting);
}


void CFileDesc::SetName(LPCTSTR sNewName) {
	m_sName = sNewName;
	m_sTitle = (LPCTSTR)m_sName + m_sName.ReverseFind(_T('\\')) + 1;
}

void CFileDesc::SetModificationDate(const FILETIME& lastModDate) {
	memcpy(&m_lastModTime, &lastModDate, sizeof(FILETIME));
}

///////////////////////////////////////////////////////////////////////////////////
// Public interface
///////////////////////////////////////////////////////////////////////////////////

// image file types supported internally (there are additional endings for RAW and WIC - these come from INI file)
// NOTE: when adding more supported filetypes, update installer to add another extension for "SupportedTypes"
static const int cnNumEndingsInternal = 20;
static const TCHAR* csFileEndingsInternal[cnNumEndingsInternal] = {
	_T("jpg"), _T("jpeg"), _T("png"),
	_T("cbz"), _T("cb7"),
	_T("avif"), _T("bmp"), _T("gif"), _T("heic"), _T("heif"), _T("ico"), _T("jfif"), _T("jxl"), _T("psb"), _T("psd"), _T("qoi"), _T("tif"), _T("tiff"), _T("tga"), _T("webp")};
// supported camera RAW formats
static const TCHAR* csFileEndingsRAW = _T("*.pef;*.dng;*.crw;*.nef;*.cr2;*.mrw;*.rw2;*.orf;*.x3f;*.arw;*.kdc;*.nrw;*.dcr;*.sr2;*.raf");


static const int MAX_ENDINGS = 48;
static int nNumEndings;
static LPCTSTR* sFileEndings;

__declspec(dllimport) bool __stdcall WICPresent(void);

// Check if Windows Image Codecs library is present
static bool WICPresentGuarded(void) {
	try {
		return WICPresent();
	} catch (...) {
		return false;
	}
}

// Parses a semicolon separated list of file endings of the form "*.nef;*.cr2;*.dng"
static void ParseAndAddFileEndings(LPCTSTR sEndings) {
	if (_tcslen(sEndings) > 2) {
		LPTSTR buffer = new TCHAR[_tcslen(sEndings) + 1]; // this buffer will not be freed deliberately!
		_tcscpy(buffer, sEndings);
		LPTSTR sStart = buffer, sCurrent = buffer;
		while (*sCurrent != 0 && nNumEndings < MAX_ENDINGS) {
			while (*sCurrent != 0 && *sCurrent != _T(';')) {
				sCurrent++;
			}
			if (*sCurrent == _T(';')) {
				*sCurrent = 0;
				sCurrent++;
			}
			if (_tcslen(sStart) > 2) {
				sFileEndings[nNumEndings++] = sStart + 2;
			}
			sStart = sCurrent;
		}
	}
}

// Gets all supported file endings, including the ones from WIC.
// The length of the returned list is nNumEndings
static LPCTSTR* GetSupportedFileEndingList() {
	if (sFileEndings == NULL) {
		sFileEndings = new LPCTSTR[MAX_ENDINGS];
		for (nNumEndings = 0; nNumEndings < cnNumEndingsInternal; nNumEndings++) {
			sFileEndings[nNumEndings] = csFileEndingsInternal[nNumEndings];
		}

		LPCTSTR sFileEndingsWIC = CSettingsProvider::This().FilesProcessedByWIC();
		if (_tcslen(sFileEndingsWIC) > 2 && WICPresentGuarded()) {
			ParseAndAddFileEndings(sFileEndingsWIC);
		}
		ParseAndAddFileEndings(CSettingsProvider::This().FileEndingsRAW());
	}
	return sFileEndings;
}

CFileList::CFileList(const CString & sInitialFile, CDirectoryWatcher & directoryWatcher, 
	Helpers::ESorting eInitialSorting, bool isSortedUpcounting, bool bWrapAroundFolder, int nLevel, bool forceSorting,
	int nMinFilesize, bool bHideHidden, bool bHideSameName)
	: m_directoryWatcher(directoryWatcher),
	m_nMinFilesize(nMinFilesize),
	m_bHideHidden(bHideHidden),
	m_bHideSameName(bHideSameName),
	m_bFindingFiles(false),
	m_bForceExitFindFiles(false)
{

	CFileDesc::SetSorting(eInitialSorting, isSortedUpcounting);
	m_bDeleteHistory = true;
	m_bWrapAroundFolder = bWrapAroundFolder;
	m_sInitialFile = sInitialFile;
	m_nLevel = nLevel;
	m_next = m_prev = NULL;
	int nPos = sInitialFile.ReverseFind(_T('\\'));
	m_sDirectory = (nPos > 0) ? sInitialFile.Left(nPos) : _T(""); // the backslash is stripped away!
	nPos = sInitialFile.ReverseFind(_T('.'));
	bool bIsDirectory = (::GetFileAttributes(sInitialFile) & FILE_ATTRIBUTE_DIRECTORY) != 0;
	CString sExtensionInitialFile = (nPos > 0) ? sInitialFile.Right(sInitialFile.GetLength()-nPos-1) : _T("");
	sExtensionInitialFile.MakeLower();
	bool bImageFile = !bIsDirectory && IsImageFile(sExtensionInitialFile);
	if (!bIsDirectory && !bImageFile && !sExtensionInitialFile.IsEmpty() && _tcsstr(csFileEndingsRAW, _T("*.") + sExtensionInitialFile) != NULL) {
		// initial file is a supported camera raw file but was excluded in INI file - add temporarily to file ending list
		ParseAndAddFileEndings(_T("*.") + sExtensionInitialFile);
		CSettingsProvider::This().AddTemporaryRAWFileEnding(sExtensionInitialFile);
		bImageFile = true;
	}
	m_bIsSlideShowList = !bIsDirectory && !bImageFile && TryReadingSlideShowList(sInitialFile);
	m_nMarkedIndexShow = -1;

	m_directoryWatcher.SetCurrentDirectory(m_sDirectory);

	if (!m_bIsSlideShowList) {
		if (bImageFile || bIsDirectory) {
			m_iterStart = m_iter = m_fileList.begin();
			if (bImageFile)
			{	//at least show 1st selected image 1st
				CFindFile fileFind;
				if (fileFind.FindFile(sInitialFile)) {
					AddToFileList(m_fileList, fileFind, sExtensionInitialFile, m_nMinFilesize, m_bHideHidden);
					m_iterStart = m_iter = m_fileList.begin();
				}
				else
					bImageFile = false;
			}
			//move potentially heavy search to separate thread
			m_bFindingFiles = true;
			m_taskFindFiles = std::async(std::launch::async, [this, &sInitialFile, &bWrapAroundFolder]() {
				FindFiles(m_fileList2);
				if (!m_bForceExitFindFiles) {
					if (sm_eMode == Helpers::NM_Auto)
						SetNavigationMode(sm_eMode);
					m_fileList.clear();
					m_fileList.splice(m_fileList.begin(), m_fileList2);
					m_fileList.sort();

					if (m_bHideSameName)
					{
						//Remove duplicate images with different extension
						CString lastName(_T(""));
						std::list<CFileDesc>::iterator iter;
						for (iter = m_fileList.begin(); iter != m_fileList.end();) {
							CString fileName = iter->GetName();
							int lastDot = fileName.ReverseFind(_T('.'));
							fileName = fileName.Left(lastDot);
							if (_tcsicmp((LPCTSTR)fileName, (LPCTSTR)lastName) == 0) {
								iter = m_fileList.erase(iter);
								if (iter == m_fileList.end()) {
									break;
								}
							}
							else iter++;
							lastName = fileName;
						}
					}

					m_iter = FindFile(sInitialFile);
					m_iterStart = bWrapAroundFolder ? m_iter : m_fileList.begin();
				}
				m_bFindingFiles = false;
				m_bForceExitFindFiles = false;
			});
		} else {
			// neither image file nor directory nor list of file names - try to read anyway but normally will fail
			CFindFile fileFind;
			if (fileFind.FindFile(sInitialFile)) {
				AddToFileList(m_fileList, fileFind, NULL, m_nMinFilesize, m_bHideHidden);
			}
			m_iter = m_iterStart = m_fileList.begin();
		}
	} else {
		sm_eMode = Helpers::NM_LoopDirectory;
		if (forceSorting) m_fileList.sort();
		m_iter = m_iterStart = m_fileList.begin();
	}
}

CFileList::~CFileList() {
	if (m_bDeleteHistory) {
		DeleteHistory();
	}
	m_fileList.clear();
}

CString CFileList::GetSupportedFileEndings() {
	LPCTSTR* allFileEndings = GetSupportedFileEndingList();
	CString sList;
	for (int i = 0; i < nNumEndings; i++) {
		sList += _T("*.");
		sList += allFileEndings[i];
		if (i+1 < nNumEndings) sList += _T(";");
	}
	return sList;
}

void CFileList::Reload(LPCTSTR sFileName, bool clearForwardHistory) {
	LPCTSTR sCurrent = sFileName;
	if (sCurrent == NULL) {
		// If async is still processing, quickly terminate it.
		m_bForceExitFindFiles = true;
		WaitIfNotReady();
		m_bForceExitFindFiles = false;

		sCurrent = Current();
		if (sCurrent == NULL) {
			m_fileList.clear();
			return;
		}
	}

	CString sCurrentFile = sCurrent;

	if (clearForwardHistory) {
		// delete the chain of CFileLists forward
		CFileList* pFileList = this->m_next;
		while (pFileList != NULL && pFileList->m_prev != NULL) {
			CFileList* thisList = pFileList;
			pFileList = pFileList->m_next;
			thisList->m_bDeleteHistory = false;
			delete thisList;
		}
		m_next = NULL;
	}

	if (!m_bIsSlideShowList) {
		FindFiles(m_fileList);
	} else {
		VerifyFiles(); // maybe some of the files got deleted or moved
	}
	m_iterStart = m_fileList.begin();
	m_iter = FindFile(sCurrentFile); // go again to current file
}

bool CFileList::CurrentFileExists() const {
	if (Current() != NULL) {
		return ::GetFileAttributes(Current()) != INVALID_FILE_ATTRIBUTES;
	}
	return false;
}

bool CFileList::DriveHasRecycleBin(LPCTSTR filePath) {
	TCHAR driveLetter[3];
	if (_tcslen(filePath) < 3) return false;
	driveLetter[0] = filePath[0];
	driveLetter[1] = filePath[1];
	driveLetter[2] = 0;

	if (driveLetter[0] == _T('\\') && driveLetter[1] == _T('\\')) return false; // remote drive, no recycle bin

	CString recycleBin = CString((LPCTSTR(driveLetter))) + _T("\\$Recycle.Bin\\");

	DWORD attributes = ::GetFileAttributes(recycleBin);
	if (attributes == INVALID_FILE_ATTRIBUTES) return false;
	if (attributes & FILE_ATTRIBUTE_DIRECTORY) return true; // this is a directory!

	return false; // this is not a directory!
}

bool CFileList::DeleteFile(LPCTSTR fileNameWithPath) const {
	if (fileNameWithPath != NULL) {
		// append a double null character
		TCHAR fileName[MAX_PATH + 1];
		_tcscpy(fileName, fileNameWithPath);
		fileName[_tcslen(fileName) + 1] = 0;

		SHFILEOPSTRUCT fileOp{ 0 };
		fileOp.hwnd = NULL;
		fileOp.wFunc = FO_DELETE;
		fileOp.pFrom = fileName;
		fileOp.pTo = NULL;
		fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
		fileOp.hNameMappings = NULL;
		if (::SHFileOperation(&fileOp) == 0 && !fileOp.fAnyOperationsAborted) {
			return true;
		}
	}
	return false;
}

void CFileList::FileHasRenamed(LPCTSTR sOldFileName, LPCTSTR sNewFileName) {
	if (_tcsicmp(sOldFileName, m_sInitialFile) == 0) {
		m_sInitialFile = sNewFileName;
	}
	std::list<CFileDesc>::iterator iter;
	WaitIfNotReady(); // If async is still processing, then wait.
	for (iter = m_fileList.begin(); iter != m_fileList.end(); iter++) {
		if (_tcsicmp(sOldFileName, iter->GetName()) == 0) {
			iter->SetName(sNewFileName);
		}
	}
}

void CFileList::ModificationTimeChanged() {
	WaitIfNotReady(); // If async is still processing, then wait.
	if (m_iter != m_fileList.end()) {
		LPCTSTR sName = m_iter->GetName();
		HANDLE hFile = ::CreateFile(sName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != NULL) {
			FILETIME lastModTime;
			if (::GetFileTime(hFile, NULL, NULL, &lastModTime)) {
				m_iter->SetModificationDate(lastModTime);
			}
			::CloseHandle(hFile);
		}
	}
}

CFileList* CFileList::Next() {
	m_nMarkedIndexShow = -1;
	if (m_fileList.size() > 0) {
		std::list<CFileDesc>::iterator iterTemp = m_iter;
		if (iterTemp == m_fileList.end())
			return this;
		iterTemp++;
		if (iterTemp == m_fileList.end()) {
			iterTemp = m_fileList.begin();
		}
		if (iterTemp == m_iterStart) {
			// we are finished with this folder
			if (!m_bWrapAroundFolder && sm_eMode == Helpers::NM_LoopDirectory) {
				return this;
			}
			CFileList* pNextList = WrapToNextImage();
			if (pNextList == NULL) {
				// no next found, go to source of the prev->prev chain
				pNextList = this;
				while (pNextList->m_prev != NULL) pNextList = pNextList->m_prev;
				if (pNextList != this) {
					return m_bWrapAroundFolder ? GotoFirstShown() : this; // stop here, do not wrap around
				}
			}
			if (pNextList != this) {
				// leave current iterator on m_iterStart and return the new list
				return pNextList;
			}
		}
		NextInFolder();
	} else {
		// current filelist is empty, try to go to another folder
		CFileList* pNextList = WrapToNextImage();
		if (pNextList != NULL) {
			return pNextList;
		}
	}
	return this;
}

CFileList* CFileList::AnyAvailableLast()
{
	if (m_prev != NULL)
		return m_prev;
	//else forcibily return last image anyway
	if (m_iter == m_fileList.begin()) {
		if (m_bWrapAroundFolder)
			MoveIterToLast();
	}
	else {
		m_iter--;
	}
	return this;
}

CFileList* CFileList::Prev() {
	m_nMarkedIndexShow = -1;
	if (m_iter == m_iterStart) {
		if (sm_eMode == Helpers::NM_LoopDirectory) {
			if (!m_bWrapAroundFolder) {
				return this;
			}
			if (m_iter == m_fileList.begin()) {
				MoveIterToLast();
			} else {
				m_iter--;
			}
			return this;
		} else {
			CFileList *pList = WrapToPrevImage();
			if (pList) return pList;
			return AnyAvailableLast();
		}
	}
	if (m_fileList.size() > 0) {
		if (m_iter == m_fileList.begin()) {
			m_iter = m_fileList.end();
			m_iter--;
		} else {
			m_iter--;
		}
	}
	return this;
}

CFileList* CFileList::NextFolder() {
	CFileList* pNextList = WrapToNextImage();
	if (pNextList == NULL) {
		// no next found, go to source of the prev->prev chain
		pNextList = this;
		while (pNextList->m_prev != NULL) pNextList = pNextList->m_prev;
		if (pNextList != this) {
			return m_bWrapAroundFolder ? GotoFirstShown() : this; // stop here, do not wrap around
		}
	}
	if (pNextList != NULL) {
		pNextList->First();
		return pNextList;
	}
	if (m_fileList.size() > 0) {
		Last();
	}
	return this;
}

CFileList* CFileList::PrevFolder() {
	CFileList* pNextList = WrapToPrevImage();
	if ((pNextList != NULL) && (pNextList != this)) {
		pNextList->Last();
		return pNextList;
	}
	if (m_fileList.size() > 0) {
		First();
	}
	return this;
}

void CFileList::First() {
	m_nMarkedIndexShow = -1;
	m_iter = m_iterStart = m_fileList.begin();
}

void CFileList::Last() {
	m_nMarkedIndexShow = -1;
	MoveIterToLast();
	m_iterStart = m_fileList.begin();
}

CFileList* CFileList::AwayFromCurrent() {
	WaitIfNotReady(); // If async is still processing, then wait.

	LPCTSTR sCurrentFile = Current();
	LPCTSTR sNextFile = PeekNextPrev(1, true, false);
	if (sCurrentFile == NULL || sNextFile == NULL || _tcscmp(sCurrentFile, sNextFile) == 0) {
		CFileList* pFileList = Prev();
		if (Current() != NULL && sCurrentFile != NULL && _tcscmp(sCurrentFile, Current()) == 0) {
			// not moved away, only one image
			m_iter = m_fileList.end();
		}
		return pFileList;
	} else {
		return Next();
	}
}

LPCTSTR CFileList::Current() const {
	if (m_fileList.size() == 0)
		WaitIfNotReady();
	if (m_iter != m_fileList.end()) {
		return m_iter->GetName();
	} else {
		return NULL;
	}
}

LPCTSTR CFileList::CurrentFileTitle() const {
	LPCTSTR sCurrent = Current();
	if (sCurrent != NULL) {
		LPCTSTR sTitle = _tcsrchr(sCurrent, _T('\\'));
		if (sTitle == NULL) sTitle = sCurrent; else sTitle++;
		return sTitle;
	} else {
		return NULL;
	}
}

LPCTSTR CFileList::CurrentDirectory() const {
	if (m_sDirectory.GetLength() > 0) {
		return m_sDirectory;
	} else {
		return NULL;
	}
}

CString CFileList::CurrentDirectoryName() const {
	if (m_sDirectory.GetLength() > 0) {
		int pos = m_sDirectory.ReverseFind('\\');
		if (pos >= 0)
		{
			int cnt = m_sDirectory.GetLength() - pos - 1;
			if (cnt > 0)
				return m_sDirectory.Right(cnt);
		}
	}
	return m_sDirectory;
}

CString CFileList::CurrentDirectoryNameShort() const {
	CString name = CurrentDirectoryName();
	int len = name.GetLength();
	if (len <= 44)
		return name;

	return name.Left(10) + "..." + name.Right(10);
}

CString CFileList::ShortName(CString path)
{
	int len = path.GetLength();
	if (len <= 23)
		return path;

	return path.Left(10) + "..." + path.Right(10);
}

CString CFileList::ShortBasePath(CString path)
{
	int len = path.GetLength();
	if (len > 0) {
		int pos = path.ReverseFind('\\');
		if (pos == (len - 1))
		{
			path = path.Left(pos);
			pos = path.ReverseFind('\\');
		}
		if (pos >= 0)
		{
			int cnt = len - pos - 1;
			if (cnt > 0)
			{
				CString name = ShortName(path.Right(cnt));
				path = path.Left(pos);
				len = path.GetLength();
				pos = path.ReverseFind('\\');
				if (pos >= 0)
				{
					cnt = len - pos - 1;
					if (cnt > 0)
					{
						name = ShortName(path.Right(cnt)) + "\\" + name;
					}
					return name;
				}
			}
		}
	}
	return ShortName(path);
}

int CFileList::CurrentIndex() const {
	int i = 0;
	std::list<CFileDesc>::const_iterator iter;
	for (iter = m_fileList.begin(); iter != m_fileList.end(); iter++) {
		if (iter == m_iter) {
			return i;
		}
		i++;
	}
	return -1;
}

const FILETIME* CFileList::CurrentModificationTime() const {
	if (m_iter != m_fileList.end()) {
		return &(m_iter->GetLastModTime());
	} else {
		return NULL;
	}
}

LPCTSTR CFileList::PeekNextPrev(int nIndex, bool bForward, bool bToggle) {
	if (bToggle) {
		return (m_nMarkedIndexShow == 0) ? m_sMarkedFile : m_sMarkedFileCurrent;
	} else {
		if (m_bFindingFiles && (m_fileList.size() <= 1))
		{
			//not ready to peek ahead, so just abort!
			return Current();
		}
		std::list<CFileDesc>::iterator thisIter = m_iter;
		LPCTSTR sFileName;
		if (nIndex != 0) {
			CFileList* pFL = bForward ? Next() : Prev();
			sFileName = pFL->PeekNextPrev(nIndex-1, bForward, bToggle);
		} else {
			sFileName = Current();
		}
		m_iter = thisIter;
		return sFileName;
	}
}

void CFileList::SetSorting(Helpers::ESorting eSorting, bool sortUpcounting) {
	if (eSorting != CFileDesc::GetSorting() || sortUpcounting != CFileDesc::IsSortedUpcounting()) {
		CString sThisFile = (m_iter != m_fileList.end()) ? m_iter->GetName() : "";
		CFileDesc::SetSorting(eSorting, sortUpcounting);
		m_fileList.sort();
		m_iter = FindFile(sThisFile);
		m_iterStart = m_fileList.begin();
	}
}

Helpers::ESorting CFileList::GetSorting() const {
	return CFileDesc::GetSorting();
}

bool CFileList::IsSortedUpcounting() const {
	return CFileDesc::IsSortedUpcounting();
}

void CFileList::SetNavigationMode(Helpers::ENavigationMode eMode) {
	if (m_bIsSlideShowList) {
		return;
	}

	sm_eMode = eMode;
	if (!m_bFindingFiles) {
		DeleteHistory();
		m_nLevel = 0;
		m_iterStart = m_fileList.begin();

		if (eMode == Helpers::NM_Auto) //defer till async complete
		{
			sm_eMode = HasSubDir() ? Helpers::NM_LoopSubDirectories :
				Helpers::NM_LoopSameDirectoryLevel;
		}
	}
}

void CFileList::MarkCurrentFile() {
	LPCTSTR sCurrent = Current();
	if (::GetFileAttributes(sCurrent) != INVALID_FILE_ATTRIBUTES) {
		m_sMarkedFile = CString(sCurrent);
		m_sMarkedFileCurrent = _T("");
		m_nMarkedIndexShow = -1;
	}
}

bool CFileList::FileMarkedForToggle() {
	return !m_sMarkedFile.IsEmpty();
}

void CFileList::ToggleBetweenMarkedAndCurrentFile() {
	if (m_nMarkedIndexShow == -1) {
		m_nMarkedIndexShow = 0;
	}
	LPCTSTR sNewFile = (m_nMarkedIndexShow == 0) ? m_sMarkedFile : m_sMarkedFileCurrent;
	if (sNewFile[0] == 0) {
		return;
	}
	if (m_nMarkedIndexShow == 0) {
		m_sMarkedFileCurrent = Current();
	}

	m_sInitialFile = sNewFile;
	int nPos = m_sInitialFile.ReverseFind(_T('\\'));
	m_sDirectory = (nPos > 0) ? m_sInitialFile.Left(nPos) : _T(""); // the backslash is stripped away!

	DeleteHistory();
	Reload(m_sInitialFile);

	m_nMarkedIndexShow = (m_nMarkedIndexShow + 1) & 1;
}

bool CFileList::CanOpenCurrentFileForReading() const
{
	LPCTSTR sCurrentFile = Current();
	if (sCurrentFile != NULL && sCurrentFile[0] != 0) {
		HANDLE hFile = ::CreateFile(Current(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != NULL) {
			::CloseHandle(hFile);
			return true;
		}
	}
	return false;
}

void CFileList::DeleteHistory(bool onlyForward) {
	// delete the chain of CFileLists forward and backward and only leave the current node alive
	CFileList* pFileList = this->m_next;
	while (pFileList != NULL && pFileList->m_prev != NULL) {
		CFileList* thisList = pFileList;
		pFileList = pFileList->m_next;
		thisList->m_bDeleteHistory = false; // prevent multiple delete
		delete thisList;
	}
	if (!onlyForward) {
		pFileList = this->m_prev;
		while (pFileList != NULL) {
			CFileList* thisList = pFileList;
			pFileList = pFileList->m_prev;
			thisList->m_bDeleteHistory = false;  // prevent multiple delete
			delete thisList;
		}
		m_prev = NULL;
	}
	m_next = NULL;
	
}

///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void CFileList::MoveIterToLast() {
	m_iter = m_fileList.end();
	--m_iter;
}

std::list<CFileDesc>::iterator CFileList::FindFile(const CString& sName) {
	int nStart = sName.ReverseFind(_T('\\')) + 1;
	if (nStart == sName.GetLength()) {
		return m_fileList.begin();
	}
	std::list<CFileDesc>::iterator iter;
	for (iter = m_fileList.begin(); iter != m_fileList.end(); iter++) {
		if (_tcsicmp((LPCTSTR)sName + nStart, iter->GetTitle()) == 0) {
			return iter;
		}
	}
	return m_fileList.begin(); // in case the file was not found
}

void CFileList::GetDirListRcursive(CString sPath, std::list<CString>& dirList, CString& sThisDirTitle)
{
	CFindFile fileFind;
	if (fileFind.FindFile(sPath + "\\*")) {
		if (fileFind.IsDirectory() && !fileFind.IsDots()
			&& ((sThisDirTitle.Compare(fileFind.GetFileName()) == 0) || (!m_bHideHidden || !fileFind.IsHidden())))
		{
			dirList.push_back(fileFind.GetFileName());
		}
		while (fileFind.FindNextFile()) {
			if (fileFind.IsDirectory() && !fileFind.IsDots()
				&& ((sThisDirTitle.Compare(fileFind.GetFileName()) == 0) || (!m_bHideHidden || !fileFind.IsHidden())))
			{
				dirList.push_back(fileFind.GetFileName());
			}
		}
	}
}

bool CFileList::GetDirList(std::list<CString>& dirList, CString &sNextDirRoot, CString& sThisDirTitle)
{
	int nPos = m_sDirectory.ReverseFind(_T('\\'));
	if (nPos <= 0) {
		return false; // root dir - no siblings
	}
	sNextDirRoot = m_sDirectory.Left(nPos);
	sThisDirTitle = m_sDirectory.Right(m_sDirectory.GetLength() - nPos - 1);
	// collect all sibling folders
	GetDirListRcursive(sNextDirRoot, dirList, sThisDirTitle);
	if (dirList.size() == 0) {
		return false; // no sibling folders
	}
	dirList.sort(); // sort is alphabetically
	return true;
}


bool CFileList::HasSubDir()
{
	int nPos = m_sDirectory.ReverseFind(_T('\\'));
	if (nPos <= 0) {
		return false; // root dir - no siblings
	}
	CString sThisDirTitle = m_sDirectory.Right(m_sDirectory.GetLength() - nPos - 1);
	CFindFile fileFind;
	if (fileFind.FindFile(m_sDirectory + "\\*")) {
		if (fileFind.IsDirectory() && !fileFind.IsDots()
			&& ((sThisDirTitle.Compare(fileFind.GetFileName()) == 0) || (!m_bHideHidden || !fileFind.IsHidden())))
		{
			return true;
		}
	}
	while (fileFind.FindNextFile()) {
		if (fileFind.IsDirectory() && !fileFind.IsDots()
			&& ((sThisDirTitle.Compare(fileFind.GetFileName()) == 0) || (!m_bHideHidden || !fileFind.IsHidden())))
		{
			return true;
		}
	}
	return false;
}

CFileList* CFileList::WrapToNextImage() {
	if (m_next != NULL) {
		assert(sm_eMode != Helpers::NM_LoopDirectory);
		if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel)
		{
			//must reset (current pointer) to 1st image! otherwise will be stuck repeating last images of folders
			m_next->m_iter = m_next->m_iterStart;
		}
		return m_next;
	}

	if (sm_eMode == Helpers::NM_LoopDirectory) {
		return NULL;
	} else if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
		std::list<CString> dirList;
		CString sNextDirRoot, sThisDirTitle;
		if (!GetDirList(dirList, sNextDirRoot, sThisDirTitle))
		{
			return NULL;
		}

		// now find the current folder and take the next in list
		// to handle the wrap around, two iterations are needed (in case the current folder is the last in the list)
		std::list<CString>::iterator iter;
		bool bFound = false;
		for (int nStep = 0; nStep < 2; nStep++) {
			for (iter = dirList.begin(); iter != dirList.end(); iter++ ) {
				if (iter->CompareNoCase(sThisDirTitle) == 0) {
					bFound = true;
				} else if (bFound) {
					CFileList* pNewList = TryCreateFileList(sNextDirRoot + '\\' + *iter + '\\', 0);
					if (pNewList != NULL) {
						return pNewList;
					}
				}
			}
		}
	} else if (sm_eMode == Helpers::NM_LoopSubDirectories) {
		CFileList* pFileList = FindFileRecursively(m_sDirectory, _T(""), false, m_nLevel, 0);
		return pFileList;
	}
	return NULL;
}

CFileList* CFileList::FindFileRecursively (const CString& sDirectory, const CString& sFindAfter,
										   bool bSearchThisFolder, int nLevel, int nRecursion, bool bInverse) {
	if (bSearchThisFolder) {
		CFileList* pFileList = TryCreateFileList(sDirectory + "\\", nLevel, bInverse);
		if (pFileList != NULL) {
			return pFileList;
		}
	}
	// create a list of all child directories
	CFindFile fileFind;
	std::list<CString> dirList;
	GetDirListRcursive(sDirectory, dirList, CString(""));

	if (dirList.size() > 0) {
		dirList.sort();

		bool bStart = sFindAfter.GetLength() == 0;
		if (!bInverse)
		{
			std::list<CString>::iterator iter;
			for (iter = dirList.begin(); iter != dirList.end(); iter++) {
				if (bStart) {
					CFileList* pFileList = FindFileRecursively(sDirectory + "\\" + *iter, _T(""), true, nLevel + 1, nRecursion + 1, false);
					if (pFileList != NULL) {
						return pFileList;
					}
				}
				if (iter->CompareNoCase(sFindAfter) == 0) {
					bStart = true;
				}
			}
		}
		else
		{
			std::list<CString>::reverse_iterator iter;
			for (iter = dirList.rbegin(); iter != dirList.rend(); iter++) {
				if (bStart) {
					CFileList* pFileList = FindFileRecursively(sDirectory + "\\" + *iter, _T(""), true, nLevel + 1, nRecursion + 1, true);
					if (pFileList != NULL) {
						return pFileList;
					}
				}
				if (iter->CompareNoCase(sFindAfter) == 0) {
					bStart = true;
				}
			}
			if (bStart) {
				CFileList* pFileList = FindFileRecursively(sDirectory, _T(""), true, nLevel + 1, nRecursion + 1, true);
				if (pFileList != NULL) {
					return pFileList;
				}
			}
		}
	}

	// terminate recursion if on start level
	if (nLevel == 0) {
		return NULL;
	}

	if (nRecursion > 0) {
		return NULL; // this branch is not containing any images
	} else {
		// backtrack
		int nPos = sDirectory.ReverseFind(_T('\\'));
		CString sParentDir = (nPos > 0) ? sDirectory.Left(nPos) : _T("");
		CString sThisDir = sDirectory.Right(sDirectory.GetLength() - nPos - 1);
		return FindFileRecursively(sParentDir, sThisDir, false, nLevel - 1, max(0, nRecursion - 1), bInverse);
	}
}

CFileList* CFileList::WrapToPrevImage() {
	// This is much easier than going to next image as we never go back to new unknown folders
	if (m_prev != NULL) {
		return m_prev;
	}
	if (sm_eMode == Helpers::NM_LoopSameDirectoryLevel) {
		std::list<CString> dirList;
		CString sNextDirRoot, sThisDirTitle;
		if (!GetDirList(dirList, sNextDirRoot, sThisDirTitle))
		{
			return AnyAvailableLast();
		}

		// now find the current folder and take the next in list
		// to handle the wrap around, two iterations are needed (in case the current folder is the last in the list)
		std::list<CString>::reverse_iterator iter;
		bool bFound = false;
		for (int nStep = 0; nStep < 2; nStep++) {
			for (iter = dirList.rbegin(); iter != dirList.rend(); iter++) {
				if (iter->CompareNoCase(sThisDirTitle) == 0) {
					bFound = true;
				}
				else if (bFound) {
					CFileList* pNewList = TryCreateFileList(sNextDirRoot + '\\' + *iter + '\\', 0, true);
					if (pNewList != NULL) {
						pNewList->Last();
						return pNewList;
					}
				}
			}
		}
	}
	else if (sm_eMode == Helpers::NM_LoopSubDirectories) {
		CFileList* pFileList = FindFileRecursively(m_sDirectory, _T(""), false, m_nLevel, 0, true);
		if (pFileList)
		{
			pFileList->Last();
			return pFileList;
		}
	}
	return AnyAvailableLast();
}

void CFileList::NextInFolder() {
	if (m_fileList.size() > 0) {
		m_iter++;
		if (m_iter == m_fileList.end()) {
			if (m_bWrapAroundFolder)
				m_iter = m_fileList.begin();
			else --m_iter;
		}
	}
}

CFileList* CFileList::GotoFirstShown() {
	if (sm_eMode == Helpers::NM_LoopDirectory)
		return this;

	LPCTSTR thisFile, prevFile;
	//LPCTSTR firstFile = Current();
	LPCTSTR firstFile = NULL;
	std::list<CFileDesc>::iterator pIter = m_fileList.end();
	if (--pIter != m_fileList.end()) {
		firstFile = pIter->GetName();
	}
	CFileList *pThis = this, *pFound = this;
	do {
		thisFile = pThis->Current();
		pFound = pThis;
		CFileList* pPrev = pThis->Prev();
		pThis = pPrev;
		prevFile = pThis->Current();
	} while (thisFile != NULL && prevFile != NULL && firstFile != NULL && _tcscmp(thisFile, prevFile) != 0 && _tcscmp(prevFile, firstFile) != 0);

	return pFound;
}

CFileList* CFileList::TryCreateFileList(const CString& directory, int nNewLevel, bool bInverse) {
	// Check if we already had this directory in the loop, do not create a new loop
	CFileList* pList = this;
	while (pList != NULL) {
		if (pList->m_sDirectory.CompareNoCase(directory.Left(directory.GetLength() - 1)) == 0) {
			return pList; //no need to create new, return existing
		}
		pList = pList->m_prev;
	}
	pList = this->m_next; //check the other direction as well
	while (pList != NULL) {
		if (pList->m_sDirectory.CompareNoCase(directory.Left(directory.GetLength() - 1)) == 0) {
			return pList; //no need to create new, return existing
		}
		pList = pList->m_next;
	}

	CFileList* pNewList = new CFileList(directory, m_directoryWatcher, CFileDesc::GetSorting(), CFileDesc::IsSortedUpcounting(), m_bWrapAroundFolder, nNewLevel, m_bHideHidden);
	/*
	* Async load folder will return 0 items initially and cause NextFolder() to fail!
	* So force WaitIfNotReady.
	* Would shortcut return upon finding 1 file be sufficient?
	*/
	pNewList->WaitIfNotReady();
	if (pNewList->m_fileList.size() > 0) {
		if (!bInverse)
		{
			pNewList->m_prev = this;
			m_next = pNewList;
		}
		else
		{
			pNewList->m_next = this;
			m_prev = pNewList;
		}
		return pNewList;
	} else {
		delete pNewList;
		return NULL;
	}
}

void CFileList::FindFiles(std::list<CFileDesc>& fileList) {
	fileList.clear();
	if (!m_sDirectory.IsEmpty()) {
		CFindFile fileFind;
		LPCTSTR* allFileEndings = GetSupportedFileEndingList();
		for (int i = 0; i < nNumEndings; i++) {
			if (m_bForceExitFindFiles) return;
			if (fileFind.FindFile(m_sDirectory + _T("\\*.") + allFileEndings[i])) {
				AddToFileList(fileList, fileFind, allFileEndings[i], m_nMinFilesize, m_bHideHidden);
				while (fileFind.FindNextFile()) {
					if (m_bForceExitFindFiles) return;
					AddToFileList(fileList, fileFind, allFileEndings[i], m_nMinFilesize, m_bHideHidden);
				}
			}
		}
	}

	m_fileList.sort();
}

void CFileList::VerifyFiles() {
	std::list<CFileDesc>::iterator iter;
	for (iter = m_fileList.begin(); iter != m_fileList.end(); iter++) {
		if (::GetFileAttributes(iter->GetName()) == INVALID_FILE_ATTRIBUTES) {
			iter = m_fileList.erase(iter);
			if (iter == m_fileList.end()) {
				break;
			}
		}
	}
}

bool CFileList::IsImageFile(const CString & sEnding) {
	CString sEndingLC = sEnding;
	sEndingLC.MakeLower();
	LPCTSTR* allFileEndings = GetSupportedFileEndingList();
	for (int i = 0; i < nNumEndings; i++) {
		if (allFileEndings[i] == sEndingLC) {
			return true;
		}
	}
	return false;
}

bool CFileList::TryReadingSlideShowList(const CString & sSlideShowFile) {
	FILE *fptr;
	if ((fptr = _tfopen(sSlideShowFile,_T("rb"))) == NULL) {
		return false;
	}
	// Try to detect if this is a UTF16 Unicode text file or a ANSI or UTF-8 coded file
	const int NUMPROBE = 64;
	uint8 buff[NUMPROBE];
	bool bUnicode;
	bool bUTF8Marker = false;
	int nSeekPos = 0;
	int nRealProbe = (int)fread(buff, 1, NUMPROBE, fptr);
	if (nRealProbe < 16) {
		// file is too short for a good guess
		bUnicode = false;
	} else {
		if (buff[0] == 0xFF && buff[1] == 0xFE) {
			// Unicode byte order marker, only supporting little endian unicode files
			bUnicode = true;
			nSeekPos = 2;
		} else if (buff[0] == 0xEF && buff[1] == 0xBB && buff[2] == 0xBF) {
			// UTF-8 encoded text file
			bUTF8Marker = true;
		} else {
			// Use a heuristic in case the BOM is missing
			int nGood = 0;
			for (int i = 0; i < nRealProbe; i++) {
				if (i & 1) {
					if (buff[i] == 0) nGood++;
				} else {
					if (buff[i] != 0) nGood++;
				}
			}
			bUnicode = nGood > nRealProbe*2/3;
		}
	}

	bool bIsUTF8EncodedXML = strstr((char*)buff, "encoding=\"utf-8\"") != NULL;
	if (bUTF8Marker || bIsUTF8EncodedXML) {
		// UTF-8 encoded text file, open the file in UTF-8 mode, it will be read as Unicode
		fclose(fptr);
		if ((fptr = _tfopen(sSlideShowFile,_T("r, ccs=UTF-8"))) == NULL) {
			return false;
		}
		nSeekPos = bIsUTF8EncodedXML ? max(0, (int)(strchr((char*)buff, '<') - (char*)buff)) : 3;
		bUnicode = true;
	}

	int FILE_BUFFER_SIZE = CSettingsProvider::This().MaxSlideShowFileListSize() * 1024;
	char* fileBuff = new char[FILE_BUFFER_SIZE];
	char* fileBuffOrig = fileBuff;
	wchar_t* wideFileBuff = (wchar_t*)fileBuff; // same buffer interpreted as Unicode
	fseek(fptr, nSeekPos, SEEK_SET);
	int nRealFileSizeChars = (int)fread(fileBuff, 1, FILE_BUFFER_SIZE, fptr);
	if (bUnicode) nRealFileSizeChars = nRealFileSizeChars/2;

	const int LINE_BUFF_SIZE = 512;
	TCHAR lineBuff[LINE_BUFF_SIZE + 1];
	bool bValid = true;
	int nTotalChars = 0;

	do {
		int nNumChars = 0;
		if (bUnicode) {
			wchar_t* pRunning = wideFileBuff;
			bool bIsNull = false;
			while (*pRunning != L'\n' && nNumChars < LINE_BUFF_SIZE && nTotalChars < nRealFileSizeChars) {
				if (*pRunning == 0) {
					bIsNull = true;
				}
				pRunning++; nNumChars++; nTotalChars++;
			}
			bValid = nNumChars < 512 && !bIsNull;
			if (bValid) {
				memcpy(lineBuff, wideFileBuff, (pRunning - wideFileBuff)*sizeof(TCHAR));
			}
			wideFileBuff = pRunning+1;
			nTotalChars++;
		} else {
			char* pRunning = fileBuff;
			bool bIsNull = false;
			while (*pRunning != '\n' && nNumChars < LINE_BUFF_SIZE && nTotalChars < nRealFileSizeChars) {
				if (*pRunning == 0) {
					bIsNull = true;
				}
				pRunning++; nNumChars++; nTotalChars++;
			}
			bValid = nNumChars < LINE_BUFF_SIZE && !bIsNull;
			if (bValid) {
				size_t nDummy;
				mbstowcs_s(&nDummy, lineBuff, LINE_BUFF_SIZE, fileBuff, pRunning - fileBuff);
			}
			fileBuff = pRunning+1;
			nTotalChars++;
		}
		if (!bValid) {
			// the file contains very long lines or null characters, it is most likely not a text file
			fclose(fptr);
			delete[] fileBuffOrig;
			return false;
		}
		// one line in lineBuff, check if there is a valid file name
		bool bRelativePath = false;
		lineBuff[nNumChars] = _T('\n'); // force end
		lineBuff[nNumChars + 1] = 0; // terminate line string
		TCHAR* pStart = _tcsstr(lineBuff, _T("\\\\"));
		if (pStart == NULL) {
			pStart = _tcsstr(lineBuff+1, _T(":\\"));
			if (pStart != NULL) {
				pStart--;
			} else {
				// try to find file name with relative path
				LPCTSTR* allFileEndings = GetSupportedFileEndingList();
				for (int i = 0; i < nNumEndings; i++) {
					pStart = Helpers::stristr(lineBuff, (LPCTSTR)(CString(_T(".")) + allFileEndings[i]));
					if (pStart != NULL) {
						while (pStart >= lineBuff && *pStart != _T('"') && *pStart != _T('>')) pStart--;
						pStart++;
						bRelativePath = true;
						break;
					}
				}
			}
			
		}
		if (pStart != NULL) {
			// extract file name and add to list of files to show
			TCHAR* pEnd = pStart;
			while (*pEnd != _T('\r') && *pEnd != _T('\n') && *pEnd != _T('"') && *pEnd != _T('<')) {
				pEnd++;
			}
			*pEnd = 0;
			CFindFile fileFind;
			CString sPath = bRelativePath ? (m_sDirectory + _T('\\') + pStart) : pStart;
			if (fileFind.FindFile(sPath)) {
				AddToFileList(m_fileList, fileFind, NULL, m_nMinFilesize, m_bHideHidden);
			}
		}
	} while (nTotalChars < nRealFileSizeChars);

	fclose(fptr);
	delete[] fileBuffOrig;
	return true;
}
