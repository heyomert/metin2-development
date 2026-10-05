#include "StdAfx.h"
#include "GrpText.h"
#include "EterBase/Stl.h"

#include "Util.h"

#include <utf8.h>

#include <cmath>

// Glyphs are rasterized by GDI and reduced to 1-bit coverage (any covered pixel becomes fully opaque white).
// This is the original Metin2 text look: crisp glyphs with a solid outline, no blurred edges.

CGraphicFontTexture::CGraphicFontTexture()
{
	Initialize();
}

CGraphicFontTexture::~CGraphicFontTexture()
{
	Destroy();
}

void CGraphicFontTexture::Initialize()
{
	CGraphicTexture::Initialize();
	m_hFont = NULL;
	m_hFontOld = NULL;
	m_isDirty = false;
	m_bItalic = false;
	m_x = 0;
	m_y = 0;
	m_step = 0;
	m_fontSize = 0;
}

bool CGraphicFontTexture::IsEmpty() const
{
	return m_hFont == NULL;
}

void CGraphicFontTexture::Destroy()
{
	HDC hDC = m_dib.GetDCHandle();
	if (hDC && m_hFontOld)
		SelectObject(hDC, m_hFontOld);

	if (m_hFont)
		DeleteObject(m_hFont);

	m_dib.Destroy();

	m_lpd3dTexture = NULL;
	CGraphicTexture::Destroy();
	stl_wipe(m_pFontTextureVector);
	m_charInfoMap.clear();

	Initialize();
}

bool CGraphicFontTexture::CreateDeviceObjects()
{
	if (!m_hFont)
		return true;

	// After device reset the GPU pages are gone: rebuild them by re-rasterizing every cached glyph.
	// Entries are updated in place so TCharacterInfomation pointers held by text instances stay valid.
	stl_wipe(m_pFontTextureVector);
	m_x = 0;
	m_y = 0;
	m_step = 0;
	m_isDirty = false;

	memset(m_dib.GetPointer(), 0, m_dib.GetWidth() * m_dib.GetHeight() * sizeof(DWORD));

	if (!AppendTexture())
		return false;

	for (const auto& pair : m_charInfoMap)
		UpdateCharacterInfomation(pair.first);

	UpdateTexture();
	return true;
}

void CGraphicFontTexture::DestroyDeviceObjects()
{
	m_lpd3dTexture = NULL;
	stl_wipe(m_pFontTextureVector);
}

bool CGraphicFontTexture::Create(const char* c_szFontName, int fontSize, bool bItalic)
{
	Destroy();

	m_fontSize = fontSize;
	m_bItalic = bItalic;

	DWORD width = 256, height = 256;
	if (GetMaxTextureWidth() > 512)
		width = 512;
	if (GetMaxTextureHeight() > 512)
		height = 512;

	if (!m_dib.Create(width, height))
		return false;

	std::wstring wFontName = Utf8ToWide(c_szFontName ? c_szFontName : "");

	// Positive lfHeight is the cell height (ascent + descent), as in the original client.
	LOGFONTW logFont;
	ZeroMemory(&logFont, sizeof(logFont));
	logFont.lfHeight = m_fontSize;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfItalic = (BYTE)m_bItalic;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_DEFAULT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = ANTIALIASED_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH;
	wcsncpy_s(logFont.lfFaceName, wFontName.c_str(), _TRUNCATE);

	m_hFont = CreateFontIndirectW(&logFont);
	if (!m_hFont)
	{
		TraceError("CGraphicFontTexture::Create - CreateFontIndirect failed for '%s' size %d", c_szFontName ? c_szFontName : "(null)", fontSize);
		return false;
	}

	HDC hDC = m_dib.GetDCHandle();
	m_hFontOld = SelectObject(hDC, m_hFont);
	m_dib.SetBkMode(TRANSPARENT);

	if (!AppendTexture())
		return false;

	return true;
}

bool CGraphicFontTexture::AppendTexture()
{
	CGraphicImageTexture* pNewTexture = new CGraphicImageTexture;

	if (!pNewTexture->Create(m_dib.GetWidth(), m_dib.GetHeight(), D3DFMT_A4R4G4B4))
	{
		delete pNewTexture;
		return false;
	}

	m_pFontTextureVector.push_back(pNewTexture);
	return true;
}

bool CGraphicFontTexture::UpdateTexture()
{
	if (!m_isDirty)
		return true;

	m_isDirty = false;

	if (m_pFontTextureVector.empty())
		return false;

	CGraphicImageTexture* pFontTexture = m_pFontTextureVector.back();

	WORD* pwDst;
	int pitch;

	if (!pFontTexture->Lock(&pitch, (void**)&pwDst))
		return false;

	pitch /= 2;  // pitch in WORDs (A4R4G4B4 = 2 bytes per pixel)

	int width = m_dib.GetWidth();
	int height = m_dib.GetHeight();

	const DWORD* pdwSrc = (const DWORD*)m_dib.GetPointer();

	// 1-bit threshold: any coverage -> opaque white, otherwise fully transparent
	for (int y = 0; y < height; ++y, pwDst += pitch, pdwSrc += width)
		for (int x = 0; x < width; ++x)
			pwDst[x] = (pdwSrc[x] & 0xff) ? 0xffff : 0;

	pFontTexture->Unlock();
	return true;
}

CGraphicFontTexture::TCharacterInfomation* CGraphicFontTexture::GetCharacterInfomation(wchar_t keyValue)
{
	TCharacterKey code = keyValue;

	TCharacterInfomationMap::iterator f = m_charInfoMap.find(code);

	if (m_charInfoMap.end() == f)
	{
		return UpdateCharacterInfomation(code);
	}
	else
	{
		return &f->second;
	}
}

CGraphicFontTexture::TCharacterInfomation* CGraphicFontTexture::UpdateCharacterInfomation(TCharacterKey keyValue)
{
	if (!m_hFont)
		return NULL;

	HDC hDC = m_dib.GetDCHandle();

	if (keyValue == 0x08)
		keyValue = L' ';

	SIZE size;
	ABCFLOAT stABC;

	if (!GetTextExtentPoint32W(hDC, &keyValue, 1, &size) || !GetCharABCWidthsFloatW(hDC, keyValue, keyValue, &stABC))
		return NULL;

	// A negative A width means GDI draws left of the pen position (e.g. 'j', 'ï' in Tahoma).
	// Reserve that space inside the cell so the pixels are not written into the previous glyph's cell.
	int leftPad = (stABC.abcfA < 0.0f) ? (int)ceilf(-stABC.abcfA) : 0;

	int cellWidth = leftPad + (int)stABC.abcfB;
	if (stABC.abcfA > 0.0f)
		cellWidth += (int)ceilf(stABC.abcfA);
	if (stABC.abcfC > 0.0f)
		cellWidth += (int)ceilf(stABC.abcfC);
	cellWidth++;

	int cellHeight = size.cy;
	float advance = ceilf(stABC.abcfA + stABC.abcfB + stABC.abcfC);

	int width = m_dib.GetWidth();
	int height = m_dib.GetHeight();

	// Atlas packing (row-based)
	if (m_x + cellWidth >= (width - 1))
	{
		m_y += (m_step + 1);
		m_step = 0;
		m_x = 0;

		if (m_y + cellHeight >= (height - 1))
		{
			if (!UpdateTexture())
				return NULL;

			if (!AppendTexture())
				return NULL;

			memset(m_dib.GetPointer(), 0, width * height * sizeof(DWORD));
			m_y = 0;
		}
	}

	TextOutW(hDC, m_x + leftPad, m_y, &keyValue, 1);
	GdiFlush();

	float rhwidth = 1.0f / float(width);
	float rhheight = 1.0f / float(height);

	TCharacterInfomation& rNewCharInfo = m_charInfoMap[keyValue];

	rNewCharInfo.index = static_cast<short>(m_pFontTextureVector.size() - 1);
	rNewCharInfo.width = (short)cellWidth;
	rNewCharInfo.height = (short)cellHeight;
	rNewCharInfo.left = float(m_x) * rhwidth;
	rNewCharInfo.top = float(m_y) * rhheight;
	rNewCharInfo.right = float(m_x + cellWidth) * rhwidth;
	rNewCharInfo.bottom = float(m_y + cellHeight) * rhheight;
	rNewCharInfo.advance = advance;
	rNewCharInfo.bearingX = (float)-leftPad;

	m_x += cellWidth;

	if (m_step < cellHeight)
		m_step = cellHeight;

	m_isDirty = true;

	return &rNewCharInfo;
}

bool CGraphicFontTexture::CheckTextureIndex(DWORD dwTexture)
{
	if (dwTexture >= m_pFontTextureVector.size())
		return false;

	return true;
}

void CGraphicFontTexture::SelectTexture(DWORD dwTexture)
{
	assert(CheckTextureIndex(dwTexture));
	m_lpd3dTexture = m_pFontTextureVector[dwTexture]->GetD3DTexture();
}
