// Header-only helpers over pizzarivals_protocol.h, used by BOTH bridges (x86 and x64).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "pizzarivals_protocol.h"

namespace pr {

// Either side may start first: whoever gets there creates the zeroed mapping and stamps it.
inline PRShared* open_shared(HANDLE* out_handle = nullptr) {
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)PR_SHM_SIZE, PR_SHM_NAME);
    if (!h) return nullptr;
    void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { CloseHandle(h); return nullptr; }
    auto* s = (PRShared*)p;
    if (InterlockedCompareExchange((volatile LONG*)&s->magic, (LONG)PR_MAGIC, 0) == 0) s->version = PR_PROTOCOL_VERSION;
    if (out_handle) *out_handle = h;
    return s->version == PR_PROTOCOL_VERSION ? s : nullptr;
}

// ---- seqlock latest-value slots ----
template <class T> inline void slot_write(PRSlotHdr& h, T& dst, const T& src) {
    InterlockedIncrement((volatile LONG*)&h.seq);          // odd: writing
    MemoryBarrier(); dst = src; MemoryBarrier();
    InterlockedIncrement((volatile LONG*)&h.seq);          // even: done
}
template <class T> inline void slot_read(PRSlotHdr& h, const T& src, T& out) {
    for (int i = 0; i < 1000; ++i) {
        uint32_t a = h.seq;
        if (a & 1) { YieldProcessor(); continue; }
        MemoryBarrier(); out = src; MemoryBarrier();
        if (h.seq == a) return;
    }
}

// ---- SPSC event rings (producer owns head, consumer owns tail) ----
inline bool ring_push(PRRing& r, uint16_t type, const void* payload, uint16_t len) {
    uint32_t h = r.head;
    if (h - r.tail >= PR_EVENT_RING_SIZE || len > sizeof(PREvent::payload)) return false;
    PREvent& e = r.ev[h & (PR_EVENT_RING_SIZE - 1)];
    e.type = type; e.len = len;
    if (len) memcpy(e.payload, payload, len);
    MemoryBarrier();
    r.head = h + 1;
    return true;
}
inline bool ring_pop(PRRing& r, PREvent& out) {
    uint32_t t = r.tail;
    if (t == r.head) return false;
    MemoryBarrier();
    out = r.ev[t & (PR_EVENT_RING_SIZE - 1)];
    MemoryBarrier();
    r.tail = t + 1;
    return true;
}

} // namespace pr
