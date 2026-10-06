# Build identity (roadmap 1.9 / T-2): which source a game/db binary was built from. Runs as a script at every build
# (cmake -P), never only at configure time, and rewrites the generated header only when the identity changed, so a new
# commit recompiles the version file alone and a no-op build recompiles nothing.
#
#   cmake -DSRC_DIR=<server-src> -DBIN_DIR=<build dir> -DOUT=<header>
#         [-DM2_BUILD_COMMIT=<40 hex> -DM2_BUILD_DIRTY=0|1] -P BuildIdentity.cmake
#
# Sources, first match wins (docs/build-and-run.md -> "Derleme kimliği"):
#   git      SRC_DIR is in a git work tree: commit = HEAD; dirty = anything git reports under SRC_DIR, untracked and
#            ignored files included (GLOB_RECURSE compiles ignored files too, e.g. src/game/old_BK/x.cpp), except this
#            build's own directory when it lies inside the tree. The only source production accepts (with dirty=0).
#   archive  SRC_DIR/SOURCE_COMMIT holds a 40-hex commit, written by `git archive` (export-subst). Clean when exported;
#            changes made after extraction are NOT detected, so it is never production provenance.
#   injected M2_BUILD_COMMIT given explicitly: an unverified claim, development only.
#   none     nothing above: commit/dirty "unknown", a visible warning; the build continues.

cmake_minimum_required(VERSION 3.16)

foreach(var SRC_DIR BIN_DIR OUT)
	if(NOT DEFINED ${var})
		message(FATAL_ERROR "BuildIdentity.cmake: ${var} is required")
	endif()
endforeach()

# CMake regular expressions have no {n} repetition: build "exactly 40 lowercase hex digits" explicitly
string(REPEAT "[0-9a-f]" 40 HEX40)

set(commit "unknown")
set(dirty "unknown")
set(src "none")
set(describe "")

find_program(GIT_BIN git)
if(GIT_BIN)
	execute_process(COMMAND "${GIT_BIN}" -C "${SRC_DIR}" rev-parse --show-toplevel
		RESULT_VARIABLE rc OUTPUT_VARIABLE top ERROR_VARIABLE err OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(rc EQUAL 0)
		execute_process(COMMAND "${GIT_BIN}" -C "${SRC_DIR}" rev-parse HEAD
			RESULT_VARIABLE rc OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE)
		# `-- .` with -C SRC_DIR: the pathspec is relative to SRC_DIR whatever the caller's directory is
		execute_process(COMMAND "${GIT_BIN}" -C "${SRC_DIR}" status --porcelain=v1 --untracked-files=all --ignored=matching -- .
			RESULT_VARIABLE rc2 OUTPUT_VARIABLE status)
		if(rc EQUAL 0 AND rc2 EQUAL 0 AND head MATCHES "^[0-9a-f]+$")
			# This build's own directory, if inside the work tree, is the one thing allowed to be there
			file(RELATIVE_PATH bin_rel "${top}" "${BIN_DIR}")
			string(REGEX REPLACE "\r" "" status "${status}")
			string(REPLACE "\n" ";" lines "${status}")
			set(changes 0)
			foreach(line IN LISTS lines)
				if(line STREQUAL "")
					continue()
				endif()
				string(SUBSTRING "${line}" 3 -1 path)
				string(REGEX REPLACE "^\"" "" path "${path}")
				if(NOT bin_rel STREQUAL "" AND NOT bin_rel MATCHES "^\\.\\./" AND NOT IS_ABSOLUTE "${bin_rel}")
					string(LENGTH "${bin_rel}/" n)
					string(SUBSTRING "${path}" 0 ${n} head_of_path)
					if(head_of_path STREQUAL "${bin_rel}/")
						continue()
					endif()
				endif()
				math(EXPR changes "${changes} + 1")
			endforeach()
			set(commit "${head}")
			if(changes EQUAL 0)
				set(dirty "0")
			else()
				set(dirty "1")
			endif()
			set(src "git")
			execute_process(COMMAND "${GIT_BIN}" -C "${SRC_DIR}" describe --tags --always --abbrev=12
				OUTPUT_VARIABLE describe OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
		else()
			message(WARNING "M2 build identity: git work tree found but HEAD/status could not be read; not using git")
		endif()
	elseif(NOT err MATCHES "not a git repository")
		# e.g. "detected dubious ownership": never fall back silently, the build would look like it has no git
		string(STRIP "${err}" err)
		message(WARNING "M2 build identity: git failed in ${SRC_DIR}: ${err}")
	endif()
endif()

if(src STREQUAL "none" AND EXISTS "${SRC_DIR}/SOURCE_COMMIT")
	file(READ "${SRC_DIR}/SOURCE_COMMIT" exported)
	string(STRIP "${exported}" exported)
	if(exported MATCHES "^${HEX40}$")
		set(commit "${exported}")
		set(dirty "0")
		set(src "archive")
	elseif(NOT exported STREQUAL "$Format:%H$")
		message(WARNING "M2 build identity: SOURCE_COMMIT is not a 40-hex commit (\"${exported}\"); ignored")
	endif()
endif()

if(DEFINED M2_BUILD_COMMIT AND NOT M2_BUILD_COMMIT STREQUAL "")
	if(NOT src STREQUAL "none")
		message(WARNING "M2 build identity: M2_BUILD_COMMIT ignored, identity comes from ${src}")
	elseif(M2_BUILD_COMMIT MATCHES "^${HEX40}$")
		set(commit "${M2_BUILD_COMMIT}")
		set(src "injected")
		if(DEFINED M2_BUILD_DIRTY AND M2_BUILD_DIRTY MATCHES "^[01]$")
			set(dirty "${M2_BUILD_DIRTY}")
		endif()
	else()
		message(WARNING "M2 build identity: M2_BUILD_COMMIT is not a 40-hex commit; ignored")
	endif()
endif()

if(src STREQUAL "none")
	message(WARNING "M2 build identity: UNKNOWN (no git work tree, no exported SOURCE_COMMIT, nothing injected). "
		"The binary reports commit=unknown and production installs reject it.")
endif()

string(REGEX REPLACE "[^A-Za-z0-9._+-]" "_" describe "${describe}")
set(content
	"// Generated by cmake/BuildIdentity.cmake at build time; do not edit.\n"
	"#pragma once\n"
	"#define M2_BUILD_COMMIT \"${commit}\"\n"
	"#define M2_BUILD_DIRTY \"${dirty}\"\n"
	"#define M2_BUILD_SRC \"${src}\"\n"
	"#define M2_BUILD_DESCRIBE \"${describe}\"\n")
string(CONCAT content ${content})
set(previous "")
if(EXISTS "${OUT}")
	file(READ "${OUT}" previous)
endif()
if(NOT previous STREQUAL content)
	# FreeBSD's make (bmake) compares modification times in whole seconds: a header rewritten in the same second as
	# the version.cpp object of the previous build looks "not newer" and that object, with the old identity, is kept
	# (measured 2026-10-06). Write a changed header only after the current second has passed. Unchanged identity:
	# nothing is written and nothing waits.
	string(TIMESTAMP started "%s" UTC)
	string(TIMESTAMP now "%s" UTC)
	while(now STREQUAL started)
		execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
		string(TIMESTAMP now "%s" UTC)
	endwhile()
	get_filename_component(out_dir "${OUT}" DIRECTORY)
	file(MAKE_DIRECTORY "${out_dir}")
	file(WRITE "${OUT}" "${content}")
endif()
message(STATUS "M2 build identity: commit=${commit} dirty=${dirty} src=${src}")
