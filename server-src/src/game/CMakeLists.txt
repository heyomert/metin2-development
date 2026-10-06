file(GLOB_RECURSE GAME_SOURCES "*.h" "*.c" "*.cpp")

add_executable(game ${GAME_SOURCES})

add_dependencies(game m2_build_identity)
target_include_directories(game PRIVATE ${M2_BUILD_IDENTITY_DIR})

target_link_libraries(game
	common
	libgame
	libpoly
	libsql
	libthecore
	liblua

	# external
	mariadbclient
	sodium
)

if (WIN32)
    target_link_libraries(game ws2_32)
    # Suppress C4244 from STL <utility> template instantiations with Lua number types
    target_compile_options(game PRIVATE /wd4244)
else()
    target_link_libraries(game pthread)
    if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_link_libraries(game md)
    endif()
endif()
