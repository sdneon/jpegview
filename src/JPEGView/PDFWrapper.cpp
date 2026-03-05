#include "stdafx.h"
#include "PDFWrapper.h"
#include "MaxImageDef.h"
#include "ImageLoadThread.h"
#include "JPEGImage.h"

#ifndef WINXP

//default frame interval in ms for images in CB* archives: 1hr
#define DEF_PDF_FRAME_TIME_MS 360000
#define EXTRA_PDF_PARAMS IF_PDF, IF_PDF, (nFrameCount > 1), request->FrameIndex, nFrameCount, DEF_PDF_FRAME_TIME_MS

// PDFium API 全体を保護するミューテックス（RAII パターン）
class CPdfiumLock {
public:
	CPdfiumLock() { ::InitializeCriticalSection(&m_cs); }
	~CPdfiumLock() { ::DeleteCriticalSection(&m_cs); }
	void Lock() { ::EnterCriticalSection(&m_cs); }
	void Unlock() { ::LeaveCriticalSection(&m_cs); }
private:
	CRITICAL_SECTION m_cs;
};
static CPdfiumLock s_pdfiumLock;

FPDF_DOCUMENT PdfReader::doc = 0;
CString PdfReader::m_sLastFileName;
int PdfReader::m_nPageCount;
void* PdfReader::m_pBuffer;

// 画面サイズに基づく最適 DPI を計算（画面にフィットする DPI）
double PdfReader::CalculateOptimalDPI(double pageWidthPt, double pageHeightPt) {
	// PDF のページサイズは points 単位 (1 point = 1/72 inch)
	double pageWidthInch = pageWidthPt / 72.0;
	double pageHeightInch = pageHeightPt / 72.0;

	// プライマリモニタの画面解像度を取得
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	// ページ全体が画面に収まる DPI を計算
	double dpiX = screenWidth / pageWidthInch;
	double dpiY = screenHeight / pageHeightInch;
	double dpi = min(dpiX, dpiY);

	// 下限 72 DPI（上限なし: MAX_IMAGE_PIXELS で安全を担保）
	dpi = max(72.0, dpi);
	return dpi;
}

void* PdfReader::ReadImage(int nFrameIndex, int& width, int& height, int& bpp,
	int& frameCount,
	bool& outOfMemory, const void* pBuffer, int fileSize)
{
	bool openNewFile = pBuffer != 0;
	outOfMemory = false;
	width = height = 0;
	bpp = 4;  // BGRA

	unsigned char* pPixelData = NULL;

	// PDFium API 全体をロック（スレッドセーフティ確保）
	s_pdfiumLock.Lock();

	// PDFium 初期化（lock 内で安全に一度だけ実行）
	static bool initialized = false;
	if (!initialized) {
		FPDF_InitLibrary();
		initialized = true;
	}

	if (openNewFile)
	{
		nFrameIndex = 0;

		// メモリバッファから PDF をロード
		doc = FPDF_LoadMemDocument(pBuffer, fileSize, NULL);
		if (doc == NULL) {
			s_pdfiumLock.Unlock();
			return NULL;
		}

		// ページ数取得（表紙のみ対応だが取得はする）
		m_nPageCount = FPDF_GetPageCount(doc);
		if (m_nPageCount < 1) {
			FPDF_CloseDocument(doc);
			doc = 0;
			s_pdfiumLock.Unlock();
			return NULL;
		}

		PdfReader::m_pBuffer = const_cast<void*>(pBuffer);
	}
	frameCount = m_nPageCount;

	// ページ 0（表紙）をロード
	FPDF_PAGE page = FPDF_LoadPage(doc, nFrameIndex);
	if (page == NULL) {
		FPDF_CloseDocument(doc);
		doc = 0;
		s_pdfiumLock.Unlock();
		return NULL;
	}

	// ページサイズ取得（points 単位）
	double pageWidthPt = FPDF_GetPageWidth(page);
	double pageHeightPt = FPDF_GetPageHeight(page);

	// 最適 DPI 計算 → ピクセルサイズ決定
	double dpi = CalculateOptimalDPI(pageWidthPt, pageHeightPt);
	width = (int)((pageWidthPt / 72.0) * dpi);
	height = (int)((pageHeightPt / 72.0) * dpi);

	// ピクセル上限チェック
	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		FPDF_ClosePage(page);
		FPDF_CloseDocument(doc);
		doc = 0;
		s_pdfiumLock.Unlock();
		return NULL;
	}
	if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
		outOfMemory = true;
		FPDF_ClosePage(page);
		FPDF_CloseDocument(doc);
		doc = 0;
		s_pdfiumLock.Unlock();
		return NULL;
	}

	// ビットマップ作成
	FPDF_BITMAP bitmap = FPDFBitmap_Create(width, height, 0);  // 0 = BGRA
	if (bitmap == NULL) {
		outOfMemory = true;
		FPDF_ClosePage(page);
		FPDF_CloseDocument(doc);
		doc = 0;
		s_pdfiumLock.Unlock();
		return NULL;
	}

	// 白背景填充
	FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);

	// レンダリング（品質優先: AA + LCD テキスト最適化）
	int flags = FPDF_ANNOT | FPDF_LCD_TEXT;
	FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, flags);

	// ピクセルデータ取得
	void* buffer_ptr = FPDFBitmap_GetBuffer(bitmap);
	int stride = FPDFBitmap_GetStride(bitmap);

	if (buffer_ptr == NULL || stride < width * bpp) {
		FPDFBitmap_Destroy(bitmap);
		FPDF_ClosePage(page);
		FPDF_CloseDocument(doc);
		doc = 0;
		s_pdfiumLock.Unlock();
		return NULL;
	}

	// ピクセルデータをコピー
	int size = width * bpp * height;
	pPixelData = new(std::nothrow) unsigned char[size];
	if (pPixelData == NULL) {
		outOfMemory = true;
		FPDFBitmap_Destroy(bitmap);
		FPDF_ClosePage(page);
		FPDF_CloseDocument(doc);
		doc = 0;
		s_pdfiumLock.Unlock();
		return NULL;
	}

	// PDFium の BGRA は JPEGView と同じフォーマット
	// stride が width*bpp より大きい場合は行ごとにコピー
	for (int y = 0; y < height; y++) {
		memcpy(pPixelData + y * width * bpp,
		       (unsigned char*)buffer_ptr + y * stride,
		       width * bpp);
	}

	// リソース解放（ドキュメントは毎回即座にクローズ）
	FPDFBitmap_Destroy(bitmap);
	FPDF_ClosePage(page);
	//FPDF_CloseDocument(doc);

	s_pdfiumLock.Unlock();

	return (void*)pPixelData;
}

void CImageLoadThread::DeleteCachedPdf()
{
	PdfReader::m_sLastFileName.Empty();
	PdfReader::m_nPageCount = 0;
	if (PdfReader::doc)
	{
		FPDF_CloseDocument(PdfReader::doc);
		PdfReader::doc = 0;
	}
	if (PdfReader::m_pBuffer)
	{
		delete[] PdfReader::m_pBuffer;
		PdfReader::m_pBuffer = 0;
	}
}

void CImageLoadThread::ProcessReadPDFRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	int nFrameIndex = request->FrameIndex;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName == PdfReader::m_sLastFileName)
	{
		bUseCachedDecoder = true;
		if (nFrameIndex >= PdfReader::m_nPageCount)
			nFrameIndex = PdfReader::m_nPageCount - 1;
		if (nFrameIndex < 0)
			nFrameIndex = 0;
		request->FrameIndex = nFrameIndex;
	}
	else {
		DeleteCachedPdf();
	}

	HANDLE hFile = 0;
	if (!bUseCachedDecoder)
	{
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) return;
	}

	char* pBuffer = NULL;
	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	int nFrameCount = 0;
	try {
		long long nFileSize = 0;
		if (!bUseCachedDecoder)
		{
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_PDF_FILE_SIZE) {
				request->OutOfMemory = true;
			}
			// ファイル全体をメモリに読み込む
			pBuffer = new(std::nothrow) char[(size_t)nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
			}
			unsigned int nNumBytesRead;
			if (::ReadFile(hFile, pBuffer, (DWORD)nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
				::CloseHandle(hFile);  // ハンドルを即座にクローズ（デコード前）
				hFile = INVALID_HANDLE_VALUE;
			}
		}

		int nWidth, nHeight, nBPP;
		uint8* pPixelData = (uint8*)PdfReader::ReadImage(
			nFrameIndex,
			nWidth, nHeight, nBPP, nFrameCount,
			request->OutOfMemory, pBuffer, (int)nFileSize);
		if (nFrameCount > 1)
			PdfReader::m_sLastFileName = sFileName;
		if (pPixelData != NULL) {
			BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
			// 表紙のみ: frame_index=0, frame_count=1
			request->Image = new CJPEGImage(
				nWidth, nHeight, pPixelData, NULL, nBPP, 0,
				EXTRA_PDF_PARAMS);
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	if (hFile != INVALID_HANDLE_VALUE) {
		::CloseHandle(hFile);
	}
	if (pBuffer && (nFrameCount <= 1))
		delete[] pBuffer;
	//o.w. PdfReader still needs it for reading (subsequent) pages
}

#endif // WINXP
