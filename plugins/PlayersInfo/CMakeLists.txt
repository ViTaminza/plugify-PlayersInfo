# PlayersInfo — Plugify C++ port of the Metamod:Source plugin "PlayersInfo": the
# server console command `mm_getinfo` prints a compact JSON document (map, team scores and one
# object per connected player) that the example.com web module polls over RCON.
#
#   plugify-PlayersInfo     the plugin shared library (entry "bin/PlayersInfo" -> bin/libPlayersInfo.so)
#   PlayersInfo_tests       the JSON/format unit tests, run by ../test.sh
#
# Links pisexcompat (players/events/schema on s2sdk), the generated levels_ranks consumer bindings
# (optional dependency: the "rank" field needs the levels_ranks plugin) and resolves the Steam game
# server API through src/steam_gameserver.cpp for the "prime" field.

set(PLAYERSINFO_VERSION "1.0.2")

# The generated levels_ranks binding globals (__levels_ranks_*) have to be compiled into every
# consumer plugin; the levels_ranks plugin directory already builds them as an object library (it
# is added before plugins/PlayersInfo), this is the fallback for a build of this directory alone.
if(NOT TARGET levels_ranks_bindings_obj)
    add_library(levels_ranks_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(levels_ranks_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(levels_ranks_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-PlayersInfo SHARED
        src/plugin.cpp
        src/steam_gameserver.cpp
        $<TARGET_OBJECTS:levels_ranks_bindings_obj>
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-PlayersInfo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated s2sdk/levels_ranks/plg headers and pisexcompat are not ours to fix; their
# warnings must not fail -Werror on our sources.
target_include_directories(plugify-PlayersInfo SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-PlayersInfo PRIVATE
        pisexcompat
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

# The JSON writer is pure string code, so the document format can be tested without an engine.
add_executable(PlayersInfo_tests tests/PlayersInfo_tests.cpp)
target_include_directories(PlayersInfo_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(PlayersInfo_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(PlayersInfo_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(PlayersInfo_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
