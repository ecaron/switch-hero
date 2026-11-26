// The background video's timing rules. Decoding needs FFmpeg and a console to
// judge; what is checked here is where the picture is meant to be and when the
// decoder must seek, which is where a video drifts or stalls if it is wrong.
#include "video.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace fret;
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
void near(double a, double b, const char *message) { check(std::abs(a - b) < 1e-9, message); }

int main() {
    try {
        // The video follows the audio: chart time plus the chart's delay is
        // the audio position, and video_start_time moves into the video.
        near(videoClock(10, 0, 0), 10, "no offsets: video time is chart time");
        near(videoClock(10, .25, 0), 10.25, "the chart delay is audio time, which the video follows");
        near(videoClock(10, .25, 1.5), 11.75, "a positive start time skips into the video");
        near(videoClock(1, 0, -2), -1, "a negative start time holds the video back");
        near(videoClock(-2, 0, 0), -2, "the lead-in is before the video starts");

        // Seeking: only when decoding forward cannot get there in time.
        check(!videoNeedsSeek(10, 10.1, 10.05), "normal playback decodes forward");
        check(!videoNeedsSeek(10, 10.1, 10), "paused on the frame shown");
        check(!videoNeedsSeek(10, 10.1, 9.9), "a hair back (clock jitter) is not a seek");
        check(videoNeedsSeek(10, 10.1, 9), "back a second: a restart or practice loop seeks");
        check(videoNeedsSeek(10, 10.1, 0), "restarting the song seeks to the start");
        check(videoNeedsSeek(10, 10.1, 30), "a practice section far ahead seeks");
        check(!videoNeedsSeek(10, 10.1, 11.5), "a short gap is decoded through");
        check(videoNeedsSeek(10, 12, 14.5), "a decoder too far behind seeks ahead");

        // Picking the frame for this moment from the queue.
        const std::vector<double> q = {1.0, 1.033, 1.067, 1.1};
        check(framesToDrop(q, .9) == q.size(), "nothing due yet: keep waiting");
        check(framesToDrop(q, 1.0) == 0, "the first frame is due");
        check(framesToDrop(q, 1.05) == 1, "show the newest due frame, drop the older one");
        check(framesToDrop(q, 1.2) == 3, "behind: skip to the last frame");
        check(framesToDrop(q, .997) == 0, "a frame due within 5 ms is shown now");
        check(framesToDrop({}, 5) == 0, "an empty queue has nothing to drop");
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "Video timing: " << checks << " checks passed\n";
    return 0;
}
