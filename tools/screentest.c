/*
 * steamu-screentest: minimal second-screen API client.
 * Draws an animated pattern on the Deck and a dot under every finger, and
 * plays a click on the Deck's speakers for every touch (Deck-only audio).
 *
 *   ./steamu-screentest [--seconds N] [--tone] [--mute-desktop]
 *
 *   --tone          also play a quiet continuous 440 Hz tone on the Deck
 *   --mute-desktop  the Deck plays only this app's audio while it runs
 */
#define STEAMU_SCREEN_IMPLEMENTATION
#include "steamu_screen.h"

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Deck-only audio: a click per touch (pitch follows x), plus an optional tone. */
static double g_click_until = 0, g_click_freq = 880, g_phase = 0, g_tone_phase = 0;

static void write_audio(sus_client* c, int tone, double t, uint64_t* written)
{
    int16_t pcm[2 * 960];
    uint64_t due = (uint64_t)(t * SUS_AUDIO_RATE);
    while (*written < due) {
        uint32_t n = (uint32_t)(due - *written), k;
        if (n > 960) n = 960;
        for (k = 0; k < n; ++k) {
            double ts = (double)(*written + k) / SUS_AUDIO_RATE, v = 0;
            if (tone) {
                g_tone_phase += 2 * M_PI * 440 / SUS_AUDIO_RATE;
                v += 0.08 * sin(g_tone_phase);
            }
            if (ts < g_click_until) {
                g_phase += 2 * M_PI * g_click_freq / SUS_AUDIO_RATE;
                v += 0.4 * sin(g_phase) * (g_click_until - ts) / 0.06;
            }
            pcm[2 * k] = pcm[2 * k + 1] = (int16_t)(v * 32767);
        }
        if (sus_audio_write(c, pcm, n) < 0) return;
        *written += n;
    }
}

int main(int argc, char** argv)
{
    int seconds = 0, tone = 0, mute_desktop = 0, i;
    uint64_t audio_written = 0;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tone")) tone = 1;
        else if (!strcmp(argv[i], "--mute-desktop")) mute_desktop = 1;
        else {
            fprintf(stderr, "usage: steamu-screentest [--seconds N] [--tone] [--mute-desktop]\n");
            return 2;
        }
    }

    sus_client* c = sus_connect("steamu-screentest");
    if (!c) {
        fprintf(stderr, "steamu-host is not running\n");
        return 1;
    }

    /* Wait for the display info (sent right after connecting). */
    sus_event ev;
    int w = 1280, h = 800, got = 0;
    double deadline = now_s() + 2;
    while (!got && now_s() < deadline) {
        struct pollfd p = {sus_fd(c), POLLIN, 0};
        poll(&p, 1, 100);
        while (sus_poll(c, &ev) == 1)
            if (ev.type == SUS_MSG_DISPLAY) {
                w = ev.display.width;
                h = ev.display.height;
                got = 1;
                printf("display: %dx%d @ %d Hz, deck %s\n", w, h, ev.display.refresh_hz,
                       ev.display.connected ? "connected" : "not connected");
            }
    }
    if (sus_attach(c, (uint32_t)w, (uint32_t)h, SUS_FORMAT_RGBA8, 0) < 0) {
        fprintf(stderr, "attach failed\n");
        return 1;
    }

    if (mute_desktop) sus_set_audio_mode(c, 1);

    float fx[10], fy[10];
    int fdown[10] = {0};
    unsigned frames = 0, touches = 0;
    double start = now_s(), next = start;
    for (;;) {
        double t = now_s() - start;
        if (seconds && t > seconds) break;

        int r;
        while ((r = sus_poll(c, &ev)) == 1) {
            if (ev.type == SUS_MSG_TOUCH && ev.touch.finger < 10) {
                int f = ev.touch.finger;
                fx[f] = ev.touch.x;
                fy[f] = ev.touch.y;
                fdown[f] = ev.touch.type != SUS_TOUCH_UP;
                ++touches;
                if (ev.touch.type == SUS_TOUCH_DOWN) {
                    g_click_until = t + 0.06;
                    g_click_freq = 440 + 880 * ev.touch.x;
                }
                if (ev.touch.type != SUS_TOUCH_MOVE)
                    printf("touch %s finger %d at %.3f, %.3f\n",
                           ev.touch.type == SUS_TOUCH_DOWN ? "down" : "up", f, ev.touch.x,
                           ev.touch.y);
            }
        }
        if (r < 0) {
            fprintf(stderr, "host went away\n");
            break;
        }

        write_audio(c, tone, t, &audio_written);

        uint8_t* px = (uint8_t*)sus_acquire(c);
        if (px) {
            uint32_t stride = sus_stride(c);
            int bar = (int)(t * 300) % w, x, y;
            for (y = 0; y < h; ++y) {
                uint8_t* row = px + (size_t)y * stride;
                for (x = 0; x < w; ++x) {
                    uint8_t* p = row + x * 4;
                    p[0] = (uint8_t)(x * 255 / w);
                    p[1] = (uint8_t)(y * 255 / h);
                    p[2] = (x > bar - 20 && x < bar + 20) ? 255 : 64;
                    p[3] = 255;
                }
            }
            for (i = 0; i < 10; ++i) {
                int cx = (int)(fx[i] * w), cy = (int)(fy[i] * h);
                if (!fdown[i]) continue;
                for (y = cy - 30; y <= cy + 30; ++y)
                    for (x = cx - 30; x <= cx + 30; ++x)
                        if (x >= 0 && y >= 0 && x < w && y < h &&
                            (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 900)
                            memset(px + (size_t)y * stride + x * 4, 255, 4);
            }
            sus_submit(c);
            ++frames;
        }
        next += 1.0 / 60;
        double wait = next - now_s();
        if (wait > 0) {
            struct timespec ts = {0, (long)(wait * 1e9)};
            nanosleep(&ts, NULL);
        } else {
            next = now_s();
        }
    }
    printf("sent %u frames, received %u touch events\n", frames, touches);
    sus_disconnect(c);
    return 0;
}
