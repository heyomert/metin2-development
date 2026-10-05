#pragma once

#include <string>
#include <vector>

// Reads and writes config/metin2.cfg in the format CPythonSystem::LoadConfig/SaveConfig use
// (UserInterface/PythonSystem.cpp). Lines this tool does not manage are kept as they are.
class CConfigFile
{
	public:
		// Returns false only when the file exists but cannot be read; a missing file is an empty config.
		bool Load(const std::wstring& path);
		bool Save(const std::wstring& path, std::wstring& error) const;

		bool Has(const char* key) const;
		int GetInt(const char* key, int defaultValue) const;
		float GetFloat(const char* key, float defaultValue) const;

		void SetInt(const char* key, int value);
		void SetFloat(const char* key, float value);

	private:
		struct SLine
		{
			std::string key;
			std::string value;
			std::string text;
		};

		const SLine* Find(const char* key) const;
		void Set(const char* key, const std::string& value);

		std::vector<SLine> m_lines;
};
