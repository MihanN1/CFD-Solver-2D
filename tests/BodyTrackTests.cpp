#include "BodyTrack.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cout << message << "\n";
    return 1;
}

bool near(double value, double expected, double tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main() {
    using namespace maskui;

    {
        const std::vector<BodyPose> empty = parseBodyTrack("");
        if (!empty.empty())
            return fail("an empty track parsed into something");
        if (!bodyTrackToMotion(empty).empty())
            return fail("an empty track wrote a bodyMotion entry");
        const BodyPose origin = bodyPoseAt(empty, 0.5);
        if (origin.x != 0.0 || origin.y != 0.0 || origin.rot != 0.0)
            return fail("an empty track did not sample as the origin");
    }

    {
        std::vector<BodyPose> track;
        BodyPose first;
        first.time = 0.0;
        first.x = 0.0;
        first.y = 0.0;
        dropBodyPose(track, first);

        BodyPose second;
        second.time = 2.0;
        second.x = 1.0;
        second.y = -0.5;
        second.rot = 90.0;
        dropBodyPose(track, second);

        if (track.size() != 2)
            return fail("two keyframes did not make a track of two");

        const BodyPose middle = bodyPoseAt(track, 1.0);
        if (!near(middle.x, 0.5, 1e-12) || !near(middle.y, -0.25, 1e-12) ||
            !near(middle.rot, 45.0, 1e-12))
            return fail("the halfway pose is not halfway");

        const std::string motion = bodyTrackToMotion(track);
        if (!contains(motion, "@0,vx=0.5") || !contains(motion, "vy=-0.25") ||
            !contains(motion, "omega=45"))
            return fail("the velocities written out do not carry the body "
                        "from one pose to the next: " + motion);
        if (!contains(motion, "@2,vx=0,vy=0,omega=0"))
            return fail("the track does not stop at its last keyframe: " +
                        motion);
    }

    {
        std::vector<BodyPose> track;
        BodyPose pose;
        pose.time = 0.0;
        pose.x = 0.25;
        pose.interp = "bezier";
        pose.ease = "out";
        dropBodyPose(track, pose);

        BodyPose replacement = pose;
        replacement.x = 0.75;
        dropBodyPose(track, replacement);
        if (track.size() != 1)
            return fail("dropping a keyframe at the same time made a second "
                        "one instead of replacing it");
        if (!near(track.front().x, 0.75, 1e-12))
            return fail("the replacement keyframe did not take");

        BodyPose later;
        later.time = 1.0;
        later.x = 1.0;
        dropBodyPose(track, later);

        const std::string text = formatBodyTrack(track);
        const std::vector<BodyPose> round = parseBodyTrack(text);
        if (round.size() != track.size())
            return fail("the track did not survive a round trip: " + text);
        if (round.front().interp != "bezier" || round.front().ease != "out")
            return fail("the interpolation did not survive a round trip");
        if (!near(round.front().x, 0.75, 1e-9) ||
            !near(round.back().x, 1.0, 1e-9))
            return fail("the positions did not survive a round trip");

        const std::string motion = bodyTrackToMotion(track);
        if (!contains(motion, "interp=bezier") ||
            !contains(motion, "ease=out"))
            return fail("a non-linear keyframe lost its interpolation on the "
                        "way to bodyMotion: " + motion);
    }

    {
        std::vector<BodyPose> track = parseBodyTrack(
            "@t=1,x=1,y=0,rot=0,interp=linear,ease=inout"
            "@t=0,x=0,y=0,rot=0,interp=linear,ease=inout");
        if (track.size() != 2 || track.front().time != 0.0)
            return fail("keyframes given out of order were not sorted");

        if (!removeBodyPose(track, 1.0))
            return fail("removing a keyframe that exists reported nothing "
                        "removed");
        if (track.size() != 1)
            return fail("removing one keyframe removed something else too");
        if (removeBodyPose(track, 5.0))
            return fail("removing a keyframe that is not there reported a "
                        "removal");
        if (!bodyTrackToMotion(track).empty())
            return fail("a single keyframe wrote a bodyMotion entry, and one "
                        "pose is not a motion");
    }

    {
        std::vector<BodyPose> track;
        BodyPose a;
        a.time = 0.0;
        BodyPose b;
        b.time = 0.0;
        b.x = 1.0;
        track.push_back(a);
        track.push_back(b);
        if (!bodyTrackToMotion(track).empty())
            return fail("two keyframes at the same instant produced a "
                        "velocity, and that is a division by zero");
    }

    {
        std::vector<BodyPose> track = parseBodyTrack(
            "@t=0,x=0,y=0,rot=0@t=1,x=2,y=0,rot=0");
        const BodyPose before = bodyPoseAt(track, -5.0);
        const BodyPose after = bodyPoseAt(track, 5.0);
        if (!near(before.x, 0.0, 1e-12))
            return fail("sampling before the first keyframe moved the body");
        if (!near(after.x, 2.0, 1e-12))
            return fail("sampling after the last keyframe moved the body");
    }

    std::cout << "BodyTrackTests OK\n";
    return 0;
}
