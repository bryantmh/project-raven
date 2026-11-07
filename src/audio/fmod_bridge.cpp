// FMOD Studio 1.10 for the game. The game calls the C++ API (FMOD::Studio::EventInstance::start and so on, SysV);
// each method here forwards to the matching C API function of a Windows FMOD Studio 1.10 DLL. C++ handles and C
// handles are the same pointers. Callbacks the game registers are called back through SysV trampolines.
//
// The DLL: `fmod_dll` in config.json, else the FMOD Studio API 1.10.20 DLLs embedded in raven.exe (fmodstudio64.dll
// + fmod64.dll, unpacked into the runtime folder; tools/fetch_fmod.py), else fmodstudio64.dll / fmodstudio.dll next
// to raven.exe. Unity builds of FMOD 1.10 ship core and studio in one fmodstudio.dll; the API package splits them
// into fmod64.dll + fmodstudio64.dll (both work).
// RAVEN_MUTE=1 selects FMOD's no-sound output (automated test runs).
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/config.h"
#include "app/paths.h"
#include "linux/lnx.h"
#include "log.h"

using namespace raven;

namespace {
using R = int;  // FMOD_RESULT
constexpr R FMOD_OK = 0, FMOD_ERR_UNINITIALIZED = 64;

HMODULE g_core = nullptr, g_studio = nullptr;
bool g_tried = false;

bool load() {
    if (g_tried) return g_studio != nullptr;
    g_tried = true;
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (!app::config().fmod_dll.empty()) candidates.push_back(app::to_wide(app::config().fmod_dll));
    fs::path exe = app::to_wide(app::paths().exe_dir), runtime = app::to_wide(app::paths().runtime_dir);
    for (auto dir : {runtime / L"runtime" / L"windows", exe}) {
        candidates.push_back(dir / L"fmodstudio64.dll");
        candidates.push_back(dir / L"fmodstudio.dll");
    }
    for (auto& p : candidates) {
        if (!fs::exists(p)) continue;
        // The split Engine package needs fmod64.dll beside fmodstudio64.dll.
        SetDllDirectoryW(p.parent_path().c_str());
        g_studio = LoadLibraryW(p.c_str());
        SetDllDirectoryW(nullptr);
        if (!g_studio) continue;
        g_core = GetProcAddress(g_studio, "FMOD_System_CreateSound") ? g_studio : LoadLibraryW((p.parent_path() / L"fmod64.dll").c_str());
        RLOG("FMOD: loaded %s", app::to_utf8(p.wstring()).c_str());
        return true;
    }
    RLOG("FMOD: no FMOD Studio 1.10 DLL found (set fmod_dll in config.json); the game will run without sound");
    return false;
}

template <typename F>
F fn(const char* name) {
    void* p = nullptr;
    if (load()) {
        p = reinterpret_cast<void*>(GetProcAddress(g_studio, name));
        if (!p && g_core) p = reinterpret_cast<void*>(GetProcAddress(g_core, name));
    }
    if (!p) RLOG("FMOD: missing %s", name);
    return reinterpret_cast<F>(p);
}

// Call FMOD C function `name` with the given argument types; returns FMOD_ERR_UNINITIALIZED without a DLL.
#define CALL(name, sig, ...)                                         \
    ([&]() -> R {                                                    \
        static auto f = fn<R(__stdcall*) sig>(#name);                \
        return f ? f(__VA_ARGS__) : FMOD_ERR_UNINITIALIZED;          \
    }())

// ---- callbacks: FMOD calls with the Microsoft ABI; the game's callbacks are SysV.
using GameChannelCb = SYSV R (*)(void*, int, int, void*, void*);
using GameEventCb = SYSV R (*)(unsigned, void*, void*);
std::mutex g_cb_mutex;
std::unordered_map<void*, GameChannelCb> g_channel_cbs;
std::unordered_map<void*, GameEventCb> g_event_cbs;

R __stdcall channel_cb(void* cc, int type, int cbtype, void* d1, void* d2) {
    GameChannelCb f;
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        auto it = g_channel_cbs.find(cc);
        if (it == g_channel_cbs.end()) return FMOD_OK;
        f = it->second;
    }
    return f(cc, type, cbtype, d1, d2);
}
R __stdcall event_cb(unsigned type, void* ev, void* params) {
    GameEventCb f;
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        auto it = g_event_cbs.find(ev);
        if (it == g_event_cbs.end()) return FMOD_OK;
        f = it->second;
    }
    return f(type, ev, params);
}

// FMOD_OPENMEMORY / OPENMEMORY_POINT / OPENUSER take data, not a path.
bool is_path(unsigned mode) { return !(mode & (0x800u | 0x10000000u | 0x8u)); }
std::string win_path(const char* p) { return app::to_utf8(lnx::map_path(p, false)); }
}  // namespace

#define FMOD_SHIM(ret, mangled, params) \
    extern "C" SYSV ret lx_##mangled params; \
    static ::raven::shim::Reg reg_##mangled(#mangled, reinterpret_cast<void*>(&lx_##mangled)); \
    extern "C" SYSV ret lx_##mangled params

// ---- pause (the settings menu): the core system's master channel group
void* g_core_system = nullptr;
namespace raven::audio {
void set_paused(bool paused) {
    if (!g_core_system) return;
    void* master = nullptr;
    if (CALL(FMOD_System_GetMasterChannelGroup, (void*, void**), g_core_system, &master) == FMOD_OK && master)
        CALL(FMOD_ChannelGroup_SetPaused, (void*, int), master, paused ? 1 : 0);
}
}  // namespace raven::audio

// ---- global
FMOD_SHIM(R, FMOD_Memory_Initialize, (void*, int, void*, void*, void*, unsigned)) { return FMOD_OK; }  // FMOD uses its own heap
FMOD_SHIM(R, FMOD_Debug_Initialize, (unsigned, int, void*, const char*)) { return FMOD_OK; }

// ---- Studio::System
FMOD_SHIM(R, _ZN4FMOD6Studio6System6createEPPS1_j, (void** out, unsigned version)) {
    R r = CALL(FMOD_Studio_System_Create, (void**, unsigned), out, version);
    RLOG("FMOD: Studio::System::create(header %#x) = %d", version, r);
    if (r == FMOD_OK) {
        void* core = nullptr;
        CALL(FMOD_Studio_System_GetLowLevelSystem, (void*, void**), *out, &core);
        g_core_system = core;
        // RAVEN_WAVWRITER=1 (tests): record the mix to fmodoutput.wav in the working folder instead of playing it.
        if (std::getenv("RAVEN_WAVWRITER")) CALL(FMOD_System_SetOutput, (void*, int), core, 3 /*FMOD_OUTPUTTYPE_WAVWRITER*/);
        else if (std::getenv("RAVEN_MUTE")) CALL(FMOD_System_SetOutput, (void*, int), core, 2 /*FMOD_OUTPUTTYPE_NOSOUND*/);
    }
    return r;
}
FMOD_SHIM(R, _ZN4FMOD6Studio6System10initializeEijjPv, (void* s, int maxch, unsigned sflags, unsigned flags, void*)) {
    R r = CALL(FMOD_Studio_System_Initialize, (void*, int, unsigned, unsigned, void*), s, maxch, sflags, flags, nullptr);
    RLOG("FMOD: Studio::System::initialize(%d channels) = %d", maxch, r);
    return r;
}
FMOD_SHIM(R, _ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE, (void* s, const char* file, unsigned flags, void** bank)) {
    std::string p = win_path(file);
    R r = CALL(FMOD_Studio_System_LoadBankFile, (void*, const char*, unsigned, void**), s, p.c_str(), flags, bank);
    RLOG("FMOD: loadBankFile(%s) = %d", file, r);
    return r;
}
FMOD_SHIM(R, _ZN4FMOD6Studio6System13flushCommandsEv, (void* s)) { return CALL(FMOD_Studio_System_FlushCommands, (void*), s); }
FMOD_SHIM(R, _ZN4FMOD6Studio6System21setListenerAttributesEiPK18FMOD_3D_ATTRIBUTES, (void* s, int l, const void* a)) {
    return CALL(FMOD_Studio_System_SetListenerAttributes, (void*, int, const void*), s, l, a);
}
FMOD_SHIM(R, _ZN4FMOD6Studio6System6updateEv, (void* s)) { return CALL(FMOD_Studio_System_Update, (void*), s); }
FMOD_SHIM(R, _ZN4FMOD6Studio6System7releaseEv, (void* s)) { return CALL(FMOD_Studio_System_Release, (void*), s); }
FMOD_SHIM(R, _ZNK4FMOD6Studio6System17getLowLevelSystemEPPNS_6SystemE, (void* s, void** out)) {
    return CALL(FMOD_Studio_System_GetLowLevelSystem, (void*, void**), s, out);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio6System6getBusEPKcPPNS0_3BusE, (void* s, const char* path, void** out)) {
    return CALL(FMOD_Studio_System_GetBus, (void*, const char*, void**), s, path, out);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio6System8getEventEPKcPPNS0_16EventDescriptionE, (void* s, const char* path, void** out)) {
    return CALL(FMOD_Studio_System_GetEvent, (void*, const char*, void**), s, path, out);
}

// ---- Studio::Bank / Bus
FMOD_SHIM(R, _ZN4FMOD6Studio4Bank14loadSampleDataEv, (void* b)) { return CALL(FMOD_Studio_Bank_LoadSampleData, (void*), b); }
FMOD_SHIM(R, _ZN4FMOD6Studio4Bank6unloadEv, (void* b)) { return CALL(FMOD_Studio_Bank_Unload, (void*), b); }
FMOD_SHIM(R, _ZN4FMOD6Studio3Bus13stopAllEventsE21FMOD_STUDIO_STOP_MODE, (void* b, int m)) { return CALL(FMOD_Studio_Bus_StopAllEvents, (void*, int), b, m); }
FMOD_SHIM(R, _ZN4FMOD6Studio3Bus7setMuteEb, (void* b, bool m)) { return CALL(FMOD_Studio_Bus_SetMute, (void*, int), b, m ? 1 : 0); }
FMOD_SHIM(R, _ZN4FMOD6Studio3Bus9setVolumeEf, (void* b, float v)) { return CALL(FMOD_Studio_Bus_SetVolume, (void*, float), b, v); }
FMOD_SHIM(R, _ZNK4FMOD6Studio3Bus15getChannelGroupEPPNS_12ChannelGroupE, (void* b, void** out)) {
    return CALL(FMOD_Studio_Bus_GetChannelGroup, (void*, void**), b, out);
}

// ---- Studio::EventDescription
FMOD_SHIM(R, _ZNK4FMOD6Studio16EventDescription14createInstanceEPPNS0_13EventInstanceE, (void* d, void** out)) {
    return CALL(FMOD_Studio_EventDescription_CreateInstance, (void*, void**), d, out);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio16EventDescription15getInstanceListEPPNS0_13EventInstanceEiPi, (void* d, void** arr, int cap, int* n)) {
    return CALL(FMOD_Studio_EventDescription_GetInstanceList, (void*, void**, int, int*), d, arr, cap, n);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio16EventDescription16getInstanceCountEPi, (void* d, int* n)) {
    return CALL(FMOD_Studio_EventDescription_GetInstanceCount, (void*, int*), d, n);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio16EventDescription18getMaximumDistanceEPf, (void* d, float* v)) {
    return CALL(FMOD_Studio_EventDescription_GetMaximumDistance, (void*, float*), d, v);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio16EventDescription18getMinimumDistanceEPf, (void* d, float* v)) {
    return CALL(FMOD_Studio_EventDescription_GetMinimumDistance, (void*, float*), d, v);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio16EventDescription7getPathEPciPi, (void* d, char* path, int size, int* got)) {
    return CALL(FMOD_Studio_EventDescription_GetPath, (void*, char*, int, int*), d, path, size, got);
}

// ---- Studio::EventInstance
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance11setCallbackEPF11FMOD_RESULTjP25FMOD_STUDIO_EVENTINSTANCEPvEj,
          (void* e, GameEventCb cb, unsigned mask)) {
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        if (cb) g_event_cbs[e] = cb; else g_event_cbs.erase(e);
    }
    return CALL(FMOD_Studio_EventInstance_SetCallback, (void*, void*, unsigned), e, cb ? reinterpret_cast<void*>(&event_cb) : nullptr, mask);
}
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance11setUserDataEPv, (void* e, void* u)) { return CALL(FMOD_Studio_EventInstance_SetUserData, (void*, void*), e, u); }
FMOD_SHIM(R, _ZNK4FMOD6Studio13EventInstance11getUserDataEPPv, (void* e, void** u)) { return CALL(FMOD_Studio_EventInstance_GetUserData, (void*, void**), e, u); }
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance15set3DAttributesEPK18FMOD_3D_ATTRIBUTES, (void* e, const void* a)) {
    return CALL(FMOD_Studio_EventInstance_Set3DAttributes, (void*, const void*), e, a);
}
FMOD_SHIM(R, _ZNK4FMOD6Studio13EventInstance15get3DAttributesEP18FMOD_3D_ATTRIBUTES, (void* e, void* a)) {
    return CALL(FMOD_Studio_EventInstance_Get3DAttributes, (void*, void*), e, a);
}
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance17setParameterValueEPKcf, (void* e, const char* n, float v)) {
    return CALL(FMOD_Studio_EventInstance_SetParameterValue, (void*, const char*, float), e, n, v);
}
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance4stopE21FMOD_STUDIO_STOP_MODE, (void* e, int m)) { return CALL(FMOD_Studio_EventInstance_Stop, (void*, int), e, m); }
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance5startEv, (void* e)) { return CALL(FMOD_Studio_EventInstance_Start, (void*), e); }
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance7releaseEv, (void* e)) {
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        g_event_cbs.erase(e);
    }
    return CALL(FMOD_Studio_EventInstance_Release, (void*), e);
}
FMOD_SHIM(R, _ZN4FMOD6Studio13EventInstance9setVolumeEf, (void* e, float v)) { return CALL(FMOD_Studio_EventInstance_SetVolume, (void*, float), e, v); }
FMOD_SHIM(R, _ZNK4FMOD6Studio13EventInstance14getDescriptionEPPNS0_16EventDescriptionE, (void* e, void** d)) {
    return CALL(FMOD_Studio_EventInstance_GetDescription, (void*, void**), e, d);
}
FMOD_SHIM(bool, _ZNK4FMOD6Studio13EventInstance7isValidEv, (void* e)) {
    static auto f = fn<int(__stdcall*)(void*)>("FMOD_Studio_EventInstance_IsValid");
    return f && f(e);
}

// ---- core System
FMOD_SHIM(R, _ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE, (void* s, const char* name, unsigned mode, void* ex, void** out)) {
    std::string p;
    if (name && is_path(mode)) {
        p = win_path(name);
        name = p.c_str();
    }
    return CALL(FMOD_System_CreateSound, (void*, const char*, unsigned, void*, void**), s, name, mode, ex, out);
}
FMOD_SHIM(R, _ZN4FMOD6System13get3DSettingsEPfS1_S1_, (void* s, float* a, float* b, float* c)) {
    return CALL(FMOD_System_Get3DSettings, (void*, float*, float*, float*), s, a, b, c);
}
FMOD_SHIM(R, _ZN4FMOD6System13set3DSettingsEfff, (void* s, float a, float b, float c)) {
    return CALL(FMOD_System_Set3DSettings, (void*, float, float, float), s, a, b, c);
}
FMOD_SHIM(R, _ZN4FMOD6System13getDriverInfoEiPciP9FMOD_GUIDPiP16FMOD_SPEAKERMODES4_, (void* s, int id, char* name, int len, void* guid, int* rate, int* mode, int* ch)) {
    return CALL(FMOD_System_GetDriverInfo, (void*, int, char*, int, void*, int*, int*, int*), s, id, name, len, guid, rate, mode, ch);
}
FMOD_SHIM(R, _ZN4FMOD6System13getNumDriversEPi, (void* s, int* n)) { return CALL(FMOD_System_GetNumDrivers, (void*, int*), s, n); }
FMOD_SHIM(R, _ZN4FMOD6System17setSoftwareFormatEi16FMOD_SPEAKERMODEi, (void* s, int rate, int mode, int raw)) {
    return CALL(FMOD_System_SetSoftwareFormat, (void*, int, int, int), s, rate, mode, raw);
}
FMOD_SHIM(R, _ZN4FMOD6System9getDriverEPi, (void* s, int* d)) { return CALL(FMOD_System_GetDriver, (void*, int*), s, d); }
FMOD_SHIM(R, _ZN4FMOD6System9setDriverEi, (void* s, int d)) {
    // The cabinet picks a specific sound card; on a PC the default device is the right one.
    return FMOD_OK;
}
FMOD_SHIM(R, _ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE, (void* s, void* snd, void* grp, bool paused, void** ch)) {
    return CALL(FMOD_System_PlaySound, (void*, void*, void*, int, void**), s, snd, grp, paused ? 1 : 0, ch);
}
FMOD_SHIM(R, _ZN4FMOD5Sound7releaseEv, (void* s)) { return CALL(FMOD_Sound_Release, (void*), s); }

// ---- ChannelControl / Channel / DSP (ChannelControl methods are the same for channels and groups)
FMOD_SHIM(R, _ZN4FMOD14ChannelControl11getUserDataEPPv, (void* c, void** u)) { return CALL(FMOD_Channel_GetUserData, (void*, void**), c, u); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl11setUserDataEPv, (void* c, void* u)) { return CALL(FMOD_Channel_SetUserData, (void*, void*), c, u); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E,
          (void* c, GameChannelCb cb)) {
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        if (cb) g_channel_cbs[c] = cb; else g_channel_cbs.erase(c);
    }
    return CALL(FMOD_Channel_SetCallback, (void*, void*), c, cb ? reinterpret_cast<void*>(&channel_cb) : nullptr);
}
FMOD_SHIM(R, _ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_, (void* c, const void* p, const void* v, const void* alt)) {
    return CALL(FMOD_Channel_Set3DAttributes, (void*, const void*, const void*, const void*), c, p, v, alt);
}
FMOD_SHIM(R, _ZN4FMOD14ChannelControl4stopEv, (void* c)) { return CALL(FMOD_Channel_Stop, (void*), c); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl6getDSPEiPPNS_3DSPE, (void* c, int i, void** d)) { return CALL(FMOD_Channel_GetDSP, (void*, int, void**), c, i, d); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl6setPanEf, (void* c, float p)) { return CALL(FMOD_Channel_SetPan, (void*, float), c, p); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl7setModeEj, (void* c, unsigned m)) { return CALL(FMOD_Channel_SetMode, (void*, unsigned), c, m); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl9setPausedEb, (void* c, bool p)) { return CALL(FMOD_Channel_SetPaused, (void*, int), c, p ? 1 : 0); }
FMOD_SHIM(R, _ZN4FMOD14ChannelControl9setVolumeEf, (void* c, float v)) { return CALL(FMOD_Channel_SetVolume, (void*, float), c, v); }
FMOD_SHIM(R, _ZN4FMOD7Channel11setPositionEjj, (void* c, unsigned pos, unsigned unit)) { return CALL(FMOD_Channel_SetPosition, (void*, unsigned, unsigned), c, pos, unit); }
FMOD_SHIM(R, _ZN4FMOD7Channel12setLoopCountEi, (void* c, int n)) { return CALL(FMOD_Channel_SetLoopCount, (void*, int), c, n); }
FMOD_SHIM(R, _ZN4FMOD3DSP15getMeteringInfoEP22FMOD_DSP_METERING_INFOS2_, (void* d, void* in, void* out)) {
    return CALL(FMOD_DSP_GetMeteringInfo, (void*, void*, void*), d, in, out);
}
FMOD_SHIM(R, _ZN4FMOD3DSP18setMeteringEnabledEbb, (void* d, bool in, bool out)) {
    return CALL(FMOD_DSP_SetMeteringEnabled, (void*, int, int), d, in ? 1 : 0, out ? 1 : 0);
}
