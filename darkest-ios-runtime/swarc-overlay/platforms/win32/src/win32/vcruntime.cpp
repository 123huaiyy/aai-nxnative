// VCRUNTIME140.dll for x64 guests: the memory and string primitives, the
// type_info and std::exception helpers. Its exception handling
// (_CxxThrowException, __CxxFrameHandler4, __C_specific_handler) belongs
// to the x64 unwinder (ARCHITECTURE.md, "64-bit guests").

#include <cstring>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/seh64.h"
#include "x86/lifted.h"
#include "x86/runtime.h"

namespace recomp::hle {
namespace {

const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }

// The Win64 _JUMP_BUFFER stores the preserved registers and the caller's PC.
struct JumpBuffer {
  uint64_t frame, rbx, rsp, rbp, rsi, rdi, r12, r13, r14, r15, rip;
  uint32_t mxcsr;
  uint16_t fpcsr, spare;
  Xmm xmm[10];
};
static_assert(sizeof(JumpBuffer) == 256);

// Win64 MSVC stores the locator before the vtable and uses image-relative
// addresses for the locator and the flattened class hierarchy.
constexpr uint32_t kNotVisible = 0x01;
constexpr uint32_t kAmbiguous = 0x02;
constexpr uint32_t kNonPublicBase = 0x04;
constexpr uint32_t kHasHierarchy = 0x40;

struct RttiObject {
  gaddr locator;
  gaddr image;
  gaddr complete;

  gaddr Relative(gaddr field) const { return image + Rd32(field); }
};

RttiObject DescribeObject(gaddr vfptr) {
  if (!vfptr) return {};
  const auto vtable = Rd64(vfptr);
  if (!vtable) return {};
  const auto locator = Rd64(vtable - 8);
  if (!locator || Rd32(locator) != 1) return {};
  auto complete = vfptr - Rd32(locator + 4);
  if (const auto displacement = Rd32(locator + 8))
    complete -= int32_t(Rd32(vfptr - displacement));
  return {locator, locator - Rd32(locator + 20), complete};
}

gaddr BaseAddress(const RttiObject& object, gaddr descriptor) {
  auto address = object.complete;
  const auto vbptr_offset = int32_t(Rd32(descriptor + 12));
  if (vbptr_offset >= 0) {
    address += vbptr_offset;
    const auto vbtable = Rd64(address);
    address += int32_t(Rd32(vbtable + int32_t(Rd32(descriptor + 16))));
  }
  return address + int32_t(Rd32(descriptor + 8));
}

bool SameType(gaddr a, gaddr b) {
  return a == b || std::strcmp(S(a + 16), S(b + 16)) == 0;
}

gaddr FindRttiCast(const RttiObject& object, gaddr source, gaddr from, gaddr to) {
  const auto hierarchy = object.Relative(object.locator + 16);
  const auto count = Rd32(hierarchy + 8);
  const auto array = object.Relative(hierarchy + 12);
  const auto descriptor = [&](uint32_t i) { return object.Relative(array + 4 * i); };

  gaddr downcast = 0, crosscast = 0;
  bool source_public = false, ambiguous_downcast = false, ambiguous_crosscast = false;
  for (uint32_t i = 0; i < count; ++i) {
    const auto base = descriptor(i);
    if (!SameType(object.Relative(base), from) || BaseAddress(object, base) != source) continue;
    source_public |= !(Rd32(base + 20) & kNotVisible);
    for (uint32_t j = 0; j < i; ++j) {
      const auto target = descriptor(j);
      if (i - j > Rd32(target + 4) || !SameType(object.Relative(target), to)) continue;
      bool visible = true;
      if (Rd32(target + 20) & kHasHierarchy) {
        const auto target_hierarchy = object.Relative(target + 24);
        const auto target_array = object.Relative(target_hierarchy + 12);
        const auto relative_base = object.Relative(target_array + 4 * (i - j));
        visible = !(Rd32(relative_base + 20) & (kNotVisible | kNonPublicBase));
      } else {
        for (uint32_t k = j + 1; k <= i; ++k)
          visible &= !(Rd32(descriptor(k) + 20) & (kNotVisible | kNonPublicBase));
      }
      if (!visible) continue;
      const auto result = BaseAddress(object, target);
      ambiguous_downcast |= downcast && downcast != result;
      downcast = result;
    }
  }
  if (downcast) return ambiguous_downcast ? 0 : downcast;
  if (!source_public) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto target = descriptor(i);
    if (Rd32(target + 20) & (kNotVisible | kAmbiguous)) continue;
    if (!SameType(object.Relative(target), to)) continue;
    const auto result = BaseAddress(object, target);
    ambiguous_crosscast |= crosscast && crosscast != result;
    crosscast = result;
  }
  return ambiguous_crosscast ? 0 : crosscast;
}

}  // namespace

// ---- memory ---------------------------------------------------------------
// Guest memory is one host mapping, so these are the host's on it.

HLE64("vcruntime140.dll", memcpy, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memmove, kCdecl) {
  std::memmove(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memset, kCdecl) {
  std::memset(GPtr(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2));
  return Arg(c, 0);
}
HLE64("vcruntime140.dll", memcmp, kCdecl) {
  int r = std::memcmp(GPtr(Arg(c, 0)), GPtr(Arg(c, 1)), Arg(c, 2));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}
HLE64("vcruntime140.dll", memchr, kCdecl) {
  const uint8_t* p = GPtr(Arg(c, 0));
  const void* r = std::memchr(p, int(uint8_t(Arg(c, 1))), Arg(c, 2));
  return r ? Arg(c, 0) + gaddr(static_cast<const uint8_t*>(r) - p) : 0;
}

// ---- strings ----------------------------------------------------------------

HLE64("vcruntime140.dll", strchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strrchr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strrchr(p, int(char(Arg(c, 1))));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}
HLE64("vcruntime140.dll", strstr, kCdecl) {
  const char* p = S(Arg(c, 0));
  const char* r = std::strstr(p, S(Arg(c, 1)));
  return r ? Arg(c, 0) + gaddr(r - p) : 0;
}

HLE64("vcruntime140.dll", wcsrchr, kCdecl) {
  const auto address = Arg(c, 0);
  const auto text = GWStr(address);
  const auto ch = char16_t(Arg(c, 1));
  const auto index = ch ? text.rfind(ch) : text.size();
  return index == std::u16string::npos ? 0 : address + index * sizeof(char16_t);
}

HLE64("vcruntime140.dll", wcsstr, kCdecl) {
  const auto address = Arg(c, 0);
  const auto index = GWStr(address).find(GWStr(Arg(c, 1)));
  return index == std::u16string::npos ? 0 : address + index * sizeof(char16_t);
}

HLE64("vcruntime140.dll", __intrinsic_setjmp, kCdecl) {
  JumpBuffer jump{Arg(c, 1), c.rbx, c.rsp + 8, c.rbp, c.rsi, c.rdi, c.r12,
                  c.r13, c.r14, c.r15, Rd64(c.rsp), c.mxcsr, uint16_t(c.fpu.cw), 0, {}};
  std::memcpy(jump.xmm, c.xmm + 6, sizeof jump.xmm);
  std::memcpy(GPtr(Arg(c, 0)), &jump, sizeof jump);
  return 0;
}

HLE64("vcruntime140.dll", longjmp, kCdecl) {
  const auto buffer = Arg(c, 0);
  const auto value = uint32_t(Arg(c, 1));
  JumpBuffer jump;
  std::memcpy(&jump, GPtr(buffer), sizeof jump);
  const auto from = Rd64(c.rsp);
  const auto* thread = CurrentThread();
  if (!jump.rip || jump.rsp <= c.rsp || jump.rsp >= thread->stack_hi ||
      (jump.frame && (jump.frame < thread->stack_lo || jump.frame >= thread->stack_hi)))
    FatalF("longjmp: the saved Win64 frame is outside the active guest stack");
  if (jump.frame) seh64::UnwindForLongjmp(c, jump.frame, buffer);
  c.rbx = jump.rbx;
  c.rsp = jump.rsp;
  c.rbp = jump.rbp;
  c.rsi = jump.rsi;
  c.rdi = jump.rdi;
  c.r12 = jump.r12;
  c.r13 = jump.r13;
  c.r14 = jump.r14;
  c.r15 = jump.r15;
  c.mxcsr = jump.mxcsr;
  c.fpu.cw = jump.fpcsr;
  std::memcpy(c.xmm + 6, jump.xmm, sizeof jump.xmm);
  c.rax = value ? value : 1;
  throw GuestResume{jump.rip, jump.rsp, from};
}

// ---- type_info ----------------------------------------------------------------
// __std_type_info_data {const char* _UndecoratedName; char _DecoratedName[];}
// (after type_info's vtable pointer). Types are equal when their decorated
// names are, past the leading '.'.

HLE64("vcruntime140.dll", __RTDynamicCast, kCdecl) {
  const auto vfptr = Arg(c, 0);
  const auto from = Arg(c, 2), to = Arg(c, 3);
  if (!vfptr) return 0;
  const auto object = DescribeObject(vfptr);
  if (object.locator && from && to) {
    if (const auto result = FindRttiCast(object, vfptr - int32_t(Arg(c, 1)), from, to))
      return result;
  }
  if (int32_t(Arg(c, 4))) FatalF("__RTDynamicCast: invalid Win64 reference cast");
  return 0;
}

HLE64("vcruntime140.dll", __RTtypeid, kCdecl) {
  const auto object = DescribeObject(Arg(c, 0));
  if (!object.locator) FatalF("__RTtypeid: no Win64 runtime type for object");
  return object.Relative(object.locator + 12);
}

HLE64("vcruntime140.dll", __std_type_info_compare, kCdecl) {
  gaddr a = Arg(c, 0), b = Arg(c, 1);
  if (a == b) return 0;
  int r = std::strcmp(S(a + 8 + 1), S(b + 8 + 1));
  return uint32_t(r < 0 ? -1 : r > 0 ? 1 : 0);
}

// The list of undecorated names type_info::name() allocated: never built
// (name() is not provided), so nothing to free.
HLE64("vcruntime140.dll", __std_type_info_destroy_list, kCdecl) { return 0; }

// ---- std::exception -------------------------------------------------------------
// __std_exception_data {const char* _What; bool _DoFree;}: _DoFree means
// _What was allocated (copied) and is owned.

HLE64("vcruntime140.dll", __std_exception_copy, kCdecl) {
  gaddr from = Arg(c, 0), to = Arg(c, 1);
  gaddr what = Rd64(from);
  if (Rd8(from + 8) && what) {
    std::string s = S(what);
    gaddr copy = HeapAllocProcess(s.size() + 1, false);
    PutStr(copy, s);
    Wr64(to, copy);
    Wr8(to + 8, 1);
  } else {
    Wr64(to, what);
    Wr8(to + 8, 0);
  }
  return 0;
}

HLE64("vcruntime140.dll", __std_exception_destroy, kCdecl) {
  gaddr data = Arg(c, 0);
  if (Rd8(data + 8)) HeapFreeProcess(Rd64(data));
  Wr64(data, 0);
  Wr8(data + 8, 0);
  return 0;
}

HLE64("vcruntime140.dll", __std_terminate, kCdecl) { FatalF("guest called std::terminate()"); }
HLE64("vcruntime140.dll", _purecall, kCdecl) {
  FatalF("guest called a pure virtual function (_purecall)");
}

}  // namespace recomp::hle
