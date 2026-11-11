#include "common/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace steamu {

bool g_verbose = false;

namespace {
std::mutex g_log_mutex;
}

void logf(char level, const char* fmt, ...)
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    localtime_r(&ts.tv_sec, &local);

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::lock_guard lock(g_log_mutex);
    std::fprintf(stderr, "%02d:%02d:%02d.%03ld %c %s\n", local.tm_hour, local.tm_min, local.tm_sec,
                 ts.tv_nsec / 1000000, level, msg);
}

} // namespace steamu
