#pragma once

/* The Slumber phone app's own screen: a scrollable settings list drawn by the module on top of
   the game's picture (see slumber_app.cpp). This header only uses plain types so the app code
   does not depend on the hook library. */

#include <cstdint>
#include <cstddef>

namespace sl {

    void Log(const char* fmt, ...);

    /* The menu rows, provided by main.cpp. */
    struct AppRow {
        const char* label;   /* short name shown in the list */
        const char* help;    /* longer text shown under the list for the selected row */
        bool        value;   /* false: ON/OFF switch, true: number (shown as "<n>x") */
        bool        on;
        int         number;
    };
    int  AppRowCount();
    void AppRowGet(int index, AppRow* out);
    /* dir 0 = A pressed, -1 = left, +1 = right. Called from the render thread. */
    void AppRowChange(int index, int dir);

    /* The Island Editor's Bridges picker, provided by main.cpp. Materials are the picker's
       cells in bridge_icons.inc order (log, suspension, stone, wooden, brick, iron, zen, red zen). */
    int  BridgeCurrent();             /* material of the bridge being edited, -1 if unknown */
    const char* BridgeShapeText();    /* its size, e.g. "4 tiles long" (kept when the look changes) */
    /* A on a material. Called from the render thread; the change is applied on the game thread. */
    void BridgePick(int material);

}

namespace sl::app {

    using GenericFn = void (*)();
    using AllocFn = void* (*)(size_t alignment, size_t size);

    /* nvnBootstrapLoader hook: returns the function the game gets for <name> (wrapped or real). */
    GenericFn WrapProc(const char* name, GenericFn real);

    /* nn::hid state hooks, after the real call: states are NpadBaseState (0x28 bytes each).
       Reads the buttons, then clears them while the app is open. */
    void OnNpadStates(void* states, int count);

    void SetAllocator(AllocFn alloc);
    void SetInputHooked();
    void SetFlipOverride(int flip);   /* -1 auto, 0 no, 1 yes */
    int  FlipOverride();

    /* Game thread, when the phone app is tapped: can the screen be shown? (Else the old menu.) */
    bool Ready();
    void Open();
    void OpenBridges();
    /* Buttons seen by the controller hooks since the last call (game thread, once a frame). */
    uint64_t TakeButtons();   /* the Bridges picker instead of the settings list */
    bool IsOpen();

}
