// Spike S1 probe: dumps every known global / player instance variable so we can learn how RoA
// starts a match. Built as mods/pizzarivals_probe.dll.
//
// GML is only ever called from inside a hooked built-in (draw_sprite_ext), i.e. from a place where
// the game's own script is already running, with the builtin calling convention verified at startup.
// An earlier version called GML from the D3D Present callback, which can fire in the middle of a
// Create event and crashed the game ("argument is not provided to script").
#include "loader_api.h"
#include <fstream>
#include <sstream>
#include <utility>
#include <cstring>
#include <cstdio>

// ---- which builtins does RoA's compiled code really call through the function table? ----
// Hook many, count calls; whichever fire become candidates for the per-frame tick.
static const char* kProbeNames[] = {
    "draw_sprite", "draw_sprite_ext", "draw_sprite_part", "draw_text", "draw_set_color", "draw_set_alpha",
    "draw_rectangle", "surface_set_target", "surface_reset_target", "shader_set", "instance_exists",
    "instance_number", "keyboard_check", "gamepad_button_check", "variable_instance_get", "variable_global_get",
    "ds_map_find_value", "ds_list_find_value", "irandom", "random", "abs", "floor", "point_distance",
    "place_meeting", "collision_rectangle", "script_execute", "room_goto", "sprite_get_width", "draw_self",
    "draw_text_ext", "draw_line", "draw_circle", "draw_set_font", "gpu_set_blendmode", "instance_create",
    "instance_destroy", "ds_map_exists", "string", "round", "min", "max", "lerp", "clamp", "sin", "cos",
    "keyboard_check_pressed", "keyboard_check_direct", "keyboard_check_released", "gamepad_axis_value", "audio_play_sound", "sprite_get_number", "array_length_1d",
};
constexpr int kN = sizeof(kProbeNames) / sizeof(kProbeNames[0]);
static int find_name(const char* n) { for (int i = 0; i < kN; ++i) if (!strcmp(kProbeNames[i], n)) return i; return -1; }
static const int kIdxGpButton = find_name("gamepad_button_check"), kIdxGpAxis = find_name("gamepad_axis_value"), kIdxKbPressed = find_name("keyboard_check_pressed");
static volatile bool g_capturing = false;

// ---- call capture: which (function, args) does the game poll, and what do they return? ----
#include <map>
struct CapKey { int fn; int a0, a1; bool operator<(const CapKey& o) const { return fn != o.fn ? fn < o.fn : a0 != o.a0 ? a0 < o.a0 : a1 < o.a1; } };
struct CapVal { int count = 0, trues = 0; double last = 0; };
static std::map<CapKey, CapVal> g_cap;
static void capture_call(int fn, RValue& out, uint32_t argc, RValue* args) {
    if (g_cap.size() > 3000) return;
    CapKey k{ fn, argc > 0 ? (int)roa::real_of(args[0]) : -9999, argc > 1 ? (int)roa::real_of(args[1]) : -9999 };
    CapVal& v = g_cap[k]; v.count++;
    double r = roa::real_of(out); v.last = r; if (r > 0.5) v.trues++;
}


static std::vector<std::string> g_candidates;
static void vkey_set(int vk, bool down);
static volatile LONG g_tick_pending = 0;        // set by Present, consumed by the hook
static int g_frame = 0;

// dump job: processed in slices so a 19k-name scan doesn't stall a frame
struct Job { bool active = false; std::string tag; size_t idx = 0; int phase = 0; double oplayer = 0; int count = 0; int inst = 0; double inst_id = 0; std::vector<std::string> arrays; std::ostringstream out; } g_job;

static std::string value_str(RValue v) {
    if (v.type == GML_TYPE_STRING) return "\"" + v.getString() + "\"";
    if (v.type == GML_TYPE_ARRAY) return "<array>";
    if (v.type == GML_TYPE_UNDEFINED) return "undefined";
    return v.toString();
}

static void load_candidates() {
    std::ifstream f("mods/pizzarivals_candidates.txt");
    std::string l;
    while (std::getline(f, l)) {
        // names that mean something special to the interpreter are not variables
        if (l.empty() || l.rfind("argument", 0) == 0 || l == "self" || l == "other" || l == "global" || l == "local" || l == "all" || l == "noone") continue;
        g_candidates.push_back(l);
    }
    roa::log("candidates: " + std::to_string(g_candidates.size()));
}


// ---- raw memory peeking, to learn how YYC lays out GML arrays ----
static bool peek(const void* p, void* out, size_t n) {
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static std::string hex(const void* p, size_t n) {
    unsigned char b[128]; if (n > sizeof b) n = sizeof b;
    if (!peek(p, b, n)) return "<unreadable>";
    std::string s; char t[4];
    for (size_t i = 0; i < n; ++i) { sprintf_s(t, "%02x", b[i]); s += t; if (i % 4 == 3) s += ' '; }
    return s;
}
static void diag_array(const char* name) {
    RValue v = roa::global_get(name);
    char hdr[200]; sprintf_s(hdr, "array %s: type=%d ptr=%p", name, v.type, v.valueArray);
    roa::log(hdr);
    if (v.type != GML_TYPE_ARRAY || !v.valueArray) return;
    roa::log("  @array: " + hex(v.valueArray, 64));
    uint32_t dw[8]; if (!peek(v.valueArray, dw, sizeof dw)) return;
    for (int i = 0; i < 8; ++i) {
        if (dw[i] < 0x10000 || dw[i] > 0x7ffe0000) continue;
        roa::log("  [dw" + std::to_string(i) + "]->" + hex((void*)(uintptr_t)dw[i], 64));
    }
}

// GM 1.4 arrays: valueArray -> { int refcount; Row* rows; ... }, Row = { int length; RValue* cells }, cell = 16-byte RValue.
static std::string cell_str(const unsigned char* c) {
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
// Appends "name[r][c] = value" lines for the first rows/cols of an array global.
static void array_lines(std::ostream& o, const char* name, const RValue& v, int max_rows, int max_cols) {
    if (v.type != GML_TYPE_ARRAY || !v.valueArray) return;
    uint32_t top[2]; if (!peek(v.valueArray, top, sizeof top)) return;
    for (int r = 0; r < max_rows; ++r) {
        uint32_t row[2];
        if (!peek((void*)(uintptr_t)(top[1] + r * 8), row, sizeof row)) break;
        if (row[0] == 0 || row[0] > 100000 || row[1] < 0x10000) break;
        for (uint32_t c = 0; c < row[0] && (int)c < max_cols; ++c) {
            unsigned char cell[16];
            if (!peek((void*)(uintptr_t)(row[1] + c * 16), cell, 16)) break;
            o << name << "[" << r << "][" << c << "] = " << cell_str(cell) << "\n";
        }
        if (row[0] > (uint32_t)max_cols) o << name << "[" << r << "] length " << row[0] << "\n";
    }
}

static void diag() {
    auto show = [](const char* what, RValue v) {
        roa::log(std::string(what) + " -> type=" + std::to_string(v.type) + " flags=" + std::to_string(v.flags) + " val=" + v.toString() + " faulted=" + (roa::g_faulted ? "1" : "0"));
        roa::g_faulted = false;
    };
    roa::g_faulted = false;
    double op = roa::call_real("asset_get_index", { "oPlayer" });
    show("asset_get_index(oPlayer)", roa::call("asset_get_index", { "oPlayer" }));
    show("instance_number(oPlayer)", roa::call("instance_number", { op }));
    RValue idv = roa::call("instance_find", { op, 0 });
    show("instance_find(oPlayer,0)", idv);
    double id = roa::real_of(idv);
    show("instance_exists(id)", roa::call("instance_exists", { id }));
    show("variable_instance_exists(id,hsp)", roa::call("variable_instance_exists", { id, "hsp" }));
    show("variable_instance_exists(id,x)", roa::call("variable_instance_exists", { id, "x" }));
    show("variable_instance_get(id,x)", roa::call("variable_instance_get", { id, "x" }));
    show("variable_instance_get(id,hsp)", roa::call("variable_instance_get", { id, "hsp" }));
    show("variable_global_exists(training_mode)", roa::call("variable_global_exists", { "training_mode" }));
    show("variable_global_get(training_mode)", roa::call("variable_global_get", { "training_mode" }));
    show("variable_global_exists(char_num)", roa::call("variable_global_exists", { "char_num" }));
    diag_array("charNames"); diag_array("player_ID"); diag_array("num_palettes"); diag_array("player_hover_character");
    show("object_get_name(id.object_index)", roa::call("object_get_name", { roa::call_real("variable_instance_get", { id, "object_index" }) }));
}

static void start_job(const char* tag) {
    diag();
    if (g_job.active) return;
    g_job.active = true; g_job.tag = tag; g_job.idx = 0; g_job.phase = 0; g_job.inst = 0; g_job.arrays.clear();
    g_job.out.str(""); g_job.out.clear();
    g_job.out << "== " << tag << " frame=" << g_frame << "\n-- globals\n";
    roa::log(std::string("dump started: ") + tag);
}

static FILE* trace_file() {
    static FILE* f = nullptr;
    if (!f) { fopen_s(&f, "mods/pizzarivals_trace.txt", "wb"); if (f) setvbuf(f, nullptr, _IONBF, 0); }
    return f;
}
static void trace(const char* what, const char* name) { if (FILE* f = trace_file()) fprintf(f, "%s %s\n", what, name); }

static void step_job() {
    const size_t SLICE = 1500;
    Job& j = g_job;
    if (j.phase == 0) {                                   // globals
        for (size_t n = 0; n < SLICE && j.idx < g_candidates.size(); ++n, ++j.idx) {
            const char* nm = g_candidates[j.idx].c_str();
            trace("G", nm);
            if (roa::global_exists(nm)) {
                RValue v = roa::global_get(nm);
                if (v.type == GML_TYPE_ARRAY) j.arrays.push_back(nm);
                j.out << nm << " = " << value_str(v) << "\n";
            }
        }
        if (j.idx >= g_candidates.size()) {
            j.phase = 1; j.idx = 0;
            j.oplayer = roa::call_real("asset_get_index", { "oPlayer" });
            j.count = (int)roa::call_real("instance_number", { j.oplayer });
            j.out << "-- oPlayer instances: " << j.count << "\n";
        }
    } else if (j.phase == 1) {                            // instance variables
        if (j.inst >= j.count) { j.phase = 2; j.idx = 0; j.out << "-- global array elements (first 6)\n"; return; }
        if (j.idx == 0) { j.inst_id = roa::call_real("instance_find", { j.oplayer, j.inst }); j.out << "[instance " << j.inst_id << "]\n"; }
        for (size_t n = 0; n < SLICE && j.idx < g_candidates.size(); ++n, ++j.idx) {
            const char* nm = g_candidates[j.idx].c_str();
            trace("I", nm);
            if (roa::call_real("variable_instance_exists", { j.inst_id, nm }) > 0.5)
                j.out << "  " << nm << " = " << value_str(roa::inst_get(j.inst_id, nm)) << "\n";
        }
        if (j.idx >= g_candidates.size()) { j.idx = 0; ++j.inst; }
    } else if (j.phase == 2) {                            // global array contents
        for (size_t n = 0; n < 150 && j.idx < j.arrays.size(); ++n, ++j.idx) {
            const char* nm = j.arrays[j.idx].c_str();
            trace("A", nm);
            RValue v = roa::global_get(nm);
            array_lines(j.out, nm, v, 6, 12);
        }
        if (j.idx >= j.arrays.size()) {
            std::ofstream("mods/pizzarivals_dump_" + j.tag + ".txt") << j.out.str();
            roa::log("dump written: " + j.tag);
            j.active = false;
        }
    }
}


// ---- F10: run mods/pizzarivals_cmd.txt, one command per line (experiment driver, no rebuild needed) ----
//   g NAME VALUE            set a global to a real number
//   gs NAME TEXT            set a global to a string
//   a NAME IDX VALUE        set element IDX of a 1-D global array (row 0) to a real number
//   get NAME [IDX]          log a global (or array element)
//   goto ROOMNAME           room_goto(asset_get_index(ROOMNAME))
//   call FUNC ARGS...       call a built-in/script by name with numeric args, log the result
static bool poke(void* dst, const void* src, size_t n) { __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool array_cell(const RValue& v, int idx, unsigned char** cell) {
    if (v.type != GML_TYPE_ARRAY || !v.valueArray) return false;
    uint32_t top[2]; if (!peek(v.valueArray, top, sizeof top)) return false;
    uint32_t row[2]; if (!peek((void*)(uintptr_t)top[1], row, sizeof row)) return false;
    if (idx < 0 || (uint32_t)idx >= row[0]) return false;
    *cell = (unsigned char*)(uintptr_t)(row[1] + idx * 16);
    return true;
}
static void run_cmd_file(const char* path = "mods/pizzarivals_cmd.txt") {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line); std::string op; is >> op;
        if (op.empty() || op[0] == '#') continue;
        roa::g_faulted = false;
        if (op == "g") { std::string n; double v; is >> n >> v; roa::call("variable_global_set", { n, v }); roa::log("set " + n + " = " + std::to_string(v)); }
        else if (op == "gs") { std::string n, t; is >> n; std::getline(is, t); if (!t.empty() && t[0] == ' ') t.erase(0, 1); roa::call("variable_global_set", { n, t }); roa::log("set " + n + " = \"" + t + "\""); }
        else if (op == "a") {
            std::string n; int i; double v; is >> n >> i >> v;
            RValue arr = roa::global_get(n.c_str()); unsigned char* c = nullptr;
            if (array_cell(arr, i, &c)) { RValue nv; nv.setReal(v); poke(c, &nv, 16); roa::log("set " + n + "[" + std::to_string(i) + "] = " + std::to_string(v)); }
            else roa::log("array cell not found: " + n + "[" + std::to_string(i) + "]");
        }
        else if (op == "capture") { std::string m; is >> m; if (m == "start") { g_cap.clear(); g_capturing = true; roa::log("capture started"); } else { g_capturing = false; roa::log("capture stopped"); } }
        else if (op == "dumpcalls") {
            for (auto& kv : g_cap) roa::log(std::string("  call ") + kProbeNames[kv.first.fn] + "(" + std::to_string(kv.first.a0) + "," + std::to_string(kv.first.a1) + ") x" + std::to_string(kv.second.count) + " true=" + std::to_string(kv.second.trues) + " last=" + std::to_string(kv.second.last));
        }
        else if (op == "getarr") {
            std::string n; is >> n; RValue v = roa::global_get(n.c_str()); std::ostringstream o; array_lines(o, n.c_str(), v, 2, 14); std::string s = o.str();
            std::istringstream ls(s); std::string l; std::string row; while (std::getline(ls, l)) { auto e = l.find(" = "); row += (e == std::string::npos ? l : l.substr(e + 3)) + " | "; } roa::log(n + ": " + row);
        }
        else if (op == "pcsnap") {
            std::string tag; std::getline(is, tag);
            roa::log("== pcsnap" + tag);
            for (auto& n : g_candidates) {
                if (n.rfind("pc_", 0) != 0) continue;
                RValue v = roa::global_get(n.c_str());
                if (v.type != GML_TYPE_ARRAY) continue;
                std::ostringstream o; array_lines(o, n.c_str(), v, 1, 5);
                std::istringstream ls(o.str()); std::string l, row;
                while (std::getline(ls, l)) { auto e = l.find(" = "); if (e != std::string::npos && l.find("length") == std::string::npos) row += l.substr(e + 3) + " "; }
                roa::log("  " + n + ": " + row);
            }
        }
        else if (op == "key") { int vk, st; is >> vk >> st; vkey_set(vk, st != 0); roa::log("key " + std::to_string(vk) + (st ? " down" : " up")); }
        else if (op == "keyclear") { for (int k = 0; k < 256; ++k) vkey_set(k, false); roa::log("keys cleared"); }
        else if (op == "get") {
            std::string n; int i = -1; is >> n; if (!(is >> i)) i = -1;
            RValue v = roa::global_get(n.c_str());
            if (i >= 0) { unsigned char* c; roa::log(n + "[" + std::to_string(i) + "] = " + (array_cell(v, i, &c) ? cell_str(c) : "?")); }
            else roa::log(n + " = " + value_str(v));
        }
        else if (op == "goto") { std::string n; is >> n; double r = roa::call_real("asset_get_index", { n }); roa::log("room_goto " + n + " (" + std::to_string(r) + ")"); roa::call("room_goto", { r }); }
        else if (op == "call") {
            std::string fn; is >> fn; std::vector<roa::Arg> args; double d;
            while (is >> d) args.push_back(d);
            RValue r = roa::call(fn.c_str(), std::move(args)); roa::log("call " + fn + " -> " + value_str(r));
        }
        else roa::log("unknown command: " + line);
        if (roa::g_faulted) roa::log("  (fault while running: " + line + ")");
    }
}


// One-shot: write every candidate variable of the first instance of an object (used to learn pHitBox's fields).
static void dump_first_instance(const char* objname, const char* file) {
    double obj = roa::call_real("asset_get_index", { objname });
    if (roa::call_real("instance_number", { obj }) < 1) return;
    double id = roa::call_real("instance_find", { obj, 0 });
    std::ostringstream o; o << "[" << objname << " instance " << id << "]\n";
    for (auto& n : g_candidates) {
        roa::g_faulted = false;
        if (roa::call_real("variable_instance_exists", { id, n.c_str() }) > 0.5) {
            RValue v = roa::inst_get(id, n.c_str());
            o << "  " << n << " = " << (roa::g_faulted ? std::string("<FAULT>") : value_str(v)) << "\n";
        }
    }
    std::ofstream(file) << o.str();
    roa::log(std::string("instance dump written: ") + file);
}

// Runs once per frame from inside the game's own draw script.
static void probe_tick() {
    static int last_players = -1, settle = 0;
    static bool f9 = false;
    if (g_job.active) { step_job(); return; }
    if (g_frame % 30 == 0) {
        double op = roa::call_real("asset_get_index", { "oPlayer" });
        int n = (int)roa::call_real("instance_number", { op });
        if (n != last_players) { roa::log("oPlayer count " + std::to_string(n) + " frame " + std::to_string(g_frame)); last_players = n; settle = n > 0 ? 120 : 0; }
    }
    if (settle > 0 && --settle == 0) start_job("training");
    static bool hb_done = false;
    if (!hb_done && last_players > 0) {
        double hb = roa::call_real("asset_get_index", { "pHitBox" });
        if (roa::call_real("instance_number", { hb }) > 0) { hb_done = true; dump_first_instance("pHitBox", "mods/pizzarivals_hitbox.txt"); }
    }
    bool now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (now && !f9) { static int mn = 0; start_job(("manual" + std::to_string(++mn)).c_str()); }
    f9 = now;
    static bool f10 = false;
    bool n10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (n10 && !f10) run_cmd_file();
    f10 = n10;
    static bool f11 = false;
    bool n11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (n11 && !f11) run_cmd_file("mods/pizzarivals_keys.txt");
    f11 = n11;
}

static void* g_origs[kN];
static volatile LONG g_counts[kN];
static int g_chosen = -1;                        // index whose detour also runs probe_tick



// ---- virtual keyboard: keys we claim are down, merged into the game's own keyboard reads ----
static volatile LONG g_vkeys[256];          // 1 = held
static volatile LONG g_vpress[256];         // frame until which "pressed" reads true
static void vkey_set(int vk, bool down) {
    if (vk < 0 || vk > 255) return;
    if (down && !g_vkeys[vk]) g_vpress[vk] = g_frame + 2;
    g_vkeys[vk] = down ? 1 : 0;
}
static void post_keyboard(const char* fn, RValue& out, uint32_t argc, RValue* args) {
    if (argc < 1) return;
    int vk = (int)roa::real_of(args[0]);
    if (vk < 0 || vk > 255) return;
    bool v = false;
    if (!strcmp(fn, "keyboard_check") || !strcmp(fn, "keyboard_check_direct")) v = g_vkeys[vk] != 0;
    else if (!strcmp(fn, "keyboard_check_pressed")) v = g_vpress[vk] > g_frame && g_vkeys[vk];
    else if (!strcmp(fn, "keyboard_check_released")) return;
    if (v) out.setReal(1);
}

static void detour_common(int i) {
    InterlockedIncrement(&g_counts[i]);
    if (i == g_chosen && InterlockedExchange(&g_tick_pending, 0)) probe_tick();
}
template <int I> static void detour(RValue& out, CInstance* s, CInstance* o, int argc, RValue* args) {
    detour_common(I);
    ((roa::FnA)g_origs[I])(out, s, o, argc, args);
    if (I < kN && kProbeNames[I][0] == 'k') post_keyboard(kProbeNames[I], out, (uint32_t)argc, args);
    if (g_capturing && (I == kIdxGpButton || I == kIdxGpAxis || I == kIdxKbPressed)) capture_call(I, out, (uint32_t)argc, args);
}
template <int... Is> static void install_all(std::integer_sequence<int, Is...>) {
    void* dets[] = { (void*)&detour<Is>... };
    for (int i = 0; i < kN; ++i) {
        void* t = roa::builtin(kProbeNames[i]);
        if (!t) { roa::log(std::string("no builtin: ") + kProbeNames[i]); continue; }
        int r = roa::hook(t, dets[i], &g_origs[i]);
        if (r != 0) roa::log(std::string("hook failed (") + std::to_string(r) + "): " + kProbeNames[i]);
    }
}

static void on_present(ID3D11RenderTargetView*, IDXGISwapChain*, ID3D11Device*, ID3D11DeviceContext*) {
    ++g_frame;                       // no GML here: only flag that a new frame happened
    InterlockedExchange(&g_tick_pending, 1);
    if (g_frame == 600 || g_frame == 3000) {
        std::ostringstream o; o << "call counts @frame " << g_frame << ":";
        for (int i = 0; i < kN; ++i) if (g_counts[i]) o << " " << kProbeNames[i] << "=" << g_counts[i];
        roa::log(o.str());
        if (g_chosen < 0) {   // first of the frequently called ones, preferring cheap pure functions
            const char* pref[] = { "draw_sprite_ext", "draw_sprite", "draw_set_alpha", "draw_set_color", "draw_text", "irandom", "abs", "floor" };
            for (auto p : pref) for (int i = 0; i < kN; ++i) if (!strcmp(kProbeNames[i], p) && g_counts[i] > 600) { g_chosen = i; break; }
            if (g_chosen >= 0) roa::log(std::string("tick builtin: ") + kProbeNames[g_chosen]);
        }
    }
}

// Logs the first access violations with module+RVA and a short call stack, so a crash identifies itself.
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_IN_PAGE_ERROR) return EXCEPTION_CONTINUE_SEARCH;
    static LONG n = 0;
    if (InterlockedIncrement(&n) > 40) return EXCEPTION_CONTINUE_SEARCH;
    auto where = [](void* a) {
        HMODULE m = nullptr; char path[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m);
        if (m) GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        char buf[300]; sprintf_s(buf, "%s+0x%X", base ? base + 1 : path, (unsigned)((char*)a - (char*)m));
        return std::string(buf);
    };
    std::ostringstream o;
    o << "EXCEPTION 0x" << std::hex << code << " at " << where(ep->ExceptionRecord->ExceptionAddress) << " tid=" << std::dec << GetCurrentThreadId()
      << " guarded=" << roa::g_faulted << " frame=" << g_frame;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        o << " (" << (ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read") << " 0x" << std::hex << ep->ExceptionRecord->ExceptionInformation[1] << ")";
    void* frames[16]; USHORT cnt = CaptureStackBackTrace(0, 16, frames, nullptr);
    o << "\n   stack:";
    for (USHORT i = 0; i < cnt; ++i) o << " " << where(frames[i]);
    roa::log(o.str());
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI boot(LPVOID) {
    AddVectoredExceptionHandler(1, crash_logger);
    for (int i = 0; i < 600 && !roa::init(); ++i) Sleep(100);
    if (!roa::init()) { roa::log("probe: init failed (sig=" + std::to_string(roa::g_sig) + ")"); return 0; }
    roa::log("probe: loader ready, builtin signature = " + std::string(roa::g_sig == 0 ? "A (stock)" : "B (loader README)"));
    load_candidates();
    install_all(std::make_integer_sequence<int, kN>{});
    roa::add_present_callback(&on_present);
    roa::log("probe: hooks installed");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, boot, nullptr, 0, nullptr));
    }
    return TRUE;
}
