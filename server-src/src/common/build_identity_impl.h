#pragma once

// Implementation of common/build_identity.h, included by exactly one file per binary (its version.cpp) after:
//   M2_COMPONENT            "game" / "db"
//   M2_VERSION_FILE         file written in the working directory at start-up
//   M2_VERSION_EXIT_ON_FAIL 1 to keep db's upstream behaviour (exit when the file cannot be opened)
// and the generated m2_build_identity.h (M2_BUILD_COMMIT, M2_BUILD_DIRTY, M2_BUILD_SRC, M2_BUILD_DESCRIBE).

#include "build_identity.h"
#include "libthecore/log.h"

#include <cstdio>
#include <cstdlib>

#if defined(__GNUC__) || defined(__clang__)
#define M2_BUILD_KEEP __attribute__((used))
#else
#define M2_BUILD_KEEP
#endif

// Read from the file by the installer (`strings`) without running the binary: the identity this binary claims for
// itself. Not a signature; deploy.log ties it to the exact file by SHA-256 (docs/build-and-run.md).
extern "C" M2_BUILD_KEEP const char g_m2BuildMarker[] =
	"M2BUILD|component=" M2_COMPONENT "|commit=" M2_BUILD_COMMIT "|dirty=" M2_BUILD_DIRTY "|src=" M2_BUILD_SRC
	"|describe=" M2_BUILD_DESCRIBE "|END";

const char* M2BuildFields()
{
	return " build=" M2_BUILD_COMMIT " build_dirty=" M2_BUILD_DIRTY " build_src=" M2_BUILD_SRC;
}

void WriteVersion()
{
	sys_log(0, "BUILD: component=%s commit=%s dirty=%s src=%s describe=%s", M2_COMPONENT, M2_BUILD_COMMIT,
		M2_BUILD_DIRTY, M2_BUILD_SRC, M2_BUILD_DESCRIBE);

#ifndef OS_WINDOWS
	FILE* fp = fopen(M2_VERSION_FILE, "w");

	if (fp)
	{
		fprintf(fp, "component=%s commit=%s dirty=%s src=%s describe=%s\n", M2_COMPONENT, M2_BUILD_COMMIT,
			M2_BUILD_DIRTY, M2_BUILD_SRC, M2_BUILD_DESCRIBE);
		fclose(fp);
	}
	else if (M2_VERSION_EXIT_ON_FAIL)
	{
		fprintf(stderr, "cannot open %s\n", M2_VERSION_FILE);
		exit(0);
	}
#endif
}
