#include "loader_api.h"
#include <unordered_map>
#include <cstdio>
#include <mutex>

namespace roa {

static HMODULE g_loader = nullptr;
static void*  (*p_func_ptr)(const char*) = nullptr;
static void   (*p_add_present)(PresentCb) = nullptr;
static void*  (*p_window)() = nullptr;
static int    (*p_hook_create)(void*, void*, void**) = nullptr;
static int    (*p_hook_enable)(void*) = nullptr;
static RValue* (*p_call)(const char*, unsigned, RValue**) = nullptr;
int g_sig = -1;
static std::unordered_map<std::string, BuiltinFn> g_cache;
static std::mutex g_log_mx;

static int try_sig(int sig, BuiltinFn f) {
    // abs(-3) must return 3. SEH-guarded: the wrong convention may fault instead of returning.
    RValue arg; arg.setReal(-3);
    RValue out;
    __try {
        if (sig == 0) ((FnA)f)(out, nullptr, nullptr, 1, &arg);
        else          ((FnB)f)(nullptr, nullptr, out, 1, &arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return out.type == GML_TYPE_REAL && out.valueReal == 3.0;
}
static void detect_signature() {
    void* f = p_func_ptr("abs");
    if (!f) return;
    for (int sig = 0; sig < 2; ++sig)
        if (try_sig(sig, f)) { g_sig = sig; return; }
}

// SEH-guarded: a bad variable/instance must not take the game down while probing.
bool g_faulted = false;
void invoke(BuiltinFn f, RValue& out, uint32_t argc, RValue* args) {
    __try {
        if (g_sig == 0) ((FnA)f)(out, nullptr, nullptr, (int)argc, args);
        else            ((FnB)f)(nullptr, nullptr, out, argc, args);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; out = RValue(); }
}

bool init() {
    if (p_func_ptr) return true;
    g_loader = GetModuleHandleA("loader.dll");
    if (!g_loader) return false;
    p_func_ptr    = (void* (*)(const char*))GetProcAddress(g_loader, "?loader_get_yyc_func_ptr@@YAPAXPBD@Z");
    p_add_present = (void (*)(PresentCb))GetProcAddress(g_loader, "?loader_add_present_callback@@YAXP6AXPAUID3D11RenderTargetView@@PAUIDXGISwapChain@@PAUID3D11Device@@PAUID3D11DeviceContext@@@Z@Z");
    p_window      = (void* (*)())GetProcAddress(g_loader, "?loader_get_window@@YAPAUHWND__@@XZ");
    p_hook_create = (int (*)(void*, void*, void**))GetProcAddress(g_loader, "?loader_hook_create@@YAHPAX0PAPAX@Z");
    p_hook_enable = (int (*)(void*))GetProcAddress(g_loader, "?loader_hook_enable@@YAHPAX@Z");
    p_call        = (RValue* (*)(const char*, unsigned, RValue**))GetProcAddress(g_loader, "?loader_yyc_call_func@@YAPAURValue@@PBDIPAPAU1@@Z");
    if (!p_call || !p_func_ptr || !p_add_present || !p_hook_create || !p_hook_enable) { p_func_ptr = nullptr; return false; }
    // The loader fills its function table on its own init thread; probe one well-known builtin.
    if (!p_func_ptr("room_get_name")) { p_func_ptr = nullptr; return false; }
    if (g_sig < 0) detect_signature();
    return g_sig >= 0;
}

BuiltinFn builtin(const char* name) {
    auto it = g_cache.find(name);
    if (it != g_cache.end()) return it->second;
    BuiltinFn f = p_func_ptr(name);
    g_cache[name] = f;
    return f;
}

void add_present_callback(PresentCb cb) { p_add_present(cb); }
void* get_window() { return p_window ? p_window() : nullptr; }
int hook(void* target, void* detour, void** original) {
    int r = p_hook_create(target, detour, original);
    if (r == 0) r = p_hook_enable(target);
    return r;
}

void log(const std::string& line) {
    std::lock_guard<std::mutex> l(g_log_mx);
    FILE* f = nullptr;
    if (fopen_s(&f, "mods/pizzarivals.log", "ab") == 0 && f) {
        fwrite(line.data(), 1, line.size(), f); fputc('\n', f); fclose(f);
    }
}

RValue call(const char* fn, std::vector<Arg> args) {
    RValue out;
    BuiltinFn f = builtin(fn);
    if (!f || g_sig < 0) return out;
    std::vector<RValue> a;
    for (auto& x : args) a.push_back(x.v);
    invoke(f, out, (uint32_t)a.size(), a.data());
    for (auto& x : args) x.v.freeValue();
    return out;
}
// RValue::getReal() in the loader's header reads BOOL from the int half of the union; RoA stores bools as doubles.
double real_of(const RValue& v) {
    switch (v.type) {
    case GML_TYPE_REAL: case GML_TYPE_BOOL: return v.valueReal;
    case GML_TYPE_INT32: return v.valueInt32;
    case GML_TYPE_INT64: return (double)v.valueInt64;
    }
    return 0;
}
double call_real(const char* fn, std::vector<Arg> args) { RValue r = call(fn, std::move(args)); return real_of(r); }
std::string call_string(const char* fn, std::vector<Arg> args) { RValue r = call(fn, std::move(args)); return r.getString(); }

bool   global_exists(const char* name) { return call_real("variable_global_exists", { name }) > 0.5; }
RValue global_get(const char* name)    { return call("variable_global_get", { name }); }
RValue inst_get(double id, const char* name) { return call("variable_instance_get", { id, name }); }
void   inst_set(double id, const char* name, Arg v) { call("variable_instance_set", { id, name, v }); }

} // namespace roa
