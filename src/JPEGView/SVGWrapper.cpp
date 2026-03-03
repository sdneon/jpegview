#include "stdafx.h"
#include "SVGWrapper.h"
#include "MaxImageDef.h"
#include "ImageLoadThread.h"
#include "JPEGImage.h"
#include "bit7z/include/bitarchivereader.hpp"

//dunno which specific distro zlib z_stream is from, can't build, so switch to 7zip
using namespace bit7z;

#ifndef WINXP

#include <shlobj.h>
#include <lunasvg.h>

BOOL StringEndsWithIgnoreCase(CString& str, CString& end);


// 画面サイズに基づく最適レンダリングサイズを計算
void SvgReader::CalculateFitSize(float svgWidth, float svgHeight,
	int& outWidth, int& outHeight) {
	// プライマリモニタの画面解像度を取得
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	// アスペクト比を維持して画面に収まるサイズを計算
	float scaleX = (float)screenWidth / svgWidth;
	float scaleY = (float)screenHeight / svgHeight;
	float scale = min(scaleX, scaleY);

	outWidth = (int)(svgWidth * scale);
	outHeight = (int)(svgHeight * scale);

	// 次元上限チェック
	if (outWidth > MAX_IMAGE_DIMENSION) {
		float ratio = (float)MAX_IMAGE_DIMENSION / outWidth;
		outWidth = MAX_IMAGE_DIMENSION;
		outHeight = (int)(outHeight * ratio);
	}
	if (outHeight > MAX_IMAGE_DIMENSION) {
		float ratio = (float)MAX_IMAGE_DIMENSION / outHeight;
		outHeight = MAX_IMAGE_DIMENSION;
		outWidth = (int)(outWidth * ratio);
	}
}

void* SvgReader::ReadImage(int& width, int& height, int& bpp,
	bool& outOfMemory, const void* buffer, int sizebytes)
{
	// フォールバック用日本語フォントの初期化（初回のみ）
	static bool fontsInitialized = false;
	if (!fontsInitialized) {
		fontsInitialized = true;

		// システムフォントディレクトリの動的取得
		char fontsDir[MAX_PATH] = {};
		if (FAILED(::SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, 0, fontsDir))) {
			// フォールバック: 標準パスを使用
			strcpy_s(fontsDir, "C:\\Windows\\Fonts");
		}

		// Yu Gothic → Meiryo → MS Gothic の優先順
		// Yu Gothic, MS Gothic は Win11 コアフォント（全エディション同梱）
		// Meiryo は Supplemental だが日本語環境では通常インストール済み
		static const char* fontFiles[] = {
			"YuGothR.ttc",
			"meiryo.ttc",
			"msgothic.ttc",
		};
		for (const char* fontFile : fontFiles) {
			char fontPath[MAX_PATH];
			sprintf_s(fontPath, "%s\\%s", fontsDir, fontFile);
			if (::GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES) {
				// generic family 名で登録し、Arial 等へのフォールバックを防ぐ
				lunasvg_add_font_face_from_file("sans-serif", false, false, fontPath);
				lunasvg_add_font_face_from_file("serif", false, false, fontPath);
				lunasvg_add_font_face_from_file("monospace", false, false, fontPath);
				// font-family 未指定時のフォールバック
				lunasvg_add_font_face_from_file("", false, false, fontPath);
				break;
			}
		}
	}

	outOfMemory = false;
	width = height = 0;
	bpp = 4;  // BGRA

	if (buffer == NULL || sizebytes <= 0 || sizebytes > MAX_SVG_FILE_SIZE) {
		return NULL;
	}

	// SVG パース
	auto document = lunasvg::Document::loadFromData((const char*)buffer, sizebytes);

	if (document == nullptr) {
		return NULL;
	}

	// SVG サイズ取得
	float svgWidth = (float)document->width();
	float svgHeight = (float)document->height();

	// サイズが未定義の場合はバウンディングボックスから取得
	if (svgWidth <= 0 || svgHeight <= 0) {
		auto box = document->boundingBox();
		svgWidth = box.w;
		svgHeight = box.h;
	}

	if (svgWidth <= 0 || svgHeight <= 0) {
		return NULL;
	}

	// 画面フィットサイズ計算
	CalculateFitSize(svgWidth, svgHeight, width, height);

	// ピクセル上限チェック
	if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
		//try: rollback to original size
		width = svgWidth;
		height = svgHeight;
		if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
			outOfMemory = true;
			return NULL;
		}
	}

	// ビットマップレンダリング（白背景: 0xFFFFFFFF）
	auto bitmap = document->renderToBitmap(width, height, 0xFFFFFF00); //changed to transparent background
	if (bitmap.isNull()) {
		outOfMemory = true;
		return NULL;
	}

	// ピクセルデータ取得（lunasvg は ARGB 形式で返す）
	const uint32_t* argbData = (const uint32_t*)bitmap.data();
	int pixelCount = width * height;

	// BGRA 形式にコピー + バイトスワップ
	unsigned char* pPixelData = new(std::nothrow) unsigned char[pixelCount * 4];

	for (int i = 0, j = 0; i < pixelCount; ++i, j += 4) {
		uint32_t pixel = argbData[i];
		// ARGB → BGRA (白背景なので alpha は無視してよい)
		pPixelData[j] = (pixel >> 0) & 0xFF;  // B
		pPixelData[j + 1] = (pixel >> 8) & 0xFF;  // G
		pPixelData[j + 2] = (pixel >> 16) & 0xFF; // R
		pPixelData[j + 3] = (pixel >> 24) & 0xFF; // A
	}

	return pPixelData;
}

void CImageLoadThread::ProcessReadSVGRequest(CRequest* request) {
	HANDLE hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return;

	UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
	try {
		bool compressed = StringEndsWithIgnoreCase(CString(request->FileName), CString(".svgz"));
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_SVG_FILE_SIZE) {
			request->OutOfMemory = true;
		}
		else
		{
			// ファイル全体をメモリに読み込む
			unsigned char* fileBuffer;
			if (!compressed)
			{
				fileBuffer = new unsigned char[(size_t)nFileSize];
				DWORD bytesRead = 0;
				if (!::ReadFile(hFile, fileBuffer, (DWORD)nFileSize, &bytesRead, NULL) || bytesRead != nFileSize) {
					delete[] fileBuffer;
					::CloseHandle(hFile);
					SetErrorMode(nPrevErrorMode);
					return;
				}
				//double check content is not compressed
				// gzip マジックバイト判定 (0x1F 0x8B)
				if ((fileBuffer[0] == 0x1F) && (fileBuffer[1] == 0x8B))
				{
					//is gzip, so divert to unzip
					compressed = true;
					delete[] fileBuffer; fileBuffer = 0;
				}
			}
			if (compressed)
			{
				int bufFilenameLen = request->FileName.GetLength() + 1;
				char* bufFilename = new char[bufFilenameLen];
				BitArchiveReader* zip7;
				try
				{
					wcstombs(bufFilename, request->FileName.GetString(), bufFilenameLen);

					Bit7zLibrary lib{ "7z.dll" };
					zip7 = new BitArchiveReader(lib, bufFilename, BitFormat::Auto);
					if (zip7->filesCount() <= 0)
					{
						delete[] bufFilename;
						return;
					}
					const auto& item = *(zip7->begin()); //always take 1st entry only
					nFileSize = item.size();
					delete zip7; zip7 = 0;
					zip7 = new BitArchiveReader(lib, bufFilename, BitFormat::Auto);
					fileBuffer = new unsigned char[(size_t)nFileSize];
					zip7->extractTo((byte_t*)fileBuffer, nFileSize, 0);
					delete zip7;
				}
				catch (const BitException&) {
					//::OutputDebugString(_T("bit7z ERR: ")); ::OutputDebugString(CString(ex.what()));
					if (zip7) delete zip7;
					delete[] bufFilename;
					return;
				}
				delete[] bufFilename;
			}

			int nWidth, nHeight, nBPP;
			uint8* pPixelData = (uint8*)SvgReader::ReadImage(
				nWidth, nHeight, nBPP,
				request->OutOfMemory, fileBuffer, (int)nFileSize);
			delete[] fileBuffer;

			if (pPixelData != NULL) {
				BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
				// frame_index=0, frame_count=1
				request->Image = new CJPEGImage(
					nWidth, nHeight, pPixelData, NULL, nBPP, 0,
					IF_SVG, false, 0, 1, 0);
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	SetErrorMode(nPrevErrorMode);
	::CloseHandle(hFile);
}

#endif  // !WINXP
