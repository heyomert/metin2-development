#pragma once

#include "GrpTexture.h"
#include "GrpImageTexture.h"
#include "GrpDIB.h"

#include <vector>
#include <map>

class CGraphicFontTexture : public CGraphicTexture
{
	public:
		typedef wchar_t TCharacterKey;

		typedef struct SCharacterInfomation
		{
			short index;
			short width;
			short height;
			float left;
			float top;
			float right;
			float bottom;
			float advance;
			float bearingX;	// cell x offset from the pen position (negative when the glyph overhangs to the left)
		} TCharacterInfomation;

		typedef std::vector<TCharacterInfomation*> TPCharacterInfomationVector;

	public:
		CGraphicFontTexture();
		virtual ~CGraphicFontTexture();

		void Destroy();
		bool Create(const char* c_szFontName, int fontSize, bool bItalic);

		bool CreateDeviceObjects();
		void DestroyDeviceObjects();

		bool CheckTextureIndex(DWORD dwTexture);
		void SelectTexture(DWORD dwTexture);

		bool UpdateTexture();

		TCharacterInfomation* GetCharacterInfomation(wchar_t keyValue);
		TCharacterInfomation* UpdateCharacterInfomation(TCharacterKey keyValue);

		bool IsEmpty() const;

	protected:
		void Initialize();

		bool AppendTexture();

	protected:
		typedef std::vector<CGraphicImageTexture*> TGraphicImageTexturePointerVector;
		typedef std::map<TCharacterKey, TCharacterInfomation> TCharacterInfomationMap;

	protected:
		// CPU-side atlas page that GDI rasterizes glyphs into
		CGraphicDib m_dib;

		HFONT m_hFont;
		HGDIOBJ m_hFontOld;

		TGraphicImageTexturePointerVector m_pFontTextureVector;

		TCharacterInfomationMap m_charInfoMap;

		int m_x;
		int m_y;
		int m_step;
		bool m_isDirty;

		LONG m_fontSize;
		bool m_bItalic;
};
