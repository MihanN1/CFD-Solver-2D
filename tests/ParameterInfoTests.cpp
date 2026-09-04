#include "ParameterInfo.hpp"

#include <cctype>
#include <iostream>
#include <set>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cout << message << "\n";
    return 1;
}

} // namespace

int main() {
    using namespace maskui;

    std::set<std::string> keys;
    for (std::size_t index = 0; index < ParameterCount; ++index) {
        const std::string key = parameterKey(index);
        if (key.empty())
            return fail("parameter " + std::to_string(index) +
                        " has no solver key, so it can never be saved or "
                        "loaded");
        if (!keys.insert(key).second)
            return fail("two parameters answer to the key '" + key +
                        "', and a .cfdui file cannot tell them apart");
    }

    for (std::size_t index = 0; index < ParameterCount; ++index) {
        const std::string hint = parameterHint(index);
        if (hint.empty())
            return fail("parameter '" + std::string(parameterKey(index)) +
                        "' has no hover hint. Every row gets one - that is "
                        "the whole point of the tooltip");
        if (hint.size() < 20)
            return fail("the hint for '" + std::string(parameterKey(index)) +
                        "' is " + std::to_string(hint.size()) +
                        " characters, which is a label rather than an "
                        "explanation");
        if (hint.size() > 200)
            return fail("the hint for '" + std::string(parameterKey(index)) +
                        "' is " + std::to_string(hint.size()) +
                        " characters. A tooltip is one or two lines; the long "
                        "version belongs in parameterHelp");
        if (std::isspace(static_cast<unsigned char>(hint.front())) ||
            std::isspace(static_cast<unsigned char>(hint.back())))
            return fail("the hint for '" + std::string(parameterKey(index)) +
                        "' has whitespace on one end");
        const char last = hint.back();
        if (last != '.' && last != '!' && last != '?')
            return fail("the hint for '" + std::string(parameterKey(index)) +
                        "' does not end in a full stop");
        if (hint.find("  ") != std::string::npos)
            return fail("the hint for '" + std::string(parameterKey(index)) +
                        "' has a double space in it");
    }

    for (std::size_t index = 0; index < PARAMETER_GROUPS.size(); ++index) {
        if (PARAMETER_GROUPS[index].firstIndex >= ParameterCount)
            return fail("group '" + std::string(PARAMETER_GROUPS[index].label) +
                        "' starts past the end of the parameter list");
        if (index > 0 &&
            PARAMETER_GROUPS[index].firstIndex <=
                PARAMETER_GROUPS[index - 1].firstIndex)
            return fail("the parameter groups are not in ascending order, and "
                        "the panel walks them assuming they are");
    }

    for (std::size_t tab = 1; tab < PARAMETER_TABS.size(); ++tab)
        for (int group : PARAMETER_TABS[tab].groups) {
            if (group < 0)
                continue;
            if (static_cast<std::size_t>(group) >= PARAMETER_GROUPS.size())
                return fail("tab '" + std::string(PARAMETER_TABS[tab].label) +
                            "' points at a group that does not exist");
        }

    std::set<int> covered;
    for (std::size_t tab = 1; tab < PARAMETER_TABS.size(); ++tab)
        for (int group : PARAMETER_TABS[tab].groups)
            if (group >= 0 && !covered.insert(group).second)
                return fail("group " + std::to_string(group) +
                            " is on two tabs at once");
    for (std::size_t group = 0; group < PARAMETER_GROUPS.size(); ++group)
        if (covered.find(static_cast<int>(group)) == covered.end())
            return fail("group '" +
                        std::string(PARAMETER_GROUPS[group].label) +
                        "' is on no tab but All, so those rows are only "
                        "reachable one way");

    std::cout << "ParameterInfoTests OK\n";
    return 0;
}
