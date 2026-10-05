#pragma once

#include <windows.h>

#include <vector>

struct SDisplayMode
{
	UINT width;
	UINT height;
	UINT bpp;
	std::vector<UINT> refreshRates;
};

// Lists the modes the game accepts, with the same rules as CPythonSystem::GetDisplaySettings
// (UserInterface/PythonSystem.cpp): default adapter, desktop format, at least 800x600, 16 or 32 bpp.
bool EnumerateDisplayModes(std::vector<SDisplayMode>& modes);
