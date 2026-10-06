#pragma once

// Build identity (cmake/BuildIdentity.cmake, docs/build-and-run.md -> "Derleme kimliği"). Defined once per binary,
// in its version.cpp, the only file that includes the generated identity header; everything else asks these.

// Writes version.txt (game) / VERSION.txt (db) as key=value and one "BUILD:" syslog line. Called at start-up.
void WriteVersion();

// " build=<commit> build_dirty=<0|1|unknown> build_src=<git|archive|injected|none>": appended to telemetry lines so
// every window names the binary that produced it.
const char* M2BuildFields();
