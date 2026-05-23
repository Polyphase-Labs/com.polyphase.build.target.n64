/**
 * @file InputTypes_Platform.h
 * @brief N64 platform extension for the engine's `InputTypes.h` fork.
 *
 * Stub — Input_N64.cpp keeps libdragon's joypad state internal. No engine-
 * facing types need to leak in. If a future phase wants to expose
 * controller-pak / rumble-pak / transfer-pak state to engine inline blocks,
 * extend here.
 */

#pragma once

#include <stdint.h>
