#pragma once

namespace steamu {

extern bool g_verbose;

void logf(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace steamu

#define LOGI(...) ::steamu::logf('I', __VA_ARGS__)
#define LOGW(...) ::steamu::logf('W', __VA_ARGS__)
#define LOGE(...) ::steamu::logf('E', __VA_ARGS__)
#define LOGD(...)                                                                                  \
    do {                                                                                           \
        if (::steamu::g_verbose) ::steamu::logf('D', __VA_ARGS__);                                 \
    } while (0)
