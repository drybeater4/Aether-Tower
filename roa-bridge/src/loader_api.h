// Thin wrapper over the exports of roa-mod-loader's loader.dll (resolved with GetProcAddress, so we
// don't need the loader's import library or its vendored dependencies to build).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include "GMLScriptEnv/yoyo.h"       // RValue (header only, from the loader's UGMMS submodule)

struct ID3D11RenderTargetView; struct IDXGISwapChain; struct ID3D11Device; struct ID3D11DeviceContext;

namespace roa {

// YYC built-in signature (see roa-mod-loader README): out, self, other, argc, args.
using BuiltinFn = void*;                      // address of a YYC builtin; calling convention found at runtime
// Two candidate signatures. README of the loader shows B; the stock YoYo runner uses A. init() tests both.
using FnA = void (*)(RValue& out, CInstance* self, CInstance* other, int argc, RValue* args);
using FnB = void (*)(CInstance* self, CInstance* other, RValue& out, uint32_t argc, RValue* args);
extern bool g_faulted;                        // set when a guarded call faulted
extern int g_sig;                             // 0 = A, 1 = B, -1 = unknown
void invoke(BuiltinFn f, RValue& out, uint32_t argc, RValue* args);
using PresentCb = void (*)(ID3D11RenderTargetView*, IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*);

bool init();                                  // false until loader.dll has finished its own setup
BuiltinFn builtin(const char* name);          // cached lookup, nullptr if the game has no such function
void add_present_callback(PresentCb cb);
void* get_window();
int  hook(void* target, void* detour, void** original);   // MinHook create+enable via the loader
void log(const std::string& line);            // appends to mods/pizzarivals.log

// Call a built-in by name. Strings are marshalled and freed here.
struct Arg {
    RValue v;
    Arg(double d) { v.setReal(d); }
    Arg(int i) { v.setReal(i); }
    Arg(const char* s) { v.setString(s); }
    Arg(const std::string& s) { v.setString(s.c_str()); }
};
RValue call(const char* fn, std::vector<Arg> args = {});
double real_of(const RValue& v);
double call_real(const char* fn, std::vector<Arg> args = {});
std::string call_string(const char* fn, std::vector<Arg> args = {});

// Convenience for instance / global variables via the GML built-ins.
bool   global_exists(const char* name);
RValue global_get(const char* name);
RValue inst_get(double inst_id, const char* name);
void   inst_set(double inst_id, const char* name, Arg value);

} // namespace roa
