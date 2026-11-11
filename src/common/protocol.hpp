// Wire protocol between the Steam Deck (client) and the PC (host).
//
// Two channels:
//  * TCP control channel (deck -> host:kControlPort). Carries the handshake
//    (HID descriptor + identity) and HID feature/output report traffic, which
//    must be reliable and ordered (Steam configures the controller with it).
//  * UDP input channel (deck -> host:kInputPort). Carries raw HID input
//    reports. Latest-wins: late/out-of-order packets are dropped instead of
//    blocking newer ones (no TCP head-of-line stalls over Wi-Fi).
//
// Both ends are little-endian x86_64, so structs are sent as-is (packed).
#pragma once

#include <cstddef>
#include <cstdint>

namespace steamu::proto {

inline constexpr uint32_t kMagic = 0x55455453; // "STEU"
inline constexpr uint16_t kVersion = 5;

inline constexpr uint16_t kControlPort = 27850;   // TCP, host listens
inline constexpr uint16_t kInputPort = 27851;     // UDP, host listens
inline constexpr uint16_t kDiscoveryPort = 27852; // UDP broadcast, host listens

inline constexpr size_t kMaxDescriptor = 4096; // HID_MAX_DESCRIPTOR_SIZE
inline constexpr size_t kMaxReport = 4096;     // UHID_DATA_MAX
inline constexpr size_t kMaxMessage = 64 * 1024;
inline constexpr size_t kMaxUsbDescriptors = 1024;
inline constexpr size_t kMaxInterfaces = 4;
inline constexpr size_t kMaxInterfaceDescriptor = 1024;

enum class Msg : uint16_t {
    Hello = 1,   // deck -> host: HelloMsg
    Welcome = 2, // host -> deck: WelcomeMsg

    GetReport = 10,      // host -> deck: GetReportMsg
    GetReportReply = 11, // deck -> host: ReportReplyMsg + data
    SetReport = 12,      // host -> deck: SetReportMsg + data
    SetReportReply = 13, // deck -> host: ReportReplyMsg
    OutputReport = 14,   // host -> deck: OutputReportMsg + data

    Ping = 20, // host -> deck: PingMsg
    Pong = 21, // deck -> host: PingMsg (echoed)

    VideoStart = 30,      // deck -> host: VideoStartMsg (deck is ready to receive)
    RequestKeyframe = 31, // deck -> host: no payload (a frame was lost)
    AudioStart = 32,      // deck -> host: AudioStartMsg (deck can play audio)

    Touch = 40, // deck -> host: TouchMsg (touchscreen, for second-screen apps)
};

enum class VideoCodec : uint8_t { H264 = 1, HEVC = 2 };

// Video travels host -> deck over UDP, one encoded frame split into chunks.
inline constexpr size_t kVideoChunkPayload = 1400; // stays below a 1500-byte MTU
inline constexpr uint8_t kVideoFlagKeyframe = 0x01;

// Audio travels host -> deck over UDP: one Opus packet per datagram,
// 48 kHz stereo. Each stream is decoded and played separately on the Deck
// (SDL mixes them).
inline constexpr int kAudioRate = 48000;
inline constexpr int kAudioChannels = 2;
inline constexpr size_t kMaxAudioPayload = 1275; // largest Opus packet
enum class AudioStream : uint8_t {
    Desktop = 0, // what the PC plays (default output's monitor)
    App = 1,     // a second-screen app's own Deck audio (steamu_screen.h)
};
inline constexpr size_t kAudioStreams = 2;

// Same values as enum uhid_report_type.
enum class ReportType : uint8_t { Feature = 0, Output = 1, Input = 2 };

#pragma pack(push, 1)

struct MsgHeader {
    uint16_t type; // Msg
    uint16_t flags;
    uint32_t size; // payload bytes following this header
};

struct HelloMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t bus;
    uint16_t vendor;
    uint16_t product;
    uint32_t device_version;
    uint32_t country;
    char name[128];
    char uniq[64];
    char hostname[64];
    uint16_t descriptor_size; // HID report descriptor of the gamepad interface
    uint8_t descriptor[kMaxDescriptor];

    // Raw USB identity of the real controller, so the PC can recreate it as
    // an identical USB device (Steam requires the gamepad on interface 2).
    // usb_valid = 0: the host synthesizes Deck-like descriptors instead.
    uint8_t usb_valid;
    uint8_t usb_speed; // USB_SPEED_* (2 = full, 3 = high)
    uint8_t gamepad_interface;
    uint8_t interface_count;
    uint16_t usb_descriptors_size; // device descriptor + config descriptor
    uint8_t usb_descriptors[kMaxUsbDescriptors];
    char usb_manufacturer[64];
    char usb_product[64];
    char usb_serial[64];
    uint16_t interface_descriptor_size[kMaxInterfaces]; // HID report descriptor per interface
    uint8_t interface_descriptor[kMaxInterfaces][kMaxInterfaceDescriptor];
};

struct WelcomeMsg {
    uint32_t magic;
    uint16_t version;
    uint16_t input_port;
    uint32_t session_token;
};

struct GetReportMsg {
    uint32_t id;
    uint8_t rtype; // ReportType
    uint8_t rnum;
};

struct SetReportMsg {
    uint32_t id;
    uint8_t rtype; // ReportType
    uint8_t rnum;
    // followed by report data (byte 0 = report number, like hidraw)
};

struct ReportReplyMsg {
    uint32_t id;
    int32_t err; // 0 = ok, otherwise positive errno
    // GetReportReply: followed by report data (byte 0 = report number)
};

struct OutputReportMsg {
    uint8_t rtype;
    // followed by report data
};

struct PingMsg {
    uint64_t host_time_us;
    uint64_t last_rtt_us; // lets the deck estimate the host/deck clock offset
};

struct VideoStartMsg {
    uint16_t udp_port; // where the deck receives video
    uint16_t width;    // deck display size = stream size
    uint16_t height;
    uint16_t refresh_hz;
};

struct AudioStartMsg {
    uint16_t udp_port; // where the deck receives audio
    uint16_t reserved;
};

struct TouchMsg {
    uint8_t type;   // 1 down, 2 move, 3 up (= sus_touch_type)
    uint8_t finger; // 0..9
    uint16_t reserved;
    float x; // 0..1 across the video picture
    float y;
};

struct VideoPacketHeader {
    uint32_t magic;
    uint32_t frame;      // frame number, increments by 1
    uint16_t index;      // chunk index within the frame
    uint16_t count;      // chunks in this frame
    uint32_t frame_size; // total encoded bytes
    uint64_t capture_us; // host CLOCK_MONOTONIC when the screen was captured
    uint8_t flags;       // kVideoFlag*
    uint8_t codec;       // VideoCodec
    uint16_t reserved;
};

struct AudioPacketHeader {
    uint32_t magic;
    uint32_t seq;        // per stream, increments by 1
    uint64_t capture_us; // host CLOCK_MONOTONIC when captured
    uint8_t stream;      // AudioStream
    uint8_t reserved;
    uint16_t samples;    // per channel in this packet
    // followed by one Opus packet
};

struct InputPacketHeader {
    uint32_t magic;
    uint32_t token; // session token from WelcomeMsg
    uint32_t seq;   // increments per report
    uint16_t size;  // report bytes following
    uint16_t reserved;
    uint64_t deck_time_us; // CLOCK_MONOTONIC on the deck when the report was read
};

struct DiscoveryRequest {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
};

struct DiscoveryReply {
    uint32_t magic;
    uint16_t version;
    uint16_t control_port;
    char hostname[64];
};

#pragma pack(pop)

} // namespace steamu::proto
