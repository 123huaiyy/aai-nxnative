#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <OpenGLES/ES3/gl.h>
#include <objc/runtime.h>
#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <cmath>
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
constexpr uint64_t kJumpReturn = kEntry + 0x20;
constexpr uint64_t kPcmCallback = kEntry + 0x30;
constexpr uint64_t kWorker = kEntry + 0x38;
constexpr uint64_t kSavedRegister = 0xa42a;
constexpr char kFixtureName[] = "options-\xE6\xB5\x8B\xE8\xAF\x95.json";
constexpr char kFixtureData[] = "{\"value\":7}\n";
std::atomic<unsigned> pcm_callbacks{0};
using PresentRenderbuffer = BOOL (*)(id, SEL, GLenum);
PresentRenderbuffer native_present = nullptr;
GLint presented_renderbuffer = 0;
bool present_succeeded = false;
GLuint present_framebuffer = 0;
SDL_Point present_size{};
uint8_t presented_pixel[4]{};

BOOL ObservePresent(id context, SEL method, GLenum target) {
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &presented_renderbuffer);
    GLint previous_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, present_framebuffer);
    glReadPixels(present_size.x / 2, present_size.y / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                 presented_pixel);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    present_succeeded = native_present(context, method, target);
    return present_succeeded;
}

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

void CheckRtti(Ctx& c) {
    const auto memory = HeapAllocProcess(1024, true);
    const auto type_derived = memory + 32, type_left = memory + 64, type_right = memory + 96;
    const auto hierarchy = memory + 160, array = memory + 192, bases = memory + 224;
    const auto primary = memory + 320, secondary = memory + 352;
    const auto left_vtable = memory + 392, right_vtable = memory + 424;
    const auto instance = memory + 480, missing = memory + 512;
    PutStr(type_derived + 16, ".?AVDerived@@");
    PutStr(type_left + 16, ".?AVLeft@@");
    PutStr(type_right + 16, ".?AVRight@@");
    PutStr(missing + 16, ".?AVMissing@@");
    Wr32(hierarchy + 4, 1);  // Multiple inheritance.
    Wr32(hierarchy + 8, 3);
    const auto rva = [&](gaddr field, gaddr value) { Wr32(field, uint32_t(value - memory)); };
    rva(hierarchy + 12, array);
    for (const auto locator : {primary, secondary}) {
        Wr32(locator, 1);
        Wr32(locator + 4, locator == secondary ? 16 : 0);
        rva(locator + 12, type_derived);
        rva(locator + 16, hierarchy);
        rva(locator + 20, locator);
    }
    const gaddr types[]{type_derived, type_left, type_right};
    for (unsigned i = 0; i < 3; ++i) {
        const auto descriptor = bases + i * 28;
        rva(array + i * 4, descriptor);
        rva(descriptor, types[i]);
        Wr32(descriptor + 4, i == 0 ? 2 : 0);
        Wr32(descriptor + 8, i == 2 ? 16 : 0);
        Wr32(descriptor + 12, UINT32_MAX);
    }
    Wr64(left_vtable - 8, primary);
    Wr64(right_vtable - 8, secondary);
    Wr64(instance, left_vtable);
    Wr64(instance + 16, right_vtable);
    const auto cast = [&](gaddr object, gaddr from, gaddr to) {
        return Import(c, "vcruntime140.dll", "__RTDynamicCast", {object, 0, from, to, 0});
    };
    Check(cast(instance, type_left, type_derived) == instance, "Win64 RTTI finds the derived save UI object");
    Check(cast(instance + 16, type_right, type_left) == instance &&
          cast(instance, type_left, type_right) == instance + 16,
          "Win64 RTTI adjusts sibling base addresses");
    Check(cast(instance, type_left, missing) == 0 && cast(0, type_left, type_right) == 0,
          "Win64 RTTI rejects a missing target and preserves null");
    Check(Import(c, "vcruntime140.dll", "__RTtypeid", {instance + 16}) == type_derived,
          "Win64 typeid on a secondary vtable finds the complete object type");
    HeapFreeProcess(memory);
}

void JumpReturn(Ctx& c) {
    Check(c.rax == 1 && c.rbx == kSavedRegister && c.xmm[6].b[0] == 0x5a,
          "longjmp restores preserved Win64 registers and maps zero to one");
    c.rax = 7;
}

void PcmCallback(Ctx& c) {
    Check(c.rcx == 0x2aa2 && c.rdx && c.r8 == 512, "Win64 SDL2 audio callback parameters");
    std::memset(GPtr(c.rdx), 0, size_t(c.r8));
    pcm_callbacks.fetch_add(1, std::memory_order_release);
    c.rsp += 8;
}

void Worker(Ctx& c) {
    Wr32(c.rcx, 0x7194);
    c.rax = 0;
    c.rsp += 8;
}

void CheckDatagram(Ctx& c) {
    const auto call = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Import(c, "ws2_32.dll", name, args);
    };
    const auto startup = HeapAllocProcess(408, true);
    Check(call("#115", {0x202, startup}) == 0 && Rd16(startup) == 0x202, "Win64 Winsock initialization");
    const auto socket = call("#23", {2, 2, 17});
    Check(socket != UINT64_MAX, "open a Win64 datagram socket");
    const auto address = HeapAllocProcess(16, true), length = HeapAllocProcess(8, true);
    Wr16(address, 2);
    Wr32(address + 4, 0x0100007f);
    Wr32(length, 16);
    Check(call("#2", {socket, address, 16}) == 0 &&
          call("#6", {socket, address, length}) == 0 && Rd16(address + 2),
          "bind and inspect the local datagram address");
    const auto option = HeapAllocProcess(8, true);
    Wr32(option, 16384);
    Check(call("#21", {socket, 0xffff, 0x1002, option, 4}) == 0, "set the Win64 socket receive buffer");
    const auto data = HeapAllocProcess(8, true);
    Wr32(data, 0x7842b99);
    Check(call("#20", {socket, data, 4, 0, address, 16}) == 4, "send a local datagram");
    const auto read_set = HeapAllocProcess(520, true), timeout = HeapAllocProcess(8, true);
    Wr32(read_set, 1);
    Wr64(read_set + 8, socket);
    Wr32(timeout, 1);
    Check(call("#18", {0, read_set, 0, 0, timeout}) == 1 && Rd32(read_set) == 1 &&
          Rd64(read_set + 8) == socket, "select reports the datagram ready");
    const auto buffers = HeapAllocProcess(16, true);
    const auto received = HeapAllocProcess(8, true), flags = HeapAllocProcess(8, true);
    const auto payload = HeapAllocProcess(8, true);
    Wr32(buffers, 4);
    Wr64(buffers + 8, payload);
    Check(call("WSARecvFrom", {socket, buffers, 1, received, flags, address, length, 0, 0}) == 0 &&
          Rd32(received) == 4 && Rd32(payload) == 0x7842b99, "receive the datagram through a Win64 WSABUF");
    Check(call("#3", {socket}) == 0 && call("#116", {}) == 0, "close the isolated datagram socket");
}

void CheckJump(Ctx& c) {
    const auto buffer = HeapAllocProcess(256, true);
    const auto stack = c.rsp, saved_bx = c.rbx;
    const auto saved_xmm = c.xmm[6];
    c.rbx = kSavedRegister;
    c.xmm[6].b[0] = 0x5a;
    c.rcx = buffer;
    c.rdx = 0;  // C-style jump without a Windows language-handler frame.
    c.rsp = stack - 8;
    Wr64(c.rsp, kJumpReturn);
    hle::CallThunk(c, hle::ThunkFor("vcruntime140.dll", "__intrinsic_setjmp"));
    Check(c.rax == 0 && c.rsp == stack && Rd64(buffer + 0x50) == kJumpReturn,
          "setjmp returns zero and records the caller's PC and stack");

    c.rbx = 0;
    c.xmm[6].b[0] = 0;
    c.rcx = buffer;
    c.rdx = 0;
    c.rsp = stack - 0x48;
    Wr64(c.rsp, kEntry + 8);
    try {
        hle::CallThunk(c, hle::ThunkFor("vcruntime140.dll", "longjmp"));
        Check(false, "longjmp reaches the saved continuation");
    } catch (const GuestResume& resume) {
        Check(resume.target == kJumpReturn && resume.ebp == stack && c.rsp == stack,
              "longjmp resumes the owning guest frame");
        Resume(c, resume);
    }
    Check(c.rax == 7, "the guest dispatcher ran the registered setjmp continuation");
    c.rbx = saved_bx;
    c.xmm[6] = saved_xmm;
}

void Entry(Ctx& c) {
    Check(pthread_main_np() == 1, "guest entry runs on UIKit's main thread");
    CheckJump(c);
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
    CheckDatagram(c);

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
    PutStr(dest, "a");
    Check(crt("strncat_s", {dest, 6, source, UINT64_MAX}) == 80 &&
          GStr(dest) == "aDark" && crt("isalnum", {'9'}) && crt("isalnum", {'_'}) == 0,
          "MSVC string append truncation and ASCII classification");
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
    const auto wide_mutex_name = HeapAllocProcess(96, true);
    PutWStr(wide_mutex_name, "Darkest video");
    const auto wide_mutex = kernel("CreateMutexW", {0, 0, wide_mutex_name});
    Check(wide_mutex && kernel("CloseHandle", {wide_mutex}) == 1, "create the video worker's Win64 mutex");
    const auto worker_result = word();
    const auto worker = kernel("CreateThread", {0, 0, kWorker, worker_result, 0, 0});
    Check(worker && kernel("WaitForSingleObject", {worker, 3000}) == 0 &&
          Rd32(worker_result) == 0x7194 && kernel("CloseHandle", {worker}) == 1,
          "start and join a Win64 video worker");
    const auto module = word();
    Check(kernel("GetModuleHandleExW", {6, kEntry, module}) == 1 && Rd64(module) == kImage,
          "resolve the guest module by a native guest address");

    const auto folder = HeapAllocProcess(520, true);
    Check(Import(c, "shell32.dll", "SHGetFolderPathW", {0, 5, 0, 0, folder}) == 0 &&
          GWStr(folder).ends_with(u"My Documents"), "Win64 documents path");
    Check(Import(c, "shell32.dll", "SHPathPrepareForWriteW", {0, 0, folder, 1}) == 0,
          "prepare the guest save directory");
    Check(Import(c, "shell32.dll", "SHPathPrepareForWriteW", {0, 0, word(), 1}) == 0,
          "prepare the empty game-data path");

    const auto file_path = HeapAllocProcess(2048, true);
    const auto path_prefix = HeapAllocProcess(32, true);
    PutWStr(path_prefix, "C:\\Smoke\\");
    Check(crt("wcscpy_s", {file_path, 1024, path_prefix}) == 0, "copy the game-data path prefix");
    constexpr size_t prefix_length = sizeof("C:\\Smoke\\") - 1;
    const auto file_name = file_path + prefix_length * 2;
    Check(kernel("MultiByteToWideChar",
                 {65001, 0, string(kFixtureName), UINT32_MAX, file_name, 1024 - prefix_length}) > 0 &&
          GWStr(file_path) == u"C:\\Smoke\\options-\u6D4B\u8BD5.json", "append the UTF-8 options filename");
    const auto read_mode = word();
    PutWStr(read_mode, "rb");
    const auto stream_out = word();
    Check(crt("_wfopen_s", {stream_out, file_path, read_mode}) == 0 &&
          Rd64(stream_out) != 0, "open the UTF-16 options path through the Windows file bridge");
    const auto stream = Rd64(stream_out);
    const auto contents = HeapAllocProcess(64, true);
    Check(crt("fseek", {stream, 0, 2}) == 0 &&
          crt("ftell", {stream}) == sizeof(kFixtureData) - 1 &&
          crt("fseek", {stream, 0, 0}) == 0 &&
          crt("fread", {contents, 1, sizeof(kFixtureData) - 1, stream}) == sizeof(kFixtureData) - 1 &&
          GStr(contents) == kFixtureData && crt("fclose", {stream}) == 0,
          "read the options file through the guest's seek, size, read and close calls");
    PutWStr(file_name, "missing.json");
    Wr64(stream_out, 1);
    Check(crt("_wfopen_s", {stream_out, file_path, read_mode}) == ENOENT && !Rd64(stream_out),
          "missing options file clears the guest stream pointer");
    Wr64(stream_out, 1);
    Check(crt("_wfopen_s", {stream_out, 0, read_mode}) == EINVAL && !Rd64(stream_out),
          "invalid options open returns the Windows CRT argument error");

    const auto extension = HeapAllocProcess(16, true);
    PutWStr(extension, ".json");
    const auto vc = [&](const char* name, std::initializer_list<uint64_t> args) {
        return Import(c, "vcruntime140.dll", name, args);
    };
    Check(vc("wcsrchr", {file_path, '\\'}) == file_name - 2 &&
          vc("wcsstr", {file_path, extension}) == file_name + 14 &&
          vc("wcsrchr", {file_path, 0}) == file_name + 24,
          "wide directory separator, suffix and terminator offsets");

    constexpr char file_template[] = "foo.ddsave";
    const auto save_name = HeapAllocProcess(64, true);
    PutWStr(save_name, file_template);
    const auto unique_name = HeapAllocProcess(520, true);
    Check(Import(c, "shell32.dll", "#47", {unique_name, 260, save_name, 0, path_prefix}) == 1 &&
          GWStr(unique_name) == u"C:\\Smoke\\foo (2).ddsave", "choose an unused overlay save filename");
    const auto write_mode = word();
    PutWStr(write_mode, "wb");
    Check(crt("_wfopen_s", {stream_out, unique_name, write_mode}) == 0 && Rd64(stream_out) != 0,
          "open the selected save path for writing");
    const auto save_stream = Rd64(stream_out);
    Check(crt("fwrite", {contents, 1, sizeof(kFixtureData) - 1, save_stream}) == sizeof(kFixtureData) - 1 &&
          crt("fclose", {save_stream}) == 0, "write and close the overlay save file");
    Check(Import(c, "shell32.dll", "#47", {unique_name, 260, save_name, 0, path_prefix}) == 1 &&
          GWStr(unique_name) == u"C:\\Smoke\\foo (3).ddsave", "skip saved files in the overlay");
    Check(kernel("SleepEx", {1, 1}) == 0, "guest save retry sleep");

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
    CheckRtti(c);
    Check(sdl("SDL_Init", {0x20}) == 0, "SDL video");
    const auto display = HeapAllocProcess(16, true);
    Check(int32_t(sdl("SDL_GetNumVideoDisplays", {})) >= 1 &&
          sdl("SDL_GetDisplayBounds", {0, display}) == 0 &&
          Rd32(display + 8) && Rd32(display + 12), "SDL2 reports the device display");
    const auto window = sdl("SDL_CreateWindow", {string("Darkest runtime smoke"), 0, 0, 640, 360, 0x301b});
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
    const auto scan_arguments = HeapAllocProcess(16, true);
    Wr64(scan_arguments, major);
    Wr64(scan_arguments + 8, minor);
    Check(crt("__stdio_common_vsscanf", {0, version, UINT64_MAX, format, 0, scan_arguments}) == 2 &&
          Rd32(major) >= 3, "parse the GLES version through the Windows varargs dispatcher");
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
    for (const char* name : {
             "glActiveTexture", "glAttachShader", "glBindAttribLocation", "glBindBuffer",
             "glBindFramebuffer", "glBindRenderbuffer", "glBindVertexArray", "glBlendEquation",
             "glBlendEquationSeparate", "glBlendFuncSeparate", "glBufferData", "glBufferSubData",
             "glCheckFramebufferStatus", "glCompileShader", "glCompressedTexImage2D", "glCreateProgram",
             "glCreateShader", "glDeleteBuffers", "glDeleteFramebuffers", "glDeleteProgram",
             "glDeleteRenderbuffers", "glDeleteShader", "glDeleteVertexArrays",
             "glEnableVertexAttribArray", "glFlushMappedBufferRange", "glFramebufferRenderbuffer",
             "glFramebufferTexture2D", "glGenBuffers", "glGenFramebuffers", "glGenRenderbuffers",
             "glGenVertexArrays", "glGenerateMipmap", "glGetActiveUniform", "glGetProgramiv",
             "glGetShaderInfoLog", "glGetShaderiv", "glGetStringi", "glGetUniformLocation",
             "glLinkProgram", "glMapBuffer", "glMapBufferRange", "glRenderbufferStorage",
             "glShaderSource", "glTexImage3D", "glUniform1f", "glUniform1i", "glUniform2f", "glUniform3f",
             "glUniform4fv", "glUniformMatrix2fv", "glUniformMatrix2x3fv", "glUniformMatrix4fv",
             "glUnmapBuffer", "glUseProgram", "glVertexAttribPointer",
         }) {
        Check(gl("wglGetProcAddress", {string(name)}) != 0, name);
    }
    const auto extensions = word();
    gl("glGetIntegerv", {GL_NUM_EXTENSIONS, extensions});
    if (Rd32(extensions) != 0)
        Check(GStr(gl("glGetStringi", {GL_EXTENSIONS, 0})).starts_with("GL_"), "indexed GLES extension string");

    const auto mapped_id = word();
    const auto bytes = HeapAllocProcess(16, true);
    for (uint32_t i = 0; i < 4; ++i) Wr32(bytes + 4 * i, i + 10);
    gl("glGenBuffers", {1, mapped_id});
    gl("glBindBuffer", {GL_ARRAY_BUFFER, Rd32(mapped_id)});
    gl("glBufferData", {GL_ARRAY_BUFFER, 16, bytes, GL_DYNAMIC_DRAW});
    auto mapped = gl("glMapBuffer", {GL_ARRAY_BUFFER, 0x88B9});  // GL_WRITE_ONLY
    Check(mapped != 0, "map a full buffer into guest memory");
    Wr32(mapped + 4, 41);
    Check(gl("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE, "unmap a written buffer");
    mapped = gl("glMapBufferRange", {GL_ARRAY_BUFFER, 4, 8, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT});
    Check(mapped != 0, "map a GLES range for explicit flush");
    Wr32(mapped, 55);
    Wr32(mapped + 4, 99);
    gl("glFlushMappedBufferRange", {GL_ARRAY_BUFFER, 4, 4});
    Check(gl("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE, "unmap the explicitly flushed range");
    mapped = gl("glMapBufferRange", {GL_ARRAY_BUFFER, 0, 16, GL_MAP_READ_BIT});
    Check(mapped && Rd32(mapped) == 10 && Rd32(mapped + 4) == 41 &&
          Rd32(mapped + 8) == 99 && Rd32(mapped + 12) == 13,
          "guest buffer round trip preserves unmodified and unflushed bytes");
    Check(gl("glUnmapBuffer", {GL_ARRAY_BUFFER}) == GL_TRUE, "unmap the read range");
    gl("glDeleteBuffersARB", {1, mapped_id});

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
    // The game leaves its offscreen depth/stencil renderbuffer bound after
    // creating a framebuffer. Inspect the binding at the actual iOS present.
    const auto guest_renderbuffer = word();
    gl("glGenRenderbuffers", {1, guest_renderbuffer});
    gl("glBindRenderbuffer", {GL_RENDERBUFFER, Rd32(guest_renderbuffer)});
    gl("glRenderbufferStorage", {GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 64, 64});
    const auto props = SDL_GetWindowProperties(SDL_GL_GetCurrentWindow());
    const auto drawable = SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_UIKIT_OPENGL_RENDERBUFFER_NUMBER, 0);
    present_framebuffer = static_cast<GLuint>(SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0));
    SDL_GetWindowSizeInPixels(SDL_GL_GetCurrentWindow(), &present_size.x, &present_size.y);
    if (present_size.x * 10 >= present_size.y * 19) {
        Check(Rd32(width) * 3 == Rd32(height) * 7 && Rd32(width) <= 1960,
              "phone landscape rendering uses a capped 21:9 scene");
    }
    Check(drawable && drawable != Rd32(guest_renderbuffer), "separate drawable and guest renderbuffers");
    const auto method = class_getInstanceMethod(
        objc_getClass("EAGLContext"), sel_registerName("presentRenderbuffer:"));
    Check(method != nullptr, "find the native iOS presentation method");
    native_present = reinterpret_cast<PresentRenderbuffer>(method_getImplementation(method));
    method_setImplementation(method, reinterpret_cast<IMP>(ObservePresent));
    sdl("SDL_GL_SwapWindow", {window});
    method_setImplementation(method, reinterpret_cast<IMP>(native_present));
    Check(present_succeeded && presented_renderbuffer == drawable,
          "iOS presents the drawable color buffer with a guest depth buffer bound");
    Check(presented_pixel[0] >= 123 && presented_pixel[0] <= 132 &&
          presented_pixel[1] == 0 && presented_pixel[2] == 0,
          "iOS presents pixels drawn through the guest framebuffer");
    const auto renderbuffer = word();
    gl("glGetIntegerv", {GL_RENDERBUFFER_BINDING, renderbuffer});
    Check(Rd32(renderbuffer) == Rd32(guest_renderbuffer) &&
          gl("glGetError", {}) == GL_NO_ERROR, "presentation preserves the guest renderbuffer binding");
    gl("glDeleteRenderbuffers", {1, guest_renderbuffer});
    sdl("SDL_InitSubSystem", {0x3200});
    Check(sdl("SDL_GameControllerEventState", {0}) == 0 &&
          sdl("SDL_GameControllerEventState", {UINT32_MAX}) == 0 &&
          sdl("SDL_GameControllerEventState", {1}) == 1 &&
          sdl("SDL_GameControllerEventState", {UINT32_MAX}) == 1,
          "Win64 game-controller events can be turned off and restored");
    sdl("SDL_GameControllerUpdate", {});
    Check(int32_t(sdl("SDL_NumHaptics", {})) >= 0, "enumerate available haptic devices");

    const auto cursor_pixels = HeapAllocProcess(64, true);
    const auto cursor_surface = sdl("SDL_CreateRGBSurfaceFrom",
        {cursor_pixels, 4, 4, 32, 16, 0xffu, 0xff00u, 0xff0000u, 0xff000000u});
    Check(cursor_surface != 0, "create the guest cursor surface");
    Check(sdl("SDL_CreateColorCursor", {cursor_surface, 0, 0}) == 0 &&
          !GStr(sdl("SDL_GetError", {})).empty(), "Windows cursor creation reports the iOS fallback");
    sdl("SDL_FreeSurface", {cursor_surface});
    sdl("SDL_SetCursor", {0});
    Check(sdl("SDL_ShowCursor", {UINT32_MAX}) <= 1, "read the default pointer state");
    Check(sdl("SDL_InitSubSystem", {SDL_INIT_AUDIO}) == 0, "initialize SDL playback");
    const auto spec = HeapAllocProcess(32, true);
    Wr32(spec, 44100);
    Wr16(spec + 4, SDL_AUDIO_S16LE);
    Wr8(spec + 6, 2);
    Wr16(spec + 8, 128);
    Wr64(spec + 16, kPcmCallback);
    Wr64(spec + 24, 0x2aa2);
    Check(sdl("SDL_OpenAudio", {spec, 0}) == 0 && Rd32(spec + 12) == 512,
          "open the SDL2 cutscene PCM device");
    sdl("SDL_LockAudio", {});
    sdl("SDL_PauseAudio", {0});
    SDL_Delay(25);
    Check(!pcm_callbacks.load(std::memory_order_acquire), "SDL2 audio lock excludes its Win64 callback");
    sdl("SDL_UnlockAudio", {});
    for (unsigned wait = 0; wait < 100 && !pcm_callbacks.load(std::memory_order_acquire); ++wait)
        SDL_Delay(5);
    Check(pcm_callbacks.load(std::memory_order_acquire) != 0, "guest audio callback delivered PCM");
    sdl("SDL_CloseAudio", {});
    sdl("SDL_QuitSubSystem", {SDL_INIT_AUDIO});
    std::fprintf(stderr, "SMOKE PASSED: Win64 RTTI, files, OpenGL presentation, SDL audio and UDP.\n");
    std::fflush(stderr);
    c.rax = 0;
}

} // namespace

int main(int, char**) {
    const auto path = std::string(std::getenv("HOME")) + "/Documents";
    if (!std::freopen((path + "/runtime-smoke.log").c_str(), "w", stderr)) return 1;
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    SDL_SetHintWithPriority(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight", SDL_HINT_OVERRIDE);
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    for (const char* name : {"first.ddtest", "second.ddtest"}) {
        FILE* file = std::fopen((path + "/" + name).c_str(), "wb");
        Check(file && std::fclose(file) == 0, "create the file enumeration fixture");
    }
    {
        FILE* file = std::fopen((path + "/" + kFixtureName).c_str(), "wb");
        Check(file && std::fwrite(kFixtureData, 1, sizeof(kFixtureData) - 1, file) == sizeof(kFixtureData) - 1 &&
              std::fclose(file) == 0, "create the UTF-8 options fixture");
        file = std::fopen((path + "/foo (1).ddsave").c_str(), "wb");
        Check(file && std::fclose(file) == 0, "create the read-only install save-name fixture");
    }
    darkest_register_startup_crt();
    const auto fixture = std::string(SDL_GetBasePath()) + "smoke.exe";
    const LiftedFn routines[] = {{kEntry, Entry}, {kJumpReturn, JumpReturn},
                                 {kPcmCallback, PcmCallback}, {kWorker, Worker}};
    RegisterLiftedFunctions(routines, 4);
    win32::GameConfig config;
    config.id = "runtime-smoke";
    config.guest_dir = "C:\\Smoke";
    config.exe_name = "smoke.exe";
    config.image_base = kImage;
    return RunProcess(config, fixture, path, "");
}
