cmake_minimum_required(VERSION 3.15)
project(m2dev-server-src)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ASan support
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

set(CMAKE_MODULE_PATH
	${CMAKE_MODULE_PATH}
	"${CMAKE_CURRENT_SOURCE_DIR}/buildtool"
)

set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/_install")

if(WIN32)
	set_property(GLOBAL PROPERTY USE_FOLDERS ON)
	
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	
	add_compile_options(
		$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>
		$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:preprocessor>
	)
	
	if (MSVC)
		add_compile_options(/MP)
	endif()
else()
	include_directories(${MARIADB_INCLUDE_DIR})
	
	add_compile_options("$<$<CXX_COMPILER_ID:GNU>:-finput-charset=UTF-8>")
endif()

add_compile_definitions(
  $<$<PLATFORM_ID:Windows>:OS_WINDOWS>
  $<$<PLATFORM_ID:FreeBSD>:OS_FREEBSD>
  $<$<PLATFORM_ID:Linux>:OS_LINUX>
  $<$<PLATFORM_ID:Darwin>:OS_MACOS>
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Build identity (cmake/BuildIdentity.cmake): computed at every build, not at configure time, so it always describes
# the source actually compiled. Only game/db version.cpp include the generated header. M2_BUILD_COMMIT/M2_BUILD_DIRTY
# inject an identity for builds with neither git nor an exported SOURCE_COMMIT (development only).
set(M2_BUILD_COMMIT "" CACHE STRING "Injected source commit (40 hex) when the source has no git and no SOURCE_COMMIT")
set(M2_BUILD_DIRTY "" CACHE STRING "Injected dirty flag (0 or 1) for M2_BUILD_COMMIT")
set(M2_BUILD_IDENTITY_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(m2_build_identity ALL
	COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -DBIN_DIR=${CMAKE_BINARY_DIR}
		-DOUT=${M2_BUILD_IDENTITY_DIR}/m2_build_identity.h
		-DM2_BUILD_COMMIT=${M2_BUILD_COMMIT} -DM2_BUILD_DIRTY=${M2_BUILD_DIRTY}
		-P ${CMAKE_SOURCE_DIR}/cmake/BuildIdentity.cmake
	BYPRODUCTS ${M2_BUILD_IDENTITY_DIR}/m2_build_identity.h
	COMMENT "M2 build identity"
	VERBATIM)

# ASan flags
if(ENABLE_ASAN)
	if(MSVC)
		add_compile_options(/fsanitize=address)
		add_link_options(/fsanitize=address)
		add_definitions(-D_DISABLE_VECTOR_ANNOTATION)
		add_definitions(-D_DISABLE_STRING_ANNOTATION)
	else()
		add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
		add_link_options(-fsanitize=address)
	endif()
endif()

add_definitions(-DNOMINMAX)
add_definitions(-DWIN32_LEAN_AND_MEAN)

include_directories(src)
include_directories(include)

# MariDB includes
include_directories(${CMAKE_BINARY_DIR}/vendor/mariadb-connector-c-3.4.5/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/vendor/mariadb-connector-c-3.4.5/include)

add_subdirectory(src)
add_subdirectory(vendor)
