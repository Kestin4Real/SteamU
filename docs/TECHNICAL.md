# SteamU technical overview

How SteamU works inside, and why it is built this way. For setup and usage
see the [README](../README.md). For writing an app that uses the Deck
screen, see the [API quick start](API-QUICKSTART.md).

* [Components](#components)
* [Network](#network)
* [Controller passthrough](#controller-passthrough)
* [Video](#video)
* [Audio](#audio)
* [Second-screen API](#second-screen-api)
* [OBS output](#obs-output)
* [Source layout](#source-layout)
* [Testing without a Deck](#testing-without-a-deck)
* [Design notes](#design-notes)

## Components

| Program | Runs on | Job |
|---|---|---|
| `steamu-host` | PC | Virtual Steam Deck controller, screen capture and encoding, audio capture, second-screen API server |
| `steamu-deck` | Deck | Reads the built-in controller, plays the video and audio, forwards touch |
| `steamu-screentest` | PC | Example second-screen API client (pattern, touch dots, Deck-only clicks) |
| `steamu-inputtest` | PC | Shows what an SDL game sees of the controller |

`steamu-deck` is a single file. SDL 3.4, a minimal FFmpeg 7.1 (HEVC/H.264
and Opus decoders, VAAPI) and libstdc++ are linked in statically. Only glibc
(2.38 or newer, so SteamOS 3.6 or newer) and libva come from the system.
The host uses the system FFmpeg, whose Ubuntu/Mint build includes NVENC.

## Network

| Port | Protocol | Direction | Content |
|---|---|---|---|
| 27850 | TCP | Deck → PC | Control: handshake, HID feature/output reports, ping, touch, stream setup |
| 27851 | UDP | Deck → PC | Controller input reports (250 Hz) |
| 27852 | UDP | Deck → PC | Discovery broadcast (the Deck finds the PC by itself) |
| chosen by the Deck | UDP | PC → Deck | Video |
| chosen by the Deck | UDP | PC → Deck | Audio |
| 27860 | UDP | PC → local | Optional OBS copy of the video (MPEG-TS) |

`--port N` moves the first three to N..N+2, so a second (test) host can run
next to the real one.

The wire format is in [`src/common/protocol.hpp`](../src/common/protocol.hpp).
It uses packed little-endian structs, with a magic number and a protocol
version (currently 5) checked in the handshake. **The host and Deck binaries
must come from the same version.**

Wi-Fi (WMM) has four priority queues, selected by the DSCP field.
Controller input, control traffic and audio are small and latency critical,
so they are marked EF (voice). Video is marked AF41 (video). Putting the
20 Mbit/s video in the voice queue once made the whole network stutter.

## Controller passthrough

```
 Steam Deck                                     PC
 ┌──────────────────────┐   UDP 27851  ┌───────────────────────────────┐
 │ /dev/hidrawN         │ ───────────▶ │ virtual USB device (USB/IP,   │
 │ (built-in controller)│  raw input   │ vhci-hcd): same descriptors,  │
 │                      │   reports    │ strings and interfaces        │
 │   steamu-deck        │              │   steamu-host                 │
 │                      │ ◀─────────── │                               │
 │ executes feature /   │   TCP 27850  │ Steam / SDL / hid-steam send  │
 │ output reports       │  (reliable)  │ feature reports (gyro on,     │
 └──────────────────────┘              │ lizard mode off, haptics...)  │
                                       └───────────────────────────────┘
```

* **Handshake.** The Deck sends its controller's exact USB device and
  configuration descriptors, strings and HID report descriptors, read from
  sysfs (`hidraw_backend.cpp`).
* **Virtual device.** The host plugs in an identical USB device through the
  kernel's USB/IP virtual host controller (`vhci-hcd`). It attaches one end
  of a socketpair via sysfs and answers the USB/IP URB protocol itself
  (`usbip_device.cpp`), so no `usbipd` is needed. Steam, SDL and the kernel's
  `hid-steam` driver see the same device as a Deck plugged in by cable.
* **Input reports** go over UDP with a sequence number. A packet older than
  the last one applied is dropped, so a late packet never holds up newer
  ones (no TCP head-of-line blocking over Wi-Fi). The host logs reports/s,
  lost and late packets, and the round-trip time every 5 seconds.
* **Feature and output reports** (GET/SET_REPORT control transfers) are
  forwarded over TCP to the real controller (`HIDIOCGFEATURE` /
  `HIDIOCSFEATURE`) and the reply goes back to the USB request. This is how
  Steam on the PC turns on the gyro, switches off lizard mode and drives
  the haptics.
* **Exclusive mode.** While a second-screen app subscribes to the controls
  exclusively, the virtual controller is held in a neutral state, so
  Steam's own mapping can't act on the same input.

## Video

```
X11 XShm grab ─▶ swscale (crop to 16:10, BGRx → NV12 BT.709) ─▶ NVENC HEVC ─▶ UDP chunks
                                                                                 │
SDL3 texture ◀─ VAAPI decode (NV12) ◀─ reassemble frame ◀────────────────────────┘
```

* **Capture.** `x11_capture.cpp` uses XShm on the chosen RandR monitor
  (default: the primary). It picks the largest centred 16:10 area of the
  monitor, which on an ultrawide means cropping the sides.
* **Scaling.** Multithreaded swscale converts that area to 1280×800 NV12,
  BT.709 limited range. The shared-memory image is wrapped without copying.
* **Encoding.** NVENC (`hevc_nvenc`, or `h264_nvenc` with `--codec h264`)
  uses preset p1, tune ull, CBR, zero latency and no B-frames, and a VBV of
  one frame, so every frame fits in one frame time on the link.
* **Keyframes.** The GOP is infinite: keyframes are only sent when the Deck
  asks for one after a loss. With OBS output on, one is also sent every 2 s.
* **Transport.** Each frame is split into 1400-byte UDP datagrams with a
  header (frame number, chunk index/count, capture time, codec). The Deck
  reassembles them and drops incomplete frames, then requests a keyframe
  instead of showing a corrupted picture.
* **Deck side.** The Deck decodes with VAAPI (software decoding is the
  fallback, e.g. when testing on an NVIDIA PC), uploads to an SDL texture
  and presents without vsync by default (`--vsync` to avoid tearing).
* **Latency.** About 7 ms from capture to present on loopback. The Deck
  syncs its clock to the PC's via ping and logs capture→present latency
  every 5 seconds.
* **Pacing.** In mirror mode the host captures at the Deck's refresh rate
  (`--fps` overrides). With a second-screen app attached it is event
  driven: each submitted frame is encoded immediately.

## Audio

```
PulseAudio/PipeWire monitor ─▶ Opus (5 ms, restricted low delay) ─▶ UDP ─┐
second-screen app PCM ───────▶ Opus ─────────────────────────────▶ UDP ─┤
                                                                         ▼
               SDL3 audio device (mixes) ◀─ jitter buffer ◀─ Opus decode (per stream)
```

* **Desktop stream.** `audio_streamer.cpp` records `@DEFAULT_MONITOR@` (what
  the default output plays) with `pa_simple`, using a capture fragment of
  one Opus frame. If capture falls more than 60 ms behind, it flushes.
  After 100 ms of digital silence it stops sending until sound comes back.
* **App stream.** PCM written by a second-screen client is encoded as a
  separate stream. A client may mute the desktop stream while it is
  connected.
* **Encoding.** Opus 48 kHz stereo, `OPUS_APPLICATION_RESTRICTED_LOWDELAY`
  (CELT only, 2.5 ms look-ahead), 128 kbit/s, 5 ms frames by default.
* **Deck side.** `audio_receiver.cpp` decodes each stream with FFmpeg's Opus
  decoder into its own `SDL_AudioStream`, all bound to one device, so SDL
  mixes them. The device buffer is 240 frames (5 ms).
* **Jitter buffer and drift.** Each stream holds about `--audio-buffer` ms
  (default 30) in its queue.
  * A smoothed queue level steers `SDL_SetAudioStreamFrequencyRatio` within
    ±0.5%. That absorbs the clock difference between the PC and the Deck
    (typically under 0.01%) without audible pitch change.
  * When the queue runs dry it prepends silence to rebuild the cushion.
  * When the queue overflows (more than target + 60 ms, e.g. after a Wi-Fi
    stall) it is cleared, so audio never lags behind the picture.
  * Expect roughly 40–50 ms from PC to Deck speaker.

## Second-screen API

The full protocol is documented at the top of
[`sdk/steamu_screen.h`](../sdk/steamu_screen.h). Inside the host
(`screen_server.cpp`):

* **Socket.** A Unix socket at `$XDG_RUNTIME_DIR/steamu-screen.sock`
  (override with `STEAMU_SCREEN_SOCKET`). The host checks the peer's
  credentials (same user only) and serves one client at a time. It never
  replaces a socket that another running host still answers on.
* **Frames.** The client creates a file in `/dev/shm` with 3 frame slots.
  The host maps it read-only and checks owner, size and path. FRAME/RELEASE
  messages hand slots back and forth, so pixels are never copied over the
  socket. A frame the encoder hasn't picked up yet is superseded by the
  next one and its slot released at once.
* **Video source switch.** While a client is attached, the video streamer
  scales and encodes its frames instead of the monitor and forces a
  keyframe at every switch. Bottom-up images (`glReadPixels`) are read with
  a negative stride, without a copy.
* **Controls.** Every input report is converted to `sus_controller_msg` in
  SDL axis conventions and sent without blocking. A slow client loses
  reports instead of slowing input down.
* **Audio.** The host keeps at most 200 ms of the client's PCM; anything
  older is dropped if the client runs ahead of real time.

## OBS output

With `--obs`, the encoded video packets are also muxed into MPEG-TS
(`obs_output.cpp`) and sent to `udp://127.0.0.1:27860`. It reuses the same
encoded stream, so there is no second encode, and it follows mirror/app
switches. Headers are resent with every keyframe, so OBS can join at any
time. It carries video only.

## Source layout

```
src/common/   protocol (wire structs), networking helpers, logging
src/host/     steamu-host: USB/IP + uhid devices, X11 capture, video,
              audio, OBS output, second-screen server
src/deck/     steamu-deck: hidraw + fake controller backends, video and
              audio receivers, SDL front end
sdk/          steamu_screen.h: second-screen API, single-header C client
tools/        screentest (API example), inputtest (SDL controller viewer)
packaging/    udev rule and module autoload for the PC
third_party/  optional local copies of the SDL/FFmpeg tarballs (git-ignored;
              CMake downloads them when missing)
```

## Testing without a Deck

```bash
./build/steamu-host -v &
./build/steamu-deck --fake --windowed --host 127.0.0.1
```

* `--fake` sends a synthetic controller: the sticks circle, the buttons
  (back buttons too) cycle, and the gyro rotates.
* `--fake-idle` is a controller nobody touches.
* `--fake-stdin` takes scripted presses on standard input (`tap A`,
  `hold R2`, `release R2`, one per line).
* Without VAAPI it falls back to software decoding.

To run a test instance next to a real one, give it its own ports and API
socket:

```bash
STEAMU_SCREEN_SOCKET=$XDG_RUNTIME_DIR/steamu-test.sock ./build/steamu-host --port 28850 &
SDL_AUDIO_DRIVER=dummy ./build/steamu-deck --fake-idle --windowed --host 127.0.0.1 --port 28850
```

Use `SDL_AUDIO_DRIVER=dummy` (or `disk`) for a Deck instance on the PC
itself. Otherwise it plays the PC's desktop audio back into the PC, and
that loops. With `SDL_AUDIO_DRIVER=disk` and
`SDL_AUDIO_DISK_OUTPUT_FILE=out.raw` you can inspect the received audio
(F32LE stereo, 48 kHz). The disk driver's timing is not exact, so expect
the occasional re-buffer there.

## Design notes

* **Why USB/IP and not uhid.** A uhid device has no USB parent and so no
  USB interface number. Steam only accepts a Steam Deck controller whose
  gamepad is on interface 2, so it ignores uhid devices. uhid remains
  available as `steamu-host --uhid`, which works for plain SDL games.
* **Why XShm and not NvFBC.** NvFBC needs a patched driver on GeForce cards.
  XShm plus multithreaded swscale costs a few milliseconds at 3440×1440,
  well inside a frame.
* **Why UDP everywhere it matters.** Over Wi-Fi a single retransmission
  stalls a TCP stream for tens of milliseconds. Input and audio are
  latest-wins, and video recovers with a keyframe.
* **Why the API uses shared memory.** A 1280×800 RGBA frame is 4 MB, and at
  90 fps copying through a socket would cost more than the encode.
