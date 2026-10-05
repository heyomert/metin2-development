// Do any glyphs draw outside the cell the original GrpFontTexture code thresholds?
// Cell (relative to TextOutW origin): x in [0, width), y in [0, tmHeight), width = B + ceil(A>0) + ceil(C>0) + 1.
#include <windows.h>
#include <cmath>
#include <cstdio>

static void Check(const wchar_t* face, int h, bool bold, bool italic)
{
	HDC dc = CreateCompatibleDC(NULL);
	const int W = 128, H = 128, ox = 40, oy = 40;
	BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
	void* bits; HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0); SelectObject(dc, bmp);
	LOGFONTW lf{}; lf.lfHeight = h; lf.lfWeight = bold ? FW_BOLD : FW_NORMAL; lf.lfItalic = italic; lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfQuality = ANTIALIASED_QUALITY; wcscpy_s(lf.lfFaceName, face);
	HFONT f = CreateFontIndirectW(&lf); SelectObject(dc, f);
	SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, 0);
	TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
	int negA = 0, outX = 0, outY = 0, total = 0;
	wchar_t exX[64] = {}, exY[64] = {}; int nx = 0, ny = 0;
	// Basic Latin, Latin-1 Supplement, Latin Extended-A (chat input can contain any of these)
	for (wchar_t ch = 0x21; ch <= 0x17F; ++ch) {
		if (ch >= 0x7F && ch <= 0xA0) continue;
		ABCFLOAT abc; if (!GetCharABCWidthsFloatW(dc, ch, ch, &abc)) continue;
		++total;
		if (abc.abcfA < 0) ++negA;
		int width = (int)abc.abcfB + (abc.abcfA > 0 ? (int)ceilf(abc.abcfA) : 0) + (abc.abcfC > 0 ? (int)ceilf(abc.abcfC) : 0) + 1;
		memset(bits, 0, W * H * 4);
		TextOutW(dc, ox, oy, &ch, 1); GdiFlush();
		bool bx = false, by = false;
		for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
			if (!(((DWORD*)bits)[y * W + x] & 0xff)) continue;
			int rx = x - ox, ry = y - oy;
			if (rx < 0 || rx >= width) bx = true;
			if (ry < 0 || ry >= tm.tmHeight) by = true;
		}
		if (bx) { ++outX; if (nx < 60) exX[nx++] = ch; }
		if (by) { ++outY; if (ny < 60) exY[ny++] = ch; }
	}
	printf("%ls %d%s%s: glyphs=%d negativeA=%d drawOutsideCellX=%d drawOutsideCellY=%d  X:", face, h, bold ? "b" : "", italic ? "i" : "", total, negA, outX, outY);
	for (int i = 0; i < nx; ++i) printf(" U+%04X", exX[i]);
	printf("  Y:");
	for (int i = 0; i < ny; ++i) printf(" U+%04X", exY[i]);
	printf("\n");
	DeleteObject(f); DeleteObject(bmp); DeleteDC(dc);
}

int main()
{
	Check(L"Tahoma", 9, false, false); Check(L"Tahoma", 12, false, false); Check(L"Tahoma", 14, false, false);
	Check(L"Tahoma", 12, false, true);
	Check(L"Arial", 12, false, false); Check(L"Arial", 18, true, false);
	return 0;
}
