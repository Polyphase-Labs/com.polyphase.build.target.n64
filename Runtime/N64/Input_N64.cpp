/**
 * @file Input_N64.cpp
 * @brief N64 controller input via libdragon's joypad API. Maps all four
 *        N64 controller ports into the engine's GamepadState slots.
 *
 * Phase 1 stub: initialises the joypad subsystem and advertises one
 * controller, but doesn't yet decode buttons/sticks per frame. Phase 2
 * fills in the per-button + per-axis mapping. The init stub is enough for
 * the engine to compile + boot.
 *
 * N64 controller layout (for Phase 2 reference):
 *   - Face: A, B
 *   - Top: Start (mapped to GAMEPAD_START)
 *   - L, R, Z (Z is a trigger; map to GAMEPAD_L2/R2 by convention)
 *   - D-pad
 *   - C-buttons (C-Up/Down/Left/Right) — these have no clean engine
 *     equivalent; map to right-stick virtual buttons (R_UP/R_DOWN/etc.)
 *     for nav compatibility.
 *   - Single analog stick (no right stick).
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <libdragon.h>

void INP_Initialize()
{
    // controller_init is called from Main_N64.cpp's boot — we just configure
    // the engine state to expect input.
    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mType = GamepadType::Standard;
    input.mGamepads[0].mConnected = true;
    input.mNumControllers = 1;

    InputInit();
    LogDebug("Input_N64: joypad initialised");
}

void INP_Shutdown()
{
    InputShutdown();
}

void INP_Update()
{
    InputAdvanceFrame();

    // Phase 1: no per-frame button/axis decode yet. controller_scan() is
    // safe to call though, so we drain it so libdragon's internal state
    // doesn't fall behind. Phase 2 reads the scanned data into
    // GetEngineState()->mInput.mGamepads[*].
    ::controller_scan();

    // Detect connected controllers so the engine knows what to expect.
    // get_controllers_present() returns a bitmask of CONTROLLER_1_INSERTED..
    // CONTROLLER_4_INSERTED. We surface controller 0 as "always connected"
    // even when absent (matches PSP behaviour — useful for emulator dev).
    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mConnected = true;

    InputPostUpdate();
}

void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
