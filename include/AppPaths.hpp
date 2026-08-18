#pragma once
#include <filesystem>
#include <string>

// Everything the program writes or looks for by a bare name is relative to the
// executable, not to whatever directory it happened to be started from. A
// desktop shortcut, a file manager and a terminal all hand the process a
// different working directory, so "output" meant three different folders and
// the frames went wherever the launcher felt like.

// Directory holding the running executable. Falls back to the working
// directory if the platform refuses to say, which no supported one does.
std::filesystem::path executableDir();

// Where the frames go. An absolute outputDir is taken as given; a relative one
// (the default "output") hangs off executableDir(). Empty means the executable's
// own directory. Creates it, and if that fails - an install under Program Files
// is not writable by a standard user - falls back to the per-user data
// directory and says which path it settled on.
std::filesystem::path resolveOutputDir(const std::string& outputDir);

// Per-user writable location, used only as the fallback above.
// %LOCALAPPDATA% on Windows, $XDG_DATA_HOME or ~/.local/share elsewhere,
// ~/Library/Application Support on macOS.
std::filesystem::path userDataDir();
