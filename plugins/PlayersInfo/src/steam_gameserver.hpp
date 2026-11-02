// steam_gameserver — the ISteamGameServer access PlayersInfo needs for the "prime" field and s2sdk
// does not expose.
//
// How the interface is obtained: through the official flat accessor libsteam_api exports,
//
//     S_API ISteamGameServer *SteamAPI_SteamGameServer_v015();          // steam_api_flat.h:1134
//
// which is the SDK's own type-safe wrapper around SteamInternal_ContextInit: it carries the
// {callback, counter, pointer} triple that SteamInternal_ContextInit calls to find or create the
// game server interface, so calling it can never get the initialisation wrong. The engine has
// already brought its game server API up in this process, so the accessor hands out the live
// interface (null while it is not up yet - every call then fails soft).
//
// The interface methods are called through the flat C wrappers the same library exports,
//
//     S_API EUserHasLicenseForAppResult SteamAPI_ISteamGameServer_UserHasLicenseForApp(
//             ISteamGameServer* self, uint64 steamID, AppId_t appID);                // :1165
//     S_API bool SteamAPI_ISteamGameServer_BUpdateUserData(
//             ISteamGameServer* self, uint64 steamIDUser, const char* name, uint32 score); // :1177
//
// resolved with dlsym(RTLD_DEFAULT, ...) because a Plugify plugin is a plain dlopen()ed shared
// object and does not link libsteam_api itself.
//
// DO NOT call SteamInternal_ContextInit directly. It takes a caller-owned, mutable
// `struct { void (*pFn)(void*); uintptr_t counter; void* ptr; }` (steam_api_internal.h:32-36) and
// **calls pFn**. A zero-initialised 0x18-byte buffer - what this file used to pass - is a null
// callback and crashed the server with RIP=0 inside libsteam_api on the first
// UserHasLicenseForApp call (PlayersInfo 1.1.0, 27035). Only the library's own accessor owns that
// triple.
//
// Everything degrades to "unavailable" (-1 / false / nullptr) when a symbol is missing or the
// interface is not up yet, so a server without steam_api keeps working - it just reports every
// player as not prime.

#pragma once

#include <cstdint>
#include <string>

namespace steamlink {

// Resolves the steam_api entry points. Safe to call repeatedly: a successful resolution is
// remembered, a failed one is retried on the next call (the symbols may appear with the process
// loading steam_api later, and the interface itself appears when the engine brings its game server
// API up). Returns false (with a message in `error`) when the process does not expose them.
bool Init(std::string& error);

// Whether both the accessor and the license query resolved. The interface itself may still be null
// (see GameServer()) - this only says the symbols are there.
[[nodiscard]] bool IsAvailable();

// Drops the resolved pointers (OnPluginEnd). Nothing is unloaded: the symbols belong to the engine's
// steam_api, this plugin only forgets them; a later Init() resolves them again.
void Shutdown();

// The live ISteamGameServer, or nullptr while the Steam game server API is not up yet.
[[nodiscard]] void* GameServer();

// ISteamGameServer::BUpdateUserData - publishes one player to the Steam server browser.
bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score);

// ISteamGameServer::UserHasLicenseForApp - returns the EUserHasLicenseForAppResult value
// (k_EUserHasLicenseResultHasLicense = 0, ...DoesNotHaveLicense = 1, ...NoAuth = 2) or -1 when
// the call is unavailable (no license symbol, or the interface is not up yet).
int UserHasLicenseForApp(std::uint64_t steamId, std::uint32_t appId);

} // namespace steamlink
