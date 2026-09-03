#pragma once

#include <string>
#include <vector>

namespace maskui {

struct BodyPose {
    double time = 0.0;
    double x = 0.0;
    double y = 0.0;
    double rot = 0.0;
    std::string interp = "linear";
    std::string ease = "inout";
};

std::vector<BodyPose> parseBodyTrack(const std::string& entry);

std::string formatBodyTrack(const std::vector<BodyPose>& track);

std::string bodyTrackToMotion(const std::vector<BodyPose>& track);

BodyPose bodyPoseAt(const std::vector<BodyPose>& track, double when);

void dropBodyPose(std::vector<BodyPose>& track, const BodyPose& pose);

bool removeBodyPose(std::vector<BodyPose>& track, double when);

} // namespace maskui
