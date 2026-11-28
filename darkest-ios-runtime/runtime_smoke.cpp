#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <OpenGLES/ES3/gl.h>
#include <pthread.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/process.h"
#include "x86/lifted.h"

using namespace recomp;

void darkest_register_startup_crt();

namespace {

constexpr uint64_t kImage = 0x14000000;
constexpr uint64_t kEntry = kImage + 0x1000;

void Check(bool condition, const char* description) {
    if (condition) return;
    std::fprintf(stderr, "SMOKE FAILED: %s (%s)\n", description, SDL_GetError());
    std::fflush(stderr);
    std::exit(1);
}

uint64_t Import(Ctx& c, const char* dll, const char* name, std::initializer_list<uint64_t> args) {
    Check(hle::HasHandler(dll, name), name);
    return CallGuest(c, hle::ThunkFor(dll, name), args);
}

void Entry(Ctx& c) {
    Check(pthread_main_np() == 1, "guest entry runs on UIKit's main thread");
    const auto sdl = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Import(c, "sdl2.dll", name, args);
    };
    const auto gl = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Import(c, "opengl32.dll", name, args);
    };
    const auto string = [](const char* text) {
        const auto address = HeapAllocProcess(std::strlen(text) + 1, false);
        PutStr(address, text);
        return address;
    };
    const auto word = [] { return HeapAllocProcess(8, true); };

    const auto crt = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Import(c, "ucrtbase.dll", name, args);
    };
    const auto kernel = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Import(c, "kernel32.dll", name, args);
    };
    const auto dest = HeapAllocProcess(64, true);
    const auto source = string("Darkest");
    Check(crt("strncpy_s", {dest, 4, source, UINT64_MAX}) == 80 && GStr(dest) == "Dar",
          "MSVC narrow truncation");
    Check(crt("strcpy_s", {dest, 4, source}) == ERANGE && Rd8(dest) == 0,
          "MSVC short destination");
    Check(crt("strncpy_s", {dest, 4, source, 3}) == 0 && GStr(dest) == "Dar",
          "MSVC counted copy");
    Check(crt("strlen", {source}) == 7 && crt("strnlen", {source, 3}) == 3 &&
          crt("strnlen", {source, 20}) == 7, "MSVC narrow string length");
    const auto wide = HeapAllocProcess(16, true);
    Wr16(wide, 0x6697);
    Wr16(wide + 2, 0x9ED1);
    Check(crt("wcscpy_s", {dest, 32, wide}) == 0 && Rd16(dest) == 0x6697,
          "MSVC UTF-16 copy");
    Check(crt("wcslen", {dest}) == 2 && crt("wcsnlen", {dest, 1}) == 1,
          "MSVC wide length");
    const auto epoch = word();
    Wr64(epoch, 1795706365);
    Check(crt("_localtime64_s", {dest, epoch}) == 0 && Rd32(dest + 20) == 126,
          "MSVC local time");
    Check(crt("_ftime64_s", {dest}) == 0 && Rd16(dest + 8) < 1000, "MSVC wall time");
    Check(crt("rand_s", {word()}) == 0, "Win64 system entropy");
    Check(crt("rand_s", {0}) == EINVAL, "Win64 rand_s rejects a null buffer");
    Check(crt("_getpid", {}) == kGuestPid && kernel("GetCurrentProcessId", {}) == kGuestPid,
          "Win64 CRT and kernel process IDs agree");
    Check(crt("_chdir", {string("c:/smoke/.")}) == 0 &&
          crt("_getcwd", {dest, 64}) == dest && GStr(dest) == "C:\\smoke",
          "Win64 guest current directory");
    Check(crt("_chdir", {string("first.ddtest")}) == UINT32_MAX &&
          crt("_getcwd", {dest, 64}) == dest && GStr(dest) == "C:\\smoke",
          "a failure to change directories preserves the guest directory");

    const auto mutex_name = string("Darkest runtime smoke mutex");
    const auto mutex = kernel("CreateMutexA", {0, 1, mutex_name});
    Check(mutex && kernel("GetLastError", {}) == 0, "create the named Win64 mutex");
    const auto duplicate = kernel("CreateMutexA", {0, 1, mutex_name});
    Check(duplicate && duplicate != mutex && kernel("GetLastError", {}) == 183,
          "open the existing mutex through a separate handle");
    Check(kernel("ReleaseMutex", {duplicate}) == 1 && kernel("ReleaseMutex", {mutex}) == 0,
          "reopening the mutex leaves its ownership count unchanged");
    Check(kernel("WaitForSingleObject", {mutex, 0}) == 0 && kernel("ReleaseMutex", {mutex}) == 1 &&
          kernel("CloseHandle", {duplicate}) == 1 && kernel("CloseHandle", {mutex}) == 1,
          "the Win64 mutex can be acquired, released and closed");

    const auto folder = HeapAllocProcess(520, true);
    Check(Import(c, "shell32.dll", "SHGetFolderPathW", {0, 5, 0, 0, folder}) == 0 &&
          GWStr(folder).ends_with(u"My Documents"), "Win64 documents path");
    Check(Import(c, "shell32.dll", "SHPathPrepareForWriteW", {0, 0, folder, 1}) == 0,
          "prepare the guest save directory");
    Check(Import(c, "shell32.dll", "SHPathPrepareForWriteW", {0, 0, word(), 1}) == 0,
          "prepare the empty game-data path");
    const auto search = HeapAllocProcess(128, true);
    PutWStr(search, "C:\\Smoke\\*.ddtest");
    const auto found = HeapAllocProcess(592, true);
    const auto handle = kernel("FindFirstFileW", {search, found});
    Check(handle != UINT64_MAX, "start Win64 file enumeration");
    const auto first = GWStr(found + 44);
    Check(kernel("FindNextFileW", {handle, found}) == 1, "next Win64 file match");
    const auto second = GWStr(found + 44);
    Check((first == u"first.ddtest" && second == u"second.ddtest") ||
          (first == u"second.ddtest" && second == u"first.ddtest"), "UTF-16 find data matches fixture");
    Check(kernel("FindNextFileW", {handle, found}) == 0 && kernel("GetLastError", {}) == 18,
          "Win64 file enumeration ends");
    Check(kernel("FindClose", {handle}) == 1, "close Win64 file search");
    Check(sdl("SDL_Init", {0x20}) == 0, "SDL video");
    const auto display = HeapAllocProcess(16, true);
    Check(int32_t(sdl("SDL_GetNumVideoDisplays", {})) >= 1 &&
          sdl("SDL_GetDisplayBounds", {0, display}) == 0 &&
          Rd32(display + 8) && Rd32(display + 12), "SDL2 reports the device display");
    const auto window = sdl("SDL_CreateWindow", {string("Darkest runtime smoke"), 0, 0, 640, 360, 0x2006});
    Check(window != 0, "game window");
    const auto gl_context = sdl("SDL_GL_CreateContext", {window});
    Check(gl_context != 0, "OpenGL ES 3 context");
    const auto wm_info = HeapAllocProcess(72, true);
    Wr8(wm_info, 2);
    Check(sdl("SDL_GetWindowWMInfo", {window, wm_info}) == 1 && Rd64(wm_info + 8) &&
          Rd32(wm_info + 4) == 1, "SDL2 supplies its Win64 window bridge");
    const auto version = gl("glGetString", {GL_VERSION});
    const auto format = string("%d.%d"), major = word(), minor = word();
    Check(version && sdl("SDL_sscanf", {version, format, major, minor}) == 2 &&
          Rd32(major) >= 3, "parse the device GLES version with the game's desktop format");
    Wr32(minor, 81);
    Check(sdl("SDL_sscanf", {string("3."), format, major, minor}) == 1 && Rd32(minor) == 81,
          "SDL2 scanning preserves an unmatched output");
    Check(sdl("SDL_EventState", {0x201, 1}) == 0 && sdl("SDL_EventState", {0x201, UINT32_MAX}) == 1,
          "enable the guest window-manager event type");
    sdl("SDL_SetWindowPosition", {window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED});
    sdl("SDL_ShowWindow", {window});
    Check(gl("wglGetCurrentDC", {}) != 0, "window DC");
    const auto createShader = gl("wglGetProcAddress", {string("glCreateShader")});
    Check(createShader != 0, "resolve shader function");
    Check(gl("wglGetProcAddress", {string("glUnsupportedForSmoke")}) == 0, "unsupported lookup");

    const auto vertex = CallGuest(c, createShader, {GL_VERTEX_SHADER});
    const auto fragment = CallGuest(c, createShader, {GL_FRAGMENT_SHADER});
    const auto compile = [&](uint64_t shader, const char* source) {
        const auto data = word();
        Wr64(data, string(source));
        gl("glShaderSource", {shader, 1, data, 0});
        gl("glCompileShader", {shader});
        const auto ok = word();
        gl("glGetShaderiv", {shader, GL_COMPILE_STATUS, ok});
        if (!Rd32(ok)) {
            const auto message = HeapAllocProcess(4096, true);
            gl("glGetShaderInfoLog", {shader, 4096, 0, message});
            std::fprintf(stderr, "Shader compiler: %s\n", GStr(message).c_str());
        }
        Check(Rd32(ok) != 0, "desktop GLSL compiles through GLES bridge");
    };
    compile(vertex, "#version 120\nattribute vec2 points;\nvoid main(){gl_Position=vec4(points,0.0,1.0);}\n");
    compile(fragment, "#version 120\nuniform float values[2]=float[](0.5,1.0);\n"
                      "void main(){gl_FragColor=vec4(values[0],0.0,0.0,values[1]);}\n");
    const auto program = gl("glCreateProgram", {});
    gl("glAttachShader", {program, vertex});
    gl("glAttachShader", {program, fragment});
    gl("glBindAttribLocation", {program, 0, string("points")});
    gl("glLinkProgram", {program});
    const auto linked = word();
    gl("glGetProgramiv", {program, GL_LINK_STATUS, linked});
    Check(Rd32(linked) != 0, "link GLES shaders");
    gl("glUseProgram", {program});

    const GLfloat triangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    const auto vertices = HeapAllocProcess(sizeof(triangle), false);
    std::memcpy(GPtr(vertices), triangle, sizeof(triangle));
    const auto buffer = word();
    gl("glGenBuffers", {1, buffer});
    gl("glBindBuffer", {GL_ARRAY_BUFFER, Rd32(buffer)});
    gl("glBufferData", {GL_ARRAY_BUFFER, sizeof(triangle), vertices, GL_STATIC_DRAW});
    gl("glVertexAttribPointer", {0, 2, GL_FLOAT, GL_FALSE, 0, 0});
    gl("glEnableVertexAttribArray", {0});

    const auto width = word();
    const auto height = word();
    sdl("SDL_GL_GetDrawableSize", {window, width, height});
    Check(Rd32(width) && Rd32(height), "drawable size");
    gl("glBindFramebuffer", {GL_FRAMEBUFFER, 0});
    gl("glViewport", {0, 0, Rd32(width), Rd32(height)});
    gl("glDrawArrays", {GL_TRIANGLES, 0, 3});

    const auto pixel = word();
    gl("glReadPixels", {Rd32(width) / 2, Rd32(height) / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel});
    Check(gl("glGetError", {}) == GL_NO_ERROR, "draw without GL errors");
    const uint32_t rgba = Rd32(pixel);
    Check((rgba & 0x00FFFF00u) == 0 && (rgba & 0xFFu) >= 126u && (rgba & 0xFFu) <= 129u
          && (rgba >> 24) == 255u, "read pixels from the program including its default uniform");
    sdl("SDL_GL_SwapWindow", {window});
    sdl("SDL_InitSubSystem", {0x3200});
    Check(sdl("SDL_GameControllerEventState", {0}) == 0 &&
          sdl("SDL_GameControllerEventState", {UINT32_MAX}) == 0 &&
          sdl("SDL_GameControllerEventState", {1}) == 1 &&
          sdl("SDL_GameControllerEventState", {UINT32_MAX}) == 1,
          "Win64 game-controller events can be turned off and restored");
    std::fprintf(stderr, "SMOKE PASSED: Win64 startup, SDL2 window, GLES version, shader draw and first frame.\n");
    std::fflush(stderr);
    c.rax = 0;
}

} // namespace

int main(int, char**) {
    const auto path = std::string(std::getenv("HOME")) + "/Documents";
    if (!std::freopen((path + "/runtime-smoke.log").c_str(), "w", stderr)) return 1;
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    SDL_SetHintWithPriority(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight", SDL_HINT_OVERRIDE);
    for (const char* name : {"first.ddtest", "second.ddtest"}) {
        FILE* file = std::fopen((path + "/" + name).c_str(), "wb");
        Check(file && std::fclose(file) == 0, "create the file enumeration fixture");
    }
    darkest_register_startup_crt();
    const auto fixture = std::string(SDL_GetBasePath()) + "smoke.exe";
    const LiftedFn routines[] = {{kEntry, Entry}};
    RegisterLiftedFunctions(routines, 1);
    win32::GameConfig config;
    config.id = "runtime-smoke";
    config.guest_dir = "C:\\Smoke";
    config.exe_name = "smoke.exe";
    config.image_base = kImage;
    return RunProcess(config, fixture, path, "");
}
