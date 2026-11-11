# SteamU

Use a Steam Deck like a Wii U GamePad for your Linux PC.

* **Controller:** the Deck's controls appear on the PC as a real Steam Deck
  controller. Steam recognizes it, and the back buttons, gyro, trackpads and
  haptics all work.
* **Screen:** the Deck shows your monitor, cropped to its 16:10 screen,
  with about 7 ms of added latency. Games and mods that support it can draw
  their own Deck screen instead and use the touchscreen.
* **Sound:** the Deck's speakers or headphones play what the PC plays.
  Games can also send sound to the Deck only.
* **Recording:** send the Deck's picture to OBS.

Everything runs over your home Wi-Fi with low-latency streaming: NVENC
video, Opus audio and raw controller reports over UDP.

> Status: all four planned phases are done (controller, mirror, second
> screen, audio). Linux PC with X11 and an NVIDIA GPU only, for now.

## Requirements

* **PC:**
  * Linux on X11, with an NVIDIA GPU (NVENC) and PulseAudio or PipeWire.
  * Developed on Linux Mint 22 (Ubuntu 24.04 base).
* **Deck:** SteamOS 3.6 or newer.
* **Network:** the PC wired to the router and the Deck on 5 or 6 GHz Wi-Fi
  is recommended.

## Quick start

### 1. Build (on the PC)

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libx11-dev libxext-dev libxrandr-dev libpulse-dev libopus-dev \
    libva-dev libdrm-dev libwayland-dev libxkbcommon-dev libegl-dev libgl-dev \
    libpipewire-0.3-dev libudev-dev libdbus-1-dev \
    libxcursor-dev libxi-dev libxfixes-dev libxss-dev
cmake -S . -B build -G Ninja
ninja -C build
```

The first build downloads and compiles a static SDL 3.4 and a minimal
FFmpeg 7.1 for the Deck app, which takes a minute or two. To build offline,
put `SDL3-3.4.0.tar.gz` and `ffmpeg-7.1.1.tar.xz` in `third_party/` first.

### 2. One-time PC setup

This loads the kernel's USB/IP virtual host controller at boot and lets
members of `plugdev` plug a virtual USB device into it:

```bash
sudo cp packaging/60-steamu.rules /etc/udev/rules.d/
sudo cp packaging/steamu-modules.conf /etc/modules-load.d/steamu.conf
sudo udevadm control --reload
sudo modprobe -r vhci-hcd; sudo modprobe vhci-hcd
```

`ls -l /sys/devices/platform/vhci_hcd.0/attach` should now show group
`plugdev`.

### 3. Put the app on the Deck

Enable SSH on the Deck (in Desktop Mode: `passwd`, then
`sudo systemctl enable --now sshd`) and copy the single-file app over:

```bash
scp build/steamu-deck deck@steamdeck:~/steamu-deck
```

In Desktop Mode, add `~/steamu-deck` to Steam with **Add a Non-Steam Game**
and give it the plain **Gamepad** controller template, so the Deck's own
Steam doesn't act on the controls.

### 4. Play

1. On the PC, run `./build/steamu-host`.
2. On the Deck, launch SteamU from Game Mode. It finds the PC by itself
   (or set the launch option `--host 192.168.x.y`) and shows your monitor.
3. Steam on the PC lists a **Steam Deck** controller under Settings →
   Controller. Games use it like a cabled Deck.

The connecting screen has a **Quit** button (touch or trackpad click) for
when you start it from Desktop Mode.

## Options

| `steamu-host` | |
|---|---|
| `--monitor NAME` | monitor to mirror, e.g. `DP-2` (default: primary) |
| `--codec hevc\|h264` | video codec (default HEVC) |
| `--bitrate MBPS` | video bitrate (default 20) |
| `--fps N` | capture rate (default: the Deck's refresh rate) |
| `--obs`, `--obs-url URL` | also send the Deck's picture to OBS (see below) |
| `--no-video`, `--no-audio` | controller only / no sound |
| `--no-desktop-audio` | only apps' own Deck sound |
| `--audio-source NAME` | another PulseAudio source (`pactl list short sources`) |
| `--audio-bitrate K`, `--audio-frame 5\|10\|20` | Opus bitrate and frame size |
| `--port N` | use ports N..N+2 instead of 27850..27852 |
| `-v` | verbose logging |

| `steamu-deck` | |
|---|---|
| `--host IP`, `--port N` | connect to this PC instead of searching the LAN |
| `--vsync` | no tearing, slightly more latency |
| `--audio-buffer MS` | audio jitter buffer (default 30; lower = less delay, more dropouts) |
| `--no-video`, `--no-audio` | |
| `--windowed` | 1280×800 window instead of fullscreen |
| `--fake`, `--fake-idle`, `--fake-stdin` | synthetic controller, for testing without a Deck |
| `-v` | verbose logging |

## Recording the Deck screen in OBS

Start the host with `--obs`. It then also sends exactly what the Deck shows
to `udp://127.0.0.1:27860` as MPEG-TS. It reuses the existing stream, so
this costs no extra encoding and doesn't affect the Deck.

In OBS:
1. **Sources → + → Media Source**.
2. Untick **Local File**.
3. **Input:** `udp://127.0.0.1:27860`
4. **Input Format:** `mpegts`
5. Tick **Restart playback when source becomes active**.

The picture exists only while a Deck is connected. Sound isn't included;
OBS records the desktop audio itself. `--obs-url` sends the stream
elsewhere, e.g. `udp://192.168.1.20:27860?pkt_size=1316` for OBS on another
PC.

## Games with their own Deck screen

Games and mods can use the Deck as a real second screen:
* draw their own picture on it,
* get touch input,
* read the Deck's controls directly,
* play sound on the Deck only.

When they disconnect, the Deck goes back to mirroring. It's a single C
header with no dependencies: see the
**[API quick start](docs/API-QUICKSTART.md)**.

`build/steamu-screentest` is a ready-made example: it draws a pattern, a dot
under each finger and clicks on every touch.

The first real user is
**[SteamU Companion](https://github.com/Kestin4Real/SteamU-MC-Companion)**, a
Minecraft (Fabric) mod. It puts your inventory, hotbar, armor, crafting and
chests on the Deck's touchscreen while you play on the PC with the Deck's
controls.

## Tips and troubleshooting

* **Latency spikes:** turn off Wi-Fi power saving on the Deck
  (`sudo iw dev wlan0 set power_save off`). Stay on 5/6 GHz, close to the
  router. Both programs log stats every 5 seconds: input rate, lost
  packets, round-trip time, video fps and latency, audio buffer.
* **Sound on both the PC and the Deck:** the PC keeps playing too. Mute
  your PC speakers if you only want the Deck.
* **No controller on the PC when Steam isn't running:** the kernel driver
  starts it in mouse/keyboard ("lizard") mode. Hold ☰ for about a second
  to switch to gamepad mode. `./build/steamu-inputtest` shows what games
  see.
* **Browser gamepad testers show nothing:** while Steam runs, it takes over
  the controller and only hands it to games launched through Steam, just as
  with a cabled Deck.
* **The Deck also reacts to its own controls:** run SteamU from Game Mode
  with the Gamepad template (see step 3). The Steam and `...` buttons
  still open the Deck's own menus.
* **Gyro or lizard mode misbehave:** Steam on both machines can send
  controller settings. Try running `steamu-deck` in Desktop Mode with Steam
  closed.

## Documentation

* [Technical overview](docs/TECHNICAL.md): how the controller, video,
  audio and API work, the network protocol, source layout and testing.
* [API quick start](docs/API-QUICKSTART.md): add a Deck screen, touch,
  controls and Deck-only audio to your game.
* [`sdk/steamu_screen.h`](sdk/steamu_screen.h): API reference and protocol.

## License

[MIT](LICENSE)
