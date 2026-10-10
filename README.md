# PlayersInfo (Plugify)

Plugify port of the Metamod:Source plugin 'PlayersInfo' (the plugin that shipped as addons/PlayersInfo). Registers the server console command 'mm_getinfo', which prints one compact JSON document to the server console: the current map, both team scores and, for every connected player with a pawn, {death, headshots, kills, name, ping, playtime, prime, rank, steamid, team, userid}. The example.com web controller polls this over RCON. 'rank' comes from the levels_ranks plugin (optional dependency, key omitted without it), 'prime' from ISteamGameServer::UserHasLicenseForApp. No config file is read (the Metamod playerinfo_settings.ini belongs to an older build, see BEHAVIOR.md).

Plugify port (fork) of **PlayersInfo** by Pisex (Metamod (C++)).
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, levels_ranks (optional), hudmenu (optional)

## Installation
Download `PlayersInfo-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/PlayersInfo`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

