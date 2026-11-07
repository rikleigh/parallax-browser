// system_utils_host.cpp - the six functions of ANGLE's system_utils_linux.cpp
// for the macOS x86_64 host check (that file needs Linux-only clocks and
// pthread_setname_np(2 args)).
#include "common/system_utils.h"

#include <mach-o/dyld.h>
#include <pthread.h>
#include <time.h>

#include <string>

namespace angle
{
std::string GetExecutablePath()
{
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0)
        return "";
    return path;
}

std::string GetExecutableDirectory()
{
    std::string executablePath = GetExecutablePath();
    size_t lastPathSepLoc = executablePath.find_last_of("/");
    return (lastPathSepLoc != std::string::npos) ? executablePath.substr(0, lastPathSepLoc) : "";
}

const char *GetSharedLibraryExtension()
{
    return "dylib";
}

double GetCurrentSystemTime()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

uint64_t GetCurrentSystemTimeNs()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + t.tv_nsec;
}

void SetCurrentThreadName(const char *name)
{
    pthread_setname_np(name);
}
}  // namespace angle

// renderergl_utils.cpp asks for the macOS version on ANGLE_PLATFORM_MACOS
// (driver_utils_mac.mm upstream); the harness is not a Mac GL driver check.
#include "libANGLE/renderer/driver_utils.h"
namespace rx
{
OSVersion GetMacOSVersion()
{
    return OSVersion(0, 0, 0);
}
}  // namespace rx
