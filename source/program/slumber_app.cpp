#include "slumber_app.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <type_traits>

/* ---- The Slumber phone app's screen ----
   Tapping the Slumber app closes the phone and opens this screen: a panel in the middle of the
   picture with the switches in a list you scroll with the D-pad / left stick (A switches, left /
   right sets off / on or changes a number, L / R page, B closes). The game keeps running behind
   it, its controller input is cleared while the panel is up.

   Drawing: the module never runs shaders. The panel is drawn on the CPU into a GPU-visible
   memory pool, and right before the game presents a frame (nvnQueuePresentTexture) a small
   command buffer copies it into the window texture being presented
   (nvnCommandBufferCopyBufferToTexture), one copy per row near the rounded corners. The NVN
   functions come from wrapping the game's own lookups: nvnBootstrapLoader is hooked (main.cpp)
   and every function it hands out passes through WrapProc. That is the same NVN API on every
   game version, no offsets in main are involved.

   Input: the game's nn::hid state reads are hooked (main.cpp); OnNpadStates sees what the game
   would see, then clears the buttons and sticks while the panel is open, and until every button
   is released after it closes (so the B that closes it doesn't reach the game).

   If something the screen needs is missing (NVN not seen, unknown window format, no memory,
   input hooks never called), Ready() says no and the phone app opens the old talk menu. */

namespace sl::app {

    /* ---------------------------------------------------------------- NVN */

    using u8 = uint8_t;
    using u32 = uint32_t;
    using u64 = uint64_t;

    /* Object sizes (NVN 53.x): memory pool 0x100, builder 0x40, command buffer 0xa0. Ours are
       bigger on purpose. */
    struct alignas(16) NvnObject { u8 raw[0x400]; };
    struct NvnCopyRegion { int x, y, z, w, h, d; };

    constexpr int PoolCpuUncached = 0x2, PoolCpuCached = 0x4, PoolGpuUncached = 0x10, PoolGpuCached = 0x20;
    constexpr int FormatRGBA8 = 0x25, FormatRGBX8Srgb = 0x37, FormatRGBA8Srgb = 0x38;
    constexpr int FormatBGRX8 = 0x99, FormatBGRA8 = 0x9a, FormatBGRX8Srgb = 0x9b, FormatBGRA8Srgb = 0x9c;
    constexpr int OriginUpperLeft = 1;   /* NVNwindowOriginMode: 0 lower left (default), 1 upper left */

    using GetProcFn      = GenericFn (*)(const void* device, const char* name);
    using DeviceInitFn   = u8 (*)(void* device, const void* builder);
    using PresentFn      = void (*)(void* queue, void* window, int index);
    using SetTexturesFn  = void (*)(void* builder, int count, void* const* textures);
    using SetCropFn      = void (*)(void* window, int x, int y, int w, int h);
    using OriginFn       = void (*)(void* device, int mode);
    using CmdInitFn      = u8 (*)(void* cmd, void* device);

    static GetProcFn     s_RealGetProc;
    static DeviceInitFn  s_RealDeviceInit;
    static PresentFn     s_RealPresent;
    static SetTexturesFn s_RealSetTextures;
    static SetCropFn     s_RealSetCrop;
    static OriginFn      s_RealOrigin;
    static CmdInitFn     s_RealCmdInit;

    static void* s_Device = nullptr;
    constexpr int MaxTextures = 8;
    static void* s_Textures[MaxTextures];
    static int s_TextureCount = 0;
    static int s_Crop[4] = { 0, 0, 0, 0 };
    static int s_OriginMode = 0;
    static std::atomic<u32> s_Presents{0};

    /* The procs the app calls itself, looked up with the game's device. */
    static struct {
        int (*TextureGetWidth)(const void*);
        int (*TextureGetHeight)(const void*);
        int (*TextureGetFormat)(const void*);
        void (*PoolBuilderSetDefaults)(void*);
        void (*PoolBuilderSetDevice)(void*, void*);
        void (*PoolBuilderSetStorage)(void*, void*, size_t);
        void (*PoolBuilderSetFlags)(void*, int);
        u8 (*PoolInitialize)(void*, const void*);
        void* (*PoolMap)(const void*);
        u64 (*PoolGetBufferAddress)(const void*);
        void (*PoolFlushMappedRange)(const void*, ptrdiff_t, size_t);
        u8 (*CmdInitialize)(void*, void*);
        void (*CmdAddCommandMemory)(void*, const void*, ptrdiff_t, size_t);
        void (*CmdAddControlMemory)(void*, void*, size_t);
        void (*CmdBeginRecording)(void*);
        u64 (*CmdEndRecording)(void*);
        void (*CmdSetCopyRowStride)(void*, ptrdiff_t);
        void (*CmdCopyBufferToTexture)(void*, u64, const void*, const void*, const NvnCopyRegion*, int);
        void (*QueueSubmitCommands)(void*, int, const u64*);
    } P;
    static bool s_ProcsLoaded = false;

    static bool LoadProcs() {
        if (s_ProcsLoaded) return true;
        if (!s_Device || !s_RealGetProc) return false;
        bool ok = true;
        auto get = [&](auto& slot, const char* name) {
            GenericFn f = s_RealGetProc(s_Device, name);
            if (!f) { Log("app: NVN has no %s", name); ok = false; }
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(f);
        };
        get(P.TextureGetWidth, "nvnTextureGetWidth");
        get(P.TextureGetHeight, "nvnTextureGetHeight");
        get(P.TextureGetFormat, "nvnTextureGetFormat");
        get(P.PoolBuilderSetDefaults, "nvnMemoryPoolBuilderSetDefaults");
        get(P.PoolBuilderSetDevice, "nvnMemoryPoolBuilderSetDevice");
        get(P.PoolBuilderSetStorage, "nvnMemoryPoolBuilderSetStorage");
        get(P.PoolBuilderSetFlags, "nvnMemoryPoolBuilderSetFlags");
        get(P.PoolInitialize, "nvnMemoryPoolInitialize");
        get(P.PoolMap, "nvnMemoryPoolMap");
        get(P.PoolGetBufferAddress, "nvnMemoryPoolGetBufferAddress");
        get(P.PoolFlushMappedRange, "nvnMemoryPoolFlushMappedRange");
        get(P.CmdInitialize, "nvnCommandBufferInitialize");
        get(P.CmdAddCommandMemory, "nvnCommandBufferAddCommandMemory");
        get(P.CmdAddControlMemory, "nvnCommandBufferAddControlMemory");
        get(P.CmdBeginRecording, "nvnCommandBufferBeginRecording");
        get(P.CmdEndRecording, "nvnCommandBufferEndRecording");
        get(P.CmdSetCopyRowStride, "nvnCommandBufferSetCopyRowStride");
        get(P.CmdCopyBufferToTexture, "nvnCommandBufferCopyBufferToTexture");
        get(P.QueueSubmitCommands, "nvnQueueSubmitCommands");
        s_ProcsLoaded = ok;
        return ok;
    }

    /* ---- wrappers handed to the game ---- */

    static void Present(void* queue, void* window, int index);

    static u8 DeviceInit(void* device, const void* builder) {
        u8 r = s_RealDeviceInit(device, builder);
        if (r && !s_Device) { s_Device = device; Log("app: NVN device %p", device); }
        return r;
    }

    static void SetTextures(void* builder, int count, void* const* textures) {
        s_RealSetTextures(builder, count, textures);
        if (!textures || count <= 0) return;
        int n = count < MaxTextures ? count : MaxTextures;
        for (int i = 0; i < n; i++) s_Textures[i] = textures[i];
        s_TextureCount = n;
        Log("app: window has %d textures", count);
    }

    static void SetCrop(void* window, int x, int y, int w, int h) {
        s_RealSetCrop(window, x, y, w, h);
        s_Crop[0] = x; s_Crop[1] = y; s_Crop[2] = w; s_Crop[3] = h;
    }

    static void SetOrigin(void* device, int mode) {
        s_RealOrigin(device, mode);
        s_OriginMode = mode;
        Log("app: window origin mode %d", mode);
    }

    static u8 CmdInit(void* cmd, void* device) {
        u8 r = s_RealCmdInit(cmd, device);
        if (r && !s_Device && device) { s_Device = device; Log("app: NVN device %p (command buffer)", device); }
        return r;
    }

    static GenericFn GetProc(const void* device, const char* name) {
        return WrapProc(name, s_RealGetProc(device, name));
    }

    template <typename T> static GenericFn Wrap(T& real, GenericFn got, T ours) {
        if (reinterpret_cast<GenericFn>(ours) != got) real = reinterpret_cast<T>(got);
        return reinterpret_cast<GenericFn>(ours);
    }

    GenericFn WrapProc(const char* name, GenericFn real) {
        if (!name || !real) return real;
        if (!strcmp(name, "nvnDeviceGetProcAddress"))     return Wrap(s_RealGetProc, real, &GetProc);
        if (!strcmp(name, "nvnDeviceInitialize"))         return Wrap(s_RealDeviceInit, real, &DeviceInit);
        if (!strcmp(name, "nvnQueuePresentTexture"))      return Wrap(s_RealPresent, real, &Present);
        if (!strcmp(name, "nvnWindowBuilderSetTextures")) return Wrap(s_RealSetTextures, real, &SetTextures);
        if (!strcmp(name, "nvnWindowSetCrop"))            return Wrap(s_RealSetCrop, real, &SetCrop);
        if (!strcmp(name, "nvnDeviceSetWindowOriginMode")) return Wrap(s_RealOrigin, real, &SetOrigin);
        if (!strcmp(name, "nvnCommandBufferInitialize"))  return Wrap(s_RealCmdInit, real, &CmdInit);
        return real;
    }

    /* ---------------------------------------------------------------- panel layout */

    /* In 720p units; drawn 1.5x on a 1080p window. */
    constexpr int PanelW = 600, PanelH = 520, PanelRadius = 18, BorderW = 2;
    constexpr int HeaderH = 64;
    constexpr int ListTop = 72, RowH = 44, VisibleRows = 8, ListBottom = ListTop + RowH * VisibleRows;
    constexpr int HelpTop = 430, FooterTop = 484;
    constexpr float MaxScale = 1.5f;
    constexpr int MaxPixW = 900, MaxPixH = 780;   /* PanelW / PanelH at MaxScale */

    struct AppGlyph { uint8_t w, h; int8_t x, y; uint16_t adv8; uint32_t offset; };
    struct AppFont { int ascent, descent; const AppGlyph* glyphs; const char* pixels; };
#include "app_font.inc"

    struct Fonts { const AppFont* title; const AppFont* body; const AppFont* small; };
    static const Fonts k_Fonts1 = { &k_Font_Title_1, &k_Font_Body_1, &k_Font_Small_1 };
    static const Fonts k_Fonts2 = { &k_Font_Title_2, &k_Font_Body_2, &k_Font_Small_2 };

    struct Rgb { u8 r, g, b; };
    /* Animal Crossing look: cream paper with faint dots, leaf-green header, the yellow cursor,
       teal switches, warm brown text. */
    constexpr Rgb ColBorder = { 150, 116, 74 }, ColBg = { 250, 244, 226 }, ColDots = { 243, 234, 208 },
                  ColHeaderTop = { 124, 204, 118 }, ColHeaderBottom = { 82, 170, 96 }, ColTitle = { 255, 255, 255 },
                  ColSubtitle = { 226, 246, 214 }, ColText = { 114, 90, 62 }, ColDim = { 160, 138, 106 },
                  ColSelect = { 255, 226, 100 }, ColSelectEdge = { 242, 192, 58 }, ColLine = { 236, 226, 200 },
                  ColOn = { 56, 186, 160 }, ColOff = { 232, 222, 198 }, ColOffSel = { 246, 214, 120 },
                  ColOffText = { 150, 130, 100 }, ColValue = { 242, 140, 66 }, ColArrow = { 255, 226, 190 },
                  ColTrack = { 238, 228, 204 }, ColThumb = { 202, 170, 118 }, ColFooter = { 242, 232, 206 },
                  ColKey = { 114, 90, 62 }, ColWhite = { 255, 255, 255 };

    /* ---------------------------------------------------------------- CPU drawing */

    static u32* s_Canvas = nullptr;   /* PixW x PixH, rows flipped when s_Flip */
    static int s_PixW = 0, s_PixH = 0;
    static float s_Scale = 1.0f;
    static bool s_Flip = false, s_Bgr = false;
    static const Fonts* s_F = &k_Fonts1;
    static int s_ClipY0 = 0, s_ClipY1 = 0;

    static int U(float v) { return (int)(v * s_Scale + (v >= 0 ? 0.5f : -0.5f)); }

    static u32 Pack(Rgb c) {
        return s_Bgr ? (u32)c.b | (u32)c.g << 8 | (u32)c.r << 16 | 0xff000000u
                     : (u32)c.r | (u32)c.g << 8 | (u32)c.b << 16 | 0xff000000u;
    }

    static u32* Row(int y) { return s_Canvas + (size_t)(s_Flip ? s_PixH - 1 - y : y) * s_PixW; }

    /* Blend c over the pixel with coverage a (0-255). */
    static void Blend(u32* p, u32 c, u32 a) {
        if (a >= 255) { *p = c; return; }
        if (a == 0) return;
        u32 d = *p, out = 0xff000000u;
        for (int s = 0; s < 24; s += 8) {
            u32 dc = (d >> s) & 0xff, sc = (c >> s) & 0xff;
            out |= ((dc * (255 - a) + sc * a + 127) / 255) << s;
        }
        *p = out;
    }

    static bool ClipRow(int y) { return y >= s_ClipY0 && y < s_ClipY1 && y >= 0 && y < s_PixH; }

    static void FillRect(int x, int y, int w, int h, Rgb col) {
        u32 c = Pack(col);
        int x0 = x < 0 ? 0 : x, x1 = x + w > s_PixW ? s_PixW : x + w;
        for (int yy = y; yy < y + h; yy++) {
            if (!ClipRow(yy)) continue;
            u32* r = Row(yy);
            for (int xx = x0; xx < x1; xx++) r[xx] = c;
        }
    }

    /* Rounded rectangle, anti-aliased corners. */
    static void FillRound(int x, int y, int w, int h, int rad, Rgb col) {
        u32 c = Pack(col);
        if (rad * 2 > h) rad = h / 2;
        if (rad * 2 > w) rad = w / 2;
        for (int yy = y; yy < y + h; yy++) {
            if (!ClipRow(yy)) continue;
            u32* r = Row(yy);
            float cy = yy + 0.5f;
            float dy = cy < y + rad ? (y + rad) - cy : cy > y + h - rad ? cy - (y + h - rad) : 0.0f;
            for (int xx = x; xx < x + w; xx++) {
                if (xx < 0 || xx >= s_PixW) continue;
                float cx = xx + 0.5f;
                float dx = cx < x + rad ? (x + rad) - cx : cx > x + w - rad ? cx - (x + w - rad) : 0.0f;
                if (dx == 0.0f && dy == 0.0f) { r[xx] = c; continue; }
                if (dx == 0.0f || dy == 0.0f) {   /* straight edge of the rounded part */
                    float d = dx + dy;
                    float cov = rad - d + 0.5f;
                    Blend(&r[xx], c, cov >= 1 ? 255 : cov <= 0 ? 0 : (u32)(cov * 255));
                    continue;
                }
                float cov = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
                Blend(&r[xx], c, cov >= 1 ? 255 : cov <= 0 ? 0 : (u32)(cov * 255));
            }
        }
    }


    static const AppGlyph* GlyphOf(const AppFont* f, char ch) {
        unsigned c = (unsigned char)ch;
        if (c < 32 || c > 126) c = '?';
        return &f->glyphs[c - 32];
    }

    static int TextWidth(const AppFont* f, const char* s, int n = -1) {
        int w8 = 0;
        for (int i = 0; s[i] && (n < 0 || i < n); i++) w8 += GlyphOf(f, s[i])->adv8;
        return (w8 + 7) / 8;
    }

    static int HexVal(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

    static void DrawGlyphs(const AppFont* f, int x, int baseline, const char* s, int n, Rgb col) {
        u32 c = Pack(col);
        int pen8 = x * 8;
        for (int i = 0; i < n && s[i]; i++) {
            const AppGlyph* g = GlyphOf(f, s[i]);
            int gx = (pen8 + 4) / 8 + g->x, gy = baseline + g->y;
            const char* px = f->pixels + g->offset;
            for (int yy = 0; yy < g->h; yy++) {
                if (!ClipRow(gy + yy)) continue;
                u32* r = Row(gy + yy);
                for (int xx = 0; xx < g->w; xx++) {
                    int v = HexVal(px[yy * g->w + xx]);
                    int X = gx + xx;
                    if (v && X >= 0 && X < s_PixW) Blend(&r[X], c, (u32)v * 17);
                }
            }
            pen8 += g->adv8;
        }
    }

    /* Draws s, cut with "..." to fit maxW (0 = no limit). align: 0 left, 1 centre, 2 right of x. */
    static void DrawText(const AppFont* f, int x, int baseline, const char* s, Rgb col, int maxW = 0, int align = 0) {
        int n = (int)strlen(s);
        int w = TextWidth(f, s);
        bool dots = false;
        if (maxW > 0 && w > maxW) {
            int dw = TextWidth(f, "...");
            while (n > 0 && TextWidth(f, s, n) + dw > maxW) n--;
            while (n > 0 && s[n - 1] == ' ') n--;
            w = TextWidth(f, s, n) + dw;
            dots = true;
        }
        if (align == 1) x -= w / 2;
        if (align == 2) x -= w;
        DrawGlyphs(f, x, baseline, s, n, col);
        if (dots) DrawGlyphs(f, x + TextWidth(f, s, n), baseline, "...", 3, col);
    }

    /* Word-wraps s into at most `lines` lines of maxW. */
    static void DrawWrapped(const AppFont* f, int x, int baseline, int lineH, const char* s, Rgb col, int maxW, int lines) {
        for (int l = 0; l < lines && *s; l++) {
            int n = (int)strlen(s);
            if (l < lines - 1 && TextWidth(f, s) > maxW) {
                int cut = 0;
                for (int i = 1; i <= n; i++) {
                    if (s[i] != ' ' && s[i] != 0) continue;
                    if (TextWidth(f, s, i) > maxW) break;
                    cut = i;
                }
                if (cut == 0) cut = n;
                char line[160];
                int m = cut < (int)sizeof(line) - 1 ? cut : (int)sizeof(line) - 1;
                memcpy(line, s, m); line[m] = 0;
                DrawText(f, x, baseline + l * lineH, line, col, maxW);
                s += cut;
                while (*s == ' ') s++;
                continue;
            }
            DrawText(f, x, baseline + l * lineH, s, col, maxW);
            break;
        }
    }

    /* Copy mask: pixels left of Inset(y) (and right of PixW - Inset(y)) are not copied, which
       rounds the panel's corners. */
    static int Inset(int y) {
        int r = U(PanelRadius);
        if (y >= r && y < s_PixH - r) return 0;
        if (y >= r) y = s_PixH - 1 - y;
        float dy = r - (y + 0.5f);
        float dx = sqrtf((float)r * r - dy * dy);
        int in = (int)ceilf(r - dx - 0.5f);
        return in < 0 ? 0 : in;
    }

    /* ---------------------------------------------------------------- menu state */

    enum class State { Closed, Opening, Open, Draining };
    static std::atomic<State> s_State{State::Closed};
    static int s_Sel = 0;
    static float s_ScrollPx = 0.0f;   /* in 720p units */
    static bool s_Dirty = true;
    static bool s_Broken = false;

    /* Input latched by the hid hooks (any thread), consumed once per presented frame. */
    static std::atomic<u64> s_InHeld{0};
    static std::atomic<u32> s_InSeq{0};
    static u32 s_LastSeq = 0;
    static u64 s_Held = 0, s_PrevHeld = 0;
    static bool s_InputHooked = false;
    static int s_FlipOverride = -1;
    static AllocFn s_Alloc = nullptr;

    constexpr u64 BtnA = 1u << 0, BtnB = 1u << 1, BtnL = 1u << 6, BtnR = 1u << 7;
    constexpr u64 BtnLeft = (1u << 12) | (1u << 16), BtnUp = (1u << 13) | (1u << 17);
    constexpr u64 BtnRight = (1u << 14) | (1u << 18), BtnDown = (1u << 15) | (1u << 19);

    struct NpadBase { int64_t sampling; u64 buttons; int32_t lx, ly, rx, ry; u32 attributes; u32 reserved; };
    static_assert(sizeof(NpadBase) == 0x28);

    void OnNpadStates(void* states, int count) {
        if (!states || count <= 0) return;
        NpadBase* s = reinterpret_cast<NpadBase*>(states);
        /* [0] is the newest. Disconnected controllers read as no buttons. */
        s_InHeld.fetch_or(s[0].buttons, std::memory_order_relaxed);
        s_InSeq.fetch_add(1, std::memory_order_release);
        if (s_State.load(std::memory_order_acquire) == State::Closed) return;
        for (int i = 0; i < count && i < 16; i++) {
            s[i].buttons = 0;
            s[i].lx = s[i].ly = s[i].rx = s[i].ry = 0;
        }
    }

    void SetAllocator(AllocFn alloc) { s_Alloc = alloc; }
    void SetInputHooked() { s_InputHooked = true; }
    void SetFlipOverride(int flip) { s_FlipOverride = flip < 0 ? -1 : flip ? 1 : 0; }
    int FlipOverride() { return s_FlipOverride; }
    bool IsOpen() { return s_State.load() != State::Closed; }

#ifdef __aarch64__
    static u64 Ticks() { u64 v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
    static u64 TickFreq() { u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#else   /* host build of the drawing code (preview tool) */
    static u64 Ticks() { return 0; }
    static u64 TickFreq() { return 19200000; }
#endif

    /* Once per presented frame: latest held buttons. */
    static u64 s_LastInputTick = 0;

    static void PollInput() {
        u32 seq = s_InSeq.load(std::memory_order_acquire);
        s_PrevHeld = s_Held;
        if (seq != s_LastSeq) {
            s_LastSeq = seq;
            s_Held = s_InHeld.exchange(0, std::memory_order_relaxed);
            s_LastInputTick = Ticks();
        }
    }

    /* The game stopped reading the controller (loading, sleep...): nothing could close the panel. */
    static bool InputStalled() { return Ticks() - s_LastInputTick > TickFreq() * 2; }

    /* Held direction with key repeat, quick: first repeat after 0.2 s, then every 50 ms, and every
       25 ms once held for a second. Returns how many steps to take this frame (the game runs at
       30 fps, so a fast repeat can be more than one step per frame). */
    struct Repeat { u64 next = 0, start = 0; };
    static int Fire(u64 mask, Repeat& r) {
        bool held = s_Held & mask, was = s_PrevHeld & mask;
        if (!held) return 0;
        u64 now = Ticks(), f = TickFreq();
        if (!was) { r.start = now; r.next = now + f / 5; return 1; }
        int steps = 0;
        while (now >= r.next && steps < 4) {
            r.next += now - r.start > f ? f / 40 : f / 20;
            steps++;
        }
        if (steps == 4 && now >= r.next) r.next = now;   /* don't pile up after a hitch */
        return steps;
    }

    static bool Pressed(u64 mask) { return (s_Held & mask) && !(s_PrevHeld & mask); }

    static void HandleInput() {
        static Repeat up, down, left, right, pgUp, pgDown;
        int count = AppRowCount();
        if (count <= 0) return;
        int sel = s_Sel;
        for (int n = Fire(BtnUp, up); n > 0; n--) sel = sel > 0 ? sel - 1 : (Pressed(BtnUp) ? count - 1 : 0);
        for (int n = Fire(BtnDown, down); n > 0; n--) sel = sel < count - 1 ? sel + 1 : (Pressed(BtnDown) ? 0 : count - 1);
        if (Fire(BtnL, pgUp)) sel = sel - VisibleRows < 0 ? 0 : sel - VisibleRows;
        if (Fire(BtnR, pgDown)) sel = sel + VisibleRows >= count ? count - 1 : sel + VisibleRows;
        if (sel != s_Sel) { s_Sel = sel; s_Dirty = true; }
        AppRow row;
        AppRowGet(s_Sel, &row);
        if (Pressed(BtnA)) { AppRowChange(s_Sel, 0); s_Dirty = true; }
        int l = row.value ? Fire(BtnLeft, left) : Pressed(BtnLeft), r = row.value ? Fire(BtnRight, right) : Pressed(BtnRight);
        for (; l > 0; l--) { AppRowChange(s_Sel, -1); s_Dirty = true; }
        for (; r > 0; r--) { AppRowChange(s_Sel, 1); s_Dirty = true; }
        if (Pressed(BtnB)) {
            s_State.store(State::Draining);
            Log("app: closed");
        }
    }

    /* Keeps the selected row inside the visible rows; the list glides there. */
    static bool StepScroll() {
        int count = AppRowCount();
        float maxScroll = (float)((count > VisibleRows ? count - VisibleRows : 0) * RowH);
        static float target = 0.0f;
        float top = (float)(s_Sel * RowH), bottom = top + RowH;
        if (top < target) target = top;
        if (bottom > target + VisibleRows * RowH) target = bottom - VisibleRows * RowH;
        if (target > maxScroll) target = maxScroll;
        if (target < 0) target = 0;
        float d = target - s_ScrollPx;
        if (d > -1.0f && d < 1.0f) {
            if (s_ScrollPx == target) return false;
            s_ScrollPx = target;
            return true;
        }
        s_ScrollPx += d * 0.7f;   /* snappy glide: there in about 3 frames */
        return true;
    }

    /* ---------------------------------------------------------------- drawing the panel */

    static void DrawSwitch(int right, int y, bool on, bool sel) {
        int w = U(62), h = U(28), x = right - w, top = y + (U(RowH) - h) / 2;
        FillRound(x, top, w, h, h / 2, on ? ColOn : sel ? ColOffSel : ColOff);
        DrawText(s_F->small, x + w / 2, top + (h + s_F->small->ascent * 7 / 10) / 2, on ? "ON" : "OFF",
                 on ? ColWhite : ColOffText, 0, 1);
    }

    static void DrawValue(int right, int y, int number, bool sel) {
        int w = U(104), h = U(28), x = right - w, top = y + (U(RowH) - h) / 2;
        FillRound(x, top, w, h, h / 2, ColValue);
        char buf[16];
        int len = 0;
        for (int v = number < 0 ? 0 : number, d = 1000; d; d /= 10)
            if (v >= d || d == 1 || len) buf[len++] = (char)('0' + (v / d) % 10);
        buf[len++] = 'x'; buf[len] = 0;
        int base = top + (h + s_F->small->ascent * 7 / 10) / 2;
        DrawText(s_F->small, x + w / 2, base, buf, ColWhite, 0, 1);
        if (sel) {
            DrawText(s_F->small, x + U(14), base, "<", ColArrow, 0, 1);
            DrawText(s_F->small, x + w - U(14), base, ">", ColArrow, 0, 1);
        }
    }

    static void DrawKey(int& x, int base, const char* key, const char* what) {
        /* Right to left: label, then the round key. */
        int tw = TextWidth(s_F->small, what);
        x -= tw;
        DrawText(s_F->small, x, base, what, ColText);
        int rad = U(11), kw = TextWidth(s_F->small, key);
        int bw = kw + U(10) > rad * 2 ? kw + U(10) : rad * 2;
        x -= U(6) + bw;
        FillRound(x, base - s_F->small->ascent * 7 / 10 / 2 - rad, bw, rad * 2, rad, ColKey);
        DrawText(s_F->small, x + bw / 2, base, key, ColWhite, 0, 1);
        x -= U(18);
    }

    /* A little gyroid for the header watermark, one char per pixel. */
    static const char* const k_Gyroid[] = {
        "......OOOO......",
        "....OOhhhhOO....",
        "...OhLLhhhhhO...",
        "..OhLhhhhhhhdO..",
        "..OhhEhhhhEhdO..",
        "..OhhEhhhhEhdO..",
        "..OhhhhhhhhhdO..",
        "..OhhhhMMhhhdO..",
        "...OhhhMMhhdO...",
        "....OOhhhhOO....",
        "...OcccccccsO...",
        "..OccccccccssO..",
        "OaOccccccccssOaO",
        "..OccccccccssO..",
        ".OcccccccccsssO.",
        ".OOOOOOOOOOOOOO.",
    };
    constexpr int GyroidW = 16, GyroidH = 16;

    static void DrawGyroid(int x, int y, int px) {
        for (int r = 0; r < GyroidH; r++) {
            const char* line = k_Gyroid[r];
            for (int c = 0; line[c]; c++) {
                Rgb col;
                switch (line[c]) {
                    case 'O': col = { 92, 58, 34 }; break;     /* outline */
                    case 'h': col = { 214, 162, 98 }; break;   /* clay head */
                    case 'L': col = { 240, 204, 148 }; break;  /* shine */
                    case 'd': col = { 182, 130, 74 }; break;   /* head shade */
                    case 'E': case 'M': col = { 58, 36, 22 }; break;   /* eyes, mouth */
                    case 'c': col = { 196, 140, 80 }; break;   /* body */
                    case 's': col = { 160, 110, 60 }; break;   /* body shade */
                    case 'a': col = { 214, 162, 98 }; break;   /* arms */
                    default: continue;
                }
                FillRect(x + c * px, y + r * px, px, px, col);
            }
        }
    }

    static void Render() {
        s_ClipY0 = 0; s_ClipY1 = s_PixH;
        int W = s_PixW, H = s_PixH;
        /* frame + background */
        FillRect(0, 0, W, H, ColBorder);
        int b = U(BorderW) < 1 ? 1 : U(BorderW);
        FillRound(b, b, W - 2 * b, H - 2 * b, U(PanelRadius) - b, ColBg);
        /* header: vertical gradient, clipped to the inner rounded shape at the top */
        int hh = U(HeaderH);
        for (int y = b; y < hh; y++) {
            float t = (float)(y - b) / (float)(hh - b);
            Rgb c = { (u8)(ColHeaderTop.r + (ColHeaderBottom.r - ColHeaderTop.r) * t),
                      (u8)(ColHeaderTop.g + (ColHeaderBottom.g - ColHeaderTop.g) * t),
                      (u8)(ColHeaderTop.b + (ColHeaderBottom.b - ColHeaderTop.b) * t) };
            s_ClipY0 = y; s_ClipY1 = y + 1;
            FillRound(b, b, W - 2 * b, hh * 2, U(PanelRadius) - b, c);
        }
        s_ClipY0 = 0; s_ClipY1 = H;
        DrawText(s_F->title, U(26), U(42), "Gyroid's menu", ColTitle);
        /* watermark: "made by saiplatano" and the gyroid, right side of the header */
        int px = U(3) < 2 ? 2 : U(3);
        int gx = W - U(24) - GyroidW * px, gy = (hh + b - GyroidH * px) / 2;
        DrawGyroid(gx, gy, px);
        DrawText(s_F->small, gx - U(10), U(40), "made by saiplatano", ColSubtitle, 0, 2);

        /* faint polka dots on the paper, like the game's menus */
        s_ClipY0 = U(HeaderH); s_ClipY1 = U(FooterTop);
        for (int gy = 0, yy = HeaderH + 12; yy < FooterTop; yy += 16, gy++)
            for (int xx = 14 + (gy & 1) * 8; xx < PanelW - 8; xx += 16)
                FillRound(U(xx) - U(2), U(yy) - U(2), U(4), U(4), U(2), ColDots);
        s_ClipY0 = 0; s_ClipY1 = H;

        /* list */
        int count = AppRowCount();
        bool bar = count > VisibleRows;
        int right = W - U(bar ? 34 : 24);
        s_ClipY0 = U(ListTop); s_ClipY1 = U(ListBottom);
        int first = (int)(s_ScrollPx / RowH);
        for (int i = first; i < count && i <= first + VisibleRows; i++) {
            AppRow row;
            AppRowGet(i, &row);
            int y = U(ListTop + i * RowH - s_ScrollPx);
            bool sel = i == s_Sel;
            if (sel) {   /* the game's yellow cursor: darker rim, bright fill */
                FillRound(U(12), y + U(3), right + U(10) - U(12), U(RowH - 6), U(14), ColSelectEdge);
                FillRound(U(12) + b, y + U(3) + b, right + U(10) - U(12) - 2 * b, U(RowH - 6) - 2 * b, U(14) - b, ColSelect);
            }
            else if (i + 1 < count && i + 1 != s_Sel && ListTop + (i + 1) * RowH - s_ScrollPx < ListBottom - 1) FillRect(U(26), y + U(RowH) - 1, right - U(26), b > 1 ? 2 : 1, ColLine);
            int ctl = row.value ? U(104) : U(62);
            DrawText(s_F->body, U(28), y + U(29), row.label, ColText, right - ctl - U(16) - U(28));
            if (row.value) DrawValue(right, y, row.number, sel);
            else DrawSwitch(right, y, row.on, sel);
        }
        s_ClipY0 = 0; s_ClipY1 = H;
        if (bar) {
            int tx = W - U(20), ty = U(ListTop + 6), th = U(VisibleRows * RowH - 12), tw = U(6);
            FillRound(tx, ty, tw, th, tw / 2, ColTrack);
            int thumbH = th * VisibleRows / count;
            if (thumbH < U(24)) thumbH = U(24);
            float maxScroll = (float)((count - VisibleRows) * RowH);
            int thumbY = ty + (int)((th - thumbH) * (maxScroll > 0 ? s_ScrollPx / maxScroll : 0));
            FillRound(tx, thumbY, tw, thumbH, tw / 2, ColThumb);
        }

        /* help for the selected row */
        FillRect(U(20), U(HelpTop), W - U(40), 1 + (b > 1), ColLine);
        if (s_Sel < count) {
            AppRow row;
            AppRowGet(s_Sel, &row);
            DrawWrapped(s_F->small, U(28), U(HelpTop + 22), U(20), row.help ? row.help : "", ColDim, W - U(56), 2);
        }

        /* footer with the buttons */
        s_ClipY0 = U(FooterTop); s_ClipY1 = H - b;
        FillRound(b, U(FooterTop) - U(PanelRadius), W - 2 * b, H - b - U(FooterTop) + U(PanelRadius), U(PanelRadius) - b, ColFooter);
        s_ClipY0 = 0; s_ClipY1 = H;
        int base = U(FooterTop) + (H - b - U(FooterTop)) / 2 + s_F->small->ascent * 7 / 10 / 2;
        int x = W - U(24);
        DrawKey(x, base, "B", "Close");
        DrawKey(x, base, "A", "Switch");
        AppRow selRow = {};
        if (s_Sel < count) AppRowGet(s_Sel, &selRow);
        DrawKey(x, base, "<>", selRow.value ? "Change" : "Off / On");
        DrawText(s_F->small, U(24), base, "L / R  Page", ColDim);
    }

    /* ---------------------------------------------------------------- GPU side */

    static NvnObject s_PoolBuilder, s_PixPool, s_CmdPool, s_Cmd;
    static u8* s_PixMem = nullptr;
    static u8* s_CmdMem = nullptr;
    static u8* s_CtlMem = nullptr;
    constexpr size_t PixBytes = ((size_t)MaxPixW * MaxPixH * 4 + 0xfff) & ~(size_t)0xfff;
    constexpr int CmdSlots = 4;
    constexpr size_t CmdSlotBytes = 0x4000, CtlSlotBytes = 0x2000;
    static bool s_GpuReady = false;
    static u64 s_PixAddr = 0;
    static int s_Slot = 0;

    static bool InitPool(NvnObject& pool, void* mem, size_t size, int flags) {
        P.PoolBuilderSetDefaults(&s_PoolBuilder);
        P.PoolBuilderSetDevice(&s_PoolBuilder, s_Device);
        P.PoolBuilderSetStorage(&s_PoolBuilder, mem, size);
        P.PoolBuilderSetFlags(&s_PoolBuilder, flags);
        return P.PoolInitialize(&pool, &s_PoolBuilder) != 0;
    }

    static bool InitGpu() {
        if (s_GpuReady) return true;
        if (!LoadProcs() || !s_PixMem || !s_CmdMem || !s_CtlMem) return false;
        if (!InitPool(s_PixPool, s_PixMem, PixBytes, PoolCpuCached | PoolGpuUncached) &&
            !InitPool(s_PixPool, s_PixMem, PixBytes, PoolCpuCached | PoolGpuCached)) {
            Log("app: could not create the picture memory pool");
            return false;
        }
        if (!InitPool(s_CmdPool, s_CmdMem, CmdSlots * CmdSlotBytes, PoolCpuUncached | PoolGpuCached)) {
            Log("app: could not create the command memory pool");
            return false;
        }
        if (!P.CmdInitialize(&s_Cmd, s_Device)) {
            Log("app: could not create the command buffer");
            return false;
        }
        s_Canvas = reinterpret_cast<u32*>(P.PoolMap(&s_PixPool));
        s_PixAddr = P.PoolGetBufferAddress(&s_PixPool);
        if (!s_Canvas || !s_PixAddr) { Log("app: picture pool not mapped"); return false; }
        s_GpuReady = true;
        Log("app: drawing ready (pool %p, gpu %lx)", s_Canvas, (unsigned long)s_PixAddr);
        return true;
    }

    static bool FormatOk(int fmt, bool* bgr) {
        switch (fmt) {
            case FormatRGBA8: case FormatRGBX8Srgb: case FormatRGBA8Srgb: *bgr = false; return true;
            case FormatBGRX8: case FormatBGRA8: case FormatBGRX8Srgb: case FormatBGRA8Srgb: *bgr = true; return true;
        }
        return false;
    }

    /* Copies the panel into the texture at (dx, dy), skipping the rounded-off corner pixels. */
    static void SubmitCopy(void* queue, void* tex, int dx, int dy) {
        s_Slot = (s_Slot + 1) % CmdSlots;
        P.CmdAddCommandMemory(&s_Cmd, &s_CmdPool, (ptrdiff_t)(s_Slot * CmdSlotBytes), CmdSlotBytes);
        P.CmdAddControlMemory(&s_Cmd, s_CtlMem + s_Slot * CtlSlotBytes, CtlSlotBytes);
        P.CmdBeginRecording(&s_Cmd);
        ptrdiff_t stride = (ptrdiff_t)s_PixW * 4;
        P.CmdSetCopyRowStride(&s_Cmd, stride);
        int r = U(PanelRadius);
        auto copyRows = [&](int y0, int rows) {
            int in = Inset(y0);
            NvnCopyRegion reg = { dx + in, dy + y0, 0, s_PixW - 2 * in, rows, 1 };
            P.CmdCopyBufferToTexture(&s_Cmd, s_PixAddr + (u64)(y0 * stride) + (u64)in * 4, tex, nullptr, &reg, 0);
        };
        for (int y = 0; y < r; y++) copyRows(y, 1);
        copyRows(r, s_PixH - 2 * r);
        for (int y = s_PixH - r; y < s_PixH; y++) copyRows(y, 1);
        u64 handle = P.CmdEndRecording(&s_Cmd);
        P.QueueSubmitCommands(queue, 1, &handle);
    }

    static void Frame(void* queue, int index) {
        PollInput();
        State st = s_State.load();
        if (st == State::Draining) {
            if (s_Held == 0 || InputStalled()) { s_State.store(State::Closed); s_Held = s_PrevHeld = 0; }
            return;
        }
        if (st == State::Opening) {
            if (!InitGpu()) {
                Log("app: can't draw - the old menu opens from now on");
                s_Broken = true;
                s_State.store(State::Draining);
                return;
            }
            s_State.store(State::Open);
            s_LastInputTick = Ticks();
            s_PrevHeld = s_Held;   /* the press that opened the app isn't one for the menu */
            s_Dirty = true;
        } else if (InputStalled()) {
            Log("app: the game stopped reading the controller - closed");
            s_State.store(State::Closed);
            return;
        } else {
            HandleInput();
            if (s_State.load() != State::Open) return;
        }
        if (index < 0 || index >= s_TextureCount || !s_Textures[index]) return;
        void* tex = s_Textures[index];
        int tw = P.TextureGetWidth(tex), th = P.TextureGetHeight(tex);
        bool bgr = false;
        if (!FormatOk(P.TextureGetFormat(tex), &bgr)) return;
        int cx = 0, cy = 0, cw = tw, ch = th;
        if (s_Crop[2] > 0 && s_Crop[3] > 0 && s_Crop[0] >= 0 && s_Crop[1] >= 0 &&
            s_Crop[0] + s_Crop[2] <= tw && s_Crop[1] + s_Crop[3] <= th) {
            cx = s_Crop[0]; cy = s_Crop[1]; cw = s_Crop[2]; ch = s_Crop[3];
        }
        float scale = ch >= 900 ? MaxScale : 1.0f;
        bool flip = s_FlipOverride >= 0 ? s_FlipOverride == 1 : s_OriginMode != OriginUpperLeft;
        if (scale != s_Scale || bgr != s_Bgr || flip != s_Flip || !s_PixW) {
            s_Scale = scale; s_Bgr = bgr; s_Flip = flip;
            s_F = scale > 1.0f ? &k_Fonts2 : &k_Fonts1;
            s_PixW = U(PanelW); s_PixH = U(PanelH);
            s_Dirty = true;
        }
        if (s_PixW > cw || s_PixH > ch) return;
        if (StepScroll()) s_Dirty = true;
        if (s_Dirty) {
            s_Dirty = false;
            Render();
            P.PoolFlushMappedRange(&s_PixPool, 0, (size_t)s_PixW * s_PixH * 4);
        }
        int dx = cx + (cw - s_PixW) / 2, dy = cy + (ch - s_PixH) / 2;
        SubmitCopy(queue, tex, dx, dy);
    }

    static void Present(void* queue, void* window, int index) {
        if (s_Presents.fetch_add(1) == 0 && s_TextureCount > 0 && LoadProcs()) {
            void* t = s_Textures[0];
            Log("app: first frame, window %dx%d format 0x%x, crop %d,%d %dx%d, origin %d",
                P.TextureGetWidth(t), P.TextureGetHeight(t), P.TextureGetFormat(t),
                s_Crop[0], s_Crop[1], s_Crop[2], s_Crop[3], s_OriginMode);
        }
        if (s_State.load(std::memory_order_acquire) != State::Closed) Frame(queue, index);
        s_RealPresent(queue, window, index);
    }

    /* ---------------------------------------------------------------- opening */

    bool Ready() {
        auto no = [](const char* why) { Log("app: using the old menu (%s)", why); return false; };
        if (s_Broken) return no("drawing failed earlier");
        if (!s_RealPresent || !s_Device) return no("NVN not seen");
        if (s_TextureCount <= 0 || s_Presents.load() == 0) return no("no window textures");
        if (!s_InputHooked || s_InSeq.load() == 0) return no("controller reads not hooked");
        if (!LoadProcs()) return no("NVN functions missing");
        bool bgr;
        int fmt = P.TextureGetFormat(s_Textures[0]);
        if (!FormatOk(fmt, &bgr)) { Log("app: window format 0x%x", fmt); return no("unknown window format"); }
        if (!s_PixMem) {
            if (!s_Alloc) return no("no allocator");
            s_PixMem = static_cast<u8*>(s_Alloc(0x1000, PixBytes));
            s_CmdMem = static_cast<u8*>(s_Alloc(0x1000, CmdSlots * CmdSlotBytes));
            s_CtlMem = static_cast<u8*>(s_Alloc(0x1000, CmdSlots * CtlSlotBytes));
            if (!s_PixMem || !s_CmdMem || !s_CtlMem) {
                s_Broken = true;
                return no("out of memory");
            }
        }
        return true;
    }

    void Open() {
        State expect = State::Closed;
        if (s_State.compare_exchange_strong(expect, State::Opening)) {
            s_Dirty = true;
            Log("app: opened");
        }
    }

}
