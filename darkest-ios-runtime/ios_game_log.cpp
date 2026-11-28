#include "x86/runtime.h"

#include <cstdio>
#include <string_view>

namespace {

bool IsReadable(recomp::gaddr address, uint64_t length) {
    const auto& memory = recomp::g_proc->mem;
    if (!address || !length || address >= memory.size() || length > memory.size() - address)
        return false;
    constexpr uint64_t page = recomp::GuestMemory::kPageSize;
    const auto end = address + length - 1;
    for (auto current = address / page * page; current <= end; current += page) {
        const auto protection = static_cast<uint8_t>(memory.QueryProt(current));
        if (!(protection & static_cast<uint8_t>(recomp::Prot::R))) return false;
    }
    return true;
}

std::string_view ReadLogString(recomp::gaddr object) {
    // MSVC x64 release basic_string<char>: 16 bytes of inline storage or a
    // guest pointer, followed by the 64-bit size and capacity.
    if (!IsReadable(object, 32)) return {};
    const auto length = recomp::Rd64(object + 16);
    const auto capacity = recomp::Rd64(object + 24);
    if (length > capacity || length > 65536) return {};
    const auto data = capacity < 16 ? object : recomp::Rd64(object);
    if (!IsReadable(data, length)) return {};
    return {recomp::g_proc->mem.ptr<const char>(data), static_cast<size_t>(length)};
}

}

void darkest_forward_game_log(recomp::Ctx& context) {
    const auto level = static_cast<int32_t>(context.rdx);
    if (!IsReadable(context.rcx, sizeof(int32_t)) ||
        level < static_cast<int32_t>(recomp::Rd32(context.rcx))) return;
    const auto message = ReadLogString(context.r8);
    const auto category = ReadLogString(context.r9);
    const char* severity = level == 0 ? "INFO" : level == 1 ? "WARNING" : level == 2 ? "ERROR" : "UNKNOWN";
    std::fprintf(stderr, "[game][%s][%.*s] %.*s\n", severity,
                 static_cast<int>(category.size()), category.empty() ? "" : category.data(),
                 static_cast<int>(message.size()), message.empty() ? "" : message.data());
}
