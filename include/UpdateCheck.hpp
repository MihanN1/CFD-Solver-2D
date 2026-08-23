#pragma once
#include <string>

namespace update {

struct Result {
    bool checked = false;      // the request actually completed
    bool newer = false;        // ... and it found something newer
    std::string latest;        // the newest tag seen, without the leading "v"
    std::string url;           // where to get it
    std::string error;         // why "checked" is false, for --check-updates
};

int compareVersions(const std::string& a, const std::string& b);

Result check(int timeoutSeconds = 4);

// Hands the URL to the browser. Best effort, and never blocks.
bool openUrl(const std::string& url);
void runStartupCheck(bool interactive);

}   // namespace update
