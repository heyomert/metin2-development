// Does the game's fullscreen CreateDeviceEx call accept a non-zero FREQUENCY, and is that rate really applied?
// CGraphicDevice::Create (client-src/src/EterLib/GrpDevice.cpp) used to set FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT (0)
// while passing D3DDISPLAYMODEEX::RefreshRate = FREQUENCY from config/metin2.cfg; on failure it retries with rate 0 and MSAA off.
// Usage: fullscreen-refresh [rate]   (default: the lowest rate the adapter lists for the desktop resolution)
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>

static void PrintCurrentMode(IDirect3DDevice9Ex* dev, const char* when)
{
	D3DDISPLAYMODEEX dm = { sizeof(dm) };
	D3DDISPLAYROTATION rot;
	dev->GetDisplayModeEx(0, &dm, &rot);

	DEVMODEW gdi = {};
	gdi.dmSize = sizeof(gdi);
	EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &gdi);

	printf("    %s: device %ux%u @ %u Hz | Windows current mode %lux%lu @ %lu Hz\n", when, dm.Width, dm.Height, dm.RefreshRate,
		gdi.dmPelsWidth, gdi.dmPelsHeight, gdi.dmDisplayFrequency);
}

static HRESULT TryCreate(IDirect3D9Ex* d3d, HWND hWnd, const D3DDISPLAYMODE& desk, UINT ppRate, UINT modeRate, bool waitForWmi)
{
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = FALSE;
	pp.BackBufferWidth = desk.Width;
	pp.BackBufferHeight = desk.Height;
	pp.hDeviceWindow = hWnd;
	pp.BackBufferFormat = desk.Format;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	pp.FullScreen_RefreshRateInHz = ppRate;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	// The game asks for 4x MSAA in fullscreen when supported (GrpDevice.cpp, "Enable MSAA only in fullscreen")
	if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, desk.Format, FALSE, D3DMULTISAMPLE_4_SAMPLES, NULL)))
		pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;

	D3DDISPLAYMODEEX mode = { sizeof(mode) };
	mode.Width = desk.Width;
	mode.Height = desk.Height;
	mode.RefreshRate = modeRate;
	mode.Format = desk.Format;
	mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

	IDirect3DDevice9Ex* dev = nullptr;
	HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &mode, &dev);
	printf("pp %3u / mode %3u / MSAA %ux: hr=0x%08lX\n", ppRate, modeRate, (unsigned)pp.MultiSampleType, (unsigned long)hr);
	if (dev)
	{
		dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
		dev->Present(NULL, NULL, NULL, NULL);
		PrintCurrentMode(dev, "while fullscreen");
		if (waitForWmi)
		{
			printf("    (holding fullscreen 8 s so WMI can be read from another process)\n");
			for (int i = 0; i < 80; ++i)
			{
				dev->Present(NULL, NULL, NULL, NULL);
				Sleep(100);
			}
		}
		dev->Release();
	}
	return hr;
}

int main(int argc, char** argv)
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"FullscreenRefreshTest";
	RegisterClassW(&wc);
	HWND hWnd = CreateWindowW(wc.lpszClassName, L"test", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

	IDirect3D9Ex* d3d = nullptr;
	Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
	D3DDISPLAYMODE desk;
	d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &desk);
	printf("desktop %ux%u @ %u Hz\n", desk.Width, desk.Height, desk.RefreshRate);

	UINT rate = argc > 1 ? (UINT)atoi(argv[1]) : 0;
	if (!rate)
	{
		rate = desk.RefreshRate;
		UINT count = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, desk.Format);
		for (UINT i = 0; i < count; ++i)
		{
			D3DDISPLAYMODE m;
			if (SUCCEEDED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, desk.Format, i, &m)) && m.Width == desk.Width && m.Height == desk.Height && m.RefreshRate < rate)
				rate = m.RefreshRate;
		}
	}

	TryCreate(d3d, hWnd, desk, 0, 0, false);              // FREQUENCY 0 (unchanged by the fix)
	TryCreate(d3d, hWnd, desk, 0, rate, false);           // FREQUENCY != 0, old game code
	TryCreate(d3d, hWnd, desk, rate, rate, true);         // FREQUENCY != 0, fixed game code

	d3d->Release();
	DestroyWindow(hWnd);
	return 0;
}
