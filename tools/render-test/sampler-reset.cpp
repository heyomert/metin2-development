// What does IDirect3DDevice9Ex::Reset do to sampler states? CStateManager (client-src/src/EterLib/StateManager.cpp)
// caches sampler states and only writes a value that differs from the cache; after Reset the game calls
// SetDefaultState, whose ResetState() clears the render/texture-stage caches but not m_SamplerStates.
// Windowed device, no display mode change.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

static void Print(IDirect3DDevice9Ex* dev, const char* when)
{
	DWORD minf, magf, mipf, aniso;
	dev->GetSamplerState(0, D3DSAMP_MINFILTER, &minf);
	dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &magf);
	dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &mipf);
	dev->GetSamplerState(0, D3DSAMP_MAXANISOTROPY, &aniso);
	printf("%-28s MINFILTER=%lu MAGFILTER=%lu MIPFILTER=%lu MAXANISOTROPY=%lu   (POINT=1 LINEAR=2 ANISOTROPIC=3, NONE=0)\n", when, minf, magf, mipf, aniso);
}

int main()
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"SamplerResetTest";
	RegisterClassW(&wc);
	HWND hWnd = CreateWindowW(wc.lpszClassName, L"test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);

	IDirect3D9Ex* d3d = nullptr;
	Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);

	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.hDeviceWindow = hWnd;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.BackBufferWidth = 320;
	pp.BackBufferHeight = 240;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;

	IDirect3DDevice9Ex* dev = nullptr;
	HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL, &dev);
	printf("CreateDeviceEx hr=0x%08lX\n", (unsigned long)hr);
	if (FAILED(hr))
		return 1;

	Print(dev, "fresh device (defaults):");

	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 4);
	Print(dev, "after SetSamplerState:");

	pp.BackBufferWidth = 400; // same as CGraphicDevice::ResizeBackBuffer
	hr = dev->Reset(&pp);
	printf("Reset hr=0x%08lX\n", (unsigned long)hr);
	Print(dev, "after Reset:");

	dev->Release();
	d3d->Release();
	DestroyWindow(hWnd);
	return 0;
}
