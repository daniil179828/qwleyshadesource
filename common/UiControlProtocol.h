#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
namespace qwui {
constexpr uint32_t Magic = 0x49555751u;
constexpr uint32_t Version = 1;
constexpr uint64_t LeaseMs = 1500;
enum Status : uint32_t { Searching=0, Ready=1, NotFound=2, Ambiguous=3,
    InvalidLayout=4, AccessFailed=5, ExternalChange=6, UnsupportedProcess=7, Stopped=8 };
enum Opcode : uint32_t { SetHidden=0, Rescan=1 };
#pragma pack(push, 8)
struct Shared {
    uint32_t magic, version, bytes, producerPid;
    uint32_t status, observedHidden, owned, lastError;
    uint64_t serverHeartbeat;
    uint32_t clientPid, requestId, ackId, requestedHide;
    uint64_t clientHeartbeat;
    uint64_t session;
    uint32_t opcode, reserved;
};
#pragma pack(pop)
static_assert(sizeof(Shared)==80, "UiControl ABI");
inline void Names(DWORD pid, wchar_t (&map)[96], wchar_t (&mutex)[96]) {
    swprintf_s(map, L"Local\\qwleyshade.UiControl.v1.%lu", (unsigned long)pid);
    swprintf_s(mutex, L"Local\\qwleyshade.UiControlLock.v1.%lu", (unsigned long)pid);
}
inline bool Valid(const Shared& s, DWORD pid) {
    return s.magic==Magic && s.version==Version && s.bytes==sizeof(Shared) && s.producerPid==pid;
}
inline bool Fresh(uint64_t now, uint64_t stamp) { return stamp && stamp<=now && now-stamp<=LeaseMs; }
inline const char* StatusText(uint32_t s) {
    switch(s) {
    case Searching:return "Searching for the game flag...";
    case Ready:return "Flag descriptor found; byte readback available";
    case NotFound:return "Flag not found in this game build";
    case Ambiguous:return "Multiple candidates; writes refused";
    case InvalidLayout:return "Descriptor layout changed; writes refused";
    case AccessFailed:return "Flag access failed";
    case ExternalChange:return "Another writer changed the flag; ownership released";
    case UnsupportedProcess:return "Not RobloxPlayerBeta.exe; search disabled";
    case Stopped:return "DLL control stopped";
    default:return "Unknown DLL status";
    }
}
}
