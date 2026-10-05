#include "StdAfx.h"
#include "TextBar.h"
#include "Util.h"

#include <utf8.h>

// Notice boards (uiTip.TipBoard / BigBoard) draw with GDI straight into the DIB.
// CBlockTexture turns every touched pixel fully opaque, which gives the original crisp look.
void CTextBar::__SetFont(int fontSize, bool isBold)
{
	LOGFONTW logFont;
	ZeroMemory(&logFont, sizeof(logFont));

	logFont.lfHeight = fontSize;
	logFont.lfWeight = isBold ? FW_BOLD : FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_DEFAULT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = ANTIALIASED_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH;
	wcscpy_s(logFont.lfFaceName, LF_FACESIZE, L"Arial");

	m_hFont = CreateFontIndirectW(&logFont);
	if (!m_hFont)
	{
		TraceError("CTextBar::__SetFont - CreateFontIndirect failed (size %d, bold %d)", fontSize, isBold);
		return;
	}

	m_hOldFont = SelectObject(m_dib.GetDCHandle(), m_hFont);
}

void CTextBar::SetTextColor(int r, int g, int b)
{
	::SetTextColor(m_dib.GetDCHandle(), RGB(r, g, b));
}

void CTextBar::GetTextExtent(const char* c_szText, SIZE* p_size)
{
	if (!p_size)
		return;

	p_size->cx = 0;
	p_size->cy = 0;

	if (!c_szText || !m_hFont)
		return;

	std::wstring wText = Utf8ToWide(c_szText);
	GetTextExtentPoint32W(m_dib.GetDCHandle(), wText.c_str(), (int)wText.length(), p_size);
}

void CTextBar::TextOut(int ix, int iy, const char * c_szText)
{
	if (!m_hFont)
		return;

	m_dib.TextOut(ix, iy, c_szText);
	Invalidate();
}

void CTextBar::OnCreate()
{
	m_dib.SetBkMode(TRANSPARENT);

	__SetFont(m_fontSize, m_isBold);
}

CTextBar::CTextBar(int fontSize, bool isBold)
{
	m_hFont = NULL;
	m_hOldFont = NULL;
	m_fontSize = fontSize;
	m_isBold = isBold;
}

CTextBar::~CTextBar()
{
	HDC hDC = m_dib.GetDCHandle();
	if (hDC && m_hOldFont)
		SelectObject(hDC, m_hOldFont);

	if (m_hFont)
		DeleteObject(m_hFont);
}
