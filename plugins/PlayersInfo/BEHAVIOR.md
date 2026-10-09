# PlayersInfo — what the binary does (analysis)

Analysed: `ref/src/binaries/PlayersInfo/PlayersInfo.so` (md5 `0473fddc…`, byte-identical to the copy
in the 27035 server backup; not stripped, but without DWARF — everything below comes from symbols,
strings and `objdump -d`).

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

`ref/prod-27016/addons/configs/playerinfo_settings.ini` (the `"Playerinfo"` block with
`faceit_key`, `use_csgo`, `premier_rank`, `post_push_url`, `post_push_password`, `debug_logs`)
belongs to an **older Pisex build of this plugin**: the analysed binary contains **no strings for
any of those keys**, no `.ini` path, no HTTP/Faceit code at all (verified over every printable
string of the .so) and no interface that would supply them. The production copy of the file is
shipped unchanged by `package.sh` (reference), but nothing reads it — the web module polls
`mm_getinfo` over RCON instead.

## Port

`plugins/PlayersInfo` (manifest name `PlayersInfo`, the original `.vdf` alias and `.so` name):

* `src/plugin.cpp` — registers `mm_getinfo` as a `ConVarFlag::GameDll` console command with s2sdk
  (`AddConsoleCommand`), hooks `player_connect`/`player_disconnect` through pisexcompat, builds the
  same document with the same schema chain, prints it with `s2sdk::PrintToServer` (the same server
  console channel as the original's `ConPrintf`).
* `src/json_writer.hpp` — compact nlohmann-compatible JSON (sorted keys, same escaping), unit
  tested byte for byte in `tests/PlayersInfo_tests.cpp`.
* `src/steam_gameserver.{hpp,cpp}` — `ISteamGameServer` for `prime`, resolved through
  `dlsym(RTLD_DEFAULT, "SteamInternal_ContextInit" / "SteamAPI_ISteamGameServer_…")` (the same
  functions the original imported as undefined symbols); without them `prime` is `false`.
* `rank` uses the ported `levels_ranks` plugin's exported `GetClientInfo(slot, 1, false)`
  (optional dependency; the key is omitted when the plugin is absent, like the original with a
  null LR core).

Deliberate differences: the original indexed `g_iConnection` with the raw event `userid` but read it
by slot; in CS2 the event userid is not the slot (see `plugins/as_admin_time`), so the port
translates it with `IGameEvent::GetPlayerSlot()` and keeps both sides slot-indexed. The document
itself is byte-compatible apart from the trailing newline: s2sdk's `PrintToServer` is
`ConMsg("%s\n", …)` while the original's `ConPrintf("%s", …)` appended nothing (a line break makes
line-based RCON parsing easier and is ignored by JSON parsers).

Needs a live check: (a) that the RCON consumer accepts the output from `PrintToServer` the same way
it accepted `ConPrintf` (same tier0 channel, different wrapper), (b) `prime` against a known prime
account, and (c) that the event `player_connect` fires reliably for every joining player (the
original depended on it as well — a player whose connect event was missed is simply absent from the
list until they reconnect).
