// playersinfo — Plugify C++ port of the Metamod:Source plugin "PlayersInfo"
// (strings, symbols and disassembly of the original binary: see BEHAVIOR.md).
//
// The original registers the server console command `mm_getinfo` (a ConCommand with
// FCVAR_GAMEDLL) which prints one compact JSON document describing the server right now; the
// example.com web module polls it over RCON. The document is exactly:
//
//   {"current_map":"<map>","players":[{"death":N,"headshots":N,"kills":N,"name":"...",
//    "ping":N,"playtime":N,"prime":true|false,"rank":N,"steamid":"<steamid64>","team":N,
//    "userid":N}, ...],"score_ct":N,"score_t":N,"time":<unix>}
//
// (key order is nlohmann's std::map order; the original never adds "rank" when the levels_ranks
// core is not loaded, reproduced here through the optional levels_ranks dependency).
//
// Values and their sources, all from the original:
//   time        time(nullptr)
//   current_map gpGlobals->mapname
//   score_ct/t  CTeam::m_iScore of the CT / T team manager (the original's cs_team_manager walk;
//               s2sdk tracks the same managers and exposes the score as GetGameTeamScore(CSTeam))
//   players[]   for every slot 0..63 whose connection timestamp is set (the original skipped
//               slots whose g_iConnection entry is 0) and whose controller has a valid
//               m_hPlayerPawn, in the original's key order
//   userid      the slot index (0-based) — the original stored the loop counter under that key
//   name        CBasePlayerController::m_iszPlayerName, "Unknown" when empty
//   team        CBaseEntity::m_iTeamNum of the controller
//   steamid     IPlayersApi::GetSteamID64(slot) printed with "%lld"
//   kills/death/headshots  CCSPlayerController::m_pActionTrackingServices ->
//               ..._ActionTrackingServices::m_matchStats -> CSPerRoundStats_t::m_iKills /
//               m_iDeaths / m_iHeadShotKills
//   ping        CCSPlayerController::m_iPing
//   playtime    time(nullptr) - the connection timestamp set by the player_connect event (0 by
//               player_disconnect)
//   prime       ISteamGameServer::UserHasLicenseForApp(steamid, 624820 /*CS2*/) or
//               (steamid, 54029 /*CS:GO prime*/) == k_EUserHasLicenseResultHasLicense
//   rank        ILRApi::GetClientInfo(slot, ST_RANK = 1, false)
//
// The plugin calls s2sdk directly - no pisexcompat, no guard library (PLUGIN_STANDARD §7). The
// safety rules of handoff/weaponskins/PITFALLS.md therefore live right here:
//   * B1/B2 map gate: OnMapStart/OnMapEnd listeners registered in OnPluginStart, unregistered in
//     OnPluginEnd; readiness (g_mapGate / g_mapReady) is latched by OnMapStart and by the first
//     OnPluginUpdate of a plugin loaded into a running map (when the listeners are registered and
//     no OnMapEnd was seen) and dropped by OnMapEnd. Every client, entity and schema call checks
//     it (playersinfo::MapGate, src/map_gate.hpp).
//   * C2 client checks: slot 0..63, IsClientConnected && IsClientInGame && !IsClientSourceTV, and a
//     non-null PlayerSlotToEntPointer; ControllerHandle() adds IsValidEntHandle on the handle and
//     IsValidEntPointer on the slot's entity pointer before any entity read.
//   * C8 schema lookups: SchemaOffset() answers -1 without a ready map (GetSchemaOffset segfaults
//     before the first map), and the offset groups cache only a complete resolution, so a lookup
//     made too early is retried instead of poisoning the session.
//   * C12/C13 events: "player_connect"/"player_disconnect" are hooked with HookEvent(Pre) in
//     OnPluginStart and unhooked in OnPluginEnd; the callbacks read the event fields only (no
//     client, entity or schema call), so they are safe during a changelevel.
//   * C11 command: s2sdk dispatches the chat spellings "!mm_getinfo"/"/mm_getinfo" to any console
//     command, so the callback refuses every caller that is not the server console (-1). The
//     registration remembers whether AddConsoleCommand created the command or only hooked an
//     existing one, and OnPluginEnd removes exactly that much (RemoveCommand for a command of ours,
//     RemoveCommandListener for one this plugin only hooked, nothing when the binding was missing)
//     - see src/command_lifecycle.hpp.
//   * A1/A7: OnPluginStart never returns Failed (that would stop every Plugify plugin) and
//     OnPluginEnd removes every command, event hook and map listener again, so `plg reload` leaves
//     nothing behind that points into the unloaded library.
//
// Porting notes:
//   * the original resolved its steam functions through the process (SteamInternal_ContextInit &
//     friends) — steamlink does the same with dlsym; without them `prime` is false.
//   * the original used the raw "userid" of the player_connect/player_disconnect events as the
//     g_iConnection index while reading the array by slot; in CS2 the event userid is not the
//     slot (see plugins/as_admin_time), so this port translates it with the event's
//     GetPlayerSlot() and keeps both sides indexed by slot.
//   * the original returned the *name* of a player as "Unknown" when empty; kept.
//   * the config file addons/configs/playerinfo_settings.ini is NOT read by the original binary
//     (no config strings exist in PlayersInfo.so, see BEHAVIOR.md) — neither does this port read it.

#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <levels_ranks/levels_ranks.hpp>

#include "command_lifecycle.hpp"
#include "connection_times.hpp"
#include "console_output.hpp"
#include "ent_handle.hpp"
#include "json_writer.hpp"
#include "map_gate.hpp"
#include "steam_gameserver.hpp"

namespace {

// The original loops over 0..63 (compared against 0x40 in GetServerInfo).
using playersinfo::kMaxSlots;
// The server console command of the original (`mm_getinfo`, FCVAR_GAMEDLL) and the hook mode its
// callback is attached/removed with (src/command_lifecycle.hpp needs the same value for both).
constexpr const char* kCommandName = "mm_getinfo";
constexpr s2sdk::HookMode kCommandMode = s2sdk::HookMode::Post;
// LR_StatsType::ST_RANK, as exported by levels_ranks.
constexpr std::int32_t kLrStatsRank = 1;
// ISteamGameServer::UserHasLicenseForApp app ids of the original's CheckPrime: 624820 (0x988b4,
// CS2) then 54029 (0xd30d, the CS:GO prime upgrade).
constexpr std::uint32_t kCs2AppId = 624820;
constexpr std::uint32_t kCsgoPrimeAppId = 54029;
// steam_api may be mapped into the process after this plugin starts: a failed symbol resolution is
// retried at this pace (the resolution is cheap and logs nothing while it keeps failing).
constexpr std::chrono::seconds kSteamInitRetryInterval{1};

// The original's g_iConnection[64]: connect time per slot (0 = not connected).
playersinfo::ConnectionTimes g_connectionTimes;
// PITFALLS B1/B2: the map gate - `g_mapGate.Ready()` is this plugin's g_mapReady. Nothing below a
// client, entity or schema call may run while it is false, and only OnMapStart, the first
// OnPluginUpdate (mid-map load) and OnPluginEnd change it.
playersinfo::MapGate g_mapGate;

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

// --- map gate (PITFALLS B1/B2) -----------------------------------------------------------------

// s2sdk listeners are plain function pointers called on the game thread; they must not throw, and
// OnMapEnd must not touch a client or an entity (PITFALLS B2 - the client list is already emptied).
void OnMapStartListener() {
	g_mapGate.OnMapStart();
}

void OnMapEndListener() {
	g_mapGate.OnMapEnd();
}

// Detaches both listeners; safe to call when only one of them was ever registered (s2sdk answers
// "not registered" for the other one).
void DropMapListeners() {
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

void RegisterMapListeners() {
	if (g_mapGate.ListenersRegistered()) {
		return;
	}
	// Registering a listener touches no client, so it is safe in OnPluginStart (PITFALLS B1).
	// Without the listeners nothing could ever close the gate again, so a missing binding keeps it
	// shut instead of opening it blindly.
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		Log("playersinfo: the s2sdk map listeners are unavailable - every client call stays refused",
			plg::Severity::Error);
		return;
	}
	try {
		// OnMapEnd first: if the second registration fails, a half-registered pair can then only
		// close the gate (never leave it open through a map change, PITFALLS B2).
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		g_mapGate.SetListenersRegistered(true);
	} catch (const std::exception& ex) {
		// A callback of this library must not survive a failed start.
		DropMapListeners();
		Log(std::string("playersinfo: registering the map listeners failed: ") + ex.what(),
			plg::Severity::Error);
	} catch (...) {
		DropMapListeners();
		Log("playersinfo: registering the map listeners failed with an unknown exception",
			plg::Severity::Error);
	}
}

void UnregisterMapListeners() {
	if (!g_mapGate.ListenersRegistered()) {
		return;
	}
	g_mapGate.SetListenersRegistered(false);
	DropMapListeners();
}

// --- client and controller checks (PITFALLS C2) ------------------------------------------------

// A map is up and the slot is in range: from here on a client query is safe.
bool CanQueryClient(std::int32_t slot) {
	return g_mapGate.Ready() && slot >= 0 && slot < kMaxSlots;
}

// A live, human-addressable slot: connected, in game and not the SourceTV proxy.
bool HasClient(std::int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr ||
		__s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return false;
	}
}

// HasClient plus a controller the slot actually resolves to.
bool HasController(std::int32_t slot) {
	if (!HasClient(slot) || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return false;
	}
	try {
		return s2sdk::PlayerSlotToEntPointer(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

// The slot's controller pointer (CCSPlayerController::FromSlot), validated; nullptr when the slot
// has no usable controller. Used for the raw reads of the match-stats chain, in the same tick as
// the reads themselves (PITFALLS C3 - never keep it across ticks).
void* ControllerPointer(std::int32_t slot) {
	if (!HasController(slot) || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr) {
		return nullptr;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return nullptr;
		}
		return controller;
	} catch (...) {
		return nullptr;
	}
}

// The controller's entity handle, or -1 when the slot has no usable controller: every caller can
// treat a negative value as "nothing to do". The handle and the slot's entity pointer are two
// views of the same controller and both have to be valid - the entity exports take the handle,
// while PlayerSlotToEntPointer is what proves the slot still resolves to that entity (between the
// two reads the engine can already have reclaimed it).
std::int32_t ControllerHandle(std::int32_t slot) {
	if (!HasController(slot) || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_IsValidEntHandle == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr) {
		return -1;
	}
	try {
		const std::int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!playersinfo::IsValidHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
			return -1;
		}
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return -1;
		}
		return handle;
	} catch (...) {
		return -1;
	}
}

// The connect timestamp a plugin loaded into a running map never saw: a connected, in-game client
// that is not a bot (bots have no Steam identity and only reach the table through player_connect,
// like they did in the original).
bool IsSeedableClient(std::int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr ||
		__s2sdk_IsClientInGame == nullptr || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsFakeClient(slot);
	} catch (...) {
		return false;
	}
}

// IPlayersApi::GetSteamID64 of the original. A bot has no Steam identity, so it is never asked
// (PITFALLS B5: a bot's network data does not exist) - it reports 0, exactly like the original's
// empty controller SteamID, and "prime" is false for it.
std::uint64_t ClientSteamID64(std::int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_GetClientSteamID64 == nullptr ||
		__s2sdk_IsFakeClient == nullptr) {
		return 0;
	}
	try {
		if (s2sdk::IsFakeClient(slot)) {
			return 0;
		}
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// CBasePlayerController::m_iszPlayerName. "<unknown>" is s2sdk's placeholder for a name it could
// not resolve and counts as empty (PITFALLS C2), exactly like the original's empty string.
std::string ClientName(std::int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(slot);
		std::string out(name.data(), name.size());
		if (out == "<unknown>") {
			out.clear();
		}
		return out;
	} catch (...) {
		return {};
	}
}

// --- schema offsets (PITFALLS C5/C6/C8) --------------------------------------------------------

// -1 without a ready map (GetSchemaOffset walks the serializer map and segfaults before the first
// map: PITFALLS C8) and -1 for a field this game build does not have; the caller retries instead of
// caching the failure.
std::int32_t SchemaOffset(const char* className, const char* fieldName) {
	if (!g_mapGate.Ready() || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(className, fieldName);
	} catch (...) {
		return -1;
	}
}

struct PlayerStatsOffsets {
	std::int32_t actionTracking = -1; // CCSPlayerController::m_pActionTrackingServices (8-byte pointer)
	std::int32_t matchStats = -1;    // ..._ActionTrackingServices::m_matchStats (embedded struct)
	std::int32_t kills = -1;         // CSPerRoundStats_t::m_iKills
	std::int32_t deaths = -1;        // CSPerRoundStats_t::m_iDeaths
	std::int32_t headshots = -1;     // CSPerRoundStats_t::m_iHeadShotKills
};

const PlayerStatsOffsets& StatsOffsets() {
	// Resolved once, but only a *complete* resolution is kept: an mm_getinfo that arrives before
	// the schema system is ready would otherwise cache -1 for the whole session and report every
	// counter as 0 forever. While the offsets are missing (or a field was renamed) ReadMatchStat()
	// answers 0, so the document stays well-formed.
	static PlayerStatsOffsets offsets;
	static bool resolved = false;
	if (!resolved) {
		PlayerStatsOffsets candidate;
		candidate.actionTracking = SchemaOffset("CCSPlayerController", "m_pActionTrackingServices");
		candidate.matchStats =
			SchemaOffset("CCSPlayerController_ActionTrackingServices", "m_matchStats");
		candidate.kills = SchemaOffset("CSPerRoundStats_t", "m_iKills");
		candidate.deaths = SchemaOffset("CSPerRoundStats_t", "m_iDeaths");
		candidate.headshots = SchemaOffset("CSPerRoundStats_t", "m_iHeadShotKills");
		if (candidate.actionTracking >= 0 && candidate.matchStats >= 0 && candidate.kills >= 0 &&
			candidate.deaths >= 0 && candidate.headshots >= 0) {
			offsets = candidate;
			resolved = true;
		}
	}
	return offsets;
}

// The controller fields are read as offsets, not through the typed s2sdk schema accessors: the
// typed accessors dereference the cached CSchemaType of the class, which can be the invalid -1
// sentinel when that entry was built around a map load (the "schema type poison" of PITFALLS C6).
// mm_getinfo is polled by the web module at arbitrary times - including right after a map load -
// so the fields this plugin reads are resolved as offsets and read raw, with the widths the
// schema lists.
struct ControllerFieldOffsets {
	std::int32_t team = -1;  // CBaseEntity::m_iTeamNum, 1 byte
	std::int32_t ping = -1;  // CCSPlayerController::m_iPing, uint32
	std::int32_t pawn = -1;  // CCSPlayerController::m_hPlayerPawn, a 4-byte entity handle
};

const ControllerFieldOffsets& ControllerOffsets() {
	static ControllerFieldOffsets offsets;
	static bool resolved = false;
	if (!resolved) {
		ControllerFieldOffsets candidate;
		candidate.team = SchemaOffset("CBaseEntity", "m_iTeamNum");
		candidate.ping = SchemaOffset("CCSPlayerController", "m_iPing");
		candidate.pawn = SchemaOffset("CCSPlayerController", "m_hPlayerPawn");
		if (candidate.team >= 0 && candidate.ping >= 0 && candidate.pawn >= 0) {
			offsets = candidate;
			resolved = true;
		}
	}
	return offsets;
}

// --- offset-based controller reads -------------------------------------------------------------

int ControllerInt(std::int32_t controllerHandle, std::int32_t offset, int size) {
	if (!g_mapGate.Ready() || !playersinfo::IsValidHandle(controllerHandle) || offset < 0 ||
		__s2sdk_GetEntData == nullptr) {
		return 0;
	}
	try {
		return static_cast<int>(s2sdk::GetEntData(controllerHandle, offset, size));
	} catch (...) {
		return 0;
	}
}

// The original required a valid pawn handle on the controller (an empty handle is -1). Both the
// int-level rule (playersinfo::IsValidHandle: not 0, not -1) and s2sdk's IsValidEntHandle have to
// agree - the second one also proves the entity system still knows index + serial.
bool ControllerHasPawn(std::int32_t controllerHandle, std::int32_t pawnOffset) {
	if (!g_mapGate.Ready() || !playersinfo::IsValidHandle(controllerHandle) || pawnOffset < 0 ||
		__s2sdk_GetEntDataEnt == nullptr || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		const std::int32_t pawn = s2sdk::GetEntDataEnt(controllerHandle, pawnOffset);
		return playersinfo::IsValidHandle(pawn) && s2sdk::IsValidEntHandle(pawn);
	} catch (...) {
		return false;
	}
}

// Reads the counter chain the original reads: controller -> m_pActionTrackingServices (a real
// pointer, kind "ptr") -> the counter. m_matchStats is NOT a second pointer on the current binary:
// the schema has it as an embedded CSMatchStats_t (kind "ref", at
// CCSPlayerController_ActionTrackingServices + 200), and CSMatchStats_t derives from
// CSPerRoundStats_t with the base at offset 0 - so a CSPerRoundStats_t field offset applies
// directly inside it. The original binary's disassembly showed two pointer hops (an older CS2
// layout); dereferencing the embedded bytes as a pointer reads m_iKills/m_iDeaths as an address
// and crashes or reports garbage. Returns 0 when a link (or its schema offset) is missing.
//
// `controller` is the pointer ControllerPointer() validated in this tick; it is never cached
// (PITFALLS C3).
int ReadMatchStat(void* controller, std::int32_t offset) {
	if (!g_mapGate.Ready() || controller == nullptr || offset < 0) {
		return 0;
	}
	const PlayerStatsOffsets& offsets = StatsOffsets();
	if (offsets.actionTracking < 0 || offsets.matchStats < 0) {
		return 0;
	}
	void* services = *reinterpret_cast<void**>(static_cast<char*>(controller) + offsets.actionTracking);
	if (services == nullptr) {
		return 0;
	}
	return static_cast<int>(
		*reinterpret_cast<std::int32_t*>(static_cast<char*>(services) + offsets.matchStats + offset));
}

// --- prime / teams / map -----------------------------------------------------------------------

bool CheckPrime(std::uint64_t steamId) {
	const int cs2 = steamlink::UserHasLicenseForApp(steamId, kCs2AppId);
	if (cs2 == 0) {
		return true;
	}
	return steamlink::UserHasLicenseForApp(steamId, kCsgoPrimeAppId) == 0;
}

// The original's GetTeamScore(): walk the cs_team_manager entities and read m_iScore by team.
//
// s2sdk tracks the same managers (g_TeamManagers, filled from the entity spawn/delete events,
// src/core/plugin.cpp) and exposes each team's score directly, so the walk, its iteration cap and
// its "search did not advance" hang class are gone: two lookups instead of an entity scan.
int TeamScore(s2sdk::CSTeam team) {
	if (!g_mapGate.Ready() || __s2sdk_GetGameTeamScore == nullptr) {
		return 0;
	}
	int score = 0;
	try {
		score = s2sdk::GetGameTeamScore(team);
	} catch (...) {
		score = 0;
	}
	// -1 = the team manager does not exist (before a map, during shutdown); the original's walk
	// left the score 0 in that case.
	return score < 0 ? 0 : score;
}

void GetTeamScore(int& ctScore, int& tScore) {
	ctScore = TeamScore(s2sdk::CSTeam::CT); // CS_TEAM_CT
	tScore = TeamScore(s2sdk::CSTeam::T);   // CS_TEAM_T
}

// Whether a map name exists right now. This is the one s2sdk map call that is safe before the
// first map - GetCurrentMap() null-checks gpGlobals itself and answers an empty string then,
// which is exactly the proof OnPluginUpdate() needs before it may open the gate (PITFALLS B1/B3).
bool HasMapName() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return false;
	}
	try {
		return !s2sdk::GetCurrentMap().empty();
	} catch (...) {
		return false;
	}
}

// gpGlobals->mapname of the original (s2sdk's GetCurrentMap reads the same field). Only asked
// while the gate is open - the empty string is the "no map" answer of the command.
std::string CurrentMap() {
	if (!g_mapGate.Ready() || !HasMapName()) {
		return {};
	}
	try {
		const plg::string map = s2sdk::GetCurrentMap();
		return std::string(map.data(), map.size());
	} catch (...) {
		return {};
	}
}

// The original's GetServerInfo().
std::string BuildServerInfo() {
	std::map<std::string, playersinfo::Json> root;

	root["time"] = playersinfo::Json::Number(static_cast<std::int64_t>(std::time(nullptr)));
	root["current_map"] = playersinfo::Json::String(CurrentMap());

	int ctScore = 0;
	int tScore = 0;
	GetTeamScore(ctScore, tScore);
	root["score_ct"] = playersinfo::Json::Number(ctScore);
	root["score_t"] = playersinfo::Json::Number(tScore);

	const auto now = static_cast<std::int64_t>(std::time(nullptr));
	const PlayerStatsOffsets& statsOffsets = StatsOffsets();
	const ControllerFieldOffsets& controllerOffsets = ControllerOffsets();
	const bool haveRanks = __levels_ranks_GetClientInfo != nullptr;

	std::vector<playersinfo::Json> players;
	for (std::int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!g_connectionTimes.IsPresent(slot)) {
			continue; // the original's "g_iConnection[slot] == 0 -> skip"
		}
		// PITFALLS C2: nothing below this line runs for a slot without a live, resolvable
		// controller - both views of it are validated by the two helpers.
		const std::int32_t controller = ControllerHandle(slot);
		if (!playersinfo::IsValidHandle(controller)) {
			continue;
		}
		void* controllerPtr = ControllerPointer(slot);
		if (controllerPtr == nullptr) {
			continue;
		}
		if (!ControllerHasPawn(controller, controllerOffsets.pawn)) {
			continue; // the original required a valid pawn handle
		}

		std::map<std::string, playersinfo::Json> player;
		player["userid"] = playersinfo::Json::Number(slot);

		std::string name = ClientName(slot);
		if (name.empty()) {
			name = "Unknown";
		}
		player["name"] = playersinfo::Json::String(name);

		player["team"] = playersinfo::Json::Number(ControllerInt(controller, controllerOffsets.team, 1));

		const std::uint64_t steamId = ClientSteamID64(slot);
		player["steamid"] = playersinfo::Json::String(std::to_string(steamId));

		player["kills"] = playersinfo::Json::Number(ReadMatchStat(controllerPtr, statsOffsets.kills));
		player["death"] = playersinfo::Json::Number(ReadMatchStat(controllerPtr, statsOffsets.deaths));
		player["headshots"] =
			playersinfo::Json::Number(ReadMatchStat(controllerPtr, statsOffsets.headshots));
		player["ping"] = playersinfo::Json::Number(
			ControllerInt(controller, controllerOffsets.ping, static_cast<int>(sizeof(std::uint32_t))));

		const std::int64_t connectedAt = g_connectionTimes.ConnectedAt(slot);
		player["playtime"] = playersinfo::Json::Number(now - connectedAt);

		player["prime"] = playersinfo::Json::Bool(steamId != 0 && CheckPrime(steamId));

		if (haveRanks) {
			try {
				player["rank"] =
					playersinfo::Json::Number(levels_ranks::GetClientInfo(slot, kLrStatsRank, false));
			} catch (...) {
				// The levels_ranks plugin was unloaded between the two calls: drop the key, like
				// the original does when its LR core pointer is null.
			}
		}

		players.push_back(playersinfo::Json::Object(player));
	}
	root["players"] = playersinfo::Json::Array(std::move(players));

	return playersinfo::Json::Object(root).Text();
}

// --- events (PITFALLS C12/C13) -----------------------------------------------------------------

// The callbacks only write the connection table: no client, entity or schema call, so they stay
// safe while a changelevel is in flight (PITFALLS C13 - events keep arriving there).
s2sdk::ResultType OnPlayerConnectEvent(const plg::string& /*name*/, void* event,
									   bool /*dontBroadcast*/) {
	if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	try {
		const std::int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
		// "bot" only exists on player_connect. The original stored a timestamp for every
		// connect, bots included, so the flag is reported rather than used as a filter.
		const bool bot = __s2sdk_GetEventBool != nullptr && s2sdk::GetEventBool(event, plg::string("bot"));
		const auto now = static_cast<std::int32_t>(std::time(nullptr));
		if (g_connectionTimes.Connect(slot, now)) {
			Log("playersinfo: slot " + std::to_string(slot) + (bot ? " (bot)" : "") + " connected",
				plg::Severity::Debug);
		}
	} catch (...) {
		// An event callback may not throw into s2sdk.
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& /*name*/, void* event,
										  bool /*dontBroadcast*/) {
	if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	try {
		const std::int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
		// The playtime of a slot that disconnects is 0 from now on until the next
		// player_connect, which is what the original's OnPlayerDisconnect did.
		g_connectionTimes.Disconnect(slot);
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

void HookPlayerEvents() {
	if (__s2sdk_HookEvent == nullptr) {
		Log("playersinfo: s2sdk HookEvent is unavailable - every playtime stays 0", plg::Severity::Error);
		return;
	}
	try {
		if (s2sdk::HookEvent(plg::string("player_connect"), &OnPlayerConnectEvent,
							 s2sdk::HookMode::Pre) != s2sdk::EventHookError::Okay) {
			Log("playersinfo: could not hook \"player_connect\"", plg::Severity::Error);
		}
		if (s2sdk::HookEvent(plg::string("player_disconnect"), &OnPlayerDisconnectEvent,
							 s2sdk::HookMode::Pre) != s2sdk::EventHookError::Okay) {
			Log("playersinfo: could not hook \"player_disconnect\"", plg::Severity::Error);
		}
	} catch (const std::exception& ex) {
		Log(std::string("playersinfo: hooking the player events failed: ") + ex.what(),
			plg::Severity::Error);
	} catch (...) {
		Log("playersinfo: hooking the player events failed with an unknown exception",
			plg::Severity::Error);
	}
}

void UnhookPlayerEvents() {
	if (__s2sdk_UnhookEvent == nullptr) {
		return;
	}
	try {
		s2sdk::UnhookEvent(plg::string("player_connect"), &OnPlayerConnectEvent, s2sdk::HookMode::Pre);
		s2sdk::UnhookEvent(plg::string("player_disconnect"), &OnPlayerDisconnectEvent,
						   s2sdk::HookMode::Pre);
	} catch (...) {
	}
}

// --- mm_getinfo --------------------------------------------------------------------------------

// One mm_getinfo document on the server console: the original's `GetServerInfo().dump(...)` through
// g_SMAPI->ConPrintf, here through s2sdk::PrintToServer (the same tier0 channel). That call is
// ConMsg("%s\n", ...), which truncates at 2047 bytes (s2sdk src/export/console.cpp:12-13), so the
// document is printed in chunks cut between JSON tokens (src/console_output.hpp): the newlines
// s2sdk adds are legal JSON whitespace and an RCON client that concatenates the lines still parses
// one object. A document with an indivisible token is replaced by a short error object.
void PrintServerInfo() {
	if (!g_mapGate.Ready()) {
		// Before the first map and between OnMapEnd and the next OnMapStart every value would come
		// from a torn-down engine state, so answer the one thing that is true then.
		playersinfo::PrintDocumentToServer("{\"error\":\"no map\"}");
		return;
	}
	const std::string info = BuildServerInfo();
	if (playersinfo::PrintDocumentToServer(info) ==
		playersinfo::ConsolePrintResult::PrintedTooLongError) {
		Log("playersinfo: the mm_getinfo document has a JSON token longer than " +
				std::to_string(playersinfo::kMaxConsoleChunkBytes) +
				" bytes - answered with an error object instead of a broken document",
			plg::Severity::Error);
	}
}

s2sdk::ResultType OnMmGetInfo(std::int32_t slot, s2sdk::ConCommandContext /*context*/,
							  const plg::vector<plg::string>& /*args*/) {
	try {
		// PITFALLS C11: any player can reach a console command from chat - s2sdk dispatches
		// "!mm_getinfo"/"/mm_getinfo" to this callback with the player's slot, and the original's
		// callback answered exactly `slot == -1` (the server console/RCON) only. Every other
		// caller, a player slot as well as any other negative value, gets "not handled", so the
		// chat line and the player's own console output stay untouched.
		if (!playersinfo::RunForServerConsoleOnly(slot, &PrintServerInfo)) {
			return s2sdk::ResultType::Continue;
		}
	} catch (const std::exception& ex) {
		Log(std::string("playersinfo: mm_getinfo failed: ") + ex.what(), plg::Severity::Error);
	} catch (...) {
		Log("playersinfo: mm_getinfo failed with an unknown exception", plg::Severity::Error);
	}
	return s2sdk::ResultType::Handled;
}

class PlayersInfoPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// A fresh start (including `plg reload`) begins with the gate shut and an empty
			// connection table: nothing below queries a client, an entity or the schema system
			// (PITFALLS B1).
			g_mapGate.Reset();
			g_connectionTimes.Reset();
			m_connectionTimesSeeded = false;
			m_commandOwnership = playersinfo::CommandOwnership::NotAttempted;
			m_steamRetryAccumulated = std::chrono::milliseconds::zero();

			RegisterMapListeners();
			HookPlayerEvents();

			std::string error;
			if (!steamlink::Init(error)) {
				Log("playersinfo: Steam game server API unavailable (" + error +
						") - 'prime' will always be false",
					plg::Severity::Warning);
			}

			// PITFALLS C11: remember what the registration actually owns. s2sdk's AddConsoleCommand
			// attaches the callback either way and only says whether the engine command itself was
			// created; OnPluginEnd must remove a command it created but only detach the callback
			// from one that already existed (see src/command_lifecycle.hpp).
			m_commandOwnership = playersinfo::RegisterCommand(
					kCommandName, "", s2sdk::ConVarFlag::GameDll, &OnMmGetInfo, kCommandMode);
			if (m_commandOwnership == playersinfo::CommandOwnership::NotAttempted) {
				Log("playersinfo: s2sdk AddConsoleCommand is unavailable - 'mm_getinfo' is not registered",
					plg::Severity::Error);
			} else if (m_commandOwnership == playersinfo::CommandOwnership::Existing) {
				// The name was already taken (another plugin, the engine, or our own entry kept by
				// s2sdk across a `plg reload`): the callback is attached to that entry and
				// OnPluginEnd detaches only the callback, never the command.
				Log("playersinfo: 'mm_getinfo' already exists; attached only a listener to the existing entry",
					plg::Severity::Info);
			}

			Log("playersinfo: ready (mm_getinfo registered" +
					std::string(__levels_ranks_GetClientInfo != nullptr ? ", levels_ranks found" : "") + ")",
				plg::Severity::Info);
		} catch (const std::exception& ex) {
			Log(std::string("playersinfo: OnPluginStart failed: ") + ex.what(), plg::Severity::Error);
		} catch (...) {
			Log("playersinfo: OnPluginStart failed with an unknown exception", plg::Severity::Error);
		}
		return {}; // never Failed: that would stop every Plugify plugin (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			// PITFALLS B1: a plugin loaded (or reloaded) into a running map never saw OnMapStart;
			// its first update is the "a map is up" signal (playersinfo::MapGate). HasMapName()
			// keeps the latch shut on a server that has no map yet - the schema lookups below
			// would walk a null serializer map there (PITFALLS C8). Only probed while the gate is
			// shut (before the first map and around a map change), never once a map is live.
			if (!g_mapGate.Ready()) {
				g_mapGate.OnPluginUpdate(HasMapName());
			}

			if (!steamlink::IsAvailable()) {
				// OnPluginStart may have run before steam_api was mapped: retry the resolution,
				// otherwise 'prime' stays false for the whole boot. Init() only remembers a fully
				// successful resolution.
				m_steamRetryAccumulated += dt;
				if (m_steamRetryAccumulated >= kSteamInitRetryInterval) {
					m_steamRetryAccumulated = std::chrono::milliseconds::zero();
					std::string error;
					if (steamlink::Init(error)) {
						Log("playersinfo: Steam game server API resolved after startup - 'prime' is live",
							plg::Severity::Info);
					}
				}
			}

			// A plugin that is loaded (or reloaded) into a running server never sees
			// player_connect for the players already there: seed their timestamps once, or
			// mm_getinfo keeps returning an empty "players" list until every one reconnects.
			if (!m_connectionTimesSeeded && g_mapGate.Ready()) {
				m_connectionTimesSeeded = true;
				const auto now = static_cast<std::int32_t>(std::time(nullptr));
				const int seeded = g_connectionTimes.Seed(
					now, [](std::int32_t slot) { return IsSeedableClient(slot); });
				if (seeded > 0) {
					Log("playersinfo: seeded " + std::to_string(seeded) +
							" connection timestamps for the players already in the game",
						plg::Severity::Info);
				}
			}
		} catch (const std::exception& ex) {
			Log(std::string("playersinfo: update failed: ") + ex.what(), plg::Severity::Error);
		} catch (...) {
			Log("playersinfo: update failed with an unknown exception", plg::Severity::Error);
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// PITFALLS A7: nothing of this plugin may stay registered in the engine - a stale
			// callback would point into the unloaded library.
			UnhookPlayerEvents();
			// PITFALLS C11: only what RegisterCommand created is removed again. RemoveCommand on a
			// command this plugin merely hooked would delete the other owner's registration, and a
			// command that was never attempted must not be touched at all.
			const playersinfo::CommandTeardown teardown = playersinfo::UnregisterCommand(
					kCommandName, &OnMmGetInfo, kCommandMode, m_commandOwnership);
			m_commandOwnership = playersinfo::CommandOwnership::NotAttempted;
			if (teardown != playersinfo::CommandTeardown::None) {
				Log(teardown == playersinfo::CommandTeardown::RemovedCommand
							? "playersinfo: removed the 'mm_getinfo' console command"
							: "playersinfo: detached the 'mm_getinfo' listener",
					plg::Severity::Debug);
			}
			UnregisterMapListeners();
			g_mapGate.Close();
			g_connectionTimes.Reset();
			m_connectionTimesSeeded = false;
			// The steam_api symbols belong to the engine's library; this only forgets the resolved
			// pointers, so a stale lookup cannot outlive the library (nothing to unload).
			steamlink::Shutdown();
		} catch (const std::exception& ex) {
			Log(std::string("playersinfo: OnPluginEnd failed: ") + ex.what(), plg::Severity::Error);
		} catch (...) {
			Log("playersinfo: OnPluginEnd failed with an unknown exception", plg::Severity::Error);
		}
		return {};
	}

private:
	// What the mm_getinfo registration owns (PITFALLS C11); OnPluginEnd decides the teardown on it.
	playersinfo::CommandOwnership m_commandOwnership = playersinfo::CommandOwnership::NotAttempted;
	bool m_connectionTimesSeeded = false;
	std::chrono::milliseconds m_steamRetryAccumulated{0};
};

PlayersInfoPlugin g_playersInfoPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_playersInfoPlugin)
