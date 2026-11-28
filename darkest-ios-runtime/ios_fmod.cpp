#include <fmod.h>
#include <fmod_studio.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>

#include "win32/hle.h"
#include "win32/kernel32_state.h"

extern "C" uint32_t darkest_fmod_build_version() { return FMOD_VERSION; }

namespace recomp::hle {
namespace {

static_assert(FMOD_VERSION == 0x00020314, "Darkest.exe imports FMOD 2.03.14");
constexpr char kStudio[] = "fmodstudio.dll";
constexpr char kCore[] = "fmod.dll";

template<typename T>
inline constexpr bool kOpaqueAudioHandle =
    std::is_same_v<T, FMOD_SYSTEM> || std::is_same_v<T, FMOD_CHANNELGROUP> ||
    std::is_same_v<T, FMOD_STUDIO_SYSTEM> || std::is_same_v<T, FMOD_STUDIO_BANK> ||
    std::is_same_v<T, FMOD_STUDIO_EVENTDESCRIPTION> ||
    std::is_same_v<T, FMOD_STUDIO_EVENTINSTANCE> || std::is_same_v<T, FMOD_STUDIO_VCA>;

template<typename T>
T AudioArg(const Ctx& c, int index) {
    if constexpr (std::is_same_v<T, float>) {
        return ArgF(c, index);
    } else if constexpr (std::is_pointer_v<T>) {
        const uint64_t value = Arg(c, index);
        using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
        // FMOD's opaque instances are scalar handles. Ordinary pointer
        // arguments still refer to the game's guest memory.
        if constexpr (kOpaqueAudioHandle<Pointee> || std::is_same_v<Pointee, void>) {
            return reinterpret_cast<T>(value);
        } else {
            return value ? reinterpret_cast<T>(GPtr(value)) : nullptr;
        }
    } else {
        return static_cast<T>(Arg(c, index));
    }
}

template<auto Target>
struct AudioCall;

template<typename... Args, FMOD_RESULT (*Target)(Args...)>
struct AudioCall<Target> {
    template<size_t... Index>
    static uint64_t Invoke(Ctx& c, std::index_sequence<Index...>) {
        return Target(AudioArg<Args>(c, Index)...);
    }

    static uint64_t Run(Ctx& c) {
        return Invoke(c, std::index_sequence_for<Args...>{});
    }
};

#define FMOD_BIND(id, dll, symbol, call) \
    const Registrar bind_##id{dll, symbol, kCdecl, AudioCall<::call>::Run, true}

FMOD_BIND(master, kCore,
    "?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z",
    FMOD_System_GetMasterChannelGroup);
FMOD_BIND(volume, kCore, "?setVolume@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z",
    FMOD_ChannelGroup_SetVolume);
FMOD_BIND(start, kStudio, "?start@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_EventInstance_Start);
FMOD_BIND(stop, kStudio, "?stop@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_STUDIO_STOP_MODE@@@Z",
    FMOD_Studio_EventInstance_Stop);
FMOD_BIND(playback, kStudio,
    "?getPlaybackState@EventInstance@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAW4FMOD_STUDIO_PLAYBACK_STATE@@@Z",
    FMOD_Studio_EventInstance_GetPlaybackState);
FMOD_BIND(release_event, kStudio, "?release@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_EventInstance_Release);
FMOD_BIND(get_user, kStudio, "?getUserData@EventInstance@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAPEAX@Z",
    FMOD_Studio_EventInstance_GetUserData);
FMOD_BIND(set_user, kStudio, "?setUserData@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEAX@Z",
    FMOD_Studio_EventInstance_SetUserData);
FMOD_BIND(parse, kStudio, "FMOD_Studio_ParseID", FMOD_Studio_ParseID);
FMOD_BIND(list_events, kStudio, "?getEventList@Bank@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAPEAVEventDescription@23@HPEAH@Z",
    FMOD_Studio_Bank_GetEventList);
FMOD_BIND(count_events, kStudio, "?getEventCount@Bank@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAH@Z",
    FMOD_Studio_Bank_GetEventCount);
FMOD_BIND(bank_sample_state, kStudio, "?getSampleLoadingState@Bank@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAW4FMOD_STUDIO_LOADING_STATE@@@Z",
    FMOD_Studio_Bank_GetSampleLoadingState);
FMOD_BIND(bank_state, kStudio, "?getLoadingState@Bank@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAW4FMOD_STUDIO_LOADING_STATE@@@Z",
    FMOD_Studio_Bank_GetLoadingState);
FMOD_BIND(unload_bank, kStudio, "?unload@Bank@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_Bank_Unload);
FMOD_BIND(unload_samples, kStudio, "?unloadSampleData@Bank@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_Bank_UnloadSampleData);
FMOD_BIND(event_by_id, kStudio, "?getEventByID@System@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEBUFMOD_GUID@@PEAPEAVEventDescription@23@@Z",
    FMOD_Studio_System_GetEventByID);
FMOD_BIND(get_vca, kStudio, "?getVCA@System@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEBDPEAPEAVVCA@23@@Z",
    FMOD_Studio_System_GetVCA);
FMOD_BIND(core, kStudio, "?getCoreSystem@System@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAPEAV13@@Z",
    FMOD_Studio_System_GetCoreSystem);
FMOD_BIND(update, kStudio, "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", FMOD_Studio_System_Update);
FMOD_BIND(release, kStudio, "?release@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ", FMOD_Studio_System_Release);
FMOD_BIND(vca_volume, kStudio, "?setVolume@VCA@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", FMOD_Studio_VCA_SetVolume);
FMOD_BIND(release_instances, kStudio, "?releaseAllInstances@EventDescription@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_EventDescription_ReleaseAllInstances);
FMOD_BIND(sample_state, kStudio, "?getSampleLoadingState@EventDescription@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAW4FMOD_STUDIO_LOADING_STATE@@@Z",
    FMOD_Studio_EventDescription_GetSampleLoadingState);
FMOD_BIND(list_instances, kStudio, "?getInstanceList@EventDescription@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAPEAVEventInstance@23@HPEAH@Z",
    FMOD_Studio_EventDescription_GetInstanceList);
FMOD_BIND(count_instances, kStudio, "?getInstanceCount@EventDescription@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAH@Z",
    FMOD_Studio_EventDescription_GetInstanceCount);
FMOD_BIND(new_instance, kStudio, "?createInstance@EventDescription@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAPEAVEventInstance@23@@Z",
    FMOD_Studio_EventDescription_CreateInstance);
FMOD_BIND(event_id, kStudio, "?getID@EventDescription@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAUFMOD_GUID@@@Z",
    FMOD_Studio_EventDescription_GetID);
FMOD_BIND(flush, kStudio, "?flushCommands@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_System_FlushCommands);
FMOD_BIND(load_samples, kStudio, "?loadSampleData@Bank@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
    FMOD_Studio_Bank_LoadSampleData);
FMOD_BIND(describe, kStudio, "?getDescription@EventInstance@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEAPEAVEventDescription@23@@Z",
    FMOD_Studio_EventInstance_GetDescription);
FMOD_BIND(lookup, kStudio, "?lookupID@System@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEBDPEAUFMOD_GUID@@@Z",
    FMOD_Studio_System_LookupID);

#undef FMOD_BIND

HLE_NAMED_X(kCore, "?setMute@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z", Mute, kCdecl, true) {
    return FMOD_ChannelGroup_SetMute(AudioArg<FMOD_CHANNELGROUP*>(c, 0), uint8_t(Arg(c, 1)) != 0);
}

HLE_NAMED_X(kStudio, "?setParameterByName@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDM_N@Z",
            Parameter, kCdecl, true) {
    return FMOD_Studio_EventInstance_SetParameterByName(
        AudioArg<FMOD_STUDIO_EVENTINSTANCE*>(c, 0), AudioArg<const char*>(c, 1),
        ArgF(c, 2), uint8_t(Arg(c, 3)) != 0);
}

HLE_NAMED_X(kStudio, "?loadBankFile@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVBank@23@@Z",
            LoadBankFile, kCdecl, true) {
    if (!Arg(c, 1)) return FMOD_ERR_INVALID_PARAM;
    const auto path = HostPath(GStr(Arg(c, 1)));
    return FMOD_Studio_System_LoadBankFile(
        AudioArg<FMOD_STUDIO_SYSTEM*>(c, 0), path.c_str(),
        AudioArg<FMOD_STUDIO_LOAD_BANK_FLAGS>(c, 2), AudioArg<FMOD_STUDIO_BANK**>(c, 3));
}

HLE_NAMED_X(kStudio, "?create@System@Studio@FMOD@@SA?AW4FMOD_RESULT@@PEAPEAV123@I@Z",
            StudioCreate, kCdecl, true) {
    const auto result = FMOD_Studio_System_Create(AudioArg<FMOD_STUDIO_SYSTEM**>(c, 0),
                                                AudioArg<unsigned>(c, 1));
    std::fprintf(stderr, "Darkest Dungeon: FMOD Studio System_Create = %d.\n", int(result));
    return result;
}

HLE_NAMED_X(kStudio, "?initialize@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@HIIPEAX@Z",
            StudioInitialize, kCdecl, true) {
    if (Arg(c, 4)) return FMOD_ERR_INVALID_PARAM;
    // Studio callbacks must execute through the Win64 bridge on the calling
    // game thread. FMOD's native audio mixer still runs independently.
    const auto flags = AudioArg<FMOD_STUDIO_INITFLAGS>(c, 2) | FMOD_STUDIO_INIT_SYNCHRONOUS_UPDATE;
    const auto result = FMOD_Studio_System_Initialize(AudioArg<FMOD_STUDIO_SYSTEM*>(c, 0),
        AudioArg<int>(c, 1), flags, AudioArg<FMOD_INITFLAGS>(c, 3), nullptr);
    std::fprintf(stderr, "Darkest Dungeon: FMOD Studio System_Initialize = %d.\n", int(result));
    return result;
}

struct Callback {
    gaddr code;
    FMOD_STUDIO_EVENT_CALLBACK_TYPE mask;
};
std::mutex callback_mutex;
std::map<FMOD_STUDIO_EVENTINSTANCE*, Callback> callbacks;

FMOD_RESULT F_CALL EventCallback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type,
                                     FMOD_STUDIO_EVENTINSTANCE* event, void* parameters) {
    Callback callback{};
    {
        std::lock_guard lock(callback_mutex);
        auto entry = callbacks.find(event);
        if (entry == callbacks.end()) return FMOD_OK;
        callback = entry->second;
        if (type == FMOD_STUDIO_EVENT_CALLBACK_DESTROYED) callbacks.erase(entry);
    }
    if (!(callback.mask & type)) return FMOD_OK;
    auto* thread = CurrentThread();
    if (!thread) {
        std::fputs("Darkest Dungeon: FMOD delivered a game callback without a game thread.\n", stderr);
        return FMOD_ERR_INTERNAL;
    }
    struct Frame {
        Ctx& context;
        Ctx saved;
        gaddr data = 0;
        ~Frame() {
            context = saved;
            if (data) HeapFreeProcess(data);
        }
    };
    Frame frame{thread->ctx, thread->ctx};
    if (type == FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_MARKER && parameters) {
        const auto& marker = *static_cast<FMOD_STUDIO_TIMELINE_MARKER_PROPERTIES*>(parameters);
        const size_t name_size = marker.name ? std::strlen(marker.name) + 1 : 0;
        frame.data = HeapAllocProcess(16 + name_size, true);
        if (name_size) {
            Wr64(frame.data, frame.data + 16);
            std::memcpy(GPtr(frame.data + 16), marker.name, name_size);
        }
        Wr32(frame.data + 8, uint32_t(marker.position));
    }
    return static_cast<FMOD_RESULT>(CallGuest(frame.context, callback.code,
        {type, reinterpret_cast<uintptr_t>(event), frame.data}));
}

HLE_NAMED_X(kStudio, "?setCallback@EventInstance@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW44@IPEAUFMOD_STUDIO_EVENTINSTANCE@@PEAX@ZI@Z",
            SetCallback, kCdecl, true) {
    constexpr auto supported = FMOD_STUDIO_EVENT_CALLBACK_DESTROYED |
                               FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_MARKER;
    auto* event = AudioArg<FMOD_STUDIO_EVENTINSTANCE*>(c, 0);
    const auto code = Arg(c, 1);
    const auto mask = AudioArg<FMOD_STUDIO_EVENT_CALLBACK_TYPE>(c, 2);
    if (code && (mask & ~supported)) return FMOD_ERR_UNSUPPORTED;
    const auto result = FMOD_Studio_EventInstance_SetCallback(
        event, code ? EventCallback : nullptr,
        code ? mask | FMOD_STUDIO_EVENT_CALLBACK_DESTROYED : 0);
    if (result == FMOD_OK) {
        std::lock_guard lock(callback_mutex);
        if (code) callbacks[event] = {code, mask};
        else callbacks.erase(event);
    }
    return result;
}

}  // namespace
}  // namespace recomp::hle