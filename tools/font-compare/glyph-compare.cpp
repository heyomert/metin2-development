// Round 2: all fonts/sizes the game uses, more FreeType variants, line metrics.
#include <windows.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <cstdio>
#include <cmath>
#include <set>
#include <string>
#include <vector>

struct Glyph { std::set<std::pair<int,int>> on; int advance = 0; };
struct GdiMetrics { int ascent, descent, height; };

static const wchar_t* kChars =
	L"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
	L"\u00C7\u00E7\u011E\u011F\u0130\u0131\u00D6\u00F6\u015E\u015F\u00DC\u00FC";

static Glyph GdiGlyph(const wchar_t* face, int lfHeight, bool bold, bool italic, wchar_t ch, GdiMetrics* m)
{
	Glyph g;
	HDC dc = CreateCompatibleDC(NULL);
	const int W = 160, H = 160;
	BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H; bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	SelectObject(dc, bmp);
	LOGFONTW lf{}; lf.lfHeight = lfHeight; lf.lfWeight = bold ? FW_BOLD : FW_NORMAL; lf.lfItalic = italic;
	lf.lfCharSet = DEFAULT_CHARSET; lf.lfOutPrecision = OUT_DEFAULT_PRECIS; lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY; lf.lfPitchAndFamily = DEFAULT_PITCH;
	wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
	HFONT f = CreateFontIndirectW(&lf);
	SelectObject(dc, f);
	SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, 0);
	TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
	m->ascent = tm.tmAscent; m->descent = tm.tmDescent; m->height = tm.tmHeight;
	const int ox = 40, oy = 40;
	TextOutW(dc, ox, oy, &ch, 1);
	GdiFlush();
	DWORD* px = (DWORD*)bits;
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x)
			if (px[y * W + x] & 0xff) g.on.insert({ x - ox, y - (oy + tm.tmAscent) });
	ABCFLOAT abc;
	if (GetCharABCWidthsFloatW(dc, ch, ch, &abc)) g.advance = (int)ceilf(abc.abcfA + abc.abcfB + abc.abcfC);
	DeleteObject(f); DeleteObject(bmp); DeleteDC(dc);
	return g;
}

struct Variant { const char* name; FT_Int32 load; FT_Render_Mode render; bool fractional; };
static const Variant kVariants[] = {
	{ "mono hint, mono render",        FT_LOAD_TARGET_MONO,   FT_RENDER_MODE_MONO,   false },
	{ "mono hint, gray render >0",     FT_LOAD_TARGET_MONO,   FT_RENDER_MODE_NORMAL, false },
	{ "normal hint, gray >0",          FT_LOAD_TARGET_NORMAL, FT_RENDER_MODE_NORMAL, false },
	{ "light hint, gray >0",           FT_LOAD_TARGET_LIGHT,  FT_RENDER_MODE_NORMAL, false },
	{ "no hint, gray >0",              FT_LOAD_NO_HINTING,    FT_RENDER_MODE_NORMAL, false },
	{ "mono hint, mono, fractional em",FT_LOAD_TARGET_MONO,   FT_RENDER_MODE_MONO,   true  },
	{ "mono hint, gray >0, fract. em", FT_LOAD_TARGET_MONO,   FT_RENDER_MODE_NORMAL, true  },
	{ "no hint, gray >0, fract. em",   FT_LOAD_NO_HINTING,    FT_RENDER_MODE_NORMAL, true  },
};

static Glyph FtGlyph(FT_Face face, const Variant& v, wchar_t ch)
{
	Glyph g;
	FT_UInt gi = FT_Get_Char_Index(face, ch);
	if (FT_Load_Glyph(face, gi, v.load) || FT_Render_Glyph(face->glyph, v.render)) return g;
	FT_GlyphSlot s = face->glyph; FT_Bitmap& b = s->bitmap;
	for (int r = 0; r < (int)b.rows; ++r)
		for (int c = 0; c < (int)b.width; ++c) {
			bool on = (v.render == FT_RENDER_MODE_MONO) ? ((b.buffer[r * b.pitch + (c >> 3)] >> (7 - (c & 7))) & 1) : (b.buffer[r * b.pitch + c] != 0);
			if (on) g.on.insert({ s->bitmap_left + c, r - s->bitmap_top });
		}
	g.advance = (int)ceilf(s->advance.x / 64.0f);
	return g;
}

static int Diff(const Glyph& a, const Glyph& b)
{
	int d = 0;
	for (auto& p : a.on) if (!b.on.count(p)) ++d;
	for (auto& p : b.on) if (!a.on.count(p)) ++d;
	return d;
}

static void Run(FT_Library lib, const char* file, const wchar_t* gdiFace, int size, bool bold, bool italic)
{
	FT_Face face; if (FT_New_Face(lib, file, 0, &face)) { printf("cannot open %s\n", file); return; }
	TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
	double emExact = size * (double)face->units_per_EM / (os2->usWinAscent + os2->usWinDescent);
	size_t n = wcslen(kChars);
	std::vector<Glyph> ref; GdiMetrics gm{}; int refPx = 0;
	for (size_t i = 0; i < n; ++i) { ref.push_back(GdiGlyph(gdiFace, size, bold, italic, kChars[i], &gm)); refPx += (int)ref.back().on.size(); }
	printf("\n=== %ls%s%s %d: GDI ascent=%d descent=%d height=%d | em exact %.3f px | refOnPixels=%d\n",
		gdiFace, bold ? " bold" : "", italic ? " italic" : "", size, gm.ascent, gm.descent, gm.height, emExact, refPx);
	for (const Variant& v : kVariants) {
		if (v.fractional) FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(emExact * 64), 72, 72);
		else FT_Set_Pixel_Sizes(face, 0, (FT_UInt)lround(emExact));
		if (italic) { FT_Matrix mt = { 0x10000L, 0x5800L, 0, 0x10000L }; FT_Set_Transform(face, &mt, NULL); }
		else FT_Set_Transform(face, NULL, NULL);
		int asc = (int)(face->size->metrics.ascender >> 6), desc = (int)(-face->size->metrics.descender >> 6), lh = (int)(face->size->metrics.height >> 6);
		int diffPx = 0, identical = 0, adv = 0;
		std::string worst;
		for (size_t i = 0; i < n; ++i) {
			Glyph g = FtGlyph(face, v, kChars[i]);
			int d = Diff(ref[i], g);
			diffPx += d; if (d == 0) ++identical; else if (d >= 3) { char b[16]; if (kChars[i] < 128) sprintf_s(b, "%c", (char)kChars[i]); else sprintf_s(b, "U+%04X ", kChars[i]); worst += b; }
			if (g.advance != ref[i].advance) ++adv;
		}
		printf("  %-31s identical %3d/%zu  diffPx %4d (%5.1f%%)  advMismatch %3d  FT asc/desc/lineH %d/%d/%d  >=3px: %s\n",
			v.name, identical, n, diffPx, 100.0 * diffPx / refPx, adv, asc, desc, lh, worst.substr(0, 60).c_str());
	}
	FT_Done_Face(face);
}

int main()
{
	FT_Library lib; FT_Init_FreeType(&lib);
	// GrpFontTexture fonts from locale_game.txt (Tahoma 9/12/14) + GM whisper italic (Tahoma:12i)
	Run(lib, "C:\\Windows\\Fonts\\tahoma.ttf", L"Tahoma", 9, false, false);
	Run(lib, "C:\\Windows\\Fonts\\tahoma.ttf", L"Tahoma", 12, false, false);
	Run(lib, "C:\\Windows\\Fonts\\tahoma.ttf", L"Tahoma", 14, false, false);
	Run(lib, "C:\\Windows\\Fonts\\tahoma.ttf", L"Tahoma", 12, false, true);
	// TextBar: upstream pre-FreeType = Tahoma; Anka2 = Arial (GetFontFaceFromCodePage). Sizes 12 (CreateTextBar) and 18 (uitip BigTextBar)
	Run(lib, "C:\\Windows\\Fonts\\tahoma.ttf", L"Tahoma", 18, false, false);
	Run(lib, "C:\\Windows\\Fonts\\arial.ttf", L"Arial", 12, false, false);
	Run(lib, "C:\\Windows\\Fonts\\arial.ttf", L"Arial", 18, false, false);
	return 0;
}
