// command_lifecycle — the `mm_getinfo` registration and its symmetrical teardown (PITFALLS C11/A7).
//
// s2sdk's AddConsoleCommand is "create or hook": it always attaches the callback, and its bool only
// says whether the engine command itself was created by this call.
//   * true  -> the valve command did not exist and now belongs to this plugin (Created);
//   * false -> the name was already taken (another plugin, the engine, or our own entry kept by
//              s2sdk across a `plg reload`, which cannot destroy a console command); the callback
//              is attached to the existing entry (Existing).
// The distinction has to be remembered, because the teardown is not interchangeable:
//   * RemoveCommand(name, cb) also calls s2sdk's RemoveValveCommand, which erases the command from
//     s2sdk's own table (taking the other owner's registration with it) or, for a command s2sdk did
//     not create, calls UnregisterConCommandCallbacks on it - both destroy somebody else's command
//     and were a real bug: a plugin that only hooked an existing `mm_getinfo` removed it on unload;
//   * RemoveCommandListener(name, cb, mode) removes only this plugin's callback and leaves the
//     command (and its owner) alone;
//   * a command that was never attempted (the AddConsoleCommand binding is missing) must not be
//     touched at all.
//
// The functions below go through the s2sdk bindings, so tests/PlayersInfo_tests.cpp can replace the
// three of them with fakes and drive every branch without an engine (no pisexcompat, no host).

#pragma once

#include <cstdint>

#include <s2sdk/s2sdk.hpp>

namespace playersinfo {

// What this plugin owns after RegisterCommand().
enum class CommandOwnership : std::uint8_t {
	NotAttempted, // the AddConsoleCommand binding is missing: no command, no callback attached
	Created,      // AddConsoleCommand answered true: the engine command itself is ours
	Existing,     // AddConsoleCommand answered false: only our callback was attached
};

// Which teardown call UnregisterCommand() made (the plugin logs it, the tests assert on it).
enum class CommandTeardown : std::uint8_t {
	None,            // nothing was registered / the needed binding is missing: nothing was removed
	RemovedCommand,  // RemoveCommand (ownership Created)
	RemovedListener, // RemoveCommandListener (ownership Existing)
};

// Registers `name` and answers what this plugin now owns. `mode` is the hook mode the callback is
// attached with - the same value has to be passed to UnregisterCommand().
inline CommandOwnership RegisterCommand(const char* name, const char* description,
										s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
										s2sdk::HookMode mode) {
	if (__s2sdk_AddConsoleCommand == nullptr) {
		return CommandOwnership::NotAttempted;
	}
	try {
		const bool created =
				s2sdk::AddConsoleCommand(plg::string(name), plg::string(description), flags, callback, mode);
		return created ? CommandOwnership::Created : CommandOwnership::Existing;
	} catch (...) {
		// s2sdk attaches the callback before it knows whether the name was free, so after a
		// throwing call the safe assumption is "a listener of ours may be attached": Existing
		// tears down with RemoveCommandListener, which can never destroy somebody else's command.
		return CommandOwnership::Existing;
	}
}

// The symmetrical teardown (PITFALLS A7) with the ownership of PITFALLS C11.
inline CommandTeardown UnregisterCommand(const char* name, s2sdk::ConCommandCallback callback,
										 s2sdk::HookMode mode, CommandOwnership ownership) {
	switch (ownership) {
	case CommandOwnership::Created: {
		if (__s2sdk_RemoveCommand == nullptr) {
			return CommandTeardown::None;
		}
		try {
			s2sdk::RemoveCommand(plg::string(name), callback);
			return CommandTeardown::RemovedCommand;
		} catch (...) {
			return CommandTeardown::None;
		}
	}
	case CommandOwnership::Existing: {
		if (__s2sdk_RemoveCommandListener == nullptr) {
			return CommandTeardown::None;
		}
		try {
			s2sdk::RemoveCommandListener(plg::string(name), callback, mode);
			return CommandTeardown::RemovedListener;
		} catch (...) {
			return CommandTeardown::None;
		}
	}
	case CommandOwnership::NotAttempted:
	default:
		return CommandTeardown::None;
	}
}

// PITFALLS C11: s2sdk dispatches the chat spellings "!mm_getinfo" / "/mm_getinfo" to the very same
// console-command callback, with the player's slot (the ConVar flags are not consulted on the chat
// path). The server console/RCON is caller -1 and nothing else: the original's callback accepted
// exactly `slot == -1`, and this is the same exact test - a player slot (>= 0) is refused, and so is
// any other negative value an engine or a future s2sdk could hand over. A refused call answers "not
// handled" and never reaches the printer, so the chat line and the player's own console output stay
// untouched. Returns whether `run` was invoked; `run` may throw and the caller is expected to
// translate that into an error.
template <typename Run>
bool RunForServerConsoleOnly(std::int32_t caller, Run&& run) {
	if (caller != -1) {
		return false;
	}
	run();
	return true;
}

} // namespace playersinfo
