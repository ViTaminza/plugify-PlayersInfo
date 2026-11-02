// Tests for the engine-free parts of plugins/PlayersInfo (no pisexcompat, no Plugify host):
//
//   src/json_writer.hpp     the serialiser that has to produce the same bytes the original's
//                           nlohmann `j.dump(-1, ' ', false, error_handler::strict)` produced,
//                           because the example.com web module parses this document. Checked:
//                           compact form (no whitespace), object members sorted like nlohmann's
//                           std::map, integer and boolean literals, the escaping table (short
//                           escapes, \u00XX for other control bytes, UTF-8 passthrough) and one
//                           full mm_getinfo-shaped document compared byte for byte.
//   src/ent_handle.hpp      the entity-handle validity rule the controller's m_hPlayerPawn read
//                           uses (sign is meaningless; only 0 and -1 mean "no entity").
//   src/map_gate.hpp        the PITFALLS B1/B2 map gate: OnMapStart/OnMapEnd transitions, the
//                           mid-map latch of the first OnPluginUpdate, the "no listeners -> never
//                           open" rule and the teardown.
//   src/connection_times.hpp the original's g_iConnection[64] table: connect/disconnect by slot,
//                           slot bounds and the once-only seeding for a plugin loaded mid-map.
//   src/command_lifecycle.hpp the mm_getinfo registration and teardown (PITFALLS C11/A7), driven
//                           through fake s2sdk bindings defined below: a command we created is
//                           removed with RemoveCommand, one we only hooked is detached with
//                           RemoveCommandListener (never RemoveCommand - that would delete the
//                           other owner's command), a missing AddConsoleCommand registers and
//                           removes nothing, and only the server console may run the command.
//   src/console_output.hpp  the chunked console output: s2sdk's PrintToServer truncates at 2047
//                           bytes (live bug on 27035: 18 bots -> 2047-byte truncated JSON), so a
//                           document is printed in pieces of at most 1800 bytes cut between JSON
//                           tokens. Checked on synthetic 18- and 64-player documents with UTF-8,
//                           quotes, backslashes and commas inside strings: every chunk fits, the
//                           chunks concatenate back to the document, the newline-separated whole
//                           parses as one object through nlohmann, and an indivisible token becomes
//                           one explicit error object. Both the fake PrintToServer binding and the
//                           missing-binding case are exercised.
//
// Only the s2sdk binding slots used by the plugin (three command ones + PrintToServer) are defined
// here as fakes; nothing links pisexcompat, s2sdk_bindings or an engine.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "command_lifecycle.hpp"
#include "connection_times.hpp"
#include "console_output.hpp"
#include "ent_handle.hpp"
#include "json_writer.hpp"
#include "map_gate.hpp"

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

// --- fake s2sdk command bindings (runtime, no engine) ------------------------------------------

// The mm_getinfo trampoline the tests hand to the fake bindings (the plugin passes its own).
s2sdk::ResultType TestCommandCallback(std::int32_t /*caller*/, s2sdk::ConCommandContext /*context*/,
									  const plg::vector<plg::string>& /*args*/) {
	return s2sdk::ResultType::Handled;
}

// Everything the fake bindings saw, so a test can assert on the exact call a teardown made.
struct FakeCommandApi {
	int addCalls = 0;
	int removeCommandCalls = 0;
	int removeListenerCalls = 0;
	bool addResult = true; // what AddConsoleCommand answers: true = created, false = already existed
	s2sdk::ConVarFlag lastFlags = s2sdk::ConVarFlag::None;
	s2sdk::HookMode lastAddMode = s2sdk::HookMode::Pre;
	s2sdk::HookMode lastRemoveListenerMode = s2sdk::HookMode::Pre;
	s2sdk::ConCommandCallback lastCallback = nullptr;
	std::string lastName;
};

FakeCommandApi g_fake;

void ResetFake(bool addResult) {
	g_fake = FakeCommandApi();
	g_fake.addResult = addResult;
}

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode mode) {
	++g_fake.addCalls;
	g_fake.lastName.assign(name.data(), name.size());
	g_fake.lastFlags = flags;
	g_fake.lastCallback = callback;
	g_fake.lastAddMode = mode;
	return g_fake.addResult;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	++g_fake.removeCommandCalls;
	g_fake.lastName.assign(name.data(), name.size());
	g_fake.lastCallback = callback;
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	++g_fake.removeListenerCalls;
	g_fake.lastName.assign(name.data(), name.size());
	g_fake.lastCallback = callback;
	g_fake.lastRemoveListenerMode = mode;
	return true;
}

// --- fake s2sdk PrintToServer (the chunked console output) -------------------------------------

// Every message the plugin handed to PrintToServer, in order. The newline s2sdk appends is not part
// of the value: it is the real module's ConMsg("%s\n", ...) decoration, added per call.
std::vector<std::string> g_printed;

void FakePrintToServer(const plg::string& message) {
	g_printed.emplace_back(message.data(), message.size());
}

void ResetPrinted() {
	g_printed.clear();
}

} // namespace

// The binding slots the plugin uses. In the plugin they are defined by s2sdk_bindings and filled by
// the plugify module; here they point at the fakes, which is what makes the command lifecycle and
// the chunked document printing testable at runtime.
extern "C" {
s2sdk::_AddConsoleCommand __s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
s2sdk::_RemoveCommand __s2sdk_RemoveCommand = &FakeRemoveCommand;
s2sdk::_RemoveCommandListener __s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
s2sdk::_PrintToServer __s2sdk_PrintToServer = &FakePrintToServer;
}

namespace {

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

	// The "no map" answer of OnMmGetInfo, byte for byte.
	Check(Json::Object({{"error", Json::String("no map")}}).Text(), "{\"error\":\"no map\"}", "no-map answer");
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

void TestMapGate() {
	std::printf("-- map gate\n");
	using playersinfo::MapGate;

	// OnPluginStart: the gate is shut and nothing has been decided yet.
	MapGate gate;
	gate.Reset();
	CHECK(!gate.Ready());
	CHECK(!gate.BetweenMaps());
	CHECK(!gate.ListenersRegistered());

	// A plugin loaded into a running map: listeners registered, no OnMapEnd seen, a map name is
	// there -> the first update opens the gate; the following ones do not change it.
	gate.SetListenersRegistered(true);
	CHECK(!gate.Ready());
	gate.OnPluginUpdate(true);
	CHECK(gate.Ready());
	gate.OnPluginUpdate(true);
	CHECK(gate.Ready());

	// OnMapEnd: every client/entity/schema call is illegal again, so only state is dropped.
	gate.OnMapEnd();
	CHECK(!gate.Ready());
	CHECK(gate.BetweenMaps());
	gate.OnPluginUpdate(true); // no second latch: only OnMapStart may open the gate again
	CHECK(!gate.Ready());
	CHECK(gate.BetweenMaps());

	// OnMapStart of the next map: open again, and no longer between maps.
	gate.OnMapStart();
	CHECK(gate.Ready());
	CHECK(!gate.BetweenMaps());

	// OnPluginEnd: closed, a stale callback cannot reach a client.
	gate.Close();
	CHECK(!gate.Ready());

	// A plugin that starts before the very first map: no map name yet -> the latch must not open
	// the gate (a schema lookup would walk a null serializer map, PITFALLS C8), and it opens as
	// soon as a map name exists (no OnMapEnd in between).
	MapGate boot;
	boot.Reset();
	boot.SetListenersRegistered(true);
	boot.OnPluginUpdate(false);
	CHECK(!boot.Ready());
	CHECK(!boot.BetweenMaps());
	boot.OnPluginUpdate(true);
	CHECK(boot.Ready());

	// Without the map listeners nothing could ever close the gate again: it stays shut forever.
	MapGate noListeners;
	noListeners.Reset();
	noListeners.OnPluginUpdate(true);
	CHECK(!noListeners.Ready());
	noListeners.SetListenersRegistered(true); // registering later is not the plugin's flow
	noListeners.OnPluginUpdate(true);
	CHECK(noListeners.Ready()); // the latch is idempotent, but nothing would close it again
	// ... and OnMapStart still opens it (the engine's own signal needs no latch).
	MapGate noListenersMapStart;
	noListenersMapStart.Reset();
	noListenersMapStart.OnMapStart();
	CHECK(noListenersMapStart.Ready());

	// A plugin loaded between maps (OnMapEnd seen before the first update): the latch must wait
	// for the next OnMapStart, or the first client query would hit the emptied client list.
	MapGate betweenMaps;
	betweenMaps.Reset();
	betweenMaps.SetListenersRegistered(true);
	betweenMaps.OnMapEnd();
	betweenMaps.OnPluginUpdate(true);
	CHECK(!betweenMaps.Ready());
	CHECK(betweenMaps.BetweenMaps());
	betweenMaps.OnMapStart();
	CHECK(betweenMaps.Ready());

	// `plg reload`: Reset returns every field to the fresh state.
	MapGate reloaded;
	reloaded.SetListenersRegistered(true);
	reloaded.OnMapStart();
	CHECK(reloaded.Ready());
	reloaded.Reset();
	CHECK(!reloaded.Ready());
	CHECK(!reloaded.BetweenMaps());
	CHECK(!reloaded.ListenersRegistered());
	reloaded.SetListenersRegistered(true);
	reloaded.OnPluginUpdate(true);
	CHECK(reloaded.Ready());
}

void TestConnectionTimes() {
	std::printf("-- connection times\n");
	using playersinfo::ConnectionTimes;

	CHECK(playersinfo::kMaxSlots == 64);

	ConnectionTimes times;
	// Nothing connected yet: every slot is filtered out of the document.
	for (std::int32_t slot = 0; slot < playersinfo::kMaxSlots; ++slot) {
		CHECK(!times.IsPresent(slot));
	}

	CHECK(times.Connect(3, 100));
	CHECK(times.IsPresent(3));
	CHECK(times.ConnectedAt(3) == 100);
	CHECK(!times.IsPresent(2));

	// A later connect overwrites the timestamp (a rejoin, or a duplicate event).
	CHECK(times.Connect(3, 250));
	CHECK(times.ConnectedAt(3) == 250);

	// player_disconnect clears the entry: playtime becomes 0 and the slot is not listed.
	CHECK(times.Disconnect(3));
	CHECK(times.IsPresent(3) == false);
	CHECK(times.ConnectedAt(3) == 0);
	// A repeated disconnect is still an in-range event; out-of-range slots are refused.
	CHECK(times.Disconnect(3));
	CHECK(!times.Connect(-1, 100)); // an event field that did not resolve
	CHECK(!times.Connect(playersinfo::kMaxSlots, 100));
	CHECK(!times.Connect(playersinfo::kMaxSlots + 10, 100));
	CHECK(!times.Disconnect(-1));
	CHECK(!times.Disconnect(playersinfo::kMaxSlots));
	CHECK(times.ConnectedAt(-1) == 0);
	CHECK(times.ConnectedAt(playersinfo::kMaxSlots) == 0);
	CHECK(!times.IsPresent(-1));

	// Seeding of a plugin loaded mid-map: only slots without a timestamp that the caller accepts
	// (connected, in game, not a bot) are filled, and the call answers how many were filled.
	ConnectionTimes seed;
	CHECK(seed.Connect(0, 50)); // this player's timestamp is known: never overwritten
	const int seeded = seed.Seed(700, [](std::int32_t slot) { return slot == 0 || slot == 4 || slot == 7; });
	CHECK(seeded == 2);
	CHECK(seed.ConnectedAt(0) == 50);
	CHECK(seed.ConnectedAt(4) == 700);
	CHECK(seed.ConnectedAt(7) == 700);
	CHECK(!seed.IsPresent(5));

	// Nothing accepted (no players in game, or only bots) -> nothing seeded.
	ConnectionTimes empty;
	CHECK(empty.Seed(700, [](std::int32_t) { return false; }) == 0);
	for (std::int32_t slot = 0; slot < playersinfo::kMaxSlots; ++slot) {
		CHECK(!empty.IsPresent(slot));
	}

	// Seeding is idempotent: the second pass finds no empty slot it would accept.
	CHECK(seed.Seed(900, [](std::int32_t slot) { return slot == 4 || slot == 7; }) == 0);
	CHECK(seed.ConnectedAt(4) == 700);

	// Reset (OnPluginStart / OnPluginEnd) empties the table again.
	seed.Reset();
	for (std::int32_t slot = 0; slot < playersinfo::kMaxSlots; ++slot) {
		CHECK(!seed.IsPresent(slot));
	}
}

void TestCommandLifecycle() {
	std::printf("-- command lifecycle\n");
	using playersinfo::CommandOwnership;
	using playersinfo::CommandTeardown;
	constexpr s2sdk::ConVarFlag kFlags = s2sdk::ConVarFlag::GameDll;
	constexpr s2sdk::HookMode kMode = s2sdk::HookMode::Post;

	// 1. No AddConsoleCommand binding: nothing is registered, so nothing may be removed either
	//    (the old teardown called RemoveCommand("mm_getinfo") even then).
	const s2sdk::_AddConsoleCommand savedAdd = __s2sdk_AddConsoleCommand;
	__s2sdk_AddConsoleCommand = nullptr;
	ResetFake(true);
	CHECK(playersinfo::RegisterCommand("mm_getinfo", "", kFlags, &TestCommandCallback, kMode) ==
		  CommandOwnership::NotAttempted);
	CHECK(g_fake.addCalls == 0);
	CHECK(playersinfo::UnregisterCommand("mm_getinfo", &TestCommandCallback, kMode,
										 CommandOwnership::NotAttempted) == CommandTeardown::None);
	CHECK(g_fake.removeCommandCalls == 0);
	CHECK(g_fake.removeListenerCalls == 0);
	__s2sdk_AddConsoleCommand = savedAdd;

	// 2. The command did not exist and s2sdk created it: the plugin owns it, so OnPluginEnd removes
	//    it with RemoveCommand (which also drops the callback s2sdk attached).
	ResetFake(/*addResult=*/true);
	const CommandOwnership created =
		playersinfo::RegisterCommand("mm_getinfo", "", kFlags, &TestCommandCallback, kMode);
	CHECK(created == CommandOwnership::Created);
	CHECK(g_fake.addCalls == 1);
	CHECK(g_fake.lastName == "mm_getinfo");
	CHECK(g_fake.lastFlags == kFlags); // FCVAR_GAMEDLL: a server console command
	CHECK(g_fake.lastAddMode == kMode);
	CHECK(g_fake.lastCallback == &TestCommandCallback);
	CHECK(playersinfo::UnregisterCommand("mm_getinfo", &TestCommandCallback, kMode, created) ==
		  CommandTeardown::RemovedCommand);
	CHECK(g_fake.removeCommandCalls == 1);
	CHECK(g_fake.removeListenerCalls == 0);
	CHECK(g_fake.lastName == "mm_getinfo");
	CHECK(g_fake.lastCallback == &TestCommandCallback);

	// 3. The name already existed (another plugin, the engine, or our own entry kept by s2sdk
	//    across a `plg reload`): only the callback s2sdk attached belongs to this plugin, so the
	//    teardown detaches it with RemoveCommandListener and never calls RemoveCommand, which would
	//    delete the other owner's command.
	ResetFake(/*addResult=*/false);
	const CommandOwnership existing =
		playersinfo::RegisterCommand("mm_getinfo", "", kFlags, &TestCommandCallback, kMode);
	CHECK(existing == CommandOwnership::Existing);
	CHECK(g_fake.addCalls == 1);
	CHECK(playersinfo::UnregisterCommand("mm_getinfo", &TestCommandCallback, kMode, existing) ==
		  CommandTeardown::RemovedListener);
	CHECK(g_fake.removeCommandCalls == 0);
	CHECK(g_fake.removeListenerCalls == 1);
	CHECK(g_fake.lastName == "mm_getinfo");
	CHECK(g_fake.lastCallback == &TestCommandCallback);
	CHECK(g_fake.lastRemoveListenerMode == kMode); // the same mode the callback was attached with

	// 4. The teardown binding is missing: the plugin stays silent instead of crashing.
	const s2sdk::_RemoveCommand savedRemove = __s2sdk_RemoveCommand;
	__s2sdk_RemoveCommand = nullptr;
	ResetFake(true);
	CHECK(playersinfo::UnregisterCommand("mm_getinfo", &TestCommandCallback, kMode,
										 CommandOwnership::Created) == CommandTeardown::None);
	CHECK(g_fake.removeCommandCalls == 0);
	__s2sdk_RemoveCommand = savedRemove;

	const s2sdk::_RemoveCommandListener savedListener = __s2sdk_RemoveCommandListener;
	__s2sdk_RemoveCommandListener = nullptr;
	ResetFake(false);
	CHECK(playersinfo::UnregisterCommand("mm_getinfo", &TestCommandCallback, kMode,
										 CommandOwnership::Existing) == CommandTeardown::None);
	CHECK(g_fake.removeListenerCalls == 0);
	__s2sdk_RemoveCommandListener = savedListener;

	// 5. PITFALLS C11: the command dumps the document for the server console (-1) and for nothing
	//    else - the original's callback answered exactly `slot == -1`. s2sdk also dispatches the
	//    chat spellings "!mm_getinfo"/"/mm_getinfo" to the same callback with the player's slot, and
	//    any other negative value (another engine caller, a future s2sdk) must not open the printer
	//    either: a player slot reaching it would leak the whole server picture into their console.
	int prints = 0;
	CHECK(!playersinfo::RunForServerConsoleOnly(0, [&prints] { ++prints; }));
	CHECK(!playersinfo::RunForServerConsoleOnly(3, [&prints] { ++prints; }));
	CHECK(!playersinfo::RunForServerConsoleOnly(-2, [&prints] { ++prints; }));
	CHECK(!playersinfo::RunForServerConsoleOnly(std::numeric_limits<std::int32_t>::min(),
												[&prints] { ++prints; }));
	CHECK(prints == 0);
	CHECK(playersinfo::RunForServerConsoleOnly(-1, [&prints] { ++prints; }));
	CHECK(prints == 1);
	// The refused call answers "not handled" in the plugin (Continue); the accepted one prints and
	// answers Handled - both paths are exercised through the dispatcher, nothing else runs here.
}

// --- chunked console output ---------------------------------------------------------------------

// A player name that contains everything a JSON splitter must not cut through: UTF-8 (two bytes and
// a Cyrillic word), a quote, a backslash and commas/colons/brackets.
std::string SyntheticName(int index) {
	static const char* const kNames[] = {
		"Player",
		"K\xC3\xA4" "se-\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA",
		"\"quoted\",colon:brace{}bracket[]",
		"back\\slash,with,commas",
	};
	std::string name(kNames[index % 4]);
	name += '-';
	name += std::to_string(index);
	return name;
}

// The same shape BuildServerInfo() produces, with every key and a realistic set of values.
std::string BuildSyntheticDocument(int playerCount) {
	std::map<std::string, Json> root;
	root["time"] = Json::Number(1759912345);
	root["current_map"] = Json::String("de_dust2");
	root["score_ct"] = Json::Number(5);
	root["score_t"] = Json::Number(3);

	std::vector<Json> players;
	for (int i = 0; i < playerCount; ++i) {
		std::map<std::string, Json> player;
		player["userid"] = Json::Number(i);
		player["name"] = Json::String(SyntheticName(i));
		player["team"] = Json::Number(2 + (i % 2));
		player["steamid"] = Json::String(std::to_string(76561198000000000ULL + static_cast<unsigned>(i)));
		player["kills"] = Json::Number(i * 3);
		player["death"] = Json::Number(i);
		player["headshots"] = Json::Number(i / 2);
		player["ping"] = Json::Number(10 + i);
		player["playtime"] = Json::Number(100 + i * 7);
		player["prime"] = Json::Bool(i % 2 == 0);
		player["rank"] = Json::Number(i % 18);
		players.push_back(Json::Object(player));
	}
	root["players"] = Json::Array(std::move(players));
	return Json::Object(root).Text();
}

// What s2sdk's PrintToServer adds per call: ConMsg("%s\n", ...).
std::string JoinAsConsoleLines(const std::vector<std::string>& lines) {
	std::string joined;
	for (const std::string& line : lines) {
		joined += line;
		joined.push_back('\n');
	}
	return joined;
}

void TestJsonChunking() {
	std::printf("-- console chunking\n");
	constexpr std::size_t kLimit = playersinfo::kMaxConsoleChunkBytes;
	CHECK(kLimit == 1800);

	// A document that already fits stays exactly one console message.
	{
		const std::string small = BuildSyntheticDocument(1);
		CHECK(small.size() <= kLimit);
		std::vector<std::string_view> chunks;
		CHECK(playersinfo::SplitJsonForConsole(small, kLimit, chunks));
		CHECK(chunks.size() == 1);
		CHECK(chunks[0] == small);
	}

	// The live case (18 bots truncated to 2047 bytes) and a full 64-slot document.
	for (const int playerCount : {18, 64}) {
		const std::string document = BuildSyntheticDocument(playerCount);
		CHECK(document.size() > kLimit); // the one-call PrintToServer cut this off

		std::vector<std::string_view> chunks;
		CHECK(playersinfo::SplitJsonForConsole(document, kLimit, chunks));
		CHECK(chunks.size() >= 2);

		bool everyChunkFits = true;
		bool everyChunkNonEmpty = true;
		for (const std::string_view chunk : chunks) {
			everyChunkFits = everyChunkFits && chunk.size() <= kLimit;
			everyChunkNonEmpty = everyChunkNonEmpty && !chunk.empty();
		}
		CHECK(everyChunkFits);
		CHECK(everyChunkNonEmpty);

		// Concatenating the lines reintroduces only newlines, so the split is lossless ...
		std::string withoutNewlines;
		for (const std::string_view chunk : chunks) {
			withoutNewlines.append(chunk);
		}
		CHECK(withoutNewlines == document);

		// ... and the newline-separated whole is one parseable JSON object with every player in it.
		std::string consoleOutput;
		for (const std::string_view chunk : chunks) {
			consoleOutput.append(chunk);
			consoleOutput.push_back('\n');
		}
		const nlohmann::json parsed = nlohmann::json::parse(consoleOutput);
		CHECK(parsed.is_object());
		CHECK(parsed["current_map"] == "de_dust2");
		CHECK(parsed["players"].size() == static_cast<std::size_t>(playerCount));
		CHECK(parsed["players"][0]["name"] == SyntheticName(0));
		CHECK(parsed["players"][1]["name"] == SyntheticName(1)); // UTF-8
		CHECK(parsed["players"][2]["name"] == SyntheticName(2)); // quotes, commas, brackets
		CHECK(parsed["players"][3]["name"] == SyntheticName(3)); // backslashes and commas
		CHECK(parsed["players"][static_cast<std::size_t>(playerCount) - 1]["userid"] ==
			  playerCount - 1);
		CHECK(parsed.dump() == document); // nothing but whitespace changed
	}

	// Commas, colons and braces inside a string are never split points: with a 12-byte limit the
	// only safe cuts are after the structural characters, so the string value survives intact.
	{
		const std::string document = "{\"a\":\"1,2,3\",\"b\":4}";
		std::vector<std::string_view> chunks;
		CHECK(playersinfo::SplitJsonForConsole(document, 12, chunks));
		CHECK(chunks.size() >= 2);
		std::string consoleOutput;
		for (const std::string_view chunk : chunks) {
			CHECK(chunk.size() <= 12);
			consoleOutput.append(chunk);
			consoleOutput.push_back('\n');
		}
		const nlohmann::json parsed = nlohmann::json::parse(consoleOutput);
		CHECK(parsed["a"] == "1,2,3");
		CHECK(parsed["b"] == 4);
	}

	// A single token longer than the limit cannot be cut safely: SplitJsonForConsole refuses and
	// leaves no chunks, so PrintJsonChunks/PrintDocumentToServer answer with the error object.
	{
		std::string tooLong = "{\"error\":\"";
		tooLong.append(kLimit + 200, 'x');
		tooLong += "\"}";
		std::vector<std::string_view> chunks;
		CHECK(!playersinfo::SplitJsonForConsole(tooLong, kLimit, chunks));
		CHECK(chunks.empty());

		std::vector<std::string> printed;
		const bool printedDocument = playersinfo::PrintJsonChunks(
				tooLong, kLimit, [&printed](std::string_view chunk) { printed.emplace_back(chunk); });
		CHECK(!printedDocument);
		CHECK(printed.size() == 1);
		CHECK(printed[0] == playersinfo::kDocumentTooLongJson);
		CHECK(nlohmann::json::parse(printed[0])["error"] == "document too long");

		ResetPrinted();
		CHECK(playersinfo::PrintDocumentToServer(tooLong) ==
			  playersinfo::ConsolePrintResult::PrintedTooLongError);
		CHECK(g_printed.size() == 1);
		CHECK(g_printed[0] == playersinfo::kDocumentTooLongJson);
	}

	// The pure helper (no s2sdk at all) prints a small document unchanged.
	{
		std::vector<std::string> pieces;
		CHECK(playersinfo::PrintJsonChunks("{\"a\":1}", kLimit,
										   [&pieces](std::string_view chunk) {
											   pieces.emplace_back(chunk);
										   }));
		CHECK(pieces.size() == 1);
		CHECK(pieces[0] == "{\"a\":1}");
	}

	// Through the fake PrintToServer binding: the plugin's own path, several messages, each within
	// the limit, and the concatenation parses.
	{
		const std::string document = BuildSyntheticDocument(18);
		ResetPrinted();
		CHECK(playersinfo::PrintDocumentToServer(document) ==
			  playersinfo::ConsolePrintResult::PrintedDocument);
		CHECK(g_printed.size() >= 2);
		bool everyChunkFits = true;
		for (const std::string& chunk : g_printed) {
			everyChunkFits = everyChunkFits && chunk.size() <= kLimit;
		}
		CHECK(everyChunkFits);
		const nlohmann::json parsed = nlohmann::json::parse(JoinAsConsoleLines(g_printed));
		CHECK(parsed["players"].size() == 18);
	}

	// ... and with the s2sdk binding missing nothing is printed and nothing crashes.
	{
		const s2sdk::_PrintToServer saved = __s2sdk_PrintToServer;
		__s2sdk_PrintToServer = nullptr;
		ResetPrinted();
		CHECK(playersinfo::PrintDocumentToServer(BuildSyntheticDocument(18)) ==
			  playersinfo::ConsolePrintResult::NoBinding);
		CHECK(g_printed.empty());
		__s2sdk_PrintToServer = saved;
		// The binding is back: printing works again.
		ResetPrinted();
		CHECK(playersinfo::PrintDocumentToServer("{\"a\":1}") ==
			  playersinfo::ConsolePrintResult::PrintedDocument);
		CHECK(g_printed.size() == 1);
	}
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
	TestMapGate();
	TestConnectionTimes();
	TestCommandLifecycle();
	TestJsonChunking();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
