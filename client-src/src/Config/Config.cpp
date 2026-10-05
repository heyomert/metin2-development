// Client settings tool (config.exe). Edits config/metin2.cfg, the file the game reads in
// CPythonSystem::LoadConfig (UserInterface/PythonSystem.cpp). Value ranges follow the game code.
#include "resource.h"
#include "ConfigFile.h"
#include "DisplayModes.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
	// CPythonSystem::SetDefaultConfig
	const int DEFAULT_WIDTH = 1024;
	const int DEFAULT_HEIGHT = 768;
	const int DEFAULT_BPP = 32;
	const int DEFAULT_FREQUENCY = 0;
	const float DEFAULT_VOLUME = 1.0f;
	const int DEFAULT_TILING = 0;
	const int DEFAULT_SHADOW = 3;
	const int DEFAULT_FOG = 0;

	// GameLib/MapManager.cpp: 0 = light, 1 = middle, 2 = dense (CPythonSystem::SetFogLevel caps at 2)
	const wchar_t* const FOG_NAMES[] = { L"Hafif", L"Orta", L"Yoğun" };

	// CPythonSystem::IsAutoTiling / IsSoftwareTiling: 0 = automatic, 1 = software (CPU), 2 = hardware (GPU)
	const wchar_t* const TILING_NAMES[] = { L"Otomatik", L"CPU", L"GPU" };

	// CPythonBackground shadow levels (UserInterface/PythonBackground.h), SetShadowLevel caps at 5
	const wchar_t* const SHADOW_NAMES[] = { L"Yok", L"Zemin", L"Zemin ve karakter", L"Hepsi", L"Hepsi (yüksek)", L"Hepsi (en yüksek)" };

	struct SState
	{
		std::wstring path;
		CConfigFile config;
		std::vector<SDisplayMode> modes;
		UINT bpp;
	};

	SState g_state;

	int Clamp(int value, int minValue, int maxValue)
	{
		return std::max(minValue, std::min(value, maxValue));
	}

	std::wstring GetConfigPath()
	{
		wchar_t exePath[MAX_PATH];
		DWORD len = GetModuleFileNameW(NULL, exePath, MAX_PATH);
		std::wstring dir(exePath, len);
		dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
		return dir + L"config\\metin2.cfg";
	}

	void AddComboItem(HWND hCombo, const std::wstring& text, LPARAM data)
	{
		int index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
		SendMessageW(hCombo, CB_SETITEMDATA, index, data);
	}

	LPARAM GetSelectedData(HWND hDlg, int id)
	{
		HWND hCombo = GetDlgItem(hDlg, id);
		int index = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
		return index == CB_ERR ? -1 : SendMessageW(hCombo, CB_GETITEMDATA, index, 0);
	}

	void SelectByData(HWND hDlg, int id, LPARAM data)
	{
		HWND hCombo = GetDlgItem(hDlg, id);
		int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
		for (int i = 0; i < count; ++i)
		{
			if (SendMessageW(hCombo, CB_GETITEMDATA, i, 0) == data)
			{
				SendMessageW(hCombo, CB_SETCURSEL, i, 0);
				return;
			}
		}

		SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
	}

	void FillNamedCombo(HWND hDlg, int id, const wchar_t* const* names, int count, int selected)
	{
		HWND hCombo = GetDlgItem(hDlg, id);
		for (int i = 0; i < count; ++i)
			AddComboItem(hCombo, names[i], i);

		SelectByData(hDlg, id, selected);
	}

	void FillFrequencies(HWND hDlg, int selectedRate)
	{
		HWND hCombo = GetDlgItem(hDlg, IDC_FREQUENCY);
		SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

		// 0 lets the driver pick the rate (the game's own default)
		AddComboItem(hCombo, L"Varsayılan", 0);

		LPARAM modeIndex = GetSelectedData(hDlg, IDC_RESOLUTION);
		if (modeIndex >= 0)
		{
			for (UINT rate : g_state.modes[modeIndex].refreshRates)
				AddComboItem(hCombo, std::to_wstring(rate) + L" Hz", rate);
		}

		SelectByData(hDlg, IDC_FREQUENCY, selectedRate);
	}

	void UpdateVolumeText(HWND hDlg, int sliderId, int textId)
	{
		int value = (int)SendDlgItemMessageW(hDlg, sliderId, TBM_GETPOS, 0, 0);
		SetDlgItemTextW(hDlg, textId, std::to_wstring(value).c_str());
	}

	void InitVolume(HWND hDlg, int sliderId, int textId, float volume)
	{
		SendDlgItemMessageW(hDlg, sliderId, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
		SendDlgItemMessageW(hDlg, sliderId, TBM_SETPAGESIZE, 0, 10);
		SendDlgItemMessageW(hDlg, sliderId, TBM_SETPOS, TRUE, Clamp((int)lroundf(volume * 100.0f), 0, 100));
		UpdateVolumeText(hDlg, sliderId, textId);
	}

	bool InitDialog(HWND hDlg)
	{
		HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_CONFIG));
		SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
		SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

		g_state.path = GetConfigPath();
		if (!g_state.config.Load(g_state.path))
		{
			MessageBoxW(hDlg, (L"Ayar dosyası okunamadı:\n" + g_state.path).c_str(), L"Ayarlar", MB_ICONERROR);
			return false;
		}

		const CConfigFile& cfg = g_state.config;

		if (!EnumerateDisplayModes(g_state.modes))
		{
			MessageBoxW(hDlg, L"Ekran modları okunamadı (DirectX 9).", L"Ayarlar", MB_ICONERROR);
			return false;
		}

		// Resolution: modes with the configured color depth; keep the current size even if the adapter does not list it
		int width = cfg.GetInt("WIDTH", DEFAULT_WIDTH);
		int height = cfg.GetInt("HEIGHT", DEFAULT_HEIGHT);
		int bpp = cfg.GetInt("BPP", DEFAULT_BPP);

		g_state.bpp = (bpp == 16) ? 16 : 32;
		g_state.modes.erase(std::remove_if(g_state.modes.begin(), g_state.modes.end(),
			[](const SDisplayMode& m) { return m.bpp != g_state.bpp; }), g_state.modes.end());

		auto current = std::find_if(g_state.modes.begin(), g_state.modes.end(),
			[&](const SDisplayMode& m) { return (int)m.width == width && (int)m.height == height; });
		if (current == g_state.modes.end())
			g_state.modes.insert(g_state.modes.begin(), { (UINT)width, (UINT)height, g_state.bpp, {} });

		HWND hResolution = GetDlgItem(hDlg, IDC_RESOLUTION);
		int selectedMode = 0;
		for (size_t i = 0; i < g_state.modes.size(); ++i)
		{
			const SDisplayMode& mode = g_state.modes[i];
			AddComboItem(hResolution, std::to_wstring(mode.width) + L"x" + std::to_wstring(mode.height), (LPARAM)i);
			if ((int)mode.width == width && (int)mode.height == height)
				selectedMode = (int)i;
		}
		SelectByData(hDlg, IDC_RESOLUTION, selectedMode);

		int frequency = cfg.GetInt("FREQUENCY", DEFAULT_FREQUENCY);
		std::vector<UINT>& rates = g_state.modes[selectedMode].refreshRates;
		if (frequency > 0 && std::find(rates.begin(), rates.end(), (UINT)frequency) == rates.end())
			rates.push_back((UINT)frequency);
		FillFrequencies(hDlg, frequency);

		InitVolume(hDlg, IDC_MUSIC_VOLUME, IDC_MUSIC_VOLUME_TEXT, cfg.GetFloat("MUSIC_VOLUME", DEFAULT_VOLUME));
		InitVolume(hDlg, IDC_VOICE_VOLUME, IDC_VOICE_VOLUME_TEXT, cfg.GetFloat("VOICE_VOLUME", DEFAULT_VOLUME));

		CheckDlgButton(hDlg, IDC_SOFTWARE_CURSOR, cfg.GetInt("SOFTWARE_CURSOR", 0) ? BST_CHECKED : BST_UNCHECKED);
		CheckRadioButton(hDlg, IDC_WINDOWED, IDC_FULLSCREEN, cfg.GetInt("WINDOWED", 0) == 1 ? IDC_WINDOWED : IDC_FULLSCREEN);
		CheckRadioButton(hDlg, IDC_GAME_IME, IDC_DEFAULT_IME, cfg.GetInt("USE_DEFAULT_IME", 0) == 1 ? IDC_DEFAULT_IME : IDC_GAME_IME);

		FillNamedCombo(hDlg, IDC_FOG, FOG_NAMES, _countof(FOG_NAMES), Clamp(cfg.GetInt("FOG_LEVEL", DEFAULT_FOG), 0, _countof(FOG_NAMES) - 1));
		FillNamedCombo(hDlg, IDC_TILING, TILING_NAMES, _countof(TILING_NAMES), Clamp(cfg.GetInt("SOFTWARE_TILING", DEFAULT_TILING), 0, _countof(TILING_NAMES) - 1));
		FillNamedCombo(hDlg, IDC_SHADOW, SHADOW_NAMES, _countof(SHADOW_NAMES), Clamp(cfg.GetInt("SHADOW_LEVEL", DEFAULT_SHADOW), 0, _countof(SHADOW_NAMES) - 1));

		return true;
	}

	bool SaveDialog(HWND hDlg)
	{
		CConfigFile& cfg = g_state.config;

		const SDisplayMode& mode = g_state.modes[GetSelectedData(hDlg, IDC_RESOLUTION)];
		cfg.SetInt("WIDTH", (int)mode.width);
		cfg.SetInt("HEIGHT", (int)mode.height);
		cfg.SetInt("BPP", (int)g_state.bpp);
		cfg.SetInt("FREQUENCY", (int)GetSelectedData(hDlg, IDC_FREQUENCY));

		cfg.SetFloat("MUSIC_VOLUME", (float)SendDlgItemMessageW(hDlg, IDC_MUSIC_VOLUME, TBM_GETPOS, 0, 0) / 100.0f);
		cfg.SetFloat("VOICE_VOLUME", (float)SendDlgItemMessageW(hDlg, IDC_VOICE_VOLUME, TBM_GETPOS, 0, 0) / 100.0f);

		cfg.SetInt("SOFTWARE_CURSOR", IsDlgButtonChecked(hDlg, IDC_SOFTWARE_CURSOR) == BST_CHECKED ? 1 : 0);
		cfg.SetInt("WINDOWED", IsDlgButtonChecked(hDlg, IDC_WINDOWED) == BST_CHECKED ? 1 : 0);
		cfg.SetInt("USE_DEFAULT_IME", IsDlgButtonChecked(hDlg, IDC_DEFAULT_IME) == BST_CHECKED ? 1 : 0);

		cfg.SetInt("FOG_LEVEL", (int)GetSelectedData(hDlg, IDC_FOG));
		cfg.SetInt("SOFTWARE_TILING", (int)GetSelectedData(hDlg, IDC_TILING));
		cfg.SetInt("SHADOW_LEVEL", (int)GetSelectedData(hDlg, IDC_SHADOW));

		std::wstring error;
		if (!cfg.Save(g_state.path, error))
		{
			MessageBoxW(hDlg, error.c_str(), L"Ayarlar", MB_ICONERROR);
			return false;
		}

		return true;
	}

	INT_PTR CALLBACK DialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
			case WM_INITDIALOG:
				if (!InitDialog(hDlg))
					EndDialog(hDlg, IDCANCEL);
				return TRUE;

			case WM_HSCROLL:
				if ((HWND)lParam == GetDlgItem(hDlg, IDC_MUSIC_VOLUME))
					UpdateVolumeText(hDlg, IDC_MUSIC_VOLUME, IDC_MUSIC_VOLUME_TEXT);
				else if ((HWND)lParam == GetDlgItem(hDlg, IDC_VOICE_VOLUME))
					UpdateVolumeText(hDlg, IDC_VOICE_VOLUME, IDC_VOICE_VOLUME_TEXT);
				return TRUE;

			case WM_COMMAND:
				switch (LOWORD(wParam))
				{
					case IDC_RESOLUTION:
						if (HIWORD(wParam) == CBN_SELCHANGE)
							FillFrequencies(hDlg, (int)GetSelectedData(hDlg, IDC_FREQUENCY));
						return TRUE;

					case IDOK:
						if (SaveDialog(hDlg))
							EndDialog(hDlg, IDOK);
						return TRUE;

					case IDCANCEL:
						EndDialog(hDlg, IDCANCEL);
						return TRUE;
				}
				break;
		}

		return FALSE;
	}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);

	DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_CONFIG), NULL, DialogProc);
	return 0;
}
