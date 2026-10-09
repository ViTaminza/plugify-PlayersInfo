// playersinfo — Plugify C++ port of the production Metamod:Source plugin "PlayersInfo"
// (ref/src/binaries/PlayersInfo/PlayersInfo.so; strings, symbols and disassembly: see BEHAVIOR.md).
//
// The original registers the server console command `mm_getinfo` (a ConCommand with
// FCVAR_GAMEDLL) which prints one compact JSON document describing the server right now; the
// vcs2.ru web module polls it over RCON. The document is exactly:
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
// Porting notes:
//   * the original resolved its steam functions through the process (SteamInternal_ContextInit &
//     friends) — steamlink does the same with dlsym; without them `prime` is false.
//   * the original used the raw "userid" of the player_connect/player_disconnect events as the
//     g_iConnection index while reading the array by slot; in CS2 the event userid is not the
//     slot (see plugins/as_admin_time), so this port translates it with the event's
//     GetPlayerSlot() and keeps both sides indexed by slot.
//   * the original returned the *name* of a player as "Unknown" when empty; kept.
//   * the config file addons/configs/playerinfo_settings.ini on the production server is NOT read
//     by this binary (no config strings exist in PlayersInfo.so, see BEHAVIOR.md) — neither does
//     this port read it.

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>
#include <s2sdk/s2sdk.hpp>

#include <levels_ranks/levels_ranks.hpp>

#include "ent_handle.hpp"
#include "json_writer.hpp"
#include "steam_gameserver.hpp"

namespace {

// The original loops over 0..63 (compared against 0x40 in GetServerInfo).
constexpr int kMaxSlots = 64;
// LR_StatsType::ST_RANK (ref/src/cs2-lvl_ranks/include/lvl_ranks.h:83).
constexpr std::int32_t kLrStatsRank = 1;
// ISteamGameServer::UserHasLicenseForApp app ids of the original's CheckPrime: 624820 (0x988b4,
// CS2) then 54029 (0xd30d, the CS:GO prime upgrade).
constexpr std::uint32_t kCs2AppId = 624820;
constexpr std::uint32_t kCsgoPrimeAppId = 54029;

// The original's g_iConnection[64]: connect time per slot (0 = not connected).
std::array<std::int32_t, kMaxSlots> g_connectionTimes{};

void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

// --- schema chain used for kills/deaths/headshots ------------------------------------------------

struct PlayerStatsOffsets {
	int actionTracking = -1; // CCSPlayerController::m_pActionTrackingServices (a raw pointer)
	int matchStats = -1;     // CCSPlayerController_ActionTrackingServices::m_matchStats (raw pointer)
	int kills = -1;          // CSPerRoundStats_t::m_iKills
	int deaths = -1;         // CSPerRoundStats_t::m_iDeaths
	int headshots = -1;      // CSPerRoundStats_t::m_iHeadShotKills
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
		candidate.actionTracking = pisex::SchemaOffset("CCSPlayerController", "m_pActionTrackingServices");
		candidate.matchStats =
			pisex::SchemaOffset("CCSPlayerController_ActionTrackingServices", "m_matchStats");
		candidate.kills = pisex::SchemaOffset("CSPerRoundStats_t", "m_iKills");
		candidate.deaths = pisex::SchemaOffset("CSPerRoundStats_t", "m_iDeaths");
		candidate.headshots = pisex::SchemaOffset("CSPerRoundStats_t", "m_iHeadShotKills");
		if (candidate.actionTracking >= 0 && candidate.matchStats >= 0 && candidate.kills >= 0 &&
			candidate.deaths >= 0 && candidate.headshots >= 0) {
			offsets = candidate;
			resolved = true;
		} else {
			return offsets; // still unresolved: every read reports 0
		}
	}
	return offsets;
}

// --- offset-based controller reads ---------------------------------------------------------------
// The typed pisex::GetEntInt/GetEntEntity helpers dereference the cached CSchemaType of the class,
// which can be the invalid -1 sentinel when that entry was built around a map load (crash report
// Signature D; see the warning in pisexcompat's schema.hpp). mm_getinfo is polled by the web
// module at arbitrary times - including right after a map load - so the three fields this plugin
// reads are resolved as offsets and read raw, with the widths the schema dump lists.
struct ControllerFieldOffsets {
	int team = -1; // CBaseEntity::m_iTeamNum, 1 byte
	int ping = -1; // CCSPlayerController::m_iPing, uint32
	int pawn = -1; // CCSPlayerController::m_hPlayerPawn, a 4-byte entity handle
};

const ControllerFieldOffsets& ControllerOffsets() {
	static ControllerFieldOffsets offsets;
	static bool resolved = false;
	if (!resolved) {
		ControllerFieldOffsets candidate;
		candidate.team = pisex::SchemaOffset("CBaseEntity", "m_iTeamNum");
		candidate.ping = pisex::SchemaOffset("CCSPlayerController", "m_iPing");
		candidate.pawn = pisex::SchemaOffset("CCSPlayerController", "m_hPlayerPawn");
		if (candidate.team >= 0 && candidate.ping >= 0 && candidate.pawn >= 0) {
			offsets = candidate;
			resolved = true;
		} else {
			return offsets; // still unresolved: the reads below report 0 / skip, like before
		}
	}
	return offsets;
}

int ControllerInt(int controllerIndex, int offset, int size) {
	const int handle = pisex::EntIndexToEntHandle(controllerIndex);
	if (!playersinfo::IsValidHandle(handle) || offset < 0 || !__s2sdk_GetEntData) {
		return 0;
	}
	try {
		return static_cast<int>(s2sdk::GetEntData(handle, offset, size));
	} catch (...) {
		return 0;
	}
}

// The original required a valid pawn handle on the controller (an empty handle is -1).
bool ControllerHasPawn(int controllerIndex, int pawnOffset) {
	const int handle = pisex::EntIndexToEntHandle(controllerIndex);
	if (!playersinfo::IsValidHandle(handle) || pawnOffset < 0 || !__s2sdk_GetEntDataEnt) {
		return false;
	}
	int32_t pawn = -1;
	try {
		pawn = s2sdk::GetEntDataEnt(handle, pawnOffset);
	} catch (...) {
		return false;
	}
	return playersinfo::IsValidHandle(pawn) && pisex::EntHandleToEntIndex(pawn) > 0;
}

// Reads the counter chain the original reads: controller -> m_pActionTrackingServices (a real
// pointer, kind "ptr") -> the counter. m_matchStats is NOT a second pointer on the current binary:
// the schema dump has it as an embedded CSMatchStats_t (kind "ref", size 184, at
// CCSPlayerController_ActionTrackingServices + 208), and CSMatchStats_t derives from
// CSPerRoundStats_t with the base at offset 0 - so a CSPerRoundStats_t field offset applies
// directly inside it. The original binary's disassembly showed two pointer hops (an older CS2
// layout); dereferencing the embedded bytes as a pointer reads m_iKills/m_iDeaths as an address
// and crashes or reports garbage. Returns 0 when a link (or its schema offset) is missing.
int ReadMatchStat(int controllerIndex, int offset) {
	if (controllerIndex == 0 || offset < 0) {
		return 0;
	}
	const PlayerStatsOffsets& offsets = StatsOffsets();
	if (offsets.actionTracking < 0 || offsets.matchStats < 0) {
		return 0;
	}
	void* controller = pisex::EntIndexToEntPointer(controllerIndex);
	if (controller == nullptr) {
		return 0;
	}
	void* services = *reinterpret_cast<void**>(static_cast<char*>(controller) + offsets.actionTracking);
	if (services == nullptr) {
		return 0;
	}
	return *reinterpret_cast<int*>(static_cast<char*>(services) + offsets.matchStats + offset);
}

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
	if (__s2sdk_GetGameTeamScore == nullptr) {
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

// The original's GetServerInfo().
std::string BuildServerInfo() {
	std::map<std::string, playersinfo::Json> root;

	root["time"] = playersinfo::Json::Number(static_cast<std::int64_t>(std::time(nullptr)));

	std::string mapName(pisex::CurrentMap());
	root["current_map"] = playersinfo::Json::String(mapName);

	int ctScore = 0;
	int tScore = 0;
	GetTeamScore(ctScore, tScore);
	root["score_ct"] = playersinfo::Json::Number(ctScore);
	root["score_t"] = playersinfo::Json::Number(tScore);

	const auto now = static_cast<std::int64_t>(std::time(nullptr));
	const PlayerStatsOffsets& offsets = StatsOffsets();
	const bool haveRanks = __levels_ranks_GetClientInfo != nullptr;

	std::vector<playersinfo::Json> players;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (g_connectionTimes[static_cast<std::size_t>(slot)] == 0) {
			continue; // the original's "g_iConnection[slot] == 0 -> skip"
		}
		const int controller = pisex::GetPlayerController(slot);
		if (controller == 0) {
			continue;
		}
		const ControllerFieldOffsets& controllerOffsets = ControllerOffsets();
		if (!ControllerHasPawn(controller, controllerOffsets.pawn)) {
			continue; // the original required a valid pawn handle
		}

		std::map<std::string, playersinfo::Json> player;
		player["userid"] = playersinfo::Json::Number(slot);

		const char* rawName = pisex::GetPlayerName(slot);
		const std::string name = (rawName != nullptr && rawName[0] != '\0') ? rawName : "Unknown";
		player["name"] = playersinfo::Json::String(name);

		player["team"] = playersinfo::Json::Number(ControllerInt(controller, controllerOffsets.team, 1));

		const std::uint64_t steamId = pisex::GetSteamID64(slot);
		player["steamid"] = playersinfo::Json::String(std::to_string(steamId));

		player["kills"] = playersinfo::Json::Number(ReadMatchStat(controller, offsets.kills));
		player["death"] = playersinfo::Json::Number(ReadMatchStat(controller, offsets.deaths));
		player["headshots"] = playersinfo::Json::Number(ReadMatchStat(controller, offsets.headshots));
		player["ping"] = playersinfo::Json::Number(ControllerInt(controller, controllerOffsets.ping, (int)sizeof(std::uint32_t)));

		const std::int64_t connectedAt = g_connectionTimes[static_cast<std::size_t>(slot)];
		player["playtime"] = playersinfo::Json::Number(now - connectedAt);

		player["prime"] = playersinfo::Json::Bool(steamId != 0 && CheckPrime(steamId));

		if (haveRanks) {
			try {
				player["rank"] = playersinfo::Json::Number(levels_ranks::GetClientInfo(slot, kLrStatsRank, false));
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

// --- events ------------------------------------------------------------------------------------

void OnPlayerConnect(const char* /*name*/, pisex::IGameEvent* event, bool /*dontBroadcast*/) {
	if (event == nullptr) {
		return;
	}
	const int slot = event->GetPlayerSlot("userid");
	if (slot >= 0 && slot < kMaxSlots) {
		g_connectionTimes[static_cast<std::size_t>(slot)] = static_cast<std::int32_t>(std::time(nullptr));
	}
}

void OnPlayerDisconnect(const char* /*name*/, pisex::IGameEvent* event, bool /*dontBroadcast*/) {
	if (event == nullptr) {
		return;
	}
	const int slot = event->GetPlayerSlot("userid");
	if (slot >= 0 && slot < kMaxSlots) {
		g_connectionTimes[static_cast<std::size_t>(slot)] = 0;
	}
}

// --- mm_getinfo --------------------------------------------------------------------------------

s2sdk::ResultType OnMmGetInfo(int32_t /*slot*/, s2sdk::ConCommandContext /*context*/,
							  const plg::vector<plg::string>& /*args*/) {
	try {
		if (!pisex::IsGameReady()) {
			s2sdk::PrintToServer("{\"error\":\"no map\"}");
			return s2sdk::ResultType::Handled;
		}
		const std::string info = BuildServerInfo();
		s2sdk::PrintToServer(plg::string(std::string_view(info)));
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
			pisex::Init("playersinfo");

			std::string error;
			if (!steamlink::Init(error)) {
				Log("playersinfo: Steam game server API unavailable (" + error +
						") - 'prime' will always be false",
					plg::Severity::Warning);
			}

			pisex::HookEvent("player_connect", &OnPlayerConnect);
			pisex::HookEvent("player_disconnect", &OnPlayerDisconnect);

			if (__s2sdk_AddConsoleCommand) {
				m_commandRegistered = s2sdk::AddConsoleCommand("mm_getinfo", "", s2sdk::ConVarFlag::GameDll,
															   &OnMmGetInfo, s2sdk::HookMode::Post);
			}
			if (!m_commandRegistered) {
				Log("playersinfo: could not register the 'mm_getinfo' console command", plg::Severity::Error);
			}

			Log("playersinfo: ready (mm_getinfo registered" +
					std::string(__levels_ranks_GetClientInfo != nullptr ? ", levels_ranks found" : "") + ")",
				plg::Severity::Info);
		} catch (const std::exception& ex) {
			Log(std::string("playersinfo: OnPluginStart failed: ") + ex.what(), plg::Severity::Error);
		} catch (...) {
			Log("playersinfo: OnPluginStart failed with an unknown exception", plg::Severity::Error);
		}
		return {}; // never Failed: that would stop every Plugify plugin
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds /*dt*/) override {
		try {
			pisex::Poll();
		} catch (...) {
			Log("playersinfo: Poll() threw", plg::Severity::Error);
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			pisex::UnhookEvent("player_connect");
			pisex::UnhookEvent("player_disconnect");
			if (m_commandRegistered) {
				if (__s2sdk_RemoveCommand) {
					s2sdk::RemoveCommand("mm_getinfo", &OnMmGetInfo);
				}
				m_commandRegistered = false;
			}
			pisex::Shutdown();
		} catch (...) {
		}
		return {};
	}

private:
	bool m_commandRegistered = false;
};

PlayersInfoPlugin g_playersInfoPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_playersInfoPlugin)
