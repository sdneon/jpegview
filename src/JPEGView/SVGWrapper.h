#pragma once

// lunasvg ベースの SVG/SVGZ レンダリングラッパー
class SvgReader
{
public:
	// SVG/SVGZ を BGRA ピクセルデータとしてレンダリング
	// 戻り値: BGRA ピクセルデータ (new unsigned char[])、失敗時 NULL
	static void* ReadImage(int& width, int& height, int& bpp,
		bool& outOfMemory, const void* buffer, int sizebytes);

private:
	// 画面サイズに基づく最適レンダリングサイズを計算
	static void CalculateFitSize(float svgWidth, float svgHeight,
		int& outWidth, int& outHeight);

	//// SVGZ (gzip 圧縮 SVG) を展開
	//static unsigned char* DecompressSvgz(const void* data,
	//	int size, unsigned long& outSize);
};
