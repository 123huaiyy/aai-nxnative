#include "x86/hle.h"
#include "win32/hle.h"
#include "win32/ucrt.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string_view>

namespace {

uint64_t SetSeTranslator(recomp::Ctx& context) {
    // Each guest thread runs on its own host thread. Keep the guest callback
    // address here without treating it as a callable host pointer.
    static thread_local recomp::gaddr translator = 0;
    const auto previous = translator;
    translator = recomp::hle::Arg(context, 0);
    return previous;
}

uint64_t RandS(recomp::Ctx& context) {
    const auto output = recomp::hle::Arg(context, 0);
    const auto& memory = recomp::g_proc->mem;
    constexpr uint64_t size = sizeof(uint32_t);
    constexpr auto writeBit = static_cast<uint8_t>(recomp::Prot::W);
    if (!output || output >= memory.size() || size > memory.size() - output ||
        !(static_cast<uint8_t>(memory.QueryProt(output)) & writeBit) ||
        !(static_cast<uint8_t>(memory.QueryProt(output + size - 1)) & writeBit)) {
        recomp::Wr32(recomp::hle::GuestErrno(), EINVAL);
        return EINVAL;
    }
    uint32_t number;
    arc4random_buf(&number, sizeof number);
    recomp::Wr32(output, number);
    return 0;
}

std::u16string_view SystemMessage(uint32_t id) {
    if ((id & 0xffff0000u) == 0x80070000u) id &= 0xffffu;
    switch (id) {
        case 2: return u"The system cannot find the file specified.\r\n";
        case 3: return u"The system cannot find the path specified.\r\n";
        case 5: return u"Access is denied.\r\n";
        case 87: return u"The parameter is incorrect.\r\n";
        case 123: return u"The filename, directory name, or volume label syntax is incorrect.\r\n";
        case 183: return u"Cannot create a file when that file already exists.\r\n";
        case 267: return u"The directory name is invalid.\r\n";
        default: return {};
    }
}

// The Windows storage path uses FROM_SYSTEM with a caller-supplied UTF-16
// buffer. Its error reporting must also handle unknown IDs without aborting.
uint64_t FormatMessageW(recomp::Ctx& context) {
    using recomp::hle::Arg;
    using recomp::hle::SetLastError;
    constexpr uint32_t kFromSystem = 0x1000, kIgnoreInserts = 0x200;
    const auto flags = uint32_t(Arg(context, 0));
    const auto buffer = Arg(context, 4);
    const auto capacity = uint32_t(Arg(context, 5));
    if (!(flags & kFromSystem) || (flags & ~(kFromSystem | kIgnoreInserts)) || !buffer) {
        SetLastError(87);
        return 0;
    }
    const auto message = SystemMessage(uint32_t(Arg(context, 2)));
    if (message.empty()) {
        SetLastError(317);  // ERROR_MR_MID_NOT_FOUND
        return 0;
    }
    if (capacity == 0) {
        SetLastError(122);
        return 0;
    }
    const size_t count = std::min(message.size(), size_t(capacity - 1));
    for (size_t i = 0; i < count; ++i) recomp::Wr16(buffer + 2 * i, message[i]);
    recomp::Wr16(buffer + 2 * count, 0);
    if (count < message.size()) {
        SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
        return 0;
    }
    return count;
}

}

void darkest_register_startup_crt() {
    constexpr char library[] = "vcruntime140.dll";
    constexpr char symbol[] = "_set_se_translator";
    // Leave the runtime's implementation in charge if it already provides one.
    if (!recomp::hle::HasHandler(library, symbol)) {
        static const recomp::hle::Registrar registration{
            library, symbol, recomp::hle::kCdecl, SetSeTranslator, true};
    }
    if (!recomp::hle::HasHandler("kernel32.dll", "FormatMessageW")) {
        static const recomp::hle::Registrar registration{
            "kernel32.dll", "FormatMessageW", 7, FormatMessageW, true};
    }
    if (!recomp::hle::HasHandler("ucrtbase.dll", "rand_s")) {
        static const recomp::hle::Registrar registration{
            "ucrtbase.dll", "rand_s", recomp::hle::kCdecl, RandS, true};
    }
}
