// Regression tests for src/steam_gameserver.cpp — the crash of PlayersInfo 1.1.0 on 27035.
//
// The deployed build obtained ISteamGameServer through SteamInternal_ContextInit() with a
// zero-initialised 0x18-byte buffer. steam_api_internal.h:32-36 documents that buffer as
// `struct { void (*pFn)(void*); uintptr_t counter; void* ptr; }` and SteamInternal_ContextInit
// *calls pFn*, so the zeroed buffer was a null callback: the first mm_getinfo that reached
// steamlink::UserHasLicenseForApp crashed with SIGSEGV, RIP=0 (libsteam_api.so+0x2d4e1), RSI =
// 0x988b4 (the CS2 app id). The fix resolves the library's own flat accessor
// `SteamAPI_SteamGameServer_v015()` instead and never builds a context itself.
//
// This test compiles the real src/steam_gameserver.cpp and drives it through a fake dlsym(): it can
// therefore prove, at runtime and without steam_api, that
//   * SteamInternal_ContextInit is never requested (the regression guard),
//   * only the documented flat symbols are requested and exactly those handle a missing library,
//   * a missing accessor / a missing license wrapper / an interface that is not up yet fail soft
//     (-1, false, nullptr) and never call the license wrapper,
//   * a live interface receives this plugin's ISteamGameServer pointer, steamID and appID, and its
//     result is passed through,
//   * Shutdown() forgets the resolved pointers and a later Init() resolves them again.
//
// No engine, no pisexcompat, no libsteam_api: only the plugin's own source is compiled in.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

#include "steam_gameserver.hpp"

namespace {

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

// --- fake dynamic loader ----------------------------------------------------------------------

struct FakeSymbol {
	const char* name;
	void* address;
};

constexpr int kMaxSymbols = 8;
constexpr int kMaxRequests = 64;

FakeSymbol g_symbols[kMaxSymbols] = {};
int g_symbolCount = 0;

// Every name the production code asked for, so the test can prove what is (and is not) resolved.
const char* g_requests[kMaxRequests] = {};
int g_requestCount = 0;
bool g_contextInitRequested = false;

// The fake ISteamGameServer instance. Only its address matters: the wrappers must receive exactly
// this pointer.
int g_instance = 0;

int g_accessorCalls = 0;
void* g_accessorResult = nullptr;

int g_licenseCalls = 0;
void* g_licenseSelf = nullptr;
std::uint64_t g_licenseSteamId = 0;
std::uint32_t g_licenseAppId = 0;
int g_licenseResult = -1;

int g_updateCalls = 0;
void* g_updateSelf = nullptr;
std::uint64_t g_updateSteamId = 0;
std::uint32_t g_updateScore = 0;
std::string g_updateName;
bool g_updateResult = true;

void ResetFakes() {
	g_symbolCount = 0;
	g_requestCount = 0;
	g_contextInitRequested = false;
	g_accessorCalls = 0;
	g_accessorResult = nullptr;
	g_licenseCalls = 0;
	g_licenseSelf = nullptr;
	g_licenseSteamId = 0;
	g_licenseAppId = 0;
	g_licenseResult = -1;
	g_updateCalls = 0;
	g_updateSelf = nullptr;
	g_updateSteamId = 0;
	g_updateScore = 0;
	g_updateName.clear();
	g_updateResult = true;
}

void AddSymbol(const char* name, void* address) {
	if (g_symbolCount < kMaxSymbols) {
		g_symbols[g_symbolCount++] = {name, address};
	}
}

void AddAllSymbols(void* accessor, void* license, void* update) {
	AddSymbol("SteamAPI_SteamGameServer_v015", accessor);
	AddSymbol("SteamAPI_ISteamGameServer_UserHasLicenseForApp", license);
	AddSymbol("SteamAPI_ISteamGameServer_BUpdateUserData", update);
}

bool WasRequested(const char* name) {
	for (int i = 0; i < g_requestCount; ++i) {
		if (g_requests[i] != nullptr && std::strcmp(g_requests[i], name) == 0) {
			return true;
		}
	}
	return false;
}

void* FakeSteamGameServer() {
	++g_accessorCalls;
	return g_accessorResult;
}

int FakeUserHasLicenseForApp(void* self, std::uint64_t steamId, std::uint32_t appId) {
	++g_licenseCalls;
	g_licenseSelf = self;
	g_licenseSteamId = steamId;
	g_licenseAppId = appId;
	return g_licenseResult;
}

bool FakeBUpdateUserData(void* self, std::uint64_t steamId, const char* name, std::uint32_t score) {
	++g_updateCalls;
	g_updateSelf = self;
	g_updateSteamId = steamId;
	g_updateName = name != nullptr ? name : "";
	g_updateScore = score;
	return g_updateResult;
}

} // namespace

// The plugin's only dynamic-loader dependency. Defining it here (instead of linking libdl) is what
// makes src/steam_gameserver.cpp testable without steam_api: the production code calls the real
// `dlsym(RTLD_DEFAULT, ...)` symbol, which resolves to this function in the test binary.
extern "C" void* dlsym(void* handle, const char* name) noexcept {
	(void)handle;
	// glibc declares `name` nonnull, so no null check here: the production code passes literals.
	if (g_requestCount < kMaxRequests) {
		g_requests[g_requestCount++] = name;
	}
	if (std::strcmp(name, "SteamInternal_ContextInit") == 0) {
		g_contextInitRequested = true;
	}
	for (int i = 0; i < g_symbolCount; ++i) {
		if (std::strcmp(g_symbols[i].name, name) == 0) {
			return g_symbols[i].address;
		}
	}
	return nullptr;
}

namespace {

constexpr std::uint64_t kSteamId = 76561198000000001ULL;
constexpr std::uint32_t kCs2AppId = 624820;

void TestMissingSymbols() {
	std::printf("-- steamlink: no steam_api at all\n");
	steamlink::Shutdown();
	ResetFakes();

	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(!error.empty());
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == -1);
	CHECK(!steamlink::BUpdateUserData(kSteamId, "player", 3));
	CHECK(g_licenseCalls == 0);
	CHECK(g_updateCalls == 0);
	// The crash of 1.1.0: the old code resolved and called SteamInternal_ContextInit with a zeroed
	// buffer. It must never be looked up again.
	CHECK(!g_contextInitRequested);
	CHECK(!WasRequested("SteamInternal_ContextInit"));
}

void TestContextInitIsNeverResolved() {
	std::printf("-- steamlink: only the flat symbols are resolved\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instance;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeUserHasLicenseForApp),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(WasRequested("SteamAPI_SteamGameServer_v015"));
	CHECK(WasRequested("SteamAPI_ISteamGameServer_UserHasLicenseForApp"));
	CHECK(!g_contextInitRequested);
	// Exactly the documented flat entry points - no guessed interface version, no context struct.
	CHECK(g_requestCount == 3);
	(void)steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId);
	CHECK(g_licenseCalls == 1);
}

void TestAccessorMissing() {
	std::printf("-- steamlink: accessor missing, license wrapper present\n");
	steamlink::Shutdown();
	ResetFakes();
	AddSymbol("SteamAPI_ISteamGameServer_UserHasLicenseForApp",
			  reinterpret_cast<void*>(&FakeUserHasLicenseForApp));

	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(error.find("SteamAPI_SteamGameServer_v015") != std::string::npos);
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == -1);
	CHECK(g_licenseCalls == 0);
}

void TestLicenseWrapperMissing() {
	std::printf("-- steamlink: accessor present, license wrapper missing\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instance;
	AddSymbol("SteamAPI_SteamGameServer_v015", reinterpret_cast<void*>(&FakeSteamGameServer));

	std::string error;
	CHECK(!steamlink::Init(error));
	CHECK(error.find("SteamAPI_ISteamGameServer_UserHasLicenseForApp") != std::string::npos);
	// A partially resolved API must not be usable: Init() clears what it resolved.
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == -1);
	CHECK(g_accessorCalls == 0);
	CHECK(g_licenseCalls == 0);
}

void TestInterfaceNotUpYet() {
	std::printf("-- steamlink: symbols present, interface not up (nullptr)\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = nullptr; // the engine has not initialised its game server API yet
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeUserHasLicenseForApp),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	// The wrappers must not be called with a null instance.
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == -1);
	CHECK(!steamlink::BUpdateUserData(kSteamId, "player", 3));
	CHECK(g_licenseCalls == 0);
	CHECK(g_updateCalls == 0);
}

void TestLive() {
	std::printf("-- steamlink: live interface\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instance;
	g_licenseResult = 0; // k_EUserHasLicenseResultHasLicense
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeUserHasLicenseForApp),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == &g_instance);

	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == 0);
	CHECK(g_licenseCalls == 1);
	// The wrapper has to receive the interface the accessor returned - never null, never a context
	// or counter value (which is what the crashed build passed to libsteam_api).
	CHECK(g_licenseSelf == &g_instance);
	CHECK(g_licenseSteamId == kSteamId);
	CHECK(g_licenseAppId == kCs2AppId);

	// A negative answer is passed through unchanged (does not have the licence / no auth).
	g_licenseResult = 2;
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == 2);
	CHECK(g_licenseCalls == 2);

	// The other wrapper shares the same instance.
	CHECK(steamlink::BUpdateUserData(kSteamId, "player", 42));
	CHECK(g_updateCalls == 1);
	CHECK(g_updateSelf == &g_instance);
	CHECK(g_updateSteamId == kSteamId);
	CHECK(g_updateName == "player");
	CHECK(g_updateScore == 42);
	g_updateResult = false;
	CHECK(!steamlink::BUpdateUserData(kSteamId, nullptr, 0)); // a null name is passed as ""
	CHECK(g_updateName.empty());
}

void TestReset() {
	std::printf("-- steamlink: shutdown and re-init\n");
	steamlink::Shutdown();
	ResetFakes();
	g_accessorResult = &g_instance;
	g_licenseResult = 0;
	AddAllSymbols(reinterpret_cast<void*>(&FakeSteamGameServer),
				  reinterpret_cast<void*>(&FakeUserHasLicenseForApp),
				  reinterpret_cast<void*>(&FakeBUpdateUserData));

	std::string error;
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());

	steamlink::Shutdown();
	CHECK(!steamlink::IsAvailable());
	CHECK(steamlink::GameServer() == nullptr);
	const int callsBefore = g_licenseCalls;
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == -1);
	CHECK(g_licenseCalls == callsBefore); // nothing may be called after Shutdown

	// OnPluginEnd dropped the pointers; a reload resolves them again.
	CHECK(steamlink::Init(error));
	CHECK(steamlink::IsAvailable());
	CHECK(steamlink::UserHasLicenseForApp(kSteamId, kCs2AppId) == 0);
	CHECK(g_licenseCalls == callsBefore + 1);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("steamlink_tests\n");

	TestMissingSymbols();
	TestContextInitIsNeverResolved();
	TestAccessorMissing();
	TestLicenseWrapperMissing();
	TestInterfaceNotUpYet();
	TestLive();
	TestReset();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
