file(GLOB_RECURSE DB_SOURCES "*.h" "*.cpp")

add_executable(db ${DB_SOURCES})

add_dependencies(db m2_build_identity)
target_include_directories(db PRIVATE ${M2_BUILD_IDENTITY_DIR})

target_link_libraries(db 
	common
	libgame 
	libpoly 
	libsql 
	libthecore

	# external
	mariadbclient
)

if (WIN32)
    target_link_libraries(db ws2_32)
else()
    target_link_libraries(db pthread)
    if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_link_libraries(db md)
    endif()
endif()
