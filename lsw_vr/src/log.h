#pragma once

// Appends a timestamped line to lsw_vr.log next to the game executable.
void Log(const char *format, ...);

// Sets the log file name (created next to the exe); call before the first Log.
// Defaults to lsw_vr.log.
void LogSetFileName(const char *name);
