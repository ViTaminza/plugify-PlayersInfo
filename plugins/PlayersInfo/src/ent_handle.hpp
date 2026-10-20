// ent_handle — entity-handle validity for the raw schema reads of the plugin.
//
// The controller reads of src/plugin.cpp (ControllerInt, ControllerHasPawn) go through s2sdk with
// an entity *handle*, so the value rule lives here (tested by tests/PlayersInfo_tests.cpp).
//
// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntIndexToEntHandle
// answers -1 for "no entity"). The sign means nothing - once the engine's serial counter passes
// 65535 bit 31 is set and a valid handle is a negative int; a `h > 0` test silently drops those
// entities. This matches pisexcompat's EntHandleToEntIndex, which treats exactly -1 and 0 as
// "none".

#pragma once

#include <cstdint>

namespace playersinfo {

inline bool IsValidHandle(std::int32_t h) { return h != -1 && h != 0; }

} // namespace playersinfo
