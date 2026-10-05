// Verifies that the GrpFontTexture rasterization (left pad + bearingX, TRANSPARENT background, clipped to the cell)
// puts exactly the same on-screen pixels as the original / Anka2 code, except the left-overhang pixels the original
// wrote into the neighbouring cell. Also checks that clipping to the cell never cuts a real glyph pixel.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

typedef std::set<std::pair<int,int>> PixelSet;

struct Dib
{
	HDC dc; HBITMAP bmp; DWORD* bits; int w, h;
	Dib(int W, int H) : w(W), h(H)
	{
		dc = CreateCompatibleDC(NULL);
		BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
		bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
		SelectObject(dc, bmp);
		SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, 0);
	}
	~Dib() { DeleteObject(bmp); DeleteDC(dc); }
	void Clear() { memset(bits, 0, w * h * 4); }
};

static void Check(const wchar_t* face, int height, bool bold, bool italic)
{
	LOGFONTW lf{}; lf.lfHeight = height; lf.lfWeight = bold ? FW_BOLD : FW_NORMAL; lf.lfItalic = italic;
	lf.lfCharSet = DEFAULT_CHARSET; lf.lfOutPrecision = OUT_DEFAULT_PRECIS; lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY; lf.lfPitchAndFamily = DEFAULT_PITCH; wcscpy_s(lf.lfFaceName, face);
	HFONT font = CreateFontIndirectW(&lf);

	Dib a(160, 160), b(160, 160);
	SelectObject(a.dc, font); SelectObject(b.dc, font);
	SetBkMode(b.dc, TRANSPARENT); // a keeps the original default (OPAQUE)
	const int ox = 40, oy = 40;

	int glyphs = 0, missing = 0, extraNotLeft = 0, overhangPx = 0, clippedReal = 0, bkDiff = 0;
	// Basic Latin, Latin-1, Latin Extended-A/B, Greek, Cyrillic (anything a player can type in chat on a Latin locale)
	for (wchar_t ch = 0x21; ch <= 0x4FF; ++ch)
	{
		if ((ch >= 0x7F && ch <= 0xA0) || (ch >= 0x250 && ch < 0x370)) continue;
		WORD gi = 0; if (GetGlyphIndicesW(a.dc, &ch, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || gi == 0xFFFF) continue;
		SIZE size; ABCFLOAT abc;
		if (!GetTextExtentPoint32W(a.dc, &ch, 1, &size) || !GetCharABCWidthsFloatW(a.dc, ch, ch, &abc)) continue;
		++glyphs;

		// Original: draw at the pen, threshold only inside the cell
		int w0 = (int)abc.abcfB + (abc.abcfA > 0 ? (int)ceilf(abc.abcfA) : 0) + (abc.abcfC > 0 ? (int)ceilf(abc.abcfC) : 0) + 1;
		a.Clear(); TextOutW(a.dc, ox, oy, &ch, 1); GdiFlush();
		PixelSet orig;
		for (int y = oy; y < oy + size.cy; ++y) for (int x = ox; x < ox + w0; ++x)
			if (a.bits[y * a.w + x] & 0xff) orig.insert({ x - ox, y - oy });

		// New: left pad, TRANSPARENT, unclipped first (to see whether anything lands outside the new cell)
		int pad = abc.abcfA < 0 ? (int)ceilf(-abc.abcfA) : 0;
		int w1 = pad + w0;
		b.Clear(); TextOutW(b.dc, ox + pad, oy, &ch, 1); GdiFlush();
		PixelSet now;
		for (int y = 0; y < b.h; ++y) for (int x = 0; x < b.w; ++x)
		{
			if (!(b.bits[y * b.w + x] & 0xff)) continue;
			if (x < ox || x >= ox + w1 || y < oy || y >= oy + size.cy) { ++clippedReal; continue; }
			now.insert({ x - ox - pad, y - oy }); // on-screen offset from the pen (bearingX = -pad)
		}

		// Background mode alone: same origin, OPAQUE vs TRANSPARENT, whole DIB
		a.Clear(); TextOutW(a.dc, ox, oy, &ch, 1);
		b.Clear(); TextOutW(b.dc, ox, oy, &ch, 1); GdiFlush();
		for (int i = 0; i < a.w * a.h; ++i) if (((a.bits[i] & 0xff) != 0) != ((b.bits[i] & 0xff) != 0)) ++bkDiff;

		for (auto& p : orig) if (!now.count(p)) ++missing;
		for (auto& p : now) if (!orig.count(p)) { if (p.first < 0) ++overhangPx; else ++extraNotLeft; }
	}
	printf("%ls %d%s%s: glyphs=%d | original pixels missing=%d | extra pixels right of pen=%d | recovered left-overhang pixels=%d | pixels outside new cell=%d | OPAQUE vs TRANSPARENT diff=%d\n",
		face, height, bold ? "b" : "", italic ? "i" : "", glyphs, missing, extraNotLeft, overhangPx, clippedReal, bkDiff);
	DeleteObject(font);
}

int main()
{
	Check(L"Tahoma", 9, false, false);
	Check(L"Tahoma", 12, false, false);
	Check(L"Tahoma", 14, false, false);
	Check(L"Tahoma", 12, false, true);
	Check(L"Arial", 12, false, false);
	Check(L"Arial", 18, true, false);
	return 0;
}
