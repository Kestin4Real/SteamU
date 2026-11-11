// steamu-inputtest: shows what an SDL3 game sees. Lists gamepads, whether
// they have gyro/accelerometer/touchpads, and prints button presses
// (including the Deck's back paddles) and gyro readings.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

int main(int argc, char** argv)
{
    int seconds = 0; // 0 = run until Ctrl+C
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: steamu-inputtest [--seconds N]\n");
            return 2;
        }
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    std::printf("SDL %d.%d.%d - waiting for gamepads...\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                SDL_MICRO_VERSION);

    std::map<SDL_JoystickID, SDL_Gamepad*> pads;
    std::map<SDL_JoystickID, Uint64> last_gyro_print;
    std::map<SDL_JoystickID, unsigned> gyro_samples;
    std::set<std::string> buttons_seen;
    bool deck_found = false, deck_gyro = false;

    const Uint64 end = seconds ? SDL_GetTicks() + Uint64(seconds) * 1000 : 0;
    bool running = true;
    while (running && (!end || SDL_GetTicks() < end)) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        switch (e.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_GAMEPAD_ADDED: {
            SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which);
            if (!pad) break;
            pads[e.gdevice.which] = pad;
            const char* name = SDL_GetGamepadName(pad);
            Uint16 vid = SDL_GetGamepadVendor(pad), pid = SDL_GetGamepadProduct(pad);
            bool gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
            bool accel = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
            if (gyro) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            if (accel) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            const char* path = SDL_GetGamepadPath(pad);
            std::printf("+ gamepad %u: '%s' %04X:%04X type=%s gyro=%s accel=%s touchpads=%d\n"
                        "  path=%s\n",
                        unsigned(e.gdevice.which), name ? name : "?", vid, pid,
                        SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)), gyro ? "yes" : "no",
                        accel ? "yes" : "no", SDL_GetNumGamepadTouchpads(pad),
                        path ? path : "?");
            if (vid == 0x28DE && pid == 0x1205) {
                deck_found = true;
                deck_gyro = deck_gyro || gyro;
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED:
            std::printf("- gamepad %u removed\n", unsigned(e.gdevice.which));
            if (pads.count(e.gdevice.which)) SDL_CloseGamepad(pads[e.gdevice.which]);
            pads.erase(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const char* b = SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button));
            std::printf("  [%u] button %s\n", unsigned(e.gbutton.which), b ? b : "?");
            if (b) buttons_seen.insert(b);
            break;
        }
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            if (e.gsensor.sensor == SDL_SENSOR_GYRO) {
                ++gyro_samples[e.gsensor.which];
                Uint64 now = SDL_GetTicks();
                if (now - last_gyro_print[e.gsensor.which] >= 500) {
                    last_gyro_print[e.gsensor.which] = now;
                    std::printf("  [%u] gyro %+.2f %+.2f %+.2f rad/s\n",
                                unsigned(e.gsensor.which), e.gsensor.data[0], e.gsensor.data[1],
                                e.gsensor.data[2]);
                }
            }
            break;
        default: break;
        }
    }

    std::printf("\nsummary: steam deck controller %s, gyro %s\n", deck_found ? "FOUND" : "not found",
                deck_gyro ? "yes" : "no");
    for (auto& [id, n] : gyro_samples) std::printf("  gamepad %u: %u gyro samples\n", id, n);
    std::printf("  buttons seen:");
    for (auto& b : buttons_seen) std::printf(" %s", b.c_str());
    std::printf("\n");

    for (auto& [id, pad] : pads) SDL_CloseGamepad(pad);
    SDL_Quit();
    return deck_found ? 0 : 1;
}
