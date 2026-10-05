#include "ConfigFile.h"

#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool CConfigFile::Load(const std::wstring& path)
{
	m_lines.clear();

	FILE* fp = _wfopen(path.c_str(), L"rt");
	if (!fp)
		return errno == ENOENT;

	char buf[256];
	while (fgets(buf, sizeof(buf), fp))
	{
		char key[256] = {};
		char value[256] = {};

		// Same tokenizing as CPythonSystem::LoadConfig: first word is the key, second the value.
		// The game stops reading at the first blank line, so blank lines are not kept.
		if (sscanf(buf, " %255s %255s", key, value) < 1)
			continue;

		std::string text = buf;
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();

		m_lines.push_back({ key, value, text });
	}

	fclose(fp);
	return true;
}

bool CConfigFile::Save(const std::wstring& path, std::wstring& error) const
{
	std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
	if (!CreateDirectoryW(dir.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		error = L"Klasör oluşturulamadı: " + dir;
		return false;
	}

	// Write a temporary file first so a failed write never leaves a truncated config behind.
	std::wstring tempPath = path + L".tmp";
	FILE* fp = _wfopen(tempPath.c_str(), L"wt");
	if (!fp)
	{
		error = L"Dosya yazılamadı: " + tempPath;
		return false;
	}

	bool ok = true;
	for (const SLine& line : m_lines)
		ok &= fprintf(fp, "%s\n", line.text.c_str()) >= 0;

	// SaveConfig ends the file with an empty line; anything after it would be ignored by the game.
	ok &= fprintf(fp, "\n") >= 0;
	ok &= fclose(fp) == 0;

	if (!ok || !MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(tempPath.c_str());
		error = L"Dosya kaydedilemedi: " + path;
		return false;
	}

	return true;
}

const CConfigFile::SLine* CConfigFile::Find(const char* key) const
{
	for (const SLine& line : m_lines)
	{
		if (!_stricmp(line.key.c_str(), key))
			return &line;
	}

	return nullptr;
}

bool CConfigFile::Has(const char* key) const
{
	return Find(key) != nullptr;
}

int CConfigFile::GetInt(const char* key, int defaultValue) const
{
	const SLine* line = Find(key);
	return line ? atoi(line->value.c_str()) : defaultValue;
}

float CConfigFile::GetFloat(const char* key, float defaultValue) const
{
	const SLine* line = Find(key);
	return line ? (float)atof(line->value.c_str()) : defaultValue;
}

void CConfigFile::Set(const char* key, const std::string& value)
{
	std::string text = std::string(key) + "\t\t\t" + value;

	for (SLine& line : m_lines)
	{
		if (!_stricmp(line.key.c_str(), key))
		{
			line.value = value;
			line.text = text;
			return;
		}
	}

	m_lines.push_back({ key, value, text });
}

void CConfigFile::SetInt(const char* key, int value)
{
	Set(key, std::to_string(value));
}

void CConfigFile::SetFloat(const char* key, float value)
{
	// SaveConfig writes the volumes with "%.3f"
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", value);
	Set(key, buf);
}
