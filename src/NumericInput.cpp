#include "NumericInput.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace maskui {
namespace {

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

} // namespace

bool parseNumericInput(const std::string& text,
                       const NumericInputRules& rules,
                       double& value,
                       std::string& error) {
    error.clear();
    if (text.empty()) {
        return fail(error, "value must not be empty");
    }

    std::size_t consumed = 0;
    double parsed = 0.0;
    try {
        parsed = std::stod(text, &consumed);
    } catch (const std::invalid_argument&) {
        return fail(error, "value is not a number");
    } catch (const std::out_of_range&) {
        return fail(error, "value is outside the supported numeric range");
    }

    if (consumed != text.size()) {
        return fail(error, "value contains unsupported characters");
    }
    if (!std::isfinite(parsed)) {
        return fail(error, "value must be finite");
    }
    if (rules.positive && parsed <= 0.0) {
        return fail(error, "value must be positive");
    }
    if (rules.integer) {
        if (std::trunc(parsed) != parsed) {
            return fail(error, "value must be a whole number");
        }
        if (parsed < static_cast<double>(std::numeric_limits<int>::min()) ||
            parsed > static_cast<double>(std::numeric_limits<int>::max())) {
            return fail(error, "whole number is outside the int range");
        }
    }

    value = parsed;
    return true;
}

std::string editableNumber(double value, bool integer) {
    std::ostringstream output;
    if (integer) {
        output << static_cast<long long>(std::llround(value));
    } else {
        output << std::setprecision(
            std::numeric_limits<double>::max_digits10) << value;
    }
    return output.str();
}

} // namespace maskui
