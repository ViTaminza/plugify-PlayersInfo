// json_writer — the minimal JSON serialiser this port needs.
//
// The original PlayersInfo builds its output with nlohmann::json and prints `j.dump(-1, ' ', false,
// error_handler::strict)`, i.e. a compact single-line document with the object members sorted
// (nlohmann's default object type is std::map). std::map ordering is reproduced here by storing
// members in a std::map; the escaping rules are nlohmann's for non-ASCII passthrough: `"` and `\`
// are escaped, the usual short escapes are used for \b \f \n \r \t, other control bytes become
// \u00XX, everything else (including UTF-8) is copied as-is.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace playersinfo {

class Json {
public:
	static Json Number(std::int64_t value) {
		Json json;
		json.m_text = std::to_string(value);
		return json;
	}

	static Json Bool(bool value) {
		Json json;
		json.m_text = value ? "true" : "false";
		return json;
	}

	static Json String(const std::string& value) {
		Json json;
		json.m_text = Escape(value);
		return json;
	}

	static Json Array(std::vector<Json> elements) {
		std::string text = "[";
		for (std::size_t i = 0; i < elements.size(); ++i) {
			if (i != 0) {
				text += ',';
			}
			text += elements[i].m_text;
		}
		text += ']';
		Json json;
		json.m_text = std::move(text);
		return json;
	}

	static Json Object(const std::map<std::string, Json>& members) {
		std::string text = "{";
		bool first = true;
		for (const auto& [key, value] : members) {
			if (!first) {
				text += ',';
			}
			first = false;
			text += Escape(key);
			text += ':';
			text += value.m_text;
		}
		text += '}';
		Json json;
		json.m_text = std::move(text);
		return json;
	}

	[[nodiscard]] const std::string& Text() const { return m_text; }

private:
	static std::string Escape(const std::string& input) {
		std::string out;
		out.reserve(input.size() + 2);
		out += '"';
		for (const char raw : input) {
			const auto byte = static_cast<unsigned char>(raw);
			switch (byte) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\b':
				out += "\\b";
				break;
			case '\f':
				out += "\\f";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (byte < 0x20) {
					static const char* digits = "0123456789abcdef";
					out += "\\u00";
					out += digits[(byte >> 4) & 0xf];
					out += digits[byte & 0xf];
				} else {
					out += raw;
				}
				break;
			}
		}
		out += '"';
		return out;
	}

	std::string m_text;
};

} // namespace playersinfo
