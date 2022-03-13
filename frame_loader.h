// Interfaces to preload and cache frames from media files.

#pragma once

#include <functional>
#include <map>
#include <memory>

#include "media_decoder.h"
#include "display_output.h"
#include "interval_set.h"
#include "thread_signal.h"
#include "unix_system.h"

namespace pivid {

// Interface to an asynchronous thread that loads frames from media into GPU.
class FrameLoader {
  public:
    // Currently loaded frames.
    struct Loaded {
        std::map<Seconds, std::shared_ptr<LoadedImage>> frames;
        IntervalSet<Seconds> have;   // Regions that are fully loaded
        std::optional<Seconds> eof;  // Where EOF is, if known
    };

    // Interrupts and shuts down the frame loader.
    virtual ~FrameLoader() = default;

    // Sets the regions of interest to load, discarding frames outside them.
    virtual void set_request(
        IntervalSet<Seconds> const&,
        std::shared_ptr<ThreadSignal> = {}
    ) = 0;

    // Returns the frames loaded so far.
    virtual Loaded loaded() const = 0;
};

// Creates a frame loader instance for a given media file and GPU device.
std::unique_ptr<FrameLoader> make_frame_loader(
    DisplayDriver*,
    std::string const& filename,
    std::function<std::unique_ptr<MediaDecoder>(std::string const&)> =
        open_media_decoder
);

// Debugging descriptions of data structures.
std::string debug(Interval<Seconds> const&);
std::string debug(IntervalSet<Seconds> const&);

}  // namespace pivid