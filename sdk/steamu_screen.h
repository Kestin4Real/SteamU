/*
 * SteamU second-screen API
 * ========================
 *
 * Lets a game (or mod) draw its own content on the Steam Deck screen
 * instead of the mirrored monitor, and receive the Deck's touch input.
 * While no client is attached, the Deck mirrors the monitor as usual.
 *
 * Transport (same user only):
 *   - Unix stream socket: $XDG_RUNTIME_DIR/steamu-screen.sock
 *     ($STEAMU_SCREEN_SOCKET overrides the path, e.g. for test setups)
 *   - Pixels: a file in /dev/shm created by the client, holding
 *     `slot_count` frames back to back (slot i at offset i*stride*height).
 *     The host maps it read-only, so frames are never copied over a socket.
 *
 * Messages: sus_header followed by `size` payload bytes, little endian,
 * packed structs.
 *
 *   client -> host  HELLO   sus_hello_msg
 *   host -> client  DISPLAY sus_display_msg  (on hello and whenever it changes)
 *   client -> host  ATTACH  sus_attach_msg   (start showing our frames)
 *   client -> host  FRAME   sus_frame_msg    (slot N holds a new frame)
 *   host -> client  RELEASE sus_release_msg  (host is done with slot N)
 *   host -> client  TOUCH   sus_touch_msg    (Deck touchscreen, 0..1 coords)
 *   client -> host  DETACH  (empty)      (or just close the socket)
 *   client -> host  INPUT_MODE sus_input_mode_msg  (get the Deck's controls)
 *   host -> client  CONTROLLER sus_controller_msg  (every controller report,
 *                                                   ~250 Hz, while subscribed)
 *   client -> host  AUDIO   PCM samples, see "Audio" below
 *   client -> host  AUDIO_MODE sus_audio_mode_msg
 *
 * Controls: a subscribed client receives the Deck's buttons, sticks,
 * triggers, trackpads and motion sensors directly, independent of Steam
 * Input on the PC. With `exclusive` set, the PC's virtual Steam Deck
 * controller is held in a neutral state meanwhile, so Steam's own mapping
 * (e.g. the desktop layout's trackpad mouse) can't act on the same input.
 * Exclusivity can be switched on and off at any time (e.g. off while the
 * game shows a menu that the mouse should drive).
 *
 * Audio: a client can play sound on the Deck's own speakers/headphones only
 * (like the Wii U GamePad speaker), e.g. a map ping or an inventory click.
 * AUDIO messages carry 48 kHz, 2-channel, signed 16-bit interleaved PCM
 * (4 bytes per frame, at most SUS_AUDIO_MAX_FRAMES frames per message).
 * Write it in real time, as it is produced, the way you would feed a sound
 * card; the host keeps at most ~200 ms and drops the oldest audio beyond
 * that. It is played mixed with the PC's desktop audio, unless the client
 * sets AUDIO_MODE mute_desktop, which silences the desktop stream on the
 * Deck while the client stays connected. Audio works whether or not the
 * client is attached to the screen.
 *
 * Slot rule: after FRAME(N) the client must not write slot N until it gets
 * RELEASE(N). With 3 slots a client can always render without waiting.
 *
 * C/C++ usage: in ONE source file
 *     #define STEAMU_SCREEN_IMPLEMENTATION
 *     #include "steamu_screen.h"
 */
#ifndef STEAMU_SCREEN_H
#define STEAMU_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUS_MAGIC 0x53435553u /* "SUCS" */
#define SUS_VERSION 1
#define SUS_SOCKET_NAME "steamu-screen.sock"
#define SUS_MAX_SLOTS 8

enum sus_msg_type {
    SUS_MSG_HELLO = 1,
    SUS_MSG_DISPLAY = 2,
    SUS_MSG_ATTACH = 3,
    SUS_MSG_FRAME = 4,
    SUS_MSG_RELEASE = 5,
    SUS_MSG_TOUCH = 6,
    SUS_MSG_DETACH = 7,
    SUS_MSG_INPUT_MODE = 8,
    SUS_MSG_CONTROLLER = 9,
    SUS_MSG_AUDIO = 10,
    SUS_MSG_AUDIO_MODE = 11,
};

#define SUS_AUDIO_RATE 48000
#define SUS_AUDIO_CHANNELS 2
#define SUS_AUDIO_MAX_FRAMES 960 /* 20 ms per AUDIO message */

enum sus_format {
    SUS_FORMAT_RGBA8 = 1, /* bytes R,G,B,A (OpenGL GL_RGBA / GL_UNSIGNED_BYTE) */
    SUS_FORMAT_BGRA8 = 2, /* bytes B,G,R,A */
};

enum sus_touch_type {
    SUS_TOUCH_DOWN = 1,
    SUS_TOUCH_MOVE = 2,
    SUS_TOUCH_UP = 3,
};

#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint16_t flags;
    uint32_t size;
} sus_header;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char app_name[64];
} sus_hello_msg;

typedef struct {
    uint8_t connected; /* a Deck is connected and showing video */
    uint8_t reserved;
    uint16_t width;    /* Deck display size: render at this size */
    uint16_t height;
    uint16_t refresh_hz;
} sus_display_msg;

typedef struct {
    char shm_path[128]; /* absolute path, must be in /dev/shm/ */
    uint32_t width;
    uint32_t height;
    uint32_t stride;     /* bytes per row */
    uint32_t slot_count; /* 1..SUS_MAX_SLOTS */
    uint32_t format;     /* sus_format */
    uint8_t flip_y;      /* 1 = rows are bottom-up (glReadPixels) */
    uint8_t reserved[3];
} sus_attach_msg;

typedef struct {
    uint32_t slot;
    uint32_t reserved;
    uint64_t time_us; /* client CLOCK_MONOTONIC when rendered (optional) */
} sus_frame_msg;

typedef struct {
    uint32_t slot;
} sus_release_msg;

typedef struct {
    uint8_t type;   /* sus_touch_type */
    uint8_t finger; /* 0..9 */
    uint16_t reserved;
    float x; /* 0 = left, 1 = right */
    float y; /* 0 = top, 1 = bottom */
} sus_touch_msg;

typedef struct {
    uint8_t subscribe; /* 1 = send me CONTROLLER messages */
    uint8_t exclusive; /* 1 = neutralise the PC's virtual controller meanwhile */
    uint16_t reserved;
} sus_input_mode_msg;

typedef struct {
    uint8_t mute_desktop; /* 1 = Deck plays only this client's audio */
    uint8_t reserved[3];
} sus_audio_mode_msg;

/* Button bits, as reported by the Steam Deck (buttons_lo / buttons_hi). */
enum {
    SUS_DECK_R2 = 0x00000001, SUS_DECK_L2 = 0x00000002, /* digital, fully pressed */
    SUS_DECK_R1 = 0x00000004, SUS_DECK_L1 = 0x00000008,
    SUS_DECK_Y = 0x00000010, SUS_DECK_B = 0x00000020, SUS_DECK_X = 0x00000040, SUS_DECK_A = 0x00000080,
    SUS_DECK_DPAD_UP = 0x00000100, SUS_DECK_DPAD_RIGHT = 0x00000200,
    SUS_DECK_DPAD_LEFT = 0x00000400, SUS_DECK_DPAD_DOWN = 0x00000800,
    SUS_DECK_VIEW = 0x00001000, SUS_DECK_STEAM = 0x00002000, SUS_DECK_MENU = 0x00004000,
    SUS_DECK_L5 = 0x00008000, SUS_DECK_R5 = 0x00010000,
    SUS_DECK_LPAD_CLICK = 0x00020000, SUS_DECK_RPAD_CLICK = 0x00040000,
    SUS_DECK_LPAD_TOUCH = 0x00080000, SUS_DECK_RPAD_TOUCH = 0x00100000,
    SUS_DECK_L3 = 0x00400000, SUS_DECK_R3 = 0x04000000,
};
enum { /* buttons_hi */
    SUS_DECK_L4 = 0x00000200, SUS_DECK_R4 = 0x00000400,
    SUS_DECK_LSTICK_TOUCH = 0x00004000, SUS_DECK_RSTICK_TOUCH = 0x00008000,
    SUS_DECK_QAM = 0x00040000,
};

typedef struct {
    uint32_t buttons_lo; /* SUS_DECK_* */
    uint32_t buttons_hi;
    int16_t left_x, left_y; /* sticks: -32768..32767, +x right, +y up */
    int16_t right_x, right_y;
    uint16_t left_trigger, right_trigger; /* 0..32767 */
    int16_t left_pad_x, left_pad_y; /* trackpads, +y up */
    int16_t right_pad_x, right_pad_y;
    uint16_t left_pad_pressure, right_pad_pressure;
    /* SDL axis convention: [0] pitch (x, right), [1] yaw (y, up), [2] roll
     * (z, towards the player). Gyro: 32768 = 2000 deg/s. Accel: 32768 = 2 g. */
    int16_t gyro[3];
    int16_t accel[3];
    uint32_t seq; /* increments per report */
} sus_controller_msg;
#pragma pack(pop)

/* ---- Client API ------------------------------------------------------- */

typedef struct sus_client sus_client;

typedef struct {
    int type; /* SUS_MSG_DISPLAY, SUS_MSG_TOUCH or SUS_MSG_CONTROLLER */
    sus_display_msg display;
    sus_touch_msg touch;
    sus_controller_msg controller;
} sus_event;

/* Connects to a running steamu-host. NULL if it is not running. */
sus_client* sus_connect(const char* app_name);
/* Creates the shared frame buffer and starts showing it on the Deck. */
int sus_attach(sus_client* c, uint32_t width, uint32_t height, uint32_t format, int flip_y);
/* Pointer to a free slot to draw into, or NULL if all slots are in use. */
void* sus_acquire(sus_client* c);
uint32_t sus_stride(const sus_client* c);
/* Hands the slot from sus_acquire() to the host. */
int sus_submit(sus_client* c);
/* Non-blocking. Returns 1 and fills *ev if an event was pending, 0 if
 * none, -1 if the host went away. */
int sus_poll(sus_client* c, sus_event* ev);
/* Socket fd, for use with poll()/select(). */
int sus_fd(const sus_client* c);
/* Subscribe to the Deck's controls (CONTROLLER events), see above. */
int sus_set_input_mode(sus_client* c, int subscribe, int exclusive);
/* Plays 48 kHz stereo s16 interleaved PCM on the Deck (any number of
 * frames; split into messages as needed). See "Audio" above. */
int sus_audio_write(sus_client* c, const int16_t* pcm, uint32_t frames);
/* mute_desktop = 1: the Deck plays only this client's audio. */
int sus_set_audio_mode(sus_client* c, int mute_desktop);
void sus_disconnect(sus_client* c);

#ifdef __cplusplus
}
#endif

/* ---- Implementation --------------------------------------------------- */
#ifdef STEAMU_SCREEN_IMPLEMENTATION

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct sus_client {
    int fd;
    int shm_fd;
    char shm_path[128];
    uint8_t* pixels;
    size_t slot_size;
    uint32_t stride, slots;
    uint8_t busy[SUS_MAX_SLOTS];
    int acquired; /* slot handed out by sus_acquire, -1 if none */
    uint8_t rx[256];
    size_t rx_len;
};

static int sus__send(sus_client* c, uint16_t type, const void* data, uint32_t size)
{
    uint8_t buf[sizeof(sus_header) + SUS_AUDIO_MAX_FRAMES * 4];
    sus_header h;
    size_t total = sizeof(h) + size, off = 0;
    h.type = type;
    h.flags = 0;
    h.size = size;
    if (size > SUS_AUDIO_MAX_FRAMES * 4) return -1;
    memcpy(buf, &h, sizeof(h));
    if (size) memcpy(buf + sizeof(h), data, size);
    while (off < total) {
        ssize_t n = send(c->fd, buf + off, total - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* The socket is non-blocking: wait (briefly) instead of
                 * leaving half a message on the wire. */
                struct pollfd p;
                p.fd = c->fd;
                p.events = POLLOUT;
                p.revents = 0;
                if (poll(&p, 1, 1000) <= 0) return -1;
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

sus_client* sus_connect(const char* app_name)
{
    struct sockaddr_un addr;
    const char* dir = getenv("XDG_RUNTIME_DIR");
    const char* path = getenv("STEAMU_SCREEN_SOCKET");
    sus_client* c;
    sus_hello_msg hello;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path && *path) snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    else if (dir) snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", dir, SUS_SOCKET_NAME);
    else return NULL;

    c = (sus_client*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->shm_fd = -1;
    c->acquired = -1;
    c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0 || connect(c->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (c->fd >= 0) close(c->fd);
        free(c);
        return NULL;
    }
    memset(&hello, 0, sizeof(hello));
    hello.magic = SUS_MAGIC;
    hello.version = SUS_VERSION;
    snprintf(hello.app_name, sizeof(hello.app_name), "%s", app_name ? app_name : "app");
    if (sus__send(c, SUS_MSG_HELLO, &hello, sizeof(hello)) < 0) {
        sus_disconnect(c);
        return NULL;
    }
    fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK);
    return c;
}

int sus_attach(sus_client* c, uint32_t width, uint32_t height, uint32_t format, int flip_y)
{
    sus_attach_msg a;
    size_t total;
    c->slots = 3;
    c->stride = width * 4;
    c->slot_size = (size_t)c->stride * height;
    total = c->slot_size * c->slots;
    snprintf(c->shm_path, sizeof(c->shm_path), "/dev/shm/steamu-screen-%d", (int)getpid());
    c->shm_fd = open(c->shm_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (c->shm_fd < 0 || ftruncate(c->shm_fd, (off_t)total) < 0) return -1;
    c->pixels = (uint8_t*)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, c->shm_fd, 0);
    if (c->pixels == MAP_FAILED) {
        c->pixels = NULL;
        return -1;
    }
    memset(&a, 0, sizeof(a));
    snprintf(a.shm_path, sizeof(a.shm_path), "%s", c->shm_path);
    a.width = width;
    a.height = height;
    a.stride = c->stride;
    a.slot_count = c->slots;
    a.format = format;
    a.flip_y = flip_y ? 1 : 0;
    return sus__send(c, SUS_MSG_ATTACH, &a, sizeof(a));
}

void* sus_acquire(sus_client* c)
{
    uint32_t i;
    if (!c->pixels) return NULL;
    if (c->acquired >= 0) return c->pixels + (size_t)c->acquired * c->slot_size;
    for (i = 0; i < c->slots; ++i) {
        if (!c->busy[i]) {
            c->acquired = (int)i;
            return c->pixels + (size_t)i * c->slot_size;
        }
    }
    return NULL;
}

uint32_t sus_stride(const sus_client* c) { return c->stride; }

int sus_submit(sus_client* c)
{
    sus_frame_msg f;
    struct timespec ts;
    if (c->acquired < 0) return -1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    memset(&f, 0, sizeof(f));
    f.slot = (uint32_t)c->acquired;
    f.time_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    c->busy[c->acquired] = 1;
    c->acquired = -1;
    return sus__send(c, SUS_MSG_FRAME, &f, sizeof(f));
}

int sus_poll(sus_client* c, sus_event* ev)
{
    for (;;) {
        sus_header h;
        if (c->rx_len >= sizeof(h)) {
            memcpy(&h, c->rx, sizeof(h));
            if (h.size > sizeof(c->rx) - sizeof(h)) return -1;
            if (c->rx_len >= sizeof(h) + h.size) {
                const uint8_t* p = c->rx + sizeof(h);
                int got = 0;
                if (h.type == SUS_MSG_RELEASE && h.size >= sizeof(sus_release_msg)) {
                    sus_release_msg r;
                    memcpy(&r, p, sizeof(r));
                    if (r.slot < SUS_MAX_SLOTS) c->busy[r.slot] = 0;
                } else if (h.type == SUS_MSG_DISPLAY && h.size >= sizeof(sus_display_msg)) {
                    ev->type = SUS_MSG_DISPLAY;
                    memcpy(&ev->display, p, sizeof(ev->display));
                    got = 1;
                } else if (h.type == SUS_MSG_TOUCH && h.size >= sizeof(sus_touch_msg)) {
                    ev->type = SUS_MSG_TOUCH;
                    memcpy(&ev->touch, p, sizeof(ev->touch));
                    got = 1;
                } else if (h.type == SUS_MSG_CONTROLLER && h.size >= sizeof(sus_controller_msg)) {
                    ev->type = SUS_MSG_CONTROLLER;
                    memcpy(&ev->controller, p, sizeof(ev->controller));
                    got = 1;
                }
                c->rx_len -= sizeof(h) + h.size;
                memmove(c->rx, c->rx + sizeof(h) + h.size, c->rx_len);
                if (got) return 1;
                continue;
            }
        }
        {
            ssize_t n = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len, 0);
            if (n == 0) return -1;
            if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
            c->rx_len += (size_t)n;
        }
    }
}

int sus_fd(const sus_client* c) { return c->fd; }

int sus_set_input_mode(sus_client* c, int subscribe, int exclusive)
{
    sus_input_mode_msg m;
    memset(&m, 0, sizeof(m));
    m.subscribe = subscribe ? 1 : 0;
    m.exclusive = exclusive ? 1 : 0;
    return sus__send(c, SUS_MSG_INPUT_MODE, &m, sizeof(m));
}

int sus_audio_write(sus_client* c, const int16_t* pcm, uint32_t frames)
{
    while (frames > 0) {
        uint32_t n = frames < SUS_AUDIO_MAX_FRAMES ? frames : SUS_AUDIO_MAX_FRAMES;
        if (sus__send(c, SUS_MSG_AUDIO, pcm, n * 4) < 0) return -1;
        pcm += n * SUS_AUDIO_CHANNELS;
        frames -= n;
    }
    return 0;
}

int sus_set_audio_mode(sus_client* c, int mute_desktop)
{
    sus_audio_mode_msg m;
    memset(&m, 0, sizeof(m));
    m.mute_desktop = mute_desktop ? 1 : 0;
    return sus__send(c, SUS_MSG_AUDIO_MODE, &m, sizeof(m));
}

void sus_disconnect(sus_client* c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    if (c->pixels) munmap(c->pixels, c->slot_size * c->slots);
    if (c->shm_fd >= 0) {
        close(c->shm_fd);
        unlink(c->shm_path);
    }
    free(c);
}

#endif /* STEAMU_SCREEN_IMPLEMENTATION */
#endif /* STEAMU_SCREEN_H */
