#include "StdAfx.h"
#include "ImageLoadThread.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "JPEGImage.h"

#include "JXLWrapper.h"
#include "TJPEGWrapper.h"
#include "PNGWrapper.h"
#include "WEBPWrapper.h"
#include "QOIWrapper.h"
#include "ReaderBMP.h"
#include "RAWWrapper.h"
#include "HEIFWrapper.h"

#include "zip/zip.h"
#include "bit7z/include/bitfileextractor.hpp"
#include "bit7z/include/bitarchivereader.hpp"

using namespace bit7z;

ZipEntry::ZipEntry(const char* a_pchName, unsigned int a_index, unsigned long long a_size) :
	index(a_index),
	size(a_size)
{
	//extract extension only, from filename provided by kuba-zip
	CString name(a_pchName);
	int pos = name.ReverseFind('.');
	if (pos >= 0)
	{
		ext = name.Right(name.GetLength() - pos - 1);
		ext.MakeLower();
	}
}

ZipEntry::ZipEntry(bit7z::tstring a_ext, unsigned int a_index, unsigned long long a_size) :
	ext(a_ext.c_str()), //extension provided directly by bit7z
	index(a_index),
	size(a_size)
{
	ext.MakeLower();
}

#define EXT_MATCHES(extension) (ext.Compare(_T(extension)) == 0)
//default frame interval in ms for images in CB* archives: 1hr
#define DEF_MANGA_FRAME_TIME_MS 360000
#define EXTRA_MANGA_PARAMS IF_ZIP, bMultipleFiles, request->FrameIndex, m_nZipCount, DEF_MANGA_FRAME_TIME_MS

BOOL StringEndsWithIgnoreCase(CString& str, CString& end)
{
	int lenFind = end.GetLength(),
		len = str.GetLength();
	if (len < lenFind) return false;
	CString find = str.Right(lenFind);
	return find.CompareNoCase(end) == 0;
}

void CImageLoadThread::ProcessReadZipRequest(CRequest* request) {
	bool bSuccess = false;
	bool bUseCachedDecoder = false,
		bMultipleFiles = false;
	const wchar_t* sFileName;
	int nFrameIndex = request->FrameIndex;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName == m_sLastZipFileName) {
		if (m_nZipCount <= 0)
			return; //abort as no valid image entries in archive; all marked as non-image

		bUseCachedDecoder = true;
		if (nFrameIndex >= m_nZipCount)
			nFrameIndex = m_nZipCount - 1;
		if (nFrameIndex < 0)
			nFrameIndex = 0;
		request->FrameIndex = nFrameIndex;
		bMultipleFiles = m_nZipCount > 1;
	}
	else {
		DeleteCachedZip();
	}

	bool use7z = !(StringEndsWithIgnoreCase(CString(sFileName), CString(".cbz"))); //true;

	int bufFilenameLen = request->FileName.GetLength() + 1;
	char* bufFilename = new char[bufFilenameLen];
	wcstombs(bufFilename, request->FileName.GetString(), bufFilenameLen);
	struct zip_t* zip = NULL;
	BitArchiveReader* zip7 = NULL;
	if (!bUseCachedDecoder) {
		if (use7z)
		{
			try
			{
				Bit7zLibrary lib{ "7z.dll" };
				zip7 = new BitArchiveReader(lib, bufFilename, BitFormat::Auto);

				for (const auto& item : *zip7) {
					if (!item.isDir())
					{
						zipEntries.push_back(ZipEntry(item.extension(), item.index(), item.size()));
					}
				}
				delete zip7; zip7 = 0; //somehow later extract will fail, unless reopened, so delete 1st
				m_nZipCount = zipEntries.size();
			}
			catch (const BitException&) {
				//::OutputDebugString(_T("bit7z ERR: ")); ::OutputDebugString(CString(ex.what()));
				delete zip7; zip7 = 0;
				delete[] bufFilename;
				return;
			}
		}
		else
		{
			zip = zip_open(bufFilename, 0, 'r');
			int n = zip_entries_total(zip);
			for (int i = 0; i < n; ++i) {
				zip_entry_openbyindex(zip, i);
				{
					if (!zip_entry_isdir(zip)) //skip folders
					{
						zipEntries.push_back(ZipEntry(zip_entry_name(zip), i, zip_entry_size(zip)));
					}
				}
				zip_entry_close(zip);
			}
			m_nZipCount = zipEntries.size(); //adjust count to valid one only
		}
		if (m_nZipCount < 0) {
			if (zip) zip_close(zip);
			else if (zip7) delete zip7;
			delete[] bufFilename;
			return;
		}
		if (m_nZipCount > 1) {
			bMultipleFiles = true;
			bUseCachedDecoder = true;
			m_sLastZipFileName = sFileName;
		}
		if (nFrameIndex >= m_nZipCount)
			nFrameIndex = m_nZipCount - 1;
		if (nFrameIndex < 0)
			nFrameIndex = 0;
	}
	char* pBuffer = NULL;
	do {
		try {
			ZipEntry z = zipEntries.at(nFrameIndex);
			CString& ext = z.ext;
			uint64_t nFileSize = z.size;
			if (!use7z)
			{
				if (!zip)
					zip = zip_open(bufFilename, 0, 'r');
				zip_entry_openbyindex(zip, z.index);
				{
					pBuffer = new(std::nothrow) char[nFileSize];
					zip_entry_noallocread(zip, (void*)pBuffer, nFileSize);
				}
				zip_entry_close(zip);
				zip_close(zip);
				zip = NULL;
			}
			else //if (use7z)
			{
				try
				{
					//const tstring name = ConvertLPCWSTRToString(entryName); //not needed, as can access by index
					pBuffer = new(std::nothrow) char[nFileSize];
					//somehow, has to reopen, else extract fails!
					Bit7zLibrary lib{ "7z.dll" };
					zip7 = new BitArchiveReader(lib, bufFilename, BitFormat::Auto);
					zip7->extractTo((byte_t*)pBuffer, nFileSize, z.index);
					delete zip7; zip7 = 0;
				}
				catch (const BitException& ex) {
					::OutputDebugString(_T("bit7z Extract ERR: ")); ::OutputDebugString(CString(ex.what()));
					delete zip7; zip7 = 0;
					delete[] bufFilename;
					return;
				}
			}

			bool bHasAnimation = false;
			if (EXT_MATCHES("jxl")
				|| ((nFileSize > 2) && (pBuffer[0] == 0xff && pBuffer[1] == 0x0a))
				|| ((nFileSize > 12) && (memcmp(pBuffer, "\x00\x00\x00\x0cJXL\x20\x0d\x0a\x87\x0a", 12) == 0)))
			{
				int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
				void* pEXIFData;
				uint8* pPixelData = (uint8*)JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
				if (pPixelData != NULL) {
					// Multiply alpha value into each AABBGGRR pixel
					BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_JXL, EXTRA_MANGA_PARAMS);
					free(pEXIFData);
					if (request->Image) bSuccess = true;
				}
			}
			else if (EXT_MATCHES("jpeg") || EXT_MATCHES("jpg")
				|| ((nFileSize > 2) && (pBuffer[0] == 0xff && pBuffer[1] == 0xd8)))
			{
				int nWidth, nHeight, nBPP;
				TJSAMP eChromoSubSampling;
				bool bOutOfMemory;

				void* pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bOutOfMemory, pBuffer, nFileSize);

				// Color and b/w JPEG is supported
				if (pPixelData != NULL && (nBPP == 3 || nBPP == 1)) {
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData,
						Helpers::FindEXIFBlock(pBuffer, nFileSize), nBPP,
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, EXTRA_MANGA_PARAMS);
					request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					request->Image->SetJPEGChromoSampling(eChromoSubSampling);
					if (request->Image) bSuccess = true;
				}
				else if (bOutOfMemory) {
					request->OutOfMemory = true;
				}
			}
			else if (EXT_MATCHES("png")
				|| ((nFileSize > 8) && (pBuffer[0] == 0x89 && pBuffer[1] == 'P' && pBuffer[2] == 'N' && pBuffer[3] == 'G' &&
					pBuffer[4] == 0x0d && pBuffer[5] == 0x0a && pBuffer[6] == 0x1a && pBuffer[7] == 0x0a)))
			{
				if (nFileSize > MAX_PNG_FILE_SIZE) {
					request->OutOfMemory = true;
					delete[] bufFilename;
					return;
				}
				int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
				bool bHasAnimation;
				void* pEXIFData = NULL;
				uint8* pPixelData = (uint8*)PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
				if (pPixelData != NULL) {
					// Multiply alpha value into each AABBGGRR pixel
					BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, EXTRA_MANGA_PARAMS);
					free(pEXIFData);
					if (request->Image) bSuccess = true;
				}
			}
			else if (EXT_MATCHES("webp")
				|| ((nFileSize > 12) && pBuffer[0] == 'R' && pBuffer[1] == 'I' && pBuffer[2] == 'F' && pBuffer[3] == 'F' &&
					pBuffer[8] == 'W' && pBuffer[9] == 'E' && pBuffer[10] == 'B' && pBuffer[11] == 'P'))
			{
				int nWidth, nHeight;
				bool bHasAnimation = bUseCachedDecoder;
				int nFrameCount = 1;
				int nFrameTimeMs = 0;
				int nBPP;
				void* pEXIFData;
				uint8* pPixelData = (uint8*)WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
				if (pPixelData && nBPP == 4) {
					// Multiply alpha value into each AABBGGRR pixel
					BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_WEBP, EXTRA_MANGA_PARAMS);
					free(pEXIFData);
					if (request->Image) bSuccess = true;
				}
				else {
					delete[] pPixelData;
				}
			}
			else if (EXT_MATCHES("qoi")
				|| ((nFileSize > 4) && !memcmp(pBuffer, "qoif", 4)))
			{
				int nWidth, nHeight, nBPP;
				void* pPixelData = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer, nFileSize);
				if (pPixelData != NULL) {
					if (nBPP == 4) {
						// Multiply alpha value into each AABBGGRR pixel
						BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);
					}
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, nBPP, 0, IF_QOI, EXTRA_MANGA_PARAMS);
					if (request->Image) bSuccess = true;
				}
			}
			else if (EXT_MATCHES("bmp"))
			{
				bool bOutOfMemory;
				request->Image = CReaderBMP::ReadBmpImage(pBuffer, nFileSize, bOutOfMemory, EXTRA_MANGA_PARAMS);
				if (request->Image) bSuccess = true;
				if (bOutOfMemory) {
					request->OutOfMemory = true;
				}
			}
			else if (Helpers::IsRawImageFormat(z.ext)
				|| ((nFileSize > 12)
					&& !memcmp(pBuffer + 8, "crx ", 4)))
			{
				int fullsize = CSettingsProvider::This().DisplayFullSizeRAW();
				// Try with libraw
				UINT nPrevErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS);
				try {
					bool bOutOfMemory = false;
					if (fullsize == 2 || fullsize == 3) {
						request->Image = RawReader::ReadImage(pBuffer, nFileSize, bOutOfMemory, fullsize == 2, EXTRA_MANGA_PARAMS);
					}
					//Skip CReaderRAW as it does Not support reading from memory
					//if (request->Image == NULL && fullsize == 2) {
					//	request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
					//}
					if (request->Image == NULL) {
						request->Image = RawReader::ReadImage(pBuffer, nFileSize, bOutOfMemory, fullsize == 0 || fullsize == 3, EXTRA_MANGA_PARAMS);
					}
					if (request->Image) bSuccess = true;
					if (bOutOfMemory) {
						request->OutOfMemory = true;
					}
				}
				catch (...) {
					// libraw.dll not found or VC++ Runtime not installed
				}
				SetErrorMode(nPrevErrorMode);
			}
			else if (EXT_MATCHES("avif")
				|| ((nFileSize > 11) && !memcmp(pBuffer + 4, "ftypavi", 7)))
			{
				try
				{
					avifRGBImage rgb;
					memset(&rgb, 0, sizeof(rgb));
					m_avifDecoder = avifDecoderCreate();

					avifResult result = avifDecoderSetIOMemory(m_avifDecoder, (const uint8_t*)pBuffer, nFileSize);
					if (result == AVIF_RESULT_OK) {
						m_avifDecoder->io->data = pBuffer;
						result = avifDecoderParse(m_avifDecoder);
						if (result == AVIF_RESULT_OK) {
							result = avifDecoderNthImage(m_avifDecoder, 0); //ignore animation, just 1st frame
							if (result == AVIF_RESULT_OK) {
								avifRGBImageSetDefaults(&rgb, m_avifDecoder->image);
								rgb.format = AVIF_RGB_FORMAT_BGRA;
								if (rgb.depth > 8)
								{
									rgb.depth = 8;
								}
								if (rgb.pixels) {
									delete[] rgb.pixels;
									rgb.pixels = 0;
								}
								rgb.rowBytes = rgb.width * avifRGBImagePixelSize(&rgb);
								rgb.pixels = new(std::nothrow) unsigned char[(size_t)rgb.rowBytes * rgb.height];
								if (rgb.pixels)
								{
									if (avifImageYUVToRGB(m_avifDecoder->image, &rgb) == AVIF_RESULT_OK) {
										BlendAlpha((uint32*)(rgb.pixels), m_avifDecoder->image->width, m_avifDecoder->image->height, request->ProcessParams.TransparencyMode);
										request->Image = new CJPEGImage(m_avifDecoder->image->width, m_avifDecoder->image->height, rgb.pixels, 0, 4, 0, IF_AVIF, EXTRA_MANGA_PARAMS);
										if (request->Image) bSuccess = true;
									}
								}
								else
								{
									request->OutOfMemory = true;
								}
							}
						}
					}
					if (m_avifDecoder && m_avifDecoder->io && m_avifDecoder->io->data)
					{
						//we'll cleanup (pBuffer) ourselves, instead of leaving it to DeleteCachedAvifDecoder
						m_avifDecoder->io->data = 0;
					}
					if (!bSuccess)
					{
						ext = "heif"; //try fallback on HeifReader
					}
				}
				catch (...) {
					request->Image = NULL;
				}
				DeleteCachedAvifDecoder();
			}
			if (!bSuccess && (EXT_MATCHES("heif") || EXT_MATCHES("heic")
				|| ((nFileSize > 11)
					&& !memcmp(pBuffer + 4, "ftyp", 4)
					&& (!memcmp(pBuffer + 8, "hei", 3) || !memcmp(pBuffer + 8, "hev", 3)))))
			{
				try
				{
					int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
					nFrameCount = 1;
					nFrameTimeMs = 0;
					void* pEXIFData;
					uint8* pPixelData = (uint8*)HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->FrameIndex, pBuffer, nFileSize);
					if (pPixelData != NULL) {
						// Multiply alpha value into each AABBGGRR pixel
						BlendAlpha((uint32*)pPixelData, nWidth, nHeight, request->ProcessParams.TransparencyMode);

						request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_HEIF, EXTRA_MANGA_PARAMS);
						free(pEXIFData);
						if (request->Image) bSuccess = true;
					}
				}
				catch (heif::Error he) {
					// invalid image
					delete request->Image;
					request->Image = NULL;
				}
			}
			//else //unable to do as API reads from file and not buffer. GDI+ (GIF, BMP), WIC, PSD, RAW
		}
		catch (...) {
			delete request->Image;
			request->Image = NULL;
			request->ExceptionError = true;
		}

		if (zip != NULL)
			zip_close(zip);
		if (pBuffer) delete[] pBuffer;

		if (!bSuccess)
		{
			//erase non-image entry
			zipEntries.erase(zipEntries.begin() + nFrameIndex);
			if ((--m_nZipCount <= 0) || (nFrameIndex >= m_nZipCount))
			{
				break; //abort
			}
			//continue: try next file in archive
		}
	} while (!bSuccess);
	delete[] bufFilename;
}

void CImageLoadThread::DeleteCachedZip()
{
	m_sLastZipFileName.Empty();
	m_nZipCount = 0;
	zipEntries.clear();
}
