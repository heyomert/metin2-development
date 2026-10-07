#include "SQLFamily.h"

#include <cctype>
#include <cstring>
#include <string>

namespace
{
	enum class TokenType { END, WORD, IDENT_QUOTED, OTHER };

	struct Token
	{
		TokenType type = TokenType::END;
		std::string text; // only for WORD and IDENT_QUOTED (never filled from a string literal)
	};

	class Lexer
	{
	public:
		explicit Lexer(const char* s) : m_p(s ? s : "") {}

		Token Next()
		{
			for (;;)
			{
				while (*m_p && std::isspace((unsigned char)*m_p))
					++m_p;

				if (!*m_p)
					return {};

				const char c = *m_p;

				// Comments: "-- " and "#" end the statement for our purposes, /* ... */ is skipped
				if (c == '#' || (c == '-' && m_p[1] == '-'))
					return {};
				if (c == '/' && m_p[1] == '*')
				{
					const char* end = std::strstr(m_p + 2, "*/");
					if (!end)
						return {};
					m_p = end + 2;
					continue;
				}

				// String literal: skipped whole; an unterminated literal ends the scan (nothing after it is trusted)
				if (c == '\'' || c == '"')
				{
					if (!SkipLiteral(c))
						return {};
					Token t;
					t.type = TokenType::OTHER;
					return t;
				}

				if (c == '`')
				{
					const char* end = std::strchr(m_p + 1, '`');
					if (!end)
						return {};
					Token t;
					t.type = TokenType::IDENT_QUOTED;
					t.text.assign(m_p + 1, end - (m_p + 1));
					m_p = end + 1;
					return t;
				}

				if (std::isalpha((unsigned char)c) || c == '_')
				{
					const char* start = m_p;
					while (*m_p && (std::isalnum((unsigned char)*m_p) || *m_p == '_' || *m_p == '$' || *m_p == '.'))
						++m_p;
					Token t;
					t.type = TokenType::WORD;
					t.text.assign(start, m_p - start);
					return t;
				}

				++m_p;
				Token t;
				t.type = TokenType::OTHER;
				return t;
			}
		}

	private:
		bool SkipLiteral(char quote)
		{
			++m_p;
			while (*m_p)
			{
				if (*m_p == '\\')
				{
					if (!m_p[1])
						return false;
					m_p += 2;
					continue;
				}
				if (*m_p == quote)
				{
					if (m_p[1] == quote) // doubled quote inside the literal
					{
						m_p += 2;
						continue;
					}
					++m_p;
					return true;
				}
				++m_p;
			}
			return false;
		}

		const char* m_p;
	};

	std::string Lower(const std::string& s)
	{
		std::string r(s);
		for (char& ch : r)
			ch = (char)std::tolower((unsigned char)ch);
		return r;
	}

	bool Is(const Token& t, const char* word)
	{
		return t.type == TokenType::WORD && Lower(t.text) == word;
	}

	// A table name: letters, digits, '_' and '.' (schema.table), at most 32 characters; anything else is refused
	bool TableName(const Token& t, std::string& out)
	{
		if (t.type != TokenType::WORD && t.type != TokenType::IDENT_QUOTED)
			return false;
		if (t.text.empty() || t.text.size() > 32)
			return false;
		for (char ch : t.text)
			if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '.'))
				return false;
		out = Lower(t.text);
		return true;
	}

	const char* const VERBS[] = { "select", "insert", "replace", "update", "delete", "do", "set", "show", "call",
		"lock", "unlock", "alter", "create", "drop", "truncate", "begin", "commit", "rollback", "start" };
}

void SQLFamily(const char* sql, char* out, size_t outSize)
{
	if (!out || !outSize)
		return;

	Lexer lex(sql);
	const Token first = lex.Next();
	std::string verb;
	if (first.type == TokenType::WORD)
	{
		const std::string v = Lower(first.text);
		for (const char* known : VERBS)
			if (v == known)
				verb = v;
	}

	std::string label = verb.empty() ? "unknown" : verb;

	if (!verb.empty())
	{
		std::string table;
		if (verb == "update")
		{
			Token t = lex.Next();
			while (Is(t, "low_priority") || Is(t, "ignore"))
				t = lex.Next();
			TableName(t, table);
		}
		else if (verb == "insert" || verb == "replace" || verb == "delete" || verb == "select")
		{
			const char* key = (verb == "insert" || verb == "replace") ? "into" : "from";
			for (Token t = lex.Next(); t.type != TokenType::END; t = lex.Next())
			{
				if (Is(t, key))
				{
					TableName(lex.Next(), table);
					break;
				}
			}
		}

		if (!table.empty())
			label += "." + table;
	}

	std::strncpy(out, label.c_str(), outSize - 1);
	out[outSize - 1] = '\0';
}
