#include "steam_gameserver.hpp"

#include <dlfcn.h>

namespace steamlink {
namespace {

// The flat entry points libsteam_api exports (steam_api_flat.h). The accessor is the SDK's own
// type-safe wrapper around SteamInternal_ContextInit - it owns the {callback, counter, pointer}
// triple that ContextInit calls, so the plugin never has to (and must not) build one itself.
using GameServerAccessorFn = void* (*)(); // S_API ISteamGameServer* SteamAPI_SteamGameServer_v015();
using UpdateUserDataFn = bool (*)(void*, std::uint64_t, const char*, std::uint32_t);
using UserHasLicenseFn = int (*)(void*, std::uint64_t, std::uint32_t);

// Symbol names are resolved by RTLD_DEFAULT lookup, never an interface version guessed by the
// plugin: these three are the exact exports of the steam_api build the CS2 server runs (verified
// with nm on the deployed libsteam_api.so - the accessor under this name, plus the two flat
// wrappers, are all present).
constexpr const char* kGameServerAccessorSymbol = "SteamAPI_SteamGameServer_v015";
constexpr const char* kUserHasLicenseSymbol = "SteamAPI_ISteamGameServer_UserHasLicenseForApp";
constexpr const char* kUpdateUserDataSymbol = "SteamAPI_ISteamGameServer_BUpdateUserData";

GameServerAccessorFn g_gameServerAccessor = nullptr;
UpdateUserDataFn g_updateUserData = nullptr;
UserHasLicenseFn g_userHasLicense = nullptr;

template <typename Fn>
Fn Resolve(const char* name) {
	return reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
}

// Forget a half-resolved state: IsAvailable()/GameServer()/the wrappers must never act on a
// partially resolved API.
void Clear() {
	g_gameServerAccessor = nullptr;
	g_updateUserData = nullptr;
	g_userHasLicense = nullptr;
}

} // namespace

bool Init(std::string& error) {
	// Called on every server frame while the symbols are missing (the update loop retries), so a
	// successful resolution short-circuits.
	if (g_gameServerAccessor != nullptr && g_userHasLicense != nullptr) {
		return true;
	}

	Clear();
	g_gameServerAccessor = Resolve<GameServerAccessorFn>(kGameServerAccessorSymbol);
	g_userHasLicense = Resolve<UserHasLicenseFn>(kUserHasLicenseSymbol);
	g_updateUserData = Resolve<UpdateUserDataFn>(kUpdateUserDataSymbol);

	if (g_gameServerAccessor == nullptr) {
		Clear();
		error = std::string(kGameServerAccessorSymbol) +
				" not found in the process (is steam_api loaded, and is the game server API part of it?)";
		return false;
	}
	if (g_userHasLicense == nullptr) {
		Clear();
		error = std::string(kUserHasLicenseSymbol) + " not found in the process";
		return false;
	}
	// BUpdateUserData is not used by this plugin: it is resolved only so the shared header stays
	// honest, and its absence is not an error.
	return true;
}

bool IsAvailable() {
	return g_gameServerAccessor != nullptr && g_userHasLicense != nullptr;
}

void Shutdown() {
	Clear();
}

void* GameServer() {
	if (g_gameServerAccessor == nullptr) {
		return nullptr;
	}
	try {
		// The accessor is the official lookup: it returns the interface the engine brought up (and
		// a valid, initialised context), or nullptr while the Steam game server API is not up yet.
		return g_gameServerAccessor();
	} catch (...) {
		return nullptr;
	}
}

bool BUpdateUserData(std::uint64_t steamId, const char* name, std::uint32_t score) {
	if (g_updateUserData == nullptr) {
		return false;
	}
	void* gameServer = GameServer();
	if (gameServer == nullptr) {
		return false;
	}
	try {
		return g_updateUserData(gameServer, steamId, name != nullptr ? name : "", score);
	} catch (...) {
		return false;
	}
}

int UserHasLicenseForApp(std::uint64_t steamId, std::uint32_t appId) {
	if (g_userHasLicense == nullptr) {
		return -1;
	}
	void* gameServer = GameServer();
	if (gameServer == nullptr) {
		// The engine has not brought its game server API up (or steam_api is not the game server
		// build): answer "unknown" instead of calling a wrapper with a null instance.
		return -1;
	}
	try {
		return g_userHasLicense(gameServer, steamId, appId);
	} catch (...) {
		return -1;
	}
}

} // namespace steamlink
