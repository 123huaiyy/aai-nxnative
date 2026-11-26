#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "win32/process.h"

extern "C" void darkest_install_code();
extern "C" uint64_t darkest_runtime_guest_arena_size();

namespace {
bool IsRegularFile(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

bool EnsureDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}
}

int main(int, char**) {
    const char* home = std::getenv("HOME");
    if (!home) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Darkest Dungeon",
                                 "The app's Documents folder is unavailable.", nullptr);
        return 1;
    }
    const std::string documents = std::string(home) + "/Documents";
    const std::string log = documents + "/darkest-startup.log";
    if (std::freopen(log.c_str(), "w", stderr)) {
        std::setvbuf(stderr, nullptr, _IOLBF, 0);
        std::fputs("Darkest Dungeon: statically linked launcher started.\n", stderr);
    }
    const std::string game = documents + "/DarkestDungeon";
    const std::string exe = game + "/_windowsnosteam/win64/Darkest.exe";
    if (!IsRegularFile(exe) || !IsRegularFile(game + "/pin")) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Darkest Dungeon",
            "Copy your DarkestDungeon PC game folder into this app in Files, then reopen the app.",
            nullptr);
        return 2;
    }

    const std::string native_data = documents + "/NativeData";
    if (!EnsureDirectory(native_data)) {
        std::fprintf(stderr, "Could not create the app data folder: %s\n",
                     std::strerror(errno));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Darkest Dungeon",
                                 "Could not create the app data folder.", nullptr);
        return 3;
    }
    setenv("XDG_DATA_HOME", native_data.c_str(), 1);
    constexpr uint64_t arenaSize = uint64_t{1} << 31;
    if (darkest_runtime_guest_arena_size() != arenaSize) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Darkest Dungeon",
                                 "The installed runtime does not match this update.", nullptr);
        return 4;
    }
    darkest_install_code();
    std::fputs("Darkest Dungeon: game functions registered.\n", stderr);

    recomp::win32::GameConfig config;
    config.id = "darkest";
    config.guest_dir = "C:\\Games\\DarkestDungeon";
    config.exe_name = "_windowsnosteam\\win64\\Darkest.exe";
    config.image_base = 0x14000000;
    return recomp::RunProcess(config, exe, game, "");
}
