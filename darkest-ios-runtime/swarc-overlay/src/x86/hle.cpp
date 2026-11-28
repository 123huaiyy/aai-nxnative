#include "x86/hle.h"

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <deque>
#include <vector>

#include "runtime/log.h"
#include "x86/watchdog.h"

#if RECOMP_IOS_COMPACT_WIN64
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#endif

namespace recomp::hle {

thread_local int t_hle_depth = 0;

namespace {

#if RECOMP_IOS_COMPACT_WIN64
enum class FrameDomain : size_t { graphics, wait, other, count };
using FrameClock = std::chrono::steady_clock;
struct FrameImportTiming {
  FrameClock::duration elapsed{};
  uint64_t calls = 0;
};
thread_local std::array<FrameImportTiming, size_t(FrameDomain::count)> frame_imports{};
thread_local uint64_t frame_cpu_start = 0;

FrameDomain MeasureDomain(const std::string& dll, const std::string& name) {
  if (dll == "opengl32.dll") return FrameDomain::graphics;
  if (dll == "sdl2.dll" && name == "SDL_GL_SwapWindow") return FrameDomain::count;
  if ((dll == "sdl2.dll" && (name == "SDL_Delay" || name.starts_with("SDL_WaitEvent"))) ||
      (dll == "kernel32.dll" && (name.starts_with("Sleep") || name.starts_with("WaitFor"))))
    return FrameDomain::wait;
  return FrameDomain::other;
}
#endif

std::map<std::pair<std::string, std::string>, Def>& Registry() {
  static std::map<std::pair<std::string, std::string>, Def> r;
  return r;
}

// Libraries whose handlers run under one lock (Serialised).
std::set<std::string>& SerialisedLibs() {
  static std::set<std::string> s;
  return s;
}

struct Slot {
  std::string dll, name;
  const Def* def;  // nullptr: unimplemented
  bool serialised;  // its library is Serialised
#if RECOMP_IOS_COMPACT_WIN64
  FrameDomain frame_domain;
#endif
};

std::mutex g_slots_mu;  // serialises AddSlot
std::deque<Slot> g_slots;  // under g_slots_mu; stable refs

// CallThunk's view of g_slots, read without a lock (it runs on every
// import call): index = (thunk - kThunkBase) / kThunkStride. AddSlot
// stores each pointer with release once its Slot is complete.
constexpr size_t kMaxSlots = kDataExportOffset / kThunkStride;
std::atomic<const Slot*> g_slot_table[kMaxSlots];

// The thunk region of this process's mode.
gaddr ThunkBase() { return g_proc->x64 ? kThunkBase64 : kThunkBase; }

gaddr AddSlot(const std::string& dll, const std::string& name) {
  std::lock_guard<std::mutex> l(g_slots_mu);
  for (size_t i = 0; i < g_slots.size(); i++)
    if (g_slots[i].dll == dll && g_slots[i].name == name)
      return ThunkBase() + uint32_t(i) * kThunkStride;
  if (g_slots.size() >= kMaxSlots) FatalF("thunk region exhausted");
  gaddr thunk = ThunkBase() + uint32_t(g_slots.size()) * kThunkStride;
  auto it = Registry().find({dll, name});
  g_slots.push_back({dll, name, it == Registry().end() ? nullptr : &it->second,
                     SerialisedLibs().count(dll) != 0
#if RECOMP_IOS_COMPACT_WIN64
                     , MeasureDomain(dll, name)
#endif
  });
  g_slot_table[g_slots.size() - 1].store(&g_slots.back(),
                                         std::memory_order_release);
  return thunk;
}

struct DataDef {
  uint32_t size;
  void (*init)(gaddr);
};
std::map<std::pair<std::string, std::string>, DataDef>& DataRegistry() {
  static std::map<std::pair<std::string, std::string>, DataDef> r;
  return r;
}

// The guest address of a data export (allocated and initialised on first
// use), or 0 if dll!name is not one.
gaddr DataAddress(const std::string& dll, const std::string& name) {
  static std::mutex mu;
  static std::map<std::pair<std::string, std::string>, gaddr> placed;
  static gaddr next = 0;
  auto it = DataRegistry().find({dll, name});
  if (it == DataRegistry().end()) return 0;
  std::lock_guard<std::mutex> l(mu);
  gaddr& a = placed[{dll, name}];
  if (a) return a;
  if (!next) next = ThunkBase() + kDataExportOffset;
  a = next;
  next = (next + it->second.size + 15) & ~15ull;
  if (next > ThunkBase() + kThunkRegionSize - 0x1000) FatalF("data export region exhausted");
  if (it->second.init) it->second.init(a);
  return a;
}

}  // namespace

#if RECOMP_IOS_COMPACT_WIN64
void ReportHleFrameTiming(unsigned frames) {
  timespec sample{};
  const uint64_t cpu_now = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &sample) == 0
    ? uint64_t(sample.tv_sec) * 1000000000 + uint64_t(sample.tv_nsec) : 0;
  if (frames) {
    const auto per_frame_ms = [frames](FrameDomain domain) {
      const auto& value = frame_imports[size_t(domain)];
      return std::chrono::duration<double, std::milli>(value.elapsed).count() / frames;
    };
    const double cpu_ms = frame_cpu_start && cpu_now >= frame_cpu_start
      ? double(cpu_now - frame_cpu_start) / frames / 1e6 : -1.0;
    std::fprintf(stderr, "Darkest Dungeon: per-frame main thread CPU %.2f ms, "
                 "GLES %llu calls/frame (%.2f ms), wait imports %.2f ms, other imports %.2f ms "
                 "(import wall time; present reported separately).\n",
                 cpu_ms, static_cast<unsigned long long>(frame_imports[size_t(FrameDomain::graphics)].calls / frames),
                 per_frame_ms(FrameDomain::graphics), per_frame_ms(FrameDomain::wait),
                 per_frame_ms(FrameDomain::other));
  }
  frame_imports = {};
  frame_cpu_start = cpu_now;
}

void DumpHleWin64Exports(FILE* out) {
  for (const auto& [name, def] : Registry())
    if (def.x64) std::fprintf(out, "WIN64 function %s!%s\n", name.first.c_str(), name.second.c_str());
  for (const auto& [name, def] : DataRegistry())
    std::fprintf(out, "WIN64 data %s!%s\n", name.first.c_str(), name.second.c_str());
}
#endif

// Each import has one handler or data export: a second registration
// would win or lose by library load order.
Registrar::Registrar(const char* dll, const char* name, int nargs, Fn fn, bool x64) {
  if (DataRegistry().count({dll, name}) ||
      !Registry().emplace(std::pair<std::string, std::string>{dll, name}, Def{nargs, fn, x64})
           .second)
    FatalF("hle: %s!%s is registered twice", dll, name);
}

DataExport::DataExport(const char* dll, const char* name, uint32_t size, void (*init)(gaddr)) {
  if (Registry().count({dll, name}) ||
      !DataRegistry().emplace(std::pair<std::string, std::string>{dll, name}, DataDef{size, init})
           .second)
    FatalF("hle: %s!%s is registered twice", dll, name);
}

Serialised::Serialised(const char* lib) { SerialisedLibs().insert(lib); }

gaddr BindImport(const std::string& lib, const std::string& name) {
  if (gaddr data = DataAddress(lib, name)) return data;
  if (!Registry().count({lib, name}))
    RECOMP_TRACE("hle: import %s!%s has no handler\n", lib.c_str(), name.c_str());
  return AddSlot(lib, name);
}

bool IsDataExport(const std::string& lib, const std::string& name) {
  return DataRegistry().count({lib, name}) != 0;
}

bool HasHandler(const std::string& lib, const std::string& name) {
  return Registry().count({lib, name}) != 0;
}

void CallThunk(Ctx& c, gaddr thunk) {
  const bool x64 = g_proc->x64;
  const gaddr off = thunk - ThunkBase();
  size_t idx = off / kThunkStride;
  const Slot* s = nullptr;
  if (off % kThunkStride == 0 && idx < kMaxSlots)
    s = g_slot_table[idx].load(std::memory_order_acquire);
  const unsigned long long ret = x64 ? Rd64(c.rsp) : Rd32(c.esp);
  if (!s)
    FatalF("call to unknown thunk %08llX (return address %08llX)",
           (unsigned long long)thunk, ret);
  if (!s->def)
    FatalF("unimplemented import %s!%s called (return address %08llX)",
           s->dll.c_str(), s->name.c_str(), ret);
  if (x64 && !s->def->x64)
    FatalF("import %s!%s called from x64 code: its handler is not audited for "
           "x64 guests (HLE64) (return address %08llX)",
           s->dll.c_str(), s->name.c_str(), ret);
  const Def& d = *s->def;
  // <ID>_TRACE_FILTER=mss32 (substring of "dll!name") narrows the trace.
  static const char* filter = Env("TRACE_FILTER");
  const bool trace = g_trace && (!filter || (s->dll + "!" + s->name).find(filter) !=
                                                std::string::npos);
  if (trace) {
    std::string args;
    for (int i = 0; i < std::max(d.nargs, 0) && i < 8; i++) {
      char buf[24];
      std::snprintf(buf, sizeof buf, i ? ", %llX" : "%llX", (unsigned long long)Arg(c, i));
      args += buf;
    }
    TraceF("[%04X] %s!%s(%s) from %08llX\n", CurrentThread()->tid,
           s->dll.c_str(), s->name.c_str(), args.c_str(), ret);
  }
  // For hang dumps: which import this thread is inside (innermost). A
  // guest exception's unwind (GuestResume) may leave through the handler,
  // so this is undone on the way out either way.
  struct Inside {
    const char* saved_name = t_wd_hle_name;
    uint64_t saved_ret = t_wd_hle_ret;
    Inside(const char* name, uint64_t ret) {
      t_wd_hle_name = name;
      t_wd_hle_ret = ret;
      ++t_hle_depth;
    }
    ~Inside() {
      --t_hle_depth;
      t_wd_hle_name = saved_name;
      t_wd_hle_ret = saved_ret;
    }
  };
  uint64_t r;
  {
#if RECOMP_IOS_COMPACT_WIN64
    const bool measure = t_hle_depth == 0 && frame_cpu_start && s->frame_domain != FrameDomain::count;
    const auto enter = measure ? FrameClock::now() : FrameClock::time_point{};
#endif
    Inside inside(s->name.c_str(), ret);
    static std::recursive_mutex serial_mu;
    std::unique_lock<std::recursive_mutex> sl;
    if (s->serialised) sl = std::unique_lock<std::recursive_mutex>(serial_mu);
    r = d.fn(c);
#if RECOMP_IOS_COMPACT_WIN64
    if (measure) {
      auto& value = frame_imports[size_t(s->frame_domain)];
      value.elapsed += FrameClock::now() - enter;
      ++value.calls;
    }
#endif
  }
  if (trace) TraceF("[%04X]   -> %llX\n", CurrentThread()->tid, (unsigned long long)r);
  if (x64) {
    c.rax = r;
    c.rsp += 8;  // the caller pops its own arguments
  } else {
    c.eax = uint32_t(r);
    c.esp += 4 + (d.nargs > 0 ? 4 * uint32_t(d.nargs) : 0);
  }
}

gaddr ThunkFor(const std::string& dll, const std::string& name) {
  return AddSlot(dll, name);
}

}  // namespace recomp::hle
