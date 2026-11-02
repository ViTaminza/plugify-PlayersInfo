// connection_times — the original's `g_iConnection[64]` table, engine-free so the tests can drive
// it.
//
// The original stored `time(nullptr)` for the slot of every `player_connect` event and 0 for
// `player_disconnect`; `GetServerInfo()` skipped the slots whose entry was still 0 (the comment in
// BEHAVIOR.md). The port keeps the same table, but indexes it by the *player slot*: in CS2 the
// event's `userid` is not the slot (see plugins/as_admin_time), so src/plugin.cpp translates it
// with s2sdk::GetEventPlayerSlot and keeps both sides slot-indexed.
//
// `playtime` is `time(nullptr) - ConnectedAt(slot)`; a slot that never connected (or has already
// disconnected) is not listed at all.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace playersinfo {

// The original loops over 0..63 (compared against 0x40 in GetServerInfo).
inline constexpr std::int32_t kMaxSlots = 64;

class ConnectionTimes {
public:
	void Reset() { m_times.fill(0); }

	// player_connect: remember when the slot connected. An out-of-range slot (an event field that
	// did not resolve) stores nothing and answers false, exactly like the original's bounds check.
	bool Connect(std::int32_t slot, std::int32_t now) {
		if (!InRange(slot)) {
			return false;
		}
		m_times[Index(slot)] = now;
		return true;
	}

	// player_disconnect: drop the timestamp; the next player_connect starts the playtime anew.
	bool Disconnect(std::int32_t slot) {
		if (!InRange(slot)) {
			return false;
		}
		m_times[Index(slot)] = 0;
		return true;
	}

	// The connect timestamp, or 0 when the slot is not connected / out of range.
	[[nodiscard]] std::int32_t ConnectedAt(std::int32_t slot) const {
		return InRange(slot) ? m_times[Index(slot)] : 0;
	}

	// The original's "g_iConnection[slot] != 0" filter.
	[[nodiscard]] bool IsPresent(std::int32_t slot) const { return ConnectedAt(slot) != 0; }

	// A plugin loaded (or reloaded) into a running map never saw player_connect for the players
	// already there: without this, mm_getinfo keeps returning an empty "players" list until every
	// one of them reconnects. Seed gives every slot that has no timestamp yet and that `shouldSeed`
	// accepts the current time once and answers how many slots were seeded. `shouldSeed` is the
	// caller's client check (connected, in game, not a bot); it is a template parameter so this
	// header stays engine-free and testable without a callback allocation.
	template <typename ShouldSeed>
	int Seed(std::int32_t now, ShouldSeed&& shouldSeed) {
		int seeded = 0;
		for (std::int32_t slot = 0; slot < kMaxSlots; ++slot) {
			if (m_times[Index(slot)] != 0) {
				continue;
			}
			if (!shouldSeed(slot)) {
				continue;
			}
			m_times[Index(slot)] = now;
			++seeded;
		}
		return seeded;
	}

private:
	[[nodiscard]] static bool InRange(std::int32_t slot) { return slot >= 0 && slot < kMaxSlots; }
	[[nodiscard]] static std::size_t Index(std::int32_t slot) { return static_cast<std::size_t>(slot); }

	std::array<std::int32_t, static_cast<std::size_t>(kMaxSlots)> m_times{};
};

} // namespace playersinfo
