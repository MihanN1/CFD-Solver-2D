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
    "useCuda", "saveInterval", "outputDir", "extraFields",
    "geometryFile", "sliceAngleX", "sliceAngleZ", "sliceRotation",
    "invertSection", "wallMotion", "profiles",
    "restart", "restartFile", "addTime",
    "gravityMode", "convection", "limiter", "timeScheme",
    "caseType", "lidSpeed", "steadyTolerance",
    "bcLeft", "bcRight", "bcBottom", "bcTop",
    "bcLeftSpeed", "bcRightSpeed", "bcBottomSpeed", "bcTopSpeed",
    "inletFrom", "inletTo", "inletProfile",
    "phases", "rho1", "rho2", "nu1", "nu2",
    "phaseInit", "phaseLevel", "phaseX", "phaseY",
    "initialPhaseFile", "vofScheme", "sources",
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

namespace {

struct EnumEntry {
    const char* name;
    int value;
};

bool assignEnumValue(int& target,
                     const std::string& key,
                     const std::string& value,
                     const std::vector<EnumEntry>& entries,
                     std::string& error) {
    std::string wanted = trimSpace(cleanValue(value));
    for (char& c : wanted)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const EnumEntry& entry : entries) {
        std::string name = entry.name;
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == wanted) {
            target = entry.value;
            return true;
        }
    }

    std::string list;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i)
            list += (i + 1 == entries.size()) ? " or " : ", ";
        list += entries[i].name;
    }
    error = badValue(key, cleanValue(value),
                     "this one takes a name, not a number: " + list);
    return false;
}

const char* enumName(int value, const std::vector<EnumEntry>& entries) {
    for (const EnumEntry& entry : entries)
        if (entry.value == value)
            return entry.name;
    return "?";
}

const std::vector<EnumEntry> kGravityModes{
    {"reduced", static_cast<int>(GravityMode::Reduced)},
    {"body", static_cast<int>(GravityMode::Body)}};

const std::vector<EnumEntry> kConvectionSchemes{
    {"upwind", static_cast<int>(ConvectionScheme::Upwind)},
    {"central", static_cast<int>(ConvectionScheme::Central)},
    {"muscl", static_cast<int>(ConvectionScheme::Muscl)}};

const std::vector<EnumEntry> kLimiters{
    {"minmod", static_cast<int>(LimiterKind::Minmod)},
    {"vanleer", static_cast<int>(LimiterKind::VanLeer)},
    {"superbee", static_cast<int>(LimiterKind::Superbee)}};

const std::vector<EnumEntry> kVofSchemes{
    {"upwind", static_cast<int>(VofScheme::Upwind)},
    {"hric", static_cast<int>(VofScheme::Hric)},
    {"cicsam", static_cast<int>(VofScheme::Cicsam)}};

const std::vector<EnumEntry> kPhaseInits{
    {"layer", static_cast<int>(PhaseInit::Layer)},
    {"drop", static_cast<int>(PhaseInit::Drop)},
    {"column", static_cast<int>(PhaseInit::Column)},
    {"file", static_cast<int>(PhaseInit::File)}};

const std::vector<EnumEntry> kTimeSchemes{
    {"euler", static_cast<int>(TimeScheme::Euler)},
    {"rk2", static_cast<int>(TimeScheme::RK2)},
    {"rk3", static_cast<int>(TimeScheme::RK3)}};

bool assignSideKind(BoundarySpec& spec,
                    const std::string& key,
                    const std::string& value,
                    std::string& error) {
    std::string why;
    BoundaryKind kind = spec.kind;
    if (!parseBoundaryKind(trimSpace(cleanValue(value)), kind, why)) {
        error = badValue(key, cleanValue(value), why);
        return false;
    }
    spec.kind = kind;
    return true;
}

}   // namespace

bool parseVofScheme(const std::string& text, VofScheme& out,
                    std::string& error) {
    const std::string key = toLower(trimSpace(text));
    if (key == "upwind") { out = VofScheme::Upwind; return true; }
    if (key == "hric")   { out = VofScheme::Hric;   return true; }
    if (key == "cicsam") { out = VofScheme::Cicsam; return true; }
    error = "'" + text + "' is not a VOF scheme. Use upwind, hric or cicsam.";
    return false;
}

bool parsePhaseInit(const std::string& text, PhaseInit& out,
                    std::string& error) {
    const std::string key = toLower(trimSpace(text));
    if (key == "layer")  { out = PhaseInit::Layer;  return true; }
    if (key == "drop")   { out = PhaseInit::Drop;   return true; }
    if (key == "column") { out = PhaseInit::Column; return true; }
    if (key == "file")   { out = PhaseInit::File;   return true; }
    error = "'" + text +
            "' is not an initial shape. Use layer, drop, column or file.";
    return false;
}

const char* vofSchemeName(VofScheme scheme) {
    switch (scheme) {
    case VofScheme::Upwind: return "upwind";
    case VofScheme::Cicsam: return "cicsam";
    default:                return "hric";
    }
}

const char* phaseInitName(PhaseInit init) {
    switch (init) {
    case PhaseInit::Drop:   return "drop";
    case PhaseInit::Column: return "column";
    case PhaseInit::File:   return "file";
    default:                return "layer";
    }
}

std::string sourcesHelp() {
    return
        "\n--- How to write sources -----------------------------------------\n"
        "  sources=x=<m>,y=<m>,r=<m>,rate=<m/s>[,angle=<deg>][,phase=<0..1>];...\n"
        "\n"
        "  A source is a disc inside the domain that pushes fluid out of itself.\n"
        "  Unlike an inlet it is not on a side, so it needs a direction:\n"
        "    x, y     centre, in metres\n"
        "    r        radius, in metres. Under one cell nothing comes out.\n"
        "    rate     speed the fluid leaves at, m/s. Negative drains instead.\n"
        "    angle    degrees, 0 is +x and it turns counter-clockwise\n"
        "    phase    which fluid comes out, 1 or 0. Ignored at one phase.\n"
        "\n"
        "  Everything a source adds has to leave somewhere, so a case with one\n"
        "  needs an outlet exactly as an inlet does, and is refused without one.\n"
        "\n"
        "    sources=\"x=0.5,y=0.2,r=0.05,rate=2,angle=90,phase=1\"\n"
        "------------------------------------------------------------------\n";
}

bool parseSources(const std::string& text,
                  std::vector<FlowSource>& out,
                  std::string& error) {
    out.clear();
    const std::string body = trimSpace(text);
    if (body.empty() || toLower(body) == "none")
        return true;

    size_t start = 0;
    while (start <= body.size()) {
        const size_t end = body.find(';', start);
        const std::string token =
            trimSpace(body.substr(start, end == std::string::npos
                                             ? std::string::npos
                                             : end - start));
        start = (end == std::string::npos) ? body.size() + 1 : end + 1;
        if (token.empty())
            continue;

        FlowSource source;
        bool sawRate = false;
        size_t at = 0;
        while (at <= token.size()) {
            const size_t comma = token.find(',', at);
            const std::string piece =
                trimSpace(token.substr(at, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - at));
            at = (comma == std::string::npos) ? token.size() + 1 : comma + 1;
            if (piece.empty())
                continue;

            const size_t equals = piece.find('=');
            if (equals == std::string::npos) {
                error = "'" + piece +
                        "' has no '=' in it. Every setting of a source is "
                        "<name>=<number>.";
                return false;
            }
            const std::string name = toLower(trimSpace(piece.substr(0, equals)));
            const std::string valueText = trimSpace(piece.substr(equals + 1));
            double value = 0.0;
            std::string why;
            if (!parseNumber(name, valueText, false, value, why)) {
                error = why;
                return false;
            }
            if (name == "x") source.x = static_cast<float>(value);
            else if (name == "y") source.y = static_cast<float>(value);
            else if (name == "r" || name == "radius")
                source.radius = static_cast<float>(value);
            else if (name == "rate" || name == "speed") {
                source.rate = static_cast<float>(value);
                sawRate = true;
            }
            else if (name == "angle") source.angle = static_cast<float>(value);
            else if (name == "phase")
                source.phase = static_cast<float>(std::min(1.0, std::max(0.0, value)));
            else {
                error = "'" + name +
                        "' is not a setting of a source. Use x, y, r, rate, "
                        "angle or phase.";
                return false;
            }
        }

        if (!(source.radius > 0.0f)) {
            error = "a source needs a radius: r=<metres>.";
            return false;
        }
        if (!sawRate) {
            error = "a source with no rate= does nothing at all.";
            return false;
        }
        out.push_back(source);
    }
    return true;
}

std::string profilesHelp() {
    return
        "\n--- How to write profiles ----------------------------------------\n"
        "  profiles=<file>@<setting>=<value>,<setting>=<value>;<next file>@...\n"
        "\n"
        "  The separator between the file and its settings is '@' and not ':',\n"
        "  because a Windows path already owns the colon.\n"
        "\n"
        "  Settings, all optional:\n"
        "    x, y     where the centre of this model lands, in metres\n"
        "    size     the larger side of its section, in metres\n"
        "    rot      turn it in the plane, degrees\n"
        "    ax, az   slice angles for this model alone, degrees\n"
        "    invert   1 mirrors it, same as invertSection but per model\n"
        "\n"
        "  A file with no '@' keeps the old behaviour: centred in the domain\n"
        "  at a fifth of its smaller side. Anything landing on or outside the\n"
        "  domain edge is refused with the number it missed by, because a body\n"
        "  touching the border is a wall, not an obstacle.\n"
        "\n"
        "    profiles=\"wing.stl@x=0.6,y=0.5,size=0.3;ball.obj@x=1.6,y=0.5\"\n"
        "------------------------------------------------------------------\n";
}

bool parseProfiles(const std::string& text,
                   std::vector<Profile>& out,
                   std::string& error) {
    out.clear();
    error.clear();
    const std::string body = trimSpace(text);
    if (body.empty())
        return true;

    size_t pos = 0;
    while (pos <= body.size()) {
        const size_t end = body.find(';', pos);
        const std::string token =
            trimSpace(body.substr(pos, end == std::string::npos
                                           ? std::string::npos
                                           : end - pos));
        pos = (end == std::string::npos) ? body.size() + 1 : end + 1;
        if (token.empty())
            continue;

        Profile profile;
        const size_t at = token.find('@');
        profile.file = trimSpace(token.substr(0, at));
        if (profile.file.empty()) {
            error = badValue("profiles", token,
                             "there is no file name in front of the '@'");
            return false;
        }

        if (at != std::string::npos) {
            std::string settings = token.substr(at + 1);
            size_t sub = 0;
            while (sub <= settings.size()) {
                const size_t comma = settings.find(',', sub);
                const std::string pair =
                    trimSpace(settings.substr(sub, comma == std::string::npos
                                                       ? std::string::npos
                                                       : comma - sub));
                sub = (comma == std::string::npos) ? settings.size() + 1
                                                   : comma + 1;
                if (pair.empty())
                    continue;

                const size_t eq = pair.find('=');
                if (eq == std::string::npos || eq == 0) {
                    error = badValue("profiles", pair,
                                     "every setting is name=value, e.g. x=1.5");
                    return false;
                }
                std::string name = trimSpace(pair.substr(0, eq));
                for (char& c : name)
                    c = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                const std::string raw = trimSpace(pair.substr(eq + 1));

                double number = 0.0;
                std::string why;
                if (!parseNumber("profiles", raw, false, number, why)) {
                    error = badValue("profiles", pair, why);
                    return false;
                }

                if (name == "x")           { profile.x = float(number); profile.placed = true; }
                else if (name == "y")      { profile.y = float(number); profile.placed = true; }
                else if (name == "size")   { profile.size = float(number); }
                else if (name == "rot")    { profile.rotation = float(number); }
                else if (name == "ax")     { profile.angleX = float(number); profile.angleSet = true; }
                else if (name == "az")     { profile.angleZ = float(number); profile.angleSet = true; }
                else if (name == "invert") { profile.invert = number != 0.0; profile.invertSet = true; }
                else {
                    error = badValue("profiles", pair,
                                     "'" + name +
                                         "' is not a profile setting. Use x, y,"
                                         " size, rot, ax, az or invert");
                    return false;
                }
            }
        }

        if (profile.size < 0.0f) {
            error = badValue("profiles", token,
                             "size is a length in metres, it cannot be "
                             "negative");
            return false;
        }
        out.push_back(std::move(profile));
    }

    return true;
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
    if (name == "profiles")
        std::cout << profilesHelp();

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
    if (!ask("phases", "How many fluids share the domain (1 or 2)")) return;
    if (phases > 1) {
        std::cout << "  Two fluids: nu and ro are ignored and each fluid gets "
                     "its own.\n  Fluid 1 is what the initial shape is made "
                     "of, fluid 2 fills the rest.\n";
        if (!ask("rho1", "Density of fluid 1 (kg/m^3, water is 1000)")) return;
        if (!ask("nu1", "Kinematic viscosity of fluid 1 (m^2/s, water is 1e-6)"))
            return;
        if (!ask("rho2", "Density of fluid 2 (kg/m^3, air is 1.225)")) return;
        if (!ask("nu2", "Kinematic viscosity of fluid 2 (m^2/s, air is 1.5e-5)"))
            return;
        if (!ask("phaseInit",
                 "What is in the domain at the start: layer, drop, column or "
                 "file"))
            return;
        if (phaseInit == PhaseInit::File) {
            if (!ask("initialPhaseFile",
                     "File holding one fraction per cell, row 0 first"))
                return;
        } else {
            if (!ask("phaseLevel",
                     phaseInit == PhaseInit::Drop
                         ? "Drop diameter as a fraction of the smaller side"
                         : "Height of fluid 1 as a fraction of Ly"))
                return;
            if (phaseInit != PhaseInit::Layer)
                if (!ask("phaseX",
                         phaseInit == PhaseInit::Column
                             ? "Width of the column as a fraction of Lx"
                             : "Drop centre x as a fraction of Lx"))
                    return;
            if (phaseInit == PhaseInit::Drop)
                if (!ask("phaseY", "Drop centre y as a fraction of Ly")) return;
        }
        if (!ask("vofScheme",
                 "How the interface is carried: hric, cicsam or upwind"))
            return;
    } else {
        if (!ask("nu", "Enter kinematic viscosity nu (m^2/s)")) return;
        if (!ask("ro", "Enter density ro. Make sure that the gas/liquid is "
                       "incompressible (meaning for air speed its less than "
                       "0.3M) (kg/m^3)")) return;
    }
    if (!ask("gravityEnabled", "Enable gravity? (0 = no, 1 = yes)")) return;
    if (gravityEnabled) {
        if (!ask("gravityAccel",
                 "Enter gravitational acceleration (m/s^2, 9.81 on Earth)"))
            return;
        if (!ask("gravityAngle",
                 "Enter gravity direction (degrees clockwise from straight "
                 "down: 0 = down, 90 = towards the inlet, 180 = up)"))
            return;
        if (phases > 1)
            std::cout << "Note: with two fluids the weight difference is what "
                         "moves them, so the force goes\n  into the solve and "
                         "gravityMode is body whether it is asked for or not.\n";
        else
            std::cout << "Note: at constant density gravity only adds "
                         "hydrostatic pressure, the velocity field is "
                         "unchanged.\n";
    }
    if (gravityEnabled && phases == 1) {
        if (!ask("gravityMode",
                 "How gravity is applied: reduced (exact at one density, the "
                 "head is added on output) or body (real force in the solve)"))
            return;
    }
    std::cout << boundaryHelp();
    if (!ask("caseType", "Case: channel or cavity")) return;
    if (caseType == CaseType::Cavity) {
        if (!ask("lidSpeed", "Speed the lid slides at (m/s)")) return;
    } else {
    if (!ask("bcLeft", "Left boundary")) return;
    if (boundaries[BoundarySide::Left].kind == BoundaryKind::MovingWall)
        if (!ask("bcLeftSpeed", "Speed the left wall slides at (m/s)")) return;
    if (!ask("bcRight", "Right boundary")) return;
    if (boundaries[BoundarySide::Right].kind == BoundaryKind::MovingWall)
        if (!ask("bcRightSpeed", "Speed the right wall slides at (m/s)")) return;
    if (!ask("bcBottom", "Bottom boundary")) return;
    if (boundaries[BoundarySide::Bottom].kind == BoundaryKind::MovingWall)
        if (!ask("bcBottomSpeed", "Speed the bottom wall slides at (m/s)")) return;
    if (!ask("bcTop", "Top boundary")) return;
    if (boundaries[BoundarySide::Top].kind == BoundaryKind::MovingWall)
        if (!ask("bcTopSpeed", "Speed the top wall slides at (m/s)")) return;
    }
    if (!ask("steadyTolerance",
             "Stop when the field stops changing? Give the rate, or 0 to run "
             "the whole of totalTime (1e-5 is a good number for a cavity)"))
        return;
    if (!ask("convection",
             "Convective scheme: upwind (first order, what this solver has "
             "always used), muscl or central")) return;
    if (convection == ConvectionScheme::Muscl)
        if (!ask("limiter", "Limiter for muscl: minmod, vanLeer or superbee"))
            return;
    if (!ask("timeScheme",
             "Time scheme: euler, rk2 or rk3. Anything but upwind wants rk2 or "
             "rk3, euler alone is only conditionally stable there")) return;
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
    if (!ask("extraFields",
             "Extra fields to write into every frame, comma separated, empty "
             "for none (vorticity, divergence, speed, objectId, density, source)"))
        return;
    if (!ask("profiles",
             "Extra models and where they sit, empty for just geometryFile "
             "(the rules are above)"))
        return;
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
    if (phases > 1) {
        std::cout << "  phases           = 2\n";
        std::cout << "  fluid 1          = rho " << rho1 << " kg/m^3, nu "
                  << nu1 << " m^2/s\n";
        std::cout << "  fluid 2          = rho " << rho2 << " kg/m^3, nu "
                  << nu2 << " m^2/s\n";
        std::cout << "  phaseInit        = " << phaseInitName(phaseInit);
        if (phaseInit == PhaseInit::File)
            std::cout << " (" << initialPhaseFile << ")";
        else
            std::cout << ", level " << phaseLevel << ", at (" << phaseX << ", "
                      << phaseY << ")";
        std::cout << "\n";
        std::cout << "  vofScheme        = " << vofSchemeName(vofScheme) << "\n";
    } else {
        std::cout << "  nu               = " << nu << " m^2/s\n";
        std::cout << "  ro               = " << ro << " kg/m^3\n";
    }
    if (!sources.empty())
        std::cout << "  sources          = " << resolvedSources().size()
                  << " (" << sources << ")\n";
    std::cout << "  gravity          = " << (gravityEnabled ? "ON" : "OFF") << "\n";
    if (gravityEnabled) {
        std::cout << "  gravityAccel     = " << gravityAccel << " m/s^2\n";
        std::cout << "  gravityAngle     = " << gravityAngle
                  << " deg (clockwise, 0 = down)\n";
        std::cout << "  gravityMode      = "
                  << enumName(static_cast<int>(gravityMode), kGravityModes)
                  << (gravityMode == GravityMode::Reduced
                          ? " (head added on output only)"
                          : " (body force, p is the total pressure)")
                  << "\n";
    }
    std::cout << "  convection       = "
              << enumName(static_cast<int>(convection), kConvectionSchemes);
    if (convection == ConvectionScheme::Muscl)
        std::cout << " (" << enumName(static_cast<int>(limiter), kLimiters)
                  << ")";
    std::cout << "\n";
    std::cout << "  timeScheme       = "
              << enumName(static_cast<int>(timeScheme), kTimeSchemes) << "\n";
    std::cout << "  caseType         = " << caseTypeName(caseType);
    if (caseType == CaseType::Cavity)
        std::cout << ", lid at " << lidSpeed << " m/s";
    std::cout << "\n";
    if (steadyTolerance > 0.0f)
        std::cout << "  steadyTolerance  = " << steadyTolerance
                  << " (stops when the field stops changing)\n";
    std::cout << "  boundaries       = "
              << boundaryKindName(boundaries[BoundarySide::Left].kind) << " | "
              << boundaryKindName(boundaries[BoundarySide::Right].kind) << " | "
              << boundaryKindName(boundaries[BoundarySide::Bottom].kind) << " | "
              << boundaryKindName(boundaries[BoundarySide::Top].kind)
              << "   (left | right | bottom | top)\n";
    for (int side = 0; side < 4; ++side) {
        const BoundarySpec& spec = boundaries.side[side];
        if (spec.kind != BoundaryKind::Inlet)
            continue;
        std::cout << "  inlet ("
                  << boundarySideName(static_cast<BoundarySide>(side))
                  << ")"
                  << std::string(std::max<size_t>(
                         1, 9 - std::string(boundarySideName(
                                    static_cast<BoundarySide>(side))).size()),
                                 ' ')
                  << "= " << (spec.speedSet ? spec.speed : U0) << " m/s, "
                  << inletProfileName(spec.profile);
        if (spec.from > 0.0f || spec.to < 1.0f)
            std::cout << ", band " << spec.from << ".." << spec.to
                      << " of the side";
        std::cout << "\n";
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
    std::cout << "  extraFields      = "
              << (extraFields.empty() ? "none" : extraFields) << "\n";
    std::cout << "  outputDir        = " << outputDir << "\n";
    std::cout << "  geometryFile     = " << geometryFile << "\n";
    std::cout << "  sliceAngleX      = " << sliceAngleX << " deg\n";
    std::cout << "  sliceAngleZ      = " << sliceAngleZ << " deg\n";
    std::cout << "  invertSection    = " << invertSection << "\n";
    std::cout << "  sliceRotation    = " << sliceRotation << " deg\n";
    std::cout << "  wallMotion       = "
              << (wallMotion.empty() ? "none" : wallMotion) << "\n";
    std::cout << "  profiles         = "
              << (profiles.empty() ? "none (geometryFile only)" : profiles)
              << "\n";
    std::cout << " CUDA? Yes/No:       " << (useCuda ? "Yes" : "No") << "\n";
    std::cout << "--------------------------------\n";
}

bool Config::emptyDomain() const {
    if (!profiles.empty())
        return false;
    std::string lowered = geometryFile;
    for (char& c : lowered)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lowered == "empty";
}

std::vector<Profile> Config::resolvedProfiles() const {
    std::vector<Profile> list;
    std::string ignored;
    if (!profiles.empty())
        parseProfiles(profiles, list, ignored);

    if (list.empty()) {
        std::string name = geometryFile;
        std::string lowered = name;
        for (char& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.empty() || lowered == "none" || lowered == "empty")
            return list;
        Profile only;
        only.file = std::move(name);
        list.push_back(std::move(only));
    }

    // A profile that says nothing about its slice takes the one the whole run
    // was configured with, so a single model behaves exactly as it always did.
    for (Profile& profile : list) {
        if (!profile.angleSet) {
            profile.angleX = sliceAngleX;
            profile.angleZ = sliceAngleZ;
        }
        if (!profile.invertSet)
            profile.invert = invertSection;
        if (profile.rotation == 0.0f)
            profile.rotation = sliceRotation;
    }
    return list;
}

std::vector<FlowSource> Config::resolvedSources() const {
    std::vector<FlowSource> list;
    std::string ignored;
    if (!sources.empty())
        parseSources(sources, list, ignored);
    return list;
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
        // Frames written before phases existed have none of these, and a
        // continuation of one is a single phase run exactly as it was.
        << "phases=" << phases << "\n"
        << "rho1=" << rho1 << "\n"
        << "rho2=" << rho2 << "\n"
        << "nu1=" << nu1 << "\n"
        << "nu2=" << nu2 << "\n"
        << "vofScheme=" << vofSchemeName(vofScheme) << "\n"
        << "phaseInit=" << phaseInitName(phaseInit) << "\n"
        << "phaseLevel=" << phaseLevel << "\n"
        << "phaseX=" << phaseX << "\n"
        << "phaseY=" << phaseY << "\n"
        << "initialPhaseFile=" << initialPhaseFile << "\n"
        // Frames written before gravity existed simply do not carry these keys,
        // and setParam is never called for them, so the defaults leave gravity
        // off. Old frames stay loadable, new frames stay readable by old builds.
        << "gravityEnabled=" << (gravityEnabled ? 1 : 0) << "\n"
        << "gravityAccel=" << gravityAccel << "\n"
        << "gravityAngle=" << gravityAngle << "\n"
        << "gravityMode="
        << enumName(static_cast<int>(gravityMode), kGravityModes) << "\n"
        << "convection="
        << enumName(static_cast<int>(convection), kConvectionSchemes) << "\n"
        << "limiter="
        << enumName(static_cast<int>(limiter), kLimiters) << "\n"
        << "timeScheme="
        << enumName(static_cast<int>(timeScheme), kTimeSchemes) << "\n"
        << "caseType=" << caseTypeName(caseType) << "\n"
        << "lidSpeed=" << lidSpeed << "\n"
        << "steadyTolerance=" << steadyTolerance << "\n"
        << "bcLeft=" << boundaryKindName(boundaries[BoundarySide::Left].kind) << "\n"
        << "bcRight=" << boundaryKindName(boundaries[BoundarySide::Right].kind) << "\n"
        << "bcBottom=" << boundaryKindName(boundaries[BoundarySide::Bottom].kind) << "\n"
        << "bcTop=" << boundaryKindName(boundaries[BoundarySide::Top].kind) << "\n"
        << "inletFrom=" << boundaries[BoundarySide::Left].from << "\n"
        << "inletTo=" << boundaries[BoundarySide::Left].to << "\n"
        << "inletProfile="
        << inletProfileName(boundaries[BoundarySide::Left].profile) << "\n";

    // Only the sides somebody actually named a speed for. Writing them all
    // would put "bcLeftSpeed=0" in every frame, and reading that back sets the
    // flag that says "0 was asked for", which is how an inlet at U0 turned
    // into an inlet at a standstill on the first continuation.
    for (int side = 0; side < 4; ++side) {
        const BoundarySpec& spec = boundaries.side[side];
        if (!spec.speedSet)
            continue;
        static const char* const kNames[4] = {
            "bcLeftSpeed", "bcRightSpeed", "bcBottomSpeed", "bcTopSpeed"};
        out << kNames[side] << "=" << spec.speed << "\n";
    }

    out
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
        << "wallMotion=" << wallMotion << "\n"
        << "sources=" << sources << "\n"
        << "profiles=" << profiles << "\n"
        << "extraFields=" << extraFields << "\n";

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
    else if (k == "gravityMode") {
        int mode = static_cast<int>(gravityMode);
        ok = assignEnumValue(mode, k, value, kGravityModes, error);
        if (ok && phases > 1 && static_cast<GravityMode>(mode) ==
                                    GravityMode::Reduced) {
            // The reduced form adds one hydrostatic field on output, and there
            // is no single one when the domain holds two densities. It is not
            // an approximation there, it is the wrong answer: the buoyancy that
            // drives the whole case never enters the solve.
            error = badValue(k, cleanValue(value),
                             "at two phases the weight of the fluid is what "
                             "moves it, so it has to be inside the solve. "
                             "gravityMode=body is the only one that is");
            ok = false;
        }
        if (ok) gravityMode = static_cast<GravityMode>(mode);
    }
    else if (k == "convection") {
        int scheme = static_cast<int>(convection);
        ok = assignEnumValue(scheme, k, value, kConvectionSchemes, error);
        if (ok) convection = static_cast<ConvectionScheme>(scheme);
    }
    else if (k == "limiter") {
        int kind = static_cast<int>(limiter);
        ok = assignEnumValue(kind, k, value, kLimiters, error);
        if (ok) limiter = static_cast<LimiterKind>(kind);
    }
    else if (k == "timeScheme") {
        int scheme = static_cast<int>(timeScheme);
        ok = assignEnumValue(scheme, k, value, kTimeSchemes, error);
        if (ok) timeScheme = static_cast<TimeScheme>(scheme);
    }
    else if (k == "phases") {
        ok = assignInt(phases, k, value, 1, 2,
                       "this solver carries one phase field, so it does one or "
                       "two fluids. Three would need a second field and a rule "
                       "for what happens where all three meet", error);
        // Two densities make the reduced pressure trick wrong rather than
        // merely inexact, so the mode moves with the key instead of waiting to
        // be noticed. Saying gravityMode=reduced afterwards is refused below.
        if (ok && phases > 1)
            gravityMode = GravityMode::Body;
    }
    else if (k == "rho1") ok = assignFloat(rho1, k, value, 1e-9, kHuge,
             "a density has to be positive", error);
    else if (k == "rho2") ok = assignFloat(rho2, k, value, 1e-9, kHuge,
             "a density has to be positive", error);
    else if (k == "nu1") ok = assignFloat(nu1, k, value, 0.0, kHuge,
             "viscosity cannot be negative", error);
    else if (k == "nu2") ok = assignFloat(nu2, k, value, 0.0, kHuge,
             "viscosity cannot be negative", error);
    else if (k == "phaseLevel") ok = assignFloat(phaseLevel, k, value, 0.0, 1.0,
             "this is a fraction of the domain, so it lives between 0 and 1", error);
    else if (k == "phaseX") ok = assignFloat(phaseX, k, value, 0.0, 1.0,
             "this is a fraction of Lx, so it lives between 0 and 1", error);
    else if (k == "phaseY") ok = assignFloat(phaseY, k, value, 0.0, 1.0,
             "this is a fraction of Ly, so it lives between 0 and 1", error);
    else if (k == "initialPhaseFile") { initialPhaseFile = cleanValue(value); ok = true; }
    else if (k == "sources") {
        std::vector<FlowSource> parsed;
        std::string why;
        ok = parseSources(cleanValue(value), parsed, why);
        if (!ok) error = badValue(k, cleanValue(value), why);
        else sources = cleanValue(value);
    }
    else if (k == "vofScheme") {
        int scheme = static_cast<int>(vofScheme);
        ok = assignEnumValue(scheme, k, value, kVofSchemes, error);
        if (ok) vofScheme = static_cast<VofScheme>(scheme);
    }
    else if (k == "phaseInit") {
        int init = static_cast<int>(phaseInit);
        ok = assignEnumValue(init, k, value, kPhaseInits, error);
        if (ok) phaseInit = static_cast<PhaseInit>(init);
    }
    else if (k == "caseType") {
        CaseType type = caseType;
        std::string why;
        ok = parseCaseType(trimSpace(cleanValue(value)), type, why);
        if (!ok) {
            error = badValue(k, cleanValue(value), why);
        } else {
            caseType = type;
            boundaries = (type == CaseType::Cavity)
                             ? cavityBoundaries(lidSpeed)
                             : defaultChannelBoundaries();
            // A cavity is an empty box unless somebody puts something in it.
            // Leaving the default in place would drop the verification circle
            // into the middle of the one case whose answer is tabulated.
            if (type == CaseType::Cavity && geometryFile == "none")
                geometryFile = "empty";
        }
    }
    else if (k == "lidSpeed") {
        ok = assignFloat(lidSpeed, k, value, -kHuge, kHuge,
             "the lid slides at a finite number of m/s", error);
        if (ok && caseType == CaseType::Cavity) {
            boundaries[BoundarySide::Top].speed = lidSpeed;
            boundaries[BoundarySide::Top].speedSet = true;
        }
    }
    else if (k == "steadyTolerance") ok = assignFloat(steadyTolerance, k, value, 0.0, kHuge,
             "this is a rate of change measured against the driving speed, so "
             "it cannot be negative; 0 turns the check off", error);
    else if (k == "bcLeft")   ok = assignSideKind(boundaries[BoundarySide::Left], k, value, error);
    else if (k == "bcRight")  ok = assignSideKind(boundaries[BoundarySide::Right], k, value, error);
    else if (k == "bcBottom") ok = assignSideKind(boundaries[BoundarySide::Bottom], k, value, error);
    else if (k == "bcTop")    ok = assignSideKind(boundaries[BoundarySide::Top], k, value, error);
    else if (k == "bcLeftSpeed" || k == "bcRightSpeed" ||
             k == "bcBottomSpeed" || k == "bcTopSpeed") {
        BoundarySide side = BoundarySide::Left;
        if (k == "bcRightSpeed")       side = BoundarySide::Right;
        else if (k == "bcBottomSpeed") side = BoundarySide::Bottom;
        else if (k == "bcTopSpeed")    side = BoundarySide::Top;
        ok = assignFloat(boundaries[side].speed, k, value, -kHuge, kHuge,
             "the speed this side imposes must be a finite number of m/s", error);
        if (ok) boundaries[side].speedSet = true;
    }
    else if (k == "inletFrom" || k == "inletTo") {
        float target = 0.0f;
        ok = assignFloat(target, k, value, 0.0, 1.0,
             "this is a fraction of the side measured from its low end, so it "
             "lives between 0 and 1", error);
        if (ok)
            for (int side = 0; side < 4; ++side) {
                if (k == "inletFrom") boundaries.side[side].from = target;
                else                  boundaries.side[side].to = target;
            }
    }
    else if (k == "inletProfile") {
        InletProfile profile = InletProfile::Uniform;
        std::string why;
        ok = parseInletProfile(trimSpace(cleanValue(value)), profile, why);
        if (!ok) error = badValue(k, cleanValue(value), why);
        else for (int side = 0; side < 4; ++side)
            boundaries.side[side].profile = profile;
    }
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
    else if (k == "extraFields") {
        const std::string wanted = trimSpace(cleanValue(value));
        std::string bad;
        size_t pos = 0;
        while (pos <= wanted.size() && bad.empty()) {
            const size_t comma = wanted.find(',', pos);
            std::string name = trimSpace(
                wanted.substr(pos, comma == std::string::npos
                                       ? std::string::npos
                                       : comma - pos));
            pos = (comma == std::string::npos) ? wanted.size() + 1 : comma + 1;
            if (name.empty())
                continue;
            for (char& c : name)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            if (name != "vorticity" && name != "divergence" &&
                name != "objectid" && name != "speed" &&
                name != "density" && name != "source")
                bad = name;
        }
        if (!bad.empty()) {
            error = badValue(k, cleanValue(value),
                             "'" + bad +
                                 "' is not a field this build can write. Use "
                                 "vorticity, divergence, speed, objectId, density or source, "
                                 "comma separated");
            ok = false;
        } else {
            extraFields = wanted;
            ok = true;
        }
    }
    else if (k == "profiles") {
        std::vector<Profile> parsed;
        ok = parseProfiles(cleanValue(value), parsed, error);
        if (ok)
            profiles = cleanValue(value);
    }
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
