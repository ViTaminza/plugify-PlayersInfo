// map_gate — the map readiness gate of PITFALLS B1/B2, engine-free so the tests can drive every
// transition.
//
// s2sdk dereferences a null client list, entity system or schema serializer map before the first
// map and in the window between OnMapEnd and the next OnMapStart (PITFALLS B2: a client query
// there reads an emptied CUtlClientVector and segfaults; PITFALLS C8: a schema lookup walks a null
// serializer map). Every client, entity and schema call of this plugin therefore sits behind
// Ready().
//
// The rules, exactly as src/plugin.cpp wires them to the s2sdk listeners:
//   * OnPluginStart -> Reset() + SetListenersRegistered(): a fresh start (including `plg reload`)
//     begins with the gate shut; nothing is touched before the first map.
//   * OnMapStart     -> OnMapStart(): the new map and its entity system exist, the gate opens.
//   * OnMapEnd       -> OnMapEnd(): only drops state (PITFALLS B2 - no client call may follow it).
//   * OnPluginUpdate -> OnPluginUpdate(mapAvailable): a plugin loaded (or reloaded) into a running
//     map never saw OnMapStart, so its first update opens the gate - but only when the map
//     listeners are registered (without them nothing could ever close the gate again), no OnMapEnd
//     was seen since OnPluginStart (the server is between maps, or doing the boot-time changelevel
//     of PITFALLS B3), and a map name actually exists (`mapAvailable`, so a plugin that starts
//     before the very first map cannot open the gate on a server that has no map yet).
//   * OnPluginEnd    -> Close(): the library is going away, a stale callback must not reach a
//     client.

#pragma once

namespace playersinfo {

class MapGate {
public:
	// Whether s2sdk client/entity/schema calls are safe right now (the plugin's g_mapReady).
	[[nodiscard]] bool Ready() const { return m_ready; }

	// Whether an OnMapEnd arrived since OnPluginStart and no OnMapStart has followed it.
	[[nodiscard]] bool BetweenMaps() const { return m_betweenMaps; }

	// Whether OnMapStart_Register/OnMapEnd_Register succeeded.
	[[nodiscard]] bool ListenersRegistered() const { return m_listeners; }

	// OnPluginStart: a fresh start (including `plg reload`) begins with the gate shut.
	void Reset() { *this = MapGate(); }

	void SetListenersRegistered(bool registered) { m_listeners = registered; }

	// OnMapStart listener: the map and its entity system are in place.
	void OnMapStart() {
		m_ready = true;
		m_betweenMaps = false;
	}

	// OnMapEnd listener: from here until the next OnMapStart every s2sdk client query can crash,
	// so this only clears state - it must not touch a client or an entity (PITFALLS B2).
	void OnMapEnd() {
		m_ready = false;
		m_betweenMaps = true;
	}

	// OnPluginUpdate: the mid-map latch described above. `mapAvailable` is the caller's proof that
	// a map exists right now (src/plugin.cpp passes "GetCurrentMap() is not empty", the one s2sdk
	// map call that is safe before the first map). The first update that can prove it opens the
	// gate; a gate that is already open, a server between maps and a plugin without listeners
	// never open it here.
	void OnPluginUpdate(bool mapAvailable) {
		if (m_ready || m_betweenMaps || !m_listeners || !mapAvailable) {
			return;
		}
		m_ready = true;
	}

	// OnPluginEnd: nothing of this plugin may answer a callback after the library is unloaded.
	void Close() { m_ready = false; }

private:
	bool m_ready = false;
	bool m_betweenMaps = false;
	bool m_listeners = false;
};

} // namespace playersinfo
