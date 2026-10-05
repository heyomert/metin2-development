#pragma once

// 32-bit top-down DIB section with its own memory DC, so GDI can rasterize text into it.
class CGraphicDib
{
	public:
		CGraphicDib();
		virtual ~CGraphicDib();

		void Destroy();
		bool Create(int width, int height);

		void SetBkMode(int iBkMode);
		void TextOut(int ix, int iy, const char * c_szText);

		int GetWidth();
		int GetHeight();

		void* GetPointer();

		HDC GetDCHandle();

	protected:
		void Initialize();

	protected:
		HDC			m_hDC;
		HBITMAP		m_hBmp;
		HGDIOBJ		m_hOldBmp;

		int			m_width;
		int			m_height;

		void *		m_pvBuf;
};
