#include "DisplayModes.h"

#include <d3d9.h>

#include <algorithm>

bool EnumerateDisplayModes(std::vector<SDisplayMode>& modes)
{
	modes.clear();

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d)
		return false;

	D3DDISPLAYMODE desktop;
	if (FAILED(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &desktop)))
	{
		d3d->Release();
		return false;
	}

	UINT count = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, desktop.Format);
	for (UINT i = 0; i < count; ++i)
	{
		D3DDISPLAYMODE mode;
		if (FAILED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, desktop.Format, i, &mode)))
			continue;

		if (mode.Width < 800 || mode.Height < 600)
			continue;

		UINT bpp;
		if (mode.Format == D3DFMT_R5G6B5)
			bpp = 16;
		else if (mode.Format == D3DFMT_X8R8G8B8)
			bpp = 32;
		else
			continue;

		auto it = std::find_if(modes.begin(), modes.end(), [&](const SDisplayMode& m) {
			return m.width == mode.Width && m.height == mode.Height && m.bpp == bpp;
		});

		if (it == modes.end())
		{
			modes.push_back({ mode.Width, mode.Height, bpp, { mode.RefreshRate } });
		}
		else if (std::find(it->refreshRates.begin(), it->refreshRates.end(), mode.RefreshRate) == it->refreshRates.end())
		{
			it->refreshRates.push_back(mode.RefreshRate);
		}
	}

	d3d->Release();

	std::sort(modes.begin(), modes.end(), [](const SDisplayMode& a, const SDisplayMode& b) {
		if (a.width != b.width)
			return a.width < b.width;
		return a.height < b.height;
	});

	for (SDisplayMode& mode : modes)
		std::sort(mode.refreshRates.begin(), mode.refreshRates.end());

	return !modes.empty();
}
