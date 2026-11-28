// SDL2.dll for x64 guests that link SDL2 themselves, on the host's SDL3.
// Guest-visible structures (SDL_Event, SDL_DisplayMode, SDL_Surface,
// SDL_SysWMinfo, SDL_RWops...) keep SDL2's x64 layouts; host objects
// (windows, game controllers, haptics) are handed to the guest as handles.
// SDL2 is middleware here like Miles or Bink: this is its API on SDL3, not
// a game's.

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "win32/win32.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "sdl2.dll";

// A guest copy of a host string, kept per thread until the next call
// (SDL_GetError, SDL_GetHint: SDL2 returns pointers it owns), and freed
// when the thread exits.
struct ThreadStrings {
  gaddr buf[4] = {};
  size_t cap[4] = {};
  ~ThreadStrings() {
    for (gaddr b : buf)
      if (b) HeapFreeProcess(b);
  }
};
thread_local ThreadStrings t_strings;

gaddr ThreadString(const char* s, int slot) {
  gaddr& buf = t_strings.buf[slot];
  size_t& cap = t_strings.cap[slot];
  size_t n = std::strlen(s) + 1;
  if (n > cap) {
    if (buf) HeapFreeProcess(buf);
    cap = std::max<size_t>(n, 256);
    buf = HeapAllocProcess(cap, false);
  }
  std::memcpy(GPtr(buf), s, n);
  return buf;
}

// SDL2's SDL_bool and int results.
uint64_t Bool(bool b) { return b ? 1 : 0; }
uint64_t Status(bool ok) { return ok ? 0 : uint32_t(-1); }

#if RECOMP_IOS_GLES
struct LegacyAudio {
  SDL_AudioStream* stream = nullptr;
  gaddr callback = 0;
  gaddr userdata = 0;
  gaddr samples = 0;
  int bytes = 0;
  uint8_t silence = 0;
  bool paused = true;
  std::atomic<bool> closing{false};
  std::recursive_mutex callback_mu;
  std::mutex completion_mu;
  std::condition_variable completion;
  bool finished = false;
};

std::mutex g_audio_mu;
std::shared_ptr<LegacyAudio> g_audio;
thread_local std::vector<std::shared_ptr<LegacyAudio>> t_locked_audio;

std::shared_ptr<LegacyAudio> LegacyDevice() {
  std::lock_guard<std::mutex> lock(g_audio_mu);
  return g_audio;
}

void ServiceAudio(LegacyAudio& audio, Thread& thread) {
  while (!audio.closing.load(std::memory_order_acquire)) {
    bool filled = false;
    {
      std::lock_guard<std::recursive_mutex> lock(audio.callback_mu);
      const int queued = SDL_GetAudioStreamQueued(audio.stream);
      if (!audio.paused && !audio.closing.load(std::memory_order_relaxed) &&
          queued >= 0 && queued < audio.bytes * 2) {
        std::memset(GPtr(audio.samples), audio.silence, audio.bytes);
        CallGuest(thread.ctx, audio.callback, {audio.userdata, audio.samples, uint32_t(audio.bytes)});
        filled = SDL_PutAudioStreamData(audio.stream, GPtr(audio.samples), audio.bytes);
      }
    }
    if (!filled) SDL_Delay(4);
  }
  {
    std::lock_guard<std::mutex> lock(audio.completion_mu);
    audio.finished = true;
  }
  audio.completion.notify_all();
}

void CloseLegacyAudio() {
  auto audio = LegacyDevice();
  if (!audio || audio->closing.exchange(true, std::memory_order_acq_rel)) return;
  {
    std::unique_lock<std::mutex> completed(audio->completion_mu);
    audio->completion.wait(completed, [&] { return audio->finished; });
  }
  {
    std::lock_guard<std::recursive_mutex> callback_lock(audio->callback_mu);
    SDL_DestroyAudioStream(audio->stream);
    audio->stream = nullptr;
    HeapFreeProcess(audio->samples);
  }
  std::lock_guard<std::mutex> device_lock(g_audio_mu);
  if (g_audio == audio) g_audio.reset();
}
#endif

}  // namespace

// ---- memory, strings ---------------------------------------------------------

HLE64(kDll, SDL_malloc, kCdecl) { return HeapAllocProcess(std::max<uint64_t>(Arg(c, 0), 1), false); }
HLE64(kDll, SDL_calloc, kCdecl) {
  uint64_t n = Arg(c, 0), size = Arg(c, 1);
  if (size && n > UINT64_MAX / size) return 0;
  return HeapAllocProcess(std::max<uint64_t>(n * size, 1), true);
}
HLE64(kDll, SDL_free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(p);
  return 0;
}
HLE64(kDll, SDL_wcslen, kCdecl) {  // wchar_t is 16-bit on Windows
  gaddr p = Arg(c, 0);
  uint64_t n = 0;
  while (Rd16(p + 2 * n)) n++;
  return n;
}

// ---- initialisation -------------------------------------------------------------
// SDL2's subsystem flags are SDL3's (SDL_INIT_TIMER, 0x1, is gone: timers
// need no init). Video goes through the runtime's own SDL setup.

HLE64(kDll, SDL_SetMainReady, kCdecl) { return 0; }

HLE64(kDll, SDL_Init, kCdecl) {
  uint32_t flags = uint32_t(Arg(c, 0)) & ~0x1u;
  if (flags & SDL_INIT_VIDEO) EnsureSdlVideo();
  return Status(SDL_InitSubSystem(flags));
}
HLE64(kDll, SDL_QuitSubSystem, kCdecl) {
  // Video stays up: the runtime's windows (and its presentation) need it.
#if RECOMP_IOS_GLES
  if (Arg(c, 0) & SDL_INIT_AUDIO) CloseLegacyAudio();
#endif
  SDL_QuitSubSystem(uint32_t(Arg(c, 0)) & ~(0x1u | SDL_INIT_VIDEO | SDL_INIT_EVENTS));
  return 0;
}
HLE64(kDll, SDL_Quit, kCdecl) { return 0; }

HLE64(kDll, SDL_GetError, kCdecl) { return ThreadString(SDL_GetError(), 0); }

HLE64(kDll, SDL_SetHint, kCdecl) {
  return Bool(SDL_SetHint(GStr(Arg(c, 0)).c_str(), GStr(Arg(c, 1)).c_str()));
}
HLE64(kDll, SDL_GetHint, kCdecl) {
  const char* v = SDL_GetHint(GStr(Arg(c, 0)).c_str());
  return v ? ThreadString(v, 1) : 0;
}

#if RECOMP_IOS_GLES
HLE64(kDll, SDL_OpenAudio, kCdecl) {
  const auto desired = Arg(c, 0);
  const auto output = Arg(c, 1);
  if (!desired) return Status(SDL_InvalidParamError("desired"));
  const auto spec = SDL_AudioSpec{SDL_AudioFormat(Rd16(desired + 4)), Rd8(desired + 6),
                                  int32_t(Rd32(desired))};
  const uint16_t samples = Rd16(desired + 8) ? Rd16(desired + 8) : 1024;
  const auto callback = Rd64(desired + 16), userdata = Rd64(desired + 24);
  const unsigned stride = SDL_AUDIO_BYTESIZE(spec.format) * unsigned(spec.channels);
  if (spec.freq <= 0 || !stride || !callback) return Status(SDL_InvalidParamError("desired"));

  std::lock_guard<std::mutex> lock(g_audio_mu);
  if (g_audio) return Status(SDL_SetError("Audio device is already open"));
  auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream) return UINT32_MAX;
  auto audio = std::make_shared<LegacyAudio>();
  audio->stream = stream;
  audio->callback = callback;
  audio->userdata = userdata;
  audio->bytes = int(stride * samples);
  audio->silence = uint8_t(SDL_GetSilenceValueForFormat(spec.format));
  audio->samples = HeapAllocProcess(audio->bytes, true);
  auto* thread = CreateGuestThread(0);
  g_audio = audio;
  StartServiceThread(thread, [audio, thread] { ServiceAudio(*audio, *thread); });
  ReleaseThread(thread);

  const auto result = output ? output : desired;
  if (result != desired) std::memcpy(GPtr(result), GPtr(desired), 32);
  Wr8(result + 7, audio->silence);
  Wr16(result + 8, samples);
  Wr32(result + 12, uint32_t(audio->bytes));
  return 0;
}

HLE64(kDll, SDL_LockAudio, kCdecl) {
  GuestSyncPoint sync;
  auto audio = LegacyDevice();
  if (audio) audio->callback_mu.lock();
  t_locked_audio.push_back(std::move(audio));
  return 0;
}

HLE64(kDll, SDL_UnlockAudio, kCdecl) {
  GuestSyncPoint sync;
  if (t_locked_audio.empty()) return 0;
  if (auto& audio = t_locked_audio.back()) audio->callback_mu.unlock();
  t_locked_audio.pop_back();
  return 0;
}

HLE64(kDll, SDL_PauseAudio, kCdecl) {
  if (auto audio = LegacyDevice()) {
    std::lock_guard<std::recursive_mutex> lock(audio->callback_mu);
    if (audio->stream) {
      audio->paused = Arg(c, 0) != 0;
      if (audio->paused) SDL_PauseAudioStreamDevice(audio->stream);
      else SDL_ResumeAudioStreamDevice(audio->stream);
    }
  }
  return 0;
}

HLE64(kDll, SDL_CloseAudio, kCdecl) {
  CloseLegacyAudio();
  return 0;
}

HLE64(kDll, SDL_sscanf, kCdecl) {
  const auto format = GStr(Arg(c, 1));
  // The shipped game parses the GL version into two Win64 ints.
  if (format != "%d.%d") FatalF("SDL_sscanf: unsupported guest format %s", format.c_str());
  const auto input = GStr(Arg(c, 0));
  int major = 0, minor = 0;
  const int matched = SDL_sscanf(input.c_str(), format.c_str(), &major, &minor);
  if (matched >= 1) Wr32(Arg(c, 2), uint32_t(major));
  if (matched >= 2) Wr32(Arg(c, 3), uint32_t(minor));
  return uint32_t(matched);
}

HLE64(kDll, SDL_SetClipboardText, kCdecl) {
  const auto text = GStr(Arg(c, 0));
  return Status(SDL_SetClipboardText(text.c_str()));
}

HLE64(kDll, SDL_GetClipboardText, kCdecl) {
  char* text = SDL_GetClipboardText();
  if (!text) return 0;
  const auto size = std::strlen(text) + 1;
  const auto result = HeapAllocProcess(size, false);
  std::memcpy(GPtr(result), text, size);
  SDL_free(text);
  return result;
}

HLE64(kDll, SDL_GetTicks, kCdecl) { return uint32_t(SDL_GetTicks()); }

HLE64(kDll, SDL_InitSubSystem, kCdecl) {
  return hle_SDL_Init(c);
}
#endif

// ---- time, CPU, paths -------------------------------------------------------------

HLE64(kDll, SDL_GetPerformanceCounter, kCdecl) { return SDL_GetPerformanceCounter(); }
HLE64(kDll, SDL_GetPerformanceFrequency, kCdecl) { return SDL_GetPerformanceFrequency(); }
HLE64(kDll, SDL_Delay, kCdecl) {
  GuestSyncPoint sync;
  SDL_Delay(uint32_t(Arg(c, 0)));
  return 0;
}
HLE64(kDll, SDL_GetCPUCount, kCdecl) { return GuestProcessorCount(); }

// The directory of the EXE as the guest sees it, with a trailing
// backslash; the caller frees it with SDL_free.
HLE64(kDll, SDL_GetBasePath, kCdecl) {
  std::string dir = GuestGameDir();
  if (dir.empty() || dir.back() != '\\') dir += '\\';
  gaddr p = HeapAllocProcess(dir.size() + 1, false);
  PutStr(p, dir);
  return p;
}

// SDL_iconv_string(to, from, text, bytes): the conversions between the
// encodings Windows games use (UTF-8, UTF-16LE/UCS-2, ASCII); the result
// is SDL_malloc'd and NUL-terminated (four zero bytes, as SDL2's).
HLE64(kDll, SDL_iconv_string, kCdecl) {
  std::string to = win32::Lower(GStr(Arg(c, 0))), from = win32::Lower(GStr(Arg(c, 1)));
  gaddr in = Arg(c, 2);
  uint64_t n = Arg(c, 3);
  const char* hto = to == "ucs-2" || to == "ucs-2-internal" || to == "wchar_t" ? "UTF-16LE"
                                                                               : to.c_str();
  const char* hfrom = from == "ucs-2" || from == "ucs-2-internal" || from == "wchar_t"
                          ? "UTF-16LE"
                          : from.c_str();
  char* r = SDL_iconv_string(hto, hfrom, reinterpret_cast<const char*>(GPtr(in)), size_t(n));
  if (!r) return 0;
  // The host result ends with up to four zero bytes; find the content's end
  // by the target's unit size.
  size_t unit = std::strstr(hto, "16") ? 2 : std::strstr(hto, "32") ? 4 : 1;
  size_t len = 0;
  for (;; len += unit) {
    bool zero = true;
    for (size_t k = 0; k < unit; k++) zero = zero && !r[len + k];
    if (zero) break;
  }
  gaddr out = HeapAllocProcess(len + 4, true);
  std::memcpy(GPtr(out), r, len);
  SDL_free(r);
  return out;
}

// ---- SDL_RWops --------------------------------------------------------------------
// A guest SDL_RWops (SDL2's x64 layout: size, seek, read, write, close,
// type) stands for a host SDL_IOStream. The guest only hands them to SDL
// functions, which use the host stream; its function pointers are thunks
// named "SDL_RWops::<op>" without handlers, so a direct call is fatal and
// says which.

namespace {

constexpr uint64_t kRwopsSize = 0x58;
std::mutex g_rw_mu;
std::map<gaddr, SDL_IOStream*> g_rw;

uint64_t NewRw(SDL_IOStream* io) {
  if (!io) return 0;
  static const char* const kOps[] = {"size", "seek", "read", "write", "close"};
  gaddr rw = HeapAllocProcess(kRwopsSize, true);
  for (int i = 0; i < 5; i++) Wr64(rw + 8 * i, ThunkFor(kDll, std::string("SDL_RWops::") + kOps[i]));
  std::lock_guard<std::mutex> l(g_rw_mu);
  g_rw[rw] = io;
  return rw;
}

// The host stream for a guest SDL_RWops passed to an SDL function; with
// freesrc the function closes it, so the guest block goes too.
SDL_IOStream* UseRw(gaddr rw, bool freesrc, const char* api) {
  std::lock_guard<std::mutex> l(g_rw_mu);
  auto it = g_rw.find(rw);
  if (it == g_rw.end()) FatalF("%s: unknown SDL_RWops %08llX", api, (unsigned long long)rw);
  SDL_IOStream* io = it->second;
  if (freesrc) {
    g_rw.erase(it);
    HeapFreeProcess(rw);
  }
  return io;
}

}  // namespace

// SDL_RWFromFile(file, mode): file is UTF-8 (a guest path).
HLE64(kDll, SDL_RWFromFile, kCdecl) {
  std::string file = GStr(Arg(c, 0)), mode = GStr(Arg(c, 1));
  bool bad;
  std::u16string w = win32::Utf8ToUtf16(reinterpret_cast<const uint8_t*>(file.data()), file.size(), &bad);
  if (bad) FatalF("SDL_RWFromFile: path is not UTF-8");
  if (mode.empty() || !std::strchr("rwa", mode[0])) FatalF("SDL_RWFromFile: mode \"%s\"", mode.c_str());
  bool plus = mode.find('+') != std::string::npos;
  PathAccess acc = mode[0] == 'r' ? (plus ? PathAccess::kUpdate : PathAccess::kRead)
                   : mode[0] == 'a' ? PathAccess::kUpdate
                                    : PathAccess::kWrite;
  std::string host = HostPath(NarrowPath(w), acc);
  RECOMP_TRACE("  SDL_RWFromFile(%s, %s) -> %s\n", file.c_str(), mode.c_str(), host.c_str());
  return NewRw(SDL_IOFromFile(host.c_str(), mode.c_str()));
}

// SDL_RWFromConstMem(mem, size): the host stream reads guest memory in
// place.
HLE64(kDll, SDL_RWFromConstMem, kCdecl) {
  return NewRw(SDL_IOFromConstMem(GPtr(Arg(c, 0)), size_t(uint32_t(Arg(c, 1)))));
}

// SDL_LoadBMP_RW(rw, freesrc). A null rw (a failed SDL_RWFromFile passed
// on) is SDL2's invalid-parameter error; images need guest SDL_Surfaces,
// which are not provided yet.
HLE64(kDll, SDL_LoadBMP_RW, kCdecl) {
  if (!Arg(c, 0)) {
    SDL_SetError("Parameter 'src' is invalid");
    return 0;
  }
  FatalF("SDL_LoadBMP_RW: guest SDL_Surface not implemented");
}

// SDL_GameControllerAddMappingsFromRW(rw, freerw): the mapping database
// format is unchanged in SDL3. The number added, or -1.
HLE64(kDll, SDL_GameControllerAddMappingsFromRW, kCdecl) {
  bool free = uint32_t(Arg(c, 1)) != 0;
  SDL_IOStream* io = UseRw(Arg(c, 0), free, "SDL_GameControllerAddMappingsFromRW");
  return uint64_t(int64_t(SDL_AddGamepadMappingsFromIO(io, free)));
}

}  // namespace recomp::hle
