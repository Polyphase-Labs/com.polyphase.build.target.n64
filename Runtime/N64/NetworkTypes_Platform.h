/**
 * @file NetworkTypes_Platform.h
 * @brief N64 platform extension for the engine's `NetworkTypes.h` fork.
 *
 * Stock N64 hardware has no network. The Modem Pak / 64DD network features
 * are experimental in libdragon and not wired up here. All NET_* calls
 * return -1 / "not supported" — see Network_N64.cpp. SocketHandle stays
 * mapped to int32_t to match the rest of the Unix-like platforms.
 */

#pragma once
#include <stdint.h>

typedef int32_t SocketHandle;
