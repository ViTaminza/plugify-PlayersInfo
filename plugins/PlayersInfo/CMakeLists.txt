# PlayersInfo — Plugify C++ port of the Metamod:Source plugin "PlayersInfo": the
# server console command `mm_getinfo` prints a compact JSON document (map, team scores and one
# object per connected player) that the example.com web module polls over RCON.
#
#   plugify-PlayersInfo     the plugin shared library (entry "bin/PlayersInfo" -> bin/libPlayersInfo.so)
#   PlayersInfo_tests       the JSON/format/guard unit tests, run by ../test.sh
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The map gate, the
# client/controller checks, the map-gated schema offsets and the command source dispatch live in
# src/plugin.cpp; the engine-free parts of them (src/json_writer.hpp, src/ent_handle.hpp,
# src/map_gate.hpp, src/connection_times.hpp) are what the tests compile in.
#
# Links the generated levels_ranks consumer bindings (optional dependency: the "rank" field needs
# the levels_ranks plugin) and resolves the Steam game server API through src/steam_gameserver.cpp
# (dlsym, hence ${CMAKE_DL_LIBS}) for the "prime" field.

set(PLAYERSINFO_VERSION "1.1.0")

# The generated levels_ranks binding globals (__levels_ranks_*) have to be compiled into every
# consumer plugin. plugins/levels_ranks builds them as the object library `lr_bindings`, but
# "plugins/PlayersInfo" sorts before "plugins/levels_ranks" (a capital P sorts before a lowercase
# l), so that target usually does not exist yet: this is the standalone fallback, named
# differently on purpose so it can never collide with plugins/levels_ranks' own target.
set(PLAYERSINFO_LR_BINDINGS "")
if(TARGET lr_bindings)
    set(PLAYERSINFO_LR_BINDINGS "$<TARGET_OBJECTS:lr_bindings>")
elseif(EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(levels_ranks_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(levels_ranks_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(levels_ranks_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(PLAYERSINFO_LR_BINDINGS "$<TARGET_OBJECTS:levels_ranks_bindings_obj>")
else()
    message(FATAL_ERROR "PlayersInfo needs the generated levels_ranks consumer bindings (plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp)")
endif()

add_library(plugify-PlayersInfo SHARED
        src/plugin.cpp
        src/steam_gameserver.cpp
        ${PLAYERSINFO_LR_BINDINGS}
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-PlayersInfo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated s2sdk/levels_ranks/plg headers are not ours to fix; their warnings must not
# fail -Werror on our sources.
target_include_directories(plugify-PlayersInfo SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-PlayersInfo PRIVATE
        plugify_sdk
        ${CMAKE_DL_LIBS}
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-PlayersInfo PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-PlayersInfo PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="PlayersInfo"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-PlayersInfo PROPERTIES OUTPUT_NAME PlayersInfo)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-PlayersInfo PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/PlayersInfo.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PlayersInfo.pplugin"
)

# The JSON writer, the chunked console output, the entity-handle rule, the map gate, the connection
# table and the mm_getinfo registration/teardown are plain code, so the document format and the
# safety rules can be tested without an engine or pisexcompat. The tests define the four s2sdk
# bindings they exercise themselves (fake __s2sdk_AddConsoleCommand/__s2sdk_RemoveCommand/
# __s2sdk_RemoveCommandListener/__s2sdk_PrintToServer), so only the headers have to be reachable -
# nothing links s2sdk_bindings or the plugify host. nlohmann (sdk/include) is used to parse the
# chunked document back, exactly like the web poller does.
add_executable(PlayersInfo_tests tests/PlayersInfo_tests.cpp)
target_include_directories(PlayersInfo_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on the test source.
target_include_directories(PlayersInfo_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(PlayersInfo_tests PRIVATE -static-libstdc++ -static-libgcc)
target_compile_options(PlayersInfo_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(PlayersInfo_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(PlayersInfo_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# steamlink_tests: the real src/steam_gameserver.cpp against a fake dlsym() - proves the interface
# comes from the official SteamAPI_SteamGameServer_v015 accessor (never from a zero-initialised
# SteamInternal_ContextInit context, which crashed the live server), that the license wrapper
# receives a valid interface pointer, and that a missing symbol / a not-yet-up interface fails soft.
# The test defines dlsym() itself, so it needs neither libdl nor libsteam_api.
add_executable(steamlink_tests tests/steamlink_tests.cpp src/steam_gameserver.cpp)
target_include_directories(steamlink_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(steamlink_tests PRIVATE -static-libstdc++ -static-libgcc)
target_compile_options(steamlink_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(steamlink_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(steamlink_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
