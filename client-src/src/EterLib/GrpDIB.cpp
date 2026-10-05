#include "StdAfx.h"
#include "GrpDIB.h"

#include <utf8.h>

CGraphicDib::CGraphicDib()
{
	Initialize();
}

CGraphicDib::~CGraphicDib()
{
	Destroy();
}

void CGraphicDib::Initialize()
{
	m_hDC = NULL;
	m_hBmp = NULL;
	m_hOldBmp = NULL;
	m_pvBuf = NULL;
	m_width = 0;
	m_height = 0;
}

void CGraphicDib::Destroy()
{
	if (m_hDC && m_hOldBmp)
		SelectObject(m_hDC, m_hOldBmp);

	if (m_hBmp)
		DeleteObject(m_hBmp);

	if (m_hDC)
		DeleteDC(m_hDC);

	Initialize();
}

bool CGraphicDib::Create(int width, int height)
{
	Destroy();

	BITMAPINFO bmi;
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	m_hDC = CreateCompatibleDC(NULL);
	if (!m_hDC)
	{
		TraceError("CGraphicDib::Create - CreateCompatibleDC failed (%u)", GetLastError());
		return false;
	}

	m_hBmp = CreateDIBSection(m_hDC, &bmi, DIB_RGB_COLORS, &m_pvBuf, NULL, 0);
	if (!m_hBmp || !m_pvBuf)
	{
		TraceError("CGraphicDib::Create - CreateDIBSection %dx%d failed (%u)", width, height, GetLastError());
		Destroy();
		return false;
	}

	m_hOldBmp = SelectObject(m_hDC, m_hBmp);

	m_width = width;
	m_height = height;

	memset(m_pvBuf, 0, width * height * sizeof(DWORD));

	::SetTextColor(m_hDC, RGB(255, 255, 255));
	::SetBkColor(m_hDC, 0);

	return true;
}

HDC CGraphicDib::GetDCHandle()
{
	return m_hDC;
}

void CGraphicDib::SetBkMode(int iBkMode)
{
	::SetBkMode(m_hDC, iBkMode);
}

void CGraphicDib::TextOut(int ix, int iy, const char* c_szText)
{
	if (!c_szText || !*c_szText)
		return;

	std::wstring wText = Utf8ToWide(c_szText);
	if (wText.empty())
		return;

	::TextOutW(m_hDC, ix, iy, wText.c_str(), (int)wText.length());
	GdiFlush();
}

void* CGraphicDib::GetPointer()
{
	return m_pvBuf;
}

int CGraphicDib::GetWidth()
{
	return m_width;
}

int CGraphicDib::GetHeight()
{
	return m_height;
}
