#include "lib.hpp"
#include "slumber_app.hpp"
#include "nn/fs.hpp"
#include "nn/os.hpp"
#include "lib/nx/arm/cache.h"
#include "lib/nx/kernel/svc.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>

namespace nn::fs {
    Result DeleteFile(char const* path);
}

/* SlumberToggle: runtime on/off switches for Slumber Island style features in ACNH 3.0.3.
   Loads as subsdk8 so it can sit next to Aden Modmenu (subsdk9).

   Switches live in sd:/slumbertoggle/toggles.txt ("name=1" / "name=0"), re-read every second.
   Each switch is a group of instruction words in main; turning it on writes the patched words,
   turning it off restores the originals. */

namespace sl {

    struct PatchWord {
        u32 offset;   /* from the start of main */
        u32 orig;     /* original instruction (0 = padding gap used as a code cave) */
        u32 patched;
    };

    struct Feature {
        const char*      key;
        const char*      label;
        const PatchWord* words;
        size_t           count;
        bool             wanted;   /* from toggles.txt */
        bool             applied;  /* current state in memory */
    };

#include "patches.inc"

    constexpr size_t FeatureCount = sizeof(g_Features) / sizeof(g_Features[0]);

    constexpr const char* MountName   = "slsd";
    constexpr const char* Dir         = "slsd:/slumbertoggle";
    constexpr const char* LogPath     = "slsd:/slumbertoggle/log.txt";
    constexpr const char* TogglesPath = "slsd:/slumbertoggle/toggles.txt";

    static bool s_SdReady = false;
    static s64  s_LogPos  = 0;

    /* Once the watch thread runs, log lines are queued and written from there: an SD
       open/write/flush/close on the game thread stalled it for about a second per line. */
    static bool s_LogQueued = false;
    static std::atomic_flag s_LogLock = ATOMIC_FLAG_INIT;
    static char s_LogQueue[0x4000];
    static size_t s_LogQueueLen = 0;

    static void WriteLogFile(const char* buf, size_t len) {
        nn::fs::FileHandle h;
        if (R_FAILED(nn::fs::OpenFile(&h, LogPath, nn::fs::OpenMode_Write | nn::fs::OpenMode_Append))) return;
        nn::fs::WriteFile(h, s_LogPos, buf, len, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
        s_LogPos += len;
        nn::fs::CloseFile(h);
    }

    /* Watch thread: write out what the game thread queued. */
    static void FlushLog() {
        static char out[sizeof(s_LogQueue)];
        while (s_LogLock.test_and_set(std::memory_order_acquire)) {}
        size_t len = s_LogQueueLen;
        memcpy(out, s_LogQueue, len);
        s_LogQueueLen = 0;
        s_LogLock.clear(std::memory_order_release);
        if (len) WriteLogFile(out, len);
    }

    void Log(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
        va_end(args);
        if (len < 0) return;
        if (len > (int)sizeof(buf) - 2) len = sizeof(buf) - 2;
        buf[len++] = '\n';

        Logging.Log(std::string_view(buf, len));
        if (!s_SdReady) return;
        if (!s_LogQueued) { WriteLogFile(buf, len); return; }

        while (s_LogLock.test_and_set(std::memory_order_acquire)) {}
        if (s_LogQueueLen + len <= sizeof(s_LogQueue)) {
            memcpy(s_LogQueue + s_LogQueueLen, buf, len);
            s_LogQueueLen += len;
        }
        s_LogLock.clear(std::memory_order_release);
    }

    /* ---- code patching ---- */

    class Mem : public exl::patch::PatcherImpl {
        public:
        u32* Rw(u32 off) { return reinterpret_cast<u32*>(RwFromAddr(off)); }
        u32* Ro(u32 off) { return reinterpret_cast<u32*>(RoFromAddr(off)); }
    };

    static void WriteWord(Mem& m, u32 off, u32 value) {
        *m.Rw(off) = value;
        armDCacheFlush(m.Rw(off), 4);
        armICacheInvalidate(m.Ro(off), 4);
    }

    /* Returns 1 if every word is patched, 0 if every word is original, -1 if mixed/unknown.
       Hook switches (no words) are just a flag, so their state is what we last set. */
    static int ReadState(Mem& m, const Feature& f) {
        if (f.count == 0) return f.applied ? 1 : 0;
        size_t on = 0, off = 0;
        for (size_t i = 0; i < f.count; i++) {
            u32 cur = *m.Ro(f.words[i].offset);
            if (cur == f.words[i].patched) on++;
            else if (cur == f.words[i].orig) off++;
        }
        if (on == f.count) return 1;
        if (off == f.count) return 0;
        return -1;
    }

    /* ---- Slumber terraforming: a copy of the per-map attribute table for terraforming code ---- */

    constexpr u32 AttrTableGot = 0x4ac39f8;   /* GOT slot holding the u64[256] per-map attribute table */
    constexpr u64 AttrRealIsland = 1ull << 3;
    constexpr u64 AttrSlumber    = 1ull << 32;
    alignas(0x10) static u64 s_TerraformAttr[256];

    /* Copy the live table; every real-island map looks exactly like a Slumber Island map
       (Slumber bit on, real-island bit off) - some checks test "not a real island" instead. */
    static void RefreshTerraformTable() {
        const u64* real = *reinterpret_cast<u64* const*>(exl::util::modules::GetTargetOffset(AttrTableGot));
        if (!real) return;
        for (int i = 0; i < 256; i++) {
            u64 v = real[i];
            if (v & AttrRealIsland) v = (v | AttrSlumber) & ~AttrRealIsland;
            s_TerraformAttr[i] = v;
        }
    }

    /* The {adrp, ldr [#0x9f8]} pairs become {adrp <copy page>, add <copy offset>} - computed here
       because they depend on where this module and main are loaded. */
    static void PrepareTerraformWords() {
        for (size_t i = 0; i < FeatureCount; i++) {
            if (strcmp(g_Features[i].key, "slumber_terraform") != 0) continue;
            PatchWord* w = const_cast<PatchWord*>(g_Features[i].words);
            uintptr_t target = reinterpret_cast<uintptr_t>(s_TerraformAttr);
            for (size_t k = 0; k + 1 < g_Features[i].count; k += 2) {
                u32 rd = w[k].orig & 31;
                uintptr_t pc = exl::util::modules::GetTargetOffset(w[k].offset);
                s64 pages = (s64)((target & ~0xfffull) - (pc & ~0xfffull)) >> 12;
                w[k].patched = 0x90000000u | (u32)((pages & 3) << 29) | (u32)(((pages >> 2) & 0x7ffff) << 5) | rd;
                w[k + 1].patched = 0x91000000u | (u32)((target & 0xfff) << 10) | (rd << 5) | rd;
            }
        }
    }

    /* ---- Slumber weather on the main island ----
       The Slumber time screen (D-pad left) stores the picked weather in the environment manager and
       copies one of two ready-made weather records (sunny "Fine07" / rain "Rain08") into the active
       weather. Those records are only built when you arrive on a Slumber Island, so here the copy
       is empty and the island keeps its own weather. And every map load on a non-Slumber map puts
       the island's own weather back. With the switch on, the records are built the first time a
       weather is picked here, and map loads take the game's own Slumber path that keeps the pick. */

    constexpr u32 StageIdGot     = 0x4ab6298;  /* GOT slot: pointer to the current map id (s16) */
    constexpr u32 EnvGlobal      = 0x4ca7238;  /* environment manager pointer */
    constexpr u32 EnvRecords     = 0x65838;    /* two weather records: [0] sunny, [1] cloudy (rain/snow) */
    constexpr u32 WeatherRecSize = 0x14c;
    constexpr u32 BuildSunnyRec  = 0x27e2220;  /* the builders the Slumber arrival code uses */
    constexpr u32 BuildRainRec   = 0x27e24f0;
    constexpr u32 KeepWeather    = 0x987e14;   /* map load: "tbz w8, #0" skips the Slumber weather copy */
    constexpr u32 KeepWeatherOrig = 0x360002e8;
    constexpr u32 Nop            = 0xd503201f;
    constexpr u32 KeepItemsCall  = 0x1288bf4;  /* load-time field item check: "bl 0x11e5af4" removes a misplaced item */
    constexpr u32 KeepItemsOrig  = 0x97fd73c0;

    static bool s_WeatherPicked = false;

    static bool CurrentMapIsSlumber() {
        const s16* stage = *reinterpret_cast<s16* const*>(exl::util::modules::GetTargetOffset(StageIdGot));
        const u64* attr  = *reinterpret_cast<u64* const*>(exl::util::modules::GetTargetOffset(AttrTableGot));
        if (!stage || !attr || *stage < 0 || *stage > 0xff) return false;
        return (attr[*stage] & AttrSlumber) != 0;
    }

    static bool FeatureOn(const char* key) {
        for (size_t i = 0; i < FeatureCount; i++)
            if (strcmp(g_Features[i].key, key) == 0) return g_Features[i].applied;
        return false;
    }

    static void SetKeepWeather(bool keep) {
        Mem m;
        u32 cur = *m.Ro(KeepWeather);
        u32 want = keep ? Nop : KeepWeatherOrig;
        if (cur == want) return;
        if (cur != KeepWeatherOrig && cur != Nop) {
            Log("slumber_weather: map-load check doesn't match (%08x) - left untouched", cur);
            return;
        }
        WriteWord(m, KeepWeather, want);
    }

    /* Called just before the time screen copies the picked record into the active weather. */
    void BeforeWeatherPick(u32 choice) {
        if (!FeatureOn("slumber_weather") || CurrentMapIsSlumber()) return;
        u8* env = *reinterpret_cast<u8**>(exl::util::modules::GetTargetOffset(EnvGlobal));
        if (!env) return;
        using BuildFn = void (*)(void* rec);
        const u32 builders[2] = { BuildSunnyRec, BuildRainRec };
        for (u32 i = 0; i < 2; i++) {
            u8* rec = env + EnvRecords + i * WeatherRecSize;
            if (*reinterpret_cast<u16*>(rec) != 0) continue;   /* already built (year is set) */
            memset(rec, 0, WeatherRecSize);
            reinterpret_cast<BuildFn>(exl::util::modules::GetTargetOffset(builders[i]))(rec);
        }
        s_WeatherPicked = true;
        SetKeepWeather(true);
        Log("slumber_weather: picked %s on this island", choice == 0 ? "sunny" : "cloudy/rain");
    }

    /* ---- Slumber HUD + build modes ----
       On Slumber Island the stage lists the resident UI scene CloudIslandModeChange: the top-right
       fence / helper / bag icons with "+". + cycles the player's build mode (pocket / fence / path),
       which changes what the D-pad storage button opens. The romfs adds the scene to the main
       island's stage (MainField.byml); with the switch off its creation is skipped there. On
       other maps + (input bit 0x12) opens the Island Designer path list from the player's button
       handler before anything else sees it, so with the switch on that handler cycles the build
       mode instead and clears the press. The pocket request (X) becomes the fence / path picker
       in modes 1 / 2 only on Slumber maps - the switch NOPs that attribute-bit test. */

    constexpr u32 PocketModeCheck     = 0x2df7ea8; /* UI request: pocket -> fence/path picker, bit 32 */
    constexpr u32 PocketModeCheckOrig = 0x36000709;
    constexpr u32 NextBuildMode     = 0x24394e8;  /* bool (player+0x1ee8, player[0x230], 1, 1) */
    constexpr u32 PlusInputBit      = 0x12;       /* player+0xed0 bitset: + pressed */
    constexpr u32 PlayModeGot       = 0x4ac3a80;  /* GOT slot: play mode object, hash at +0x38 */
    constexpr u32 SlumberPlayMode   = 0xdb5d97b8;

    static void SetCheck(u32 off, u32 orig, bool open) {
        Mem m;
        u32 cur = *m.Ro(off);
        u32 want = open ? Nop : orig;
        if (cur == want) return;
        if (cur != orig && cur != Nop) {
            Log("slumber_hud: check at %x doesn't match (%08x) - left untouched", off, cur);
            return;
        }
        WriteWord(m, off, want);
    }

    /* Fence picker: on Slumber maps the picked fence goes into the player's fence slot (players
       entry +0x13c0) and into the hand; elsewhere it becomes a normal pocket item. */
    constexpr u32 FencePickCheck     = 0x24368c8;
    constexpr u32 FencePickCheckOrig = 0x360001a8;
    constexpr u32 PlayerIndex        = 0x2482d8c;  /* (player idx*, 1) -> index into the players array */
    constexpr u32 PlayersGot         = 0x4ab4fb8;  /* GOT slot: players array (0x13f0 per player) */

    static void SetPlusModeCheck(bool open) {
        SetCheck(PocketModeCheck, PocketModeCheckOrig, open);
        SetCheck(FencePickCheck, FencePickCheckOrig, open);
    }

    /* Placing a fence (player state 0xa6, enter 0x238c3b0) works out what the hand holds
       afterwards: on Slumber maps the same fence (unlimited), elsewhere the hand minus one, which
       empties a single picked fence ("IsLastFence"). In fence mode outside Slumber Island the
       attribute-bit test is opened just for that call; pocket fences in pocket mode still get used. */
    constexpr u32 FenceKeepCheck     = 0x238c430;
    constexpr u32 FenceKeepCheckOrig = 0x360000e8;

    /* Build mode of a player (0 pocket, 1 fence, 2 path; -1 unknown). idx = the game's
       player-index byte (player + 0x1ee8, or a copy of it). */
    static s32 BuildMode(u8* idx) {
        u32 i = reinterpret_cast<u32 (*)(u8*, u32)>(exl::util::modules::GetTargetOffset(PlayerIndex))(idx, 1);
        if (i >= 8) return -1;
        u8* players = **reinterpret_cast<u8***>(exl::util::modules::GetTargetOffset(PlayersGot));
        if (!players) return -1;
        u8* entry = players + i * 0x13f0;
        return entry[0x13d4] ? *reinterpret_cast<s32*>(entry + 0x13b8) : -1;
    }

    static bool InFenceModeIdx(u8* idx) { return BuildMode(idx) == 1; }

    static bool InFenceMode(u8* player) { return InFenceModeIdx(player + 0x1ee8); }

    /* Picking a fence back up (player state 0xa8) gives it to the player (0x238d900) unless bit 0
       of byte +0x37 of the pick-up request is set, which it is on Slumber Island: the fence just
       goes away. In fence mode outside Slumber Island that bit is set for the call. */
    bool FencePickupNoGive(u8* idx) {
        return idx && FeatureOn("slumber_hud") && !CurrentMapIsSlumber() && InFenceModeIdx(idx);
    }

    bool FreeFenceBegin(u8* player) {
        if (!player || !FeatureOn("slumber_hud") || CurrentMapIsSlumber() || !InFenceMode(player)) return false;
        SetCheck(FenceKeepCheck, FenceKeepCheckOrig, true);
        return true;
    }

    void FreeFenceEnd() { SetCheck(FenceKeepCheck, FenceKeepCheckOrig, false); }

    /* Picking a path in path mode (picker handler 0x2436850): Slumber's code sets the build mode
       to pocket (0x2436d78 "str wzr, [x27]") and turns EquipLicenseHelmet off (0x243733c
       "mov w2, wzr" before the flag setter). Outside Slumber Island path mode is kept instead:
       the mode store is skipped and the flag is written as 1 - just for that call (live-tested). */
    constexpr u32 PathModeStore     = 0x2436d78;
    constexpr u32 PathModeStoreOrig = 0xb900037f;
    constexpr u32 PathHelmetArg     = 0x243733c;
    constexpr u32 PathHelmetArgOrig = 0x2a1f03e2;   /* mov w2, wzr */
    constexpr u32 PathHelmetArgOn   = 0x52800022;   /* mov w2, #1 */

    static void SetWord(u32 off, u32 orig, u32 patched, bool on) {
        Mem m;
        u32 cur = *m.Ro(off);
        u32 want = on ? patched : orig;
        if (cur == want) return;
        if (cur != orig && cur != patched) {
            Log("slumber_hud: word at %x doesn't match (%08x) - left untouched", off, cur);
            return;
        }
        WriteWord(m, off, want);
    }

    bool PathPickBegin(u8* idx) {
        if (!idx || !FeatureOn("slumber_hud") || CurrentMapIsSlumber() || BuildMode(idx) != 2) return false;
        SetCheck(PathModeStore, PathModeStoreOrig, true);
        SetWord(PathHelmetArg, PathHelmetArgOrig, PathHelmetArgOn, true);
        return true;
    }

    void PathPickEnd() {
        SetCheck(PathModeStore, PathModeStoreOrig, false);
        SetWord(PathHelmetArg, PathHelmetArgOrig, PathHelmetArgOn, false);
    }

    /* Island Designer helmet (player flag "EquipLicenseHelmet", read like the game does in its UI
       request code: 0x264bce0(player idx*, {name holder, name})). Path build mode puts it on too:
       the mode switch sets the flag to "new mode == path" (checked live). So the helmet only means
       the Island Designer app when the build mode is not path; then the designer owns the
       top-right corner and +: the Slumber HUD closes, + opens the designer tool menu and X opens
       the pockets. In path mode Slumber keeps both, + switches on (leaving path mode takes the
       helmet off) and the designer's own "+" box is kept hidden. */
    constexpr u32 PlayerFlagGet  = 0x264bce0;
    constexpr u32 NameHolderGot  = 0x4ab2020;   /* GOT slot: name holder vtable (+0x10) */

    static bool HelmetOn(u8* idx) {
        struct { const void* vt; const char* name; } holder = {
            *reinterpret_cast<u8* const*>(exl::util::modules::GetTargetOffset(NameHolderGot)) + 0x10,
            "EquipLicenseHelmet" };
        return reinterpret_cast<u32 (*)(u8*, void*)>(exl::util::modules::GetTargetOffset(PlayerFlagGet))(idx, &holder) != 0;
    }

    static bool DesignerApp(u8* idx) { return HelmetOn(idx) && BuildMode(idx) != 2; }

    /* Local player's index byte, copied every frame by the button handler (a copy, so a freed
       player object is never read). 0xff = none yet. */
    static u8 s_LocalIdx = 0xff;

    /* Player button handler, before it runs: + switches the build mode outside Slumber Island. */
    void HudPlus(u8* player) {
        if (!player || !FeatureOn("slumber_hud") || CurrentMapIsSlumber()) return;
        s_LocalIdx = player[0x1ee8];
        bool designer = DesignerApp(player + 0x1ee8);
        SetCheck(PocketModeCheck, PocketModeCheckOrig, !designer);
        if (designer) return;
        u32* input = reinterpret_cast<u32*>(player + 0xed0) + (PlusInputBit >> 5);
        u32 bit = 1u << (PlusInputBit & 31);
        if (!(*input & bit)) return;
        using NextFn = bool (*)(u8*, u32, u32, u32);
        auto next = reinterpret_cast<NextFn>(exl::util::modules::GetTargetOffset(NextBuildMode));
        if (next(player + 0x1ee8, *reinterpret_cast<u32*>(player + 0x230), 1, 1)) *input &= ~bit;
    }

    /* The HUD page (state machine at +0x1a8: 1 idle/closed, 2 appear, 3 open, 4 disappear;
       +0x1f0 = requested next state, indexed by byte +0x200; +0x1f8 = player-index byte). Its idle
       state (0x2c51f14) reopens it every frame the HUD should show, so while the designer helmet
       is on outside Slumber Island it is sent to "disappear" and its idle work is skipped; once the
       helmet is off the idle state opens it again by itself (both checked live). */
    constexpr u32 HudStateOpen      = 3;
    constexpr u32 HudStateDisappear = 4;

    static bool DesignerOwnsHud(u8* page) {
        return page && FeatureOn("slumber_hud") && !CurrentMapIsSlumber() && DesignerApp(page + 0x1f8);
    }

    /* MainScreen shows the Island Designer "+" box while EquipLicenseHelmet is set and hides it
       (its animator's own hide) when it isn't: 0x2dfb378 "cbz w0, <hide>" right after the flag
       read. In path mode outside Slumber Island that branch is made unconditional before
       MainScreen's update runs, so the box stays hidden under the Slumber HUD. (v0.28 instead
       played the box's "out" animation every frame, which fought MainScreen's "in" - the box
       popped in and out about once a second.) */
    constexpr u32 DesignerBoxCheck     = 0x2dfb378;
    constexpr u32 DesignerBoxCheckOrig = 0x340015a0;   /* cbz w0, 0x2dfb62c */
    constexpr u32 DesignerBoxHide      = 0x140000ad;   /* b 0x2dfb62c */

    void MainScreenBeforeUpdate() {
        bool hide = false;
        if (s_LocalIdx != 0xff && FeatureOn("slumber_hud") && !CurrentMapIsSlumber()) {
            u8 idx = s_LocalIdx;
            hide = HelmetOn(&idx) && BuildMode(&idx) == 2;
        }
        SetWord(DesignerBoxCheck, DesignerBoxCheckOrig, DesignerBoxHide, hide);
    }

    void HudPageUpdate(u8* page) {
        if (!DesignerOwnsHud(page)) return;
        s32* req = reinterpret_cast<s32*>(page + 0x1f0) + (page[0x200] & 1);
        if (*reinterpret_cast<u32*>(page + 0x1b0) == HudStateOpen && *req == -1) *req = HudStateDisappear;
    }

    bool HudIdleSkip(u8* page) { return DesignerOwnsHud(page); }

    /* The resident HUD is being created: always on Slumber Island, on other maps only when on. */
    bool HudAllowed() {
        if (FeatureOn("slumber_hud") || CurrentMapIsSlumber()) return true;
        const u8* mode = *reinterpret_cast<u8* const*>(exl::util::modules::GetTargetOffset(PlayModeGot));
        return mode && *reinterpret_cast<const u32*>(mode + 0x38) == SlumberPlayMode;
    }

    /* ---- The Slumber phone app ----
       The phone shows apps from an 18-entry table. App 6 (the original Nook Miles) is hidden once
       Nook Miles+ (app 7) is unlocked, which leaves one button free. With the switch on, app 6
       shows as "Slumber": its name label points at our MenuDevice message 0090, its icon at "Type"
       frame 4 (an unused frame whose texture the romfs repaints), and tapping it starts the Island
       Designer flow at our "SlumberApp" entry point, which opens the Slumber settings menu. */

    constexpr int SlumberAppId         = 6;
    constexpr int MilesPlusAppId       = 7;
    constexpr u32 AppRecords           = 0x55c6818;  /* 18 x 0x38: +0 app id, +0x10 name label (char*) */
    constexpr u32 AppRecordSize        = 0x38;
    constexpr u32 AppIconFrames        = 0x408148c;  /* float[18]: "Type" animation frame per app id */
    constexpr float SlumberIconFrame   = 4.0f;
    constexpr float MilesIconFrame     = 11.0f;
    constexpr u32 IslandDesignerHandler = 0x2e03090; /* app 13's launch handler: flow kind 2 */
    constexpr const char* AppNameLabel = "0090";
    constexpr const char* AppEntryPoint = "SlumberApp";

    static const char* s_MilesLabel = nullptr;  /* app 6's own label, restored when switched off */
    bool g_LaunchSlumberApp = false;

    bool PhoneAppOn() { return FeatureOn("slumber_phone_app"); }

    static const char** AppNameSlot(int app_id) {
        u8* recs = reinterpret_cast<u8*>(exl::util::modules::GetTargetOffset(AppRecords));
        for (int i = 0; i < 18; i++)
            if (*reinterpret_cast<s32*>(recs + i * AppRecordSize) == app_id)
                return reinterpret_cast<const char**>(recs + i * AppRecordSize + 0x10);
        return nullptr;
    }

    /* Name + icon of app 6. The app table is filled in by the game after boot, so this also runs
       from the watch thread until the name slot exists. */
    static void ApplyPhoneApp(bool on) {
        const char** name = AppNameSlot(SlumberAppId);
        if (name && *name) {
            if (!s_MilesLabel && *name != AppNameLabel) s_MilesLabel = *name;
            const char* want = on ? AppNameLabel : s_MilesLabel;
            if (want && *name != want) *name = want;
        }
        Mem m;
        float* rw = reinterpret_cast<float*>(m.Rw(AppIconFrames)) + SlumberAppId;
        float want = on ? SlumberIconFrame : MilesIconFrame;
        if (*rw != want) {
            *rw = want;
            armDCacheFlush(rw, sizeof(float));
        }
    }

    /* ---- Island editor: Happy Home Paradise's exterior editor on the main island ----
       HHP edits a vacation home's yard with UI scene 0x6f (cWherearenExteriorEdit): an
       FtrEditLayout actor plus the hand cursor. On the main island it is blocked or ends up in
       the wrong place in six spots, each handled below while the switch is on (all live-tested
       on the Switch):
       - 0x13e44ac "may this map edit furniture": true only on HHP / room maps (attribute bits
         6/21/25/34). FtrActorMgr's init only creates FtrEditLayout (class 11) at map load when
         it says yes, and both start-edit functions (0x13e3fb8, 0x13e462c) bail out without it.
       - 0x32fa3b0 the editor request gate (HHP manager state 4/5).
       - 0x15dc334 "warp the player while editing" (player state 0x61 enter/exit): rooms move the
         player to their entrance, HHP yards don't. On the island that spot is in the top-left sea.
       - 0x13b8fc4 the edit area (min, max): the HHP plot or the room, else 0..85 (top-left sea).
         Here the island's 7x6-acre grid, x 160..1280 and z 160..1120 (one tile = 10).
       - 0x7f9070 the edit camera's look-at (CamState cEditWherearenNpcGarden): the HHP plot's
         centre or a fixed (40, 0, 40). Here the cursor, so the camera follows it.
       - 0x13d5d30 puts the cursor at the area's centre once editing has started; here the first
         call after a start moves it to the player's tile instead.
       The editor opens from its own phone app: the DIY Recipes app slot the phone hides (app 3
       once DIY Recipes+ (app 4) is unlocked, else app 4). Its handler only closes the phone; the
       player's next button-handler frame then requests the scene, like the game's D-pad does. */

    struct Vec3f { float x, y, z; };

    constexpr s16 MainFieldStage       = 1;
    constexpr u32 HhpPlayMode          = 0xe820ff3d;
    constexpr u32 FtrEditLayoutVtable  = 0x48a6a48;
    constexpr u32 LayoutCursor         = 0xa3c;      /* Vec3f; copies at +0xa30, +0xa48, +0xf10 */
    constexpr u32 PlayerPos            = 0x29a0;     /* player actor: live position (Vec3f) */
    /* The cursor may go over the whole map, the 1-acre sea ring around the island included
       (0..1440 x 0..1280, 10 units in from the edge). The middle stays (720, 640): the closing
       re-centre is recognised by it. The player is only ever put back on the island grid. */
    constexpr float EditMinX = 10.0f, EditMinZ = 10.0f, EditMaxX = 1430.0f, EditMaxZ = 1270.0f;
    constexpr float IslandMinX = 160.0f, IslandMinZ = 160.0f, IslandMaxX = 1280.0f, IslandMaxZ = 1120.0f;
    constexpr u32 UiMgrGlobal          = 0x55b6f18;  /* the D-pad handler's UI request target */
    constexpr u32 UiRequest            = 0x2df7b98;  /* (mgr, scene id, {player index byte}, 0) */
    constexpr u32 ExteriorEditScene    = 0x6f;
    constexpr int DiyAppId             = 3;
    constexpr int DiyPlusAppId         = 4;
    constexpr const char* EditorAppLabel = "0091";
    constexpr float EditorIconFrame    = 17.0f;      /* Happy Home Network's icon */
    constexpr u32 ThemeBannerNone      = 0x345d7cc;  /* exterior menu banner: theme 0xffff -> 0 */
    constexpr u32 ThemeBannerNoneOrig  = 0x2a1f03e2; /* mov w2, wzr */
    constexpr u32 ThemeBannerSkip      = 0x14000014; /* b 0x345d81c (skip setting the banner text) */

    static u8* s_EditLayout     = nullptr;  /* FtrEditLayout of the last start-edit */
    static bool s_CursorToPlayer = false;
    static bool s_EditOpening   = false;    /* editor requested, cursor not at the player yet */
    static u8* s_LocalPlayer    = nullptr;  /* player actor seen by the button handler */
    static bool s_WarpArmed     = false;    /* editor has run: closing it may warp the player */
    static float s_WarpX = 0, s_WarpZ = 0;  /* cursor before closing re-centres it */
    static bool s_EditorClosed = false;     /* the editor just closed (its warp ran) */
    static s64 s_CursorBAt = 0;             /* B pressed in the editor's cursor mode (no menu open): a real close */
    static bool ClosedWithB() {
        return s_CursorBAt && nn::os::GetSystemTick().GetInt64Value() - s_CursorBAt < (s64)nn::os::GetSystemTickFrequency() * 5;
    }
    static bool s_EditClosing = false;      /* closing: the cursor was re-centred, camera stays put */
    static bool s_EditorPending = false;    /* app tapped: open the editor once the phone is gone */
    static int s_EditorSlot     = -1;       /* app id shown as the editor (3 or 4) */
    static const char* s_DiyLabel[2] = {};  /* the DIY apps' own name labels and icon frames */
    static float s_DiyFrame[2]  = { -1.0f, -1.0f };

    bool EditorAppOn() { return FeatureOn("island_editor"); }
    int EditorSlot() { return s_EditorSlot; }

    /* On the main island with the switch on (not while HHP's own play mode runs). */
    bool EditorMap() {
        if (!EditorAppOn()) return false;
        const s16* stage = *reinterpret_cast<s16* const*>(exl::util::modules::GetTargetOffset(StageIdGot));
        if (!stage || *stage != MainFieldStage) return false;
        const u8* mode = *reinterpret_cast<u8* const*>(exl::util::modules::GetTargetOffset(PlayModeGot));
        return !mode || *reinterpret_cast<const u32*>(mode + 0x38) != HhpPlayMode;
    }

    /* On the main island (not while HHP's own play mode runs), whatever the editor switch says. */
    bool EditorMapStage() {
        const s16* stage = *reinterpret_cast<s16* const*>(exl::util::modules::GetTargetOffset(StageIdGot));
        if (!stage || *stage != MainFieldStage) return false;
        const u8* mode = *reinterpret_cast<u8* const*>(exl::util::modules::GetTargetOffset(PlayModeGot));
        return !mode || *reinterpret_cast<const u32*>(mode + 0x38) != HhpPlayMode;
    }

    bool EditArea(float* min, float* max) {
        if (!min || !max || !EditorMap()) return false;
        min[0] = EditMinX; min[1] = 0.0f; min[2] = EditMinZ;
        max[0] = EditMaxX; max[1] = 0.0f; max[2] = EditMaxZ;
        return true;
    }

    static void SetCursorSpeed(u8* layout, bool fast);

    void EditStart(u8* layout) {
        s_WarpArmed = false;
        s_CursorBAt = 0;
        s_EditorClosed = false;
        s_EditClosing = false;
        s_EditLayout = layout;
        s_CursorToPlayer = EditorMap();
        if (!s_CursorToPlayer) SetCursorSpeed(layout, false);   /* rooms keep the normal speed */
    }

    static bool LiveLayout(u8* layout) {
        return layout && *reinterpret_cast<uintptr_t*>(layout) == exl::util::modules::GetTargetOffset(FtrEditLayoutVtable);
    }

    /* Closing the editor re-centres the cursor before the player is warped: keep where it was. */
    void EditCursorBeforeCentre(u8* layout) {
        if (!s_WarpArmed || layout != s_EditLayout || !LiveLayout(layout)) return;
        const Vec3f* c = reinterpret_cast<const Vec3f*>(layout + LayoutCursor);
        s_WarpX = c->x; s_WarpZ = c->z;
        if (ClosedWithB()) s_EditClosing = true;   /* the camera keeps looking here, not at the re-centred cursor */
    }

    void EditCursorStart(u8* layout) {
        if (!s_CursorToPlayer || layout != s_EditLayout || !s_LocalPlayer || !LiveLayout(layout) || !EditorMap()) return;
        s_CursorToPlayer = false;
        s_EditOpening = false;
        const Vec3f* pos = reinterpret_cast<const Vec3f*>(s_LocalPlayer + PlayerPos);
        float x = pos->x, z = pos->z;
        if (x < EditMinX + 5) x = EditMinX + 5;
        if (x > EditMaxX - 5) x = EditMaxX - 5;
        if (z < EditMinZ + 5) z = EditMinZ + 5;
        if (z > EditMaxZ - 5) z = EditMaxZ - 5;
        Vec3f c = { (float)(int)(x / 10.0f) * 10.0f + 5.0f, 0.0f, (float)(int)(z / 10.0f) * 10.0f + 5.0f };
        for (u32 off : { 0xa30u, LayoutCursor, 0xa48u, 0xf10u }) *reinterpret_cast<Vec3f*>(layout + off) = c;
        Log("island editor: cursor starts at the player (%d, %d)", (int)c.x, (int)c.z);
    }

    /* Cursor speed: the editor's stick ranges (normal, and the slower one used while holding) are
       parameters of its misc params (0x13c9440(layout)); each value pointer comes from the
       parameter's vfunc 0xf0. On the main island they run 1.25x faster; elsewhere the defaults. */
    constexpr u32 EditMiscParams     = 0x13c9440;   /* (layout) -> params */
    constexpr u32 ParamStickRange    = 0x160;       /* Vector2f mStickFactorRange */
    constexpr u32 ParamStickSpecial  = 0x198;       /* Vector2f mStickFactorRangeSpecial */
    constexpr float CursorSpeedScale = 1.25f;
    static float s_StickDefault[2][2];
    static bool s_StickKnown = false;

    static float* EditParam(u8* params, u32 off) {
        u8* obj = params + off;
        using GetFn = float* (*)(u8*);
        return reinterpret_cast<GetFn>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(obj) + 0xf0))(obj);
    }

    static void SetCursorSpeed(u8* layout, bool fast) {
        if (!layout) return;
        u8* params = reinterpret_cast<u8* (*)(u8*)>(exl::util::modules::GetTargetOffset(EditMiscParams))(layout);
        if (!params) return;
        float* v[2] = { EditParam(params, ParamStickRange), EditParam(params, ParamStickSpecial) };
        if (!v[0] || !v[1]) return;
        if (!s_StickKnown) {
            for (int i = 0; i < 2; i++) { s_StickDefault[i][0] = v[i][0]; s_StickDefault[i][1] = v[i][1]; }
            s_StickKnown = true;
        }
        float k = fast ? CursorSpeedScale : 1.0f;
        for (int i = 0; i < 2; i++) { v[i][0] = s_StickDefault[i][0] * k; v[i][1] = s_StickDefault[i][1] * k; }
    }

    /* The editor's player state (0x61) warps the player on entering and leaving it, to
       0x2309d50's target (a room entrance indoors), and while it runs keeps re-applying the
       position saved on entering (player +0xba4, and the position request at *(player+0x4c8)
       +0x2e8). On the main island only the leaving warp is taken, once the editor has run, and it
       goes to the cursor on the ground there; the saved position becomes that spot too. */
    constexpr u32 GroundHeight       = 0x15e026c;   /* (unused, pos*, out attr*, flags) -> y */
    constexpr u32 PlayerSavedPos     = 0xba4;
    constexpr u32 PlayerPosRequest   = 0x4c8;       /* -> +0x2e4..0x2e6 flags, +0x2e8 Vec3f */

    /* Closing the editor puts the cursor on the area middle first; the exterior menu closing
       (spawn menu, B) also runs the warp check, with the cursor anywhere: that is not a close. */
    static bool CursorAtEditCentre() {
        if (!LiveLayout(s_EditLayout)) return false;
        const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
        float mx = (EditMinX + EditMaxX) * 0.5f, mz = (EditMinZ + EditMaxZ) * 0.5f;
        return c->x - mx > -1 && c->x - mx < 1 && c->z - mz > -1 && c->z - mz < 1;
    }

    bool EditorWarpTarget(Vec3f* out) {
        if (!EditorMap() || !s_WarpArmed || !LiveLayout(s_EditLayout)) return false;
        s64 now = nn::os::GetSystemTick().GetInt64Value();
        if (!CursorAtEditCentre() || !ClosedWithB()) {
            static s64 s_LastNote = 0;
            if (now - s_LastNote > nn::os::GetSystemTickFrequency() * 2) { s_LastNote = now; Log("island editor: warp check without B in cursor mode (a menu closed) - not a close"); }
            return false;
        }
        s_WarpArmed = false;
        /* The cursor may be out at sea: the player goes to the nearest spot on the island grid. */
        float wx = s_WarpX, wz = s_WarpZ;
        if (wx < IslandMinX + 5) wx = IslandMinX + 5;
        if (wx > IslandMaxX - 5) wx = IslandMaxX - 5;
        if (wz < IslandMinZ + 5) wz = IslandMinZ + 5;
        if (wz > IslandMaxZ - 5) wz = IslandMaxZ - 5;
        Vec3f c = { wx, 0.0f, wz };
        c.y = reinterpret_cast<float (*)(float, float*, u32*, u64)>(exl::util::modules::GetTargetOffset(GroundHeight))(0.0f, &c.x, nullptr, 0);
        *out = c;
        if (s_LocalPlayer) {
            *reinterpret_cast<Vec3f*>(s_LocalPlayer + PlayerSavedPos) = c;
            u8* req = *reinterpret_cast<u8**>(s_LocalPlayer + PlayerPosRequest);
            if (req) {
                *reinterpret_cast<Vec3f*>(req + 0x2e8) = c;
                req[0x2e4] = req[0x2e5] = req[0x2e6] = 1;
            }
        }
        Log("island editor: player moved to the cursor (%d, %d)", (int)c.x, (int)c.z);
        s_EditorClosed = true;
        return true;
    }

    bool EditorWarpAllowed() { return EditorMap() && s_WarpArmed && s_CursorBAt && CursorAtEditCentre(); }

    /* Without the game's warp: the player's live position too (EditorWarpTarget already set the
       saved position and the position request). */
    void EditorPlacePlayerNow(const Vec3f& c) {
        if (!s_LocalPlayer) return;
        *reinterpret_cast<Vec3f*>(s_LocalPlayer + PlayerPos) = c;
        Log("island editor: placed without the warp wipe");
    }

    /* Spawn menu (further down): while it opens / is open the camera stays where the cursor was. */
    bool SpawnCameraHold(Vec3f* out);

    /* Until the cursor has been moved to the player, the camera looks at the player: the first
       frames would otherwise show the corner fallback or the last session's cursor. */

    bool EditCameraTarget(Vec3f* out) {
        if (!EditorMap()) return false;
        if (SpawnCameraHold(out)) return true;
        if (LiveLayout(s_EditLayout)) SetCursorSpeed(s_EditLayout, true);
        if (LiveLayout(s_EditLayout) && !s_EditOpening && !s_CursorToPlayer) {
            /* Track the cursor for the closing warp; the re-centred spot (area middle) is not one. */
            const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
            float mx = (EditMinX + EditMaxX) * 0.5f, mz = (EditMinZ + EditMaxZ) * 0.5f;
            bool centre = c->x - mx > -1 && c->x - mx < 1 && c->z - mz > -1 && c->z - mz < 1;
            if (!s_WarpArmed || !centre) { s_WarpX = c->x; s_WarpZ = c->z; s_EditClosing = false; }
            /* Closing (B) puts the cursor on the area middle while the edit camera still runs, which
               swung the camera across the map for a moment (seen in Eden): it stays on the last spot. */
            else if (s_WarpArmed && centre && ClosedWithB()) s_EditClosing = true;
            s_WarpArmed = true;
        }
        if ((s_EditOpening || s_CursorToPlayer || !LiveLayout(s_EditLayout)) && s_LocalPlayer) {
            *out = *reinterpret_cast<const Vec3f*>(s_LocalPlayer + PlayerPos);
            return true;
        }
        if (!LiveLayout(s_EditLayout)) return false;
        *out = *reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
        if (s_EditClosing) { out->x = s_WarpX; out->z = s_WarpZ; }   /* not the re-centred spot */
        return true;
    }

    /* ---- Island editor: grab buildings the HHP way ----
       In HHP the editor hovers a house through its actor: each frame (play mode HHP only) it asks
       0x13d0308 for the building actor under the cursor, then 0x13f1bc4 turns that into a hover of
       kind 4, and everything after (outline, "Hold A Grab", drag, drop, cancel) goes through the
       actor's building-edit component at +0xc98 (vtable 0x49a5880, 34 slots). The resolvers only
       accept HHP garden actors, so on the main island the module answers them itself: each building
       body gets a component of ours whose geometry slots are the game's own (reading a fake owner
       with a 4x4-tile footprint) and whose grab / drag / drop / cancel move the building's actors
       (body, wall, roof, door, door decoration, name sign: each keeps its live position as a
       current + previous Vec3f pair; writing both moves the model at once, live-tested). On drop
       the building's save record (20 bytes: type, X, Z in 5-unit cells) moves by the same amount,
       which the island uses from its next load. Collision stays at the old spot until then. */
    constexpr u32 ActorMgrGlobal   = 0x4b93e08;
    constexpr u32 SaveRootGlobal   = 0x5474040;
    constexpr u32 BuildingBodyVt   = 0x499b1a0;
    constexpr u32 SignVt           = 0x49a33d0;
    constexpr u32 BldCompVtable    = 0x49a5880;  /* HHP garden house component */
    constexpr int BldCompSlots     = 34;
    constexpr u32 ActorEditComp    = 0xc98;
    constexpr u32 ActorTransform   = 0x68;       /* +0x2b4 rotation (y) */
    constexpr u32 ItemNoneInit     = 0x267d4ac;  /* (u16* item) */
    constexpr u32 BldRectFull      = 0x2a85468;  /* slot 0xd8 (comp, rect out, pos) */
    constexpr u32 BldRectMin       = 0x2a8622c;  /* slot 0xd0 (comp, cell out, pos) */
    constexpr u32 PosVtableGot     = 0x4abbc20;  /* vtable of the (vt, Vec3f) position argument */
    constexpr u32 ActorRecordIndex = 0x174;      /* building's index in the island building list */
    constexpr u32 FieldMgrGlobal   = 0x5535578;  /* +0x26e8: per-building collision slots */
    constexpr u32 BldSlotOffset    = 0x26e8;
    constexpr u32 BldSlotRemove    = 0x2a3cfa0;  /* (slots, index, play mode): unmark tiles, drop collision */
    constexpr u32 BldSlotCreate    = 0x2a3a450;  /* (slots, index, play mode): collision + tiles from the record */
    struct PartKind { u32 vt; u32 pair; };
    static const PartKind k_BuildingParts[] = {
        { BuildingBodyVt, 0x7f0 }, { 0x49a0bb8, 0x808 }, { 0x49a0838, 0x7f0 },
        { 0x499e4d0, 0xa78 }, { 0x49a04e8, 0x7e0 }, { SignVt, 0x7d8 },
    };
    constexpr int MaxDragParts = 16;
    constexpr int MaxBldComps  = 12;
    constexpr float BldLift    = 5.0f;   /* HHP holds a house 5 above the ground */

    /* Laid out like the game's component where the editor and the reused slots read it. */
    struct BldComp {
        const void* vt;          /* +0x00 */
        u8* owner;               /* +0x08 fake owner (footprint at +0x5a8 / +0x5b8, transform at +0x68) */
        s32 state;               /* +0x10 0 = normal, 1 = disabled */
        s32 grabbed;             /* +0x14 */
        u8 game[0x48];           /* +0x18 the game's own fields, unused here */
        u8* body;
        Vec3f pos;               /* slot 0x20 result */
        u8* part[MaxDragParts];
        u32 pair[MaxDragParts];
        int parts;
        float startX, startZ, dx, dz;
        alignas(16) u8 fakeOwner[0x600];
    };
    static BldComp s_Comps[MaxBldComps];
    static const void* s_BldVt[BldCompSlots];

    static uintptr_t Main(u32 off) { return exl::util::modules::GetTargetOffset(off); }
    static u32 VtOf(u8* a) { return (u32)(*reinterpret_cast<uintptr_t*>(a) - exl::util::modules::GetTargetStart()); }

    static u32 PartPair(u8* actor) {
        u32 vt = VtOf(actor);
        for (const PartKind& k : k_BuildingParts) if (k.vt == vt) return k.pair;
        return 0;
    }

    template <typename F> static void ForEachActor(F f) {
        u8* mgr = *reinterpret_cast<u8**>(Main(ActorMgrGlobal));
        u8* tab = mgr ? *reinterpret_cast<u8**>(mgr + 0x1c0) : nullptr;
        u8** arr = tab ? *reinterpret_cast<u8***>(tab + 8) : nullptr;
        if (!arr) return;
        u32 n = *reinterpret_cast<u32*>(tab);
        for (u32 i = 0; i < n && i < 0x1000; i++) {
            u8* a = arr[i];
            if (!a || (*reinterpret_cast<u32*>(a + 0x248) & 2)) continue;
            f(a);
        }
    }

    static u8* ActorOfHandle(u32 h) {
        u8* mgr = *reinterpret_cast<u8**>(Main(ActorMgrGlobal));
        u8* tab = mgr && h ? *reinterpret_cast<u8**>(mgr + 0x1c0) : nullptr;
        u8** arr = tab ? *reinterpret_cast<u8***>(tab + 8) : nullptr;
        u8* a = arr ? arr[h >> 20] : nullptr;
        if (!a || (*reinterpret_cast<u32*>(a + 0x248) & 2) || *reinterpret_cast<u32*>(a + 0x230) != h) return nullptr;
        return a;
    }

    /* Our component on this actor, if it has one. */
    static BldComp* CompOf(u8* actor) {
        if (!actor) return nullptr;
        BldComp* c = *reinterpret_cast<BldComp**>(actor + ActorEditComp);
        if (c < &s_Comps[0] || c >= &s_Comps[MaxBldComps] || c->body != actor) return nullptr;
        return c;
    }

    static void ShiftParts(BldComp* c, float dx, float dz, float dy = 0.0f) {
        for (int i = 0; i < c->parts; i++)
            for (u32 off : { c->pair[i], c->pair[i] + 0xcu }) {
                Vec3f* p = reinterpret_cast<Vec3f*>(c->part[i] + off);
                p->x += dx; p->y += dy; p->z += dz;
            }
    }

    /* Raw save records: save root -> main block (+0x70) -> building view (+0x116cc0). */
    static u8* BuildingRecords() {
        u8* d = *reinterpret_cast<u8**>(Main(SaveRootGlobal));
        if (!d || (d[0x453] & 1)) return nullptr;
        u8** p3 = *reinterpret_cast<u8***>(d + 0x10);
        u8* x = p3 ? *p3 : nullptr;
        u8* data = x ? *reinterpret_cast<u8**>(x + 0x20) : nullptr;
        if (!data) return nullptr;
        u8* view = data + 0x70 + 0x116cc0;
        return *reinterpret_cast<u8**>(view + 8) + *reinterpret_cast<u64*>(view + 0x10);
    }

    /* The save object (*(save root+0x10))[0] holds the live main.dat image at +0x10 and two more
       full images: +0x18 (pointer) and +0x8 (an object with the image inline at +0x250). Save and
       End writes from those, not from the live one, so building changes made by the module came
       back after a reload (found live in Eden 2026-11-23: patching both copies kept a removed
       bridge removed). The building list is copied into both after every change. Each copy is
       only used when its first 16 bytes match the live image's header. */
    constexpr u32 BuildingListBytes = 47 * 0x14;
    static void SyncSaveCopies() {
        u8* d = *reinterpret_cast<u8**>(Main(SaveRootGlobal));
        if (!d || (d[0x453] & 1)) return;
        u8** p3 = *reinterpret_cast<u8***>(d + 0x10);
        u8* x = p3 ? *p3 : nullptr;
        u8* data = x ? *reinterpret_cast<u8**>(x + 0x20) : nullptr;
        if (!data) return;
        u8* view = data + 0x70 + 0x116cc0;
        u8* live = *reinterpret_cast<u8**>(view + 8);
        u64 off = *reinterpret_cast<u64*>(view + 0x10);
        if (!live || live != *reinterpret_cast<u8**>(x + 0x10)) return;
        u8* inl = *reinterpret_cast<u8**>(x + 8);
        u8* copies[2] = { *reinterpret_cast<u8**>(x + 0x18), inl ? inl + 0x250 : nullptr };
        static int s_Warned = 0;
        for (int i = 0; i < 2; i++) {
            u8* c = copies[i];
            if (!c || c == live || memcmp(c, live, 16) != 0) {
                if (!(s_Warned & (1 << i))) { s_Warned |= 1 << i; Log("save copies: copy %d not found (%p) - not synced", i, c); }
                continue;
            }
            if (memcmp(c + off, live + off, BuildingListBytes) != 0) memcpy(c + off, live + off, BuildingListBytes);
        }
    }

    static void SaveBuildingMove(u32 index, float bodyX, float bodyZ, s32 dcx, s32 dcz) {
        u8* recs = BuildingRecords();
        if (!recs) { Log("buildings: save records not found"); return; }
        int best = -1, bestDz = 61;
        if (index < 47) {   /* the building's own list index, if its record sits where the body is */
            s16* r = reinterpret_cast<s16*>(recs + index * 0x14);
            if (r[0] && r[1] * 5 == (s32)bodyX) best = (int)index;
        }
        for (int i = 0; i < 47 && best < 0; i++) {
            s16* r = reinterpret_cast<s16*>(recs + i * 0x14);
            if (!r[0] || r[1] * 5 != (s32)bodyX) continue;
            int dz = r[2] * 5 - (s32)bodyZ; if (dz < 0) dz = -dz;
            if (dz < bestDz) { bestDz = dz; best = i; }
        }
        if (best < 0) { Log("buildings: no record at (%d, %d)", (int)bodyX, (int)bodyZ); return; }
        s16* r = reinterpret_cast<s16*>(recs + best * 0x14);
        Log("buildings: record %d (type %d) (%d, %d) -> (%d, %d)", best, r[0], r[1], r[2], r[1] + dcx, r[2] + dcz);
        r[1] = (s16)(r[1] + dcx);
        r[2] = (s16)(r[2] + dcz);
        SyncSaveCopies();
    }

    /* The island's building collision / occupied tiles, as HHP refreshes its house on grab and drop.
       The play mode hash picks the island's building list (main island: land index 1). */
    static u32 PlayModeHash() {
        const u8* mode = *reinterpret_cast<u8* const*>(Main(PlayModeGot));
        return mode ? *reinterpret_cast<const u32*>(mode + 0x38) : 0;
    }

    static void BuildingSlot(BldComp* c, bool create) {
        u8* field = *reinterpret_cast<u8**>(Main(FieldMgrGlobal));
        if (!field) return;
        u32 idx = *reinterpret_cast<u32*>(c->body + ActorRecordIndex);
        if (create)
            reinterpret_cast<u64 (*)(u8*, u32, u64, u64, u64, u64, u64, u64)>(Main(BldSlotCreate))(field + BldSlotOffset, idx, PlayModeHash(), 0, 0, 0, 0, 0);
        else
            reinterpret_cast<void (*)(u8*, u32, u32)>(Main(BldSlotRemove))(field + BldSlotOffset, idx, PlayModeHash());
    }

    static void RespawnHouse(u8* body);
    static void PlayEditFx(const char* effect, const char* sound, const Vec3f& at);
    static int StructureOfBody(u8* body);   /* 26 bridge, 27 incline, else 0 */
    static int RemovableBuildingOfBody(u8* body);   /* a spawned shop / service copy: its type, else 0 */
    /* Buildings tab cells (tab 7, see RemovableBuildingOfBody) */
    s16 k_BuildingCells[] = {   /* not const: dbg pokes a type into a cell to try it */ 19, 21, 20, 24, 25, 28, 23, 29, 42, 9 };   /* Cranny, Museum, Resident Services, Able Sisters, Campsite, Redd, tent, studio, hotel, empty house */
    constexpr int BuildingCells = sizeof(k_BuildingCells) / sizeof(k_BuildingCells[0]);

    /* -- component slots (x0 = component) -- */
    static void BldNoop(BldComp*) {}

    /* 0x20: footprint centre the way HHP reports it (z 5 in front of the body). */
    bool SpawnPosFor(const u8* body, Vec3f* out);   /* spawn menu: the borrowed house stands at the cursor */

    /* Held like furniture: while a building / bridge is held it bobs gently and glides to each new
       cell instead of jumping (a visual offset on top of the real position, undone before anything
       reads or changes it for real). */
    float s_SquareDy = 0.0f;   /* square height vs the ground, live-tunable (sl::s_SquareDy): it drew over buildings */
    static BldComp* s_HeldComp = nullptr;
    static float s_DragOffX = 0, s_DragOffZ = -5.0f;
    static void SquareOffset(u8* body, float* ox, float* oz);
    static Vec3f s_HeldV = {};          /* offset applied to the parts right now */
    static float s_GlideX = 0, s_GlideZ = 0;
    static int s_HeldT = 0;
    static void HeldUndo(BldComp* c) {
        if (c != s_HeldComp) return;
        ShiftParts(c, -s_HeldV.x, -s_HeldV.z, -s_HeldV.y);
        s_HeldV = {};
    }
    static void HeldTick() {
        BldComp* c = s_HeldComp;
        if (!c) return;
        if (!c->grabbed || !c->body) { s_HeldComp = nullptr; s_HeldV = {}; return; }
        s_HeldT++;
        s_GlideX *= 0.65f; s_GlideZ *= 0.65f;
        if (s_GlideX > -0.05f && s_GlideX < 0.05f) s_GlideX = 0;
        if (s_GlideZ > -0.05f && s_GlideZ < 0.05f) s_GlideZ = 0;
        float bob = 0.8f * __builtin_sinf(s_HeldT * 0.10f);
        Vec3f v = { s_GlideX, bob, s_GlideZ };
        ShiftParts(c, v.x - s_HeldV.x, v.z - s_HeldV.z, v.y - s_HeldV.y);
        s_HeldV = v;
    }

    static Vec3f* BldPos(BldComp* c) {
        Vec3f sp;
        if (SpawnPosFor(c->body, &sp)) {
            /* 100 under the ground: the editor's square / hand for the borrowed house showed at the
               cursor for a moment when the spawn menu opened (Switch video) */
            c->pos = { sp.x, reinterpret_cast<const Vec3f*>(c->body + 0x7f0)->y - 1000.0f, sp.z };   /* (100 under still showed a tiny square) */
            return &c->pos;
        }
        const Vec3f* p = reinterpret_cast<const Vec3f*>(c->body + 0x7f0);
        Vec3f hv = c == s_HeldComp ? s_HeldV : Vec3f{};
        /* the square is drawn here (its size from the footprint FootOf sets): the middle of the
           structure, not the body (sizes / offsets changed nothing while this was body z - 5) */
        float ox, oz;
        SquareOffset(c->body, &ox, &oz);
        c->pos = { p->x - hv.x + ox, p->y - hv.y - (c->grabbed ? BldLift : 0.0f), p->z - hv.z + oz };
        return &c->pos;
    }

    /* 0x58: grab - collect the building's actors around the body. */
    static bool RespawnPending(u8* body);
    static bool BldGrab(BldComp* c) {
        if (c->state == 1) return false;
        /* just dropped and still being rebuilt: grabbing the old actor, which the rebuild then deleted,
           left the hand dragging nothing (the camera followed it out to sea - Switch video) */
        if (RespawnPending(c->body)) { Log("buildings: still rebuilding - grab ignored"); return false; }
        const Vec3f* b = reinterpret_cast<const Vec3f*>(c->body + 0x7f0);
        float bx = b->x, bz = b->z;
        c->parts = 0;
        bool structure = StructureOfBody(c->body) != 0;
        if (structure) {   /* a bridge / incline is its body alone (a neighbour bridge came along) */
            c->part[0] = c->body; c->pair[0] = 0x7f0; c->parts = 1;
        }
        ForEachActor([&](u8* a) {
            u32 pair = PartPair(a);
            if (structure || !pair || c->parts >= MaxDragParts) return;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + (VtOf(a) == SignVt ? 0x160 : pair));
            float dx = p->x - bx, dz = p->z - bz;
            if (dx < -8 || dx > 8 || dz < -8 || dz > 8) return;
            c->part[c->parts] = a; c->pair[c->parts] = pair; c->parts++;
        });
        c->startX = bx; c->startZ = bz; c->dx = c->dz = 0;
        SquareOffset(c->body, &s_DragOffX, &s_DragOffZ);   /* BldPos reports the square's middle */
        c->grabbed = 1;
        ShiftParts(c, 0, 0, BldLift);
        s_HeldComp = c; s_HeldV = {}; s_GlideX = s_GlideZ = 0; s_HeldT = 0;
        BuildingSlot(c, false);
        Log("buildings: grabbed building at (%d, %d), %d parts", (int)bx, (int)bz, c->parts);
        PlayEditFx(nullptr, "家具をつかむ", Vec3f{ bx, b->y, bz });   /* like grabbing furniture */
        return true;
    }

    /* Nearest multiple of step (truncating toward zero left a double-width dead zone at the start
       and made the house lag, then jump). */
    static float RoundToStep(float v, float step) {
        return (float)(int)(v / step + (v >= 0 ? 0.5f : -0.5f)) * step;
    }

    /* 0x68: drag to pos (+8 x, +0x10 z: footprint centre, already on the editor's grid). The house
       follows it in 5-unit cells, the unit of its save record. */
    static void SquareDims(u8* body, int fk, float* hxOut, float* hzOut, float* oxOut, float* ozOut);
    static void BldDrag(BldComp* c, const u8* pos) {
        if (!c->grabbed || !pos) return;
        float x = *reinterpret_cast<const float*>(pos + 8) - s_DragOffX, z = *reinterpret_cast<const float*>(pos + 0x10) - s_DragOffZ;
        float tx = RoundToStep(x - c->startX, 5.0f);
        float tz = RoundToStep(z - c->startZ, 5.0f);
        if (tx == c->dx && tz == c->dz) return;
        ShiftParts(c, tx - c->dx, tz - c->dz);
        if (c == s_HeldComp) {   /* looks like it stays put, then glides over */
            s_GlideX -= tx - c->dx; s_GlideZ -= tz - c->dz;
            ShiftParts(c, -(tx - c->dx), -(tz - c->dz));
            s_HeldV.x -= tx - c->dx; s_HeldV.z -= tz - c->dz;
        }
        c->dx = tx; c->dz = tz;
    }

    /* Buildings are rebuilt at grass level wherever they go (seen on a Switch: a shop dragged onto
       the beach hung in the air). Flat ground at 0 or above around (x, z): the middle and 4 points
       <r> away. */
    [[maybe_unused]] static bool FlatGroundAt(float x, float z, float r, float* loOut = nullptr, float* hiOut = nullptr) {
        using HeightFn = float (*)(float, float*, u32*, u64);
        float lo = 1e9f, hi = -1e9f;
        for (int k = 0; k < 5; k++) {
            float pt[3] = { x + (k == 1 ? -r : k == 2 ? r : 0.0f), 0.0f, z + (k == 3 ? -r : k == 4 ? r : 0.0f) };
            float h = reinterpret_cast<HeightFn>(exl::util::modules::GetTargetOffset(GroundHeight))(0.0f, pt, nullptr, 0);
            if (h < lo) lo = h;
            if (h > hi) hi = h;
        }
        if (loOut) *loOut = lo;
        if (hiOut) *hiOut = hi;
        return lo >= -0.5f && hi - lo <= 1.0f;
    }

    /* 0x70: placement check - anywhere goes on the main island (bridges may stack too). */
    static int StructureOfBody(u8* body);
    static u32 BldValid(BldComp* c, u64, const u8*) { return c->grabbed ? 1 : 0; }   /* SnapBuildingsToGround fixes the height */

    /* 0x78: drop at pos and keep it. */
    static void BldDrop(BldComp* c, const u8* pos) {
        if (!c->grabbed) return;
        BldDrag(c, pos);
        HeldUndo(c); s_HeldComp = nullptr;
        c->grabbed = 0;
        ShiftParts(c, 0, 0, -BldLift);
        if (c->dx != 0 || c->dz != 0)
            SaveBuildingMove(*reinterpret_cast<u32*>(c->body + ActorRecordIndex), c->startX, c->startZ,
                             (s32)(c->dx / 5.0f), (s32)(c->dz / 5.0f));
        BuildingSlot(c, true);
        PlayEditFx(nullptr, "家具をはなす", *reinterpret_cast<const Vec3f*>(c->body + 0x7f0));   /* like dropping furniture */
        if (c->dx == 0 && c->dz == 0) return;
        Log("buildings: dropped, moved by (%d, %d)", (int)c->dx, (int)c->dz);
        RespawnHouse(c->body);   /* rebuilt at the new spot: ground height, door, etc. */
    }

    /* 0x60: put back. */
    static void BldCancel(BldComp* c) {
        if (!c->grabbed) return;
        HeldUndo(c); s_HeldComp = nullptr;
        ShiftParts(c, -c->dx, -c->dz, -BldLift);
        c->dx = c->dz = 0;
        c->grabbed = 0;
        BuildingSlot(c, true);
        Log("buildings: put back");
    }

    /* 0xb0: "can be removed" - the editor shows its own "Y Remove" for the hovered object when this
       says yes (found live by flipping the slots). Yes only for bridges / inclines, never while a
       spawn menu (opened on a borrowed house) is involved: with yes on every building the editor got
       stuck after a spawn menu closed. Y itself is handled in EditorPad, which hides it from the game. */
    bool SpawnBusy();
    static u32 BldCanRemove(BldComp* c) { return c && c->body && !SpawnBusy() && (StructureOfBody(c->body) || RemovableBuildingOfBody(c->body)) ? 1 : 0; }

    static void InitBldVtable() {
        if (s_BldVt[0]) return;
        const uintptr_t* game = reinterpret_cast<const uintptr_t*>(Main(BldCompVtable));
        for (int i = 0; i < BldCompSlots; i++) s_BldVt[i] = reinterpret_cast<const void*>(game[i]);
        s_BldVt[0x10 / 8] = reinterpret_cast<const void*>(&BldNoop);    /* destructors: ours are static */
        s_BldVt[0x18 / 8] = reinterpret_cast<const void*>(&BldNoop);
        s_BldVt[0x20 / 8] = reinterpret_cast<const void*>(&BldPos);
        s_BldVt[0x58 / 8] = reinterpret_cast<const void*>(&BldGrab);
        s_BldVt[0x60 / 8] = reinterpret_cast<const void*>(&BldCancel);
        s_BldVt[0x68 / 8] = reinterpret_cast<const void*>(&BldDrag);
        s_BldVt[0x70 / 8] = reinterpret_cast<const void*>(&BldValid);
        s_BldVt[0x78 / 8] = reinterpret_cast<const void*>(&BldDrop);
        s_BldVt[0xb0 / 8] = reinterpret_cast<const void*>(&BldCanRemove);

    }

    static bool CompInUse(BldComp& c) {
        return c.body && ActorOfHandle(*reinterpret_cast<u32*>(c.body + 0x230)) == c.body &&
               *reinterpret_cast<BldComp**>(c.body + ActorEditComp) == &c;
    }

    /* Give a building body our component (reusing a free one). */
    static BldComp* AttachComp(u8* body) {
        if (BldComp* c = CompOf(body)) return c;
        InitBldVtable();
        BldComp* slot = nullptr;
        for (BldComp& c : s_Comps) if (!CompInUse(c)) { slot = &c; break; }
        if (!slot) {   /* all in use: take the first idle one */
            for (BldComp& c : s_Comps) if (!c.grabbed) { slot = &c; break; }
            if (!slot) return nullptr;
            *reinterpret_cast<BldComp**>(slot->body + ActorEditComp) = nullptr;
        }
        memset(slot, 0, sizeof(*slot));
        slot->vt = s_BldVt;
        slot->owner = slot->fakeOwner;
        slot->body = body;
        *reinterpret_cast<u8**>(slot->fakeOwner + ActorTransform) = *reinterpret_cast<u8**>(body + ActorTransform);
        *reinterpret_cast<s32*>(slot->fakeOwner + 0x5b8) = 3;   /* footprint tiles 0..3 x 0..3 */
        *reinterpret_cast<s32*>(slot->fakeOwner + 0x5bc) = 3;
        *reinterpret_cast<BldComp**>(body + ActorEditComp) = slot;
        return slot;
    }

    /* 0x13d0308 on the main island: the building whose footprint holds the cursor cell
       (query +8 / +0xc, 5-unit cells). Fills the full rect (out) and the min cell (minOut) like
       the game. Returns the body's actor handle or 0. */
    static int StructureOfBody(u8* body);   /* 26 bridge, 27 incline, else 0 */
    static bool IsBridgeBody(u8* body) { return StructureOfBody(body) != 0; }
    constexpr float BridgeReach = 28.0f;   /* a bridge is up to 5 tiles long: hover within this of its middle */
    constexpr float InclineReach = 18.0f;

    /* ---- Spawn menu (D-pad left in the island editor) - TEST stage 1: open only ----
       The exterior menu is a UI sequence the editor starts on X with the hovered building. D-pad left
       in cursor mode arms a spawn: for a moment the hover answers a house even where there is none,
       and an X press is fed in, so the editor opens the menu as usual; it then starts on the Bridges
       tab and its picks are only logged (house design changes are blocked). */
    static s64 s_SpawnAt = 0;            /* tick of the D-pad left press (0 = none) */
    static bool s_SpawnArmed = false;    /* the next menu open is a spawn menu */
    static bool s_SpawnMode = false;     /* the open menu is a spawn menu */
    static s64 s_CursorQueryTick = 0;    /* last hover query: the editor's cursor mode is running */
    static s64 NowTick() { return nn::os::GetSystemTick().GetInt64Value(); }
    static s64 MsTicks(int ms) { return nn::os::GetSystemTickFrequency() * ms / 1000; }
    static bool SpawnWithin(int fromMs, int toMs) {
        if (!s_SpawnAt) return false;
        s64 d = NowTick() - s_SpawnAt;
        return d >= MsTicks(fromMs) && d < MsTicks(toMs);
    }

    static Vec3f s_SpawnCam = {};        /* cursor when D-pad left was pressed */
    static u32 s_HoverHandle = 0;        /* bridge / incline under the cursor (last hover lookup), or 0 */
    bool RemoveStructure(u8* body);
    static u8* s_SpawnBody = nullptr;    /* the house borrowed to open the menu on */
    bool SpawnBusy() { return s_SpawnArmed || s_SpawnMode; }

    /* The menu frames its building: while spawning, the borrowed house reports the cursor's spot. */
    bool SpawnPosFor(const u8* body, Vec3f* out) {
        if (!body || body != s_SpawnBody || !(s_SpawnArmed || s_SpawnMode) || !out) return false;
        *out = s_SpawnCam;
        return true;
    }

    /* The editor's per-frame house hover (0x13cfd7c) runs every frame in cursor mode, also with
       the cursor standing still (the hover lookups only run while it moves). */
    void CursorHeartbeat() { s_CursorQueryTick = NowTick(); }

    /* The menu opens on the borrowed house, which made the edit camera fly to it: keep it here. */
    bool SpawnCameraHold(Vec3f* out) {
        /* No menu came of the D-pad press: stop holding (the camera stayed put while the cursor left). */
        if (s_SpawnArmed && !SpawnWithin(0, 800)) { s_SpawnArmed = false; s_SpawnBody = nullptr; Log("spawn menu: did not open"); }
        if (!(s_SpawnArmed || s_SpawnMode) || !out) return false;
        /* The cursor moves again (menu closed, its close was missed): the camera follows it again. */
        if (s_SpawnMode && LiveLayout(s_EditLayout)) {
            const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
            float dx = c->x - s_SpawnCam.x, dz = c->z - s_SpawnCam.z;
            if (dx * dx + dz * dz > 12.0f * 12.0f) {
                s_SpawnMode = false; s_SpawnBody = nullptr;
                Log("spawn menu: closed (cursor moved)");
                return false;
            }
        }
        *out = s_SpawnCam;
        return true;
    }

    /* 0x7d10d4: the edit camera's point = look-at (0x7f9070) + an offset at *(cam+8)+0x30920. The
       exterior menu zooms in on its house; while it is a spawn menu the camera keeps the offset it
       had before the menu (logged when it changes, to confirm this is the zoom). */
    static Vec3f s_CamOffset = {};
    static bool s_CamOffsetSet = false;
    void EditCameraPoint(u8* cam, Vec3f* out) {
        if (!cam || !out || !EditorMap()) return;
        u8* inner = *reinterpret_cast<u8**>(cam + 8);
        if (!inner) return;
        Vec3f off = *reinterpret_cast<const Vec3f*>(inner + 0x30920);
        static Vec3f s_Logged = {};
        static int s_Logs = 0;
        float ch = (off.x - s_Logged.x) * (off.x - s_Logged.x) + (off.y - s_Logged.y) * (off.y - s_Logged.y) +
                   (off.z - s_Logged.z) * (off.z - s_Logged.z);
        if (ch > 0.25f && s_Logs < 80) {
            s_Logs++; s_Logged = off;
            Log("camera: offset (%d, %d, %d)%s", (int)(off.x * 10), (int)(off.y * 10), (int)(off.z * 10),
                (s_SpawnArmed || s_SpawnMode) ? " - spawn menu, kept" : "");
        }
        if (s_SpawnArmed || s_SpawnMode) {
            if (!s_CamOffsetSet) return;
            out->x += s_CamOffset.x - off.x;
            out->y += s_CamOffset.y - off.y;
            out->z += s_CamOffset.z - off.z;
            return;
        }
        s_CamOffset = off;
        s_CamOffsetSet = true;
    }

    /* 0x7cfa20: the edit camera's second delegate (beside the look-at point 0x7d10d4): a Vec3 that
       blends zoom presets (*(x0+8)+0x315e4 = preset index). Spawn menu: the value from before it. */
    static Vec3f s_CamZoom = {};
    static bool s_CamZoomSet = false;
    static u8* s_DbgCam = nullptr;      /* edit camera controller, for the debug channel */

    /* The exterior menu zooms onto its house by switching the camera's state machine (camera+0x408,
       current state at +0x410) from the editor camera (0x13) to CamState::cDemo (0x22), through the
       generic change-state 0x3dbb160 (found live: camera snapshot diff around the menu opening). A spawn
       menu's house is only borrowed, so the camera stays the editor's. */
    bool BlockCameraState(u8* sm, u32 state) {
        if (state != 0x22 || !s_DbgCam || sm != s_DbgCam + 0x408 || !EditorMap()) return false;
        if (!(s_SpawnArmed || s_SpawnMode)) return false;
        Log("spawn menu: close-up camera skipped");
        return true;
    }
    void DbgApplyHolds();
    void EditCameraZoom(u8* self, Vec3f* v) {
        if (!self || !v || !EditorMap()) return;
        u8* cam = *reinterpret_cast<u8**>(self + 8);
        s_DbgCam = cam;
        DbgApplyHolds();
        /* Diagnostic: which camera fields does the spawn menu change? Snapshot on D-pad left, diff
           1.2 s later (values that moved, at most 80 lines). */
        constexpr u32 SnapSize = 0x38000;
        alignas(16) static u8 s_Snap[SnapSize];
        static s64 s_SnapAt = 0;
        static bool s_Diffed = true;
        if (cam && s_SpawnAt && s_SpawnAt != s_SnapAt && SpawnWithin(0, 50)) {
            memcpy(s_Snap, cam, SnapSize);
            s_SnapAt = s_SpawnAt; s_Diffed = false;
            Log("camera: snapshot at D-pad left (cam %lx)", (unsigned long)(uintptr_t)cam);
        }
        if (cam && !s_Diffed && s_SnapAt == s_SpawnAt && !SpawnWithin(0, 1200)) {
            s_Diffed = true;
            int n = 0;
            for (u32 o = 0; o + 4 <= SnapSize && n < 80; o += 4) {
                u32 ia = *reinterpret_cast<const u32*>(s_Snap + o), ib = *reinterpret_cast<const u32*>(cam + o);
                if (ia == ib) continue;
                float a = *reinterpret_cast<const float*>(s_Snap + o), b = *reinterpret_cast<const float*>(cam + o);
                bool fa = (ia & 0x7f800000) != 0x7f800000 && (a > 1e-4f || a < -1e-4f) && a < 1e5f && a > -1e5f;
                bool fb = (ib & 0x7f800000) != 0x7f800000 && (b > 1e-4f || b < -1e-4f) && b < 1e5f && b > -1e5f;
                if (fa || fb) {
                    float d = a - b; if (d < 0) d = -d;
                    if (d < 0.05f) continue;
                    Log("camera: +%x  %d -> %d (/100)", o, (int)(a * 100), (int)(b * 100));
                } else if (ia < 0x10000 && ib < 0x10000) {
                    Log("camera: +%x  int %d -> %d", o, (int)ia, (int)ib);
                } else continue;
                n++;
            }
            Log("camera: diff done, %d changes listed", n);
        }
        static Vec3f s_Logged = {};
        static int s_Logs = 0;
        float ch = (v->x - s_Logged.x) * (v->x - s_Logged.x) + (v->y - s_Logged.y) * (v->y - s_Logged.y) +
                   (v->z - s_Logged.z) * (v->z - s_Logged.z);
        if (ch > 0.01f && s_Logs < 120) {
            s_Logs++; s_Logged = *v;
            Log("camera: zoom (%d, %d, %d) /100, preset %d, t %d%s", (int)(v->x * 100), (int)(v->y * 100), (int)(v->z * 100),
                cam ? *reinterpret_cast<s32*>(cam + 0x315e4) : -1, (int)(*reinterpret_cast<float*>(self + 0x10) * 100),
                (s_SpawnArmed || s_SpawnMode) ? " - spawn menu, kept" : "");
        }
        if (s_SpawnArmed || s_SpawnMode) {
            if (s_CamZoomSet) *v = s_CamZoom;
            return;
        }
        s_CamZoom = *v;
        s_CamZoomSet = true;
    }

    bool ReshapeStructure(u8* body, bool rotate, bool resize);

    /* The bridge / incline nearest the cursor, within its reach (the hover lookup runs for several
       cells, so its last answer is not always the one under the cursor). */
    static u8* NearestStructure() {
        if (!LiveLayout(s_EditLayout)) return nullptr;
        const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
        u8* body = nullptr;
        float best = 1e9f;
        ForEachActor([&](u8* a) {
            if (VtOf(a) != BuildingBodyVt) return;
            int kind = StructureOfBody(a);
            if (!kind) return;
            float reach = kind == 26 ? BridgeReach : InclineReach;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + 0x7f0);
            float d = (p->x - c->x) * (p->x - c->x) + (p->z - c->z) * (p->z - c->z);
            if (d < reach * reach && d < best) { best = d; body = a; }
        });
        return body;
    }

    /* For Y: the nearest bridge / incline, or a spawned shop / service copy (within 30 of its middle). */
    constexpr float BuildingReach = 30.0f;
    static u8* NearestRemovable() {
        if (!LiveLayout(s_EditLayout)) return nullptr;
        const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
        u8* body = nullptr;
        float best = 1e9f;
        ForEachActor([&](u8* a) {
            if (VtOf(a) != BuildingBodyVt) return;
            int kind = StructureOfBody(a);
            float reach = kind == 26 ? BridgeReach : kind == 27 ? InclineReach : RemovableBuildingOfBody(a) ? BuildingReach : 0.0f;
            if (reach <= 0.0f) return;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + 0x7f0);
            float d = (p->x - c->x) * (p->x - c->x) + (p->z - c->z) * (p->z - c->z);
            if (d < reach * reach && d < best) { best = d; body = a; }
        });
        return body;
    }

    /* Quick start (sai: "skip the game opening"): from boot until the player is first on the island,
       A is pressed for you (100 ms every 600 ms, from 3 s after boot, at most 3 minutes): the title
       screen and the notices after it go by without waiting. Off with quick_start=0. */
    static bool s_QuickStart = false;   /* toggles.txt "quick_start" (off: it kept pressing A in Eden) */
    void QuickStartPad(void* states, int count) {
        static s64 s_Boot = 0;
        static bool s_Done = false;
        if (s_Done || !states || count <= 0) return;
        s64 now = NowTick();
        if (!s_Boot) s_Boot = now;
        if (s_LocalPlayer || !s_QuickStart || now - s_Boot > MsTicks(180000)) {
            s_Done = true;
            Log("quick start: done (%s)", s_LocalPlayer ? "on the island" : !s_QuickStart ? "off" : "timed out");
            return;
        }
        s64 t = now - s_Boot - MsTicks(3000);
        if (t < 0 || t % MsTicks(600) >= MsTicks(100)) return;
        u8* s = reinterpret_cast<u8*>(states);
        for (int i = 0; i < count && i < 16; i++) *reinterpret_cast<u64*>(s + i * 0x28 + 8) |= 1ull;
    }

    /* Controller reads (all of them pass here): D-pad left -> spawn; X fed in 60..110 ms later. */
    void EditorPad(void* states, int count) {
        if (!states || count <= 0 || !EditorMap() || s_EditClosing) return;
        u8* s = reinterpret_cast<u8*>(states);
        u64 newest = *reinterpret_cast<u64*>(s + 8);
        static bool s_LeftWas = false;
        bool left = (newest >> 12) & 1;
        bool cursorMode = s_CursorQueryTick && NowTick() - s_CursorQueryTick < MsTicks(150);
        if (cursorMode && s_SpawnMode) { s_SpawnMode = false; s_SpawnBody = nullptr; Log("spawn menu: closed"); }
        if (left && !s_LeftWas && cursorMode && !SpawnWithin(0, 400)) {
            s_SpawnAt = NowTick();
            s_SpawnArmed = true;
            if (LiveLayout(s_EditLayout)) s_SpawnCam = *reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
            Log("spawn menu: D-pad left");
        }
        s_LeftWas = left;
        /* B in cursor mode closes the editor; B in a menu only closes the menu (the game runs its
           closing warp check for both, with the cursor on the area middle). */
        static bool s_BWas = false;
        bool bBtn = (newest >> 1) & 1;
        if (bBtn && !s_BWas) s_CursorBAt = cursorMode && !SpawnBusy() ? NowTick() : 0;
        s_BWas = bBtn;
        /* Y on a hovered bridge / incline removes it (like Y on an item). */
        static bool s_YWas = false;
        bool y = (newest >> 3) & 1;
        /* Y removes, ZL turns (quarter turn), ZR resizes the bridge / incline nearest the cursor;
           those presses are then hidden from the game (it has its own Y path for removable objects). */
        static bool s_ZlWas = false, s_ZrWas = false;
        bool zl = (newest >> 8) & 1, zr = (newest >> 9) & 1;
        u64 hide = 0;
        static bool s_Hiding[3] = {};   /* decided on the press (before a removal), kept while held */
        bool near = cursorMode && !SpawnBusy() && (((y && !s_YWas) && NearestRemovable()) || (((zl && !s_ZlWas) || (zr && !s_ZrWas)) && NearestStructure()));
        if (y && !s_YWas) s_Hiding[0] = near;
        if (zl && !s_ZlWas) s_Hiding[1] = near;
        if (zr && !s_ZrWas) s_Hiding[2] = near;
        if (cursorMode && !SpawnBusy() && LiveLayout(s_EditLayout) && ((y && !s_YWas) || (zl && !s_ZlWas) || (zr && !s_ZrWas))) {
            u8* body = (y && !s_YWas) ? NearestRemovable() : NearestStructure();
            if (!body) {
                const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
                Log("structures: no bridge or incline near the cursor (%d, %d)", (int)c->x, (int)c->z);
            } else if (y && !s_YWas) {
                if (RemoveStructure(body)) s_HoverHandle = 0;
            } else {
                /* at most one turn / resize per 0.35 s: holding the button turned the bridge ~50 times
                   (the hidden presses read as new ones) */
                static s64 s_LastReshape = 0;
                if (NowTick() - s_LastReshape >= MsTicks(350)) {
                    s_LastReshape = NowTick();
                    if (zl && !s_ZlWas) ReshapeStructure(body, true, false);
                    else ReshapeStructure(body, false, true);
                }
            }
        }
        /* A tapped (released within 0.3 s, not grabbed) on a bridge / incline you spawned: a quarter
           turn, like tapping A on furniture (holding A still grabs) */
        {
            static bool s_AWas = false;
            static s64 s_ADown = 0;
            static u32 s_ABody = 0;
            bool aBtn = newest & 1;
            if (aBtn && !s_AWas) {
                u8* nb = cursorMode && !SpawnBusy() ? NearestStructure() : nullptr;
                s_ABody = nb ? *reinterpret_cast<u32*>(nb + 0x230) : 0;
                s_ADown = NowTick();
            }
            if (!aBtn && s_AWas && s_ABody && NowTick() - s_ADown < MsTicks(300)) {
                u8* nb = ActorOfHandle(s_ABody);
                BldComp* cc = nb ? CompOf(nb) : nullptr;
                if (nb && !(cc && cc->grabbed)) ReshapeStructure(nb, true, false);
            }
            if (!aBtn) s_ABody = 0;
            s_AWas = aBtn;
        }
        if (!y) s_Hiding[0] = false;
        if (!zl) s_Hiding[1] = false;
        if (!zr) s_Hiding[2] = false;
        if (s_Hiding[0]) hide |= 1ull << 3;
        if (s_Hiding[1]) hide |= 1ull << 8;
        if (s_Hiding[2]) hide |= 1ull << 9;
        if (hide) for (int i = 0; i < count && i < 16; i++) *reinterpret_cast<u64*>(s + i * 0x28 + 8) &= ~hide;
        s_ZlWas = zl; s_ZrWas = zr;
        s_YWas = y;
        if (!SpawnWithin(0, 400)) return;
        bool feedX = SpawnWithin(60, 110);
        for (int i = 0; i < count && i < 16; i++) {
            u64* b = reinterpret_cast<u64*>(s + i * 0x28 + 8);
            *b &= ~(1ull << 12);                 /* the D-pad left press itself does nothing else */
            if (feedX) *b |= 1ull << 2;          /* X */
        }
    }

    /* The highlight square from the structure itself (sai: "match automatically"): centre = the body
       plus an offset, size per kind (bridges by their length), turned by the record's rotation, written
       over the rect the game made (cells of 5 units: +0x10 x0, +0x14 z0, +0x20 x1, +0x24 z1, inclusive).
       Half sizes / offsets in world units at rotation 0, tunable live (sl::s_Dim): [0] houses / shops,
       [1] bridges (half length = base + per-tile x length), [2] inclines, [3] diagonal bridges. */
    struct FootTune { s32 tx, tz; float dx, dy, dz; };
    FootTune s_Foot[3] = { { 3, 3, -3, 0, -3 }, { 3, 3, -3, 0, -3 }, { 3, 3, 0, 0, 0 } };   /* not static: dbg finds it by symbol */
    struct DimTune { float hx, hz, ox, oz, perTile; };
    DimTune s_Dim[4] = { { 21, 22, 0, -22, 0 }, { 5, 15, 0, 0, 5 }, { 20, 17.5f, 0, -5, 0 }, { 10, 10, 0, 0, 5 } };
    /* Houses / shops: one size per record type (sai's player house and a villager house differ):
       half sizes and the middle's offset from the body, world units, tunable live (sl::s_TypeDim,
       index = record type, 4 floats each). 0 = use s_Dim[0]. */
    struct TypeDim { float hx, hz, ox, oz; };
    /* measured from sai's straight-down screenshots (2026-11-26): player houses, villager houses,
       Nook's Cranny 19, Resident Services 20, Museum 21, Able Sisters 24, campsite 25 */
    TypeDim s_TypeDim[48] = {
        {}, { 26, 21, 0, -5 }, { 26, 21, 0, -5 }, { 26, 21, 0, -5 }, { 26, 21, 0, -5 },
        { 26, 21, 0, -5 }, { 26, 21, 0, -5 }, { 26, 21, 0, -5 }, { 26, 21, 0, -5 },
        { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 },
        { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 },
        { 21.5f, 22, 0, -5 }, { 21.5f, 22, 0, -5 },
        { 34, 29, 0, -5 }, { 29.5f, 28, 0, -5 }, { 36, 29, 0, -5 }, {}, {}, { 24, 24, 0, 5 },   /* Able Sisters: a tile further forward (live on a Switch) */
        { 21, 25, 0, -5 } };
    static s16* RecordOfBody(u8* body) {
        u8* recs = BuildingRecords();
        u32 i = body ? *reinterpret_cast<u32*>(body + ActorRecordIndex) : 99;
        return recs && i < 47 ? reinterpret_cast<s16*>(recs + i * 0x14) : nullptr;
    }
    static bool BridgeLook(u16 id, int* mat, int* shape);
    /* The square's half sizes and middle offset for a body, already turned by its record's rotation. */
    static void SquareDims(u8* body, int fk, float* hxOut, float* hzOut, float* oxOut, float* ozOut) {
        int rot = 0, shape = 0;
        if (s16* r = RecordOfBody(body)) {
            rot = r[3] & 3;
            int m;
            if (fk == 1 && !BridgeLook((u16)r[4], &m, &shape)) shape = 0;
        }
        /* the record's rotation isn't always 0-3 (4 seen on an incline, a bridge across the screen read
           0): the body's own angle (+0x2b4 of its transform, a full turn = 2^32) is what's drawn */
        if (u8* xf = *reinterpret_cast<u8**>(body + ActorTransform)) {
            u32 ang = *reinterpret_cast<u32*>(xf + 0x2b4);
            rot = (int)(((ang + 0x20000000u) >> 30) & 3);
        }
        if (fk == 0) if (s16* r = RecordOfBody(body)) { if (r[0] == 26) fk = 1; else if (r[0] == 27) fk = 2; }
        int dk = fk == 1 && shape >= 3 ? 3 : fk;
        DimTune d = s_Dim[dk];
        if (fk == 0) if (s16* r = RecordOfBody(body)) if (r[0] > 0 && r[0] < 48 && s_TypeDim[r[0]].hx > 0) {
            const TypeDim& t = s_TypeDim[r[0]];
            d.hx = t.hx; d.hz = t.hz; d.ox = t.ox; d.oz = t.oz; d.perTile = 0;
        }
        float len = (float)((shape % 3) + 3);            /* bridges: 3 / 4 / 5 tiles */
        float hx = d.hx + (fk == 1 ? d.perTile * len : 0.0f), hz = d.hz + (dk == 3 ? d.perTile * len : 0.0f);
        float ox = d.ox, oz = d.oz;
        for (int k = 0; k < rot; k++) { float t = hx; hx = hz; hz = t; t = ox; ox = -oz; oz = t; }
        *hxOut = hx; *hzOut = hz; *oxOut = ox; *ozOut = oz;
    }
    /* Square middle minus the body position, rounded so the middle sits on the 5-unit grid. */
    static void SquareOffset(u8* body, float* oxOut, float* ozOut) {
        *oxOut = 0; *ozOut = -5.0f;
        Vec3f sp;
        if (!body || SpawnPosFor(body, &sp)) return;
        int k = StructureOfBody(body);
        float hx, hz, ox, oz;
        SquareDims(body, k == 26 ? 1 : k == 27 ? 2 : 0, &hx, &hz, &ox, &oz);
        const Vec3f* p = reinterpret_cast<const Vec3f*>(body + 0x7f0);
        auto r5 = [](float v) { return (float)(s32)__builtin_floorf(v / 5.0f + 0.5f) * 5.0f; };
        *oxOut = r5(p->x + ox) - p->x;
        *ozOut = r5(p->z + oz) - p->z;
    }
    static void FootOf(u8* body, int fk, s32* txOut, s32* tzOut, Vec3f* pos) {
        const Vec3f* p = reinterpret_cast<const Vec3f*>(body + 0x7f0);
        {   /* which body / record the square is for, once per change (inclines didn't take their size) */
            static u8* s_LastBody = nullptr;
            if (body != s_LastBody) {
                s_LastBody = body;
                s16* rr = RecordOfBody(body);
                u8* xf = *reinterpret_cast<u8**>(body + ActorTransform);
                float lhx, lhz, lox, loz;
                SquareDims(body, fk, &lhx, &lhz, &lox, &loz);
                Log("square: body %p rec %d type %d rot %d ang %x kind %d at %d %d half %d %d off %d %d", body, (int)*reinterpret_cast<u32*>(body + ActorRecordIndex),
                    rr ? rr[0] : -1, rr ? rr[3] : -1, xf ? *reinterpret_cast<u32*>(xf + 0x2b4) : 0, fk, (int)p->x, (int)p->z,
                    (int)lhx, (int)lhz, (int)lox, (int)loz);
            }
        }
        float hx, hz, ox, oz;
        SquareDims(body, fk, &hx, &hz, &ox, &oz);
        /* The game's rect (0x2a85468): W = +0x5b8 + 1 tiles (with +0x5a8 = 0), D = +0x5bc + 1 tiles
           (with +0x5ac = 1), first tile = floor((pos - 5*W + 5) / 10), so its middle is a tile middle
           for odd sizes and a tile line for even ones: no offset can move it less than a tile. So the
           middle wanted is rounded to 5 units and the size whose parity puts the square's middle there
           exactly is used (the nearer of the two, the bigger on a tie). Sizes are not turned by the
           game: hx / hz are already turned above. */
        float cx = p->x + ox, cz = p->z + oz;
        auto pick = [](float half, float& c) -> s32 {
            float raw = half / 5.0f;                       /* tiles */
            s32 c5 = (s32)(c / 5.0f + (c >= 0 ? 0.5f : -0.5f));
            c = c5 * 5.0f;
            bool odd = (c5 & 1) != 0;                      /* middle of a tile -> odd width */
            s32 lo = (s32)raw, hi = lo + 1;
            if (((lo & 1) != 0) != odd) lo--;
            if (((hi & 1) != 0) != odd) hi++;
            if (lo < 1) return hi;
            if (hi > 5) return lo;                         /* the game draws no square wider than 5 tiles (sai's house at 6) */
            return raw - lo < hi - raw ? lo : hi;
        };
        s32 w = pick(hx, cx), dz = pick(hz, cz);
        *txOut = w - 1;
        *tzOut = dz - 1;
        pos->x = cx; pos->z = cz; pos->y += s_Foot[fk].dy;
    }
    u32 BuildingAtCell(const u8* query, u8* out, u8* minOut) {
        s_CursorQueryTick = NowTick();
        float cx = *reinterpret_cast<const s32*>(query + 8) * 5.0f + 2.5f;
        float cz = *reinterpret_cast<const s32*>(query + 0xc) * 5.0f + 2.5f;
        u8* body = nullptr;
        /* D-pad left just pressed: always the borrowed house (a bridge next to the cursor got the menu,
           which then wasn't a spawn menu and the pick was dropped - Switch video) */
        bool spawning = s_SpawnArmed && SpawnWithin(0, 300);
        if (!spawning) ForEachActor([&](u8* a) {
            if (body || VtOf(a) != BuildingBodyVt || IsBridgeBody(a)) return;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + 0x7f0);
            if (cx >= p->x - 20 && cx < p->x + 20 && cz >= p->z - 25 && cz < p->z + 25) body = a;
        });
        if (!body && !spawning) {   /* bridges and inclines (building bodies too): near their middle */
            float best = 1e9f;
            ForEachActor([&](u8* a) {
                if (VtOf(a) != BuildingBodyVt) return;
                int kind = StructureOfBody(a);
                if (!kind) return;
                float reach = kind == 26 ? BridgeReach : InclineReach;
                const Vec3f* p = reinterpret_cast<const Vec3f*>(a + 0x7f0);
                float d = (p->x - cx) * (p->x - cx) + (p->z - cz) * (p->z - cz);
                if (d < reach * reach && d < best) { best = d; body = a; }
            });
        }
        s_HoverHandle = body && StructureOfBody(body) ? *reinterpret_cast<u32*>(body + 0x230) : 0;
        if (!body && spawning) {   /* spawn: borrow any house to open the menu on */
            ForEachActor([&](u8* a) {
                if (!body && VtOf(a) == BuildingBodyVt && !IsBridgeBody(a) && !StructureOfBody(a) && !RemovableBuildingOfBody(a)) body = a;
            });
            s_SpawnBody = body;
        }
        if (!body) return 0;
        BldComp* c = AttachComp(body);
        if (!c) return 0;
        /* the borrowed house's footprint (4x4 tiles) showed as a big square at the cursor for a moment
           when D-pad left was pressed (Switch video): one tile while it's only borrowed */
        /* Footprint (last tile index in x / z) and the square's offset per kind, tunable live with dbg
           poke (sl::s_Foot): [0] houses / shops, [1] bridges, [2] inclines. */
        int fk = StructureOfBody(body) == 26 ? 1 : StructureOfBody(body) == 27 ? 2 : 0;   /* 26 bridge, 27 incline */
        bool borrowed = body == s_SpawnBody && (s_SpawnArmed || s_SpawnMode);
        /* The square the editor draws is the footprint (owner +0x5b8 / +0x5bc: last TILE index, 10
           units each, before turning) placed at the position given below, turned by the transform's
           rotation. (Writing the cell rect afterwards changed nothing on screen.) Size and middle come
           from FootOf (per kind / record type, measured from screenshots). */
        Vec3f fp = *BldPos(c);
        if (borrowed) {
            *reinterpret_cast<s32*>(c->owner + 0x5b8) = 0;
            *reinterpret_cast<s32*>(c->owner + 0x5bc) = 0;
        } else {
            s32 tx, tz;
            FootOf(body, fk, &tx, &tz, &fp);
            *reinterpret_cast<s32*>(c->owner + 0x5a8) = 0;   /* see FootOf: W = tx + 1, D = tz + 1 */
            *reinterpret_cast<s32*>(c->owner + 0x5ac) = 1;
            *reinterpret_cast<s32*>(c->owner + 0x5b8) = tx;
            *reinterpret_cast<s32*>(c->owner + 0x5bc) = tz;
        }
        struct { uintptr_t vt; Vec3f pos; u32 rot; u32 zero; } at = {
            *reinterpret_cast<uintptr_t*>(Main(PosVtableGot)) + 0x10, fp,
            *reinterpret_cast<u32*>(*reinterpret_cast<u8**>(body + ActorTransform) + 0x2b4), 0 };
        using RectFn = void (*)(BldComp*, u8*, void*);
        if (out) reinterpret_cast<RectFn>(Main(BldRectFull))(c, out, &at);
        if (minOut) reinterpret_cast<RectFn>(Main(BldRectMin))(c, minOut, &at);
        return *reinterpret_cast<u32*>(body + 0x230);
    }

    /* 0x13f1bc4 on the main island: hover of kind 4 on a building with our component. */
    bool BuildingHover(u8* query, const u32* handle, const u8* rect) {
        u8* body = ActorOfHandle(handle ? *handle : 0);
        if (!CompOf(body) || !query || !rect) return false;
        s32 x0 = *reinterpret_cast<const s32*>(rect + 0x10), z0 = *reinterpret_cast<const s32*>(rect + 0x14);
        s32 x1 = *reinterpret_cast<const s32*>(rect + 0x20), z1 = *reinterpret_cast<const s32*>(rect + 0x24);
        u32 rot = *reinterpret_cast<u32*>(*reinterpret_cast<u8**>(body + ActorTransform) + 0x2b4);
        u32 item[2] = { 0xfffe, 0 };
        reinterpret_cast<void (*)(u32*)>(Main(ItemNoneInit))(item);
        *reinterpret_cast<float*>(query + 0x28) = (x0 * 5.0f + x1 * 5.0f + 5.0f) * 0.5f;
        /* height of the highlight: was 0 (under the cliff-level ground at 15); the body's height +
           s_SquareDy (live-tunable) - sai: the square drew over the building */
        *reinterpret_cast<float*>(query + 0x2c) = reinterpret_cast<const Vec3f*>(body + 0x7f0)->y + s_SquareDy;
        *reinterpret_cast<float*>(query + 0x30) = (z0 * 5.0f + z1 * 5.0f + 5.0f) * 0.5f;
        *reinterpret_cast<u32*>(query + 0x34) = rot;
        *reinterpret_cast<u32*>(query + 0x38) = 0;
        *reinterpret_cast<u32*>(query + 0x60) = item[0];
        *reinterpret_cast<u32*>(query + 0x64) = item[1];
        *reinterpret_cast<u32*>(query + 0x8c) = *handle;
        *reinterpret_cast<u32*>(query + 0x98) = 2;
        *reinterpret_cast<u32*>(query + 0xa0) = *handle;
        *reinterpret_cast<u32*>(query + 0x18) = 4;
        return true;
    }

    /* ---- Island editor: house exteriors through HHP's exterior menu ----
       The menu (X on a hovered house) reads the house's design with 0x2a914b0(out, actor handle)
       and applies each choice with 0x2a91660(actor handle, design); both only accept HHP's own
       house, so on island houses the menu showed nothing and every choice failed ("There isn't
       enough space here..."). For an island house body the module answers them from the house's
       save data instead: design = { u16 style, u16 exterior, u16 roof, u16 door, 8-byte door
       decoration }, the same ids the island's house builders read (player houses: save
       +0x119228 + i*0xfa08 -> ids +4, copy +0xa, decoration +0x289fc; villager houses:
       +0x1962d8 + i*0x10d8 -> ids +8, copy +0xe, decoration +0x1c8). Each choice then rebuilds
       the house right away the way the island builds it at load: its body / wall / roof / door
       actors are deleted and the island's building spawner (0x2975a38 on field + 0x1e08, fed the
       building's record) creates them again from the save. The menu keeps the old actor handle,
       so old -> new handles are remembered. */
    constexpr u32 PlayerHouseOfKind  = 0x2a673a0;  /* (int* index, kind) -> found */
    constexpr u32 VillagerHouseOfKind = 0x2a67620;
    constexpr u32 BodyParamKind      = 0x288;      /* body: structure param at +0x280 (vt), kind at +8 */
    constexpr u32 StyleField         = 0xb6f501d2; /* exterior param column holding the style */

    struct HouseExt { u16* ids; u16* ids2; u8* deco; };

    constexpr u32 LandGlobal         = 0x509d040;  /* +0x38: main island land, +0x148+8 building list */
    constexpr u32 BuildingRecordGet  = 0x1260490;  /* (list, index) -> record, returned through x8 */
    struct BuildingRecord { alignas(16) u8 raw[0x40]; };   /* game's is 0x28 bytes */
    constexpr u32 BuildingSpawner    = 0x1e08;     /* field + 0x1e08: island building spawner */
    constexpr u32 BuildingSpawn      = 0x2975a38;  /* (spawner, record, index) -> body handle */
    constexpr u32 ActorRequestDelete = 0x5fb3d0;   /* (actor) */
    constexpr int MaxHandleRemaps    = 16;
    static u32 s_RemapFrom[MaxHandleRemaps], s_RemapTo[MaxHandleRemaps];
    static int s_RemapNext = 0;

    static u8* SaveMain() {
        u8* d = *reinterpret_cast<u8**>(Main(SaveRootGlobal));
        if (!d || (d[0x453] & 1)) return nullptr;
        u8** p3 = *reinterpret_cast<u8***>(d + 0x10);
        u8* x = p3 ? *p3 : nullptr;
        u8* data = x ? *reinterpret_cast<u8**>(x + 0x20) : nullptr;
        return data ? data + 0x70 : nullptr;
    }

    static u8* SaveView(u8* view) { return *reinterpret_cast<u8**>(view + 8) + *reinterpret_cast<u64*>(view + 0x10); }

    static bool HouseExtOf(u8* body, HouseExt* out) {
        u8* S = SaveMain();
        if (!S || !body) return false;
        s32 kind = *reinterpret_cast<s32*>(body + BodyParamKind);
        int idx = 0;
        using IdxFn = u64 (*)(int*, int);
        if ((reinterpret_cast<IdxFn>(Main(VillagerHouseOfKind))(&idx, kind) & 1) && idx >= 0 && idx < 10) {
            u8* R = SaveView(S + 0x1962d8 + idx * 0x10d8);
            *out = { reinterpret_cast<u16*>(R + 8), reinterpret_cast<u16*>(R + 0xe), R + 0x1c8 };
            return true;
        }
        if ((reinterpret_cast<IdxFn>(Main(PlayerHouseOfKind))(&idx, kind) & 1) && idx >= 0 && idx < 8) {
            u8* R = SaveView(S + 0x119228 + idx * 0xfa08);
            *out = { reinterpret_cast<u16*>(R + 4), reinterpret_cast<u16*>(R + 0xa), R + 0x289fc };
            return true;
        }
        return false;
    }

    /* Exterior param row of an id -> its style column (as 0x2a83270 does for HHP's house). */
    static u16 StyleOf(u16 id) {
        u8* obj = *reinterpret_cast<u8**>(Main(FieldMgrGlobal));
        if (!obj) return 0;
        u32 r = (u32)id - *reinterpret_cast<u16*>(obj + 0x9f0);
        if (r >= *reinterpret_cast<u32*>(obj + 0x9d0)) return 0;
        u32* row = *reinterpret_cast<u32**>(*reinterpret_cast<u64*>(obj + 0x9d8) + (u64)r * 8);
        if (!row || !*row) return 0;
        u8* hdr = reinterpret_cast<u8*>(row) - *row;
        const s32* f = reinterpret_cast<const s32*>(hdr + (hdr[10] ? 0x1c : 0xc));
        u16 n = *reinterpret_cast<u16*>(hdr + 8);
        for (u16 i = 0; i < n; i++, f += 2)
            if ((u32)f[0] == StyleField) return *reinterpret_cast<u16*>(reinterpret_cast<u8*>(row) + (f[1] == -1 ? 0 : f[1]));
        return 0;
    }

    static u8* HouseBodyOfHandle(u32 h) {
        for (int n = 0; n < MaxHandleRemaps && h && !ActorOfHandle(h); n++) {   /* respawned house */
            u32 to = 0;
            for (int i = 0; i < MaxHandleRemaps; i++) if (s_RemapFrom[i] == h) to = s_RemapTo[i];
            if (!to) break;
            h = to;
        }
        u8* a = ActorOfHandle(h);
        return a && VtOf(a) == BuildingBodyVt ? a : nullptr;
    }

    /* Rebuild a building from its record and save data: the island's spawner creates new actors
       and the old ones (body + wall / roof / door / decoration) stay until the new body has its
       parts, so the house never blinks out. Choices made meanwhile are folded into one more
       rebuild once this one is done. */
    constexpr int MaxRespawns = 4;
    struct Respawn { u32 idx; u32 newBody; u32 old[MaxDragParts + 1]; int oldCount; int frames; bool dirty; };
    static Respawn s_Respawns[MaxRespawns] = { { 0xffffffff }, { 0xffffffff }, { 0xffffffff }, { 0xffffffff } };
    static bool RespawnPending(u8* body) {
        if (!body) return false;
        u32 idx = *reinterpret_cast<u32*>(body + ActorRecordIndex);
        for (const Respawn& r : s_Respawns) if (r.idx != 0xffffffff && r.idx == idx) return true;
        return false;
    }

    static bool HouseNear(u8* a, const Vec3f& b, u32* pairOut) {
        u32 pair = PartPair(a);
        if (!pair || VtOf(a) == SignVt) return false;
        const Vec3f* p = reinterpret_cast<const Vec3f*>(a + pair);
        if (p->x - b.x < -8 || p->x - b.x > 8 || p->z - b.z < -8 || p->z - b.z > 8) return false;
        if (pairOut) *pairOut = pair;
        return true;
    }

    static void StartRespawn(Respawn& r, u8* body) {
        u8* field = *reinterpret_cast<u8**>(Main(FieldMgrGlobal));
        u8* land = *reinterpret_cast<u8**>(Main(LandGlobal));
        u8* island = land ? *reinterpret_cast<u8**>(land + 0x38) : nullptr;
        u8* list = island ? *reinterpret_cast<u8**>(island + 0x148 + 8) : nullptr;
        if (!field || !list) { r.idx = 0xffffffff; return; }
        r.idx = *reinterpret_cast<u32*>(body + ActorRecordIndex);
        r.oldCount = 0; r.frames = 0; r.dirty = false;
        const Vec3f b = *reinterpret_cast<const Vec3f*>(body + 0x7f0);
        ForEachActor([&](u8* a) {
            if (r.oldCount <= MaxDragParts && (a == body || HouseNear(a, b, nullptr)))
                r.old[r.oldCount++] = *reinterpret_cast<u32*>(a + 0x230);
        });
        u32 oldHandle = *reinterpret_cast<u32*>(body + 0x230);
        BuildingRecord record = reinterpret_cast<BuildingRecord (*)(u8*, u32)>(Main(BuildingRecordGet))(list, r.idx);
        r.newBody = reinterpret_cast<u32 (*)(u8*, u8*, u32, u64, u64, u64, u64, u64)>(Main(BuildingSpawn))(field + BuildingSpawner, record.raw, r.idx, 0, 0, 0, 0, 0);
        s_RemapFrom[s_RemapNext] = oldHandle; s_RemapTo[s_RemapNext] = r.newBody;
        s_RemapNext = (s_RemapNext + 1) % MaxHandleRemaps;
        Log("buildings: building %d rebuilt (%x -> %x, %d old actors)", r.idx, oldHandle, r.newBody, r.oldCount);
    }

    static void RespawnHouse(u8* body) {
        if (!body) return;
        u32 idx = *reinterpret_cast<u32*>(body + ActorRecordIndex);
        Respawn* free = nullptr;
        for (Respawn& r : s_Respawns) {
            if (r.idx == idx) { r.dirty = true; return; }   /* one already running: redo after it */
            if (r.idx == 0xffffffff && !free) free = &r;
        }
        if (free) StartRespawn(*free, body);
    }

    /* ---- Unlock everything (switch "unlock_all") ----
       Once per session, when the island is loaded, the progress flags behind the phone apps,
       the Island Designer app and its permits, and Happy Home Paradise's tools are written
       into the save with the game's own setters, as if earned: player flags for every resident
       (0x264bfe8: player index, name, value), HHP flags for the island (0x2658bb8: name, value).
       Names are passed as sead::SafeString { vtable, c string }. HHP's facility / NPC story
       flags are left alone: forcing them without the buildings breaks its story. */
    constexpr u32 SafeStringVtGot   = 0x4ab2020;   /* GOT: sead::SafeString vtable base (+0x10) */
    constexpr u32 PlayerMgrGlobal   = 0x54021a0;
    constexpr u32 PlayerOfIndex     = 0x24830c0;   /* (mgr, u8* index, 1) -> player or null */
    constexpr u32 PlayerFlagSet     = 0x264bfe8;   /* (u8* index, name, value) */
    constexpr u32 HhpFlagSet        = 0x2658bb8;   /* (name, value) */
    static const char* const k_UnlockPlayerFlags[] = {
        "MainmenuEncyclopedia", "MainmenuMap", "MainmenuMydesign", "MainmenuBank", "MainmenuRecipe",
        "MainmenuRecipe_v2", "MainmenuCall", "MainmenuFriendList", "MainmenuOnlineShopping",
        "MainmenuRndDailyQuest", "MainmenuAchievement", "MainmenuChatLog", "MainmenuTips",
        "MainmenuMydesignShowcase", "MainmenuMydesignPatternPlus", "MainmenuCamera1stPersonView",
        "MainmenuUnlockMydesingPro", "UnlockMyDesignProCategory", "UnlockMydesignPro2",
        "MainmenuConstructLicense", "GetLicenseRiver", "GetLicenseCliff", "GetLicenseGrdStone",
        "GetLicenseGrdBrick", "GetLicenseGrdDarkSoil", "GetLicenseGrdStonePattern", "GetLicenseGrdSand",
        "GetLicenseGrdTile", "GetLicenseGrdWood", "GetLicenseGrdMydesign",
        "UnlockWherearen", "WHEREAREN_Unlock_AccentWall", "WHEREAREN_Unlock_ResortPlannerApp",
        "WHEREAREN_Unlock_ResortStudyApp", "WHEREAREN_Unlock_CoordinateNpcHouse",
        "WHEREAREN_Unlock_ResortPlannerAppNetwork", "WHEREAREN_Unlock_ResortPlannerAppRevisit",
        "WHEREAREN_Unlock_RoomDimming", "WHEREAREN_Unlock_DustCloth_01", "WHEREAREN_Unlock_DustCloth_02",
        "WHEREAREN_Unlock_EnvSound_01", "WHEREAREN_Unlock_EnvSound_02", "WHEREAREN_Unlock_EnvSound_03",
        "WHEREAREN_Unlock_RoomSizeModeUnlock",
    };
    static const char* const k_UnlockHhpFlags[] = {
        "Unlock_Kabetasu", "Unlock_ShareHouse", "Unlock_Extension", "Unlock_MargeCatalog_DIY",
        "Unlock_MargeCatalog_General", "Unlock_ThemeFreeEdit", "Unlock_HousePhoto", "Unlock_CreateRoad",
        "Unlock_Insect", "Unlock_FlowerCatalog", "Unlock_MessageBottle", "Unlock_PillarCatalog_01",
        "Unlock_PillarCatalog_02", "Unlock_PillarCatalog_Brick", "Unlock_PillarCatalog_Concrete",
        "Unlock_PillarCatalog_Steel", "Unlock_PillarCatalog_Plain", "Unlock_PillarCatalog_Marble",
        "Unlock_Exchange", "Unlock_Donation", "Unlock_InGameSNS", "Unlock_Amiibo", "Unlock_MncBox",
        "Unlock_WorkingTable", "Unlock_MncBridgeBoat", "Unlock_ManCatalogShop", "Unlock_ManDeferredPayment",
        "Unlock_Party1", "Unlock_Party2",
    };
    struct SafeStr { uintptr_t vt; const char* str; };
    static bool s_UnlockDone = false;

    static void UnlockTick() {
        if (s_UnlockDone || !FeatureOn("unlock_all")) return;
        const s16* stage = *reinterpret_cast<s16* const*>(Main(StageIdGot));
        if (!stage || *stage != MainFieldStage) return;
        u8* players = *reinterpret_cast<u8**>(Main(PlayerMgrGlobal));
        if (!players || !SaveMain()) return;
        s_UnlockDone = true;
        SafeStr name = { *reinterpret_cast<uintptr_t*>(Main(SafeStringVtGot)) + 0x10, nullptr };
        using ExistsFn = void* (*)(u8*, u8*, u32);
        using PlayerSetFn = void (*)(u8*, SafeStr*, u32);
        using HhpSetFn = void (*)(SafeStr*, u32);
        int residents = 0;
        for (u8 i = 0; i < 8; i++) {
            u8 idx = i;
            if (!reinterpret_cast<ExistsFn>(Main(PlayerOfIndex))(players, &idx, 1)) continue;
            residents++;
            for (const char* f : k_UnlockPlayerFlags) {
                name.str = f;
                idx = i;
                reinterpret_cast<PlayerSetFn>(Main(PlayerFlagSet))(&idx, &name, 1);
            }
        }
        for (const char* f : k_UnlockHhpFlags) {
            name.str = f;
            reinterpret_cast<HhpSetFn>(Main(HhpFlagSet))(&name, 1);
        }
        Log("unlock_all: %d player flags for %d residents, %d HHP flags",
            (int)(sizeof(k_UnlockPlayerFlags) / sizeof(k_UnlockPlayerFlags[0])), residents,
            (int)(sizeof(k_UnlockHhpFlags) / sizeof(k_UnlockHhpFlags[0])));
    }

    /* ---- Stacked cliffs with the cliff tool ----
       At a flush edge (a tile at your level whose other side drops a level) the cliff tool refuses
       to build. When it refuses on the main island, the tile in front is raised one level (or
       lowered, if it is one level above you), so a cliff can sit right on the edge of another,
       the way a save editor stacks them. The change goes through the game's own instant
       terrain update (what a normal build runs): 0x11ee4e0 writes a 3 x 3 block of tiles and marks
       it, then 0x11ee6e0 (cliff models), 0x11eeba8 and 0x11eed50 (collision) refresh it.
       Terrain grid: in the main save at +0x116c50, tiles at grid[1] + grid[2], 112 x 96 tiles of
       14 bytes, index x * 96 + z, tile x = world x / 10 - 16; a tile is { u16 model, u16 variation,
       u8 angle, u16 road model, u16 road variation, u8 road angle, u8 elevation @+12 }. Edge pieces
       depend on which of the 8 neighbours are lower (learned from the island's own cliffs). */
    constexpr u32 TerrainGrid      = 0x116c50;
    constexpr u32 FieldLandGot     = 0x4ab27a8;   /* -> field land manager; +0x1f8 -> +0xa8 terrain */
    constexpr u32 TerrainApply     = 0x11ee4e0;
    constexpr u32 TerrainModels    = 0x11ee6e0;
    constexpr u32 TerrainRefresh   = 0x11eeba8;
    constexpr u32 TerrainCollision = 0x11eed50;
    constexpr int TerrainW = 112, TerrainH = 96, TileSize = 14, TileElev = 12;
    constexpr int TerrainMaxLevel  = 3;
    constexpr u32 StackCooldown    = 90;          /* frames: one swing asks several times */
    struct EdgePiece { u8 model, angle; };
    /* neighbour "lower" mask (bit 0 N, 1 NE, 2 E, ... 7 NW; north = -z) -> piece; 0xff = use EdgeFallback */
    static const EdgePiece k_EdgePieces[256] = {
#include "edge_table.inc"
    };
    struct TerrainBlock { s16 cx, cz, x0, z0, x1, z1; u8 tiles[9][TileSize]; };
    struct TerrainApplyArgs { u8* terrain; u8* objA; u8* objB; TerrainBlock* block; u32 mode; u8 animate, removing; u8 pad[2]; };
    static TerrainBlock s_StackBlock;
    static int s_StackX = -1, s_StackZ = -1, s_StackDelta = 0;
    static bool s_StackPending = false;
    static u32 s_Frame = 0, s_StackFrame = 0;

    static u8* TerrainTiles() {
        u8* S = SaveMain();
        if (!S) return nullptr;
        u8* grid = S + TerrainGrid;
        u8* base = *reinterpret_cast<u8**>(grid + 8);
        return base ? base + *reinterpret_cast<u64*>(grid + 0x10) : nullptr;
    }

    static bool CliffModel(u16 m) {
        switch (m) { case 0: case 22: case 25: case 28: case 29: case 31: case 32: case 33: case 34: case 37: case 40: return true; }
        return false;
    }

    static EdgePiece EdgeFallback(u8 mask) {
        bool n = mask & 1, e = mask & 4, s = mask & 0x10, w = mask & 0x40;
        int sides = n + e + s + w;
        if (sides == 0) {
            if (mask & 2) return { 40, 2 };
            if (mask & 8) return { 40, 1 };
            if (mask & 0x20) return { 40, 0 };
            if (mask & 0x80) return { 40, 3 };
            return { 0, 0 };
        }
        if (sides == 1) return { 37, (u8)(w ? 0 : s ? 1 : e ? 2 : 3) };
        if (sides == 2) {
            if (n && s) return { 28, 1 };
            if (e && w) return { 28, 0 };
            return { 31, (u8)(n && e ? 3 : e && s ? 2 : s && w ? 1 : 0) };
        }
        if (sides == 3) return { 25, (u8)(!w ? 3 : !e ? 1 : !n ? 2 : 0) };
        return { 22, 0 };   /* lone tile, as a normal build makes it */
    }

    /* After the cliff tool's land decision (0x21e37a0): action 0 = create, 1 = remove; anything
       else, or result 1, is a refusal. On a refusal the tile the tool aims at (player +0x764 / +0x76c)
       is stacked when it sits on an edge: raised if it is at your level, lowered if one above. */
    constexpr u32 DecisionAction = 0x28;
    constexpr u32 PlayerToolTarget = 0x764;      /* Vec3f the cliff tool aims at */

    void CliffDecided(u8* obj, u32 result) {
        if (!FeatureOn("cliff_stack") || !EditorMapStage() || !s_LocalPlayer || !obj) return;
        u32 action = *reinterpret_cast<u32*>(obj + DecisionAction);
        if (action < 2 && result != 1) return;
        if (s_StackPending || s_Frame - s_StackFrame < StackCooldown) return;
        u8* t = TerrainTiles();
        if (!t) return;
        const float* target = reinterpret_cast<const float*>(s_LocalPlayer + PlayerToolTarget);
        int gx = (int)(target[0] / 10.0f) - 16, gz = (int)(target[2] / 10.0f) - 16;
        const Vec3f* pp = reinterpret_cast<const Vec3f*>(s_LocalPlayer + PlayerPos);
        int px = (int)(pp->x / 10.0f) - 16, pz = (int)(pp->z / 10.0f) - 16;
        if (gx < 1 || gz < 1 || gx >= TerrainW - 1 || gz >= TerrainH - 1 || px < 0 || pz < 0 || px >= TerrainW || pz >= TerrainH) return;
        if (gx == px && gz == pz) return;
        int te = t[(gx * TerrainH + gz) * TileSize + TileElev], pe = t[(px * TerrainH + pz) * TileSize + TileElev];
        int delta = te == pe ? 1 : te == pe + 1 ? -1 : 0;
        if (!delta || te + delta > TerrainMaxLevel || te + delta < 0) return;
        if (!CliffModel(*reinterpret_cast<u16*>(t + (gx * TerrainH + gz) * TileSize))) return;
        /* only on an edge: some neighbour of the target is lower than it */
        bool edge = false;
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++)
                if (t[((gx + dx) * TerrainH + gz + dz) * TileSize + TileElev] < te) edge = true;
        if (!edge) return;
        s_StackX = gx; s_StackZ = gz; s_StackDelta = delta;
        s_StackPending = true; s_StackFrame = s_Frame;
    }

    static bool s_WallsOn = false;   /* lower wall pieces wanted (read by the render threads) */

    /* Next frame: build the 3 x 3 block and run the game's terrain update on it. */
    static void CliffStackTick() {
        s_Frame++;
        s_WallsOn = FeatureOn("cliff_stack") && EditorMapStage();
        if (!s_StackPending) return;
        s_StackPending = false;
        u8* t = TerrainTiles();
        u8* land = *reinterpret_cast<u8**>(Main(FieldLandGot));
        land = land ? *reinterpret_cast<u8**>(land) : nullptr;
        u8* mid = land ? *reinterpret_cast<u8**>(land + 0x1f8) : nullptr;
        u8* terrain = mid ? *reinterpret_cast<u8**>(mid + 0xa8) : nullptr;
        if (!t || !terrain) return;
        int gx = s_StackX, gz = s_StackZ;
        auto tile = [&](int x, int z) { return t + (x * TerrainH + z) * TileSize; };
        int newElev = tile(gx, gz)[TileElev] + s_StackDelta;
        auto elev = [&](int x, int z) -> int {
            if (x < 0 || z < 0 || x >= TerrainW || z >= TerrainH) return 0;
            return (x == gx && z == gz) ? newElev : tile(x, z)[TileElev];
        };
        static const int nx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, nz[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        TerrainBlock& b = s_StackBlock;
        b.cx = gx + 16; b.cz = gz + 16; b.x0 = gx + 15; b.z0 = gz + 15; b.x1 = gx + 17; b.z1 = gz + 17;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                int x = gx + dx, z = gz + dz;
                u8* o = b.tiles[(dz + 1) * 3 + dx + 1];
                memcpy(o, tile(x, z), TileSize);
                u16* model = reinterpret_cast<u16*>(o);
                if (!CliffModel(*model)) continue;   /* river / pond tiles stay as they are */
                int h = elev(x, z);
                o[TileElev] = (u8)h;
                EdgePiece pc = { 0, 0 };
                if (h > 0) {
                    u8 mask = 0;
                    for (int i = 0; i < 8; i++) if (elev(x + nx[i], z + nz[i]) < h) mask |= 1 << i;
                    pc = k_EdgePieces[mask].model == 0xff ? EdgeFallback(mask) : k_EdgePieces[mask];
                }
                if (*model != pc.model) model[1] = 0;
                *model = pc.model;
                o[4] = pc.angle;
            }
        }
        TerrainApplyArgs a = { terrain, terrain + 0x778, *reinterpret_cast<u8**>(terrain + 0x58), &b, 1, 1, (u8)(s_StackDelta < 0), {} };
        using StepFn = u64 (*)(u64, TerrainApplyArgs*);
        u64 r = reinterpret_cast<StepFn>(Main(TerrainApply))(1, &a);
        if (r & 1) r = reinterpret_cast<StepFn>(Main(TerrainModels))(r, &a);
        if (r & 1) r = reinterpret_cast<StepFn>(Main(TerrainRefresh))(r, &a);
        if (r & 1) r = reinterpret_cast<StepFn>(Main(TerrainCollision))(r, &a);
        Log("cliff stack: tile (%d,%d) %s to level %d (%s)", gx, gz, s_StackDelta > 0 ? "raised" : "lowered", newElev,
            (r & 1) ? "applied" : "update stopped early");
    }

    /* Next to a stacked cliff the game's own build / removal can give a raised tile the flat piece
       (model 0) although a neighbour is now lower: that side then has no wall and shows a hole.
       Every terrain change (the game's and ours) goes through 0x11ee4e0 with its 3 x 3 block, so
       before it is applied such tiles get the edge piece their lower neighbours call for. */
    void FixTerrainBlock(TerrainApplyArgs* a) {
        if (!a || !a->block || a->mode != 1 || !s_WallsOn) return;
        u8* t = TerrainTiles();
        if (!t) return;
        TerrainBlock& b = *a->block;
        int x0 = (b.x0 < b.x1 ? b.x0 : b.x1) - 16, z0 = (b.z0 < b.z1 ? b.z0 : b.z1) - 16;
        auto elev = [&](int x, int z) -> int {
            int bx = x - x0, bz = z - z0;
            if (bx >= 0 && bx < 3 && bz >= 0 && bz < 3) return b.tiles[bz * 3 + bx][TileElev];
            if (x < 0 || z < 0 || x >= TerrainW || z >= TerrainH) return 0;
            return t[(x * TerrainH + z) * TileSize + TileElev];
        };
        static const int nx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, nz[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        int fixed = 0;
        for (int k = 0; k < 9; k++) {
            u8* o = b.tiles[k];
            u16* model = reinterpret_cast<u16*>(o);
            int h = o[TileElev];
            if (*model != 0 || h == 0) continue;
            int x = x0 + k % 3, z = z0 + k / 3;
            u8 mask = 0;
            for (int i = 0; i < 8; i++) if (elev(x + nx[i], z + nz[i]) < h) mask |= 1 << i;
            if (!mask) continue;
            EdgePiece pc = k_EdgePieces[mask].model == 0xff ? EdgeFallback(mask) : k_EdgePieces[mask];
            *model = pc.model; model[1] = 0; o[4] = pc.angle;
            fixed++;
        }
        if (fixed) Log("cliff stack: gave %d tile(s) by (%d,%d) their missing cliff wall", fixed, x0 + 1, z0 + 1);
    }

    /* Each cliff piece model is one level (15 units) tall, so a tile two or more levels above a
       neighbour leaves a see-through band under its wall. Every frame each visible tile's pieces are
       drawn with 0x146ab80(model mgr, model, 3x4 transform, ...) (through 0x146aa88; the change
       builder 0x1211890 calls it too); each call adds an instance, so more pieces can be drawn under
       the tile, one per missing level. Each lower level gets the piece its own neighbours call for
       (at level L only neighbours below L need a wall there), so it meets the cliffs beside it; the
       game's model for a piece kind is learnt from the normal cliffs it draws (any variation will
       do). Every piece is placed at its tile centre, the cliff piece at exactly elevation * 15 (the
       path / top layer 0.05 higher), rotated by angle * 90 degrees. Runs on the render worker
       threads, about 2000 times a frame, so the switch and stage checks come from the frame tick. */
    constexpr float LevelHeight = 15.0f;
    constexpr int MaxExtraWalls = 3;
    constexpr int PieceKinds    = 41;
    static u8* s_PieceModel[PieceKinds][4];   /* tile model id, variation -> the game's model */

    struct LowerPiece { u8* model; float xf[12]; };

    /* Fills out[] with the pieces to draw under this one; returns how many. */
    int CliffLowerPieces(u8* model, const float* xf, LowerPiece* out) {
        if (!xf || !model || !s_WallsOn) return 0;
        float fx = xf[3] - (int)(xf[3] / 10.0f) * 10.0f, fz = xf[11] - (int)(xf[11] / 10.0f) * 10.0f;
        if (fx < 4.9f || fx > 5.1f || fz < 4.9f || fz > 5.1f) return 0;   /* not at a tile centre */
        int gx = (int)(xf[3] / 10.0f) - 16, gz = (int)(xf[11] / 10.0f) - 16;
        if (gx < 0 || gz < 0 || gx >= TerrainW || gz >= TerrainH) return 0;
        u8* t = TerrainTiles();
        if (!t) return 0;
        const u8* p = t + (gx * TerrainH + gz) * TileSize;
        int h = p[TileElev];
        u16 kind = *reinterpret_cast<const u16*>(p), var = reinterpret_cast<const u16*>(p)[1] & 3;
        if (h < 1 || kind == 0 || kind >= PieceKinds || !CliffModel(kind)) return 0;
        float dy = xf[7] - h * LevelHeight;
        if (dy < -0.02f || dy > 0.02f) return 0;   /* only the cliff layer */
        s_PieceModel[kind][var] = model;
        if (h < 2) return 0;
        auto elev = [&](int x, int z) -> int {
            return (x < 0 || z < 0 || x >= TerrainW || z >= TerrainH) ? 0 : t[(x * TerrainH + z) * TileSize + TileElev];
        };
        static const int nx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, nz[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        static const float rc[4] = { 1, 0, -1, 0 }, rs[4] = { 0, 1, 0, -1 };   /* cos, sin of angle * 90 */
        int n = 0;
        for (int level = h - 1; level >= 1 && level > h - 1 - MaxExtraWalls; level--) {
            u8 mask = 0;
            for (int i = 0; i < 8; i++) if (elev(gx + nx[i], gz + nz[i]) < level) mask |= 1 << i;
            if (!mask) break;   /* nothing lower at this level, nor below */
            EdgePiece pc = k_EdgePieces[mask].model == 0xff ? EdgeFallback(mask) : k_EdgePieces[mask];
            u8* m = nullptr;
            if (pc.model < PieceKinds)
                for (int v = 0; v < 4 && !m; v++) m = s_PieceModel[pc.model][(var + v) & 3];
            LowerPiece& o = out[n++];
            memcpy(o.xf, xf, sizeof(o.xf));
            o.xf[7] = level * LevelHeight + dy;
            if (m) {
                int a = pc.angle & 3;
                o.model = m;
                o.xf[0] = rc[a]; o.xf[1] = 0; o.xf[2] = rs[a];
                o.xf[4] = 0;     o.xf[5] = 1; o.xf[6] = 0;
                o.xf[8] = -rs[a]; o.xf[9] = 0; o.xf[10] = rc[a];
            } else {
                o.model = model;   /* piece not seen yet: the top one again */
            }
        }
        return n;
    }

    /* Every frame: finish rebuilds whose new body has spawned its parts. */
    /* A dropped bridge / incline lands with a little bounce, like furniture: once its rebuilt body is
       there, its height follows these offsets for a few frames. */
    static u32 s_BounceH = 0;
    static int s_BounceF = -1;
    static float s_BounceBase = 0.0f;
    static const float k_Bounce[] = { 2.0f, 3.5f, 4.0f, 3.5f, 2.2f, 0.6f, -0.8f, -0.6f, 0.3f, 0.6f, 0.2f, 0.0f };
    static void BounceTick() {
        if (s_BounceF < 0) return;
        u8* b = ActorOfHandle(s_BounceH);
        if (!b) { s_BounceF = -1; return; }
        if (s_BounceF == 0) s_BounceBase = reinterpret_cast<Vec3f*>(b + 0x7f0)->y;
        float y = s_BounceBase + k_Bounce[s_BounceF];
        reinterpret_cast<Vec3f*>(b + 0x7f0)->y = y;
        reinterpret_cast<Vec3f*>(b + 0x7fc)->y = y;
        if (++s_BounceF >= (int)(sizeof(k_Bounce) / sizeof(k_Bounce[0]))) s_BounceF = -1;
    }

    void ShrinkTick();
    void TurnPopTick();
    void BuildingTick() {
        HeldTick();
        TurnPopTick();
        ShrinkTick();
        BounceTick();
        CliffStackTick();
        UnlockTick();
        using DelFn = void (*)(u8*);
        for (Respawn& r : s_Respawns) {
            if (r.idx == 0xffffffff) continue;
            r.frames++;
            u8* nb = ActorOfHandle(r.newBody);
            if (!nb) {   /* the spawner's handle doesn't always resolve (seen with bridges): the record's body that isn't an old one */
                ForEachActor([&](u8* a) {
                    if (nb || VtOf(a) != BuildingBodyVt || *reinterpret_cast<u32*>(a + ActorRecordIndex) != r.idx) return;
                    u32 h = *reinterpret_cast<u32*>(a + 0x230);
                    for (int i = 0; i < r.oldCount; i++) if (r.old[i] == h) return;
                    nb = a;
                });
            }
            bool ready = r.frames >= 150;
            if (nb && r.frames >= 2 && StructureOfBody(nb)) ready = true;   /* a bridge / incline is its body alone: a duplicate showed for ~3 s */
            if (nb && r.frames >= 4) {
                const Vec3f b = *reinterpret_cast<const Vec3f*>(nb + 0x7f0);
                int fresh = 0;
                ForEachActor([&](u8* a) {
                    if (a == nb || !HouseNear(a, b, nullptr)) return;
                    u32 h = *reinterpret_cast<u32*>(a + 0x230);
                    for (int i = 0; i < r.oldCount; i++) if (r.old[i] == h) return;
                    fresh++;
                });
                if (fresh > 0 || r.frames >= 90) ready = true;
            }
            if (!ready) continue;
            if (nb && StructureOfBody(nb)) { s_BounceH = *reinterpret_cast<u32*>(nb + 0x230); s_BounceF = 0; }
            for (int i = 0; i < r.oldCount; i++)
                if (u8* a = ActorOfHandle(r.old[i])) reinterpret_cast<DelFn>(Main(ActorRequestDelete))(a);
            r.idx = 0xffffffff;
            if (r.dirty && nb) StartRespawn(r, nb);
        }
    }

    /* 0x2a914b0: current design of the house (out: 16 bytes). */
    bool BridgeDesignGet(u32 handle);

    bool HouseDesignGet(u8* out, u32 handle) {
        if (s_SpawnArmed) { s_SpawnArmed = false; s_SpawnMode = true; Log("spawn menu: opened (handle %x)", handle); }
        HouseExt e;
        if (!out || !HouseExtOf(HouseBodyOfHandle(handle), &e)) return out && BridgeDesignGet(handle);
        BridgeDesignGet(0);   /* a house: not on a bridge */
        u16* o = reinterpret_cast<u16*>(out);
        o[0] = StyleOf(e.ids[0]);
        o[1] = e.ids[0]; o[2] = e.ids[1]; o[3] = e.ids[2];
        memcpy(out + 8, e.deco, 8);
        return true;
    }

    /* 0x2a91660: apply a design chosen in the menu to the house's save data. */
    bool BridgeTabOpen();
    bool MenuOnBridge();

    bool HouseDesignSet(u32 handle, const u8* design) {
        if (s_SpawnMode) { Log("spawn menu: house design change ignored"); return true; }
        if (BridgeTabOpen() || MenuOnBridge()) { Log("bridges: house pick ignored (menu on a bridge or incline / extra tab)"); return true; }
        HouseExt e;
        if (!design || !HouseExtOf(HouseBodyOfHandle(handle), &e)) return false;
        const u16* d = reinterpret_cast<const u16*>(design);
        for (int i = 0; i < 3; i++)
            if (d[1 + i] != 0xffff) e.ids[i] = e.ids2[i] = d[1 + i];
        memcpy(e.deco, design + 8, 8);
        u8* body = HouseBodyOfHandle(handle);
        Log("exterior: house kind %d -> %x %x %x deco %x", *reinterpret_cast<s32*>(body + BodyParamKind),
            e.ids[0], e.ids[1], e.ids[2], *reinterpret_cast<const u16*>(design + 8));
        RespawnHouse(body);
        return true;
    }

    /* ---- Island editor: a "Bridges" tab in HHP's house exterior menu ----
       The menu (scene WherearenIslandsCustomHome, code ~0x3404000-0x3460000) has a generic tab
       strip at menu + 0x1c260. Its tabs come from a static descriptor in .data, 0x4a33ed0
       = { u64 count 5, entries* 0x5667308 }, entries of 0x70 bytes: +0x08 u32 kind (picks the
       tab icon frame), +0x10 FixedSafeString<64> STR_CategoryName label (the bubble), +0x68 u8
       shown. 0x3036870(strip, desc) builds the tabs from it; 0x30358c0(strip, params) binds
       the tab buttons, params +0x14 = how many buttons (5, from 0x340abf0). 0x3033694(button,
       kind) sets a tab's icon: tab type 0x16 maps the five kinds to Pattern frames 0-4.
       The romfs (tools/bridges_tab.py) adds button L_Category_05, CategoryNum frame 5 (6 tabs),
       Pattern frame 5 (bridge icon) and label 1290 "Bridges". Here: on the main island the menu
       gets a 6-entry descriptor, 6 buttons, and the bridge kind shows Pattern frame 5. The menu
       keeps its current tab at +0x1c290 and clamps it to 0-4 when it fills the grid, so for now
       the Bridges tab shows the Shape grid and picks made there are ignored.
       NOT YET TESTED IN GAME. */
    constexpr u32 HomeTabDesc        = 0x4a33ed0;   /* { u64 count, entry* } */
    constexpr u32 HomeTabEntrySize   = 0x70;
    constexpr u32 BridgeTabKind      = 0x7f;        /* not one of the game's kinds (0x2a-0x2f) */
    constexpr u32 InclineTabKind     = 0x7e;
    constexpr u32 BuildingsTabKind   = 0x7d;
    constexpr int BridgeTab          = 5, InclineTab = 6, BuildingsTab = 7, HomeTabCount = 8;
    constexpr u32 MenuTabIndex       = 0x1c290;
    constexpr u32 TabBtnKind         = 0x270, TabBtnAnim = 600;
    constexpr u32 AnimFrameQueryGot  = 0x4ac4f00;   /* vtable base of the anim query object */

    struct alignas(8) HomeTabDescT { u64 count; u8* entries; };
    alignas(16) static u8 s_HomeTabs[HomeTabCount * HomeTabEntrySize];
    static HomeTabDescT s_HomeTabDesc = { 0, nullptr };
    static bool s_HomeTabButtons = false;   /* 0x340abf0 is binding the menu's tab buttons */
    static u8* s_HomeMenu = nullptr;         /* the open menu (seen on a tab change) */

    bool HomeTabsWanted(const void* desc) {
        return desc == reinterpret_cast<const void*>(Main(HomeTabDesc)) && EditorMap();
    }

    /* The 6-tab descriptor: the game's five tabs, then Bridges (a copy of Roof with its own kind
       and label). Built from the live table each time, so it follows the game's static init. */
    const void* HomeTabs(const void* gameDesc) {
        const HomeTabDescT* g = reinterpret_cast<const HomeTabDescT*>(gameDesc);
        if (!g || g->count != 5 || !g->entries) return gameDesc;
        memcpy(s_HomeTabs, g->entries, 5 * HomeTabEntrySize);
        static const struct { int tab; u32 kind; const char* label; } extra[] = {
            { BridgeTab, BridgeTabKind, "1290" }, { InclineTab, InclineTabKind, "1291" },
            { BuildingsTab, BuildingsTabKind, "1292" } };   /* bridges_tab.py */
        for (const auto& x : extra) {
            u8* e = s_HomeTabs + x.tab * HomeTabEntrySize;
            memcpy(e, g->entries + 1 * HomeTabEntrySize, HomeTabEntrySize);
            *reinterpret_cast<u32*>(e + 0x08) = x.kind;
            /* FixedSafeString<64> at +0x10: vtable, char* (-> own buffer at +0x24), s32 capacity */
            *reinterpret_cast<char**>(e + 0x18) = reinterpret_cast<char*>(e + 0x24);
            memset(e + 0x24, 0, 0x40);
            memcpy(e + 0x24, x.label, 5);
            e[0x68] = 1;
        }
        s_HomeTabDesc = { HomeTabCount, s_HomeTabs };
        Log("bridges: menu gets %d tabs", HomeTabCount);
        return &s_HomeTabDesc;
    }

    void HomeTabButtonsBegin(bool on) { s_HomeTabButtons = on && EditorMap(); }

    /* 0x30358c0 params while 0x340abf0 binds the tab buttons: 5 -> 6 buttons. */
    void HomeTabButtonParams(u8* params) {
        if (!s_HomeTabButtons || !params) return;
        u32* n = reinterpret_cast<u32*>(params + 0x14);
        if (*n == 5) { *n = HomeTabCount; Log("bridges: %d tab buttons", HomeTabCount); }
    }

    /* After 0x3033694 put frame 0 on an unknown kind: the bridge kind gets Pattern frame 5
       (same calls as the end of 0x3033694). */
    void HomeTabIcon(u8* btn, u32 kind) {
        int tab = kind == BridgeTabKind ? BridgeTab : kind == InclineTabKind ? InclineTab : kind == BuildingsTabKind ? BuildingsTab : -1;
        if (tab < 0 || !btn) return;
        u8* anim = *reinterpret_cast<u8**>(btn + TabBtnAnim);
        if (!anim || !*reinterpret_cast<u8**>(anim + 8)) return;
        struct { uintptr_t vt; u32 id; u32 pad; } q = { *reinterpret_cast<uintptr_t*>(Main(AnimFrameQueryGot)) + 0x10, 9, 0 };
        using SelFn = void (*)(u8*, void*);
        using MaxFn = float (*)(void*);
        using SetFn = void (*)(float, void*);
        u8* vt = *reinterpret_cast<u8**>(anim);
        u8* ft = *reinterpret_cast<u8**>(anim + 24);
        reinterpret_cast<SelFn>(*reinterpret_cast<uintptr_t*>(vt + 0x20))(anim, &q);
        float max = reinterpret_cast<MaxFn>(*reinterpret_cast<uintptr_t*>(ft + 0xb0))(*reinterpret_cast<void**>(anim + 8));
        q.id = 0x14;
        reinterpret_cast<SelFn>(*reinterpret_cast<uintptr_t*>(vt + 0x18))(anim, &q);
        float frame = (float)tab <= max ? (float)tab : 0.0f;
        reinterpret_cast<SetFn>(*reinterpret_cast<uintptr_t*>(ft + 0x158))(frame, *reinterpret_cast<void**>(anim + 8));
        Log("bridges: tab icon frame %d (anim max %d)", (int)frame, (int)max);
    }

    /* 0x2fe5370(bar, tab count, selected): plays the bar's CategoryNum animation at frame
       <count> when the animation is that long, else to its end. Frame f lays out f + 1 tabs (the
       game always had 5 tabs, so it always ran "to the end", frame 4). With the 6-frame animation
       that would be wrong both ways (6 tabs: end; 5 tabs in HHP: frame 5 = 6-tab layout), so the
       house menu's bar (remembered when its tabs are set) gets frame count - 1. */
    /* The bar is created after the tabs are set, so the strip is remembered and its bar
       (strip +0x20) looked up when needed; the tab-changed handler also re-applies the layout. */
    static u8* s_HomeStrip = nullptr;
    static s32 s_Tab0Current = -1;   /* tab 0's own current item while the extra tabs borrow it */
    static u8* s_BarDone = nullptr;      /* bar / count the layout was last applied to */
    static u32 s_BarDoneCount = 0;

    void HomeTabStrip(u8* strip, const void* desc) {
        if (desc == reinterpret_cast<const void*>(Main(HomeTabDesc)) && strip) { s_HomeStrip = strip; s_BarDone = nullptr; s_Tab0Current = -1; }
    }

    static u8* HomeBar() { return s_HomeStrip ? *reinterpret_cast<u8**>(s_HomeStrip + 0x20) : nullptr; }

    void HomeTabBarCount(u8* bar, u32 count, bool force = false) {
        static int s_Dbg = 0;
        if (s_Dbg < 12 && EditorMap()) { s_Dbg++; Log("bridges: bar call %p count %d (strip %p bar %p)", bar, (int)count, s_HomeStrip, HomeBar()); }
        if (!bar || bar != HomeBar() || count < 1 || count > (u32)HomeTabCount) return;
        if (!force && bar == s_BarDone && count == s_BarDoneCount) return;
        s_BarDone = bar; s_BarDoneCount = count;
        u8* anim = *reinterpret_cast<u8**>(bar + 0x208);
        if (!anim || !*reinterpret_cast<u8**>(anim + 8)) { Log("bridges: bar has no CategoryNum anim"); return; }
        struct { uintptr_t vt; u32 id; u32 pad; } q = { *reinterpret_cast<uintptr_t*>(Main(AnimFrameQueryGot)) + 0x10, 0x14, 0 };
        u8* vt = *reinterpret_cast<u8**>(anim);
        u8* ft = *reinterpret_cast<u8**>(anim + 24);
        reinterpret_cast<void (*)(u8*, void*)>(*reinterpret_cast<uintptr_t*>(vt + 0x18))(anim, &q);
        reinterpret_cast<void (*)(float, void*)>(*reinterpret_cast<uintptr_t*>(ft + 0x158))((float)(count - 1), *reinterpret_cast<void**>(anim + 8));
        static u32 s_Logged = 0;
        if (s_Logged != count) { s_Logged = count; Log("bridges: tab bar laid out for %d tabs", (int)count); }
    }

    /* 0x340b120 (the menu's tab-changed handler): remember the menu and its tab. */
    void HomeTabChanged(u8* menu) {
        if (!menu || !EditorMap()) return;
        s_HomeMenu = menu;
        if (u8* strip = menu + 0x1c260; strip == s_HomeStrip)
            HomeTabBarCount(HomeBar(), *reinterpret_cast<u32*>(strip + 0x28));
        Log("bridges: menu tab %d", *reinterpret_cast<s32*>(menu + MenuTabIndex));
    }

    constexpr u32 MenuGridCount = 0x1c1f0;
    constexpr u32 BridgeCells   = 8, InclineCells = 8;

    /* 0x340de50 (fill the grid for the current tab; tabs 0-4 set the item count at +0x1c1f0,
       other tabs keep it): the Bridges tab has one cell per bridge material. */
    int MenuStructureLook(int tab);

    void HomeGridFill(u8* menu) {
        if (menu && EditorMap() && menu + 0x1c260 == s_HomeStrip)   /* also lays the tab bar out */
            HomeTabBarCount(HomeBar(), *reinterpret_cast<u32*>(s_HomeStrip + 0x28));
        if (!menu || !EditorMap()) return;
        s32 tab = *reinterpret_cast<s32*>(menu + MenuTabIndex);
        if (tab == BridgeTab) *reinterpret_cast<u32*>(menu + MenuGridCount) = BridgeCells;
        if (tab == InclineTab) *reinterpret_cast<u32*>(menu + MenuGridCount) = InclineCells;
        if (tab == BuildingsTab) *reinterpret_cast<u32*>(menu + MenuGridCount) = BuildingCells;
        /* current look (the pink card with the tick) and the cursor's start: the grid reads the
           current item of the tab clamped to 0-4 (tab 0, +0x1c1f4), so on the extra tabs tab 0's
           slot holds ours, and gets its own value back on the game's tabs */
        s32* tab0 = reinterpret_cast<s32*>(menu + 0x1c1f4);
        if (tab >= BridgeTab) {
            int cur = MenuStructureLook(tab);
            if (cur < 0) cur = 0;
            if (s_Tab0Current < 0) s_Tab0Current = *tab0;
            *tab0 = cur;
            *reinterpret_cast<s32*>(menu + 0x1c254) = cur;
            *reinterpret_cast<s32*>(menu + 0x1c258) = cur;
        } else if (s_Tab0Current >= 0) {
            *tab0 = s_Tab0Current;
            s_Tab0Current = -1;
        }
    }

    /* 0x340d5b0(menu, event): the grid's picture delegate. Event { op, cell, from cell, -,
       page* (+0xc first item) }; op 0 = show item <first + cell> in <cell>. The game loads a
       prerendered icon archive into one of the menu's 270 picture slots (menu +0x2a0, 0x1a8
       each) with 0x3312d40(slot, SafeString name, item, kind) and puts the slot on the cell
       (cells at menu +0x240, 0x20 each, +8 slot, +0x18 changed). On the extra tabs the item is a
       bridge / incline look: its picture is Layout_HouseShapeIcon_B<n> / S<n> (bridges_tab.py,
       rendered from the game's models), loaded with the house-shape icon kind (0x1b). */
    constexpr u32 IconLoad       = 0x3312d40;
    constexpr u32 CellFrame      = 0x340d350;   /* (float frame, menu, item) */
    constexpr u32 NoneItemGot    = 0x4ab37e0;
    constexpr u32 IconKindShape  = 0x1b;

    /* Name bubble on a hovered cell: 0x340b370(menu, event) shows it for the roof / wall / door
       tabs from the tab's id list (roofs: count +0x1c4e0, u16* +0x1c4e8) and the message
       STR_HouseRoofName "%05d" of the id, and hides it on other tabs. On our tabs it runs as the
       roof tab with a list of made-up ids 60000+ (bridges) / 60010+ (inclines), whose names
       bridges_tab.py adds to STR_HouseRoofName (the game's bridge / incline kit names). */
    constexpr u32 CellNameOff  = 0x1c4e0;
    /* With the spaces left (sai): bridges_tab.py adds each name once per count 0..47, as
       61000 / 62000 / 63000 (bridges / inclines / buildings) + cell * 50 + spaces left. */
    int SpawnSpacesLeft();
    static u16 s_CellNameIds[16];

    void HomeCellName(u8* menu, void* ev, void (*orig)(u8*, void*)) {
        u32& tab = *reinterpret_cast<u32*>(menu + MenuTabIndex);
        if (!EditorMap() || (tab != (u32)BridgeTab && tab != (u32)InclineTab && tab != (u32)BuildingsTab)) { orig(menu, ev); return; }
        u32 savedTab = tab;
        u32& count = *reinterpret_cast<u32*>(menu + CellNameOff);
        const u16*& ids = *reinterpret_cast<const u16**>(menu + CellNameOff + 8);
        u32 savedCount = count;
        const u16* savedIds = ids;
        tab = 1;
        count = savedTab == (u32)BuildingsTab ? (u32)BuildingCells : 8;
        int left = SpawnSpacesLeft();
        u32 base = savedTab == (u32)BridgeTab ? 61000 : savedTab == (u32)InclineTab ? 62000 : 63000;
        for (u32 i = 0; i < count && i < 16; i++) s_CellNameIds[i] = (u16)(base + i * 50 + (u32)left);
        ids = s_CellNameIds;
        orig(menu, ev);
        tab = savedTab;
        count = savedCount;
        ids = savedIds;
    }

    bool HomeGridPicture(u8* menu, const s32* ev) {
        if (!menu || !ev || !EditorMap()) return false;
        /* the grid fills once the tab bar exists: lay the bar out for our tab count here too */
        if (menu + 0x1c260 == s_HomeStrip) HomeTabBarCount(HomeBar(), *reinterpret_cast<u32*>(s_HomeStrip + 0x28));
        s32 tab = *reinterpret_cast<s32*>(menu + MenuTabIndex);
        if (tab < BridgeTab || ev[0] != 0) return false;
        u32 cell = (u32)ev[1];
        s32 idx = *reinterpret_cast<const s32*>(*reinterpret_cast<u8* const*>(ev + 4) + 0xc) + (s32)cell;
        s32 slot = -1;
        if (idx >= 0 && idx < *reinterpret_cast<s32*>(menu + MenuGridCount)) {
            for (s32 k = 0; k < 0x10e; k++) {
                u8* sl = menu + 0x2a0 + k * 0x1a8;
                if (*reinterpret_cast<s32*>(sl + 0x1a0) != 0 || ((sl[0x18] >> 1) & 1)) continue;
                char name[4] = { tab == BridgeTab ? 'B' : tab == InclineTab ? 'S' : 'H', (char)('0' + idx % 10), 0, 0 };
                struct { uintptr_t vt; const char* str; } nm = { *reinterpret_cast<uintptr_t*>(Main(SafeStringVtGot)) + 0x10, name };
                reinterpret_cast<void (*)(u8*, void*, void*, u32)>(Main(IconLoad))(sl, &nm, *reinterpret_cast<void**>(Main(NoneItemGot)), IconKindShape);
                slot = k;
                break;
            }
        }
        reinterpret_cast<void (*)(float, u8*, u32)>(Main(CellFrame))(0.0f, menu, (u32)idx);
        u32 n = *reinterpret_cast<u32*>(menu + 0x238);
        u8* entry = *reinterpret_cast<u8**>(menu + 0x240) + (cell < n ? cell : 0) * 0x20;
        if (*reinterpret_cast<s32*>(entry + 8) != slot) {
            *reinterpret_cast<s32*>(entry + 8) = slot;
            if (slot != -1) *reinterpret_cast<s32*>(entry + 0x18) = 1;
        }
        return true;
    }

    bool BridgeTabOpen() {
        return s_HomeMenu && EditorMap() && *reinterpret_cast<s32*>(s_HomeMenu + MenuTabIndex) >= BridgeTab;
    }

    /* ---- bridge looks (save side; the grid of the Bridges tab will call ApplyBridgePick) ----
       Bridges are entries of the island's building list (save records of 20 bytes, type 26). A
       bridge's look and size are one id (record +8, BridgeType in NHSE's naming: material x
       {3, 4, 5 tiles long, 3 diagonal sizes}). A choice keeps the size and swaps the material,
       then the bridge is rebuilt with the island's building spawner like a house; if the old
       bridge actor can't be told apart, the new one is removed again and the new look shows
       from the next island load. Guesses (unverified): record +8 is the id; the spawner builds
       bridges and sets +0x174 (record index) on them. */
    constexpr s16 BridgeBuildingType = 26, InclineBuildingType = 27;
    constexpr u32 BridgeIdOffset     = 8;
    constexpr int BridgeMaterials    = 8, BridgeShapes = 6;
    /* [material][shape]: Log, Suspension, Stone, Wooden, Brick, Iron, Zen, Red zen (the grid's
       order, bridge_icons.py) x 3 / 4 / 5 long, diagonal 2.5 / 3.0 / 3.5. */
    static const u16 k_BridgeIds[BridgeMaterials][BridgeShapes] = {
        { 0x0e, 0x0d, 0x0f, 0x10, 0x11, 0x12 },   /* log */
        { 0x07, 0x06, 0x08, 0x09, 0x0a, 0x0b },   /* suspension */
        { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 },   /* stone */
        { 0x2c, 0x25, 0x2d, 0x2e, 0x2f, 0x30 },   /* wooden */
        { 0x31, 0x26, 0x34, 0x35, 0x36, 0x37 },   /* brick */
        { 0x27, 0x19, 0x28, 0x29, 0x2a, 0x2b },   /* iron */
        { 0x13, 0x0c, 0x14, 0x15, 0x16, 0x17 },   /* zen ("Japanese") */
        { 0x20, 0x18, 0x21, 0x22, 0x23, 0x24 },   /* red zen */
    };

    struct BridgeSwap { u32 idx, newH; int frames; };
    static BridgeSwap s_BridgeSwap = { 0, 0, -1 };

    static bool BridgeLook(u16 id, int* mat, int* shape) {
        for (int m = 0; m < BridgeMaterials; m++)
            for (int k = 0; k < BridgeShapes; k++)
                if (k_BridgeIds[m][k] == id) { *mat = m; *shape = k; return true; }
        return false;
    }

    static s16* BridgeRecord(int i) {
        u8* recs = BuildingRecords();
        if (!recs || i < 0 || i >= 47) return nullptr;
        s16* r = reinterpret_cast<s16*>(recs + i * 0x14);
        return r[0] == BridgeBuildingType ? r : nullptr;
    }

    static void LogRecord(const char* what, const u8* r);

    /* TEST (spawn stage 2a): the facts needed to write a new record - the cursor, every building
       record, and where each bridge / incline / house body really stands. Read-only. */
    [[maybe_unused]] static void SpawnDump() {
        if (LiveLayout(s_EditLayout)) {
            const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
            Log("spawn dump: cursor %d %d %d", (int)c->x, (int)c->y, (int)c->z);
        }
        u8* recs = BuildingRecords();
        if (recs) for (int i = 0; i < 47; i++) {
            char tag[16];
            snprintf(tag, sizeof(tag), "rec %02d", i);
            LogRecord(tag, recs + i * 0x14);
        }
        ForEachActor([&](u8* a) {
            if (VtOf(a) != BuildingBodyVt) return;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + 0x7f0);
            u32 idx = *reinterpret_cast<u32*>(a + ActorRecordIndex);
            u32 rot = *reinterpret_cast<u32*>(*reinterpret_cast<u8**>(a + ActorTransform) + 0x2b4);
            Log("spawn dump: body rec %d kind %d at %d %d %d rot %x", (int)idx, StructureOfBody(a), (int)p->x, (int)p->y, (int)p->z, rot);
        });
    }

    /* Building body of a bridge or incline: its record (body +0x174) has type 26 / 27. */
    static int StructureOfBody(u8* body) {
        u8* recs = BuildingRecords();
        if (!recs || !body) return 0;
        u32 i = *reinterpret_cast<u32*>(body + ActorRecordIndex);
        s16 t = i < 47 ? *reinterpret_cast<s16*>(recs + i * 0x14) : 0;
        return t == BridgeBuildingType || t == InclineBuildingType ? t : 0;
    }

    /* Buildings tab (tab 7): shops / services the spawn menu can place. Tested by writing copies into
       a save (2026-11-26, real Switch): a copy loads and is saved, its door leads into the original
       (you come out of the original). Built at grass level: on the beach they float, so the tab only
       places on flat ground at 0 or above. Order = the tab's cells (building_icons.py). */
    /* (k_BuildingCells: see the forward declarations near RemovableBuildingOfBody's) */

    /* A copy is any record of one of those types with an earlier record of the same type: Y may
       remove it. The first one (the island's own) never. */
    static int RemovableBuildingOfBody(u8* body) {
        u8* recs = BuildingRecords();
        if (!recs || !body) return 0;
        u32 i = *reinterpret_cast<u32*>(body + ActorRecordIndex);
        if (i >= 47) return 0;
        s16 t = *reinterpret_cast<s16*>(recs + i * 0x14);
        if (t <= 0 || t == BridgeBuildingType || t == InclineBuildingType) return 0;
        /* sai: any building may go, except the ones holding story / save data: player houses (1-8),
           Resident Services (20) and the Airport (22) - but a spawned copy of those may */
        bool keep = (t >= 1 && t <= 8) || t == 20 || t == 22;
        if (!keep) return t;
        for (u32 k = 0; k < i; k++) if (*reinterpret_cast<s16*>(recs + k * 0x14) == t) return t;
        return 0;
    }

    static int s_MenuBridge = -1;   /* record of the bridge / incline the house menu was opened on, or -1 */
    static int s_MenuKind = 0;      /* its record type (26 / 27) */
    bool ApplyInclinePick(int rec, int which);
    bool ApplyBridgePick(int rec, int material);
    bool SpawnStructure(int tab, int cell);

    /* 0x2a914b0 for a bridge: the menu was opened on a bridge. The design stays "none" (0xffff),
       the menu then starts on the Bridges tab (HomeTabStart). */
    bool BridgeDesignGet(u32 handle) {
        u8* a = ActorOfHandle(handle);
        for (int n = 0; n < MaxHandleRemaps && !a && handle; n++) {   /* swapped bridge (maybe more than once) */
            u32 to = 0;
            for (int i = 0; i < MaxHandleRemaps; i++) if (s_RemapFrom[i] == handle) to = s_RemapTo[i];
            if (!to) break;
            handle = to;
            a = ActorOfHandle(handle);
        }
        if (!a || VtOf(a) != BuildingBodyVt || !IsBridgeBody(a)) { s_MenuBridge = -1; return false; }
        s_MenuBridge = (int)*reinterpret_cast<u32*>(a + ActorRecordIndex);
        s_MenuKind = StructureOfBody(a);
        u8* recs = BuildingRecords();
        if (recs) LogRecord(s_MenuKind == BridgeBuildingType ? "menu on bridge" : "menu on incline", recs + s_MenuBridge * 0x14);
        return true;
    }

    bool MenuOnBridge() { return s_MenuBridge >= 0 && EditorMap(); }

    /* 0x3036ca0(strip, owner, first tab): a menu opened on a bridge starts on the Bridges tab. */
    u32 HomeTabStart(u8* strip, u32 first) {
        if (s_SpawnMode && strip && strip == s_HomeStrip) { Log("spawn menu: starts on the Bridges tab"); return (u32)BridgeTab; }
        if (!MenuOnBridge() || !strip || strip != s_HomeStrip) return first;
        int tab = s_MenuKind == InclineBuildingType ? InclineTab : BridgeTab;
        Log("bridges: menu opens on tab %d", tab);
        return (u32)tab;
    }

    /* 0x340c1d0(menu, event): A on a grid cell. On the Bridges tab: which cell (the game's own
       index math, list = menu->+0x220->+0x578), then swap the bridge the menu was opened on.
       The game's own handler still runs afterwards (v0.64): skipping it left the grid without
       its hand cursor and the menu stuck until it was closed and opened again. The house pick
       it makes is ignored (HouseDesignSet), and the grid is refilled on the next frame
       (s_RefillMenu) so the tick moves to the new look outside the press handler. */
    static u8* s_RefillMenu = nullptr;
    struct PendingSpawn { s32 tab; s32 cell; s64 at; bool on; };
    static PendingSpawn s_PendingSpawn = {};
    /* BridgeTick: the picked structure is placed when the editor is back in cursor mode (menu closed),
       or after 8 s whatever happens. */
    static void PendingSpawnTick() {
        if (!s_PendingSpawn.on) return;
        s64 now = NowTick();
        bool cursor = s_CursorQueryTick && now - s_CursorQueryTick < MsTicks(100) && now - s_PendingSpawn.at > MsTicks(150);
        if (!cursor && now - s_PendingSpawn.at < MsTicks(8000)) return;
        s_PendingSpawn.on = false;
        SpawnStructure(s_PendingSpawn.tab, s_PendingSpawn.cell);
    }

    void HomeGridPick(u8* menu, u8* event) {
        s32 tab = menu ? *reinterpret_cast<s32*>(menu + MenuTabIndex) : -1;
        if (!menu || !event || !EditorMap() || tab < BridgeTab) return;
        u8* owner = *reinterpret_cast<u8**>(menu + 0x220);
        u8* list = owner ? *reinterpret_cast<u8**>(owner + 0x578) : nullptr;
        if (!list) return;
        void* cell = *reinterpret_cast<void**>(event + 8);
        s32 n = *reinterpret_cast<s32*>(list + 0x208);
        void** cells = *reinterpret_cast<void***>(list + 0x210);
        s32 slot = -1;
        for (s32 i = 0; i < n && cells; i++) if (cells[i] == cell) { slot = i; break; }
        if (slot < 0) return;
        s32 cols = *reinterpret_cast<s32*>(list + 0x2e8);
        s32 top = *reinterpret_cast<s32*>(list + 0x430) - *reinterpret_cast<s32*>(list + 0x438);
        s32 row = cols ? slot / cols : 0;
        if (!((list[1000] >> 1) & 1)) top += row;
        else {
            u32 lim = *reinterpret_cast<u32*>(list + 0x434);
            u32 adj = (s32)lim < top ? ~lim : 0;
            if (top < 0) adj = lim + 1;
            s32 i3 = top + row + (s32)adj;
            s32 wrap = *reinterpret_cast<s32*>(list + 0x3d0);
            s32 i4 = wrap <= i3 ? wrap : 0;
            if (-1 < i3) wrap = -i4;
            top = wrap + i3;
        }
        s32 idx = top * cols - row * cols + slot;
        if (idx < 0 || idx >= *reinterpret_cast<s32*>(list + 0x3cc)) idx = -1;
        Log("bridges: cell %d picked (slot %d)", idx, slot);
        int want = tab == BridgeTab ? BridgeBuildingType : InclineBuildingType;
        /* A spawn: the menu came from D-pad left. On a Switch the menu sometimes took longer to open
           than the spawn timing allows ("did not open"), or the cursor heartbeat ended spawn mode
           while it opened; then picks were ignored. So a pick within 10 s of D-pad left, in a menu
           not opened on a bridge / incline, is a spawn too. */
        bool spawnPick = s_SpawnMode || (!MenuOnBridge() && SpawnWithin(0, 10000));
        if (spawnPick) {
            Log("spawn menu: pick tab %d cell %d%s", tab, idx, s_SpawnMode ? "" : " (late menu)");
            if (idx >= 0) SpawnStructure(tab, idx);   /* right away, while the menu is open (sai prefers it) */
            return;
        }
        if (tab == BuildingsTab) { Log("buildings: open the menu with D-pad left to place one"); return; }
        if (idx >= 0 && MenuOnBridge() && s_MenuKind == want) {
            if (want == BridgeBuildingType) ApplyBridgePick(s_MenuBridge, idx);
            else ApplyInclinePick(s_MenuBridge, idx);
            s_RefillMenu = menu;   /* next frame: the tick moves to the new look */
        } else if (idx >= 0) Log("bridges: point at a %s and press X to change it", want == BridgeBuildingType ? "bridge" : "incline");
    }

    static void LogRecord(const char* what, const u8* r) {
        Log("bridges: %s %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x", what,
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17], r[18], r[19]);
    }

    /* The island's building list keeps its own storage of the 20-byte records (what the game saves
       from): 0x1260490 reads entry <i> as accessor = (*list+0x10 vt[0])(list+0x10), entry =
       accessor->vt[0x20](accessor, i) + accessor[1]. Changes must go there too, or they are lost
       on saving (the save view at +0x116cc0 is only what the field reads). */
    static u8* LandBuildingEntry(u32 idx) {
        u8* land = *reinterpret_cast<u8**>(Main(LandGlobal));
        u8* island = land ? *reinterpret_cast<u8**>(land + 0x38) : nullptr;
        u8* list = island ? *reinterpret_cast<u8**>(island + 0x148 + 8) : nullptr;
        if (!list) return nullptr;
        u8* holder = list + 0x10;
        using GetFn = u8* (*)(u8*);
        u8* acc = reinterpret_cast<GetFn>(**reinterpret_cast<uintptr_t**>(holder))(holder);
        if (!acc) return nullptr;
        u8* vt = *reinterpret_cast<u8**>(acc);
        u32 n = reinterpret_cast<u32 (*)(u8*)>(*reinterpret_cast<uintptr_t*>(vt + 0x18))(acc);
        if (idx >= n) return nullptr;
        u64 off = reinterpret_cast<u64 (*)(u8*, u32)>(*reinterpret_cast<uintptr_t*>(vt + 0x20))(acc, idx);
        return *reinterpret_cast<u8**>(acc + 8) + off;
    }

    static void SetLandBuildingId(u32 idx, u16 oldId, u16 newId) {
        u8* e = LandBuildingEntry(idx);
        if (!e) { Log("bridges: land list entry %d not found", (int)idx); return; }
        LogRecord("land entry", e);
        u16* id = reinterpret_cast<u16*>(e + BridgeIdOffset);
        u8* recs = BuildingRecords();
        if (*id == oldId || *id == newId) { *id = newId; Log("bridges: land entry %d id -> %x%s", (int)idx, newId, recs && e == recs + idx * 0x14 ? " (same as save view)" : ""); }
        else Log("bridges: land entry %d id %x unexpected, left alone", (int)idx, *id);
    }

    /* Rebuild bridge <idx> from its (changed) record. */
    static void RespawnBridge(int idx, u16 oldId, u16 newId) {
        u8* field = *reinterpret_cast<u8**>(Main(FieldMgrGlobal));
        u8* land = *reinterpret_cast<u8**>(Main(LandGlobal));
        u8* island = land ? *reinterpret_cast<u8**>(land + 0x38) : nullptr;
        u8* list = island ? *reinterpret_cast<u8**>(island + 0x148 + 8) : nullptr;
        if (!field || !list) { Log("bridges: no building list - new look shows after a reload"); return; }
        BuildingRecord record = reinterpret_cast<BuildingRecord (*)(u8*, u32)>(Main(BuildingRecordGet))(list, (u32)idx);
        LogRecord("list record", record.raw);
        u16* rid = reinterpret_cast<u16*>(record.raw + BridgeIdOffset);   /* the list's record may be a copy */
        s16 rt = *reinterpret_cast<s16*>(record.raw);
        if ((rt == BridgeBuildingType || rt == InclineBuildingType) && *rid == oldId) *rid = newId;
        u32 newH = reinterpret_cast<u32 (*)(u8*, u8*, u32, u64, u64, u64, u64, u64)>(Main(BuildingSpawn))(field + BuildingSpawner, record.raw, (u32)idx, 0, 0, 0, 0, 0);
        Log("bridges: spawn requested, handle %x", newH);
        s_BridgeSwap = { (u32)idx, newH, 0 };
    }

    /* A few frames after a respawn: the old bridge is the other actor of the new one's class with
       the same record index. If there is none, the new actor goes instead (never two bridges). */
    static void FinishBridgeSwap() {
        using DelFn = void (*)(u8*);
        BridgeSwap& w = s_BridgeSwap;
        u8* nb = ActorOfHandle(w.newH);
        if (!nb) {
            if (w.frames < 90) return;
            Log("bridges: nothing spawned - new look shows after a reload");
            w.frames = -1;
            return;
        }
        u32 vt = VtOf(nb), oldH = 0;
        bool indexed = *reinterpret_cast<u32*>(nb + ActorRecordIndex) == w.idx;
        if (indexed)
            ForEachActor([&](u8* a) {
                if (!oldH && a != nb && VtOf(a) == vt && *reinterpret_cast<u32*>(a + ActorRecordIndex) == w.idx)
                    oldH = *reinterpret_cast<u32*>(a + 0x230);
            });
        Log("bridges: new bridge %x (vt %x, index %s), old %x", w.newH, vt, indexed ? "ok" : "differs", oldH);
        if (u8* ob = ActorOfHandle(oldH)) {
            /* the open menu keeps the old bridge's handle: lead it to the new one */
            s_RemapFrom[s_RemapNext] = oldH; s_RemapTo[s_RemapNext] = w.newH;
            s_RemapNext = (s_RemapNext + 1) % MaxHandleRemaps;
            reinterpret_cast<DelFn>(Main(ActorRequestDelete))(ob);
            /* (its walkable shape is rebuilt once the island editor closes: RebuildShapes) */
        }
        else {
            reinterpret_cast<DelFn>(Main(ActorRequestDelete))(nb);
            Log("bridges: old bridge not found - new look shows after a reload");
        }
        w.frames = -1;
    }

    static void RememberOverride(int idx, u16 oldId, u16 newId);

    /* Swap bridge record <rec> to <material>, keeping its size. */
    bool ApplyBridgePick(int rec, int material) {
        s16* r = BridgeRecord(rec);
        if (!r || material < 0 || material >= BridgeMaterials || s_BridgeSwap.frames >= 0) return false;
        u16* id = reinterpret_cast<u16*>(reinterpret_cast<u8*>(r) + BridgeIdOffset);
        int m, k;
        if (!BridgeLook(*id, &m, &k)) { Log("bridges: unknown bridge id %x", *id); return false; }
        if (m == material) return true;
        u16 oldId = *id, newId = k_BridgeIds[material][k];
        *id = newId;
        Log("bridges: record %d id %x -> %x", rec, oldId, newId);
        SetLandBuildingId((u32)rec, oldId, newId);
        RememberOverride(rec, oldId, newId);
        RespawnBridge(rec, oldId, newId);
        return true;
    }

    /* Inclines: one id per look (record +8, NHSE's SlopeType), no sizes. The grid's order; the names
       are guesses from NHSE's identifiers (check them in game): */
    static const u16 k_InclineIds[] = {
        0x1d,   /* natural slope (SlopeNatural) */
        0x02,   /* log stairs? (SlopeWood) */
        0x03,   /* wooden stairs (SlopeWoodStair) */
        0x00,   /* stone stairs (SlopeStoneStair) */
        0x04,   /* brick stairs (SlopeBrickStair) */
        0x01,   /* iron stairs (SlopeIronStair) */
        0x1e,   /* blue wooden? (SlopeWoodBlue) */
        0x1f,   /* blue iron stairs? (SlopeIronStairBlue) */
    };

    bool ApplyInclinePick(int rec, int which) {
        u8* recs = BuildingRecords();
        if (!recs || rec < 0 || rec >= 47 || which < 0 || which >= (int)(sizeof(k_InclineIds) / 2) || s_BridgeSwap.frames >= 0) return false;
        s16* r = reinterpret_cast<s16*>(recs + rec * 0x14);
        if (r[0] != InclineBuildingType) return false;
        u16* id = reinterpret_cast<u16*>(reinterpret_cast<u8*>(r) + BridgeIdOffset);
        u16 oldId = *id, newId = k_InclineIds[which];
        if (oldId == newId) return true;
        *id = newId;
        Log("bridges: incline record %d id %x -> %x", rec, oldId, newId);
        SetLandBuildingId((u32)rec, oldId, newId);
        RememberOverride(rec, oldId, newId);
        RespawnBridge(rec, oldId, newId);
        return true;
    }

    /* Swaps made this session. Closing the island editor puts back the snapshot of the island it
       took when it opened, which undid them (the log showed the record back at the old id); each
       frame a record found back at its old id gets the new one again, so it is what gets saved.
       An override is dropped once the record holds anything else (e.g. rebuilt through Nook). */
    struct LookOverride { s32 idx; u16 oldId, newId; };
    constexpr int MaxOverrides = 32;
    static LookOverride s_Overrides[MaxOverrides];
    static int s_OverrideCount = 0;

    /* Records whose walkable shape (collision / height, built per building slot from the record)
       still has the old look's. It can't be rebuilt while the island editor runs (its play mode
       builds nothing: the bridge lost all collision), so it is done once the editor has closed. */
    static u64 s_ShapePending = 0;
    static u64 s_RemovePending = 0;   /* of those: removed records (Y in the editor) - only clear the shape */
    static int s_ShapeWait = 0;

    /* Rebuild a swapped bridge / incline's slot the way the game does at island load (the type
       switch at 0x2981abc): BldSlotRemove (unmark its tiles, unregister the collision, empty the
       slot), then the type's own slot builder - 0x2a3aef0 for bridges, 0x2a3b680 for inclines -
       which reads the record's id, loads that model's collision (slot +0x100 / +0x1a8 name) and
       re-marks the tiles (0x12620a0). (0x2a3a450 is the house builder: a box you can't walk on.) */
    constexpr u32 BridgeSlotCreate  = 0x2a3aef0;
    constexpr u32 InclineSlotCreate = 0x2a3b680;
    constexpr u32 PlayModeTableGot  = 0x4abbf90;

    static void RebuildShapes() {
        if (!s_ShapePending) { s_ShapeWait = 0; return; }
        if (!s_EditorClosed || !EditorMapStage()) { s_ShapeWait = 0; return; }   /* editor still open */
        if (++s_ShapeWait < 20) return;                                                 /* let the closing settle */
        u8* field = *reinterpret_cast<u8**>(Main(FieldMgrGlobal));
        u8* tbl = *reinterpret_cast<u8**>(Main(PlayModeTableGot));
        u8* recs = BuildingRecords();
        if (!field || !tbl || !recs) return;
        u32 h = PlayModeHash(), mi = 0;
        for (u32 i = 0; i < 0x39; i++) if (reinterpret_cast<u32*>(tbl + 8)[i] == h) { mi = i; break; }
        if (mi != 1 && mi != 0x32) { if (s_ShapeWait == 20) Log("bridges: shape rebuild waits (play mode %u)", mi); s_ShapeWait = 19; return; }
        using RemoveFn = void (*)(u8*, u32, u32);
        using CreateFn = u64 (*)(u8*, u32, u32);
        for (int i = 0; i < 47; i++) {
            if (!(s_ShapePending >> i & 1)) continue;
            s16 type = *reinterpret_cast<s16*>(recs + i * 0x14);
            u16 id = *reinterpret_cast<u16*>(recs + i * 0x14 + BridgeIdOffset);
            if (type != BridgeBuildingType && type != InclineBuildingType) {
                if (type == 0 && (s_RemovePending >> i & 1)) {
                    reinterpret_cast<RemoveFn>(Main(BldSlotRemove))(field + BldSlotOffset, (u32)i, h);
                    Log("spawn: walk shape of removed record %d cleared", i);
                } else if (type > 0) {
                    /* a house / shop placed from the Buildings tab: walls (the house builder) - it was
                       walk-through until the island was reloaded */
                    reinterpret_cast<RemoveFn>(Main(BldSlotRemove))(field + BldSlotOffset, (u32)i, h);
                    u64 ret = reinterpret_cast<u64 (*)(u8*, u32, u64, u64, u64, u64, u64, u64)>(Main(BldSlotCreate))(
                        field + BldSlotOffset, (u32)i, h, 0, 0, 0, 0, 0);
                    Log("buildings: walls of record %d (type %d) built (ret %d)", i, type, (int)(ret & 1));
                }
                continue;
            }
            u8* slot = field + 0x26f8 + 8 + i * 0x208;
            const char* oldName = type == BridgeBuildingType ? reinterpret_cast<const char*>(slot + 0x114)
                                                             : *reinterpret_cast<const char**>(slot + 0x1b0);
            Log("bridges: rebuilding shape of record %d (type %d id %x), was '%.40s'", i, type, id, oldName ? oldName : "?");
            reinterpret_cast<RemoveFn>(Main(BldSlotRemove))(field + BldSlotOffset, (u32)i, h);
            u64 ret = reinterpret_cast<CreateFn>(Main(type == BridgeBuildingType ? BridgeSlotCreate : InclineSlotCreate))(field + BldSlotOffset, (u32)i, h);
            const char* newName = type == BridgeBuildingType ? *reinterpret_cast<const char**>(slot + 0x108)
                                                             : *reinterpret_cast<const char**>(slot + 0x1b0);
            Log("bridges: shape rebuilt (ret %d, slot obj %p, now '%.40s')", (int)(ret & 1),
                *reinterpret_cast<u8**>(slot + 8), newName ? newName : "?");
        }
        s_ShapePending = 0;
        s_RemovePending = 0;
        s_ShapeWait = 0;
        s_EditorClosed = false;
        SyncSaveCopies();
    }

    static void RememberOverride(int idx, u16 oldId, u16 newId) {
        s_ShapePending |= 1ull << idx;
        for (int i = 0; i < s_OverrideCount; i++)
            if (s_Overrides[i].idx == idx) {
                if (s_Overrides[i].oldId == newId) { s_Overrides[i] = s_Overrides[--s_OverrideCount]; return; }  /* swapped back */
                s_Overrides[i].newId = newId; return;
            }
        if (s_OverrideCount < MaxOverrides) s_Overrides[s_OverrideCount++] = { idx, oldId, newId };
    }

    static void KeepOverrides() {
        u8* recs = BuildingRecords();
        if (!recs || !s_OverrideCount) return;
        for (int i = 0; i < s_OverrideCount; i++) {
            LookOverride& o = s_Overrides[i];
            u16* id = reinterpret_cast<u16*>(recs + o.idx * 0x14 + BridgeIdOffset);
            s16 type = *reinterpret_cast<s16*>(recs + o.idx * 0x14);
            if ((type != BridgeBuildingType && type != InclineBuildingType) || (*id != o.oldId && *id != o.newId)) {
                Log("bridges: record %d changed elsewhere (%x) - override dropped", (int)o.idx, *id);
                s_Overrides[i--] = s_Overrides[--s_OverrideCount];
                continue;
            }
            if (*id == o.oldId) {
                *id = o.newId;
                if (u8* e = LandBuildingEntry((u32)o.idx)) *reinterpret_cast<u16*>(e + BridgeIdOffset) = o.newId;
                Log("bridges: record %d was put back to %x - set to %x again", (int)o.idx, o.oldId, o.newId);
                s_ShapePending |= 1ull << o.idx;   /* the restore rebuilt the old walkable shape */
            }
        }
    }

    /* Grid index of the current look of the bridge / incline the menu was opened on, or -1. */
    int MenuStructureLook(int tab) {
        u8* recs = BuildingRecords();
        if (!recs || s_MenuBridge < 0 || s_MenuBridge >= 47) return -1;
        s16* r = reinterpret_cast<s16*>(recs + s_MenuBridge * 0x14);
        u16 id = *reinterpret_cast<u16*>(reinterpret_cast<u8*>(r) + BridgeIdOffset);
        if (tab == BridgeTab && r[0] == BridgeBuildingType) { int m, k; return BridgeLook(id, &m, &k) ? m : -1; }
        if (tab == InclineTab && r[0] == InclineBuildingType)
            for (int i = 0; i < (int)(sizeof(k_InclineIds) / 2); i++) if (k_InclineIds[i] == id) return i;
        return -1;
    }

    /* ---- Spawn menu, stage 2: a pick creates a new bridge / incline at the cursor ----
       Building records (save view and the land list, 47 x 0x14 bytes, little endian): +0 u16 type
       (0 = empty slot, 26 bridge, 27 incline), +2 u16 X and +4 u16 Z in 5-unit steps (for bridges
       and inclines X*5, Z*5 is exactly where the actor stands), +6 rotation in quarter turns, +8 u16
       look id. The new record goes into the first empty slot, the building spawner builds it now and
       its walkable shape is built once the editor has closed (s_ShapePending). Closing the editor
       restores its opening snapshot, which empties the slot again: KeepNew writes it back. */
    struct NewStructure { int idx; u8 raw[0x14]; };
    static NewStructure s_New[16];
    static int s_NewCount = 0;

    static u32 SpawnRecordActor(int idx) {
        u8* field = *reinterpret_cast<u8**>(Main(FieldMgrGlobal));
        u8* land = *reinterpret_cast<u8**>(Main(LandGlobal));
        u8* island = land ? *reinterpret_cast<u8**>(land + 0x38) : nullptr;
        u8* list = island ? *reinterpret_cast<u8**>(island + 0x148 + 8) : nullptr;
        if (!field || !list) { Log("spawn: no building list"); return 0; }
        BuildingRecord record = reinterpret_cast<BuildingRecord (*)(u8*, u32)>(Main(BuildingRecordGet))(list, (u32)idx);
        LogRecord("spawn: list record", record.raw);
        u32 h = reinterpret_cast<u32 (*)(u8*, u8*, u32, u64, u64, u64, u64, u64)>(Main(BuildingSpawn))(field + BuildingSpawner, record.raw, (u32)idx, 0, 0, 0, 0, 0);
        Log("spawn: record %d spawn requested, handle %x", idx, h);
        return h;
    }

    static void WriteRecord(int idx, const u8* raw) {
        u8* recs = BuildingRecords();
        if (recs) memcpy(recs + idx * 0x14, raw, 0x14);
        u8* e = LandBuildingEntry((u32)idx);
        if (e && (!recs || e != recs + idx * 0x14)) memcpy(e, raw, 0x14);
        SyncSaveCopies();
    }

    static void PlayEditFx(const char* effect, const char* sound, const Vec3f& at);
    /* Free records for a spawn (all 47 building records; at most 16 spawns kept at once). */
    int SpawnSpacesLeft() {
        u8* recs = BuildingRecords();
        if (!recs) return 0;
        int n = 0;
        for (int i = 0; i < 47; i++) {
            const u8* r = recs + i * 0x14;
            bool empty = true;
            for (int k = 0; k < 0x14; k++) if (r[k]) { empty = false; break; }
            if (empty) n++;
        }
        int cap = 16 - s_NewCount;
        if (n > cap) n = cap;
        return n < 0 ? 0 : n > 47 ? 47 : n;
    }

    bool SpawnStructure(int tab, int cell) {
        u8* recs = BuildingRecords();
        if (!recs || !LiveLayout(s_EditLayout) || cell < 0) return false;
        s16 type;
        u16 id;
        if (tab == BridgeTab) {
            if (cell >= BridgeMaterials) return false;
            type = BridgeBuildingType; id = k_BridgeIds[cell][0];
        } else if (tab == InclineTab) {
            if (cell >= (int)(sizeof(k_InclineIds) / 2)) return false;
            type = InclineBuildingType; id = k_InclineIds[cell];
        } else if (tab == BuildingsTab) {
            if (cell >= BuildingCells) return false;
            type = k_BuildingCells[cell]; id = 0;
        } else return false;
        int slot = -1;
        for (int i = 0; i < 47 && slot < 0; i++) {
            const u8* r = recs + i * 0x14;
            bool empty = true;
            for (int k = 0; k < 0x14; k++) if (r[k]) { empty = false; break; }
            if (empty) slot = i;
        }
        if (slot < 0 || s_NewCount >= 16) { Log("spawn: no free building slot"); return false; }
        const Vec3f* c = reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
        u8 raw[0x14] = {};
        *reinterpret_cast<u16*>(raw + 0) = (u16)type;
        *reinterpret_cast<u16*>(raw + 2) = (u16)(int)(c->x / 5.0f + 0.5f);
        *reinterpret_cast<u16*>(raw + 4) = (u16)(int)(c->z / 5.0f + 0.5f);
        *reinterpret_cast<u16*>(raw + 6) = 0;
        *reinterpret_cast<u16*>(raw + 8) = id;
        WriteRecord(slot, raw);
        s_New[s_NewCount].idx = slot;
        memcpy(s_New[s_NewCount].raw, raw, 0x14);
        s_NewCount++;
        Log("spawn: %s id %x in record %d at %d %d", type == BridgeBuildingType ? "bridge" : type == InclineBuildingType ? "incline" : "building", id, slot,
            *reinterpret_cast<u16*>(raw + 2) * 5, *reinterpret_cast<u16*>(raw + 4) * 5);
        SpawnRecordActor(slot);
        s_ShapePending |= 1ull << slot;
        PlayEditFx("道造成煙：四角", "道造成煙：四角", *c);   /* the path tool's puff, with its own sound */
        return true;
    }

    /* The editor's closing restore empties new records again: put them back and rebuild them. */
    static void KeepNew() {
        u8* recs = BuildingRecords();
        if (!recs) return;
        for (int i = 0; i < s_NewCount; i++) {
            u8* r = recs + s_New[i].idx * 0x14;
            if (memcmp(r, s_New[i].raw, 0x14) == 0) continue;
            /* The game fills in more of the record once the building is spawned: still ours when the
               type, position and look are the ones we wrote - keep the game's version from now on. */
            const u16* now = reinterpret_cast<const u16*>(r);
            const u16* mine = reinterpret_cast<const u16*>(s_New[i].raw);
            if (now[0] == mine[0] && now[4] == mine[4]) {   /* same type and look: still ours */
                if (now[1] != mine[1] || now[2] != mine[2] || now[3] != mine[3]) {
                    LogRecord("spawn: record changed by the game, was", s_New[i].raw);
                    LogRecord("spawn: record changed by the game, now", r);
                }
                memcpy(s_New[i].raw, r, 0x14);
                continue;
            }
            bool empty = true;
            for (int k = 0; k < 0x14; k++) if (r[k]) { empty = false; break; }
            if (!empty) {   /* something else is there now */
                LogRecord("spawn: forgotten, mine", s_New[i].raw);
                LogRecord("spawn: forgotten, now ", r);
                Log("spawn: record %d taken by something else - forgotten", s_New[i].idx);
                s_New[i--] = s_New[--s_NewCount];
                continue;
            }
            WriteRecord(s_New[i].idx, s_New[i].raw);
            Log("spawn: record %d was emptied - written back", s_New[i].idx);
            SpawnRecordActor(s_New[i].idx);
            s_ShapePending |= 1ull << s_New[i].idx;
        }
    }

    /* ZL / ZR on a bridge / incline you spawned: a quarter turn (record +6) or the next size (bridges:
       3 / 4 / 5 long, then the diagonal 2.5 / 3.0 / 3.5 of the same material). The record is
       rewritten, the old actor deleted and the structure spawned again; the keeper follows. Only
       spawned ones (in s_New), whose record the keeper protects from the editor's closing restore. */
    static void DeleteActorsOfRecord(int idx);
    struct ReshapeSwap { bool active; u32 old[4]; int n; u32 newH; int frames; };
    static ReshapeSwap s_Reshape = {};
    static void FinishReshape() {
        if (!s_Reshape.active) return;
        using DelFn = void (*)(u8*);
        for (int i = 0; i < s_Reshape.n; i++)
            if (u8* a = ActorOfHandle(s_Reshape.old[i])) reinterpret_cast<DelFn>(Main(ActorRequestDelete))(a);
        s_Reshape.active = false;
    }
    static void ReshapeTick() {
        if (!s_Reshape.active) return;
        s_Reshape.frames++;
        /* 2 frames: the new one is drawn by then. Waiting for its actor by handle didn't work (the
           lookup stayed empty), so both showed stacked for ~2 s (Switch video). */
        if (s_Reshape.frames >= 2) FinishReshape();
    }
    /* A turn like furniture: the actor can't be turned while standing (its turn is set when it's built),
       so the old one dips (smaller, lower) for the frames before it's deleted and the new one pops up
       from that with a little overshoot. Scale (body +0x818) and height are read every frame. */
    struct TurnPop { bool active; int idx; u32 old[4]; int n; int f; float y; u32 h; };
    static TurnPop s_Pop = {};
    static const float k_PopS[] = { 0.82f, 0.88f, 0.95f, 1.02f, 1.07f, 1.08f, 1.05f, 1.01f, 0.99f, 1.0f };
    static const float k_PopY[] = { -1.5f, -0.8f, 0.2f, 1.2f, 1.8f, 1.6f, 1.0f, 0.4f, 0.0f, 0.0f };
    static void SetLook(u8* a, float sc, float y) {
        *reinterpret_cast<Vec3f*>(a + 0x818) = { sc, sc, sc };
        reinterpret_cast<Vec3f*>(a + 0x7f0)->y = y;
        reinterpret_cast<Vec3f*>(a + 0x7fc)->y = y;
    }
    void TurnPopTick() {
        if (!s_Pop.active) return;
        u8* nb = s_Pop.h ? ActorOfHandle(s_Pop.h) : nullptr;
        if (!nb && s_Pop.f == 0) {
            ForEachActor([&](u8* a) {
                if (nb || VtOf(a) != BuildingBodyVt || *reinterpret_cast<u32*>(a + ActorRecordIndex) != (u32)s_Pop.idx) return;
                u32 h = *reinterpret_cast<u32*>(a + 0x230);
                for (int i = 0; i < s_Pop.n; i++) if (s_Pop.old[i] == h) return;
                nb = a;
            });
            if (!nb) return;
            s_Pop.h = *reinterpret_cast<u32*>(nb + 0x230);
            s_Pop.y = reinterpret_cast<Vec3f*>(nb + 0x7f0)->y;
        }
        if (!nb || s_Pop.f >= (int)(sizeof(k_PopS) / sizeof(k_PopS[0]))) { s_Pop.active = false; return; }
        SetLook(nb, k_PopS[s_Pop.f], s_Pop.y + k_PopY[s_Pop.f]);
        s_Pop.f++;
    }

    bool ReshapeStructure(u8* body, bool rotate, bool resize) {
        u8* recs = BuildingRecords();
        int type = StructureOfBody(body);
        u32 idx = body ? *reinterpret_cast<u32*>(body + ActorRecordIndex) : 99;
        if (!recs || !type || idx >= 47) return false;
        int n = -1;
        for (int i = 0; i < s_NewCount; i++) if (s_New[i].idx == (int)idx) n = i;
        if (n < 0) { Log("structures: record %d is not one you spawned - not turned / resized", (int)idx); return false; }
        u8 raw[0x14];
        memcpy(raw, recs + idx * 0x14, 0x14);
        u16* r = reinterpret_cast<u16*>(raw);
        if (rotate) r[3] = (u16)((r[3] + 1) & 3);
        if (resize) {
            int m, k;
            if (type != BridgeBuildingType || !BridgeLook(r[4], &m, &k)) { Log("structures: record %d has no other sizes", (int)idx); return false; }
            r[4] = k_BridgeIds[m][(k + 1) % BridgeShapes];
        }
        Vec3f at = *reinterpret_cast<const Vec3f*>(body + 0x7f0);
        /* The new look is spawned first and the old actor deleted once the new one is there (a few
           frames later, ReshapeTick): deleting first made the structure blink and its outline / the
           Y prompt flicker (seen in a Switch video). */
        FinishReshape();   /* a swap still running (fast presses): finish it now */
        s_Reshape.n = 0;
        ForEachActor([&](u8* a) {
            if (s_Reshape.n < 4 && VtOf(a) == BuildingBodyVt && *reinterpret_cast<u32*>(a + ActorRecordIndex) == idx && StructureOfBody(a))
                s_Reshape.old[s_Reshape.n++] = *reinterpret_cast<u32*>(a + 0x230);
        });
        if (rotate) {
            if (s_Pop.active) if (u8* a = s_Pop.h ? ActorOfHandle(s_Pop.h) : nullptr) SetLook(a, 1.0f, s_Pop.y);
            s_Pop = {};
            s_Pop.active = true; s_Pop.idx = (int)idx; s_Pop.n = s_Reshape.n;
            for (int i = 0; i < s_Reshape.n; i++) {
                s_Pop.old[i] = s_Reshape.old[i];
                if (u8* a = ActorOfHandle(s_Reshape.old[i])) SetLook(a, 0.85f, reinterpret_cast<Vec3f*>(a + 0x7f0)->y - 1.5f);
            }
        }
        WriteRecord((int)idx, raw);
        memcpy(s_New[n].raw, raw, 0x14);
        s_Reshape.newH = SpawnRecordActor((int)idx);
        s_Reshape.frames = 0;
        s_Reshape.active = true;
        s_ShapePending |= 1ull << idx;
        Log("structures: record %d %s -> rotation %d id %x", (int)idx, rotate ? "turned" : "resized", r[3], r[4]);
        if (rotate) PlayEditFx(nullptr, "90度回転させる", at);
        else PlayEditFx("道造成煙：角丸", "橋坂外観変更", at);   /* (auditioned on a Switch, all approved) */
        return true;
    }

    /* ---- Y removes a bridge / incline: its record is emptied, its actor deleted now, its walkable shape
       cleared once the editor has closed. The editor's closing restore brings the record (and the actor)
       back: KeepRemoved empties it again and deletes the actor of that record. ---- */
    struct RemovedStructure { int idx; u8 raw[0x14]; };
    static RemovedStructure s_Removed[16];
    static int s_RemovedCount = 0;

    static void DeleteActorsOfRecord(int idx) {
        using DelFn = void (*)(u8*);
        ForEachActor([&](u8* a) {
            if (VtOf(a) == BuildingBodyVt && *reinterpret_cast<u32*>(a + ActorRecordIndex) == (u32)idx && (StructureOfBody(a) || RemovableBuildingOfBody(a)))
                reinterpret_cast<DelFn>(Main(ActorRequestDelete))(a);
        });
    }

    /* Removal like furniture being put away: the actors shrink (scale at body +0x818, and sink a
       little in case the scale is only read when built) for a few frames, then are deleted. */
    struct Shrinking { u32 h; int f; float y; };
    static Shrinking s_Shrink[8];
    static int s_ShrinkCount = 0;
    constexpr int ShrinkFrames = 6;   /* 12, then 8, were a bit slow (sai) */
    static void ShrinkActorsOfRecord(int idx) {
        using DelFn = void (*)(u8*);
        ForEachActor([&](u8* a) {
            if (VtOf(a) != BuildingBodyVt || *reinterpret_cast<u32*>(a + ActorRecordIndex) != (u32)idx) return;
            if (s_ShrinkCount >= 8) { reinterpret_cast<DelFn>(Main(ActorRequestDelete))(a); return; }
            s_Shrink[s_ShrinkCount++] = { *reinterpret_cast<u32*>(a + 0x230), 0, reinterpret_cast<Vec3f*>(a + 0x7f0)->y };
        });
    }
    void ShrinkTick() {
        using DelFn = void (*)(u8*);
        for (int i = 0; i < s_ShrinkCount; i++) {
            Shrinking& k = s_Shrink[i];
            u8* a = ActorOfHandle(k.h);
            if (a && ++k.f < ShrinkFrames) {
                float t = (float)k.f / ShrinkFrames;
                float sc = 1.0f - t * t;             /* eases in, like the game's put-away */
                if (sc < 0.02f) sc = 0.02f;
                Vec3f* s3 = reinterpret_cast<Vec3f*>(a + 0x818);
                *s3 = { sc, sc, sc };
                float y = k.y - 4.0f * t * t;
                reinterpret_cast<Vec3f*>(a + 0x7f0)->y = y;
                reinterpret_cast<Vec3f*>(a + 0x7fc)->y = y;
                continue;
            }
            if (a) reinterpret_cast<DelFn>(Main(ActorRequestDelete))(a);
            s_Shrink[i--] = s_Shrink[--s_ShrinkCount];
        }
    }

    bool RemoveStructure(u8* body) {
        u8* recs = BuildingRecords();
        int type = StructureOfBody(body);
        if (!type) type = RemovableBuildingOfBody(body);
        u32 idx = body ? *reinterpret_cast<u32*>(body + ActorRecordIndex) : 99;
        if (!recs || !type || idx >= 47 || s_RemovedCount >= 16) return false;
        RemovedStructure& r = s_Removed[s_RemovedCount++];
        r.idx = (int)idx;
        memcpy(r.raw, recs + idx * 0x14, 0x14);
        for (int i = 0; i < s_NewCount; i++) if (s_New[i].idx == (int)idx) s_New[i--] = s_New[--s_NewCount];
        for (int i = 0; i < s_OverrideCount; i++) if (s_Overrides[i].idx == (int)idx) s_Overrides[i--] = s_Overrides[--s_OverrideCount];
        /* every actor of this record, not just the one under the cursor: a building that was just
           moved still has its old actor for a moment, which stayed visible and needed more presses */
        Vec3f at = *reinterpret_cast<const Vec3f*>(body + 0x7f0);
        PlayEditFx("道造成消失煙", "道造成消失煙", at);   /* the path tool's vanishing smoke + its sound ("put away" was a faint click) */
        ShrinkActorsOfRecord((int)idx);   /* (includes <body>; deleted once shrunk - deleting one twice isn't safe) */
        u8 zero[0x14] = {};
        WriteRecord((int)idx, zero);
        s_ShapePending |= 1ull << idx;
        s_RemovePending |= 1ull << idx;
        Log("spawn: removed %s record %d (id %x)", type == BridgeBuildingType ? "bridge" : type == InclineBuildingType ? "incline" : "building", (int)idx,
            *reinterpret_cast<u16*>(r.raw + 8));
        return true;
    }

    static void KeepRemoved() {
        u8* recs = BuildingRecords();
        if (!recs) return;
        for (int i = 0; i < s_RemovedCount; i++) {
            const u16* now = reinterpret_cast<const u16*>(recs + s_Removed[i].idx * 0x14);
            const u16* was = reinterpret_cast<const u16*>(s_Removed[i].raw);
            if (now[0] == 0) continue;                                    /* still empty */
            if (now[0] != was[0] || now[1] != was[1] || now[2] != was[2] || now[4] != was[4]) {
                Log("spawn: removed record %d reused - forgotten", s_Removed[i].idx);
                s_Removed[i--] = s_Removed[--s_RemovedCount];
                continue;
            }
            u8 zero[0x14] = {};
            WriteRecord(s_Removed[i].idx, zero);
            DeleteActorsOfRecord(s_Removed[i].idx);
            s_ShapePending |= 1ull << s_Removed[i].idx;
            s_RemovePending |= 1ull << s_Removed[i].idx;
            Log("spawn: removed record %d came back - emptied again", s_Removed[i].idx);
        }
    }

    /* ---- XLink (effects ELink / sounds SLink) on the editor cursor ----
       The editor cursor has an XLink user "FtrEditCursor" in both ELink2DB and SLink2DB (found by
       reading the DBs: keywords such as 道造成消失煙 (path-building vanish smoke), 坂の撤去 (incline
       removal), しまう (put away), 90度回転させる (rotate 90), 橋坂外観変更 (bridge/incline look change)).
       The game emits one like 0x13ba1d8: asset = 0x3af5ec0(*(*(obj+0x30)+0x18), name); then
       0x3af47b0(obj, {SafeString-ish vt (*(main+0x4acadf8)+0x10), asset, 0}, &handle). The cursor's
       XLink objects: 0x13b9f78(area fx, owner) runs while editing; owner+8 is the ELink object it emits
       the edit-area effect on. */
    static u8* s_XOwner = nullptr;   /* captured from 0x13b9f78's second argument */
    static u8* s_XArea = nullptr;
    void CaptureEditXLink(u8* area, u8* owner) { s_XArea = area; s_XOwner = owner; }

    static bool MemOk(uintptr_t a, size_t len, bool write);
    static bool XLinkEmit(u8* obj, const char* name, const Vec3f* at = nullptr, float scale = 1.0f) {
        if (!obj || !name) return false;
        u8* a30 = *reinterpret_cast<u8**>(obj + 0x30);
        void* ctx = a30 ? *reinterpret_cast<void**>(a30 + 0x18) : nullptr;
        if (!ctx) return false;
        void* asset = reinterpret_cast<void* (*)(void*, const char*)>(Main(0x3af5ec0))(ctx, name);
        if (!asset) return false;
        struct { uintptr_t vt; void* asset; u8 flag; u8 pad[7]; } q = { *reinterpret_cast<uintptr_t*>(Main(0x4acadf8)) + 0x10, asset, 0, {} };
        struct { u64 h; u32 x; u32 pad; } out = {};
        reinterpret_cast<void (*)(u8*, void*, void*)>(Main(0x3af47b0))(obj, &q, &out);
        /* An effect plays where it's told: like 0x13ba100 (the edit-area effect), the started event
           (handle +0, checked by its id at +0x20 == handle +8) gets a position at +0xa8, scale 1 at
           +0xb4, and flag 4 at +0x184 (use that transform). Without it nothing showed. */
        u8* ev = reinterpret_cast<u8*>(out.h);
        if (at && ev && MemOk(reinterpret_cast<uintptr_t>(ev), 0x188, true) && *reinterpret_cast<u32*>(ev + 0x20) == out.x) {
            *reinterpret_cast<float*>(ev + 0xa8) = at->x;
            *reinterpret_cast<float*>(ev + 0xac) = at->y;
            *reinterpret_cast<float*>(ev + 0xb0) = at->z;
            *reinterpret_cast<float*>(ev + 0xb4) = scale;
            *reinterpret_cast<float*>(ev + 0xb8) = scale;
            *reinterpret_cast<float*>(ev + 0xbc) = scale;
            *reinterpret_cast<u32*>(ev + 0x184) |= 4;
        }
        return true;
    }

    /* Recording of XLink keyword searches and emits (dbg "xlog"), to find which XLink object plays
       which keyword (the editor cursor's, for the effects / sounds above). */
    struct XSearchRec { uintptr_t ctx; char name[48]; uintptr_t asset; };
    /* The editor's own keyword table (main+0x4a4c328, 37 names): only these are recorded, the rest of
       the game searches ~150 keywords a second (footsteps, ripples...). */
    constexpr u32 EditKeywordTable = 0x4a4c328;
    constexpr int EditKeywordCount = 37;
    static bool IsEditKeyword(const char* name) {
        const char* const* t = reinterpret_cast<const char* const*>(Main(EditKeywordTable));
        for (int i = 0; i < EditKeywordCount; i++) if (t[i] && strcmp(t[i], name) == 0) return true;
        return false;
    }
    struct XEmitRec { uintptr_t obj, ctx, asset; };
    static XSearchRec s_XSearch[256];
    static XEmitRec s_XEmit[256];
    static std::atomic<u32> s_XSearchN{0}, s_XEmitN{0};
    static std::atomic<int> s_XRecord{0};
    static uintptr_t XCtxOf(u8* obj) {
        if (!obj) return 0;
        u8* a30 = *reinterpret_cast<u8**>(obj + 0x30);
        return a30 ? *reinterpret_cast<uintptr_t*>(a30 + 0x18) : 0;
    }
    /* The editor cursor's XLink objects, noted whenever the game looks up one of the editor's own
       keywords (hovering / grabbing something): the sound one (SLink) finds it, the effect one
       (ELink) doesn't. An XLink object sits 0x100 after its lookup context (checked: obj+0x30 ->
       +0x18 is the context again). */
    static uintptr_t s_EditSCtx = 0, s_EditECtx = 0;
    static u8* XObjOfCtx(uintptr_t ctx) {
        if (!ctx || !MemOk(ctx + 0x100, 0x38, false)) return nullptr;
        u8* obj = reinterpret_cast<u8*>(ctx + 0x100);
        u8* a30 = *reinterpret_cast<u8**>(obj + 0x30);
        if (!a30 || !MemOk(reinterpret_cast<uintptr_t>(a30), 0x20, false)) return nullptr;
        return XCtxOf(obj) == ctx ? obj : nullptr;
    }
    void XRecSearch(void* ctx, const char* name, void* asset) {
        if (!name) return;
        bool edit = EditorMap() && LiveLayout(s_EditLayout) && (u8)name[0] >= 0x80 && IsEditKeyword(name);
        /* the first ones of an editor session are kept while they're alive: later lookups of these
           keywords also come from other objects (a hovered bridge), and sounds sent to those went
           nowhere once the bridge was rebuilt or removed (Switch: no turn / resize / remove sounds) */
        if (edit) {
            static u8* s_CtxLayout = nullptr;   /* a new editor session: capture again */
            if (s_CtxLayout != s_EditLayout) { s_CtxLayout = s_EditLayout; s_EditSCtx = s_EditECtx = 0; }
            if (asset) { if (!XObjOfCtx(s_EditSCtx)) s_EditSCtx = (uintptr_t)ctx; }
            else if (!XObjOfCtx(s_EditECtx)) s_EditECtx = (uintptr_t)ctx;
        }
        if (!s_XRecord.load(std::memory_order_relaxed) || !edit) return;
        u32 i = s_XSearchN.fetch_add(1) & 255;
        s_XSearch[i].ctx = (uintptr_t)ctx;
        s_XSearch[i].asset = (uintptr_t)asset;
        strncpy(s_XSearch[i].name, name, sizeof(s_XSearch[i].name) - 1);
        s_XSearch[i].name[sizeof(s_XSearch[i].name) - 1] = 0;
    }
    void XRecEmit(u8* obj, void* q) {
        if (!s_XRecord.load(std::memory_order_relaxed) || !obj || !q) return;
        uintptr_t asset = *reinterpret_cast<uintptr_t*>(reinterpret_cast<u8*>(q) + 8);
        u32 ns = s_XSearchN.load();
        bool ours = false;
        for (u32 k = (ns > 256 ? ns - 256 : 0); k < ns && !ours; k++) if (s_XSearch[k & 255].asset == asset) ours = true;
        if (!ours) return;
        u32 i = s_XEmitN.fetch_add(1) & 255;
        s_XEmit[i] = { (uintptr_t)obj, XCtxOf(obj), asset };
    }

    /* dbg "emit <obj> <keyword>": run on the game thread (BridgeTick). */
    static std::atomic<int> s_EmitPending{0};
    static uintptr_t s_EmitObj = 0;
    static char s_EmitName[96];
    static void RunPendingEmit() {
        if (!s_EmitPending.load(std::memory_order_acquire)) return;
        s_XRecord.store(0);   /* (our own emit isn't recorded) */
        Vec3f at = {};
        if (LiveLayout(s_EditLayout)) at = *reinterpret_cast<const Vec3f*>(s_EditLayout + LayoutCursor);
        bool ok = XLinkEmit(reinterpret_cast<u8*>(s_EmitObj), s_EmitName, &at);
        Log("xlink: emit '%s' on %lx -> %s", s_EmitName, (unsigned long)s_EmitObj, ok ? "found + emitted" : "keyword not found");
        s_EmitPending.store(0, std::memory_order_release);
    }

    /* The editor's own effects / sounds for our actions (names from the FtrEditCursor users, auditioned
       on a Switch: the sound plays; effects need a position). */
    /* Effects 2.5x the path tool's size (sai: "make the clouds bigger"). Sounds get no position:
       the effect event's layout (+0xa8 position...) isn't a sound event's, and with it most sounds
       didn't play (sai: "barely no sounds"); the dbg test without a position played fine. */
    /* Played from BridgeTick (the game's frame): turn / remove run in the controller hook, and sounds
       started there never played (Switch: silent turns), while the same sounds sent through dbg
       (played in BridgeTick) did. */
    struct FxReq { const char* effect; const char* sound; Vec3f at; };
    static FxReq s_FxQueue[8];
    static int s_FxCount = 0;
    static void PlayEditFx(const char* effect, const char* sound, const Vec3f& at) {
        if (s_FxCount < 8) s_FxQueue[s_FxCount++] = { effect, sound, at };
    }
    static void RunEditFx() {
        for (int i = 0; i < s_FxCount; i++) {
            const FxReq& r = s_FxQueue[i];
            /* 8 up: at the structure's middle the smoke looked like it came from underneath it */
            Vec3f up = { r.at.x, r.at.y + 8.0f, r.at.z };
            if (r.effect) { if (u8* e = XObjOfCtx(s_EditECtx)) XLinkEmit(e, r.effect, &up, 2.5f); }
            if (r.sound) {
                u8* so = XObjOfCtx(s_EditSCtx);
                if (so) XLinkEmit(so, r.sound);
                else Log("xlink: no sound player yet (hover something in the editor once)");
            }
        }
        s_FxCount = 0;
    }

    /* Houses and shops: the game builds them at the cliff level's height, so on the beach (lower) they
       hung in the air (seen on a Switch). Every 20 frames each building on the main island whose middle
       isn't on the ground is moved there (body and its parts, like a grab; within 20, so a building
       half over a cliff edge is left alone). Bridges / inclines and a held building are skipped. */
    static void SnapBuildingsToGround() {
        static int s_Tick = 0;
        if (++s_Tick < 20) return;
        s_Tick = 0;
        if (!EditorMapStage()) return;
        struct Snap { u8* body; float x, z, dy; };
        Snap snaps[48];
        int n = 0;
        using HeightFn = float (*)(float, float*, u32*, u64);
        ForEachActor([&](u8* a) {
            if (n >= 48 || VtOf(a) != BuildingBodyVt || StructureOfBody(a)) return;
            BldComp* c = CompOf(a);
            if (c && c->grabbed) return;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + 0x7f0);
            float pt[3] = { p->x, 0.0f, p->z };
            float g = reinterpret_cast<HeightFn>(exl::util::modules::GetTargetOffset(GroundHeight))(0.0f, pt, nullptr, 0);
            float dy = g - p->y;
            /* only down (the beach): moving up lifted a house a little above the ground where the
               ground under its middle is uneven (sai's house) */
            if (dy > -0.3f || dy < -20.0f) return;
            snaps[n++] = { a, p->x, p->z, dy };
        });
        if (!n) return;
        ForEachActor([&](u8* a) {
            u32 pair = PartPair(a);
            if (!pair) return;
            const Vec3f* p = reinterpret_cast<const Vec3f*>(a + (VtOf(a) == SignVt ? 0x160 : pair));
            for (int i = 0; i < n; i++) {
                float dx = p->x - snaps[i].x, dz = p->z - snaps[i].z;
                if (dx < -8 || dx > 8 || dz < -8 || dz > 8) continue;
                for (u32 off : { pair, pair + 0xcu }) reinterpret_cast<Vec3f*>(a + off)->y += snaps[i].dy;
                break;
            }
        });
        static int s_Logged = 0;
        if (s_Logged < 10) { s_Logged++; Log("buildings: %d moved to the ground (first by %d)", n, (int)snaps[0].dy); }
    }

    /* Game thread, every frame. */
    void BridgeTick() {
        RunPendingEmit();
        RunEditFx();
        ReshapeTick();
        PendingSpawnTick();
        SnapBuildingsToGround();
        KeepRemoved();
        KeepNew();
        KeepOverrides();
        RebuildShapes();
        if (s_BridgeSwap.frames >= 0 && ++s_BridgeSwap.frames >= 4) FinishBridgeSwap();
        if (s_HomeMenu && !EditorMap()) s_HomeMenu = nullptr;
        if (u8* m = s_RefillMenu) {
            s_RefillMenu = nullptr;
            if (m == s_HomeMenu && EditorMap() && *reinterpret_cast<s32*>(m + MenuTabIndex) >= BridgeTab)
                reinterpret_cast<void (*)(u8*)>(Main(0x340de50))(m);
        }
    }

    /* 0x13ba988 (edit object of a handle) for our buildings: handle +8 type, +0x10 actor handle. */
    void* BuildingEditObject(const u8* h) {
        if (!h) return nullptr;
        u32 type = *reinterpret_cast<const u32*>(h + 8);
        if (type != 2 && type != 3) return nullptr;
        return CompOf(ActorOfHandle(*reinterpret_cast<const u32*>(h + 0x10)));
    }

    /* Name + icon of the two DIY app slots: the editor's slot shows "Island Editor", the other
       keeps its own. Runs from the phone's unlock check and the watch thread. */
    static void ApplyEditorApp(bool on) {
        Mem m;
        for (int id = DiyAppId; id <= DiyPlusAppId; id++) {
            int k = id - DiyAppId;
            bool editor = on && id == s_EditorSlot;
            const char** name = AppNameSlot(id);
            if (name && *name) {
                if (!s_DiyLabel[k] && *name != EditorAppLabel) s_DiyLabel[k] = *name;
                const char* want = editor ? EditorAppLabel : s_DiyLabel[k];
                if (want && *name != want) *name = want;
            }
            float* frame = reinterpret_cast<float*>(m.Rw(AppIconFrames)) + id;
            if (s_DiyFrame[k] < 0 && *frame != EditorIconFrame) s_DiyFrame[k] = *frame;
            float want = editor ? EditorIconFrame : s_DiyFrame[k];
            if (want >= 0 && *frame != want) {
                *frame = want;
                armDCacheFlush(frame, sizeof(float));
            }
        }
        /* HHP's exterior menu shows the vacation home's theme in a banner. Island houses have none
           (theme 0xffff), which the game shows as theme 0, "Construction Job Site" (System
           OneRoom_ThemeName 0000). With the editor on, the "no theme" path skips setting the text
           (0x345d7cc "mov w2, wzr" -> b 0x345d81c); homes with a real theme never take it. */
        SetWord(ThemeBannerNone, ThemeBannerNoneOrig, ThemeBannerSkip, on);
    }

    void SetEditorSlot(int slot) {
        if (slot == s_EditorSlot) return;
        s_EditorSlot = slot;
        ApplyEditorApp(EditorAppOn());
        Log("island editor: phone app uses app slot %d", slot);
    }

    void EditorAppLaunched(u8* result) {
        result[8] = 1;   /* close the phone; flow kind (+4) stays "none" */
        s_EditorPending = true;
        Log("phone: Island Editor app opened");
    }

    /* Player button handler, every frame before it runs. */
    void EditorTick(u8* player) {
        if (!player) return;
        s_LocalPlayer = player;
        if (!s_EditorPending) return;
        s_EditorPending = false;
        if (!EditorMap()) {
            Log("island editor: only on the main island");
            return;
        }
        u8* mgr = *reinterpret_cast<u8**>(exl::util::modules::GetTargetOffset(UiMgrGlobal));
        if (!mgr) return;
        struct { u8 idx; u8 pad[7]; u64 rest[3]; } param = {};
        u8 idx = player[0x1ee8];
        param.idx = idx < 8 ? idx : 0xff;
        using RequestFn = void (*)(u8*, u32, void*, u32);
        s_EditOpening = true;
        reinterpret_cast<RequestFn>(exl::util::modules::GetTargetOffset(UiRequest))(mgr, ExteriorEditScene, &param, 0);
        Log("island editor: requested the exterior editor");
    }

    /* ---- Faster terraforming ----
       The cliff/river tool states (ready, build, remove, dig, fill) wait until the character's
       animation has finished. While one of them runs, the playing animation's rate
       (player + 0x60 -> controller, +0x1138 -> active slot, +0xa8 float, normally 1.0) is set to
       terraform_speed every frame; the game eases it back towards 1.0 on its own afterwards.
       (Updating the controller several times per frame does nothing: it advances by time.) */

    /* toggles.txt "terraform_speed=N" (1-60), re-read every second like the switches. */
    static float s_TerraformSpeed = 60.0f;

    static s64 s_LastTerraformTick = 0;

    static u8* s_SpedPlayer = nullptr;   /* set while a state of this player runs sped up */

    static void SetAnimRate(u8* player, float rate) {
        u8* ctrl = *reinterpret_cast<u8**>(player + 0x60);
        if (!ctrl) return;
        u8* slot = *reinterpret_cast<u8**>(ctrl + 0x1138);
        if (slot) *reinterpret_cast<float*>(slot + 0xa8) = rate;
    }

    void FastTerraform(u8* player) {
        if (!player || !FeatureOn("fast_terraform")) return;
        s_LastTerraformTick = nn::os::GetSystemTick().GetInt64Value();
        SetAnimRate(player, s_TerraformSpeed);
        s_SpedPlayer = player;
    }

    /* Leaving a sped-up state: the game would only ease the rate back by ~0.05 per frame
       (about 20 s from 60x), so put it straight back to normal. */
    void EndFastTerraform(u8* player) {
        if (player && player == s_SpedPlayer) {
            SetAnimRate(player, 1.0f);
            s_SpedPlayer = nullptr;
        }
    }

    /* Player state 12 is a generic "start acting" state (also used outside terraforming), so it is
       only sped up within 10 seconds of a terraform state. */
    void FastTerraformStart(u8* player) {
        if (!player || !FeatureOn("fast_terraform") || !s_LastTerraformTick) return;
        s64 now = nn::os::GetSystemTick().GetInt64Value();
        if (now - s_LastTerraformTick < 10 * nn::os::GetSystemTickFrequency()) {
            SetAnimRate(player, s_TerraformSpeed);
            s_SpedPlayer = player;
        }
    }

    u32 PhoneAppHandler() { return IslandDesignerHandler; }
    const char* PhoneAppEntry() { return AppEntryPoint; }

    static void SetFeature(Feature& f, bool enable) {
        if (enable && strcmp(f.key, "slumber_terraform") == 0) RefreshTerraformTable();
        if (f.count == 0) {
            if (strcmp(f.key, "slumber_phone_app") == 0) ApplyPhoneApp(enable);
            if (strcmp(f.key, "island_editor") == 0) ApplyEditorApp(enable);
            if (strcmp(f.key, "slumber_hud") == 0) SetPlusModeCheck(enable);
            if (strcmp(f.key, "slumber_weather") == 0) {
                /* Off: map loads go back to the island's own weather; on: wait for the next pick. */
                if (!enable) { s_WeatherPicked = false; SetKeepWeather(false); }
                else if (s_WeatherPicked) SetKeepWeather(true);
            }
            if (f.applied != enable) Log("%s (%s): %s", f.key, f.label, enable ? "ON" : "OFF");
            f.applied = enable;
            return;
        }
        Mem m;
        int state = ReadState(m, f);
        if (state < 0) {
            Log("%s: game code doesn't match the expected words - left untouched", f.key);
            return;
        }
        if ((state == 1) == enable) { f.applied = enable; return; }

        /* Enable: fill code caves (orig == 0) before writing the jumps into them.
           Disable: restore the jumps first, then clear the caves. */
        for (int pass = 0; pass < 2; pass++) {
            bool caves = (pass == 0) == enable;
            for (size_t i = 0; i < f.count; i++) {
                const PatchWord& w = f.words[i];
                if ((w.orig == 0) != caves) continue;
                WriteWord(m, w.offset, enable ? w.patched : w.orig);
            }
        }
        f.applied = enable;
        Log("%s (%s): %s", f.key, f.label, enable ? "ON" : "OFF");
    }

    /* ---- toggles.txt ---- */

    static char s_LastToggles[4096];

    static void WriteDefaultToggles() {
        char buf[4096];
        int len = snprintf(buf, sizeof(buf),
            "# SlumberToggle switches: 1 = on, 0 = off. Changes apply within a second.\n");
        for (size_t i = 0; i < FeatureCount; i++)
            len += snprintf(buf + len, sizeof(buf) - len, "%s=%d    # %s\n",
                            g_Features[i].key, g_Features[i].wanted ? 1 : 0, g_Features[i].label);
        len += snprintf(buf + len, sizeof(buf) - len, "terraform_speed=%d    # animation speed while terraforming (1-60)\n",
                        (int)s_TerraformSpeed);
        len += snprintf(buf + len, sizeof(buf) - len, "quick_start=%d    # press A for you on the title screen until you're on the island\n",
                        s_QuickStart ? 1 : 0);
        len += snprintf(buf + len, sizeof(buf) - len,
                        "app_flip_y=%d    # Slumber app screen: -1 auto; 1 or 0 if it shows upside down\n", app::FlipOverride());
        if (!s_SdReady) return;
        nn::fs::DeleteFile(TogglesPath);
        nn::fs::CreateFile(TogglesPath, len);
        nn::fs::FileHandle h;
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, TogglesPath, nn::fs::OpenMode_Write))) {
            nn::fs::WriteFile(h, 0, buf, len, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
        }
        /* Remember what we wrote so the watch thread doesn't treat it as an outside edit. */
        snprintf(s_LastToggles, sizeof(s_LastToggles), "%s", buf);
    }

    /* Reads toggles.txt; returns true if its content changed since the last read. */
    static bool ReadToggles() {
        nn::fs::FileHandle h;
        if (R_FAILED(nn::fs::OpenFile(&h, TogglesPath, nn::fs::OpenMode_Read))) return false;
        long size = 0;
        nn::fs::GetFileSize(&size, h);
        char buf[4096] = {};
        if (size > (long)sizeof(buf) - 1) size = sizeof(buf) - 1;
        nn::fs::ReadFile(h, 0, buf, size);
        nn::fs::CloseFile(h);
        if (strcmp(buf, s_LastToggles) == 0) return false;
        strcpy(s_LastToggles, buf);

        for (char* line = buf; line && *line;) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            char* eq = strchr(line, '=');
            if (line[0] != '#' && eq) {
                *eq = 0;
                for (size_t i = 0; i < FeatureCount; i++)
                    if (strcmp(line, g_Features[i].key) == 0) g_Features[i].wanted = eq[1] == '1';
                if (strcmp(line, "app_flip_y") == 0) {
                    int flip = eq[1] == '-' ? -1 : eq[1] == '1' ? 1 : 0;
                    if (flip != app::FlipOverride()) {
                        app::SetFlipOverride(flip);
                        Log("app_flip_y = %d", flip);
                    }
                }
                if (strcmp(line, "quick_start") == 0) {
                    bool on = eq[1] != '0';
                    if (on != s_QuickStart) { s_QuickStart = on; Log("quick_start = %d", on ? 1 : 0); }
                }
                if (strcmp(line, "terraform_speed") == 0) {
                    int v = 0;
                    for (const char* c = eq + 1; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
                    if (v >= 1 && v <= 60 && (float)v != s_TerraformSpeed) {
                        s_TerraformSpeed = (float)v;
                        Log("terraform_speed = %d", v);
                    }
                }
            }
            line = next;
        }
        return true;
    }

    static void ApplyAll() {
        for (size_t i = 0; i < FeatureCount; i++)
            if (g_Features[i].wanted != g_Features[i].applied) SetFeature(g_Features[i], g_Features[i].wanted);
    }

    /* ---- in-game menu: island flags named "SLT_<key>" (Island Designer app > Slumber settings) ---- */

    static Feature* FindSlt(const char* name) {
        if (!name || strncmp(name, "SLT_", 4) != 0) return nullptr;
        for (size_t i = 0; i < FeatureCount; i++)
            if (strcmp(name + 4, g_Features[i].key) == 0) return &g_Features[i];
        return nullptr;
    }

    static void SetFromMenu(Feature& f, bool on) {
        f.wanted = on;
        SetFeature(f, on);
        WriteDefaultToggles();  /* saves the current choices */
    }

    /* ---- The Slumber app screen's rows (slumber_app.cpp draws them) ----
       One row per switch, plus the terraforming speed after "fast_terraform". The screen shows
       and changes what is wanted; the switches are applied on the game thread (AppTick, like the
       old talk menu did) and saved to toggles.txt by the watch thread (no SD writes in a frame). */

    struct AppText { const char* key; const char* label; const char* help; };
    static const AppText k_AppText[] = {
        { "slumber_dpad", "Slumber catalog + time", "Slumber Island's catalog and its time / weather screen on the D-pad." },
        { "instant_bridges", "Instant bridges & inclines", "Bridges and inclines are finished the moment you place them." },
        { "trees_anywhere", "Trees anywhere", "Plant trees anywhere." },
        { "outdoor_rugs", "Rugs outdoors", "Place rugs outdoors." },
        { "outdoor_partition_walls", "Partition walls outdoors", "Place partition walls outdoors." },
        { "level4_access", "Level-4 cliffs", "Reach level-4 cliffs." },
        { "slumber_terraform", "Slumber terraforming", "Experimental: terraform with Slumber Island's rules on your island." },
        { "terraform_near_trees", "Terraform near trees", "Experimental: build cliffs and water right next to trees and cliffs." },
        { "slumber_weather", "Slumber weather", "Pick sunny or rain / snow for your island on the Slumber time screen." },
        { "slumber_phone_app", "Slumber phone app", "This app. Off removes it from the phone; toggles.txt or Island Designer's Slumber settings bring it back." },
        { "fast_terraform", "Faster terraforming", "Cliff and water building plays fast, at the speed set below." },
        { "slumber_hud", "Slumber HUD + build modes", "Slumber's fence / helper / bag HUD on your island, + switches the build mode." },
        { "island_editor", "Island editor app", "Happy Home Paradise's outdoor editor on your island: place items anywhere, move houses (needs HHP)." },
        { "unlock_all", "Unlock everything", "All phone apps, construction permits and HHP unlocks." },
        { "cliff_stack", "Stacked cliffs", "The cliff tool stacks a cliff on a flush edge, instantly, with solid walls." },
        { "fences_anywhere", "Fences anywhere", "Place fences anywhere: no 'can't put it here' checks." },
    };
    static const int k_SpeedSteps[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60 };
    constexpr int SpeedStepCount = sizeof(k_SpeedSteps) / sizeof(k_SpeedSteps[0]);

    /* What InstallAppHooks found (logged at startup, once the SD card is up). */
    bool g_AppNvnHook = false;
    int  g_AppNpadHooks = 0;
    bool g_AppAlloc = false;

    static std::atomic<bool> s_AppApply{false};   /* switches changed: apply on the game thread */
    static std::atomic<bool> s_AppSave{false};    /* save toggles.txt from the watch thread */

    static int SpeedRow() {
        for (size_t i = 0; i < FeatureCount; i++)
            if (strcmp(g_Features[i].key, "fast_terraform") == 0) return (int)i + 1;
        return (int)FeatureCount;
    }

    /* Row -> feature index, or -1 for the speed row. */
    static int RowFeature(int row) {
        int speed = SpeedRow();
        return row == speed ? -1 : row < speed ? row : row - 1;
    }

    int AppRowCount() { return (int)FeatureCount + 1; }

    void AppRowGet(int row, AppRow* out) {
        int f = RowFeature(row);
        if (f < 0) {
            *out = { "Terraform speed", "How fast terraforming plays while Faster terraforming is on (1x to 60x).",
                     true, false, (int)s_TerraformSpeed };
            return;
        }
        if (f >= (int)FeatureCount) { *out = { "", "", false, false, 0 }; return; }
        const Feature& feat = g_Features[f];
        *out = { feat.key, feat.label, false, feat.wanted, 0 };
        for (const AppText& t : k_AppText)
            if (strcmp(t.key, feat.key) == 0) { out->label = t.label; out->help = t.help; }
    }

    void AppRowChange(int row, int dir) {
        int f = RowFeature(row);
        if (f < 0) {
            int cur = (int)s_TerraformSpeed, next = cur;
            if (dir >= 0) {
                next = dir == 0 ? k_SpeedSteps[0] : k_SpeedSteps[SpeedStepCount - 1];
                for (int v : k_SpeedSteps) if (v > cur) { next = v; break; }
            } else {
                next = k_SpeedSteps[0];
                for (int v : k_SpeedSteps) if (v < cur) next = v;
            }
            if (next != cur) {
                s_TerraformSpeed = (float)next;
                s_AppSave = true;
                Log("app: terraform_speed = %d", next);
            }
            return;
        }
        if (f >= (int)FeatureCount) return;
        Feature& feat = g_Features[f];
        bool want = dir == 0 ? !feat.wanted : dir > 0;
        if (want == feat.wanted) return;
        feat.wanted = want;
        s_AppApply = true;
        Log("app: %s -> %s", feat.key, want ? "ON" : "OFF");
    }

    /* Game thread, every frame. */
    void AppTick() {
        if (!s_AppApply.exchange(false)) return;
        ApplyAll();
        s_AppSave = true;
    }

    /* ---- live debug channel ----
       The watch thread reads slumbertoggle/cmd.txt (4x a second), runs its lines, writes the answers
       to slumbertoggle/dbg_out.txt and deletes cmd.txt (work-tools/dbg.py does the round trip).
       Nothing stops the game; every address is checked with svcQueryMemory first.
       Addresses: names main, cam, player, layout, recs, hex numbers, + / -, [expr] = u64 at expr.
         peek <addr> [words]          hex + float per word
         poke <addr> <hex32>...       pokef <addr> <float>...
         hold <addr> <float>          written every editor frame (8 max); unhold clears all
         snap <addr> <bytes>          remember a block;  diff  lists words that changed since */
    constexpr const char* CmdPath    = "slsd:/slumbertoggle/cmd.txt";
    constexpr const char* DbgOutPath = "slsd:/slumbertoggle/dbg_out.txt";
    static char s_DbgOut[0x8000];
    static size_t s_DbgLen = 0;
    struct DbgHold { uintptr_t addr; float v; };
    static DbgHold s_Holds[8];
    static std::atomic<int> s_HoldCount{0};
    alignas(16) static u8 s_DbgSnap[0x40000];
    static uintptr_t s_DbgSnapAddr = 0;
    static size_t s_DbgSnapLen = 0;

    static void DbgPrint(const char* fmt, ...) {
        if (s_DbgLen >= sizeof(s_DbgOut) - 2) return;
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(s_DbgOut + s_DbgLen, sizeof(s_DbgOut) - s_DbgLen - 1, fmt, args);
        va_end(args);
        if (n < 0) return;
        s_DbgLen += (size_t)n;
        if (s_DbgLen > sizeof(s_DbgOut) - 2) s_DbgLen = sizeof(s_DbgOut) - 2;
        s_DbgOut[s_DbgLen++] = '\n';
    }

    static bool MemOk(uintptr_t a, size_t len, bool write) {
        while (len) {
            MemoryInfo mi = {};
            u32 pi = 0;
            if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return false;
            if (!(mi.perm & (write ? 2 : 1)) || mi.type == 0) return false;
            uintptr_t end = mi.addr + mi.size;
            if (end <= a) return false;
            size_t chunk = end - a < len ? end - a : len;
            a += chunk; len -= chunk;
        }
        return true;
    }

    static const char* SkipWs(const char* p) { while (*p == ' ' || *p == '\t') p++; return p; }

    static bool DbgExpr(const char*& p, uintptr_t* out);
    static bool DbgTerm(const char*& p, uintptr_t* out) {
        p = SkipWs(p);
        if (*p == '[') {
            p++;
            uintptr_t a;
            if (!DbgExpr(p, &a)) return false;
            p = SkipWs(p);
            if (*p != ']') return false;
            p++;
            if (!MemOk(a, 8, false)) { DbgPrint("! cannot read %lx", (unsigned long)a); return false; }
            *out = *reinterpret_cast<const uintptr_t*>(a);
            return true;
        }
        struct { const char* name; uintptr_t v; } names[] = {
            { "main", exl::util::modules::GetTargetStart() }, { "cam", (uintptr_t)s_DbgCam },
            { "player", (uintptr_t)s_LocalPlayer }, { "layout", (uintptr_t)s_EditLayout },
            { "recs", (uintptr_t)BuildingRecords() }, { "xowner", (uintptr_t)s_XOwner }, { "xarea", (uintptr_t)s_XArea },
            { "xe", (uintptr_t)XObjOfCtx(s_EditECtx) }, { "xs", (uintptr_t)XObjOfCtx(s_EditSCtx) },
            { "mod", exl::util::modules::GetSelfStart() },
        };
        for (auto& n : names) {
            size_t l = strlen(n.name);
            if (strncmp(p, n.name, l) == 0 && !((p[l] >= 'a' && p[l] <= 'z') || (p[l] >= '0' && p[l] <= '9'))) {
                p += l; *out = n.v;
                return true;
            }
        }
        char* end = nullptr;
        unsigned long long v = strtoull(p, &end, 16);
        if (end == p) return false;
        p = end; *out = (uintptr_t)v;
        return true;
    }
    static bool DbgExpr(const char*& p, uintptr_t* out) {
        uintptr_t v;
        if (!DbgTerm(p, &v)) return false;
        while (true) {
            p = SkipWs(p);
            if (*p != '+' && *p != '-') break;
            char op = *p++;
            uintptr_t t;
            if (!DbgTerm(p, &t)) return false;
            v = op == '+' ? v + t : v - t;
        }
        *out = v;
        return true;
    }

    /* Editor frames: keep the held values. */
    void DbgApplyHolds() {
        int n = s_HoldCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; i++) *reinterpret_cast<float*>(s_Holds[i].addr) = s_Holds[i].v;
    }

    static void DbgLine(char* line) {
        const char* p = SkipWs(line);
        if (!*p || *p == '#') return;
        char cmd[16] = {};
        int ci = 0;
        while (*p && *p != ' ' && ci < 15) cmd[ci++] = *p++;
        DbgPrint("> %s", line);
        if (strcmp(cmd, "find") == 0) {   /* find <hex bytes> [max hits]: every readable+writable region */
            u8 pat[64];
            int pn = 0;
            p = SkipWs(p);
            while (*p && *p != ' ' && pn < 64) {
                auto hexv = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; };
                int hi = hexv(p[0]), lo = p[1] ? hexv(p[1]) : -1;
                if (hi < 0 || lo < 0) break;
                pat[pn++] = (u8)(hi * 16 + lo);
                p += 2;
            }
            long maxHits = strtol(SkipWs(p), nullptr, 0);
            if (maxHits <= 0) maxHits = 32;
            if (pn < 2) { DbgPrint("! pattern too short"); return; }
            int hits = 0;
            u64 a = 0, scanned = 0;
            while (a < 0x8000000000ull && hits < maxHits) {
                MemoryInfo mi = {};
                u32 pi = 0;
                if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || mi.addr + mi.size <= a) break;
                if ((mi.perm & 3) == 3 && mi.type != 0 && mi.size < 0x100000000ull) {
                    const u8* base = reinterpret_cast<const u8*>(mi.addr);
                    /* The game keeps allocating / freeing while this runs (seen on a Switch: a region
                       shrank mid-scan and the read faulted, crashing the game). Re-check each 4 KB page
                       just before reading it, compare byte by byte (memcmp's wide loads can run past
                       the end), and stop at the first page that isn't readable+writable any more. */
                    u64 okTo = 0;
                    for (u64 o = 0; o + pn <= mi.size && hits < maxHits; o += 2) {
                        if (o + pn > okTo) {
                            MemoryInfo pm = {};
                            u32 ppi = 0;
                            u64 at = mi.addr + ((o + pn - 1) & ~0xfffull);
                            if (R_FAILED(svcQueryMemory(&pm, &ppi, at)) || (pm.perm & 3) != 3 || pm.type == 0) break;
                            okTo = pm.addr + pm.size - mi.addr;
                            if (okTo > ((o + pn - 1) & ~0xfffull) + 0x1000) okTo = ((o + pn - 1) & ~0xfffull) + 0x1000;
                        }
                        if (base[o] != pat[0]) continue;
                        int k = 1;
                        while (k < pn && base[o + k] == pat[k]) k++;
                        if (k != pn) continue;
                        if (base + o == pat) continue;
                        DbgPrint("hit %lx (region %lx +%lx, type %x)", (unsigned long)(mi.addr + o), (unsigned long)mi.addr, (unsigned long)o, mi.type);
                        hits++;
                    }
                    scanned += mi.size;
                }
                a = mi.addr + mi.size;
            }
            DbgPrint("%d hits, %lu MB scanned", hits, (unsigned long)(scanned >> 20));
            return;
        }
        if (strcmp(cmd, "unhold") == 0) { s_HoldCount.store(0); DbgPrint("holds cleared"); return; }
        if (strcmp(cmd, "xrec") == 0) {   /* xrec: clear and start recording XLink searches / emits */
            s_XSearchN.store(0); s_XEmitN.store(0); s_XRecord.store(1); DbgPrint("recording"); return;
        }
        if (strcmp(cmd, "xlog") == 0) {   /* xlog: stop recording, list what was recorded (non-ASCII names, i.e. keywords) */
            s_XRecord.store(0);
            u32 ns = s_XSearchN.load(), ne = s_XEmitN.load();
            DbgPrint("%u searches, %u emits", ns, ne);
            int shown = 0;
            for (u32 k = (ns > 256 ? ns - 256 : 0); k < ns && shown < 120; k++) {
                const XSearchRec& r = s_XSearch[k & 255];
                bool kw = false;
                for (const char* c = r.name; *c; c++) if ((u8)*c >= 0x80) kw = true;
                if (!kw) continue;
                DbgPrint("S ctx %lx asset %lx %s", (unsigned long)r.ctx, (unsigned long)r.asset, r.name); shown++;
            }
            for (u32 k = (ne > 256 ? ne - 256 : 0); k < ne && shown < 240; k++, shown++)
                DbgPrint("E obj %lx ctx %lx asset %lx", (unsigned long)s_XEmit[k & 255].obj, (unsigned long)s_XEmit[k & 255].ctx, (unsigned long)s_XEmit[k & 255].asset);
            return;
        }
        if (strcmp(cmd, "diff") == 0) {
            if (!s_DbgSnapLen || !MemOk(s_DbgSnapAddr, s_DbgSnapLen, false)) { DbgPrint("! no snapshot"); return; }
            const u8* now = reinterpret_cast<const u8*>(s_DbgSnapAddr);
            int n = 0;
            for (size_t o = 0; o + 4 <= s_DbgSnapLen && n < 400; o += 4) {
                u32 a = *reinterpret_cast<const u32*>(s_DbgSnap + o), b = *reinterpret_cast<const u32*>(now + o);
                if (a == b) continue;
                float fa, fb;
                memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                DbgPrint("+%05lx  %08x -> %08x   %.4f -> %.4f", (unsigned long)o, a, b, (double)fa, (double)fb);
                n++;
            }
            DbgPrint("%d words changed%s", n, n >= 400 ? " (list cut at 400)" : "");
            return;
        }
        uintptr_t a;
        if (!DbgExpr(p, &a)) { DbgPrint("! bad address"); return; }
        if (strcmp(cmd, "addr") == 0) { DbgPrint("= %lx (main+%lx)", (unsigned long)a, (unsigned long)(a - exl::util::modules::GetTargetStart())); return; }
        if (strcmp(cmd, "emit") == 0) {   /* emit <obj> <keyword>: XLink keyword on an XLink object (game thread; result in log.txt) */
            if (s_EmitPending.load()) { DbgPrint("! an emit is still pending"); return; }
            if (!MemOk(a, 0x38, false)) { DbgPrint("! cannot read %lx", (unsigned long)a); return; }
            const char* nm = SkipWs(p);
            size_t n = strlen(nm);
            while (n && (nm[n - 1] == '\r' || nm[n - 1] == '\n' || nm[n - 1] == ' ')) n--;
            if (!n || n >= sizeof(s_EmitName)) { DbgPrint("! bad keyword"); return; }
            memcpy(s_EmitName, nm, n); s_EmitName[n] = 0;
            s_EmitObj = a;
            s_EmitPending.store(1, std::memory_order_release);
            DbgPrint("queued (see log.txt)");
            return;
        }
        if (strcmp(cmd, "peek") == 0) {
            long words = strtol(SkipWs(p), nullptr, 0);
            if (words <= 0) words = 16;
            if (words > 1024) words = 1024;
            if (!MemOk(a, words * 4, false)) { DbgPrint("! cannot read %lx", (unsigned long)a); return; }
            for (long i = 0; i < words; i++) {
                u32 w = reinterpret_cast<const u32*>(a)[i];
                float f;
                memcpy(&f, &w, 4);
                DbgPrint("%lx +%03lx  %08x  %.4f", (unsigned long)(a + i * 4), (unsigned long)(i * 4), w, (double)f);
            }
            return;
        }
        if (strcmp(cmd, "snap") == 0) {
            long len = strtol(SkipWs(p), nullptr, 0);
            if (len <= 0 || len > (long)sizeof(s_DbgSnap)) len = sizeof(s_DbgSnap);
            if (!MemOk(a, len, false)) { DbgPrint("! cannot read %lx", (unsigned long)a); return; }
            memcpy(s_DbgSnap, reinterpret_cast<const void*>(a), len);
            s_DbgSnapAddr = a; s_DbgSnapLen = len;
            DbgPrint("snapshot of %lx, %ld bytes", (unsigned long)a, len);
            return;
        }
        bool isF = strcmp(cmd, "pokef") == 0, isHold = strcmp(cmd, "hold") == 0;
        if (strcmp(cmd, "poke") == 0 || isF || isHold) {
            int i = 0;
            p = SkipWs(p);
            while (*p) {
                char* end = nullptr;
                u32 w;
                if (isF || isHold) { float f = strtof(p, &end); memcpy(&w, &f, 4); }
                else w = (u32)strtoul(p, &end, 16);
                if (end == p) break;
                p = SkipWs(end);
                uintptr_t at = a + i * 4;
                if (!MemOk(at, 4, true)) { DbgPrint("! cannot write %lx", (unsigned long)at); return; }
                if (isHold) {
                    int n = s_HoldCount.load();
                    if (n >= 8) { DbgPrint("! 8 holds already"); return; }
                    memcpy(&s_Holds[n].v, &w, 4);
                    s_Holds[n].addr = at;
                    s_HoldCount.store(n + 1, std::memory_order_release);
                }
                *reinterpret_cast<u32*>(at) = w;
                DbgPrint("%s %lx = %08x", isHold ? "held" : "wrote", (unsigned long)at, w);
                i++;
            }
            return;
        }
        DbgPrint("! unknown command '%s'", cmd);
    }

    static void DbgPoll() {
        nn::fs::FileHandle h;
        if (R_FAILED(nn::fs::OpenFile(&h, CmdPath, nn::fs::OpenMode_Read))) return;
        long size = 0;
        nn::fs::GetFileSize(&size, h);
        static char buf[4096];
        if (size > (long)sizeof(buf) - 1) size = sizeof(buf) - 1;
        if (size < 0) size = 0;
        nn::fs::ReadFile(h, 0, buf, size);
        nn::fs::CloseFile(h);
        buf[size] = 0;
        s_DbgLen = 0;
        for (char* line = buf; line && *line;) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            size_t l = strlen(line);
            if (l && line[l - 1] == '\r') line[l - 1] = 0;
            DbgLine(line);
            line = next;
        }
        DbgPrint("# done");
        nn::fs::DeleteFile(DbgOutPath);
        nn::fs::CreateFile(DbgOutPath, s_DbgLen);
        if (R_SUCCEEDED(nn::fs::OpenFile(&h, DbgOutPath, nn::fs::OpenMode_Write))) {
            nn::fs::WriteFile(h, 0, s_DbgOut, s_DbgLen, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(h);
        }
        nn::fs::DeleteFile(CmdPath);
    }

    /* ---- startup ---- */

    alignas(0x1000) static u8 s_ThreadStack[0x4000];
    static nn::os::ThreadType s_Thread;

    static void WatchThread(void*) {
        while (true) {
            for (int q = 0; q < 4; q++) {   /* debug commands 4x a second */
                nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(250));
                DbgPoll();
            }
            FlushLog();
            if (s_AppSave.exchange(false)) WriteDefaultToggles();   /* changes made in the Slumber app */
            if (ReadToggles()) ApplyAll();
            /* The real table is filled during boot; keep the terraforming copy in sync. */
            for (size_t i = 0; i < FeatureCount; i++)
                if (g_Features[i].applied && strcmp(g_Features[i].key, "slumber_terraform") == 0) RefreshTerraformTable();
            /* Same for the phone's app table. */
            ApplyPhoneApp(PhoneAppOn());
            ApplyEditorApp(EditorAppOn());
        }
    }

    void Init() {
        Result rc = nn::fs::MountSdCardForDebug(MountName);
        if (R_FAILED(rc)) {
            Logging.Log("[slumbertoggle] SD mount failed - switches stay as the game started");
        } else {
            nn::fs::CreateDirectory(Dir);
            nn::fs::DeleteFile(LogPath);
            nn::fs::CreateFile(LogPath, 0);
            s_SdReady = true;
        }
        Log("SlumberToggle v0.69-fence1 - main base %lx", exl::util::modules::GetTargetStart());

        Log("Slumber app screen: NVN hook %s, %d controller hooks, allocator %s",
            g_AppNvnHook ? "on" : "MISSING", g_AppNpadHooks, g_AppAlloc ? "found" : "MISSING");

        PrepareTerraformWords();

        /* Current state in memory (an IPS version of a patch may already be applied). */
        Mem m;
        /* Keep creative placements: since 3.0 the save processing at load (SaveProc thread) clears
           field items it considers misplaced, e.g. furniture standing in water (found in Eden with a
           write watchpoint on a moon's tile: 0x1288bf4 calls the remover 0x11e5af4). The call is
           skipped, so items placed with NHSE / the island editor stay where they are. */
        if (*m.Ro(KeepItemsCall) == KeepItemsOrig) { WriteWord(m, KeepItemsCall, Nop); Log("keep_items: load-time item removal skipped"); }
        else Log("keep_items: unexpected code at %x (%08x) - left alone", KeepItemsCall, *m.Ro(KeepItemsCall));
        for (size_t i = 0; i < FeatureCount; i++) {
            int st = ReadState(m, g_Features[i]);
            g_Features[i].applied = st == 1;
            Log("  %s: %s at start", g_Features[i].key, st == 1 ? "on" : st == 0 ? "off" : "MISMATCH");
        }

        if (!s_SdReady) return;
        if (!ReadToggles()) {
            WriteDefaultToggles();
            ReadToggles();
        }
        ApplyAll();

        if (R_SUCCEEDED(nn::os::CreateThread(&s_Thread, WatchThread, nullptr, s_ThreadStack, sizeof(s_ThreadStack), 20))) {  /* nn::os priorities are 0-31 */
            nn::os::SetThreadName(&s_Thread, "SlumberToggle");
            s_LogQueued = true;
            nn::os::StartThread(&s_Thread);
        } else {
            Log("could not start the watch thread - edit toggles.txt then restart instead");
        }
    }
}

extern "C" void nnMain();

HOOK_DEFINE_TRAMPOLINE(MainHook) {
    static void Callback() {
        sl::Init();
        Orig();
    }
};

/* Island ("cLand") event flags. The argument is a name holder: [0] = object, [1] = char* name.
   Offsets are for ACNH 3.0.3 (build FF1D1C05670DB602). */
static const char* FlagName(long* ref) { return ref ? reinterpret_cast<const char*>(ref[1]) : nullptr; }

/* Log menu flag accesses ("SLT_..." names only). */
static void Trace(const char* what, long* ref, u32 value) {
    const char* name = FlagName(ref);
    if (name && strncmp(name, "SLT_", 4) == 0) sl::Log("menu flag %s %s value=%u", what, name, value);
}

/* u16 get - used by EventFlowQueryEventFlagSystemBool/Int for cLand. */
HOOK_DEFINE_TRAMPOLINE(LandFlagGet) {
    static u16 Callback(long* ref) {
        Trace("get", ref, 0);
        if (sl::Feature* f = sl::FindSlt(FlagName(ref))) return f->applied ? 1 : 0;
        return Orig(ref);
    }
};

/* bool get - used directly by game code. */
HOOK_DEFINE_TRAMPOLINE(LandFlagGetBool) {
    static bool Callback(long* ref) {
        Trace("getbool", ref, 0);
        if (sl::Feature* f = sl::FindSlt(FlagName(ref))) return f->applied;
        return Orig(ref);
    }
};

/* set - used by EventFlowActionEventFlagSystemOn (1) / Off (0) / Set for cLand. */
HOOK_DEFINE_TRAMPOLINE(LandFlagSet) {
    static void Callback(long* ref, u32 value) {
        Trace("set", ref, value);
        if (sl::Feature* f = sl::FindSlt(FlagName(ref))) {
            sl::SetFromMenu(*f, value != 0);
            return;
        }
        Orig(ref, value);
    }
};

/* Phone: is app <id> unlocked? App 6 is the Slumber app while Nook Miles+ (7) is unlocked. */
HOOK_DEFINE_TRAMPOLINE(PhoneAppUnlocked) {
    static u64 Callback(u8* player, s32 app_id) {
        if (app_id == sl::SlumberAppId && sl::PhoneAppOn() && (Orig(player, sl::MilesPlusAppId) & 1)) return 1;
        /* The Island Editor app takes whichever DIY Recipes slot the phone hides. */
        if ((app_id == sl::DiyAppId || app_id == sl::DiyPlusAppId) && sl::EditorAppOn()) {
            int slot = (Orig(player, sl::DiyPlusAppId) & 1) ? sl::DiyAppId : sl::DiyPlusAppId;
            sl::SetEditorSlot(slot);
            if (app_id == slot) return 1;
        }
        return Orig(player, app_id);
    }
};

/* Phone: badge of app <id> (0 none, 1 red unread dot, 2 "new"). App 6 would show Nook Miles'
   unclaimed-rewards dot, so the Slumber app gets none. */
HOOK_DEFINE_TRAMPOLINE(PhoneAppBadge) {
    static u32 Callback(u8* menu, s32 app_id) {
        if (app_id == sl::SlumberAppId && sl::PhoneAppOn()) {
            using UnlockFn = u64 (*)(u8* player, s32 app_id);
            auto unlocked = reinterpret_cast<UnlockFn>(exl::util::modules::GetTargetOffset(0x2e5a0e0));
            if (unlocked(menu + 0x1f8, sl::MilesPlusAppId) & 1) return 0;
        }
        if (app_id == sl::EditorSlot() && sl::EditorAppOn()) return 0;
        return Orig(menu, app_id);
    }
};

/* Island Editor app handler (result: +0 next phone page, +4 flow kind, +8 "close the phone"). */
static void EditorAppHandler(u8* result, u8*) { sl::EditorAppLaunched(result); }

/* Slumber app handler: close the phone and open the app's own screen. */
static void SlumberAppHandler(u8* result, u8*) {
    result[8] = 1;
    sl::app::Open();
}

/* Phone: "blr x8" calling handler[app id](result, player); the app id is at [x29 - 4].
   The Slumber app opens its own screen (slumber_app.cpp); if that can't be drawn, it calls
   Island Designer's handler (start flow kind 2) for the old talk menu instead. */
HOOK_DEFINE_INLINE(PhoneAppLaunch) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        s32 app_id = *reinterpret_cast<s32*>(ctx->X[29] - 4);
        sl::g_LaunchSlumberApp = false;
        if (app_id == sl::SlumberAppId && sl::PhoneAppOn()) {
            if (sl::app::Ready()) {
                ctx->X[8] = reinterpret_cast<u64>(&SlumberAppHandler);
                sl::Log("phone: Slumber app opened");
            } else {
                ctx->X[8] = exl::util::modules::GetTargetOffset(sl::PhoneAppHandler());
                sl::g_LaunchSlumberApp = true;
                sl::Log("phone: Slumber app opened (talk menu)");
            }
        }
        if (app_id == sl::EditorSlot() && sl::EditorAppOn())
            ctx->X[8] = reinterpret_cast<u64>(&EditorAppHandler);
    }
};

/* Phone: fills the flow name/entry for a flow kind ([self + 0x140]). out + 0x18 = entry name. */
HOOK_DEFINE_TRAMPOLINE(PhoneAppFlowName) {
    static bool Callback(u8* self, u8* out) {
        bool r = Orig(self, out);
        if (sl::g_LaunchSlumberApp && r && *reinterpret_cast<s32*>(self + 0x140) == 2) {
            *reinterpret_cast<const char**>(out + 0x18) = sl::PhoneAppEntry();
            sl::g_LaunchSlumberApp = false;
        }
        return r;
    }
};

/* Terraform tool states (per-frame "exec" handlers, argument = player): speed up the animation. */
#define FAST_TERRAFORM_STATE(name)                                      \
    HOOK_DEFINE_TRAMPOLINE(name) {                                      \
        static void Callback(u8* player) {                              \
            Orig(player);                                               \
            sl::FastTerraform(player);                                  \
        }                                                               \
    };
FAST_TERRAFORM_STATE(CliffReadyExec)
FAST_TERRAFORM_STATE(CliffCreateExec)
FAST_TERRAFORM_STATE(CliffDeleteExec)
FAST_TERRAFORM_STATE(RiverReadyExec)
FAST_TERRAFORM_STATE(RiverDigExec)
FAST_TERRAFORM_STATE(RiverBuryExec)
FAST_TERRAFORM_STATE(CliffWaitExec)
FAST_TERRAFORM_STATE(CliffAfterExec)
FAST_TERRAFORM_STATE(RiverWaitExec)

/* Exit handlers of the same states (player, next state): back to normal speed. */
#define FAST_TERRAFORM_EXIT(name)                                       \
    HOOK_DEFINE_TRAMPOLINE(name) {                                      \
        static void Callback(u8* player, u64 next) {                    \
            Orig(player, next);                                         \
            sl::EndFastTerraform(player);                               \
        }                                                               \
    };
FAST_TERRAFORM_EXIT(ActStartExit)
FAST_TERRAFORM_EXIT(CliffReadyExit)
FAST_TERRAFORM_EXIT(CliffWaitExit)
FAST_TERRAFORM_EXIT(CliffCreateExit)
FAST_TERRAFORM_EXIT(CliffDeleteExit)
FAST_TERRAFORM_EXIT(CliffAfterExit)
FAST_TERRAFORM_EXIT(RiverReadyExit)
FAST_TERRAFORM_EXIT(RiverWaitExit)
FAST_TERRAFORM_EXIT(RiverDigExit)
FAST_TERRAFORM_EXIT(RiverBuryExit)

HOOK_DEFINE_TRAMPOLINE(ActStartExec) {
    static void Callback(u8* player) {
        Orig(player);
        sl::FastTerraformStart(player);
    }
};

/* Slumber time screen: weather button picked. [page + 0x10] = 0 sunny / 1 cloudy. */
HOOK_DEFINE_TRAMPOLINE(WeatherPick) {
    static void Callback(u8* page) {
        if (page) sl::BeforeWeatherPick(page[0x10]);
        Orig(page);
    }
};

/* Stage load: create the resident CloudIslandModeChange scene (the Slumber HUD). Returning null
   leaves it out, as on a stage that doesn't list it. */
HOOK_DEFINE_TRAMPOLINE(HudCreate) {
    static void* Callback(void* mgr, u32 index) {
        if (!sl::HudAllowed()) return nullptr;
        return Orig(mgr, index);
    }
};

/* Placing a fence: player state 0xa6's enter work (argument = player). */
HOOK_DEFINE_TRAMPOLINE(FencePutEnter) {
    static u32 Callback(u8* player) {
        bool free = sl::FreeFenceBegin(player);
        u32 r = Orig(player);
        if (free) sl::FreeFenceEnd();
        return r;
    }
};

/* Slumber HUD page update (vtable slot 52 of CloudIslandModeChangePage). */
HOOK_DEFINE_TRAMPOLINE(HudPageUpdate) {
    static void Callback(u8* page) {
        sl::HudPageUpdate(page);
        Orig(page);
    }
};

/* Generic UI page update, every page every frame (x0 = page). Only the MainScreen page (vtable
   0x49e0480) is touched. (v0.27 hooked 0x2e0d404, which is inside MainScreen's update, not its
   start - the game crashed after loading.) */
constexpr u32 MainScreenVtable = 0x49e0480;

HOOK_DEFINE_TRAMPOLINE(PageUpdate) {
    static u64 Callback(u8* page) {
        if (page && *reinterpret_cast<u8**>(page) == reinterpret_cast<u8*>(exl::util::modules::GetTargetOffset(MainScreenVtable)))
            sl::MainScreenBeforeUpdate();
        return Orig(page);
    }
};

/* Slumber HUD page, idle (closed) state exec: reopens the HUD when it should show. */
HOOK_DEFINE_TRAMPOLINE(HudIdleExec) {
    static void Callback(u8* page) {
        if (sl::HudIdleSkip(page)) return;
        Orig(page);
    }
};

/* Fence / path picker handler: (player idx*, item*, ...) -> bool. */
HOOK_DEFINE_TRAMPOLINE(PickerChoose) {
    static u32 Callback(u8* idx, u32* item, u32 a3, u32 a4, u32 a5) {
        bool path = sl::PathPickBegin(idx);
        u32 r = Orig(idx, item, a3, a4, a5);
        if (path) sl::PathPickEnd();
        return r;
    }
};

/* Fence pick-up: give the picked fence to the player (idx = player-index byte, req = request). */
HOOK_DEFINE_TRAMPOLINE(FencePickupGive) {
    static void* Callback(u8* idx, u8* req) {
        if (!req || !sl::FencePickupNoGive(idx)) return Orig(idx, req);
        u8 old = req[0x37];
        req[0x37] = old | 1;
        void* r = Orig(idx, req);
        req[0x37] = old;
        return r;
    }
};

/* Player button handler (D-pad, +, X ... -> UI scenes); returns true when it opened something. */
HOOK_DEFINE_TRAMPOLINE(ButtonMenus) {
    static bool Callback(u8* player) {
        sl::HudPlus(player);
        sl::EditorTick(player);
        return Orig(player);
    }
};

/* ---- Island editor hooks (see sl::EditorMap) ---- */

/* "May this map edit furniture?" (FtrActorMgr init, start-edit). */
HOOK_DEFINE_TRAMPOLINE(EditMapCheck) {
    static bool Callback() { return sl::EditorMap() || Orig(); }
};

/* Exterior editor request gate. */
HOOK_DEFINE_TRAMPOLINE(EditRequestGate) {
    static bool Callback() { return sl::EditorMap() || Orig(); }
};

/* "Warp the player to the room entrance while editing" (player state 0x61). */
HOOK_DEFINE_TRAMPOLINE(EditWarpCheck) {
    static u32 Callback() {
        if (sl::EditorMap()) {
            /* Seamless exit: no game warp (it plays a ~1 s wipe to hide the jump); the player is put at
               the cursor here and now, where the edit camera already looks (tested in Eden). */
            if (sl::EditorWarpAllowed()) {
                sl::Vec3f v;
                if (sl::EditorWarpTarget(&v)) sl::EditorPlacePlayerNow(v);
            }
            return 0;
        }
        return Orig();
    }
};

/* Where closing the editor puts the player (Vec3f in s0-s2): the cursor on the main island. */
HOOK_DEFINE_TRAMPOLINE(EditWarpTarget) {
    static sl::Vec3f Callback(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::Vec3f v;
        if (sl::EditorWarpTarget(&v)) return v;
        return Orig(a0, a1, a2, a3, a4, a5, a6, a7);
    }
};

/* Edit area (min, max). */
HOOK_DEFINE_TRAMPOLINE(EditAreaGet) {
    static void Callback(float* min, float* max) {
        if (sl::EditArea(min, max)) return;
        Orig(min, max);
    }
};

/* Start-edit on the FtrEditLayout (x0); the other arguments are passed through. */
HOOK_DEFINE_TRAMPOLINE(EditStart) {
    static void Callback(u8* layout, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::EditStart(layout);
        Orig(layout, a1, a2, a3, a4, a5, a6, a7);
    }
};

/* Cursor to the area's centre (x0 = FtrEditLayout). */
HOOK_DEFINE_TRAMPOLINE(EditCursorCentre) {
    static void Callback(u8* layout) {
        sl::EditCursorBeforeCentre(layout);
        Orig(layout);
        sl::EditCursorStart(layout);
    }
};

/* "Can the held item go here?" (x0 = drag holder; +0x88 set = cache the answer at +0x78).
   0x13f20f8 puts the item at the target when this says yes, else back where it came from.
   On the main island the answer is always yes (the game's own check still runs first). */
/* XLink keyword search (ctx, name) -> asset and emit (obj, {vt, asset, flag}, handle*): recorded for
   dbg "xrec" / "xlog". */
HOOK_DEFINE_TRAMPOLINE(XLinkSearch) {
    static void* Callback(void* ctx, const char* name) {
        void* r = Orig(ctx, name);
        sl::XRecSearch(ctx, name, r);
        return r;
    }
};
HOOK_DEFINE_TRAMPOLINE(XLinkEmitHook) {
    static u64 Callback(u8* obj, void* q, void* out) {
        sl::XRecEmit(obj, q);
        return Orig(obj, q, out);
    }
};

/* Editor's garden-edit-area effect update (x0 = area effect, x1 = the cursor's XLink owner). */
HOOK_DEFINE_TRAMPOLINE(EditAreaFx) {
    static void Callback(u8* area, u8* owner) {
        sl::CaptureEditXLink(area, owner);
        Orig(area, owner);
    }
};

HOOK_DEFINE_TRAMPOLINE(EditCanPlace) {
    static u32 Callback(u8* holder, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        u32 r = Orig(holder, a1, a2, a3, a4, a5, a6, a7);
        if (!holder || !sl::EditorMap()) return r;
        if (holder[0x88]) holder[0x78] = 1;
        return 1;
    }
};

/* Edit camera look-at; a Vec3f comes back in s0-s2. */
HOOK_DEFINE_TRAMPOLINE(EditCameraFocus) {
    static sl::Vec3f Callback() {
        sl::Vec3f v;
        if (sl::EditCameraTarget(&v)) return v;
        return Orig();
    }
};

/* Edit camera point = look-at + offset (x0 = camera, x1 = out). */
HOOK_DEFINE_TRAMPOLINE(EditCameraPointHook) {
    static void Callback(u8* cam, sl::Vec3f* out) {
        Orig(cam, out);
        sl::EditCameraPoint(cam, out);
    }
};

/* Generic state machine change-state (x0 = state machine, w1 = new state). */
HOOK_DEFINE_TRAMPOLINE(StateChangeHook) {
    static void Callback(u8* sm, u32 state) {
        if (sl::BlockCameraState(sm, state)) return;
        Orig(sm, state);
    }
};

/* Edit camera zoom blend (x0 = delegate owner, x1 = params); Vec3 back in s0-s2. */
HOOK_DEFINE_TRAMPOLINE(EditCameraZoomHook) {
    static sl::Vec3f Callback(u8* self, u8* params) {
        sl::Vec3f v = Orig(self, params);
        sl::EditCameraZoom(self, &v);
        return v;
    }
};

/* HHP exterior menu: the vacation home's theme banner (layout RootPane > N_InOut_00 > N_Capture_00 >
   {P_Line_00 zigzag, N_Center_00 > T_Text_00 theme text}) means nothing on the island, where houses
   have no theme. 0x345d81c ends the banner-text code (both the normal path and the editor's skip,
   see ThemeBannerSkip); x19 = the text object, +0x90 its T_Text_00 pane. On the island editor map,
   hide the whole banner. ui2d panes in this build: +0x18 parent, +0x58 flag (bit 0 = visible), +0xb0 name;
   each step checks the pane's name first. */
static bool PaneNamed(u64 p, const char* want) {
    return p && !(p & 7) && strncmp(reinterpret_cast<const char*>(p + 0xb0), want, 24) == 0;
}
HOOK_DEFINE_INLINE(ThemeBannerHide) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (!sl::EditorMap()) return;
        u64 obj = ctx->X[19];
        if (!obj || (obj & 7)) return;
        u64 text = *reinterpret_cast<const u64*>(obj + 0x90);
        if (!PaneNamed(text, "T_Text_00")) return;
        u64 center = *reinterpret_cast<const u64*>(text + 0x18);
        if (!PaneNamed(center, "N_Center_00")) return;
        u64 capture = *reinterpret_cast<const u64*>(center + 0x18);
        if (!PaneNamed(capture, "N_Capture_00")) return;
        u64 banner = *reinterpret_cast<const u64*>(capture + 0x18);
        if (!PaneNamed(banner, "N_InOut_00")) return;
        u8* flag = reinterpret_cast<u8*>(banner + 0x58);
        if (*flag & 1) {
            *flag &= ~1;
            sl::Log("island editor: theme banner hidden");
        }
    }
};

/* The island editor's screen change plays HHP's interior wipe (~1.2 s each way). The wipe page
   (x19 in its command handlers 0x2cb5bc4 / 0x2cb5dc0 / 0x2cb5ed4 and its update 0x2cb73d0) holds
   five wipe layouts, +0x244 = the one in use (2 = WipeInteriorWherearenIslands); each handler
   starts an animation and sets its speed with s0 = 1.0 through vfunc +0x190. On the island editor
   map that wipe runs WipeSpeed times faster, so the screen changes almost at once while the game's
   own wipe logic (and whatever waits for it) runs as before. Door and other wipes are untouched. */
constexpr float WipeSpeed = 120.0f;

HOOK_DEFINE_INLINE(EditorWipeSpeed) {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        u8* page = reinterpret_cast<u8*>(ctx->X[19]);
        if (!page || !sl::EditorMap()) return;
        if (*reinterpret_cast<const u32*>(page + 0x244) != 2) return;
        ctx->S[0] = WipeSpeed;
    }
};

/* HHP's per-frame house hover (0x13cfd7c: play mode == HHP?) also runs on the main island. */
HOOK_DEFINE_INLINE(BuildingHoverGate) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (sl::EditorMap()) { ctx->X[8] = ctx->X[11]; sl::CursorHeartbeat(); }
    }
};

/* Building actor under the cursor cell (x0 query, x2 full rect out, x3 min cell out). */
HOOK_DEFINE_TRAMPOLINE(BuildingAtCell) {
    static u32 Callback(u8* query, u64 flags, u8* rect, u8* cell, u64 a4, u64 a5, u64 a6, u64 a7) {
        if (sl::EditorMap()) return sl::BuildingAtCell(query, rect, cell);
        return Orig(query, flags, rect, cell, a4, a5, a6, a7);
    }
};

/* Hover query of kind 4 for a building actor (x0 holder, x1 query, x2 &handle, x3 rect). */
HOOK_DEFINE_TRAMPOLINE(BuildingHover) {
    static bool Callback(u8* holder, u8* query, u32* handle, u8* rect) {
        if (sl::EditorMap() && sl::BuildingHover(query, handle, rect)) return true;
        return Orig(holder, query, handle, rect);
    }
};

/* Edit object behind a hover / held handle. */
HOOK_DEFINE_TRAMPOLINE(EditObjectOf) {
    static void* Callback(u8* handle) {
        if (sl::EditorMap())
            if (void* c = sl::BuildingEditObject(handle)) return c;
        return Orig(handle);
    }
};

/* "Hovered house without / with HHP exterior editing" (x0 query: +0x18 kind, +0x90 handle).
   The first opens the house-sized outline; the second would offer the exterior button, which
   only works on HHP's own house, so island buildings get the first. */
HOOK_DEFINE_TRAMPOLINE(HouseHoverPlain) {
    static bool Callback(u8* query) {
        if (sl::EditorMap() && *reinterpret_cast<s32*>(query + 0x18) == 4 && sl::BuildingEditObject(query + 0x90))
            return true;
        return Orig(query);
    }
};

HOOK_DEFINE_TRAMPOLINE(HouseHoverExterior) {
    static bool Callback(u8* query) {
        if (sl::EditorMap() && *reinterpret_cast<s32*>(query + 0x18) == 4 && sl::BuildingEditObject(query + 0x90))
            return false;
        return Orig(query);
    }
};

/* Exterior menu: the hovered house's design (x0 out, x1 actor handle). */
HOOK_DEFINE_TRAMPOLINE(HouseDesignGet) {
    static void Callback(u8* out, u64 handle) {
        if (sl::EditorMap() && sl::HouseDesignGet(out, (u32)handle)) return;
        Orig(out, handle);
    }
};

/* Exterior menu: apply a design (x0 actor handle, x1 design). */
HOOK_DEFINE_TRAMPOLINE(HouseDesignSet) {
    static u32 Callback(u64 handle, u8* design, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        if (sl::EditorMap() && sl::HouseDesignSet((u32)handle, design)) return 1;
        return Orig(handle, design, a2, a3, a4, a5, a6, a7);
    }
};

/* ---- Bridges tab of the house exterior menu (see sl::HomeTabs) ---- */

/* Tab strip: build the tabs from a descriptor { count, entries }. */
HOOK_DEFINE_TRAMPOLINE(TabStripSetTabs) {
    static void Callback(u8* strip, const void* desc, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::HomeTabStrip(strip, desc);
        if (sl::HomeTabsWanted(desc)) desc = sl::HomeTabs(desc);
        Orig(strip, desc, a2, a3, a4, a5, a6, a7);
    }
};

/* The house menu binds its tab strip (and its tab buttons, through TabStripInit). */
HOOK_DEFINE_TRAMPOLINE(HomeTabBind) {
    static void Callback(u8* menu, u8* layout, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::HomeTabButtonsBegin(true);
        Orig(menu, layout, a2, a3, a4, a5, a6, a7);
        sl::HomeTabButtonsBegin(false);
    }
};

/* Tab strip: bind its buttons (params +0x14 = button count). */
HOOK_DEFINE_TRAMPOLINE(TabStripInit) {
    static void Callback(u8* strip, u8* params, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::HomeTabButtonParams(params);
        Orig(strip, params, a2, a3, a4, a5, a6, a7);
    }
};

/* Tab button: show the icon of a tab kind. */
HOOK_DEFINE_TRAMPOLINE(TabButtonIcon) {
    static void Callback(u8* btn, u64 kind, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        Orig(btn, kind, a2, a3, a4, a5, a6, a7);
        sl::HomeTabIcon(btn, (u32)kind);
    }
};

/* Tab bar: lay out for <count> tabs. */
HOOK_DEFINE_TRAMPOLINE(TabBarCount) {
    static void Callback(u8* bar, u64 count, u64 sel, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        Orig(bar, count, sel, a3, a4, a5, a6, a7);
        sl::HomeTabBarCount(bar, (u32)count, true);
    }
};

/* Tab strip: select the first tab. */
HOOK_DEFINE_TRAMPOLINE(TabStripStart) {
    static void Callback(u8* strip, u64 owner, u64 first, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        Orig(strip, owner, sl::HomeTabStart(strip, (u32)first), a3, a4, a5, a6, a7);
    }
};

/* House menu: A on a grid cell. */
HOOK_DEFINE_TRAMPOLINE(HomeGridPick) {
    static void Callback(u8* menu, u8* event, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::HomeGridPick(menu, event);
        Orig(menu, event, a2, a3, a4, a5, a6, a7);
    }
};

/* House menu: the tab changed. */
HOOK_DEFINE_TRAMPOLINE(HomeTabChanged) {
    static void Callback(u8* menu, u8* arg, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::HomeTabChanged(menu);
        Orig(menu, arg, a2, a3, a4, a5, a6, a7);
    }
};

/* House menu: fill the grid for the current tab. */
HOOK_DEFINE_TRAMPOLINE(HomeGridFill) {
    static void Callback(u8* menu, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        sl::HomeGridFill(menu);
        Orig(menu, a1, a2, a3, a4, a5, a6, a7);
    }
};

/* House menu: the grid's picture delegate (op 0 = show an item in a cell). */
HOOK_DEFINE_TRAMPOLINE(HomeCellName) {
    static void Callback(u8* menu, void* ev) {
        sl::HomeCellName(menu, ev, [](u8* m, void* e) { Orig(m, e); });
    }
};

HOOK_DEFINE_TRAMPOLINE(HomeGridPicture) {
    static void Callback(u8* menu, const s32* ev, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        if (sl::HomeGridPicture(menu, ev)) return;
        Orig(menu, ev, a2, a3, a4, a5, a6, a7);
    }
};

/* Cliff tool's land decision (its only caller is the tool): obj +0x28 = action. */
HOOK_DEFINE_TRAMPOLINE(CliffDecision) {
    static u32 Callback(u8* obj, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        u32 r = Orig(obj, a1, a2, a3, a4, a5, a6, a7);
        sl::CliffDecided(obj, r);
        return r;
    }
};

/* Terrain change (3 x 3 block): raised tiles left without a wall get their edge piece first. */
HOOK_DEFINE_TRAMPOLINE(TerrainApplyFix) {
    static u64 Callback(u64 prev, sl::TerrainApplyArgs* a) {
        sl::FixTerrainBlock(a);
        return Orig(prev, a);
    }
};

/* Terrain piece instance placement: stacked cliffs get a wall piece for each missing level below. */
HOOK_DEFINE_TRAMPOLINE(TerrainPiecePlace) {
    static void Callback(u8* mgr, u8* model, float* xf, u32 a3, u8* a4, u32 a5, u8* a6, u8 a7) {
        Orig(mgr, model, xf, a3, a4, a5, a6, a7);
        sl::LowerPiece lower[sl::MaxExtraWalls];
        int n = sl::CliffLowerPieces(model, xf, lower);
        for (int k = 0; k < n; k++) Orig(mgr, lower[k].model, lower[k].xf, a3, a4, a5, a6, a7);
    }
};

/* Camera pose apply, every frame: drives building rebuilds. */
HOOK_DEFINE_TRAMPOLINE(FrameTick) {
    static u64 Callback(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
        u64 r = Orig(a0, a1, a2, a3, a4, a5, a6, a7);
        sl::BuildingTick();
        sl::BridgeTick();
        sl::AppTick();
        return r;
    }
};

/* ---- Slumber app screen: NVN and controller hooks (see slumber_app.cpp) ---- */

/* Address of a function the SDK module exports, or 0.
   Read-only lookup through the module's own dynamic section: exl::reloc::GetSymbol can
   (re)initialize the rtld module object it is given, and if the layout scan ever mistakes
   another module for the SDK (it once took this module, on Eden) that corrupts the game's
   rtld state and the first lazy-bound call panics in BindJumpSlotRel. */
/* Address of <name> exported by one game module, or 0 (read-only lookup).
   ACNH's sdk and subsdk0 only carry a GNU hash table (DT_GNU_HASH), main a SysV one (DT_HASH). */
static uintptr_t ModuleSymbol(exl::util::ModuleIndex idx, const char* name) {
    constexpr s64 DtGnuHash = 0x6ffffef5;
    if (!exl::util::HasModule(idx)) return 0;
    const auto& info = exl::util::GetModuleInfo(idx);
    if (info.m_Total.m_Start == exl::util::GetSelfModuleInfo().m_Total.m_Start) return 0;
    const uintptr_t base = info.m_Total.m_Start;
    const u32* hash = nullptr;
    const u32* gnu = nullptr;
    const Elf_Sym* syms = nullptr;
    const char* strs = nullptr;
    for (const Elf_Dyn* d = info.m_Mod->GetDynamic(); d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_HASH) hash = reinterpret_cast<const u32*>(base + d->d_un.d_val);
        else if (d->d_tag == DtGnuHash) gnu = reinterpret_cast<const u32*>(base + d->d_un.d_val);
        else if (d->d_tag == DT_SYMTAB) syms = reinterpret_cast<const Elf_Sym*>(base + d->d_un.d_val);
        else if (d->d_tag == DT_STRTAB) strs = reinterpret_cast<const char*>(base + d->d_un.d_val);
    }
    if (!syms || !strs) return 0;
    auto match = [&](u32 i) {
        const Elf_Sym& s = syms[i];
        return s.st_shndx != SHN_UNDEF && s.st_value && strcmp(strs + s.st_name, name) == 0;
    };
    if (gnu && gnu[0]) {
        const u32 nbucket = gnu[0], symoffset = gnu[1], bloom_size = gnu[2];
        const u32* buckets = gnu + 4 + bloom_size * 2;   /* bloom words are 64-bit */
        const u32* chain = buckets + nbucket;
        u32 h = 5381;
        for (const char* p = name; *p; p++) h = h * 33 + (u8)*p;
        u32 i = buckets[h % nbucket];
        if (i >= symoffset) {
            while (true) {
                const u32 h2 = chain[i - symoffset];
                if ((h | 1) == (h2 | 1) && match(i)) return base + syms[i].st_value;
                if (h2 & 1) break;
                i++;
            }
        }
    }
    if (hash && hash[0]) {
        const u32 nbucket = hash[0];
        const u32* bucket = hash + 2;
        const u32* chain = bucket + nbucket;
        u32 h = 0;
        for (const char* p = name; *p; p++) {
            h = (h << 4) + (u8)*p;
            u32 g = h & 0xf0000000;
            if (g) h ^= g >> 24;
            h &= ~g;
        }
        for (u32 i = bucket[h % nbucket]; i; i = chain[i])
            if (match(i)) return base + syms[i].st_value;
    }
    return 0;
}

/* Address of a function a game module exports (the SDK first, then main and the subsdks), or 0. */
static uintptr_t SdkSymbol(const char* name) {
    using exl::util::ModuleIndex;
    if (uintptr_t at = ModuleSymbol(ModuleIndex::Sdk, name)) return at;
    for (int i = (int)ModuleIndex::Main; i < (int)ModuleIndex::Sdk; i++)
        if (uintptr_t at = ModuleSymbol((ModuleIndex)i, name)) return at;
    return 0;
}

/* Every NVN function the game looks up passes through here. */
HOOK_DEFINE_TRAMPOLINE(NvnBootstrap) {
    static sl::app::GenericFn Callback(const char* name) {
        return sl::app::WrapProc(name, Orig(name));
    }
};

/* nn::hid state reads, one hook per state type (NpadBaseState layout for all of them):
   detail::GetNpadStates(int* written, State*, int count, const NpadIdType&) is what the
   public GetNpadState(s) call; the public ones are hooked too in case some don't go through it. */
#define NPAD_HOOKS(type)                                                                     \
    HOOK_DEFINE_TRAMPOLINE(NpadDetail##type) {                                               \
        static int Callback(int* written, void* states, int count, const u32& id) {          \
            int r = Orig(written, states, count, id);                                        \
            sl::QuickStartPad(states, count);                                \
            sl::EditorPad(states, count);                                     \
            sl::app::OnNpadStates(states, count);                                            \
            return r;                                                                        \
        }                                                                                    \
    };                                                                                       \
    HOOK_DEFINE_TRAMPOLINE(NpadStates##type) {                                               \
        static void Callback(void* states, int count, const u32& id) {                       \
            Orig(states, count, id);                                                         \
            sl::QuickStartPad(states, count);                                \
            sl::EditorPad(states, count);                                     \
            sl::app::OnNpadStates(states, count);                                            \
        }                                                                                    \
    };                                                                                       \
    HOOK_DEFINE_TRAMPOLINE(NpadState##type) {                                                \
        static void Callback(void* state, const u32& id) {                                   \
            Orig(state, id);                                                                 \
            sl::QuickStartPad(state, 1);                                       \
            sl::EditorPad(state, 1);                                           \
            sl::app::OnNpadStates(state, 1);                                                 \
        }                                                                                    \
    };
NPAD_HOOKS(FullKey)
NPAD_HOOKS(Handheld)
NPAD_HOOKS(JoyDual)
NPAD_HOOKS(JoyLeft)
NPAD_HOOKS(JoyRight)

template <typename Hook> static int HookSdk(const char* name) {
    uintptr_t at = SdkSymbol(name);
    if (!at) return 0;
    Hook::InstallAtPtr(at);
    return 1;
}

#define NPAD_INSTALL(type, mangled)                                                              \
    n += HookSdk<NpadDetail##type>("_ZN2nn3hid6detail13GetNpadStatesEPiPNS0_" mangled "EiRKj");  \
    n += HookSdk<NpadStates##type>("_ZN2nn3hid13GetNpadStatesEPNS0_" mangled "EiRKj");           \
    n += HookSdk<NpadState##type>("_ZN2nn3hid12GetNpadStateEPNS0_" mangled "ERKj");

static void InstallAppHooks() {
    if (!HookSdk<NvnBootstrap>("nvnBootstrapLoader")) return;   /* no NVN hook: no input hooks either */
    sl::g_AppNvnHook = true;
    int n = 0;
    NPAD_INSTALL(FullKey, "16NpadFullKeyState")
    NPAD_INSTALL(Handheld, "17NpadHandheldState")
    NPAD_INSTALL(JoyDual, "16NpadJoyDualState")
    NPAD_INSTALL(JoyLeft, "16NpadJoyLeftState")
    NPAD_INSTALL(JoyRight, "17NpadJoyRightState")
    sl::g_AppNpadHooks = n;
    if (n) sl::app::SetInputHooked();
    if (uintptr_t alloc = SdkSymbol("aligned_alloc")) {
        sl::app::SetAllocator(reinterpret_cast<sl::app::AllocFn>(alloc));
        sl::g_AppAlloc = true;
    }
}

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();
    MainHook::InstallAtFuncPtr(nnMain);
    InstallAppHooks();
    LandFlagGet::InstallAtOffset(0x263ea30);
    LandFlagGetBool::InstallAtOffset(0x263f0a0);
    LandFlagSet::InstallAtOffset(0x263f3c4);
    WeatherPick::InstallAtOffset(0x2cacf14);
    HudCreate::InstallAtOffset(0x32d8b70);
    ButtonMenus::InstallAtOffset(0x2268cd4);
    FencePutEnter::InstallAtOffset(0x238c3b0);
    FencePickupGive::InstallAtOffset(0x238d900);
    HudPageUpdate::InstallAtOffset(0x2c52ce0);
    HudIdleExec::InstallAtOffset(0x2c51f14);
    PageUpdate::InstallAtOffset(0x2b0c720);
    PickerChoose::InstallAtOffset(0x2436850);
    PhoneAppUnlocked::InstallAtOffset(0x2e5a0e0);
    PhoneAppBadge::InstallAtOffset(0x2e5fff8);
    PhoneAppLaunch::InstallAtOffset(0x2dffb64);
    PhoneAppFlowName::InstallAtOffset(0x2e01c98);
    EditMapCheck::InstallAtOffset(0x13e44ac);
    EditRequestGate::InstallAtOffset(0x32fa3b0);
    EditWarpCheck::InstallAtOffset(0x15dc334);
    EditWarpTarget::InstallAtOffset(0x2309d50);
    EditAreaGet::InstallAtOffset(0x13b8fc4);
    EditStart::InstallAtOffset(0x13c97d0);
    EditCursorCentre::InstallAtOffset(0x13d5d30);
    EditCameraFocus::InstallAtOffset(0x7f9070);
    EditCameraPointHook::InstallAtOffset(0x7d10d4);
    EditCameraZoomHook::InstallAtOffset(0x7cfa20);
    StateChangeHook::InstallAtOffset(0x3dbb160);
    EditCanPlace::InstallAtOffset(0x13ea4c0);
    EditAreaFx::InstallAtOffset(0x13b9f78);
    XLinkSearch::InstallAtOffset(0x3af5ec0);
    XLinkEmitHook::InstallAtOffset(0x3af47b0);
    BuildingHoverGate::InstallAtOffset(0x13cfd7c);
    BuildingAtCell::InstallAtOffset(0x13d0308);
    BuildingHover::InstallAtOffset(0x13f1bc4);
    EditObjectOf::InstallAtOffset(0x13ba988);
    HouseHoverPlain::InstallAtOffset(0x13ed990);
    HouseHoverExterior::InstallAtOffset(0x13ed9dc);
    HouseDesignGet::InstallAtOffset(0x2a914b0);
    HouseDesignSet::InstallAtOffset(0x2a91660);
    TabStripSetTabs::InstallAtOffset(0x3036870);
    HomeTabBind::InstallAtOffset(0x340abf0);
    TabStripInit::InstallAtOffset(0x30358c0);
    TabButtonIcon::InstallAtOffset(0x3033694);
    HomeTabChanged::InstallAtOffset(0x340b120);
    HomeGridFill::InstallAtOffset(0x340de50);
    TabBarCount::InstallAtOffset(0x2fe5370);
    TabStripStart::InstallAtOffset(0x3036ca0);
    HomeGridPick::InstallAtOffset(0x340c1d0);
    HomeGridPicture::InstallAtOffset(0x340d5b0);
    HomeCellName::InstallAtOffset(0x340b370);
    FrameTick::InstallAtOffset(0x7a8414);
    /* cliff tool states 0x228a3e8.., river tool states 0x2288578.. (exec handlers) */
    CliffReadyExec::InstallAtOffset(0x229dcd8);
    CliffCreateExec::InstallAtOffset(0x229e028);
    CliffDeleteExec::InstallAtOffset(0x229e1d0);
    RiverReadyExec::InstallAtOffset(0x2294f24);
    RiverDigExec::InstallAtOffset(0x2295284);
    RiverBuryExec::InstallAtOffset(0x2295428);
    /* player states (live table): 12 start-acting, 131-135 cliff tool, river tool after 0x2288578 */
    CliffWaitExec::InstallAtOffset(0x229de80);   /* 132 */
    CliffAfterExec::InstallAtOffset(0x229e388);  /* 135 */
    RiverWaitExec::InstallAtOffset(0x22950dc);
    ActStartExec::InstallAtOffset(0x2290770);    /* 12 */
    ActStartExit::InstallAtOffset(0x229080c);
    CliffReadyExit::InstallAtOffset(0x229dd74);
    CliffWaitExit::InstallAtOffset(0x229df1c);
    CliffDecision::InstallAtOffset(0x21e37a0);
    TerrainPiecePlace::InstallAtOffset(0x146ab80);
    TerrainApplyFix::InstallAtOffset(0x11ee4e0);
    CliffCreateExit::InstallAtOffset(0x229e0c4);
    CliffDeleteExit::InstallAtOffset(0x229e26c);
    CliffAfterExit::InstallAtOffset(0x229e424);
    RiverReadyExit::InstallAtOffset(0x2294fc0);
    RiverWaitExit::InstallAtOffset(0x2295178);
    RiverDigExit::InstallAtOffset(0x2295320);
    RiverBuryExit::InstallAtOffset(0x22954c4);
    ThemeBannerHide::InstallAtOffset(0x345d81c);
    /* the speed setter right after "fmov s0, #1.0" in each wipe command */
    for (u32 off : { 0x2cb5c88u, 0x2cb5e80u, 0x2cb5f98u, 0x2cb75fcu }) EditorWipeSpeed::InstallAtOffset(off);
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
