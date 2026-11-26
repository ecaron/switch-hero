#pragma once
// Background video: a song's video.webm, played behind the highway.
//
// The video never drives anything. Every frame the game asks for the picture
// at the song's current moment, and the decoder follows: pausing, practice
// loops, restarts and resuming are all just a different moment to show, and a
// jump the queued frames cannot reach becomes a seek. A decoder that cannot
// keep up drops frames; it can never hold the song clock back.
#include "song.hpp"
#include <SDL.h>
#include <cstddef>
#include <memory>
#include <vector>

namespace fret {
// Where in the video the picture belongs. `chartTime` is the moment the
// highway is drawing (visual offset applied), `songOffset` the chart's delay
// against the audio and `videoStart` song.ini's video_start_time, so the
// video stays locked to the audio exactly as the highway is.
inline double videoClock(double chartTime, double songOffset, double videoStart) {
    return chartTime + songOffset + videoStart;
}
// Whether the playhead has moved somewhere the decoder cannot reach by
// decoding forward: backwards past the frame on screen, or so far ahead that
// decoding every frame in between would take longer than seeking.
inline bool videoNeedsSeek(double shown, double ahead, double target) {
    constexpr double backwards = .25, forwards = 2.0;
    return target < shown - backwards || target > ahead + forwards;
}
// Given the queued frames' timestamps, oldest first, how many to throw away
// so the first one left is the newest that is due at `target`. Returns
// `pts.size()` when none is due yet: the next one is still in the future.
inline size_t framesToDrop(const std::vector<double> &pts, double target) {
    constexpr double early = .005; // a frame due within 5 ms counts as due
    size_t due = pts.size();
    for (size_t i = 0; i < pts.size() && pts[i] <= target + early; ++i)
        due = i;
    return due;
}

class BackgroundVideo {
  public:
    BackgroundVideo();
    ~BackgroundVideo();
    BackgroundVideo(const BackgroundVideo &) = delete;
    BackgroundVideo &operator=(const BackgroundVideo &) = delete;
    // False when the game was built without FFmpeg (a desktop build without
    // its libraries); open() then does nothing.
    static bool supported();
    // Starts decoding `file` on a background thread. Opening the same file
    // again keeps the decoder, so a restart only seeks.
    void open(const fs::path &file);
    void close();
    bool active() const;
    // The picture for `seconds` into the video, uploaded to a texture, or null
    // when there is nothing to show: before the video starts, after it ends,
    // while the first frame is still decoding, or if it cannot be played.
    // Main thread only.
    SDL_Texture *frame(SDL_Renderer *renderer, double seconds);
    int width() const;
    int height() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace fret
