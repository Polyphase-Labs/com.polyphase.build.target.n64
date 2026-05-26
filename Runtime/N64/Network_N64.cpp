/**
 * @file Network_N64.cpp
 * @brief N64 network stub. Stock N64 hardware has no network — the only
 *        accessories that ever provided one (Modem Pak, 64DD, RandNet) are
 *        either unsupported in libdragon or rare hardware that doesn't
 *        warrant a runtime path here.
 *
 * Every NET_* function reports "no network" — sockets fail to create,
 * sends/receives error, IP lookup returns 0. The engine's higher layers
 * (NetDatum sync, multiplayer scenes, HTTP, etc.) gate themselves on
 * NET_IsActive() returning false and degrade to single-player gracefully.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Network/Network.h"
#include "Log.h"

#include <libdragon.h>  // debugf macro

#include <cstdint>
#include <cstring>

void NET_Initialize()
{
    LogDebug("Network_N64: stock N64 has no network - NET_* stubbed");
}

void NET_Shutdown() {}
void NET_Update() {}

bool NET_IsActive() { return false; }

SocketHandle NET_SocketCreate() { return -1; }
void NET_SocketBind(SocketHandle /*socketHandle*/, uint32_t /*ipAddr*/, uint16_t /*port*/) {}
int32_t NET_SocketRecv(SocketHandle /*socketHandle*/, char* /*buffer*/, uint32_t /*size*/) { return -1; }
int32_t NET_SocketRecvFrom(SocketHandle /*socketHandle*/, char* /*buffer*/, uint32_t /*size*/, uint32_t& addr, uint16_t& port)
{
    addr = 0;
    port = 0;
    return -1;
}
int32_t NET_SocketSendTo(SocketHandle /*socketHandle*/, const char* /*buffer*/, uint32_t /*size*/, uint32_t /*addr*/, uint16_t /*port*/)
{
    return -1;
}
void NET_SocketClose(SocketHandle /*socketHandle*/) {}
void NET_SocketSetBlocking(SocketHandle /*socketHandle*/, bool /*blocking*/) {}
void NET_SocketSetBroadcast(SocketHandle /*socketHandle*/, bool /*broadcast*/) {}
void NET_SocketGetIpAndPort(SocketHandle /*socketHandle*/, uint32_t& ipAddr, uint16_t& port)
{
    ipAddr = 0;
    port = 0;
}

SocketHandle NET_SocketCreateStream() { return -1; }
bool NET_SocketConnect(SocketHandle /*socketHandle*/, uint32_t /*ipAddr*/, uint16_t /*port*/, int32_t /*timeoutMs*/) { return false; }
int32_t NET_SocketSend(SocketHandle /*socketHandle*/, const char* /*buffer*/, uint32_t /*size*/) { return -1; }

uint32_t NET_ResolveHost(const char* /*hostname*/) { return 0; }

uint32_t NET_IpStringToUint32(const char* /*ipString*/) { return 0; }
void NET_IpUint32ToString(uint32_t /*ipUint32*/, char* outIpString)
{
    if (outIpString) outIpString[0] = '\0';
}

uint32_t NET_GetIpAddress() { return 0; }
uint32_t NET_GetSubnetMask() { return 0; }

#endif // POLYPHASE_PLATFORM_ADDON
