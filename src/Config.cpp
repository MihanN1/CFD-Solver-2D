#include "Config.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// --- values, wherever they come from ---------------------------------------
// strtof and atoi report nothing at all: "nu=0,002" quietly became 0.0 and the
// run went on inviscid, "useCuda=true" turned CUDA off. std::cin >> value was
// worse: it set failbit, left the junk in the buffer and every prompt after it
// answered itself. Everything below refuses the value instead and says how it
// should have been written.

constexpr double kTiny = 1e-30;            // "anything except zero"
constexpr double kHuge = 1e30;             // "no upper limit worth naming"
constexpr double kIntMax = 2147483647.0;

// Every key the command line, the prompts and the frame header accept, in the
// spelling print() and serialize() use. Keep in sync with printUsage().
const char* const kKeys[] = {
    "Lx", "Ly", "nx", "ny", "U0", "nu", "ro",
    "gravityEnabled", "gravityAccel", "gravityAngle",
    "CFL", "totalTime", "dtUpdateInterval", "dtSafety",
    "omega", "smootherOmega",
    "mgIterations", "mgTolerance", "mgMinCoarseSize",
    "useCuda", "saveInterval", "outputDir",
    "geometryFile", "sliceAngleX", "sliceAngleZ", "sliceRotation",
    "invertSection", "wallMotion",
    "restart", "restartFile", "addTime",
};

std::string trimSpace(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// argv on Windows can keep the quotes the shell did not eat, and a path typed
// into a prompt usually arrives with them too.
std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

std::string cleanValue(const std::string& raw) {
    return trimSpace(stripQuotes(trimSpace(raw)));
}

std::string badValue(const std::string& key,
                     const std::string& value,
                     const std::string& why) {
    return "not a right way to write " + key + "=" + value + ": " + why;
}

bool parseNumber(const std::string& key, const std::string& raw, bool integer,
                 double& out, std::string& error) {
    const std::string text = cleanValue(raw);

    if (text.empty()) {
        error = badValue(key, text,
                         "the value is missing, write " + key + "=<number>");
        return false;
    }
    if (text.find(',') != std::string::npos) {
        std::string dotted = text;
        std::replace(dotted.begin(), dotted.end(), ',', '.');
        error = badValue(key, text,
                         "the decimal separator is a dot, write " + key + "=" +
                         dotted);
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);

    if (end == text.c_str()) {
        error = badValue(key, text, "that is not a number");
        return false;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (*end != '\0') {
        error = badValue(key, text,
                         std::string("'") + end + "' is stuck to the number; "
                         "units and extra characters are not part of the value");
        return false;
    }
    if (errno == ERANGE || !std::isfinite(parsed)) {
        error = badValue(key, text, "that number is out of range");
        return false;
    }
    if (integer) {
        double whole = 0.0;
        if (std::modf(parsed, &whole) != 0.0) {
            error = badValue(key, text,
                             key + " counts things, so it has to be a whole "
                             "number");
            return false;
        }
        if (parsed < -kIntMax || parsed > kIntMax) {
            error = badValue(key, text, "that number is out of range");
            return false;
        }
    }
    out = parsed;
    return true;
}

bool inRange(const std::string& key, const std::string& raw, double value,
             double lo, double hi, const char* rule, std::string& error) {
    if (value >= lo && value <= hi)
        return true;
    error = badValue(key, cleanValue(raw), rule);
    return false;
}

bool assignFloat(float& target, const std::string& key, const std::string& raw,
                 double lo, double hi, const char* rule, std::string& error) {
    double v = 0.0;
    if (!parseNumber(key, raw, false, v, error)) return false;
    if (!inRange(key, raw, v, lo, hi, rule, error)) return false;
    target = static_cast<float>(v);
    return true;
}

bool assignDouble(double& target, const std::string& key, const std::string& raw,
                  double lo, double hi, const char* rule, std::string& error) {
    double v = 0.0;
    if (!parseNumber(key, raw, false, v, error)) return false;
    if (!inRange(key, raw, v, lo, hi, rule, error)) return false;
    target = v;
    return true;
}

bool assignInt(int& target, const std::string& key, const std::string& raw,
               double lo, double hi, const char* rule, std::string& error) {
    double v = 0.0;
    if (!parseNumber(key, raw, true, v, error)) return false;
    if (!inRange(key, raw, v, lo, hi, rule, error)) return false;
    target = static_cast<int>(v);
    return true;
}

bool assignBool(bool& target, const std::string& key, const std::string& raw,
                std::string& error) {
    const std::string text = toLower(cleanValue(raw));
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        target = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        target = false;
        return true;
    }
    error = badValue(key, cleanValue(raw),
                     key + " is a switch, write " + key + "=1 or " + key +
                     "=0 (true/false, yes/no and on/off work too)");
    return false;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}
}

std::string wallMotionHelp() {
    return
        "\n"
        "--- How to write wallMotion --------------------------------------\n"
        "Shape of the line:   <object>:<setting>=<value>,<setting>=<value>\n"
        "                     and ';' in front of the next object's number\n"
        "  Object numbers are printed with the mesh, right after this screen.\n"
        "  ',' and ';' both just separate settings; a new object starts\n"
        "  wherever a number and a colon appear, and each object is listed\n"
        "  once, with everything it does inside that one entry.\n"
        "\n"
        "Every object picks ONE of the two groups below.\n"
        "\n"
        "  A. The wall holds the fluid (no-slip) and now drags it along:\n"
        "     rot=<deg/s>    the surface turns about this object's own centre,\n"
        "                    counter-clockwise. rot=90 is a quarter turn a\n"
        "                    second; a spinning cylinder, a blade, a valve\n"
        "     slideX=<m/s>   the surface runs along +x, like a conveyor belt\n"
        "     slideY=<m/s>   the same along +y\n"
        "     The body itself never moves, only the velocity its surface hands\n"
        "     to the fluid. rot, slideX and slideY add up, so one object can\n"
        "     take all three at once.\n"
        "\n"
        "  B. The wall stops holding the fluid at all:\n"
        "     slip=1         free-slip. The fluid slides past the surface and\n"
        "                    the wall exerts no drag on it, so no boundary\n"
        "                    layer grows. slip=0 is the default no-slip wall\n"
        "\n"
        "  A and B cannot be mixed on the same object: rot and slide push the\n"
        "  fluid through the grip that slip=1 removes, so 'slip=1,rot=90' asks\n"
        "  for a surface that turns and touches nothing. That is refused.\n"
        "\n"
        "Empty line = every wall stands still and holds the fluid (no-slip).\n"
        "\n"
        "Examples:\n"
        "  1:rot=90              object 1 spins at 90 deg/s counter-clockwise\n"
        "  1:slideX=0.5          its surface runs along +x at 0.5 m/s\n"
        "  1:rot=90,slideX=0.5   both at once - one object, one entry\n"
        "  1:slip=1              object 1 is frictionless instead\n"
        "  1:rot=90;2:slip=1     object 1 spins, object 2 slips\n"
        "------------------------------------------------------------------\n";
}

bool parseWallMotion(const std::string& text,
                     std::vector<WallMotion>& out,
                     std::string& error) {
    out.clear();
    error.clear();

    const std::string body = cleanValue(text);
    if (body.empty())
        return true;

    std::vector<std::string> tokens;
    for (size_t pos = 0; pos < body.size();) {
        size_t end = body.find_first_of(",;", pos);
        if (end == std::string::npos)
            end = body.size();
        tokens.push_back(trimSpace(body.substr(pos, end - pos)));
        pos = end + 1;
    }

    int current = -1;
    for (std::string token : tokens) {
        if (token.empty())
            continue;

        const size_t colon = token.find(':');
        if (colon != std::string::npos) {
            const std::string idText = trimSpace(token.substr(0, colon));
            double id = 0.0;
            std::string why;
            if (!parseNumber("object", idText, true, id, why) || id < 1.0) {
                error = badValue("wallMotion", body,
                                 "'" + idText + "' is not an object number; "
                                 "objects are numbered from 1 and the number "
                                 "comes first, e.g. 1:rot=90");
                return false;
            }
            for (const WallMotion& done : out) {
                if (done.object != static_cast<int>(id))
                    continue;
                error = badValue("wallMotion", body,
                                 "object " + idText + " is listed twice. One "
                                 "object gets one entry, with everything it "
                                 "does inside it: write " + idText +
                                 ":rot=90,slideX=0.5, not " + idText +
                                 ":rot=90;" + idText + ":slideX=0.5");
                return false;
            }
            out.push_back(WallMotion());
            out.back().object = static_cast<int>(id);
            current = static_cast<int>(out.size()) - 1;
            token = trimSpace(token.substr(colon + 1));
            if (token.empty())
                continue;
        }

        if (current < 0) {
            error = badValue("wallMotion", body,
                             "'" + token + "' comes before any object number. "
                             "The line starts with the object it is about and "
                             "a colon, so this reads 1:" + token +
                             " if object 1 is the one meant");
            return false;
        }

        const size_t eq = token.find('=');
        if (eq == std::string::npos || eq == 0) {
            double stray = 0.0;
            std::string why;
            const bool bareNumber =
                parseNumber("wallMotion", token, false, stray, why);
            error = badValue("wallMotion", body,
                             bareNumber
                                 ? "'" + token + "' is a bare number; a comma "
                                   "separates settings here, so the decimal "
                                   "separator inside one is a dot"
                                 : "'" + token + "' is not a setting. Every "
                                   "setting is name=value, e.g. rot=90, and "
                                   "the names are rot, slideX, slideY and "
                                   "slip");
            return false;
        }

        const std::string name = toLower(trimSpace(token.substr(0, eq)));
        const std::string value = token.substr(eq + 1);

        if (name == "slip") {
            if (!assignBool(out[current].slip, name, value, error))
                return false;
            continue;
        }

        double parsed = 0.0;
        if (!parseNumber(name, value, false, parsed, error))
            return false;

        if (name == "rot" || name == "rotation")
            out[current].rotation = static_cast<float>(parsed);
        else if (name == "slidex")
            out[current].slideX = static_cast<float>(parsed);
        else if (name == "slidey")
            out[current].slideY = static_cast<float>(parsed);
        else {
            error = badValue("wallMotion", body,
                             "'" + name + "' is not a wall setting. There are "
                             "four: rot=<deg/s> spins the surface about the "
                             "object's own centre counter-clockwise, "
                             "slideX=<m/s> and slideY=<m/s> drag it in a "
                             "straight line, and slip=1 makes the wall "
                             "frictionless instead of dragging anything");
            return false;
        }
    }

    for (const WallMotion& done : out) {
        if (!done.slip)
            continue;
        if (done.rotation == 0.0f && done.slideX == 0.0f && done.slideY == 0.0f)
            continue;

        // Quoting back the two lines the object could have been given beats
        // naming the rule: whichever of them was meant can be copied straight
        // into the answer.
        const std::string id = std::to_string(done.object);
        std::ostringstream moving;
        moving << id << ":";
        const char* separator = "";
        if (done.rotation != 0.0f) {
            moving << "rot=" << done.rotation;
            separator = ",";
        }
        if (done.slideX != 0.0f) {
            moving << separator << "slideX=" << done.slideX;
            separator = ",";
        }
        if (done.slideY != 0.0f)
            moving << separator << "slideY=" << done.slideY;

        error = badValue("wallMotion", body,
                         "object " + id + " is asked to slip and to move its "
                         "surface at once, and those are opposites. A moving "
                         "wall pushes the fluid by holding on to it, and "
                         "slip=1 is exactly the setting that lets go, so "
                         "together they leave a surface that turns and touches "
                         "nothing. Keep one of the two: " + id + ":slip=1 for "
                         "a frictionless wall, or " + moving.str() + " for a "
                         "wall that drags the flow");
        return false;
    }

    return true;
}

std::string Config::canonicalKey(const std::string& key) {
    std::string name = cleanValue(key);
    while (!name.empty() && (name.front() == '-' || name.front() == '/'))
        name.erase(name.begin());

    const std::string lower = toLower(name);
    for (const char* known : kKeys)
        if (toLower(known) == lower)
            return known;
    return std::string();
}

std::string Config::suggestKey(const std::string& key) {
    const std::string exact = canonicalKey(key);
    if (!exact.empty())
        return exact;

    std::string name = cleanValue(key);
    while (!name.empty() && (name.front() == '-' || name.front() == '/'))
        name.erase(name.begin());
    const std::string lower = toLower(name);
    if (lower.empty())
        return std::string();

    std::string best;
    int bestDistance = 0;
    for (const char* known : kKeys) {
        const int d = editDistance(lower, toLower(known));
        if (best.empty() || d < bestDistance) {
            best = known;
            bestDistance = d;
        }
    }
    const int limit = std::max(2, static_cast<int>(lower.size()) / 3);
    return bestDistance <= limit ? best : std::string();
}

std::string Config::currentValue(const std::string& key) const {
    const std::string wanted = canonicalKey(key);
    if (wanted.empty())
        return std::string();

    // Not part of serialize(), they describe the run and not the physics.
    if (wanted == "restart")
        return restart ? "1" : "0";
    if (wanted == "restartFile")
        return restartFile;
    if (wanted == "addTime") {
        std::ostringstream out;
        out << addTime;
        return out.str();
    }

    std::istringstream text(serialize());
    std::string line;
    while (std::getline(text, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (canonicalKey(line.substr(0, eq)) != wanted)
            continue;

        const std::string value = line.substr(eq + 1);
        // serialize() prints at full float precision, which reads terribly in
        // a prompt ("0.00999999978"). Round it back to what print() shows.
        char* end = nullptr;
        const double number = std::strtod(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0') {
            std::ostringstream out;
            out << number;
            return out.str();
        }
        return value;
    }
    return std::string();
}

bool Config::ask(const std::string& key, const std::string& prompt) {
    const std::string canon = canonicalKey(key);
    const std::string name = canon.empty() ? key : canon;

    // wallMotion is the one answer with a grammar of its own, so its rules go
    // on the screen before the cursor gets there and not only after a refusal.
    // Here rather than at the call site, so re-entering it from the
    // confirmation menu shows the same block.
    if (name == "wallMotion")
        std::cout << wallMotionHelp();

    for (;;) {
        std::cout << prompt;
        const std::string shown = currentValue(name);
        if (!shown.empty())
            std::cout << " [" << shown << "]";
        std::cout << ": ";

        std::string line;
        if (!std::getline(std::cin, line)) {
            // Ctrl+Z, Ctrl+D, or a script that ran out of input. Reading on a
            // dead stream returns instantly, so asking anything else would
            // just scroll the remaining questions past without an answer.
            std::cout << "\nEnd of input. Keeping the rest of the "
                         "configuration as it is.\n";
            return false;
        }

        line = trimSpace(line);
        if (line.empty())
            return true;   // Enter keeps what is already there

        std::string error, warning;
        if (setParam(name, line, error, &warning)) {
            if (!warning.empty())
                std::cout << "  Warning: " << warning << "\n";
            return true;
        }
        std::cout << "  " << error << "\n";
    }
}

void Config::readFromConsole() {
    std::cout << "=== CFD-Solver-2D Configuration ===\n";
    std::cout << "Press Enter to keep the value shown in brackets.\n\n";
    std::cout << "Start a new simulation or continue an old one?\n";
    std::cout << "  0 = new simulation\n";
    std::cout << "  1 = continue from a saved .vtk\n";
    if (!ask("restart", "Your choice"))
        return;
    if (restart) {
        while (restartFile.empty()) {
            if (!ask("restartFile",
                     "Enter path to the .vtk to continue from (or the folder "
                     "with the frames, newest one wins)"))
                return;
        }
        std::cout << "Configuration will be restored from that frame.\n";
        return;
    }
    if (!ask("Lx", "Enter domain width Lx (m)")) return;
    if (!ask("Ly", "Enter domain height Ly (m)")) return;
    if (!ask("nx", "Enter number of cells in x-direction nx")) return;
    if (!ask("ny", "Enter number of cells in y-direction ny")) return;
    if (!ask("U0", "Enter inlet velocity U0 (m/s)")) return;
    if (!ask("nu", "Enter kinematic viscosity nu (m^2/s)")) return;
    if (!ask("ro", "Enter density ro. Make sure that the gas/liquid is "
                   "incompressible (meaning for air speed its less than 0.3M) "
                   "(kg/m^3)")) return;
    if (!ask("gravityEnabled", "Enable gravity? (0 = no, 1 = yes)")) return;
    if (gravityEnabled) {
        if (!ask("gravityAccel",
                 "Enter gravitational acceleration (m/s^2, 9.81 on Earth)"))
            return;
        if (!ask("gravityAngle",
                 "Enter gravity direction (degrees clockwise from straight "
                 "down: 0 = down, 90 = towards the inlet, 180 = up)"))
            return;
        std::cout << "Note: at constant density gravity only adds hydrostatic"
                     " pressure, the velocity field is unchanged.\n";
    }
    if (!ask("CFL", "Enter CFL number (recommended 0.3-0.5)")) return;
    if (!ask("totalTime", "Enter total simulation time (seconds)")) return;
    if (!ask("dtUpdateInterval",
             "Enter steps between dt recomputations (recommended 5)")) return;
    if (!ask("omega", "Enter SOR relaxation parameter omega (coarsest multigrid "
                      "level, 1.6-1.85)")) return;
    if (!ask("smootherOmega",
             "Enter SOR relaxation parameter smootherOmega (V-cycle smoother, "
             "1.0-1.3 recommended)")) return;
    if (!ask("mgIterations",
             "Enter multigrid V-cycles per step (2 by default, 4-10 max "
             "recommended)")) return;
    if (!ask("mgTolerance",
             "Enter multigrid relative residual tolerance (1e-4 HEAVILY "
             "recommended)")) return;
    if (!ask("mgMinCoarseSize",
             "Enter minimum coarse grid size (8 recommended)")) return;
    if (!ask("saveInterval",
             "Enter VTK save interval in steps (1 = every step, 20 "
             "recommended)")) return;
    if (!ask("geometryFile",
             "Enter path to 3D model (or 'none' for circle)")) return;
    if (!ask("sliceAngleX",
             "Enter around the axis going towards the observer (degrees)"))
        return;
    if (!ask("sliceAngleZ", "Enter around a vertical axis (degrees)")) return;
    if (!ask("sliceRotation",
             "Enter rotation in the simulation plane (degrees)")) return;
    if (!ask("invertSection", "Mirror the section? (0 = no, 1 = yes)")) return;
    if (!ask("wallMotion",
             "Wall behaviour (Enter leaves every wall stationary and no-slip)"))
        return;
    if (!ask("useCuda",
             "Use cuda? (0 = no, 1 = yes, ignored on a CPU-only build)"))
        return;
    std::cout << "Configuration read.\n";
}
void Config::print() const {
    std::cout << "\n--- Current Configuration ---\n";
    std::cout << "  mode             = " << (restart ? "CONTINUE" : "NEW") << "\n";
    if (restart) {
        std::cout << "  restartFile      = " << restartFile << "\n";
        std::cout << "  addTime          = " << addTime
                  << " s (0 = totalTime is used as is)\n";
    }
    std::cout << "  Lx               = " << Lx << " m\n";
    std::cout << "  Ly               = " << Ly << " m\n";
    std::cout << "  nx               = " << nx << "\n";
    std::cout << "  ny               = " << ny << "\n";
    std::cout << "  U0               = " << U0 << " m/s\n";
    std::cout << "  nu               = " << nu << " m^2/s\n";
    std::cout << "  ro               = " << ro << " kg/m^3\n";
    std::cout << "  gravity          = " << (gravityEnabled ? "ON" : "OFF") << "\n";
    if (gravityEnabled) {
        std::cout << "  gravityAccel     = " << gravityAccel << " m/s^2\n";
        std::cout << "  gravityAngle     = " << gravityAngle
                  << " deg (clockwise, 0 = down)\n";
    }
    std::cout << "  CFL              = " << CFL << "\n";
    std::cout << "  totalTime        = " << totalTime << " s\n";
    std::cout << "  dtUpdateInterval = " << dtUpdateInterval << " steps\n";
    std::cout << "  omega            = " << omega << " (coarsest level)\n";
    std::cout << "  smootherOmega    = " << smootherOmega << " (V-cycle smoother)\n";
    std::cout << "  mgIterations     = " << mgIterations << " V-cycles/step\n";
    std::cout << "  mgTolerance      = " << mgTolerance << " (relative)\n";
    std::cout << "  mgMinCoarseSize  = " << mgMinCoarseSize << " cells/axis\n";
    std::cout << "  saveInterval     = " << saveInterval << " steps\n";
    std::cout << "  outputDir        = " << outputDir << "\n";
    std::cout << "  geometryFile     = " << geometryFile << "\n";
    std::cout << "  sliceAngleX      = " << sliceAngleX << " deg\n";
    std::cout << "  sliceAngleZ      = " << sliceAngleZ << " deg\n";
    std::cout << "  invertSection    = " << invertSection << "\n";
    std::cout << "  sliceRotation    = " << sliceRotation << " deg\n";
    std::cout << "  wallMotion       = "
              << (wallMotion.empty() ? "none" : wallMotion) << "\n";
    std::cout << " CUDA? Yes/No:       " << (useCuda ? "Yes" : "No") << "\n";
    std::cout << "--------------------------------\n";
}

std::string Config::serialize() const {
    std::ostringstream out;

    out << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "Lx=" << Lx << "\n"
        << "Ly=" << Ly << "\n"
        << "nx=" << nx << "\n"
        << "ny=" << ny << "\n"
        << "U0=" << U0 << "\n"
        << "nu=" << nu << "\n"
        << "ro=" << ro << "\n"
        // Frames written before gravity existed simply do not carry these keys,
        // and setParam is never called for them, so the defaults leave gravity
        // off. Old frames stay loadable, new frames stay readable by old builds.
        << "gravityEnabled=" << (gravityEnabled ? 1 : 0) << "\n"
        << "gravityAccel=" << gravityAccel << "\n"
        << "gravityAngle=" << gravityAngle << "\n"
        << "CFL=" << CFL << "\n"
        << "dtUpdateInterval=" << dtUpdateInterval << "\n"
        << "dtSafety=" << dtSafety << "\n"
        << "omega=" << omega << "\n"
        << "smootherOmega=" << smootherOmega << "\n"
        << "mgIterations=" << mgIterations << "\n"
        << "mgTolerance=" << mgTolerance << "\n"
        << "mgMinCoarseSize=" << mgMinCoarseSize << "\n"
        << "saveInterval=" << saveInterval << "\n"
        << "useCuda=" << (useCuda ? 1 : 0) << "\n"
        << "sliceAngleX=" << sliceAngleX << "\n"
        << "sliceAngleZ=" << sliceAngleZ << "\n"
        << "sliceRotation=" << sliceRotation << "\n"
        << "invertSection=" << (invertSection ? 1 : 0) << "\n";

    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "totalTime=" << totalTime << "\n";

    out << "outputDir=" << outputDir << "\n"
        << "geometryFile=" << geometryFile << "\n"
        << "wallMotion=" << wallMotion << "\n";

    return out.str();
}

bool Config::setParam(const std::string& key, const std::string& value) {
    std::string ignored;
    return setParam(key, value, ignored, nullptr);
}

bool Config::setParam(const std::string& key,
                      const std::string& value,
                      std::string& error,
                      std::string* warning) {
    error.clear();
    if (warning)
        warning->clear();

    const std::string k = canonicalKey(key);
    if (k.empty()) {
        const std::string guess = suggestKey(key);
        error = badValue(trimSpace(key), cleanValue(value),
                         guess.empty()
                             ? "there is no such parameter, run with --help "
                               "for the list"
                             : "there is no such parameter. Did you mean " +
                                   guess + "?");
        return false;
    }

    bool ok = false;
    if      (k == "Lx") ok = assignFloat(Lx, k, value, kTiny, kHuge,
             "the domain width must be a positive length in metres", error);
    else if (k == "Ly") ok = assignFloat(Ly, k, value, kTiny, kHuge,
             "the domain height must be a positive length in metres", error);
    else if (k == "nx") ok = assignInt(nx, k, value, 8, kIntMax,
             "the grid needs at least 8 cells per axis, the multigrid has "
             "nothing to coarsen below that", error);
    else if (k == "ny") ok = assignInt(ny, k, value, 8, kIntMax,
             "the grid needs at least 8 cells per axis, the multigrid has "
             "nothing to coarsen below that", error);
    else if (k == "U0") ok = assignFloat(U0, k, value, -kHuge, kHuge,
             "the inlet velocity must be a finite number", error);
    else if (k == "nu") ok = assignFloat(nu, k, value, 0.0, kHuge,
             "viscosity cannot be negative (0 means inviscid)", error);
    else if (k == "ro") ok = assignFloat(ro, k, value, kTiny, kHuge,
             "density must be positive", error);
    else if (k == "gravityEnabled") ok = assignBool(gravityEnabled, k, value, error);
    else if (k == "gravityAccel") ok = assignFloat(gravityAccel, k, value, 0.0, kHuge,
             "this is a magnitude, it cannot be negative; to point gravity the "
             "other way use gravityAngle=180", error);
    else if (k == "gravityAngle") ok = assignFloat(gravityAngle, k, value, -kHuge, kHuge,
             "the angle must be a finite number of degrees", error);
    else if (k == "CFL") ok = assignFloat(CFL, k, value, kTiny, kHuge,
             "the CFL number must be positive (0.3-0.5 is the usual range)", error);
    else if (k == "totalTime") ok = assignDouble(totalTime, k, value, kTiny, kHuge,
             "the simulated time must be positive", error);
    else if (k == "dtUpdateInterval") ok = assignInt(dtUpdateInterval, k, value, 1, kIntMax,
             "dt is recomputed every N steps, so N is at least 1", error);
    else if (k == "dtSafety") ok = assignFloat(dtSafety, k, value, kTiny, kHuge,
             "this is the fraction of the stable dt that is actually taken, so "
             "it must be positive (0.9 = 90%)", error);
    else if (k == "omega") ok = assignFloat(omega, k, value, kTiny, 2.0 - 1e-6,
             "SOR only converges for 0 < omega < 2", error);
    else if (k == "smootherOmega") ok = assignFloat(smootherOmega, k, value, kTiny, 2.0 - 1e-6,
             "SOR only converges for 0 < smootherOmega < 2", error);
    else if (k == "mgIterations") ok = assignInt(mgIterations, k, value, 1, kIntMax,
             "at least one V-cycle per pressure solve", error);
    else if (k == "mgTolerance") ok = assignFloat(mgTolerance, k, value, kTiny, 1.0,
             "this is a relative residual, so it lives between 0 and 1 "
             "(1e-4 recommended)", error);
    else if (k == "mgMinCoarseSize") ok = assignInt(mgMinCoarseSize, k, value, 2, kIntMax,
             "the coarsest grid needs at least 2 cells per axis", error);
    else if (k == "useCuda") ok = assignBool(useCuda, k, value, error);
    else if (k == "saveInterval") ok = assignInt(saveInterval, k, value, 1, kIntMax,
             "a frame is written every N steps, so N is at least 1", error);
    else if (k == "outputDir")    { outputDir = cleanValue(value); ok = true; }
    else if (k == "geometryFile") { geometryFile = cleanValue(value); ok = true; }
    else if (k == "sliceAngleX") ok = assignFloat(sliceAngleX, k, value, -kHuge, kHuge,
             "the angle must be a finite number of degrees", error);
    else if (k == "sliceAngleZ") ok = assignFloat(sliceAngleZ, k, value, -kHuge, kHuge,
             "the angle must be a finite number of degrees", error);
    else if (k == "sliceRotation") ok = assignFloat(sliceRotation, k, value, -kHuge, kHuge,
             "the angle must be a finite number of degrees", error);
    else if (k == "invertSection") ok = assignBool(invertSection, k, value, error);
    else if (k == "wallMotion") {
        std::vector<WallMotion> parsed;
        ok = parseWallMotion(value, parsed, error);
        if (ok)
            wallMotion = cleanValue(value);
    }
    else if (k == "restart")       ok = assignBool(restart, k, value, error);
    else if (k == "restartFile")  { restartFile = cleanValue(value); ok = true; }
    else if (k == "addTime") ok = assignDouble(addTime, k, value, -kHuge, kHuge,
             "addTime must be a finite number of seconds", error);

    if (!ok)
        return false;

    if (warning) {
        const std::string shown = k + "=" + cleanValue(value);
        if (k == "CFL" && CFL > 1.0f)
            *warning = shown + " is above 1; advection here is explicit, so "
                               "the run will most likely blow up";
        else if (k == "dtSafety" && dtSafety > 1.0f)
            *warning = shown + " takes a bigger step than the stability "
                               "estimate allows";
        else if ((k == "omega" && omega >= 1.95f) ||
                 (k == "smootherOmega" && smootherOmega >= 1.95f))
            *warning = shown + " is very close to 2, where SOR stops being "
                               "reliable";
        else if (k == "mgTolerance" && mgTolerance > 0.1f)
            *warning = shown + " is a very loose tolerance; the pressure solve "
                               "will stop long before the field is divergence "
                               "free";
        else if (k == "nu" && nu == 0.0f)
            *warning = shown + " is inviscid; nothing damps the smallest scales";
    }
    return true;
}

bool Config::modifyParam(const std::string& name) {
    const std::string canon = canonicalKey(name);
    if (canon.empty()) {
        const std::string guess = suggestKey(name);
        std::cout << "There is no parameter called '" << trimSpace(name) << "'";
        if (!guess.empty())
            std::cout << ". Did you mean " << guess << "?";
        std::cout << "\n";
        return false;
    }
    // confirm() reprints the whole configuration on the next turn, so there is
    // nothing to announce here.
    return ask(canon, "New " + canon);
}
bool Config::confirm() {
    print();
    std::cout << "\nTo change a parameter, type its name (e.g. 'nx'), or the\n"
                 "whole thing at once ('nx=256'), and press Enter.\n";
    std::cout << "To confirm all parameters and proceed, just press Enter (empty line).\n";
    std::cout << "> ";

    std::string input;
    if (!std::getline(std::cin, input)) {
        std::cout << "\nEnd of input, going with the configuration above.\n";
        return true;
    }
    input = trimSpace(input);

    if (input.empty()) {
        return true;   // confirmed
    }

    const size_t eq = input.find('=');
    if (eq != std::string::npos && eq > 0) {
        const std::string key = input.substr(0, eq);
        std::string error, warning;
        if (setParam(key, input.substr(eq + 1), error, &warning)) {
            if (!warning.empty())
                std::cout << "Warning: " << warning << "\n";
        } else {
            std::cout << error << "\n";
            // This branch never went through ask(), so the rules have not been
            // printed and a one-line refusal is all there would be to go on.
            if (canonicalKey(key) == "wallMotion")
                std::cout << wallMotionHelp();
        }
        return false;
    }

    modifyParam(input);
    return false;  // not confirmed yet, loop again
}
