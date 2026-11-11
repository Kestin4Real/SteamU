# Second-screen API quick start

With the SteamU second-screen API, your game or mod can:

* **draw its own picture on the Deck** instead of the mirrored monitor (a
  map, an inventory, a second camera...),
* **receive the Deck's touchscreen**,
* **read the Deck's controls directly** (buttons, sticks, triggers,
  trackpads, gyro, at 250 Hz), and optionally take them away from Steam
  Input while your game has focus,
* **play sound on the Deck only**, like the Wii U GamePad speaker.

When your app disconnects, or `steamu-host` isn't running, nothing breaks:
the Deck simply mirrors the monitor again. That makes it safe to ship the
feature on by default.

Everything is in one header, [`sdk/steamu_screen.h`](../sdk/steamu_screen.h).
It has no dependencies and works from C and C++. For other languages, see
[Other languages](#other-languages).

## 1. Connect and attach

```c
#define STEAMU_SCREEN_IMPLEMENTATION   /* in exactly one .c/.cpp file */
#include "steamu_screen.h"

sus_client* deck = sus_connect("My Game");      /* NULL: SteamU isn't running */
if (deck) {
    /* The host sends the Deck's display size right after connecting. */
    sus_event ev;
    int w = 1280, h = 800;
    /* ...poll until ev.type == SUS_MSG_DISPLAY, then w = ev.display.width... */
    sus_attach(deck, w, h, SUS_FORMAT_RGBA8, /*flip_y=*/0);
}
```

Render at the size from the `DISPLAY` event: it is the Deck's native
resolution (1280×800). Any other size gets scaled.

## 2. Send frames

```c
void* pixels = sus_acquire(deck);         /* NULL if all 3 slots are busy */
if (pixels) {
    draw_my_ui(pixels, sus_stride(deck)); /* w*h RGBA, stride bytes per row */
    sus_submit(deck);
}
```

* There are 3 slots, so you can always draw while the host is still
  encoding the previous frame. If `sus_acquire` returns NULL, skip this
  frame; don't wait for a slot.
* Only submit when something changed. Each frame is encoded as soon as it
  arrives, so a static screen costs nothing.
* **OpenGL:** `glReadPixels` returns rows bottom-up. Attach with
  `flip_y = 1` and pass the buffer as is; the host flips it for free. For
  no stall, read back asynchronously (PBO) and submit a frame later.
* `SUS_FORMAT_BGRA8` is also accepted.

## 3. Handle events

`sus_poll` never blocks. Call it once per frame until it returns 0 (use
`sus_fd()` with `poll()` if you prefer).

```c
sus_event ev;
int r;
while ((r = sus_poll(deck, &ev)) == 1) {
    switch (ev.type) {
    case SUS_MSG_DISPLAY:      /* Deck (dis)connected or resized */
        break;
    case SUS_MSG_TOUCH:        /* ev.touch.x/y in 0..1, finger 0..9 */
        if (ev.touch.type == SUS_TOUCH_DOWN) on_tap(ev.touch.x * w, ev.touch.y * h);
        break;
    case SUS_MSG_CONTROLLER:   /* only after sus_set_input_mode(deck, 1, ...) */
        if (ev.controller.buttons_lo & SUS_DECK_A) jump();
        break;
    }
}
if (r < 0) { sus_disconnect(deck); deck = NULL; }   /* host went away */
```

Touch coordinates are relative to your picture: (0, 0) is its top-left
corner and (1, 1) its bottom-right.

## 4. Controls (optional)

```c
sus_set_input_mode(deck, /*subscribe=*/1, /*exclusive=*/1);
```

* **subscribe:** you receive a `SUS_MSG_CONTROLLER` event for every
  controller report (about 250 Hz). Sticks are -32768..32767 with +y up;
  triggers are 0..32767.
* **Gyro and accelerometer** use the SDL axis convention: `gyro[0]` is
  pitch, `gyro[1]` is yaw and `gyro[2]` is roll; 32768 = 2000 °/s.
* **exclusive:** the PC's virtual controller is held neutral meanwhile, so
  Steam Input (mouse emulation, a game's own controller support...) doesn't
  also act on the same buttons.
* **When to switch:** you can change the mode at any time. A good pattern is
  exclusive while your game window has focus and not exclusive when it
  doesn't, so the Deck still drives the desktop when you alt-tab.
* **Button bits:** the bits are the `SUS_DECK_*` constants. The back
  buttons are `SUS_DECK_L4`/`R4` in `buttons_hi` and `SUS_DECK_L5`/`R5` in
  `buttons_lo`.

## 5. Deck-only audio (optional)

```c
int16_t pcm[2 * 240];                     /* 5 ms, 48 kHz, stereo interleaved */
fill_my_sound(pcm, 240);
sus_audio_write(deck, pcm, 240);
sus_set_audio_mode(deck, /*mute_desktop=*/0);  /* 1: Deck plays only your audio */
```

* Write audio **in real time**, as you produce it, like feeding a sound
  card. The host keeps at most ~200 ms and drops the oldest audio beyond
  that.
* It is mixed with the PC's desktop sound on the Deck, and is never played
  on the PC.
* Audio works with or without an attached screen.

## 6. Done

```c
sus_disconnect(deck);                     /* the Deck goes back to mirroring */
```

Closing the connection is all it takes, and it also happens when your
process exits or crashes. Nothing has to be cleaned up on the host.

## Complete example

A compact but complete client: it fills the screen with a colour that
follows your finger, clicks on every tap and quits when you press B.
`tools/screentest.c` is a longer one.

```c
#define STEAMU_SCREEN_IMPLEMENTATION
#include "steamu_screen.h"
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    sus_client* deck = sus_connect("example");
    if (!deck) { fprintf(stderr, "steamu-host is not running\n"); return 1; }

    sus_event ev;
    int w = 1280, h = 800, have_display = 0;
    while (!have_display) {                      /* wait for the display size */
        struct pollfd p = {sus_fd(deck), POLLIN, 0};
        if (poll(&p, 1, 2000) <= 0) return 1;
        while (sus_poll(deck, &ev) == 1)
            if (ev.type == SUS_MSG_DISPLAY) {
                w = ev.display.width;
                h = ev.display.height;
                have_display = 1;
            }
    }
    sus_attach(deck, (uint32_t)w, (uint32_t)h, SUS_FORMAT_RGBA8, 0);
    sus_set_input_mode(deck, 1, 0);              /* read buttons, keep Steam Input */

    float tx = 0.5f, ty = 0.5f;
    int running = 1, dirty = 1, click = 0;
    double start = now_s();
    long long written = 0;                       /* audio frames sent so far */
    while (running) {
        struct pollfd p = {sus_fd(deck), POLLIN, 0};
        poll(&p, 1, 5);                          /* ~5 ms tick */
        int r;
        while ((r = sus_poll(deck, &ev)) == 1) {
            if (ev.type == SUS_MSG_TOUCH && ev.touch.type != SUS_TOUCH_UP) {
                tx = ev.touch.x;
                ty = ev.touch.y;
                dirty = 1;
                if (ev.touch.type == SUS_TOUCH_DOWN) click = 2880; /* 60 ms */
            }
            if (ev.type == SUS_MSG_CONTROLLER && (ev.controller.buttons_lo & SUS_DECK_B))
                running = 0;
        }
        if (r < 0) break;                        /* host went away */

        /* Audio paced by the clock: send exactly what is due since the last
         * tick (a decaying 880 Hz click after a tap, silence otherwise). */
        long long due = (long long)((now_s() - start) * SUS_AUDIO_RATE);
        while (written < due) {
            int16_t pcm[2 * 480];
            int n = due - written > 480 ? 480 : (int)(due - written);
            for (int i = 0; i < n; ++i) {
                float v = click > 0 ? 0.3f * sinf((float)written * 6.2832f * 880 / 48000) * click / 2880 : 0;
                pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(v * 32767);
                if (click > 0) --click;
                ++written;
            }
            sus_audio_write(deck, pcm, (uint32_t)n);
        }

        uint8_t* px = dirty ? (uint8_t*)sus_acquire(deck) : NULL;
        if (px) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint8_t* q = px + (size_t)y * sus_stride(deck) + (size_t)x * 4;
                    q[0] = (uint8_t)(tx * 255);
                    q[1] = (uint8_t)(ty * 255);
                    q[2] = 128;
                    q[3] = 255;
                }
            sus_submit(deck);
            dirty = 0;
        }
    }
    sus_disconnect(deck);
    return 0;
}
```

```bash
cc -O2 -I sdk example.c -o example -lm && ./example
```

## Testing

* `steamu-host` must be running. The Deck isn't needed: run
  `steamu-deck --fake-idle --windowed --host 127.0.0.1` on the PC and click
  into its window to simulate touch.
* To keep your real host undisturbed, start a test host with its own socket
  and ports, and point your app at the same socket:

  ```bash
  STEAMU_SCREEN_SOCKET=$XDG_RUNTIME_DIR/steamu-test.sock ./build/steamu-host --port 28850
  STEAMU_SCREEN_SOCKET=$XDG_RUNTIME_DIR/steamu-test.sock ./my-game
  ```

* `steamu-host` logs connects, attaches, input mode changes and audio
  activity, so check its output first when something doesn't show up.

## Other languages

The protocol is small and documented at the top of `steamu_screen.h`:

* **Messages:** an 8-byte header (`type`, `flags`, `size`) followed by a
  packed little-endian struct, sent over a Unix stream socket.
* **Frames:** pixels go through a `/dev/shm` file that your process creates
  and the host maps.
* **Fixed sizes:** every message must be sent with exactly the struct size
  from the header (e.g. `sus_attach_msg` is 152 bytes), padding included.

The [SteamU Companion](https://github.com/Kestin4Real/SteamU-MC-Companion)
Minecraft mod implements the
protocol in pure Java (`ScreenClient.java`) and is a good reference for
JVM or other managed languages. It renders with OpenGL, reads back
asynchronously and uses exclusive controls while in game.

## Rules of thumb

* Keep everything on the Deck screen big: at 1280×800 on a 7" screen, touch
  targets of 80–100 px are comfortable.
* Never block your game loop on SteamU: poll, skip a frame when no slot is
  free, and treat a failed call as "SteamU went away".
* Only claim exclusive controls while your game actually uses them.
* Only one app can be connected at a time. A second one is refused until
  the first disconnects.
