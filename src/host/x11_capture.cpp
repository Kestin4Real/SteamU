#include "host/x11_capture.hpp"

#include "common/log.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>

namespace steamu::host {

X11Capture::~X11Capture()
{
    close();
}

bool X11Capture::open(const std::string& monitor, int aspect_w, int aspect_h)
{
    close();
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) {
        LOGE("cannot open X display (is DISPLAY set?)");
        return false;
    }
    if (!XShmQueryExtension(dpy_)) {
        LOGE("X server has no MIT-SHM extension");
        close();
        return false;
    }

    // Pick the monitor.
    int count = 0;
    XRRMonitorInfo* mons = XRRGetMonitors(dpy_, DefaultRootWindow(dpy_), True, &count);
    XRRMonitorInfo* chosen = nullptr;
    for (int i = 0; i < count; ++i) {
        char* name = XGetAtomName(dpy_, mons[i].name);
        const bool match = monitor.empty() ? mons[i].primary : monitor == name;
        if (match && !chosen) chosen = &mons[i];
        XFree(name);
    }
    if (!chosen && monitor.empty() && count > 0) chosen = &mons[0];
    if (!chosen) {
        LOGE("monitor '%s' not found", monitor.c_str());
        if (mons) XRRFreeMonitors(mons);
        close();
        return false;
    }
    char* name = XGetAtomName(dpy_, chosen->name);
    const int mx = chosen->x, my = chosen->y, mw = chosen->width, mh = chosen->height;

    // Largest centered crop with the target aspect ratio (even sizes for YUV).
    if (int64_t(mw) * aspect_h > int64_t(mh) * aspect_w) {
        h_ = mh & ~1;
        w_ = int(int64_t(mh) * aspect_w / aspect_h) & ~1;
    } else {
        w_ = mw & ~1;
        h_ = int(int64_t(mw) * aspect_h / aspect_w) & ~1;
    }
    x_ = mx + (mw - w_) / 2;
    y_ = my + (mh - h_) / 2;
    LOGI("capturing monitor %s (%dx%d at %d,%d): crop %dx%d at %d,%d", name, mw, mh, mx, my, w_,
         h_, x_, y_);
    XFree(name);
    XRRFreeMonitors(mons);

    // Shared-memory image for the crop.
    auto* shm = new XShmSegmentInfo{};
    shm_ = shm;
    image_ = XShmCreateImage(dpy_, DefaultVisual(dpy_, DefaultScreen(dpy_)),
                             unsigned(DefaultDepth(dpy_, DefaultScreen(dpy_))), ZPixmap, nullptr,
                             shm, unsigned(w_), unsigned(h_));
    if (!image_ || image_->bits_per_pixel != 32) {
        LOGE("unsupported X visual (need 32 bpp)");
        close();
        return false;
    }
    shm->shmid = shmget(IPC_PRIVATE, size_t(image_->bytes_per_line) * size_t(h_), IPC_CREAT | 0600);
    shm->shmaddr = image_->data = static_cast<char*>(shmat(shm->shmid, nullptr, 0));
    shm->readOnly = False;
    if (shm->shmaddr == reinterpret_cast<char*>(-1) || !XShmAttach(dpy_, shm)) {
        LOGE("XShmAttach failed");
        close();
        return false;
    }
    XSync(dpy_, False);
    shmctl(shm->shmid, IPC_RMID, nullptr); // freed automatically once detached
    return true;
}

void X11Capture::close()
{
    auto* shm = static_cast<XShmSegmentInfo*>(shm_);
    if (dpy_ && shm && shm->shmaddr && shm->shmaddr != reinterpret_cast<char*>(-1)) {
        XShmDetach(dpy_, shm);
        shmdt(shm->shmaddr);
    }
    if (image_) {
        image_->data = nullptr;
        XDestroyImage(image_);
        image_ = nullptr;
    }
    delete shm;
    shm_ = nullptr;
    if (dpy_) XCloseDisplay(dpy_);
    dpy_ = nullptr;
}

const uint8_t* X11Capture::grab()
{
    if (!image_) return nullptr;
    if (!XShmGetImage(dpy_, DefaultRootWindow(dpy_), image_, x_, y_, AllPlanes)) return nullptr;
    return reinterpret_cast<const uint8_t*>(image_->data);
}

int X11Capture::stride() const
{
    return image_ ? image_->bytes_per_line : 0;
}

} // namespace steamu::host
