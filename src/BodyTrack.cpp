#include "BodyTrack.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace maskui {
namespace {

std::string lower(std::string text) {
    for (char& character : text)
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string itemValue(const std::string& block, const std::string& key) {
    std::size_t at = 0;
    while (at <= block.size()) {
        std::size_t comma = block.find(',', at);
        if (comma == std::string::npos)
            comma = block.size();
        const std::string item = block.substr(at, comma - at);
        const std::size_t equals = item.find('=');
        if (equals != std::string::npos &&
            lower(item.substr(0, equals)) == key)
            return item.substr(equals + 1);
        if (comma == block.size())
            break;
        at = comma + 1;
    }
    return std::string();
}

double numberValue(const std::string& block,
                   const std::string& key,
                   double fallback) {
    const std::string text = itemValue(block, key);
    if (text.empty())
        return fallback;
    try {
        const double parsed = std::stod(text);
        return std::isfinite(parsed) ? parsed : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string trimmed(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

} // namespace

std::vector<BodyPose> parseBodyTrack(const std::string& entry) {
    std::vector<BodyPose> track;
    std::size_t at = 0;
    while (at < entry.size()) {
        const std::size_t open = entry.find('@', at);
        if (open == std::string::npos)
            break;
        std::size_t close = entry.find('@', open + 1);
        if (close == std::string::npos)
            close = entry.size();
        const std::string block =
            trimmed(entry.substr(open + 1, close - open - 1));
        BodyPose pose;
        pose.time = numberValue(block, "t", 0.0);
        pose.x = numberValue(block, "x", 0.0);
        pose.y = numberValue(block, "y", 0.0);
        pose.rot = numberValue(block, "rot", 0.0);
        const std::string interp = itemValue(block, "interp");
        const std::string ease = itemValue(block, "ease");
        if (!interp.empty())
            pose.interp = interp;
        if (!ease.empty())
            pose.ease = ease;
        track.push_back(pose);
        at = close;
    }
    std::sort(track.begin(), track.end(),
              [](const BodyPose& a, const BodyPose& b) {
                  return a.time < b.time;
              });
    return track;
}

std::string formatBodyTrack(const std::vector<BodyPose>& track) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const BodyPose& pose : track)
        out << "@t=" << pose.time << ",x=" << pose.x << ",y=" << pose.y
            << ",rot=" << pose.rot << ",interp=" << pose.interp
            << ",ease=" << pose.ease;
    return out.str();
}

std::string bodyTrackToMotion(const std::vector<BodyPose>& track) {
    if (track.size() < 2)
        return std::string();

    std::ostringstream out;
    out << std::setprecision(9);
    bool wrote = false;
    for (std::size_t k = 0; k + 1 < track.size(); ++k) {
        const BodyPose& from = track[k];
        const BodyPose& to = track[k + 1];
        const double span = to.time - from.time;
        if (!(span > 1e-9))
            continue;
        if (wrote)
            out << ',';
        wrote = true;
        out << '@' << from.time;
        if (from.interp != "linear")
            out << ",interp=" << from.interp << ",ease=" << from.ease;
        out << ",vx=" << (to.x - from.x) / span
            << ",vy=" << (to.y - from.y) / span
            << ",omega=" << (to.rot - from.rot) / span;
    }
    if (!wrote)
        return std::string();
    out << ",@" << track.back().time << ",vx=0,vy=0,omega=0";
    return out.str();
}

BodyPose bodyPoseAt(const std::vector<BodyPose>& track, double when) {
    BodyPose current;
    current.time = when;
    if (track.empty())
        return current;
    if (when <= track.front().time) {
        current = track.front();
        current.time = when;
        return current;
    }
    if (when >= track.back().time) {
        current = track.back();
        current.time = when;
        return current;
    }
    for (std::size_t k = 0; k + 1 < track.size(); ++k) {
        if (when < track[k].time || when > track[k + 1].time)
            continue;
        const double span = track[k + 1].time - track[k].time;
        const double t = span > 1e-12 ? (when - track[k].time) / span : 0.0;
        current = track[k];
        current.time = when;
        current.x += (track[k + 1].x - track[k].x) * t;
        current.y += (track[k + 1].y - track[k].y) * t;
        current.rot += (track[k + 1].rot - track[k].rot) * t;
        return current;
    }
    current = track.back();
    current.time = when;
    return current;
}

void dropBodyPose(std::vector<BodyPose>& track, const BodyPose& pose) {
    bool replaced = false;
    for (BodyPose& existing : track)
        if (std::fabs(existing.time - pose.time) < 1e-6) {
            existing = pose;
            replaced = true;
        }
    if (!replaced)
        track.push_back(pose);
    std::sort(track.begin(), track.end(),
              [](const BodyPose& a, const BodyPose& b) {
                  return a.time < b.time;
              });
}

bool removeBodyPose(std::vector<BodyPose>& track, double when) {
    const std::size_t before = track.size();
    track.erase(std::remove_if(track.begin(), track.end(),
                               [&](const BodyPose& pose) {
                                   return std::fabs(pose.time - when) < 1e-6;
                               }),
                track.end());
    return track.size() != before;
}

} // namespace maskui
