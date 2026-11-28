// SDL2's windows, events, keyboard, mouse and game controllers on SDL3
// (see sdl2.cpp).
// SDL2 windows are HWNDs too (CreateSdl2Window), so the HWND a game gets
// from SDL_GetWindowWMInfo works with DXGI and user32. Window coordinates
// and sizes are pixels, as SDL2 reports them to a DPI-aware Windows
// program; SDL3 works in points, converted with the window's pixel
// density. A guest SDL_Window* is an opaque heap block.
//
// Events: once SDL2's video is up, the host event stream is SDL2's
// (SetHostEventSink): each SDL3 event becomes the SDL2 event(s) it stands
// for, in SDL2's x64 layout (a 56-byte union, millisecond timestamps), or
// is dropped when SDL2 has no such event (pens, cameras, SDL3-only window
// and device events) or disables it by default (drag and drop, which a
// game enables with SDL_EventState).

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/user32_window.h"
#include "win32/win32.h"
#include "x86/runtime.h"
#if RECOMP_IOS_GLES
#include "x86/watchdog.h"
#endif

#if RECOMP_IOS_GLES
extern void RequestGameLandscape(SDL_Window* window);
#endif

namespace recomp::hle {

#if RECOMP_IOS_GLES
void CaptureGlesDefaultFramebuffer(SDL_Window* window, SDL_Point requested);
bool PresentGlesWindow(SDL_Window* window, uint64_t& swap_ns);
void LogGlesVertexStream();
void GameDrawableSize(SDL_Window* window, int* width, int* height);
void GameDrawablePointer(SDL_Window* window, float& x, float& y, float& dx, float& dy);
void ForgetGameDrawable(SDL_Window* window);
#endif

namespace {

constexpr const char* kDll = "sdl2.dll";

uint64_t Bool(bool b) { return b ? 1 : 0; }

// ---- windows ----------------------------------------------------------------------

struct Sdl2Window {
  SDL_Window* sdl;
  uint32_t hwnd;
  uint32_t flags;  // SDL2 flags given at creation that SDL3 does not keep
};

std::mutex g_win_mu;
std::map<gaddr, Sdl2Window> g_windows;  // by guest SDL_Window*

#if RECOMP_IOS_GLES
struct GlContext {
  gaddr window;
  SDL_GLContext native;
};
std::map<gaddr, GlContext> g_gl_contexts;

bool SetGlesProfile() {
  return SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) &&
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) &&
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
}
#endif

Sdl2Window Must(gaddr w, const char* api) {
  std::lock_guard<std::mutex> l(g_win_mu);
  auto it = g_windows.find(w);
  if (it == g_windows.end()) FatalF("%s: unknown SDL_Window %08llX", api, (unsigned long long)w);
  return it->second;
}

int ToPoints(SDL_Window* w, int px) {
  float d = SDL_GetWindowPixelDensity(w);
  return d > 0 ? int(std::lround(px / d)) : px;
}
int ToPixels(SDL_Window* w, float pt) {
  float d = SDL_GetWindowPixelDensity(w);
  return int(std::floor(pt * (d > 0 ? d : 1)));
}

void GuestPointer(SDL_Window* window, float& x, float& y, float& dx, float& dy) {
  if (!window) return;
#if RECOMP_IOS_GLES
  if (SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL) {
    GameDrawablePointer(window, x, y, dx, dy);
    return;
  }
#endif
  const float density = SDL_GetWindowPixelDensity(window);
  x *= density;
  y *= density;
  dx *= density;
  dy *= density;
}

// SDL2 window flags.
enum : uint32_t {
  kFullscreen = 0x1, kOpenGl = 0x2, kShown = 0x4, kHidden = 0x8, kBorderless = 0x10,
  kResizable = 0x20, kMinimized = 0x40, kMaximized = 0x80, kMouseGrabbed = 0x100,
  kInputFocus = 0x200, kMouseFocus = 0x400, kFullscreenDesktop = 0x1001,
  kAllowHighDpi = 0x2000, kMouseCapture = 0x4000, kAlwaysOnTop = 0x8000,
  kKeyboardGrabbed = 0x100000,
};

// ---- events -----------------------------------------------------------------------

using Event = std::array<uint8_t, 56>;  // SDL2's SDL_Event

std::mutex g_ev_mu;
std::deque<Event> g_events;

// SDL2 event types.
enum : uint32_t {
  kQuit = 0x100, kDisplayEvent = 0x150, kWindowEvent = 0x200, kSysWm = 0x201, kKeyDown = 0x300,
  kKeyUp = 0x301, kTextEditing = 0x302, kTextInput = 0x303, kKeymapChanged = 0x304,
  kMouseMotion = 0x400, kMouseButtonDown = 0x401, kMouseButtonUp = 0x402, kMouseWheel = 0x403,
  kJoyAxis = 0x600, kJoyBall = 0x601, kJoyHat = 0x602, kJoyButtonDown = 0x603,
  kJoyButtonUp = 0x604, kJoyDeviceAdded = 0x605, kJoyDeviceRemoved = 0x606,
  kJoyBattery = 0x607, kPadAxis = 0x650, kPadButtonDown = 0x651, kPadButtonUp = 0x652,
  kPadAdded = 0x653, kPadRemoved = 0x654, kPadRemapped = 0x655, kPadTouchDown = 0x656,
  kPadTouchMotion = 0x657, kPadTouchUp = 0x658, kPadSensor = 0x659, kClipboard = 0x900,
};

// The SDL2 queue is separate from SDL3's. Keep event switches on the guest
// queue so SDL3 can still track devices and feed the host input layer.
std::set<uint32_t> g_disabled_events{kSysWm};

bool SetEventState(uint32_t type, int32_t state) {
  const bool was_enabled = !g_disabled_events.contains(type);
  if (state == 1) g_disabled_events.erase(type);
  if (state == 0 && was_enabled) {
    g_disabled_events.insert(type);
    std::erase_if(g_events, [type](const Event& event) {
      uint32_t queued_type;
      std::memcpy(&queued_type, event.data(), sizeof queued_type);
      return queued_type == type;
    });
  }
  return was_enabled;
}

// SDL2's joystick device indices (SDL_NumJoysticks, SDL_GameControllerOpen,
// the which of the ADDED events): the connected joysticks in the order
// they appeared. SDL3 has instance IDs only.
std::mutex g_dev_mu;
std::vector<SDL_JoystickID> g_devices;

// Brings the list up to date with the connected joysticks: SDL2 counts
// devices as the driver sees them, before their events are pumped.
void SyncDevices() {
  int n = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&n);
  std::lock_guard<std::mutex> l(g_dev_mu);
  std::erase_if(g_devices, [&](SDL_JoystickID d) { return !std::count(ids, ids + n, d); });
  for (int i = 0; i < n; i++)
    if (!std::count(g_devices.begin(), g_devices.end(), ids[i])) g_devices.push_back(ids[i]);
  SDL_free(ids);
}

int DeviceIndex(SDL_JoystickID id) {
  std::lock_guard<std::mutex> l(g_dev_mu);
  for (size_t i = 0; i < g_devices.size(); i++)
    if (g_devices[i] == id) return int(i);
  return -1;
}

// The joystick at SDL2 device index i, or 0.
SDL_JoystickID DeviceAt(int i) {
  SyncDevices();
  std::lock_guard<std::mutex> l(g_dev_mu);
  return i >= 0 && size_t(i) < g_devices.size() ? g_devices[i] : 0;
}

struct Builder {
  Event e{};
  Builder(uint32_t type, uint64_t timestamp_ns) {
    Put32(0, type);
    Put32(4, uint32_t(timestamp_ns / 1000000));
  }
  void Put8(int o, uint8_t v) { e[o] = v; }
  void Put16(int o, uint16_t v) { std::memcpy(&e[o], &v, 2); }
  void Put32(int o, uint32_t v) { std::memcpy(&e[o], &v, 4); }
  void PutF(int o, float v) { std::memcpy(&e[o], &v, 4); }
  void Push() {
    std::lock_guard<std::mutex> l(g_ev_mu);
    uint32_t type;
    std::memcpy(&type, e.data(), sizeof type);
    if (!g_disabled_events.contains(type)) g_events.push_back(e);
  }
};

// SDL3 window events with an SDL2 SDL_WINDOWEVENT counterpart: its event id.
uint8_t WindowEventId(uint32_t t) {
  switch (t) {
    case SDL_EVENT_WINDOW_SHOWN: return 1;
    case SDL_EVENT_WINDOW_HIDDEN: return 2;
    case SDL_EVENT_WINDOW_EXPOSED: return 3;
    case SDL_EVENT_WINDOW_MOVED: return 4;
    case SDL_EVENT_WINDOW_RESIZED: return 5;
    case SDL_EVENT_WINDOW_MINIMIZED: return 7;
    case SDL_EVENT_WINDOW_MAXIMIZED: return 8;
    case SDL_EVENT_WINDOW_RESTORED: return 9;
    case SDL_EVENT_WINDOW_MOUSE_ENTER: return 10;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return 11;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return 12;
    case SDL_EVENT_WINDOW_FOCUS_LOST: return 13;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return 14;
    case SDL_EVENT_WINDOW_HIT_TEST: return 16;
    case SDL_EVENT_WINDOW_ICCPROF_CHANGED: return 17;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: return 18;
    default: return 0;
  }
}

int DisplayIndex(SDL_DisplayID id) {
  int n = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&n);
  int r = -1;
  for (int i = 0; i < n; i++)
    if (ids[i] == id) r = i;
  SDL_free(ids);
  return r;
}

// SDL2 keycodes are SDL3's except for SDL3's extended ones (keys SDL2
// cannot name: SDLK_UNKNOWN); SDL3's KMOD_LEVEL5 does not exist in SDL2.
uint32_t Keycode(SDL_Keycode k) { return k & SDLK_EXTENDED_MASK ? 0 : k; }
uint16_t Keymod(SDL_Keymod m) { return uint16_t(m & ~SDL_KMOD_LEVEL5); }

// Relative mouse motion in pixels: SDL3's float deltas, with the
// fractions carried so slow motion is not lost.
float g_rel_x = 0, g_rel_y = 0;

// Text in SDL2's 32-byte event buffers: at most 31 bytes, split on UTF-8
// sequence boundaries.
std::vector<std::string> TextChunks(const char* text) {
  std::vector<std::string> out;
  std::string s = text ? text : "";
  size_t i = 0;
  while (i < s.size()) {
    size_t n = std::min<size_t>(31, s.size() - i);
    while (n && i + n < s.size() && (uint8_t(s[i + n]) & 0xC0) == 0x80) n--;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

void Convert(const SDL_Event& e) {
  const uint64_t ts = e.common.timestamp;
  switch (e.type) {
    case SDL_EVENT_QUIT: Builder(kQuit, ts).Push(); return;
    case SDL_EVENT_TERMINATING: case SDL_EVENT_LOW_MEMORY: case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND: case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND: case SDL_EVENT_LOCALE_CHANGED:
      Builder(e.type, ts).Push();  // the same numbers in SDL2, no payload
      return;
    case SDL_EVENT_KEYMAP_CHANGED: Builder(kKeymapChanged, ts).Push(); return;
    case SDL_EVENT_CLIPBOARD_UPDATE: Builder(kClipboard, ts).Push(); return;
    case SDL_EVENT_DISPLAY_ORIENTATION: case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_REMOVED: case SDL_EVENT_DISPLAY_MOVED: {
      Builder b(kDisplayEvent, ts);
      b.Put32(8, uint32_t(DisplayIndex(e.display.displayID)));
      b.Put8(12, uint8_t(e.type - SDL_EVENT_DISPLAY_ORIENTATION + 1));
      b.Put32(16, uint32_t(e.display.data1));
      b.Push();
      return;
    }
    default: break;
  }
  if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) {
    uint8_t id = WindowEventId(e.type);
    if (!id) return;
    SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
    int32_t d1 = e.window.data1, d2 = e.window.data2;
    if (e.type == SDL_EVENT_WINDOW_RESIZED && w) {
#if RECOMP_IOS_GLES
      if (SDL_GetWindowFlags(w) & SDL_WINDOW_OPENGL) {
        GameDrawableSize(w, &d1, &d2);
      } else
#endif
      {
        d1 = ToPixels(w, float(d1));
        d2 = ToPixels(w, float(d2));
      }
    } else if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
      d1 = DisplayIndex(SDL_DisplayID(d1));
    }
    auto push = [&](uint8_t ev) {
      Builder b(kWindowEvent, ts);
      b.Put32(8, e.window.windowID);
      b.Put8(12, ev);
      b.Put32(16, uint32_t(d1));
      b.Put32(20, uint32_t(d2));
      b.Push();
    };
    // SDL2 follows every size change with SIZE_CHANGED.
    push(id);
    if (id == 5) push(6);
    return;
  }
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
      Builder b(e.key.down ? kKeyDown : kKeyUp, ts);
      b.Put32(8, e.key.windowID);
      b.Put8(12, e.key.down);
      b.Put8(13, e.key.repeat);
      b.Put32(16, uint32_t(e.key.scancode));
      b.Put32(20, Keycode(e.key.key));
      b.Put16(24, Keymod(e.key.mod));
      b.Push();
      return;
    }
    case SDL_EVENT_TEXT_INPUT:
      for (const std::string& t : TextChunks(e.text.text)) {
        Builder b(kTextInput, ts);
        b.Put32(8, e.text.windowID);
        std::memcpy(&b.e[12], t.data(), t.size());
        b.Push();
      }
      return;
    case SDL_EVENT_TEXT_EDITING: {
      std::vector<std::string> t = TextChunks(e.edit.text);
      Builder b(kTextEditing, ts);
      b.Put32(8, e.edit.windowID);
      if (!t.empty()) std::memcpy(&b.e[12], t[0].data(), t[0].size());
      b.Put32(44, uint32_t(e.edit.start));
      b.Put32(48, uint32_t(e.edit.length));
      b.Push();
      return;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      SDL_Window* w = SDL_GetWindowFromID(e.motion.windowID);
      float x = e.motion.x, y = e.motion.y, dx = e.motion.xrel, dy = e.motion.yrel;
      GuestPointer(w, x, y, dx, dy);
      g_rel_x += dx;
      g_rel_y += dy;
      int32_t rx = int32_t(std::trunc(g_rel_x)), ry = int32_t(std::trunc(g_rel_y));
      g_rel_x -= float(rx);
      g_rel_y -= float(ry);
      Builder b(kMouseMotion, ts);
      b.Put32(8, e.motion.windowID);
      b.Put32(12, e.motion.which);
      b.Put32(16, e.motion.state);
      b.Put32(20, uint32_t(int32_t(std::floor(x))));
      b.Put32(24, uint32_t(int32_t(std::floor(y))));
      b.Put32(28, uint32_t(rx));
      b.Put32(32, uint32_t(ry));
      b.Push();
      return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
      SDL_Window* w = SDL_GetWindowFromID(e.button.windowID);
      float x = e.button.x, y = e.button.y, dx = 0, dy = 0;
      GuestPointer(w, x, y, dx, dy);
      Builder b(e.button.down ? kMouseButtonDown : kMouseButtonUp, ts);
      b.Put32(8, e.button.windowID);
      b.Put32(12, e.button.which);
      b.Put8(16, e.button.button);
      b.Put8(17, e.button.down);
      b.Put8(18, e.button.clicks);
      b.Put32(20, uint32_t(int32_t(std::lround(x))));
      b.Put32(24, uint32_t(int32_t(std::lround(y))));
      b.Push();
      return;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      SDL_Window* w = SDL_GetWindowFromID(e.wheel.windowID);
      float x = e.wheel.mouse_x, y = e.wheel.mouse_y, dx = 0, dy = 0;
      GuestPointer(w, x, y, dx, dy);
      Builder b(kMouseWheel, ts);
      b.Put32(8, e.wheel.windowID);
      b.Put32(12, e.wheel.which);
      b.Put32(16, uint32_t(e.wheel.integer_x));
      b.Put32(20, uint32_t(e.wheel.integer_y));
      b.Put32(24, uint32_t(e.wheel.direction));
      b.PutF(28, e.wheel.x);
      b.PutF(32, e.wheel.y);
      b.Put32(36, uint32_t(int32_t(std::lround(x))));
      b.Put32(40, uint32_t(int32_t(std::lround(y))));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_ADDED: {
      {
        std::lock_guard<std::mutex> l(g_dev_mu);
        if (!std::count(g_devices.begin(), g_devices.end(), e.jdevice.which))
          g_devices.push_back(e.jdevice.which);
      }
      Builder b(kJoyDeviceAdded, ts);
      b.Put32(8, uint32_t(DeviceIndex(e.jdevice.which)));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_REMOVED: {
      {
        std::lock_guard<std::mutex> l(g_dev_mu);
        std::erase(g_devices, e.jdevice.which);
      }
      Builder b(kJoyDeviceRemoved, ts);
      b.Put32(8, e.jdevice.which);
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
      Builder b(kJoyAxis, ts);
      b.Put32(8, e.jaxis.which);
      b.Put8(12, e.jaxis.axis);
      b.Put16(16, uint16_t(e.jaxis.value));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_BALL_MOTION: {
      Builder b(kJoyBall, ts);
      b.Put32(8, e.jball.which);
      b.Put8(12, e.jball.ball);
      b.Put16(16, uint16_t(e.jball.xrel));
      b.Put16(18, uint16_t(e.jball.yrel));
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
      Builder b(kJoyHat, ts);
      b.Put32(8, e.jhat.which);
      b.Put8(12, e.jhat.hat);
      b.Put8(13, e.jhat.value);
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP: {
      Builder b(e.jbutton.down ? kJoyButtonDown : kJoyButtonUp, ts);
      b.Put32(8, e.jbutton.which);
      b.Put8(12, e.jbutton.button);
      b.Put8(13, e.jbutton.down);
      b.Push();
      return;
    }
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED: {
      // SDL_JoystickPowerLevel: unknown -1, empty, low, medium, full, wired.
      int level = e.jbattery.state == SDL_POWERSTATE_CHARGING ||
                          e.jbattery.state == SDL_POWERSTATE_CHARGED
                      ? 5
                  : e.jbattery.percent < 0   ? -1
                  : e.jbattery.percent <= 5  ? 0
                  : e.jbattery.percent <= 20 ? 1
                  : e.jbattery.percent <= 70 ? 2
                                             : 3;
      Builder b(kJoyBattery, ts);
      b.Put32(8, e.jbattery.which);
      b.Put32(12, uint32_t(level));
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      Builder b(kPadAxis, ts);
      b.Put32(8, e.gaxis.which);
      b.Put8(12, e.gaxis.axis);
      b.Put16(16, uint16_t(e.gaxis.value));
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      // SDL2's button numbers are SDL3's (A = south, ..., the paddles in
      // the same order).
      Builder b(e.gbutton.down ? kPadButtonDown : kPadButtonUp, ts);
      b.Put32(8, e.gbutton.which);
      b.Put8(12, e.gbutton.button);
      b.Put8(13, e.gbutton.down);
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
      Builder b(kPadAdded, ts);
      b.Put32(8, uint32_t(DeviceIndex(e.gdevice.which)));
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: case SDL_EVENT_GAMEPAD_REMAPPED: {
      Builder b(e.type == SDL_EVENT_GAMEPAD_REMOVED ? kPadRemoved : kPadRemapped, ts);
      b.Put32(8, e.gdevice.which);
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {
      Builder b(kPadTouchDown + (e.type - SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN), ts);
      b.Put32(8, e.gtouchpad.which);
      b.Put32(12, uint32_t(e.gtouchpad.touchpad));
      b.Put32(16, uint32_t(e.gtouchpad.finger));
      b.PutF(20, e.gtouchpad.x);
      b.PutF(24, e.gtouchpad.y);
      b.PutF(28, e.gtouchpad.pressure);
      b.Push();
      return;
    }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
      Builder b(kPadSensor, ts);
      b.Put32(8, e.gsensor.which);
      b.Put32(12, uint32_t(e.gsensor.sensor));
      for (int i = 0; i < 3; i++) b.PutF(16 + 4 * i, e.gsensor.data[i]);
      uint64_t us = e.gsensor.sensor_timestamp / 1000;
      std::memcpy(&b.e[32], &us, 8);
      b.Push();
      return;
    }
    default:
      return;  // no SDL2 counterpart, or disabled by default in SDL2
  }
}

bool g_events_up = false;

// SDL2's video is up: its events from now on (main thread).
void TakeHostEvents() {
  if (g_events_up) return;
  g_events_up = true;
  SetHostEventSink(Convert);
}

}  // namespace

#if RECOMP_IOS_GLES
uint32_t ActiveGlesWindowDc() {
  SDL_Window* active = SDL_GL_GetCurrentWindow();
  if (!active) return 0;
  std::lock_guard<std::mutex> lock(g_win_mu);
  for (const auto& [_, window] : g_windows)
    if (window.sdl == active) return DcForWindow(window.hwnd);
  return 0;
}
#endif

// ---- windows --------------------------------------------------------------------------

// SDL_CreateWindow(title, x, y, w, h, flags).
HLE64(kDll, SDL_CreateWindow, kCdecl) {
  std::string title = Arg(c, 0) ? GStr(Arg(c, 0)) : "";
  int w = int(int32_t(Arg(c, 3))), h = int(int32_t(Arg(c, 4)));
  uint32_t flags = uint32_t(Arg(c, 5));
  RECOMP_TRACE("  SDL_CreateWindow(\"%s\", %d, %d, %dx%d, flags %X)\n", title.c_str(),
               int(int32_t(Arg(c, 1))), int(int32_t(Arg(c, 2))), w, h, flags);
  SDL_WindowFlags f = 0;
#if RECOMP_IOS_GLES
  if (flags & kOpenGl) {
    if (!SetGlesProfile()) {
      std::fprintf(stderr, "Darkest Dungeon: OpenGL ES profile failed: %s\n", SDL_GetError());
      return 0;
    }
    f |= SDL_WINDOW_OPENGL;
  }
  if (flags & kFullscreen) f |= SDL_WINDOW_FULLSCREEN;
#else
  if (flags & (kFullscreen | kOpenGl))
    FatalF("SDL_CreateWindow: flags %X (fullscreen/OpenGL windows not provided yet)", flags);
#endif
  if (flags & kHidden) f |= SDL_WINDOW_HIDDEN;  // SDL_WINDOW_SHOWN is ignored
  if (flags & kBorderless) f |= SDL_WINDOW_BORDERLESS;
  if (flags & kResizable) f |= SDL_WINDOW_RESIZABLE;
  if (flags & kMinimized) f |= SDL_WINDOW_MINIMIZED;
  if (flags & kMaximized) f |= SDL_WINDOW_MAXIMIZED;
  if (flags & kAlwaysOnTop) f |= SDL_WINDOW_ALWAYS_ON_TOP;
  TakeHostEvents();
  SDL_Window* sdl;
#if RECOMP_IOS_GLES
  std::fprintf(stderr, "Darkest Dungeon: creating SDL window %dx%d, flags 0x%x.\n", w, h, flags);
#endif
  uint32_t hwnd = CreateSdl2Window(title, w, h, f, &sdl);
  if (!hwnd) {
#if RECOMP_IOS_GLES
    std::fprintf(stderr, "Darkest Dungeon: SDL window failed: %s\n", SDL_GetError());
#endif
    return 0;
  }
#if RECOMP_IOS_GLES
  if (!(flags & kHidden)) RequestGameLandscape(sdl);
#endif
  gaddr p = HeapAllocProcess(16, true);
  std::lock_guard<std::mutex> l(g_win_mu);
  g_windows[p] = {sdl, hwnd, flags & (kAllowHighDpi | kOpenGl)};
  return p;
}

HLE64(kDll, SDL_DestroyWindow, kCdecl) {
  Sdl2Window w = Must(Arg(c, 0), "SDL_DestroyWindow");
#if RECOMP_IOS_GLES
  ForgetGameDrawable(w.sdl);
#endif
  {
    std::lock_guard<std::mutex> l(g_win_mu);
#if RECOMP_IOS_GLES
    for (auto it = g_gl_contexts.begin(); it != g_gl_contexts.end();) {
      if (it->second.window != Arg(c, 0)) {
        ++it;
        continue;
      }
      SDL_GL_DestroyContext(it->second.native);
      HeapFreeProcess(it->first);
      it = g_gl_contexts.erase(it);
    }
#endif
    g_windows.erase(Arg(c, 0));
  }
  DestroySdl2Window(w.hwnd);
  HeapFreeProcess(Arg(c, 0));
  return 0;
}

#if RECOMP_IOS_GLES
HLE64(kDll, SDL_GL_SetAttribute, kCdecl) {
  SDL_GLAttr attribute = static_cast<SDL_GLAttr>(uint32_t(Arg(c, 0)));
  if (attribute == SDL_GL_CONTEXT_PROFILE_MASK || attribute == SDL_GL_CONTEXT_MAJOR_VERSION ||
      attribute == SDL_GL_CONTEXT_MINOR_VERSION) {
    return SetGlesProfile() ? 0 : UINT32_MAX;
  }
  if (attribute >= SDL_GL_FLOATBUFFERS) {
    SDL_SetError("Unknown SDL2 OpenGL attribute");
    return UINT32_MAX;
  }
  return SDL_GL_SetAttribute(attribute, int32_t(Arg(c, 1))) ? 0 : UINT32_MAX;
}

HLE64(kDll, SDL_GL_CreateContext, kCdecl) {
  const auto window = Arg(c, 0);
  auto* sdl = Must(window, "SDL_GL_CreateContext").sdl;
  SDL_GLContext native = SDL_GL_CreateContext(sdl);
  if (!native) {
    std::fprintf(stderr, "Darkest Dungeon: OpenGL ES context failed: %s\n", SDL_GetError());
    return 0;
  }
  CaptureGlesDefaultFramebuffer(sdl, {});
  std::fprintf(stderr, "Darkest Dungeon: OpenGL ES context ready.\n");
  const auto handle = HeapAllocProcess(8, true);
  std::lock_guard<std::mutex> lock(g_win_mu);
  g_gl_contexts.emplace(handle, GlContext{window, native});
  return handle;
}

HLE64(kDll, SDL_GL_DeleteContext, kCdecl) {
  const auto handle = Arg(c, 0);
  std::lock_guard<std::mutex> lock(g_win_mu);
  auto found = g_gl_contexts.find(handle);
  if (found == g_gl_contexts.end()) return 0;
  SDL_Window* active = found->second.native == SDL_GL_GetCurrentContext()
                         ? SDL_GL_GetCurrentWindow() : nullptr;
  if (SDL_GL_DestroyContext(found->second.native)) {
    if (active) ForgetGameDrawable(active);
    g_gl_contexts.erase(found);
    HeapFreeProcess(handle);
  }
  return 0;
}

HLE64(kDll, SDL_GL_GetDrawableSize, kCdecl) {
  SDL_Window* window = Must(Arg(c, 0), "SDL_GL_GetDrawableSize").sdl;
  int width = 0, height = 0;
  GameDrawableSize(window, &width, &height);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(width));
  if (Arg(c, 2)) Wr32(Arg(c, 2), uint32_t(height));
  return 0;
}

HLE64(kDll, SDL_GL_SetSwapInterval, kCdecl) {
  return SDL_GL_SetSwapInterval(int32_t(Arg(c, 0))) ? 0 : UINT32_MAX;
}

HLE64(kDll, SDL_GL_SwapWindow, kCdecl) {
  SDL_Window* window = Must(Arg(c, 0), "SDL_GL_SwapWindow").sdl;
  const auto present_start = SDL_GetTicksNS();
  uint64_t swap_ns = 0;
  const bool swapped = PresentGlesWindow(window, swap_ns);
  const auto now = SDL_GetTicksNS();
  if (swapped) WatchdogFrame();
  static bool logged_first_swap = false;
  if (swapped && !logged_first_swap) {
    logged_first_swap = true;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    std::fprintf(stderr, "Darkest Dungeon: first OpenGL ES swap at %llu ms, %dx%d.\n",
                 (unsigned long long)SDL_GetTicks(), width, height);
  }
  static uint64_t previous_frame = 0, period_start = 0, longest_frame = 0;
  static uint64_t total_present = 0, longest_present = 0, total_swap = 0;
  static unsigned frames = 0, slow_frames = 0;
  if (swapped) {
    if (period_start) {
      ++frames;
      longest_frame = std::max(longest_frame, now - previous_frame);
      if (now - previous_frame >= SDL_NS_PER_SECOND / 30) ++slow_frames;
      total_present += now - present_start;
      total_swap += swap_ns;
      longest_present = std::max(longest_present, now - present_start);
      if (now - period_start >= 10 * SDL_NS_PER_SECOND) {
        std::fprintf(stderr, "Darkest Dungeon: %.1f fps over %.1f s, longest frame %.1f ms, "
                     "frames >=33.3 ms %u/%u, present avg %.2f ms, max %.2f ms, "
                     "SDL swap avg %.2f ms (includes vsync).\n",
                     frames * double(SDL_NS_PER_SECOND) / (now - period_start),
                     double(now - period_start) / SDL_NS_PER_SECOND,
                     double(longest_frame) / SDL_NS_PER_MS, slow_frames, frames,
                     double(total_present) / frames / SDL_NS_PER_MS,
                     double(longest_present) / SDL_NS_PER_MS,
                     double(total_swap) / frames / SDL_NS_PER_MS);
        LogGlesVertexStream();
        period_start = now;
        frames = slow_frames = 0;
        longest_frame = 0;
        total_present = longest_present = total_swap = 0;
      }
    } else {
      period_start = now;
    }
    previous_frame = now;
  }
  PumpHostEvents();
  return 0;
}
#endif

HLE64(kDll, SDL_ShowWindow, kCdecl) {
  SDL_Window* window = Must(Arg(c, 0), "SDL_ShowWindow").sdl;
  SDL_ShowWindow(window);
#if RECOMP_IOS_GLES
  RequestGameLandscape(window);
#endif
  return 0;
}
HLE64(kDll, SDL_HideWindow, kCdecl) {
  SDL_HideWindow(Must(Arg(c, 0), "SDL_HideWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_RaiseWindow, kCdecl) {
  if (!Background()) SDL_RaiseWindow(Must(Arg(c, 0), "SDL_RaiseWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_MinimizeWindow, kCdecl) {
  SDL_MinimizeWindow(Must(Arg(c, 0), "SDL_MinimizeWindow").sdl);
  return 0;
}
HLE64(kDll, SDL_SetWindowTitle, kCdecl) {
  SDL_SetWindowTitle(Must(Arg(c, 0), "SDL_SetWindowTitle").sdl, GStr(Arg(c, 1)).c_str());
  return 0;
}

// The client area in pixels.
HLE64(kDll, SDL_GetWindowSize, kCdecl) {
  SDL_Window* w = Must(Arg(c, 0), "SDL_GetWindowSize").sdl;
  int pw = 0, ph = 0;
#if RECOMP_IOS_GLES
  if (SDL_GetWindowFlags(w) & SDL_WINDOW_OPENGL) GameDrawableSize(w, &pw, &ph);
  else
#endif
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(pw));
  if (Arg(c, 2)) Wr32(Arg(c, 2), uint32_t(ph));
  return 0;
}
HLE64(kDll, SDL_SetWindowSize, kCdecl) {
  SDL_Window* w = Must(Arg(c, 0), "SDL_SetWindowSize").sdl;
  SDL_SetWindowSize(w, ToPoints(w, int(int32_t(Arg(c, 1)))), ToPoints(w, int(int32_t(Arg(c, 2)))));
  return 0;
}

HLE64(kDll, SDL_GetWindowFlags, kCdecl) {
  Sdl2Window w = Must(Arg(c, 0), "SDL_GetWindowFlags");
  SDL_WindowFlags f = SDL_GetWindowFlags(w.sdl);
  uint32_t r = w.flags;
  if (f & SDL_WINDOW_FULLSCREEN)
    r |= SDL_GetWindowFullscreenMode(w.sdl) ? kFullscreen : kFullscreenDesktop;
  r |= f & SDL_WINDOW_HIDDEN ? kHidden : kShown;
  if (f & SDL_WINDOW_BORDERLESS) r |= kBorderless;
  if (f & SDL_WINDOW_RESIZABLE) r |= kResizable;
  if (f & SDL_WINDOW_MINIMIZED) r |= kMinimized;
  if (f & SDL_WINDOW_MAXIMIZED) r |= kMaximized;
  if (f & SDL_WINDOW_MOUSE_GRABBED) r |= kMouseGrabbed;
  if (f & SDL_WINDOW_INPUT_FOCUS) r |= kInputFocus;
  if (f & SDL_WINDOW_MOUSE_FOCUS) r |= kMouseFocus;
  if (f & SDL_WINDOW_MOUSE_CAPTURE) r |= kMouseCapture;
  if (f & SDL_WINDOW_ALWAYS_ON_TOP) r |= kAlwaysOnTop;
  if (f & SDL_WINDOW_KEYBOARD_GRABBED) r |= kKeyboardGrabbed;
  return r;
}

#if RECOMP_IOS_GLES
HLE64(kDll, SDL_GetNumVideoDisplays, kCdecl) {
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays) return UINT32_MAX;
  SDL_free(displays);
  return uint32_t(count);
}

HLE64(kDll, SDL_GetDisplayBounds, kCdecl) {
  const int index = int32_t(Arg(c, 0));
  if (!Arg(c, 1)) return UINT32_MAX;
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays) return UINT32_MAX;
  const SDL_DisplayID display = index >= 0 && index < count ? displays[index] : 0;
  SDL_free(displays);
  SDL_Rect bounds{};
  if (!display || !SDL_GetDisplayBounds(display, &bounds)) return UINT32_MAX;
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
  const float density = mode && mode->pixel_density > 0 ? mode->pixel_density : 1.0f;
  const gaddr out = Arg(c, 1);
  Wr32(out, uint32_t(std::lround(bounds.x * density)));
  Wr32(out + 4, uint32_t(std::lround(bounds.y * density)));
  Wr32(out + 8, uint32_t(std::lround(bounds.w * density)));
  Wr32(out + 12, uint32_t(std::lround(bounds.h * density)));
  return 0;
}

HLE64(kDll, SDL_SetWindowPosition, kCdecl) {
  SDL_Window* window = Must(Arg(c, 0), "SDL_SetWindowPosition").sdl;
  const auto position = [window](uint32_t value) {
    if (SDL_WINDOWPOS_ISCENTERED(value)) return int(SDL_WINDOWPOS_CENTERED);
    if (SDL_WINDOWPOS_ISUNDEFINED(value)) return int(SDL_WINDOWPOS_UNDEFINED);
    return ToPoints(window, int32_t(value));
  };
  SDL_SetWindowPosition(window, position(Arg(c, 1)), position(Arg(c, 2)));
  return 0;
}

HLE64(kDll, SDL_SetTextInputRect, kCdecl) {
  SDL_Window* window = SDL_GetKeyboardFocus();
  if (!window || !Arg(c, 0)) return 0;
  const gaddr r = Arg(c, 0);
  float left = 0, top = 0, sx = 1, sy = 1;
  GuestPointer(window, left, top, sx, sy);
  if (sx <= 0 || sy <= 0) return 0;
  const SDL_Rect area{
      int(std::lround((int32_t(Rd32(r)) - left) / sx)),
      int(std::lround((int32_t(Rd32(r + 4)) - top) / sy)),
      int(std::lround(int32_t(Rd32(r + 8)) / sx)),
      int(std::lround(int32_t(Rd32(r + 12)) / sy))};
  SDL_SetTextInputArea(window, &area, 0);
  return 0;
}

HLE64(kDll, SDL_StartTextInput, kCdecl) {
  if (SDL_Window* window = SDL_GetKeyboardFocus()) SDL_StartTextInput(window);
  return 0;
}

HLE64(kDll, SDL_StopTextInput, kCdecl) {
  if (SDL_Window* window = SDL_GetKeyboardFocus()) SDL_StopTextInput(window);
  return 0;
}

HLE64(kDll, SDL_ShowSimpleMessageBox, kCdecl) {
  const auto title = GStr(Arg(c, 1));
  const auto message = GStr(Arg(c, 2));
  auto* window = Arg(c, 3) ? Must(Arg(c, 3), "SDL_ShowSimpleMessageBox").sdl : nullptr;
  return SDL_ShowSimpleMessageBox(uint32_t(Arg(c, 0)), title.c_str(), message.c_str(), window)
            ? 0 : UINT32_MAX;
}
#endif

// SDL_GetWindowWMInfo(window, SDL_SysWMinfo*): {SDL_version (3 bytes),
// subsystem @4, HWND @8, HDC @16, HINSTANCE @24} for SDL_SYSWM_WINDOWS.
HLE64(kDll, SDL_GetWindowWMInfo, kCdecl) {
  Sdl2Window w = Must(Arg(c, 0), "SDL_GetWindowWMInfo");
  gaddr info = Arg(c, 1);
  if (Rd8(info) != 2) {
    SDL_SetError("Application not compiled with SDL 2");
    return 0;
  }
  Wr32(info + 4, 1);  // SDL_SYSWM_WINDOWS
  Wr64(info + 8, w.hwnd);
  Wr64(info + 16, DcForWindow(w.hwnd));
  Wr64(info + 24, g_win32->img.image_base);
  return 1;
}

// ---- events ---------------------------------------------------------------------------

HLE64(kDll, SDL_EventState, kCdecl) {
  std::lock_guard<std::mutex> l(g_ev_mu);
  return Bool(SetEventState(uint32_t(Arg(c, 0)), int32_t(Arg(c, 1))));
}

HLE64(kDll, SDL_PumpEvents, kCdecl) {
  PumpHostEvents();
  return 0;
}

// SDL_PollEvent(event): pumps, then takes the next event (with a null
// event, only tells whether there is one).
HLE64(kDll, SDL_PollEvent, kCdecl) {
  PumpHostEvents();
  std::lock_guard<std::mutex> l(g_ev_mu);
  if (g_events.empty()) return 0;
  if (gaddr out = Arg(c, 0)) {
    std::memcpy(GPtr(out), g_events.front().data(), 56);
    g_events.pop_front();
  }
  return 1;
}

#if RECOMP_IOS_GLES
namespace {
std::mutex g_surface_mu;
std::map<gaddr, SDL_Surface*> g_surfaces;
}

HLE64(kDll, SDL_CreateRGBSurfaceFrom, kCdecl) {
  const auto format = SDL_GetPixelFormatForMasks(
      int32_t(Arg(c, 3)), uint32_t(Arg(c, 5)), uint32_t(Arg(c, 6)),
      uint32_t(Arg(c, 7)), uint32_t(Arg(c, 8)));
  if (format == SDL_PIXELFORMAT_UNKNOWN) return 0;
  const gaddr pixels = Arg(c, 0);
  SDL_Surface* surface = SDL_CreateSurfaceFrom(
      int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), format,
      pixels ? GPtr(pixels) : nullptr, int32_t(Arg(c, 4)));
  if (!surface) return 0;
  const auto handle = HeapAllocProcess(8, true);
  std::lock_guard<std::mutex> lock(g_surface_mu);
  g_surfaces.emplace(handle, surface);
  return handle;
}

HLE64(kDll, SDL_FreeSurface, kCdecl) {
  const auto handle = Arg(c, 0);
  std::lock_guard<std::mutex> lock(g_surface_mu);
  if (auto surface = g_surfaces.find(handle); surface != g_surfaces.end()) {
    SDL_DestroySurface(surface->second);
    g_surfaces.erase(surface);
    HeapFreeProcess(handle);
  }
  return 0;
}

HLE64(kDll, SDL_CreateColorCursor, kCdecl) {
  // The game accesses SDL2's Windows cursor driver data and replaces its
  // HCURSOR. iOS has no HCURSOR, so let the caller retain the default pointer.
  SDL_Unsupported();
  return 0;
}

HLE64(kDll, SDL_SetCursor, kCdecl) {
  if (!Arg(c, 0)) SDL_SetCursor(nullptr);
  return 0;
}

HLE64(kDll, SDL_FreeCursor, kCdecl) { return 0; }

HLE64(kDll, SDL_ShowCursor, kCdecl) {
  const auto toggle = int32_t(Arg(c, 0));
  if ((toggle == 1 && !SDL_ShowCursor()) || (toggle == 0 && !SDL_HideCursor()))
    return UINT32_MAX;
  return Bool(SDL_CursorVisible());
}
#endif

// ---- game controllers ---------------------------------------------------------------
// A guest SDL_GameController* or SDL_Joystick* is a heap block standing for
// the host gamepad; a controller's joystick (SDL_GameControllerGetJoystick)
// is one block per controller, as SDL2 returns the same pointer each time.

HLE64(kDll, SDL_GameControllerEventState, kCdecl) {
  const auto state = int32_t(Arg(c, 0));
  std::lock_guard<std::mutex> l(g_ev_mu);
  bool any_enabled = false;
  for (uint32_t type = kPadAxis; type <= kPadSensor; ++type)
    any_enabled = SetEventState(type, state) || any_enabled;
  return state == -1 ? Bool(any_enabled) : uint32_t(state);
}

namespace {

struct Pad {
  SDL_Gamepad* pad;
  gaddr joystick;  // its SDL_Joystick* block
  gaddr name;      // guest copy of the name (SDL_GameControllerName)
};

std::mutex g_pad_mu;
std::map<gaddr, Pad> g_pads;           // by SDL_GameController*
std::map<gaddr, SDL_Joystick*> g_joys;  // by SDL_Joystick*

SDL_Gamepad* PadOf(gaddr gc, const char* api) {
  std::lock_guard<std::mutex> l(g_pad_mu);
  auto it = g_pads.find(gc);
  if (it == g_pads.end()) FatalF("%s: unknown SDL_GameController %08llX", api, (unsigned long long)gc);
  return it->second.pad;
}

SDL_Joystick* JoyOf(gaddr j, const char* api) {
  std::lock_guard<std::mutex> l(g_pad_mu);
  auto it = g_joys.find(j);
  if (it == g_joys.end()) FatalF("%s: unknown SDL_Joystick %08llX", api, (unsigned long long)j);
  return it->second;
}

}  // namespace

HLE64(kDll, SDL_NumJoysticks, kCdecl) {
  SyncDevices();
  std::lock_guard<std::mutex> l(g_dev_mu);
  return g_devices.size();
}

HLE64(kDll, SDL_IsGameController, kCdecl) {
  SDL_JoystickID id = DeviceAt(int(int32_t(Arg(c, 0))));
  return Bool(id && SDL_IsGamepad(id));
}

HLE64(kDll, SDL_JoystickGetDeviceInstanceID, kCdecl) {
  SDL_JoystickID id = DeviceAt(int(int32_t(Arg(c, 0))));
  return id ? id : uint32_t(-1);
}

HLE64(kDll, SDL_GameControllerOpen, kCdecl) {
  SDL_JoystickID id = DeviceAt(int(int32_t(Arg(c, 0))));
  SDL_Gamepad* pad = id ? SDL_OpenGamepad(id) : nullptr;
  if (!pad) {
    if (!id) SDL_SetError("There are %d joysticks available", int(g_devices.size()));
    return 0;
  }
  // SDL2 hands out one object per device, reference counted.
  std::lock_guard<std::mutex> l(g_pad_mu);
  for (auto& [gc, p] : g_pads)
    if (p.pad == pad) return gc;
  gaddr gc = HeapAllocProcess(16, true), j = HeapAllocProcess(16, true);
  const char* name = SDL_GetGamepadName(pad);
  std::string n = name ? name : "";
  gaddr gname = HeapAllocProcess(n.size() + 1, false);
  PutStr(gname, n);
  g_pads[gc] = {pad, j, gname};
  g_joys[j] = SDL_GetGamepadJoystick(pad);
  return gc;
}

HLE64(kDll, SDL_GameControllerClose, kCdecl) {
  gaddr gc = Arg(c, 0);
  if (!gc) return 0;
  SDL_Gamepad* pad = PadOf(gc, "SDL_GameControllerClose");
  const SDL_JoystickID id = SDL_GetGamepadID(pad);
  SDL_CloseGamepad(pad);
  // The last close of the device frees it (SDL3 counts opens too).
  if (SDL_GetGamepadFromID(id) == pad) return 0;
  std::lock_guard<std::mutex> l(g_pad_mu);
  Pad p = g_pads[gc];
  g_joys.erase(p.joystick);
  g_pads.erase(gc);
  HeapFreeProcess(p.joystick);
  HeapFreeProcess(p.name);
  HeapFreeProcess(gc);
  return 0;
}

HLE64(kDll, SDL_GameControllerGetAttached, kCdecl) {
  return Bool(SDL_GamepadConnected(PadOf(Arg(c, 0), "SDL_GameControllerGetAttached")));
}

// SDL2's axis numbers are SDL3's.
HLE64(kDll, SDL_GameControllerGetAxis, kCdecl) {
  return uint16_t(SDL_GetGamepadAxis(PadOf(Arg(c, 0), "SDL_GameControllerGetAxis"),
                                     SDL_GamepadAxis(int32_t(Arg(c, 1)))));
}

HLE64(kDll, SDL_GameControllerGetButton, kCdecl) {
  const auto button = SDL_GamepadButton(int32_t(Arg(c, 1)));
  return Bool(SDL_GetGamepadButton(PadOf(Arg(c, 0), "SDL_GameControllerGetButton"), button));
}

HLE64(kDll, SDL_GameControllerUpdate, kCdecl) {
  SDL_UpdateGamepads();
  return 0;
}

HLE64(kDll, SDL_GameControllerGetJoystick, kCdecl) {
  PadOf(Arg(c, 0), "SDL_GameControllerGetJoystick");
  std::lock_guard<std::mutex> l(g_pad_mu);
  return g_pads[Arg(c, 0)].joystick;
}

HLE64(kDll, SDL_JoystickInstanceID, kCdecl) {
  SDL_JoystickID id = SDL_GetJoystickID(JoyOf(Arg(c, 0), "SDL_JoystickInstanceID"));
  return id ? id : uint32_t(-1);
}

HLE64(kDll, SDL_GameControllerName, kCdecl) {
  PadOf(Arg(c, 0), "SDL_GameControllerName");
  std::lock_guard<std::mutex> l(g_pad_mu);
  return g_pads[Arg(c, 0)].name;
}

HLE64(kDll, SDL_GameControllerGetVendor, kCdecl) {
  return SDL_GetGamepadVendor(PadOf(Arg(c, 0), "SDL_GameControllerGetVendor"));
}
HLE64(kDll, SDL_GameControllerGetProduct, kCdecl) {
  return SDL_GetGamepadProduct(PadOf(Arg(c, 0), "SDL_GameControllerGetProduct"));
}

// Sensor types are numbered alike (accelerometer 1, gyroscope 2, ...).
HLE64(kDll, SDL_GameControllerHasSensor, kCdecl) {
  return Bool(SDL_GamepadHasSensor(PadOf(Arg(c, 0), "SDL_GameControllerHasSensor"),
                                   SDL_SensorType(int32_t(Arg(c, 1)))));
}
HLE64(kDll, SDL_GameControllerSetSensorEnabled, kCdecl) {
  return SDL_SetGamepadSensorEnabled(PadOf(Arg(c, 0), "SDL_GameControllerSetSensorEnabled"),
                                     SDL_SensorType(int32_t(Arg(c, 1))), uint32_t(Arg(c, 2)) != 0)
             ? 0
             : uint32_t(-1);
}
HLE64(kDll, SDL_GameControllerGetSensorData, kCdecl) {
  int n = int(int32_t(Arg(c, 3)));
  std::vector<float> v(size_t(std::max(n, 0)));
  if (!SDL_GetGamepadSensorData(PadOf(Arg(c, 0), "SDL_GameControllerGetSensorData"),
                                SDL_SensorType(int32_t(Arg(c, 1))), v.data(), n))
    return uint32_t(-1);
  std::memcpy(GPtr(Arg(c, 2)), v.data(), 4 * v.size());
  return 0;
}

HLE64(kDll, SDL_JoystickIsHaptic, kCdecl) {
  return SDL_IsJoystickHaptic(JoyOf(Arg(c, 0), "SDL_JoystickIsHaptic")) ? 1 : 0;
}

#if RECOMP_IOS_GLES
namespace {
std::mutex g_haptic_mu;
std::map<gaddr, SDL_Haptic*> g_haptics;
}

HLE64(kDll, SDL_NumHaptics, kCdecl) {
  int count = 0;
  auto* devices = SDL_GetHaptics(&count);
  SDL_free(devices);
  return uint32_t(count);
}

HLE64(kDll, SDL_HapticOpenFromJoystick, kCdecl) {
  auto* haptic = SDL_OpenHapticFromJoystick(JoyOf(Arg(c, 0), "SDL_HapticOpenFromJoystick"));
  if (!haptic) return 0;
  const auto handle = HeapAllocProcess(8, false);
  std::lock_guard<std::mutex> lock(g_haptic_mu);
  g_haptics.emplace(handle, haptic);
  return handle;
}

HLE64(kDll, SDL_HapticClose, kCdecl) {
  const auto handle = Arg(c, 0);
  std::lock_guard<std::mutex> lock(g_haptic_mu);
  if (auto haptic = g_haptics.find(handle); haptic != g_haptics.end()) {
    SDL_CloseHaptic(haptic->second);
    g_haptics.erase(haptic);
    HeapFreeProcess(handle);
  }
  return 0;
}
#endif

}  // namespace recomp::hle
