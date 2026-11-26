#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <OpenGLES/ES3/gl.h>
#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "win32/hle.h"
#include "win32/process.h"
#include "x86/lifted.h"

using namespace recomp;

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
        const auto address = hle::HeapAllocProcess(std::strlen(text) + 1, false);
        PutStr(address, text);
        return address;
    };
    const auto word = [] { return hle::HeapAllocProcess(8, true); };

    Check(sdl("SDL_Init", {0x20}) == 0, "SDL video");
    const auto window = sdl("SDL_CreateWindow", {string("Darkest runtime smoke"), 0, 0, 640, 360, 0x2006});
    Check(window != 0, "game window");
    const auto gl_context = sdl("SDL_GL_CreateContext", {window});
    Check(gl_context != 0, "OpenGL ES 3 context");
    Check(gl("wglGetCurrentDC", {}) != 0, "window DC");
    const auto createShader = gl("wglGetProcAddress", {string("glCreateShader")});
    Check(createShader != 0, "resolve shader function");
    Check(gl("wglGetProcAddress", {string("glUnsupportedForSmoke")}) == 0, "unsupported lookup");
    Check(gl("glGetString", {GL_VERSION}) != 0, "read GLES version");

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
            const auto message = hle::HeapAllocProcess(4096, true);
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
    const auto vertices = hle::HeapAllocProcess(sizeof(triangle), false);
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
    std::fprintf(stderr, "SMOKE PASSED: native main thread, guest dispatch, GLSL conversion and first frame.\n");
    std::fflush(stderr);
    c.rax = 0;
}

} // namespace

int main(int, char**) {
    const auto path = std::string(std::getenv("HOME")) + "/Documents";
    if (!std::freopen((path + "/runtime-smoke.log").c_str(), "w", stderr)) return 1;
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    SDL_SetHintWithPriority(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight", SDL_HINT_OVERRIDE);
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
