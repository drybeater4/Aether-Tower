// Helpers for reading/writing RoA's GML data from native code (x86, GM 1.4 YYC layout).
//
// Arrays: RValue.valueArray -> { int refcount; Row* rows; ... }, Row = { int length; RValue* cells },
// cell = 16-byte RValue. 1-D GML arrays live in row 0 and are indexed by column.
#pragma once
#include "loader_api.h"
#include <string>
#include <sstream>
#include <cstring>
#include <cstdio>

namespace gml {

inline bool peek(const void* p, void* out, size_t n) {
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool poke(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline std::string cell_str(const unsigned char* c) {
    int type = *(const int*)(c + 12);
    char buf[160];
    switch (type) {
    case GML_TYPE_REAL: case GML_TYPE_BOOL: sprintf_s(buf, "%g", *(const double*)c); return buf;
    case GML_TYPE_INT32: sprintf_s(buf, "%d", *(const int*)c); return buf;
    case GML_TYPE_INT64: sprintf_s(buf, "%lld", *(const long long*)c); return buf;
    case GML_TYPE_UNDEFINED: return "undefined";
    case GML_TYPE_ARRAY: return "<array>";
    case GML_TYPE_STRING: {
        uint32_t ref[3]; uint32_t sp = *(const uint32_t*)c;
        if (!peek((void*)(uintptr_t)sp, ref, sizeof ref)) return "<badstr>";
        char s[72] = {}; if (!peek((void*)(uintptr_t)ref[0], s, 64)) return "<badstr>";
        s[64] = 0; return std::string("\"") + s + "\"";
    }
    }
    sprintf_s(buf, "<type %d>", type); return buf;
}

inline double cell_real(const unsigned char* c) {
    int type = *(const int*)(c + 12);
    if (type == GML_TYPE_REAL || type == GML_TYPE_BOOL) return *(const double*)c;
    if (type == GML_TYPE_INT32) return *(const int*)c;
    if (type == GML_TYPE_INT64) return (double)*(const long long*)c;
    return 0;
}

// Pointer to cell [row][col] of an array value, or null.
inline unsigned char* array_cell(const RValue& v, int row, int col) {
    if (v.type != GML_TYPE_ARRAY || !v.valueArray) return nullptr;
    uint32_t top[2]; if (!peek(v.valueArray, top, sizeof top)) return nullptr;
    uint32_t r[2];   if (!peek((void*)(uintptr_t)(top[1] + row * 8), r, sizeof r)) return nullptr;
    if (col < 0 || (uint32_t)col >= r[0] || r[1] < 0x10000) return nullptr;
    return (unsigned char*)(uintptr_t)(r[1] + col * 16);
}

// 1-D global array element (RoA's per-player arrays are 1-based: player 1 is index 1).
inline double garr_get(const char* name, int idx, double fallback = 0) {
    RValue v = roa::global_get(name);
    unsigned char* c = array_cell(v, 0, idx);
    unsigned char buf[16];
    return (c && peek(c, buf, 16)) ? cell_real(buf) : fallback;
}
inline bool garr_set(const char* name, int idx, double value) {
    RValue v = roa::global_get(name);
    unsigned char* c = array_cell(v, 0, idx);
    if (!c) return false;
    RValue nv; nv.setReal(value);
    return poke(c, &nv, 16);
}

inline double gget(const char* name, double fallback = 0) {
    if (!roa::global_exists(name)) return fallback;
    return roa::real_of(roa::global_get(name));
}
inline void gset(const char* name, double v) { roa::call("variable_global_set", { name, v }); }

inline double iget(double inst, const char* name, double fallback = 0) {
    RValue v = roa::inst_get(inst, name);
    return v.type == GML_TYPE_UNDEFINED ? fallback : roa::real_of(v);
}
inline void iset(double inst, const char* name, double v) { roa::call("variable_instance_set", { inst, name, v }); }

inline double asset(const char* name) { return roa::call_real("asset_get_index", { name }); }
inline int inst_count(double obj) { return (int)roa::call_real("instance_number", { obj }); }
inline double inst_find(double obj, int i) { return roa::call_real("instance_find", { obj, i }); }

} // namespace gml
