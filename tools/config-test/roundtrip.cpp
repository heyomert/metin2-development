// Round-trip test for client-src/src/Config/ConfigFile: write settings the way config.exe does, then read the file
// back with a copy of the game's parser (CPythonSystem::LoadConfig, client-src/src/UserInterface/PythonSystem.cpp)
// and check every value plus the keys config.exe does not manage.
#include "../../client-src/src/Config/ConfigFile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

static int g_failures = 0;

static void Expect(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		++g_failures;
}

// Same loop as CPythonSystem::LoadConfig: stop at the first line sscanf reports EOF for, keep the last value per key.
static std::map<std::string, std::string> GameParse(const wchar_t* path)
{
	std::map<std::string, std::string> values;
	FILE* fp = _wfopen(path, L"rt");
	if (!fp)
		return values;

	char buf[256], command[256], value[256];
	while (fgets(buf, 256, fp))
	{
		if (sscanf(buf, " %s %s\n", command, value) == EOF)
			break;
		for (char* c = command; *c; ++c) *c = (char)toupper(*c);
		values[command] = value;
	}

	fclose(fp);
	return values;
}

static void WriteFile(const wchar_t* path, const char* text)
{
	FILE* fp = _wfopen(path, L"wt");
	fputs(text, fp);
	fclose(fp);
}

static void Run(const char* name, const char* input, const wchar_t* path)
{
	printf("%s\n", name);
	WriteFile(path, input);
	std::map<std::string, std::string> before = GameParse(path);
	std::string inputGamma = before.count("GAMMA") ? before["GAMMA"] : std::string();

	CConfigFile cfg;
	Expect(cfg.Load(path), "load");

	// What SaveDialog writes for: 1366x768, 60 Hz, music 45%, sfx 80%, fullscreen, external IME,
	// software cursor, fog 1, tiling 1, shadow 5
	cfg.SetInt("WIDTH", 1366);
	cfg.SetInt("HEIGHT", 768);
	cfg.SetInt("BPP", 32);
	cfg.SetInt("FREQUENCY", 60);
	cfg.SetFloat("MUSIC_VOLUME", 45 / 100.0f);
	cfg.SetFloat("VOICE_VOLUME", 80 / 100.0f);
	cfg.SetInt("SOFTWARE_CURSOR", 1);
	cfg.SetInt("WINDOWED", 0);
	cfg.SetInt("USE_DEFAULT_IME", 1);
	cfg.SetInt("FOG_LEVEL", 1);
	cfg.SetInt("SOFTWARE_TILING", 1);
	cfg.SetInt("SHADOW_LEVEL", 5);

	std::wstring error;
	Expect(cfg.Save(path, error), "save");

	std::map<std::string, std::string> v = GameParse(path);
	Expect(atoi(v["WIDTH"].c_str()) == 1366 && atoi(v["HEIGHT"].c_str()) == 768, "game reads 1366x768");
	Expect(atoi(v["BPP"].c_str()) == 32 && atoi(v["FREQUENCY"].c_str()) == 60, "game reads 32 bpp, 60 Hz");
	// find(), not operator[]: a missing key must stay missing for the stability check below
	auto gamma = v.find("GAMMA");
	Expect((gamma == v.end() ? std::string() : gamma->second) == inputGamma, "GAMMA (not managed by config.exe) kept as it was");
	Expect(fabs(atof(v["MUSIC_VOLUME"].c_str()) - 0.45) < 1e-6 && fabs(atof(v["VOICE_VOLUME"].c_str()) - 0.80) < 1e-6, "game reads volumes 0.450 / 0.800");
	Expect(atoi(v["SOFTWARE_CURSOR"].c_str()) == 1 && atoi(v["WINDOWED"].c_str()) == 0 && atoi(v["USE_DEFAULT_IME"].c_str()) == 1, "game reads cursor 1, fullscreen, external IME");
	Expect(atoi(v["FOG_LEVEL"].c_str()) == 1 && atoi(v["SOFTWARE_TILING"].c_str()) == 1 && atoi(v["SHADOW_LEVEL"].c_str()) == 5, "game reads fog 1, tiling 1, shadow 5");

	// Second save must not duplicate keys or grow the file
	CConfigFile again;
	again.Load(path);
	again.Save(path, error);
	std::map<std::string, std::string> v2 = GameParse(path);
	Expect(v2 == v, "load + save again is stable");
}

int main()
{
	const wchar_t* path = L"roundtrip_metin2.cfg";

	// File written by the game (client/config/metin2.cfg, 2026-10-05)
	Run("game-written file",
		"WIDTH\t\t\t\t\t\t1024\nHEIGHT\t\t\t\t\t\t768\nBPP\t\t\t\t\t\t32\nFREQUENCY\t\t\t\t\t50\nSOFTWARE_CURSOR\t\t\t0\n"
		"OBJECT_CULLING\t\t\t\t1\nVISIBILITY\t\t\t\t\t3\nMUSIC_VOLUME\t\t\t\t0.000\nVOICE_VOLUME\t\t\t\t0.000\nGAMMA\t\t\t\t\t\t1\n"
		"IS_SAVE_ID\t\t\t\t\t0\nSAVE_ID\t\t\t\t\t0\nPRE_LOADING_DELAY_TIME\t\t20\nDECOMPRESSED_TEXTURE\t\t0\nWINDOWED\t\t\t1\n"
		"USE_DEFAULT_IME\t\t0\nSOFTWARE_TILING\t\t2\nSHADOW_LEVEL\t\t\t3\nFOG_LEVEL\t\t\t\t2\n\n", path);
	std::map<std::string, std::string> v = GameParse(path);
	Expect(v["OBJECT_CULLING"] == "1" && v["VISIBILITY"] == "3" && v["IS_SAVE_ID"] == "0" && v["SAVE_ID"] == "0"
		&& v["PRE_LOADING_DELAY_TIME"] == "20" && v["DECOMPRESSED_TEXTURE"] == "0", "unmanaged keys kept");

	// File written by the old config.exe (client/metin2.cfg, 2026-10-05): integer volumes, missing keys
	Run("old config.exe file",
		"WIDTH\t\t\t\t\t1360\nHEIGHT\t\t\t\t\t768\nBPP\t\t\t\t\t32\nFREQUENCY\t\t\t\t60\nSOFTWARE_CURSOR\t\t0\nVISIBILITY\t\t\t\t3\n"
		"SOFTWARE_TILING\t\t0\nSHADOW_LEVEL\t\t\t3\nMUSIC_VOLUME\t\t\t2\nVOICE_VOLUME\t\t\t5\nGAMMA\t\t\t\t\t3\nWINDOWED\t\t\t\t0\nUSE_DEFAULT_IME\t\t0\n\n", path);

	// A blank line in the middle: the game stops reading there, so it must not survive a save
	Run("blank line in the middle", "WIDTH 800\n\nHEIGHT 600\nSAVE_ID abc\n", path);
	v = GameParse(path);
	Expect(v["SAVE_ID"] == "abc", "key after the blank line is visible to the game after saving");

	// Missing file: treated as empty config
	_wremove(path);
	CConfigFile missing;
	printf("missing file\n");
	Expect(missing.Load(path), "missing file loads as empty config");

	printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
	return g_failures ? 1 : 0;
}
