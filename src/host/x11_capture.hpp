// Captures a region of one X11 monitor through MIT-SHM (XShmGetImage).
// The region is the largest centered crop with the requested aspect ratio
// (16:10 for the Deck), e.g. 2304x1440 out of a 3440x1440 ultrawide.
#pragma once

#include <cstdint>
#include <string>

typedef struct _XDisplay Display;
struct _XImage;

namespace steamu::host {

class X11Capture {
public:
    X11Capture() = default;
    ~X11Capture();
    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    // monitor: RandR monitor name (e.g. "DP-2"), empty = primary.
    bool open(const std::string& monitor, int aspect_w, int aspect_h);
    void close();

    // Copies the current screen contents. Returns BGRX pixels or nullptr.
    const uint8_t* grab();

    int width() const { return w_; }
    int height() const { return h_; }
    int stride() const;

private:
    Display* dpy_ = nullptr;
    _XImage* image_ = nullptr;
    void* shm_ = nullptr; // XShmSegmentInfo
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
};

} // namespace steamu::host
