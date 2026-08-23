#pragma once
#include <string>

// "Is there a newer release than this one?", asked once at startup.
//
// The rule is the plain one: any published version greater than the one this
// binary was built as counts, whether it moved the major or only the minor -
// 0.2 sees 0.3, 0.10 and 1.0 alike, and does not see 0.2 or 0.1.9. The version
// is compared number by number, so 0.10 is correctly newer than 0.9 rather
// than alphabetically older.
//
// Nothing here is allowed to hold the program up: the request has a short
// timeout, every failure is silent unless asked about, and no network at all
// is the normal case rather than an error.

namespace update {

struct Result {
    bool checked = false;      // the request actually completed
    bool newer = false;        // ... and it found something newer
    std::string latest;        // the newest tag seen, without the leading "v"
    std::string url;           // where to get it
    std::string error;         // why "checked" is false, for --check-updates
};

// -1 when a < b, 0 when equal, 1 when a > b. Missing components count as zero,
// so "0.2" == "0.2.0", and anything after the numbers ("0.3-beta") is ignored
// for ordering but keeps the tag itself intact.
int compareVersions(const std::string& a, const std::string& b);

// One HTTPS GET against the releases API. timeoutSeconds covers the whole
// exchange; on Windows this is WinHTTP, elsewhere curl or wget, whichever is
// on the machine.
Result check(int timeoutSeconds = 4);

// Hands the URL to the browser. Best effort, and never blocks.
bool openUrl(const std::string& url);

// The startup path: honours the "checkForUpdates" setting and the
// FLUID_SOLVER_NO_UPDATE_CHECK environment variable, prints one line when
// something newer exists and - when a person is at the keyboard - offers to
// open the release page. A non-interactive run only prints.
void runStartupCheck(bool interactive);

}   // namespace update
