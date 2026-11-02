# PlayersInfo — what the binary does (analysis)

Analysed: the original `PlayersInfo.so` (md5 `0473fddc…`; not stripped, but without DWARF —
everything below comes from symbols, strings and `objdump -d`).

Metamod:Source plugin, author tag `Pisex`, name/log tag `PlayersInfo`,
<https://discord.gg/g798xERK5Y>. Needs the ABKAM ecosystem at runtime: it looks up `IUtilsApi`,
`IPlayersApi` and `ILRApi` through `ISmmAPI::MetaFactory` and clears them again on unload. No
CVars, no config file, no HTTP client — but it links `nlohmann::json` and steam_api
(`SteamGameServer_GetHSteamUser`, `SteamInternal_ContextInit`,
`SteamInternal_FindOrCreateGameServerInterface` are undefined imports resolved against the server
process).

## Exported surface

| symbol | what it is |
|---|---|
| `PlayersInfo::Load/Unload/AllPluginsLoaded` | the ISmmPlugin entry points (interfaces: `VEngineCvar007`, `SchemaSystem_001`, `Source2EngineToServer001`) |
| `mm_getinfo_command` / `mm_getinfo_callback` | a **static `ConCommand`** created in the static initialiser: `ConCommand::Create("mm_getinfo", …)` with flags `4` = `FCVAR_GAMEDLL`, registered when the plugin calls `ConVar_Register(0x1080004, …)` in `Load` |
| `OnPlayerConnect` / `OnPlayerDisconnect` | `IGameEvent` listeners for `player_connect` / `player_disconnect`, registered through `g_pUtils->vtable[0x70]` (IUtilsApi::HookEvent) |
| `GetServerInfo` / `GetTeamScore` / `CheckPrime` / `GameEntitySystem` | the workers |
| `StartupServer` | IUtilsApi startup callback: caches the entity systems + `gpGlobals` and zeroes the connection table |

`mm_getinfo_callback` does `GetServerInfo().dump(-1, ' ', false, error_handler::strict)` and prints
the string server-side through `g_SMAPI->ConPrintf("%s", …)` — i.e. the JSON goes to the **server
console**, which is how a remote web module reads it (RCON).

## The JSON

Built with `nlohmann::json` (object = `std::map`, so keys come out **alphabetically sorted**):

```json
{"current_map":"de_dust2","players":[{"death":4,"headshots":3,"kills":12,"name":"…","ping":31,
 "playtime":1234,"prime":true,"rank":7,"steamid":"76561198000000000","team":2,"userid":3}],
 "score_ct":5,"score_t":3,"time":1759912345}
```

| key | source (disassembly) |
|---|---|
| `time` | `(int64)time(nullptr)` |
| `current_map` | `g_SMAPI->Format("%s", gpGlobals->mapname)` |
| `score_ct` / `score_t` | `GetTeamScore()`: walk the `cs_team_manager` entities, read `CBaseEntity::m_iTeamNum` and `CTeam::m_iScore`; team 3 → CT, team 2 → T |
| `players` | array; the loop is `for (i = 0; i < 0x40; i++)` (64 slots) |

Per player, in evaluation order `userid, name, team, steamid, kills, death, headshots, ping,
playtime, prime, rank`:

| key | source |
|---|---|
| `userid` | the **loop counter** (0-based slot) |
| `name` | `IPlayersApi::GetPlayerName(slot)` (`CBasePlayerController::m_iszPlayerName`), `"Unknown"` when empty |
| `team` | `CBaseEntity::m_iTeamNum` of the controller |
| `steamid` | `IPlayersApi::GetSteamID64(slot)` formatted with `"%lld"` (decimal string) |
| `kills` / `death` / `headshots` | `CCSPlayerController::m_pActionTrackingServices` → `CCSPlayerController_ActionTrackingServices::m_matchStats` → `CSPerRoundStats_t::m_iKills` / `m_iDeaths` / `m_iHeadShotKills` (two raw-pointer hops, schema offsets cached) |
| `ping` | `CCSPlayerController::m_iPing` |
| `playtime` | `time(nullptr) - g_iConnection[slot]` |
| `prime` | `CheckPrime(steamid)`: `SteamGameServer()->UserHasLicenseForApp(steamid, 624820 /*0x988b4*/) == 0` or `…(steamid, 54029 /*0xd30d*/) == 0` (the CS2 / CS:GO-prime app ids; 0 = has licence) |
| `rank` | `ILRApi::GetClientInfo(slot, ST_RANK = 1, false)` — **only if the LR core was found** (the code skips the assignment when `g_pLRCore` is null, so the key is missing then) |

Slot selection (this is the filter that matters): a slot is included when
`g_iConnection[slot] != 0` (i.e. the slot fired `player_connect` and has not disconnected), the
controller entity exists (`GetEntityIdentity(CEntityIndex(slot + 1))`), and the controller's
`m_hPlayerPawn` handle resolves to an entity. `OnPlayerConnect` stores `time()` into
`g_iConnection[event->GetInt("userid")]`; `OnPlayerDisconnect` stores 0.

## The config file is not read by this build

`playerinfo_settings.ini` (the `"Playerinfo"` block with
`faceit_key`, `use_csgo`, `premier_rank`, `post_push_url`, `post_push_password`, `debug_logs`)
belongs to an **older Pisex build of this plugin**: the analysed binary contains **no strings for
any of those keys**, no `.ini` path, no HTTP/Faceit code at all (verified over every printable
string of the .so) and no interface that would supply them. The same file is shipped unchanged by
`package.sh` (reference), but nothing reads it — the web module polls `mm_getinfo` over RCON
instead.

## Port

`plugins/PlayersInfo` (manifest name `PlayersInfo`, the original `.vdf` alias and `.so` name).
Pure `s2sdk` (`docs/PLUGIN_STANDARD.md` §7): no `pisexcompat` and no guard library — the safety
checks live in the plugin itself.

* `src/plugin.cpp` — registers `mm_getinfo` as a `ConVarFlag::GameDll` console command with s2sdk
  (`AddConsoleCommand`), hooks `player_connect`/`player_disconnect` with
  `s2sdk::HookEvent(..., HookMode::Pre)` and unhooks them in `OnPluginEnd`, builds the same document
  with the same schema chain, prints it with `s2sdk::PrintToServer` (the same server console channel
  as the original's `ConPrintf`, but split into token-aligned chunks - see
  `src/console_output.hpp` below, because one ConMsg call truncates at 2047 bytes). The map gate (`OnMapStart`/`OnMapEnd` listeners + the first
  `OnPluginUpdate` latch, `PITFALLS B1/B2`), the slot/client/controller checks (`C2`), the
  map-gated schema offsets (`C8`), the event hooks (`C12`) and the wiring of the command lifecycle
  (`C11`/`A7`, see below) are implemented there.
* `src/map_gate.hpp` — the B1/B2 map readiness gate as an engine-free state machine, driven by the
  s2sdk listeners and unit tested in `tests/PlayersInfo_tests.cpp`; every client, entity and schema
  call is behind its `Ready()`.
* `src/connection_times.hpp` — the original's `g_iConnection[64]` table (connect/disconnect by slot,
  the once-only seeding for a plugin loaded mid-map), engine-free and unit tested.
* `src/command_lifecycle.hpp` — the `mm_getinfo` registration and its symmetrical teardown
  (`PITFALLS C11`/`A7`): s2sdk's `AddConsoleCommand` always attaches the callback and only reports
  whether the engine command itself was created, so the plugin remembers that answer — `OnPluginEnd`
  removes a command it created with `RemoveCommand`, only detaches its callback from one that already
  existed (`RemoveCommandListener`, which leaves the other owner's command alone), and touches
  nothing when the binding was missing. The server-console-only dispatch lives here too, and the
  whole file is unit tested against fake `__s2sdk_*` command bindings.
* `src/console_output.hpp` — the document printer. `s2sdk::PrintToServer` is `ConMsg("%s\n", …)`,
  which truncates at 2047 bytes (live bug on 27035: 18 bots produced a 2047-byte truncated document
  and the web poller's `json.loads` failed with EOF), so the document is printed in pieces of at
  most 1800 bytes cut **only between JSON tokens** (after `,` `:` `{` `[` `}` `]` outside strings).
  The newline each call appends is legal JSON whitespace, so an RCON client that concatenates the
  lines parses one object; a document with a single indivisible token is replaced by one explicit
  `{"error":"document too long"}` object, and a missing `PrintToServer` binding prints nothing.
  Unit tested on synthetic 18- and 64-player documents (UTF-8, quotes, backslashes, commas inside
  strings), with a fake binding and without it.
* `src/json_writer.hpp` — compact nlohmann-compatible JSON (sorted keys, same escaping), unit
  tested byte for byte in `tests/PlayersInfo_tests.cpp`.
* `src/ent_handle.hpp` — the entity-handle validity rule (`0`/`-1` mean "no entity", the sign is
  meaningless), unit tested.
* `src/steam_gameserver.{hpp,cpp}` — `ISteamGameServer` for `prime`, obtained through the official
  flat accessor `SteamAPI_SteamGameServer_v015()` and called through
  `SteamAPI_ISteamGameServer_UserHasLicenseForApp`, both resolved with `dlsym(RTLD_DEFAULT, …)`;
  a missing symbol or an interface that is not up yet answers `-1` and `prime` is `false`.
  **Never** `SteamInternal_ContextInit` with a caller-built context: its buffer is
  `{callback, counter, pointer}` and the callback is called, so a zeroed buffer (the 1.1.0 bug that
  crashed 27035 with SIGSEGV/RIP=0 on the first `mm_getinfo`) is a null function pointer.
  Regression-tested in `tests/steamlink_tests.cpp` against a fake `dlsym()`.
* `rank` uses the ported `levels_ranks` plugin's exported `GetClientInfo(slot, 1, false)`
  (optional dependency; the key is omitted when the plugin is absent, like the original with a
  null LR core).

Deliberate differences: the original indexed `g_iConnection` with the raw event `userid` but read it
by slot; in CS2 the event userid is not the slot (see `plugins/as_admin_time`), so the port
translates it with `s2sdk::GetEventPlayerSlot()` and keeps both sides slot-indexed. The document
itself is byte-compatible apart from the trailing newline: s2sdk's `PrintToServer` is
`ConMsg("%s\n", …)` while the original's `ConPrintf("%s", …)` appended nothing (a line break makes
line-based RCON parsing easier and is ignored by JSON parsers).

Needs an in-game check: (a) that the RCON consumer accepts the output from `PrintToServer` the same way
it accepted `ConPrintf` (same tier0 channel, different wrapper), (b) `prime` against a known prime
account, and (c) that the event `player_connect` fires reliably for every joining player (the
original depended on it as well — a player whose connect event was missed is simply absent from the
list until they reconnect).
