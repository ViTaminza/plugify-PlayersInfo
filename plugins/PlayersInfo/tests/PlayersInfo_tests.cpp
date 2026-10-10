// Tests for plugins/PlayersInfo/src/json_writer.hpp — the serialiser that has to produce the same
// bytes the original's nlohmann `j.dump(-1, ' ', false, error_handler::strict)` produced, because
// the example.com web module parses this document.
//
// Checked: compact form (no whitespace), object members sorted like nlohmann's std::map, integer
// and boolean literals, the escaping table (short escapes, \u00XX for other control bytes, UTF-8
// passthrough), one full mm_getinfo-shaped document compared byte for byte, and the entity-handle
// validity rule (src/ent_handle.hpp) the controller's m_hPlayerPawn read uses.

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ent_handle.hpp"
#include "json_writer.hpp"

namespace {

using playersinfo::Json;

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                             \
		}                                                                                          \
	} while (0)

void Check(const std::string& actual, const std::string& expected, const char* what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", what, expected.c_str(), actual.c_str());
	}
}

void TestScalars() {
	std::printf("-- scalars\n");
	Check(Json::Number(0).Text(), "0", "zero");
	Check(Json::Number(-17).Text(), "-17", "negative");
	Check(Json::Number(76561198000000000LL).Text(), "76561198000000000", "steamid");
	Check(Json::Bool(true).Text(), "true", "true");
	Check(Json::Bool(false).Text(), "false", "false");
	Check(Json::String("").Text(), "\"\"", "empty string");
	Check(Json::String("de_dust2").Text(), "\"de_dust2\"", "plain string");
}

void TestEscaping() {
	std::printf("-- escaping\n");
	Check(Json::String("a\"b").Text(), "\"a\\\"b\"", "quote");
	Check(Json::String("a\\b").Text(), "\"a\\\\b\"", "backslash");
	Check(Json::String("a\nb").Text(), "\"a\\nb\"", "newline");
	Check(Json::String("a\rb").Text(), "\"a\\rb\"", "carriage return");
	Check(Json::String("a\tb").Text(), "\"a\\tb\"", "tab");
	Check(Json::String("a\bb").Text(), "\"a\\bb\"", "backspace");
	Check(Json::String("a\fb").Text(), "\"a\\fb\"", "form feed");
	Check(Json::String(std::string("a\x01") + "b").Text(), "\"a\\u0001b\"", "control byte");
	// ensure_ascii=false: UTF-8 goes through untouched.
	Check(Json::String("K\xC3\xA4se").Text(), "\"K\xC3\xA4se\"", "utf-8 passthrough");
}

void TestContainers() {
	std::printf("-- containers\n");
	Check(Json::Array({}).Text(), "[]", "empty array");
	Check(Json::Array({Json::Number(1), Json::Number(2)}).Text(), "[1,2]", "array order is kept");

	std::map<std::string, Json> object;
	object["b"] = Json::Number(2);
	object["a"] = Json::Number(1);
	object["c"] = Json::String("x");
	Check(Json::Object(object).Text(), "{\"a\":1,\"b\":2,\"c\":\"x\"}", "object members sorted");
	Check(Json::Object({}).Text(), "{}", "empty object");
}

// The exact document the original produces for one player (keys in nlohmann's std::map order).
void TestFullDocument() {
	std::printf("-- full document\n");
	std::map<std::string, Json> player;
	player["userid"] = Json::Number(3);
	player["name"] = Json::String("Player<One>");
	player["team"] = Json::Number(2);
	player["steamid"] = Json::String("76561198000000000");
	player["kills"] = Json::Number(12);
	player["death"] = Json::Number(4);
	player["headshots"] = Json::Number(3);
	player["ping"] = Json::Number(31);
	player["playtime"] = Json::Number(1234);
	player["prime"] = Json::Bool(true);
	player["rank"] = Json::Number(7);

	std::map<std::string, Json> root;
	root["time"] = Json::Number(1759912345);
	root["current_map"] = Json::String("de_dust2");
	root["score_ct"] = Json::Number(5);
	root["score_t"] = Json::Number(3);
	root["players"] = Json::Array({Json::Object(player)});

	const std::string expected =
		"{\"current_map\":\"de_dust2\",\"players\":[{\"death\":4,\"headshots\":3,\"kills\":12,\"name\":\"Player<One>\","
		"\"ping\":31,\"playtime\":1234,\"prime\":true,\"rank\":7,\"steamid\":\"76561198000000000\",\"team\":2,\"userid\":3}],"
		"\"score_ct\":5,\"score_t\":3,\"time\":1759912345}";
	Check(Json::Object(root).Text(), expected, "mm_getinfo document");

	// A player without the optional rank key (no levels_ranks plugin).
	player.erase("rank");
	root["players"] = Json::Array({Json::Object(player)});
	const std::string withoutRank =
		"{\"current_map\":\"de_dust2\",\"players\":[{\"death\":4,\"headshots\":3,\"kills\":12,\"name\":\"Player<One>\","
		"\"ping\":31,\"playtime\":1234,\"prime\":true,\"steamid\":\"76561198000000000\",\"team\":2,\"userid\":3}],"
		"\"score_ct\":5,\"score_t\":3,\"time\":1759912345}";
	Check(Json::Object(root).Text(), withoutRank, "document without rank");
}

void TestHandleValidity() {
	std::printf("-- entity handle validity\n");
	// A handle seen in game: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	CHECK(playersinfo::IsValidHandle(-921534371));
	CHECK(playersinfo::IsValidHandle(0x7FFFFFFF));
	CHECK(playersinfo::IsValidHandle(1));
	CHECK(!playersinfo::IsValidHandle(0));  // "none" / index 0 (the world)
	CHECK(!playersinfo::IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("PlayersInfo_tests\n");

	TestScalars();
	TestEscaping();
	TestContainers();
	TestFullDocument();
	TestHandleValidity();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
