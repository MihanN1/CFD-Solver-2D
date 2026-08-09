#pragma once

#include <string>

namespace maskui {

struct NumericInputRules {
    bool integer = false;
    bool positive = false;
};

bool parseNumericInput(const std::string& text,
                       const NumericInputRules& rules,
                       double& value,
                       std::string& error);

std::string editableNumber(double value, bool integer);

} // namespace maskui
