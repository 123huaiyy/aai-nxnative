#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <string>

#include "win32/process.h"

namespace fs = std::filesystem;

int main(int, char**) {
    const char* home = std::getenv("HOME");
    if (!home) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Darkest Dungeon",
                                 "The app's Documents folder is unavailable.", nullptr);
        return 1;
    }
    fs::path documents = fs::path(home) / "Documents";
    fs::path game = documents / "DarkestDungeon";
    const auto exe = game / "_windowsnosteam/win64/Darkest.exe";
    if (!fs::is_regular_file(exe) || !fs::is_regular_file(game / "pin")) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Darkest Dungeon",
            "Copy your DarkestDungeon PC game folder into this app in Files, then reopen the app.",
            nullptr);
        return 2;
    }

    fs::path bundle = SDL_GetBasePath();
    fs::path module = bundle / "Frameworks/DarkestCode.framework/DarkestCode";
    void* library = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto install = library ? reinterpret_cast<void (*)()>(dlsym(library, "darkest_install_code"))
                           : nullptr;
    if (!install) {
        const char* error = dlerror();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Darkest Dungeon",
                                 error ? error : "Game code could not load.", nullptr);
        return 3;
    }

    fs::path native_data = documents / "NativeData";
    fs::create_directories(native_data);
    setenv("XDG_DATA_HOME", native_data.c_str(), 1);
    fs::path log = documents / "darkest-startup.log";
    if (std::freopen(log.c_str(), "w", stderr)) std::setvbuf(stderr, nullptr, _IOLBF, 0);
    install();

    recomp::win32::GameConfig config;
    config.id = "darkest";
    config.guest_dir = "C:\\Games\\DarkestDungeon";
    config.exe_name = "_windowsnosteam\\win64\\Darkest.exe";
    return recomp::RunProcess(config, exe.string(), game.string(), "");
}
