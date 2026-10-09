// PizzaRivals RoA bridge (x86). Loaded by roa-mod-loader as mods/pizzarivals.dll.
//
// Runs a hidden 1-player Training match for the character Pizza Tower asks for, feeds it Pizza
// Tower's input, mirrors Pizza Tower's solids into it, and reports the player's state, hitboxes
// and rendered pixels back over shared memory (protocol/pizzarivals_protocol.h).
//
// Threading rule (learned the hard way): GML is only called from inside the game's own script
// context -- the `floor` builtin detour for the per-frame tick and the `controls_update` script
// detour for input -- never from the D3D Present callback, which can fire in the middle of a
// Create event.
#include "../../pt-bridge/extension/audio_loopback.h"
#include "gml_util.h"
#include "../../protocol/pr_shm.h"
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <map>
#include <unordered_map>
#include <cmath>
#include <algorithm>

using namespace gml;

// ------------------------------------------------------------------------------------------
// constants discovered with the probe (see docs/DESIGN.md "findings")
// ------------------------------------------------------------------------------------------
static const int    kRoomMenu = 2;                 // mainMenu_room
static int          g_match_room = 34;             // room index of the match room (default stage_training_clean)
static std::string  g_match_room_name = "stage_custom_clean"; // blank-ish room; training stage art cannot be hidden
static const DWORD  kControlsUpdateRva = 0x8FDB30; // gml_Script_controls_update (Ghidra 0x00CFDB30 - image base 0x400000)
static const DWORD  kControlsIntakeRva = 0x68F710; // gml_Script_controls_intake (Ghidra 0x00A8F710)
static const DWORD  kPlayerControlsUpdateRva = 0x9391B0; // gml_Script_player_controls_update (Ghidra 0x00D391B0)
static const DWORD  kCameraStep2Rva = 0x3C90360;   // gml_Object_camera_obj_Step_2 (Ghidra 0x04090360)
static const DWORD  kGetStageDataRva = 0x1EA5A10;  // gml_Script_get_stage_data (Ghidra 0x022A5A10)
static const DWORD  kDeathUpdateRva = 0x7454B0;     // gml_Script_death_update (Ghidra 0x00B454B0)
static const DWORD  kPlayerKoRva = 0x32568C0;      // gml_Object_oPlayer_Other_14 (Ghidra 0x036568C0): the KO/death event
static const int    kSlot = 1;                     // RoA arrays are 1-based; P1 = 1
static const double kKeyColor = 0xFF00FF;          // BGR magenta, used as the chroma key background

static PRShared* g_s = nullptr;
static int  g_frame = 0;
static volatile LONG g_tick_pending = 0;

// ------------------------------------------------------------------------------------------
// state
// ------------------------------------------------------------------------------------------
static std::unordered_map<std::string, int> g_stock_index;   // "stock:zetterburn" -> charNames index
static std::string g_req_id;                                 // request waiting for the character list
static std::string g_char_id;                                // what PT asked for
static int  g_pending_char = -1;                             // charNames index waiting to be booted
static bool g_booting = false, g_ready = false;
static int  g_boot_frame = 0;
static double g_player = -4;                                 // oPlayer instance id of player 1
static std::vector<PRSolid> g_pool_geom;                     // geometry of g_pool_solid[i] as mirrored from Pizza Tower
static std::vector<size_t> g_clipped;                        // indices of pooled solids whose corner is cut away while he runs down a ramp
static std::vector<double> g_pool_solid, g_pool_platform_ids;        // our mirrored block instances
static uint32_t g_solid_rev_seen = 0;
static PRSolidSet g_solids_cache{};                          // last PT solid set, in PT coordinates
// PT world = RoA world + offset. RoA's room is small, so we keep the player near its centre and
// slide everything else instead (recentering), tracking the accumulated shift here.
static const double kRoomW = 32000;      // the room's width (half of it must stay under 16384: some character code creates a surface that wide)
static double g_ox = 0, g_oy = 0;
static bool g_warp_pending = false;
static bool g_solids_dirty = false;
static uint32_t g_world = 0;                              // the warp serial the solids / targets from Pizza Tower must carry (anything else is from another room)
static bool g_solids_ready = true;                      // false from a warp until the first solid set of the new world has arrived
static int g_warp_frame = 0;
static volatile uint32_t g_epoch = 0;                   // bumped whenever the player is moved on purpose (recenter, scene shift, boot, warp): a position jump after that is not a glitch
static volatile uint32_t g_warp_gen = 1;               // bumped whenever g_ox/g_oy are re-derived
static bool g_recenter_on = false;
static double g_patch_block = -1;           // a wide floor patch under the player from a warp until the new room's solids arrive (he must not become airborne: that ends a run)
static double g_slope_block = -1;           // pooled solid that follows the player along PT slopes
static const double kCenterX = 640, kCenterY = 480;
static double g_obj_oplayer = -1, g_obj_phitbox = -1, g_obj_solid = -1, g_obj_plat = -1, g_obj_parblock = -1, g_obj_parjump = -1;

// values the Present callback needs (written by tick on the same thread)
static double g_view_x = 0, g_view_y = 0, g_view_w = 960, g_view_h = 540;
static volatile int g_jump_guard = 0;               // frames left in which a jump press is swallowed right after takeoff
static volatile bool g_pad_active = false;          // a gamepad was used in the last 1.5 s
static volatile UINT g_bb_w = 0, g_bb_h = 0;      // last captured backbuffer size
static volatile double g_present_fps = 60, g_tick_fps = 60;
static double g_cap_cx = 0, g_cap_cy = 0, g_cap_xf = 0, g_cap_yf = 0;                    // world point to centre the captured frame on
static volatile bool g_cap_valid = false;
static uint32_t g_frame_out = 0;

static void send_event(uint16_t type, const std::string& s = "") {
    if (g_s) pr::ring_push(g_s->to_pt, type, s.c_str(), (uint16_t)(s.size() + 1));
}

// ------------------------------------------------------------------------------------------
// input injection: runs right after the game's own controls_update, before the player steps
// ------------------------------------------------------------------------------------------
typedef RValue* (__cdecl* ScriptFn)(CInstance*, CInstance*, RValue*, int, RValue**);
// Is a gamepad being used? Checked every frame (a late answer sends the first press through Pizza Tower's keys: Y became a taunt for a moment).
static void update_pad_active() {
    static int last_active = -1000, done = -1;
    if (done == g_frame) return;
    done = g_frame;
    bool act = false;
    for (int dev = 0; dev < 4 && !act; ++dev) {
        if (roa::call_real("gamepad_is_connected", { (double)dev }) < 0.5) continue;
        for (int btn = 32769; btn <= 32784 && !act; ++btn) if (roa::call_real("gamepad_button_check", { (double)dev, (double)btn }) > 0.5) act = true;
        for (int ax = 32785; ax <= 32788 && !act; ++ax) if (std::fabs(roa::call_real("gamepad_axis_value", { (double)dev, (double)ax })) > 0.5) act = true;
    }
    if (act) last_active = g_frame;
    g_pad_active = (g_frame - last_active < 90);
}
static void apply_input() {
    update_pad_active();
    if (!g_s || !g_ready) return;
    PRInput in; pr::slot_read(g_s->h_input, g_s->input, in);
    PRPtState pt; pr::slot_read(g_s->h_pt, g_s->pt, pt);
    bool paused_now = (pt.flags & PR_PT_PAUSED) != 0;
    if (!paused_now && ((pt.flags & PR_PT_NATIVE_INPUT) || g_pad_active)) return;      // the game's own controller handling stays in charge
    uint16_t b = (pt.flags & (PR_PT_PAUSED)) ? 0 : in.buttons;
    static int dbg = 0;
    if (in.buttons && ++dbg % 20 == 1) { char m[160]; sprintf_s(m, "input: buttons=%x ptflags=%x frame=%u -> applied=%x", in.buttons, pt.flags, in.frame_id, b); roa::log(m); }
    static int hl = 0, hr = 0, hu = 0, hd = 0;
    bool L = b & PR_BTN_LEFT, R = b & PR_BTN_RIGHT, U = b & PR_BTN_UP, D = b & PR_BTN_DOWN;
    hl = L ? hl + 1 : 0; hr = R ? hr + 1 : 0; hu = U ? hu + 1 : 0; hd = D ? hd + 1 : 0;
    auto S = [](const char* n, double v) { garr_set(n, kSlot, v); };
    S("pc_left_down", L);   S("pc_left_down_timer", L ? hl : 9999);   S("pc_left_pressed", L && hl == 1);   S("pc_left_stick_down", L);
    S("pc_right_down", R);  S("pc_right_down_timer", R ? hr : 9999);  S("pc_right_pressed", R && hr == 1);  S("pc_right_stick_down", R);
    S("pc_up_down", U);     S("pc_up_down_timer", U ? hu : 9999);     S("pc_up_pressed", U && hu == 1);     S("pc_up_stick_down", U);
    S("pc_down_down", D);   S("pc_down_down_timer", D ? hd : 9999);   S("pc_down_pressed", D && hd == 1);   S("pc_down_stick_down", D);
    double sx = R ? 1 : L ? -1 : 0, sy = D ? 1 : U ? -1 : 0;
    if (in.stick_x || in.stick_y) { sx = in.stick_x / 100.0; sy = -in.stick_y / 100.0; }
    S("pc_left_stick_xpos", sx); S("pc_left_stick_ypos", sy);
    S("pc_menu_left_stick_xpos", sx); S("pc_menu_left_stick_ypos", sy);
    bool any = L || R || U || D;
    S("pc_joy_pad_idle", any ? 0 : 1);
    S("pc_joy_dir", any ? std::fmod(std::atan2(-sy, sx) * 57.29578 + 360.0, 360.0) : 0);
    // Buttons exist at two levels in RoA's input layer: the per-action pc_*_down and the raw Xbox-layout
    // pc_menu_*_down / pc_menu_*_pressed (jump=Y, attack=B, special=A, shield=RB, taunt=LB, strong=X).
    // Set both so whichever one the player code consumes sees the press, with a one-frame edge.
    struct Map { uint16_t mask; const char* act; const char* menu_down; const char* menu_pressed; bool prev; };
    static Map maps[] = {
        { PR_BTN_JUMP,    "pc_jump_down",    "pc_menu_y_down",  "pc_menu_y_pressed",  false },
        { PR_BTN_ATTACK,  "pc_attack_down",  "pc_menu_b_down",  "pc_menu_b_pressed",  false },
        { PR_BTN_SPECIAL, "pc_special_down", "pc_menu_a_down",  "pc_menu_a_pressed",  false },
        { PR_BTN_SHIELD,  "pc_shield_down",  "pc_menu_rb_down", "pc_menu_rb_pressed", false },
        { PR_BTN_TAUNT,   "pc_taunt_down",   "pc_menu_lb_down", "pc_menu_lb_pressed", false },
        { PR_BTN_STRONG,  "pc_strong_down",  "pc_menu_x_down",  "pc_menu_x_pressed",  false },
    };
    for (auto& m : maps) {
        bool now = (b & m.mask) != 0;
        S(m.act, now); S(m.menu_down, now); S(m.menu_pressed, now && !m.prev);
        m.prev = now;
    }
    S("pc_any_pressed", b != 0);

}

static ScriptFn g_controls_update_orig = nullptr;

// The player object caches its own copy of each button: *_down, a one-frame *_pressed edge and a
// *_counter (frames since the press, capped at 10). player_controls_update fills them from the
// pc_* globals; we overwrite them right after it so Pizza Tower's presses are what the state machine sees.
static void apply_player_vars() {
    update_pad_active();
    if (!g_s || !g_ready || g_player < 0) return;
    PRInput in; pr::slot_read(g_s->h_input, g_s->input, in);
    PRPtState pt; pr::slot_read(g_s->h_pt, g_s->pt, pt);
    if ((pt.flags & PR_PT_NATIVE_INPUT) || g_pad_active) return;
    uint16_t b = (pt.flags & PR_PT_PAUSED) ? 0 : in.buttons;
    struct Btn { uint16_t mask; const char* down; const char* pressed; const char* counter; bool prev; int cnt; };
    static Btn btns[] = {
        { PR_BTN_JUMP,    "jump_down",    "jump_pressed",    "jump_counter",    false, 10 },
        { PR_BTN_ATTACK,  "attack_down",  "attack_pressed",  "attack_counter",  false, 10 },
        { PR_BTN_SPECIAL, "special_down", "special_pressed", "special_counter", false, 10 },
        { PR_BTN_SHIELD,  "shield_down",  "shield_pressed",  "shield_counter",  false, 10 },
        { PR_BTN_TAUNT,   "taunt_down",   "taunt_pressed",   nullptr,           false, 10 },
        { PR_BTN_STRONG,  "strong_down",  "strong_pressed",  nullptr,           false, 10 },
    };
    for (auto& k : btns) {
        bool now = (b & k.mask) != 0;
        bool edge = now && !k.prev;
        k.cnt = edge ? 0 : (k.cnt < 10 ? k.cnt + 1 : 10);
        iset(g_player, k.down, now);
        iset(g_player, k.pressed, edge);
        if (k.counter) iset(g_player, k.counter, k.cnt);
        k.prev = now;
    }
    iset(g_player, "any_pressed", b != 0);
}
struct Pin { double x = 0, y = 0; bool on = false; };
static std::map<double, Pin> g_pins;                  // stand-in CPUs held exactly on their enemy (applied inside the game's own step: RoA ignores x written from outside it)
static volatile LONG g_shift_pending = 0; static double g_shift_dx = 0;   // scene shift: moves the player far to the right inside RoA (done inside the game's own step: x written from outside is undone)
static void apply_room_size();
static ScriptFn g_pcu_orig = nullptr;
static RValue* __cdecl player_controls_update_detour(CInstance* self, CInstance* other, RValue* res, int argc, RValue** args) {
    RValue* r = g_pcu_orig(self, other, res, argc, args);
    // this runs once per fighter (P1 and the stand-in CPUs): only P1's own call gets our button state
    bool is_p1 = self && g_player >= 0 && ((CInstance*)self)->id == (int)g_player;
    if (!is_p1 && self && !g_pins.empty()) {
        auto it = g_pins.find((double)((CInstance*)self)->id);
        if (it != g_pins.end() && it->second.on) {
            double id = it->first; const Pin& pn = it->second;
            iset(id, "x", pn.x); iset(id, "y", pn.y); iset(id, "xprevious", pn.x); iset(id, "yprevious", pn.y); iset(id, "hsp", 0); iset(id, "vsp", 0);
            int sl = (int)iget(id, "player", 2); garr_set("g_player_x", sl, pn.x); garr_set("g_player_y", sl, pn.y);
        }
    }
    if (is_p1 && g_shift_pending) {
        double nx = iget(g_player, "x") + g_shift_dx;
        iset(g_player, "x", nx); iset(g_player, "xprevious", nx); garr_set("g_player_x", kSlot, nx);
        g_ox -= g_shift_dx; g_solids_dirty = true; ++g_warp_gen; ++g_epoch; g_shift_pending = 0;
        roa::log("scene shifted by " + std::to_string(g_shift_dx) + ": player x " + std::to_string(nx) + " offset x " + std::to_string(g_ox));
    }
    if (is_p1 && !g_solids_ready && g_frame - g_warp_frame < 45 && g_player >= 0) { iset(g_player, "vsp", 0); if (g_patch_block >= 0) iset(g_patch_block, "x", std::floor(iget(g_player, "x") - 400)); }   // nothing to stand on yet: the patch follows him, he does not fall (a runner keeps running)
    if (is_p1) apply_player_vars();
    if (is_p1) apply_room_size();      // fresh just before the attack scripts run (Kragg's pillar reads it)
    if (is_p1 && g_jump_guard > 0 && g_player >= 0) {   // see tick(): RoA re-derives a fresh jump press on the pad for several frames after takeoff
        --g_jump_guard;
        iset(g_player, "jump_pressed", 0); iset(g_player, "jump_counter", 10);
    }
    return r;
}

// ---- camera: follow the player wherever PT puts him ----
typedef void (__cdecl* EventFn)(CInstance*, CInstance*);
static EventFn g_cam_orig = nullptr, g_ko_orig = nullptr;
static void center_view() {
    if (!g_ready || g_player < 0) return;
    double px = iget(g_player, "x"), py = iget(g_player, "y");
    double w = gget("g_wview", 960), h = gget("g_hview", 540);
    double vx = px - w / 2, vy = py - h / 2 - 40;
    gset("g_xview", vx); gset("g_yview", vy);
    gset("view_xview", vx); gset("view_yview", vy);
    // this runtime positions the real view through a camera object, not the old view_* variables
    static double cam = -1; if (cam < 0) { cam = gget("view_camera", -1); roa::log("view camera id " + std::to_string(cam) + ", camera_set_view_pos " + (roa::builtin("camera_set_view_pos") ? "available" : "MISSING")); }
    if (cam >= 0) roa::call("camera_set_view_pos", { cam, vx, vy });
}
static void __cdecl camera_step2_detour(CInstance* s, CInstance* o) { g_cam_orig(s, o); center_view(); }
// ---- no blast zones: swallow the KO event while a bridged match is running ----
static void __cdecl ko_detour(CInstance* s, CInstance* o) { if (g_ready) return; g_ko_orig(s, o); }
// ---- blast zones: get_stage_data(SD 7..10) = left/right/top/bottom blast zone. Report them absurdly far away so RoA
// never destroys projectiles, cancels moves or "rescues" the character; Pizza Tower decides what harms him. ----
static double g_bottom_all = 0;      // bottom blast zone as seen by everyone but the player: below the lowest fighter
static ScriptFn g_stage_data_orig = nullptr;
static double g_bz[4] = { -100, 1380, -100, 904 };   // the stage's real blast zone (left, right, top, bottom), RoA space
static RValue* __cdecl stage_data_detour(CInstance* s, CInstance* o, RValue* res, int argc, RValue** args) {
    RValue* r = g_stage_data_orig(s, o, res, argc, args);
    if (r && argc >= 1 && args && args[0]) {
        int idx = (int)gml::cell_real((const unsigned char*)args[0]);
        if (idx >= 7 && idx <= 10) {
            { double real = gml::cell_real((const unsigned char*)r); if (real > -1e5 && real < 1e5) g_bz[idx - 7] = real; }
            // left, right and top blast zones are far away; the bottom follows the room height (see apply_room_size)
            if (idx != 10) { *(double*)r = (idx == 7 || idx == 9) ? -200000.0 : 200000.0; *(int*)((char*)r + 12) = GML_TYPE_REAL; }
            else if (g_bottom_all > 0 && s && g_player >= 0 && ((CInstance*)s)->id != (int)g_player) { *(double*)r = g_bottom_all; *(int*)((char*)r + 12) = GML_TYPE_REAL; }
        }
    }
    return r;
}
static ScriptFn g_death_update_orig = nullptr;
static RValue* __cdecl death_update_detour(CInstance* s, CInstance* o, RValue* res, int argc, RValue** args) {
    if (g_ready) return res;           // bridged match: nobody dies, PT decides what harms the player
    return g_death_update_orig(s, o, res, argc, args);
}
static ScriptFn g_controls_intake_orig = nullptr;
static RValue* __cdecl controls_intake_detour(CInstance* self, CInstance* other, RValue* res, int argc, RValue** args) {
    RValue* r = g_controls_intake_orig(self, other, res, argc, args);
    apply_input();
    return r;
}
static RValue* __cdecl controls_update_detour(CInstance* self, CInstance* other, RValue* res, int argc, RValue** args) {
    RValue* r = g_controls_update_orig(self, other, res, argc, args);
    apply_input();
    return r;
}

// ------------------------------------------------------------------------------------------
// match boot / character switch
// ------------------------------------------------------------------------------------------
static int g_proxy_index = -1, g_proxy_slot = 2;
static void build_stock_table() {
    RValue names = roa::global_get("charNames");
    for (int i = 2; i < 40; ++i) {
        unsigned char* c = array_cell(names, 0, i);
        unsigned char buf[16];
        if (!c || !peek(c, buf, 16)) break;
        std::string s = cell_str(buf);
        if (s.size() < 3 || s[0] != '"') break;
        s = s.substr(1, s.size() - 2);
        std::string key = "stock:";
        for (char ch : s) key += (ch == ' ') ? '_' : (char)tolower(ch);
        g_stock_index[key] = i;
        roa::log("stock " + std::to_string(i) + " = " + s);
    }
    roa::log("stock characters known: " + std::to_string(g_stock_index.size()));
    for (int i = 2; i < 400; ++i) {
        unsigned char* c = array_cell(names, 0, i);
        unsigned char buf[16];
        if (!c || !peek(c, buf, 16)) continue;
        std::string s = cell_str(buf);
        if (s.size() > 2 && s[0] == '"' && s.substr(1, s.size() - 2) == "PT Enemy Proxy") { g_proxy_index = i; roa::log("enemy proxy character found at index " + std::to_string(i)); break; }
    }
    { std::string all; int cnt = 0; for (int i = 20; i < 400; ++i) { unsigned char* c = array_cell(names, 0, i); unsigned char buf[16]; if (!c || !peek(c, buf, 16)) continue; std::string s = cell_str(buf); if (s.size() > 2 && cnt++ < 12) all += std::to_string(i) + "=" + s + " "; } roa::log("charNames beyond stock: " + all); }
    if (g_proxy_index < 0) roa::log("enemy proxy character NOT found (install it in %LOCALAPPDATA%/RivalsofAether/workshop/PTEnemy)");
}

static bool g_window_was_shown = false;
static int g_watchp = 0, g_watchp_idx = 1;
static bool g_cpu_targets = true;                      // map Pizza Tower enemies onto CPU fighters (falls back to piñatas for extras)
static std::vector<double> g_dummies;                  // oPlayer instances of the stand-in CPUs
static std::vector<uint32_t> g_dummy_enemy;             // Pizza Tower enemy id each one currently stands in for
static bool g_css_mode = false;                      // RoA's own window is open so the player can pick a character / skin / workshop character
static HWND g_css_prev_fg = nullptr;
static double g_enemy_pct = 0;                      // "enemy percent" option: the stand-in CPUs are kept at this damage
static double g_speed_mult = 1;                      // "2x speed" option: horizontal movement stats are multiplied
static std::map<std::string, double> g_speed_orig;
static std::string g_css_name;
static void show_roa_window() {
    HWND w = (HWND)roa::get_window(); if (!w) return;
    g_css_prev_fg = GetForegroundWindow();
    LONG ex = GetWindowLongA(w, GWL_EXSTYLE);
    SetWindowLongA(w, GWL_EXSTYLE, (ex | WS_EX_APPWINDOW) & ~(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
    if (roa::builtin("window_set_size")) roa::call("window_set_size", { 1280, 720 });
    SetWindowPos(w, HWND_TOPMOST, 60, 40, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(w);
    g_window_was_shown = true;
    roa::log("RoA window shown for character selection");
}
static std::map<std::string, double> g_speed_set;      // what we wrote last, to notice a character that rewrites its stats every frame
static bool g_speed_displace = false;                  // that kind of character (Wrastor) gets the extra speed as extra movement instead
static int g_speed_votes = 0;
static void apply_speed() {
    if (g_player < 0) return;
    static const char* names[] = { "walk_speed", "walk_accel", "initial_dash_speed", "dash_speed", "dash_turn_accel", "max_jump_hsp", "leave_ground_max", "air_max_speed", "air_accel", "roll_forward_max", "roll_backward_max", "ground_friction" };
    if (g_speed_displace) {
        for (auto& kv : g_speed_set) if (g_speed_orig.count(kv.first)) iset(g_player, kv.first.c_str(), g_speed_orig[kv.first]);
        g_speed_set.clear();
        return;
    }
    for (const char* n : names) {
        if (roa::call_real("variable_instance_exists", { g_player, n }) < 0.5) continue;
        double cur = iget(g_player, n);
        auto st = g_speed_set.find(n);
        if (st != g_speed_set.end() && std::fabs(cur - st->second) > 1e-6) {          // the character changed it since: that is its new base value
            g_speed_orig[n] = cur;
            if (g_speed_mult > 1.001 && (!strcmp(n, "walk_speed") || !strcmp(n, "dash_speed") || !strcmp(n, "initial_dash_speed"))) ++g_speed_votes;
        }
        auto it = g_speed_orig.find(n);
        if (it == g_speed_orig.end()) { it = g_speed_orig.emplace(n, cur).first; roa::log(std::string("speed stat ") + n + " = " + std::to_string(cur)); }
        double v = it->second * g_speed_mult;
        iset(g_player, n, v);
        g_speed_set[n] = v;
    }
    if (g_speed_votes >= 20) {
        g_speed_displace = true;
        roa::log("speed: this character rewrites its movement stats every frame; the multiplier is applied as extra movement");
    }
}
// the extra movement for such a character: whatever horizontal speed the engine just gave it, times (multiplier - 1), stopped by blocks
static void speed_displace_step() {
    if (!g_speed_displace || g_speed_mult < 1.001 || g_player < 0) return;
    double hs = iget(g_player, "hsp");
    if (std::fabs(hs) < 0.05 || iget(g_player, "hitstun") > 0) return;
    double px = iget(g_player, "x"), py = iget(g_player, "y");
    double extra = hs * (g_speed_mult - 1);
    int sg = extra > 0 ? 1 : -1, n = (int)std::floor(std::fabs(extra) + 0.5), moved = 0;
    for (int i = 0; i < n; ++i) {
        if (roa::call_real("place_meeting", { px + sg, py, g_obj_parblock }) > 0.5) {
            bool up = false;
            for (int k = 1; k <= 4 && !up; ++k) if (roa::call_real("place_meeting", { px + sg, py - k, g_obj_parblock }) < 0.5) { py -= k; up = true; }      // a small step (a ramp's edge)
            if (!up) break;
        }
        px += sg; ++moved;
    }
    if (moved) { iset(g_player, "x", px); iset(g_player, "y", py); }
}
static std::string char_name_of(int idx) {
    RValue names = roa::global_get("charNames");
    unsigned char* c = array_cell(names, 0, idx);
    unsigned char buf[16];
    if (c && peek(c, buf, 16)) { std::string s = cell_str(buf); if (s.size() > 2 && s[0] == '"') return s.substr(1, s.size() - 2); }
    return "Workshop character";
}

// Pizza Tower plays this game's sound and keeps its output session nearly silent; if Pizza Tower is gone (or never was) restore it
static void restore_own_volume() {
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        praudio::Bridge b; b.pid = GetCurrentProcessId(); b.set_session_volume(1.0f, true);
        CoUninitialize();
    }).detach();
}
static int g_boot_char = -1, g_boot_color = 0;
static void boot_match(int char_index, int color = 0) {
    ++g_epoch; g_solids_ready = false; g_warp_frame = g_frame;
    g_proxy_index = -1;      // the workshop enemy proxy is parked (RoA crashes when several slots hold a workshop character): stand-ins are plain Zetterburn CPUs
    g_boot_char = char_index; g_boot_color = color;
    garr_set("player_color", kSlot, color);
    gset("training_mode", 1);
    static const char* kWsArrays[] = { "player_ugc_path", "player_path", "player_workshop_sendvar", "player_workshop_random", "player_ugc_path" };
    if (g_proxy_index >= 0 && g_cpu_targets) {      // the proxy was loaded for one slot in the character select: give the other CPU slots the same workshop data
        for (const char* n : kWsArrays) {
            if (!roa::global_exists(n)) continue;
            RValue src = roa::call("variable_global_array_get", { n, g_proxy_slot });
            roa::log(std::string("workshop data ") + n + "[" + std::to_string(g_proxy_slot) + "] = " + src.toString());
            for (int s = 2; s <= 4; ++s) { if (s == g_proxy_slot) continue; roa::Arg a(0.0); a.v = src; roa::call("variable_global_array_set", { n, s, a }); }
        }
    }
    garr_set("player_select", kSlot, char_index);
    garr_set("player_connected", kSlot, 1);
    garr_set("player_stock", kSlot, 99);
    for (int s = 2; s <= 4; ++s) {   // stand-in CPUs: regular CPU fighters that Pizza Tower enemies are mapped onto (so real hits, grabs...)
        garr_set("player_connected", s, g_cpu_targets ? 1 : 0); if (g_cpu_targets) { garr_set("player_select", s, g_proxy_index >= 0 ? g_proxy_index : 2); garr_set("player_stock", s, 99); }
    }
    if (roa::builtin("room_set_width")) { roa::call("room_set_width", { (double)g_match_room, kRoomW }); roa::call("room_set_height", { (double)g_match_room, 100000 }); }
    roa::call("room_goto", { (double)g_match_room });
    g_booting = true; g_ready = false; g_boot_frame = g_frame; g_player = -4; g_shift_pending = 0; g_warp_pending = true; g_ox = 0; g_oy = 0;
    roa::log("booting match, char index " + std::to_string(char_index));
}

// Moves a block-like instance out of the way (we cannot destroy instances from here).
static void park(double id) { iset(id, "x", -100000); iset(id, "y", -100000); }

// RoA only runs the character; the player should see Pizza Tower. Keep RoA's window alive (so it still
// renders) but off-screen, out of the taskbar and silent. Create mods/pizzarivals_showwindow.txt to debug it visually.
// RoA keeps music and effects on separate settings: silence only the music.
// RoA keeps its volume settings in the profile it saves; Pizza Tower owns the sound while the bridge runs, so the settings are only
// changed while the game runs and put back whenever RoA saves its profiles (and for good when the bridge goes away).
static bool g_hover_hud = true;                           // option: RoA's name / percent display above the player
static double g_orig_music_on = -1, g_orig_music_vol = -1, g_orig_bgm_fade = -1, g_orig_sfx = -1, g_orig_names = -1;
static bool g_audio_changed = false;
static void remember_audio_settings() {
    if (g_orig_music_vol >= 0) return;
    g_orig_music_on = gget("music_on", 1); g_orig_music_vol = gget("music_volume", 1); g_orig_bgm_fade = gget("bgm_fade_volume", 1); g_orig_sfx = gget("sfx_volume", 1); g_orig_names = gget("display_names", 1);
    if (g_orig_music_vol < 0.01) { g_orig_music_vol = 1; g_orig_music_on = 1; if (g_orig_bgm_fade < 0.01) g_orig_bgm_fade = 1; }     // (an earlier version saved the muted values)
    if (g_orig_sfx < 0.01) g_orig_sfx = 1;
    roa::log("audio settings of the game: music " + std::to_string(g_orig_music_vol) + " on " + std::to_string(g_orig_music_on) + " sfx " + std::to_string(g_orig_sfx) + " names " + std::to_string(g_orig_names));
}
static void restore_audio_settings() {
    if (g_orig_music_vol < 0) return;
    gset("music_on", g_orig_music_on); gset("music_volume", g_orig_music_vol); gset("bgm_fade_volume", g_orig_bgm_fade); gset("sfx_volume", g_orig_sfx); gset("display_names", g_orig_names);
    g_audio_changed = false;
}
static void mute_music() { remember_audio_settings(); g_audio_changed = true; gset("music_on", 0); gset("music_volume", 0); gset("bgm_fade_volume", 0); gset("sfx_volume", 1.0); }
static ScriptFn g_save_profiles_orig = nullptr;
static RValue* __cdecl save_profiles_detour(CInstance* self, CInstance* other, RValue* res, int argc, RValue** args) {
    bool was = g_audio_changed;
    if (was) restore_audio_settings();
    RValue* r = g_save_profiles_orig(self, other, res, argc, args);
    if (was) { mute_music(); if (!g_hover_hud) gset("display_names", 0); }
    return r;
}
// Keeps RoA's window out of sight: off-screen, no taskbar button, never flashing for attention. Style changes only reach the taskbar
// after a hide/show cycle, so the first call after the window was shown for character selection does that cycle.
static void enforce_hidden(bool cycle) {
    if (GetFileAttributesA("mods/pizzarivals_showwindow.txt") != INVALID_FILE_ATTRIBUTES) return;
    HWND w = (HWND)roa::get_window();
    if (!w) return;
    if (cycle) ShowWindow(w, SW_HIDE);
    LONG ex = GetWindowLongA(w, GWL_EXSTYLE);
    LONG want = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
    if (want != ex) SetWindowLongA(w, GWL_EXSTYLE, want);
    RECT rc; GetWindowRect(w, &rc);
    if (cycle || rc.left > -20000 || rc.top > -20000) SetWindowPos(w, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (cycle) ShowWindow(w, SW_SHOWNOACTIVATE);
    FLASHWINFO fi{}; fi.cbSize = sizeof fi; fi.hwnd = w; fi.dwFlags = FLASHW_STOP; FlashWindowEx(&fi);
}
static void hide_roa_window() {
    if (GetFileAttributesA("mods/pizzarivals_showwindow.txt") != INVALID_FILE_ATTRIBUTES) return;
    // size first (it can move the window), then hide
    if (roa::builtin("window_set_fullscreen")) roa::call("window_set_fullscreen", { 0 });
    if (roa::builtin("window_set_size")) roa::call("window_set_size", { 1920, 1080 });
    enforce_hidden(g_window_was_shown); g_window_was_shown = false;
    if (roa::builtin("audio_master_gain")) roa::call("audio_master_gain", { 1 });   // character sound effects stay audible
    mute_music();
    // render at exactly 2x the 960x540 view so the capture samples whole pixels
    roa::log(std::string("window resize: window_set_size ") + (roa::builtin("window_set_size") ? "available" : "MISSING") + ", window_set_fullscreen " + (roa::builtin("window_set_fullscreen") ? "available" : "MISSING"));
}

static void post_boot_setup() {
    {   // which gamepads does RoA see? (two entries for one physical pad would explain doubled presses)
        std::string o = "gamepads: count=" + std::to_string((int)roa::call_real("gamepad_get_device_count", {}));
        for (int i = 0; i < 12; ++i) if (roa::call_real("gamepad_is_connected", { (double)i }) > 0.5) o += " [" + std::to_string(i) + "] " + roa::call_string("gamepad_get_description", { (double)i });
        roa::log(o);
        o = "controller assignment:"; for (const char* n : { "pc_controller_num", "pc_control_type", "player_controller", "controller_num", "g_controller_num" }) if (roa::global_exists(n)) { RValue v = roa::global_get(n); o += std::string(" ") + n + "=" + gml::cell_str((const unsigned char*)&v); }
        roa::log(o);
    }
    hide_roa_window();
    // the stage's own solids would fight the mirrored Pizza Tower geometry: park them
    for (double obj : { g_obj_parblock, g_obj_parjump }) {
        int n = inst_count(obj);
        for (int i = 0; i < n; ++i) park(inst_find(obj, i));
        roa::log("parked " + std::to_string(n) + " stage blocks");
    }
    g_speed_orig.clear(); g_speed_set.clear(); g_speed_displace = false; g_speed_votes = 0;
    if (g_css_prev_fg) { SetForegroundWindow(g_css_prev_fg); g_css_prev_fg = nullptr; }
    g_pool_solid.clear(); g_pool_platform_ids.clear(); g_pool_geom.clear(); g_clipped.clear(); g_solid_rev_seen = 0; g_slope_block = -1; g_patch_block = -1;
    // render only the character on a flat key colour
    for (const char* o : { "dust_bg_surface", "dust_fg_surface" }) { double a = asset(o); if (a >= 0) roa::call("instance_deactivate_object", { a }); }   // their draw event crashes on hit-dust
    // Stage art is hidden, not deactivated: articles/projectiles (rocks, puddles, flowers) read stage data and vanish without it.
    for (const char* o : { "HUD", "stage_HUD", "obj_draw_stagebackground", "obj_stage_main", "obj_custom_stage", "draw_tut_fgs", "draw_tut_stage_bgs", "tall_block_obj", "platform_blocker" }) {
        double a = asset(o);
        if (a < 0) continue;
        int n = inst_count(a);
        for (int i = 0; i < n; ++i) iset(inst_find(a, i), "visible", 0);
    }
    {   // RoA derives its blast zone (and what it destroys / cancels) from the room size: -100..width+100, -100..height+184.
        // Make the room enormous so the blast zone is out of reach wherever Pizza Tower's level puts the character.
        const double big = 100000;
        gset("room_width", kRoomW); gset("room_height", big);
        if (g_player >= 0) { iset(g_player, "room_width", kRoomW); iset(g_player, "room_height", big); }
        if (roa::builtin("room_set_width")) { roa::call("room_set_width", { (double)g_match_room, kRoomW }); roa::call("room_set_height", { (double)g_match_room, big }); }
        roa::log("room enlarged: " + std::to_string(gget("room_width")) + " x " + std::to_string(gget("room_height")));
    }
    gset("background_color", kKeyColor);
    gset("background_showcolor", 1);
}

// ------------------------------------------------------------------------------------------
// collision mirror: PT solids -> pooled solid_32_obj / jumpthrough_32_obj instances
// ------------------------------------------------------------------------------------------
static double make_block(double obj) {
    // `instance_create` is a RoA script (not a builtin): run it through script_execute
    RValue r = roa::call("script_execute", { asset("instance_create"), 0, 0, obj });
    return roa::real_of(r);
}
static void place_block(double id, const PRSolid& s) {
    // jump-through platforms are only a thin surface: a thick one makes the player count as "on the ground"
    // while jumping up through it (repeated ground jumps in mid-air)
    double h = (s.kind == PR_SOLID_PLATFORM) ? 8.0 : (double)s.h;
    iset(id, "image_xscale", s.w / 32.0);
    iset(id, "image_yscale", h / 32.0);
    iset(id, "x", s.x - g_ox);
    iset(id, "y", s.y - g_oy);
}

static std::vector<PRSolid> g_slopes;                       // PT slope boxes (kind 2: rises right, 3: rises left), handled by update_slope_block()
static void place_all_solids() {
    g_slopes.clear(); g_clipped.clear();
    size_t used_s = 0, used_p = 0;
    for (uint32_t i = 0; i < g_solids_cache.count; ++i) {
        const PRSolid& s = g_solids_cache.solids[i];
        if (s.kind == PR_SOLID_SLOPE_UP_R || s.kind == PR_SOLID_SLOPE_UP_L) { g_slopes.push_back(s); continue; }
        bool plat = (s.kind == PR_SOLID_PLATFORM);
        auto& pool = plat ? g_pool_platform_ids : g_pool_solid;
        size_t& used = plat ? used_p : used_s;
        if (used >= pool.size()) {
            double id = make_block(plat ? g_obj_plat : g_obj_solid);
            if (id < 0) { roa::log("could not create block"); return; }
            pool.push_back(id);
        }
        if (!plat) { if (g_pool_geom.size() <= used) g_pool_geom.resize(used + 1); g_pool_geom[used] = s; }
        place_block(pool[used++], s);
    }
    for (size_t i = used_s; i < g_pool_solid.size(); ++i) park(g_pool_solid[i]);
    for (size_t i = used_p; i < g_pool_platform_ids.size(); ++i) park(g_pool_platform_ids[i]);
}
// ---- Pizza Tower enemies as hittable stand-ins -------------------------------------------------------------------------
// RoA's attacks only "hit" things RoA knows about. Pizza Tower sends the enemies' rectangles; each gets an invisible abyss pinata
// (a punching-bag enemy) laid over it, kept alive, in place and silent, so every hit goes through RoA's real pipeline: the
// character's own hit effects (skins included), sounds, hitpause, projectiles reacting (Kragg's rock bouncing), meter...
static std::vector<double> g_pool_targets;
static int g_target_count = 0;
static void drive_pinata(size_t k, const PRTarget* t, double obj) {
    if (k >= g_pool_targets.size()) { if (!t) return; double nid = make_block(obj); if (nid < 0) return; g_pool_targets.push_back(nid); }
    double id = g_pool_targets[k];
    if (roa::call_real("variable_instance_exists", { id, "hitpoints" }) < 0.5) {      // it died (or was removed): make a new one when it is needed
        if (!t) return;
        double nid = make_block(obj); if (nid < 0) return; g_pool_targets[k] = id = nid;
    }
    if (t) {
        iset(id, "x", std::floor(t->x - g_ox)); iset(id, "y", std::floor(t->y - g_oy));
        iset(id, "image_xscale", t->w / 101.0); iset(id, "image_yscale", t->h / 101.0);
    } else { iset(id, "x", 90000); iset(id, "y", 0); }
    iset(id, "visible", 0); iset(id, "image_speed", 0);
    iset(id, "hitpoints", 9999); iset(id, "health", 100); iset(id, "dying", 0); iset(id, "destroyed", 0);
    iset(id, "hsp", 0); iset(id, "vsp", 0); iset(id, "knock_hsp", 0); iset(id, "knock_vsp", 0);
}

// ---- stand-in CPU fighters ---------------------------------------------------------------------------------------------
// Each CPU stands in for one Pizza Tower enemy (the nearest ones get them). RoA ignores x written to a player but honours its speed,
// so it is steered with hsp (capped per frame: RoA moves a fast player pixel by pixel and a huge value freezes the game); y can be
// written directly. The body gets an empty collision mask so Pizza Tower's walls cannot catch it; hits register against its hurtbox
// object, which has its own sprite and is scaled to the enemy. When RoA hits the CPU, RoA takes over (JUGGLE): the CPU is released
// and gets its real physics back (knockback, hitstun, landing, combos at 0%), Pizza Tower puts the enemy where the CPU is, and when
// the CPU has recovered and stands on the ground again the enemy is killed.
struct DummyState { uint32_t enemy = 0; int mode = 0; int since = 0; double orig_mask = -1; int clean = 0; double base_w = 0, base_h = 0; int ground = 0; double orig_hs[4] = { -1, -1, -1, -1 }; double orig_ch = 0; bool have_hs = false; bool gun_init = false; double mask_h = 0; double px = 0, py = 0; bool have_prev = false; double ew = 0, eh = 0, last_pct = 0, h_angle = 45, h_power = 7, h_dmg = 8; int h_dir = 1, nostun = 0, launch = 0, last_hit = 0; bool release_me = false; };
static std::map<double, DummyState> g_dstate;
static std::map<uint32_t, double> g_gun_pct;       // the percent each gun boss has taken (it survives the stand-in CPU being let go and picked up again)

static void wipe_marks(double id) {
    for (const char* n : { "marked", "marked_player", "enemy_plantID", "enemy_tree_plantID", "in_tree_plant", "root_in_hitstun" }) iset(id, n, 0);
}
static void restore_cpu_box(double id, DummyState& ds);
static void release_dummy(double id, DummyState& ds) {
    wipe_marks(id); iset(id, "image_yscale", 1); restore_cpu_box(id, ds);
    bool was_hit = ds.mode == 1;
    { auto it = g_pins.find(id); if (it != g_pins.end()) it->second.on = false; } ds.ground = 0; iset(id, "spr_dir", 1);
    ds.gun_init = false; ds.enemy = 0; ds.mode = 0; ds.clean = was_hit ? 24 : 2; if (iget(id, "pr_on", -1) >= 0) { iset(id, "pr_on", 0); iset(id, "pr_mode", 0); iset(id, "pr_done", 0); }      // fresh start: also drops anything RoA attached to it (Maypul's seed mark...)
    double hb = iget(id, "hurtboxID", -4);
    if (hb >= 0) { iset(hb, "image_xscale", 1); iset(hb, "image_yscale", 1); }
}

static std::map<int, double> g_box_sprites;
static double box_sprite(double w, double h) {
    int W = ((int)std::ceil(w) + 3) & ~3, H = ((int)std::ceil(h) + 3) & ~3;
    if (W < 8) W = 8; if (H < 8) H = 8; if (W > 700) W = 700; if (H > 700) H = 700;
    int key = (W << 12) | H;
    auto it = g_box_sprites.find(key); if (it != g_box_sprites.end()) return it->second;
    double spr = -1, sf = roa::call_real("surface_create", { (double)W, (double)H });
    if (sf >= 0) {
        roa::call("surface_set_target", { sf }); roa::call("draw_clear", { 16777215 }); roa::call("surface_reset_target");
        spr = roa::call_real("sprite_create_from_surface", { sf, 0, 0, (double)W, (double)H, 0, 0, W / 2.0, (double)H });
        roa::call("surface_free", { sf });
    }
    g_box_sprites[key] = spr;
    return spr;
}
static const char* kHurtVars[4] = { "hurtbox_spr", "crouchbox_spr", "air_hurtbox_spr", "hitstun_hurtbox_spr" };
static void set_cpu_box(double id, double hb, double w, double h) {
    double spr = box_sprite(w, h); if (spr < 0) return;
    for (const char* v : kHurtVars) iset(id, v, spr);
    if (hb >= 0) { iset(hb, "sprite_index", spr); iset(hb, "mask_index", spr); iset(hb, "image_yscale", 1); }
    double sd = iget(id, "spr_dir", 1); if (sd != 1 && sd != -1) iset(id, "spr_dir", 1);
    double ch = h < 30 ? 30 : (h > 140 ? 140 : h); iset(id, "char_height", ch);     // effects that hang on the victim are sized by this
}
static double g_empty_spr = -2;
static const double kMirrorPadX = 10, kMirrorPadY = 8;   // the RoA mirror of an enemy is a little bigger than the enemy (it trails Pizza Tower by a frame)
static double g_pt_kill_pct = 25;                         // "PT kill percent": a hit on an enemy already at this percent (or more) gets Pizza Tower's own reaction
static int g_hit_logs = 0;
static int g_floor_frames = 60;                           // option: how long the enemy stays on the ground before it dies
static bool g_pt_always = false;                          // option: every hit on a stand-in CPU is answered with Pizza Tower's own reaction (it kills)
static double dummy_pct(int slot) { return garr_get("player_damage", slot); }

static void restore_cpu_box(double id, DummyState& ds) {
    if (!ds.have_hs) return;
    for (int i = 0; i < 4; ++i) iset(id, kHurtVars[i], ds.orig_hs[i]);
    if (ds.orig_ch > 0) iset(id, "char_height", ds.orig_ch);
}

static void drive_dummy(double id, DummyState& ds, const PRTarget* t, PRFeedback* fb) {
    static bool kp_init = false; if (!kp_init) { kp_init = true; std::ifstream kf("mods/pizzarivals_killpct.txt"); double v; if (kf >> v) { g_pt_kill_pct = v; roa::log("kill percent from file: " + std::to_string(v)); } }
    static bool nohide = GetFileAttributesA("mods/pizzarivals_nohide.txt") != INVALID_FILE_ATTRIBUTES;
    if (!nohide) iset(id, "visible", 0);
    int sl = (int)iget(id, "player", 2);
    // The hurtbox object takes the CPU's mask sprite as its own sprite. A 1x1 pixel (origin top-left) makes the body harmless to Pizza
    // Tower's walls and lets the hurtbox be any rectangle: scale = the enemy's size, origin = the enemy's top-left corner.
    static double pixel = -2; if (pixel == -2) pixel = asset("spr_whitepixel");
    if (g_empty_spr == -2) g_empty_spr = asset("empty_sprite");
    static bool nomask = GetFileAttributesA("mods/pizzarivals_nomask.txt") != INVALID_FILE_ATTRIBUTES; if (nomask) pixel = -1;
    double hb = iget(id, "hurtboxID", -4);
    if (ds.orig_mask < 0) {
        ds.orig_mask = iget(id, "mask_index", -1);
        for (int i = 0; i < 4; ++i) ds.orig_hs[i] = iget(id, kHurtVars[i], -1); ds.orig_ch = iget(id, "char_height", 52); ds.have_hs = true;
        if (hb >= 0) { ds.base_w = iget(hb, "bbox_right") - iget(hb, "bbox_left") + 1; ds.base_h = iget(hb, "bbox_bottom") - iget(hb, "bbox_top") + 1; }
        if (ds.base_w < 4) ds.base_w = 42; if (ds.base_h < 4) ds.base_h = 59;
    }
    double st = iget(id, "state");
    const bool boss = (ds.enemy & 0x80000000u) != 0, gun = (ds.enemy & 0x40000000u) != 0;       // bosses ignore the enemy options: 0% start, 10% finishing blow, a quarter second on the ground
    const double epct = boss ? 0.0 : g_enemy_pct; const int ffr = boss ? 45 : g_floor_frames;
    // never let the CPU tech, dodge, parry, act on its own or stay invincible: any of that makes it unhittable or sends it somewhere unexpected
    iset(id, "ai_disabled", 1); iset(id, "ai_tech_chance", 0); iset(id, "ai_parry_chance", 0); iset(id, "can_tech", 0); iset(id, "can_wall_tech", 0); iset(id, "untechable", 1);
    iset(id, "invincible", 0); iset(id, "invince_time", 0); iset(id, "intangible", 0); iset(id, "respawn_invince_time", 0); iset(id, "initial_invince", 0);
    // a fresh enemy sits at the chosen percent; once RoA owns it (juggle) the percent is left alone and grows with every hit
    if (ds.mode == 0 && !(iget(id, "hitstun") > 0 || st == 12) && !(gun && ds.gun_init)) { double sv = epct; if (gun) { auto gi = g_gun_pct.find(ds.enemy); if (gi != g_gun_pct.end()) sv = gi->second; } roa::call("script_execute", { asset("set_player_damage"), (double)sl, sv }); if (gun) ds.gun_init = true; }
    if (gun && ds.mode == 0 && !(iget(id, "hitstun") > 0 || st == 12)) g_gun_pct[ds.enemy] = dummy_pct(sl);   // (not on the frame it was hit: that hit's damage must stay)
    if (ds.clean > 0) {                                                                    // being reset
        // a reset wipes whatever RoA attached to the fighter (Maypul's mark...): one frame of "dead", then plain idle again
        iset(id, "state", ds.clean == 24 ? 15 : 1); iset(id, "state_timer", 0); iset(id, "hitstun", 0); iset(id, "hitpause", 0); iset(id, "hitstop", 0); iset(id, "hitstop_full", 0); iset(id, "hitstun_full", 0); iset(id, "free", 1);
        iset(id, "invincible", 0); iset(id, "invince_time", 0);
        --ds.clean; fb->mode = -1;
    }
    if (!t && ds.mode == 0) {                                                              // parked: far above, out of everyone's way
        wipe_marks(id);                                                                    // (a parked CPU must never be a target for Maypul's up special...)
        if (std::fabs(iget(id, "spr_dir", 1)) > 1.5) iset(id, "spr_dir", 1);
        if (ds.clean == 0) { double fy = iget(g_player, "y") - 5000; iset(id, "hsp", 0); iset(id, "y", fy); iset(id, "yprevious", fy); iset(id, "vsp", 0); Pin& pn = g_pins[id]; pn.x = 20000 + 300 * (double)(((long long)id) % 3); pn.y = fy; pn.on = true; }
        if (pixel >= 0) iset(id, "mask_index", pixel);
        if (hb >= 0) { iset(hb, "image_xscale", 1); iset(hb, "image_yscale", 1); }
        fb->mode = -1;
        return;
    }
    if (t) { ds.ew = t->w; ds.eh = t->h; }
    double hitstun = iget(id, "hitstun"), free_ = iget(id, "free", 1);
    auto read_hit = [&]() {                                                                // what hit it (for a finishing hit handed to Pizza Tower)
        double hbx = iget(id, "enemy_hitboxID", -4);
        ds.h_angle = 45; ds.h_power = 7; ds.h_dmg = 8;
        if (hbx >= 0) { ds.h_angle = iget(hbx, "kb_angle", 45); ds.h_power = iget(hbx, "kb_value", 7); ds.h_dmg = iget(hbx, "damage", 8); }
        ds.h_dir = iget(g_player, "spr_dir", 1) >= 0 ? 1 : -1;
    };
    auto on_hit = [&](double pre_pct) {
        read_hit();
        double pct = dummy_pct(sl);
        // RoA thinks anything far from its small stage is about to die: huge hitstun and hitpause on every hit. Keep them normal.
        double cap = 25 + pct * 0.6, hs = iget(id, "hitstun"), hp = iget(id, "hitpause"), hstop = iget(id, "hitstop");
        if (g_hit_logs < 30 || hstop >= 12 || hp >= 12 || hs > cap + 10) { ++g_hit_logs; char m[300]; sprintf_s(m, "hit on cpu: pre=%.0f pct=%.0f hitstun=%.0f full=%.0f hitpause=%.0f hitstop=%.0f cap=%.0f angle=%.0f power=%.1f dmg=%.0f x=%.0f y=%.0f p1y=%.0f roomh=%.0f p1attack=%.0f", pre_pct, pct, hs, iget(id, "hitstun_full"), hp, hstop, cap, ds.h_angle, ds.h_power, ds.h_dmg, iget(id, "x"), iget(id, "y"), iget(g_player, "y"), gget("room_height"), iget(g_player, "attack")); roa::log(m); }
        if (hs > cap) { iset(id, "hitstun", cap); if (iget(id, "hitstun_full") > cap) iset(id, "hitstun_full", cap); }
        if (hp > 18) iset(id, "hitpause", 18);
        if (hstop > 18) iset(id, "hitstop", 18);
        if (iget(g_player, "hitpause") > 18) iset(g_player, "hitpause", 18);
        if (iget(g_player, "hitstop") > 18) iset(g_player, "hitstop", 18);
        double thr = gun ? 25.0 : ((ds.enemy & 0x20000000u) ? 0.0 : (boss ? 10.0 : g_pt_kill_pct)); if ((g_pt_always && !boss) || (pre_pct >= thr - 0.01)) ds.launch = 4;           // the enemy was already past the kill percent: this hit finishes it Pizza Tower style
    };
    int gun_fb = 0;
    if (gun && ds.mode == 0 && (hitstun > 0 || st == 12)) {         // a gun boss does not react: the hit only adds percent; at 10% the blow takes a piece of its health and the percent starts over
        read_hit();
        double pn = dummy_pct(sl);
        iset(id, "hitstun", 0); iset(id, "hitstun_full", 0); iset(id, "hitpause", 0); iset(id, "hitstop", 0); iset(id, "hitstop_full", 0);
        iset(id, "state", 1); iset(id, "state_timer", 0); iset(id, "hsp", 0); iset(id, "vsp", 0);
        g_gun_pct[ds.enemy] = pn; if (pn >= 25 - 0.01) { gun_fb = 3; g_gun_pct[ds.enemy] = 0; roa::call("script_execute", { asset("set_player_damage"), (double)sl, 0.0 }); roa::log("gun boss blow at " + std::to_string(pn) + "%"); }
        hitstun = 0; st = 1;
    }
    if (ds.mode == 0) {
        if (hitstun > 0 || st == 12) {                                                      // RoA hit it: RoA owns it from now on
            ds.have_prev = false; ds.mode = 1; ds.since = g_frame; ds.last_hit = g_frame; ds.nostun = 0; ds.launch = 0; ds.last_pct = epct;
            if (t) {                                                                        // body and enemy share their feet: the (real, bottom-centred) hurtbox lands on the enemy
                double fx = std::floor(t->x - g_ox), fy = std::floor(t->y + t->h / 2 - g_oy);
                iset(id, "x", fx); iset(id, "y", fy); iset(id, "xprevious", fx); iset(id, "yprevious", fy);
            }
            if (ds.orig_mask >= 0) iset(id, "mask_index", ds.orig_mask);
            { auto it = g_pins.find(id); if (it != g_pins.end()) it->second.on = false; }
            if (std::fabs(iget(id, "spr_dir", 1)) > 1.5) iset(id, "spr_dir", 1);
            roa::log("stand-in CPU hit: juggling enemy " + std::to_string(ds.enemy));
            on_hit(ds.last_pct);
            ds.last_pct = dummy_pct(sl);
        } else {
            if (pixel >= 0) iset(id, "mask_index", pixel);
            iset(id, "image_yscale", 1);
            if (hitstun <= 0 && (st == 0 || st == 14 || st == 15)) { iset(id, "state", 1); iset(id, "state_timer", 0); iset(id, "hitpause", 0); iset(id, "hitstop", 0); }   // respawn / dead poses cannot be hit
            double Wp = t->w + 2 * kMirrorPadX, Hp = t->h + 2 * kMirrorPadY;
            set_cpu_box(id, hb, Wp, Hp);
            double ty = std::floor(t->y + t->vy * 1.5 + t->h / 2 - 3 - g_oy), tx = std::floor(t->x + t->vx * 1.5 - g_ox);
            { Pin& pn = g_pins[id]; pn.x = tx; pn.y = ty; pn.on = true; }
            iset(id, "x", tx); iset(id, "y", ty); iset(id, "yprevious", ty); iset(id, "hsp", 0); iset(id, "vsp", 0);
            fb->mode = 0; fb->pct = (float)(gun ? dummy_pct(sl) : epct);
            if (gun_fb == 3) { fb->mode = 3; fb->angle = (float)ds.h_angle; fb->power = (float)ds.h_power; fb->dmg = (float)ds.h_dmg; fb->dir = ds.h_dir; fb->pct = 0; }
            return;
        }
    }
    // JUGGLE: the CPU follows RoA's physics (real mask again); the enemy follows the CPU
    { auto it = g_pins.find(id); if (it != g_pins.end()) it->second.on = false; }
    if (std::fabs(iget(id, "spr_dir", 1)) > 1.5) iset(id, "spr_dir", 1);
    if (ds.orig_mask >= 0) iset(id, "mask_index", ds.orig_mask);
    if (ds.mask_h <= 0 && ds.orig_mask >= 0) { double mh = roa::call_real("sprite_get_bbox_bottom", { ds.orig_mask }) - roa::call_real("sprite_get_bbox_top", { ds.orig_mask }) + 1; ds.mask_h = mh > 8 ? mh : 59; }
    if (ds.mask_h > 0 && ds.eh > 0) { double sy = ds.eh / ds.mask_h; iset(id, "image_yscale", sy < 0.6 ? 0.6 : (sy > 2.0 ? 2.0 : sy)); }
    if (ds.ew > 0) set_cpu_box(id, hb, ds.ew + 2 * kMirrorPadX, ds.eh + 2 * kMirrorPadY);
    double pct = dummy_pct(sl);
    if (ds.launch == 0 && pct > ds.last_pct + 0.5) { on_hit(ds.last_pct); ds.last_pct = pct; ds.last_hit = g_frame; ds.nostun = 0; }   // a new hit: percent went up
    bool resting = free_ < 0.5 || std::fabs(iget(id, "vsp")) < 0.8;      // (on a slope the CPU is held up without ever "landing")
    if (hitstun > 0 || !resting) ds.nostun = 0; else ++ds.nostun;      // the timer starts once it is resting on the ground out of hitstun
    int age = g_frame - ds.since;
    fb->pct = (float)pct;
    {   // a hit never teleports an enemy: a jump of more than 150px in a frame (a script moving its victim) is undone
        double cxn = iget(id, "x"), cyn = iget(id, "y");
        if (ds.have_prev && (std::fabs(cxn - ds.px) > 150 || std::fabs(cyn - ds.py) > 150)) {
            static int tl = 0; if (tl++ < 30) roa::log("teleport undone: cpu moved " + std::to_string(cxn - ds.px) + "," + std::to_string(cyn - ds.py) + " attack=" + std::to_string(iget(g_player, "attack")));
        }
        ds.px = cxn; ds.py = cyn; ds.have_prev = true;
    }
    fb->cx = (float)(iget(id, "x") + g_ox); fb->cy = (float)(iget(id, "y") + g_oy - ds.eh / 2 - 3);
    if (ds.launch > 0) {                                                                   // the finishing hit: Pizza Tower takes the enemy back with its own reaction
        fb->mode = 3; fb->angle = (float)ds.h_angle; fb->power = (float)ds.h_power; fb->dmg = (float)ds.h_dmg; fb->dir = ds.h_dir;
        if (--ds.launch == 0) ds.release_me = true;
        return;
    }
    // out of hitstun for a second after the last hit: the enemy dies (Pizza Tower style: it was hit once and is just left there)
    if (hitstun > 0 || iget(id, "hitpause") > 0) { if (hitstun > 0) ds.last_hit = g_frame; }
    bool over = (ds.nostun >= ffr && g_frame - ds.last_hit >= ffr + 15) || age > 3600;
    if (over) roa::log(std::string("enemy ") + std::to_string(ds.enemy) + " finished: " + (ds.nostun >= 60 ? "stood out of hitstun for a second" : "age cap") + " age=" + std::to_string(age) + " hitstun=" + std::to_string(hitstun) + " free=" + std::to_string(free_));
    fb->mode = over ? 2 : 1;
}

static void sync_targets() {
    if (g_player < 0 || !g_s) return;
    PRTargetSet ts; pr::slot_read(g_s->h_target, g_s->targets, ts);
    if (ts.count > PR_MAX_TARGETS) ts.count = 0;
    if (ts.world != g_world) ts.count = 0;                               // from another room's coordinates
    { static uint32_t last_rev = 0; static int last_change = 0; if (ts.revision != last_rev) { last_rev = ts.revision; last_change = g_frame; } else if (g_frame - last_change > 8) ts.count = 0; }
    if (g_warp_pending) ts.count = 0;                                    // coordinates are about to change: let go of everything for a moment
    // the stand-in CPUs: every oPlayer that is not ours
    g_dummies.clear();
    { int n = inst_count(g_obj_oplayer); for (int i = 0; i < n; ++i) { double id = inst_find(g_obj_oplayer, i); if (id != g_player) g_dummies.push_back(id); } }
    size_t nd = g_dummies.size();
    std::vector<DummyState*> ds(nd);
    for (size_t d = 0; d < nd; ++d) ds[d] = &g_dstate[g_dummies[d]];
    // which enemies deserve a CPU: the nearest ones (Pizza Tower sends them nearest first)
    size_t want = ts.count < nd ? ts.count : nd;
    auto index_of = [&](uint32_t eid) { for (uint32_t i = 0; i < ts.count; ++i) if (ts.t[i].id == eid) return (int)i; return -1; };
    std::vector<int> tidx(nd, -1); std::vector<char> taken(ts.count, 0);
    for (size_t d = 0; d < nd; ++d) {
        if (!ds[d]->enemy) continue;
        int i = index_of(ds[d]->enemy);
        bool juggling = ds[d]->mode == 1;
        if (i < 0 || (!juggling && i >= (int)want)) { if (juggling && i < 0) roa::log("juggled enemy " + std::to_string(ds[d]->enemy) + " is no longer reported by Pizza Tower: released"); release_dummy(g_dummies[d], *ds[d]); continue; }
        tidx[d] = i; taken[i] = 1;
    }
    for (size_t i = 0; i < want; ++i) {
        if (taken[i]) continue;
        for (size_t d = 0; d < nd; ++d) if (!ds[d]->enemy && ds[d]->clean == 0) { ds[d]->enemy = ts.t[i].id; ds[d]->mode = 0; tidx[d] = (int)i; taken[i] = 1; break; }
    }
    if (GetFileAttributesA("mods/pizzarivals_dbg_hit.txt") != INVALID_FILE_ATTRIBUTES) {
        if (g_frame % 20 == 0) { char m[260]; sprintf_s(m, "dbg p1 f=%d state=%.0f attack=%.0f hitpause=%.0f hitstop=%.0f state_timer=%.0f window=%.0f x=%.0f y=%.0f buttons-attack_down=%.0f", g_frame, iget(g_player, "state"), iget(g_player, "attack"), iget(g_player, "hitpause"), iget(g_player, "hitstop"), iget(g_player, "state_timer"), iget(g_player, "window"), iget(g_player, "x"), iget(g_player, "y"), iget(g_player, "attack_down")); roa::log(m); }
        int nh = inst_count(g_obj_phitbox);
        for (int i = 0; i < nh && i < 4; ++i) { double h = inst_find(g_obj_phitbox, i); char m[300]; sprintf_s(m, "dbg hitbox f=%d #%d player=%.0f bbox x %.0f..%.0f y %.0f..%.0f hitbox_timer=%.0f", g_frame, i, iget(h, "player"), iget(h, "bbox_left"), iget(h, "bbox_right"), iget(h, "bbox_top"), iget(h, "bbox_bottom"), iget(h, "hitbox_timer", -1)); roa::log(m); }
        for (size_t d = 0; d < nd; ++d) { if (!ds[d]->enemy) continue; double hb = iget(g_dummies[d], "hurtboxID", -4); if (hb < 0) continue; char m[300]; sprintf_s(m, "dbg hurtbox f=%d cpu %zu bbox x %.0f..%.0f y %.0f..%.0f cpu x=%.0f y=%.0f state=%.0f hitstun=%.0f", g_frame, d, iget(hb, "bbox_left"), iget(hb, "bbox_right"), iget(hb, "bbox_top"), iget(hb, "bbox_bottom"), iget(g_dummies[d], "x"), iget(g_dummies[d], "y"), iget(g_dummies[d], "state"), iget(g_dummies[d], "hitstun")); roa::log(m); }
    }
    PRFeedbackSet fs{}; PRDebugSet dbg{};
    for (size_t d = 0; d < nd; ++d) {
        PRFeedback fb{}; fb.enemy = ds[d]->enemy; fb.mode = -1;
        drive_dummy(g_dummies[d], *ds[d], tidx[d] >= 0 ? &ts.t[tidx[d]] : nullptr, &fb);
        if (fb.mode == 2 || ds[d]->release_me) { release_dummy(g_dummies[d], *ds[d]); ds[d]->release_me = false; }
        if (fb.enemy && fb.mode >= 0 && fs.count < 8) fs.f[fs.count++] = fb;
        double hb = iget(g_dummies[d], "hurtboxID", -4);
        if (hb >= 0 && dbg.count < 8) {
            PRDebugBox& o = dbg.b[dbg.count++];
            o.x0 = (float)(iget(hb, "bbox_left") + g_ox); o.x1 = (float)(iget(hb, "bbox_right") + 1 + g_ox);
            o.y0 = (float)(iget(hb, "bbox_top") + g_oy);  o.y1 = (float)(iget(hb, "bbox_bottom") + 1 + g_oy);
            o.kind = ds[d]->enemy ? (ds[d]->mode == 1 ? 2 : 1) : 0; o.enemy = (int32_t)ds[d]->enemy;
        }
    }
    if (g_frame % 90 == 0) for (size_t d = 0; d < nd; ++d) { double id = g_dummies[d]; double hb = iget(id, "hurtboxID", -4); char m[300]; sprintf_s(m, "cpu %zu: x=%.0f y=%.0f mode=%d enemy=%u tidx=%d state=%.0f mask=%.0f hurtbox spr=%.0f mask=%.0f (player hurtbox_spr %.0f) bbox %.0f..%.0f x %.0f..%.0f scale %.2fx%.2f; want=%zu count=%u pending=%d", d, iget(id, "x"), iget(id, "y"), ds[d]->mode, ds[d]->enemy, tidx[d], iget(id, "state"), iget(id, "mask_index", -9), hb >= 0 ? iget(hb, "sprite_index", -9) : 0, hb >= 0 ? iget(hb, "mask_index", -9) : 0, iget(id, "hurtbox_spr", -9), hb >= 0 ? iget(hb, "bbox_left") : 0, hb >= 0 ? iget(hb, "bbox_right") : 0, hb >= 0 ? iget(hb, "bbox_top") : 0, hb >= 0 ? iget(hb, "bbox_bottom") : 0, hb >= 0 ? iget(hb, "image_xscale") : 0, hb >= 0 ? iget(hb, "image_yscale") : 0, want, ts.count, (int)g_warp_pending); roa::log(m); }
    pr::slot_write(g_s->h_fb, g_s->feedback, fs);
    pr::slot_write(g_s->h_debug, g_s->debug, dbg);
    if (GetFileAttributesA("mods/pizzarivals_markscan.txt") != INVALID_FILE_ATTRIBUTES) {   // diagnostic: what does a special (Maypul's mark...) leave on the CPU / on the player?
        static std::vector<std::string> names; static std::map<std::string, std::string> base; static double basecpu = -1;
        double cpu = -1; for (size_t d = 0; d < nd; ++d) if (ds[d]->enemy) { cpu = g_dummies[d]; break; }
        auto snap = [&](double id) { std::map<std::string, std::string> m; for (const std::string& n : names) { if (roa::call_real("variable_instance_exists", { id, n.c_str() }) < 0.5) continue; RValue v = roa::inst_get(id, n.c_str()); if (roa::g_faulted) { roa::g_faulted = false; continue; } m[n] = (v.type == GML_TYPE_REAL || v.type == GML_TYPE_INT32) ? std::to_string(roa::real_of(v)) : (v.type == GML_TYPE_STRING ? std::string("\"") + v.getString() + "\"" : (v.type == GML_TYPE_ARRAY ? std::string("<array>") : v.toString())); } return m; };
        if (names.empty()) { std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n; while (std::getline(cf, n)) if (!n.empty() && n.rfind("argument", 0) != 0 && n.find("view_") != 0 && n.find("window_") != 0 && n.find("display_") != 0) names.push_back(n); }
        if (cpu >= 0 && base.empty() && ds[0]) { base = snap(cpu); basecpu = cpu; roa::log("markscan baseline: " + std::to_string(base.size()) + " vars on cpu " + std::to_string((int)cpu)); }
        if (cpu >= 0 && !base.empty() && g_frame % 120 == 0) {
            auto cur = snap(cpu); std::string o = "markscan f=" + std::to_string(g_frame) + " CPU diffs:";
            int k = 0; for (auto& kv : cur) { auto it = base.find(kv.first); if ((it == base.end() || it->second != kv.second) && k++ < 60) o += " " + kv.first + "=" + kv.second.substr(0, 30); }
            roa::log(o);
            std::string o2 = "markscan f=" + std::to_string(g_frame) + " P1 vars holding a CPU id:";
            auto p1 = snap(g_player); for (auto& kv : p1) for (size_t d = 0; d < nd; ++d) if (kv.second == std::to_string(g_dummies[d])) o2 += " " + kv.first + "=" + kv.second;
            roa::log(o2);
            std::string o3 = "markscan f=" + std::to_string(g_frame) + " CPU keyword vars:";
            for (auto& kv : cur) { const std::string& n = kv.first; if (n.find("mark") != std::string::npos || n.find("seed") != std::string::npos || n.find("ivy") != std::string::npos || n.find("vine") != std::string::npos || n.find("plant") != std::string::npos || n.find("latch") != std::string::npos || n.find("root") != std::string::npos || n.find("tether") != std::string::npos || n.find("grapple") != std::string::npos || n.find("flower") != std::string::npos) o3 += " " + n + "=" + kv.second.substr(0, 20); }
            roa::log(o3);
            for (const char* an : { "obj_article1", "obj_article2", "obj_article3", "obj_article_platform", "obj_article_solid" }) {
                double ob = asset(an); if (ob < 0) continue; int na = inst_count(ob);
                for (int i = 0; i < na && i < 6; ++i) { double aid = inst_find(ob, i); std::string o4 = std::string("markscan ") + an + " #" + std::to_string(i) + " id=" + std::to_string((int)aid) + " vars holding a CPU id/near:"; auto av = snap(aid);
                    for (auto& kv : av) for (size_t d = 0; d < nd; ++d) if (kv.second == std::to_string(g_dummies[d])) o4 += " " + kv.first + "=" + kv.second;
                    o4 += " pos=(" + std::to_string(iget(aid, "x")) + "," + std::to_string(iget(aid, "y")) + ") player=" + std::to_string(iget(aid, "player", -1));
                    roa::log(o4); }
            }
        }
    }
    { int n = 0; for (size_t d = 0; d < nd; ++d) if (ds[d]->enemy) ++n; g_target_count = n; }
}

static void sync_solids() {
    PRSolidSet set; pr::slot_read(g_s->h_solid, g_s->solids, set);
    { static int dl = 0; if ((g_frame % 120 == 0) && dl++ < 40) roa::log("solids: set world=" + std::to_string(set.world) + " rev=" + std::to_string(set.revision) + " count=" + std::to_string(set.count) + " mine world=" + std::to_string(g_world) + " ready=" + std::to_string((int)g_solids_ready)); }
    if (set.world == g_world && set.revision != g_solid_rev_seen) { g_solid_rev_seen = set.revision; g_solids_cache = set; g_solids_dirty = true; g_solids_ready = true; if (g_patch_block >= 0) park(g_patch_block); }
    if (g_solids_dirty) { place_all_solids(); g_solids_dirty = false; }
}


// RoA keeps copies of the player's position (xprevious/yprevious and the g_player_x/g_player_y arrays);
// writing only x/y gets undone on the next frame, so move all of them.
static void set_player_pos(double x, double y) {
    ++g_epoch;
    iset(g_player, "x", x); iset(g_player, "y", y);
    iset(g_player, "xprevious", x); iset(g_player, "yprevious", y);
    garr_set("g_player_x", kSlot, x); garr_set("g_player_y", kSlot, y);
}


// The highest slope surface under a body (feet centre px, feet py, half width hw, moving 'look'), PT space.
static bool slope_find(double px, double py, double hw, double look, double& best, bool* inside = nullptr) {
    if (inside) *inside = false;
    bool found = false; best = 0;
    // pass 0: strict. pass 1 (only if nothing was found): also let a ramp's LOW end continue flat for a moment, so he rolls off the
    // bottom onto the floor instead of dropping the last few pixels (and landing).
    for (int pass = 0; pass < 2 && !found; ++pass)
    for (const PRSolid& s : g_slopes) {
        double l = s.x, r = s.x + s.w;
        if (s.w <= 0 || px + hw < l - 2 || px - hw > r + 2) continue;
        bool upR = (s.kind == PR_SOLID_SLOPE_UP_R);
        auto line_y = [&](double x, bool& valid) -> double {
            valid = true;
            if (x < l) { if (upR && pass == 0) valid = false; return upR ? (s.y + s.h) : s.y; }
            if (x > r) { if (!upR && pass == 0) valid = false; return upR ? s.y : (s.y + s.h); }
            return upR ? (s.y + s.h) - (x - l) / s.w * s.h : s.y + (x - l) / s.w * s.h;
        };
        double lead = px + (look > 0.5 ? hw : (look < -0.5 ? -hw : 0));
        bool va, vc; double a = line_y(px, va), c = line_y(lead, vc);
        double ly = 0; bool any = false;
        if (va) { ly = a; any = true; }
        if (vc && (!any || c < ly)) { ly = c; any = true; }
        if (!any) continue;
        bool in_tri = (px >= l && px <= r && py > ly + 2 && py <= s.y + s.h + 2 && ly >= py - (s.h + 8));      // below the ramp's line, inside the ramp: he went through it
        if (!in_tri && (ly < py - 30 || ly > py + 24)) continue;
        if (!found || ly < best) { best = ly; found = true; if (inside) *inside = in_tri; }
    }
    return found;
}

// Slopes, the way the castle-siege workshop stage does it: RoA has no sloped collision, so a small solid step
// is placed under the player's feet on the slope's line each frame and follows him up or down the ramp.
static void update_slope_block() {
    if (g_slope_block < 0) {
        g_slope_block = make_block(g_obj_solid);
        if (g_slope_block < 0) return;
        park(g_slope_block);
    }
    double look = iget(g_player, "hsp");
    double px = iget(g_player, "x") + look + g_ox, py = iget(g_player, "y") + g_oy;   // feet, one frame ahead, in PT space
    double hw = iget(g_player, "char_width", 36) / 2.0;          // the body is wide: the highest surface under either foot edge supports him
    double best = 0;
    bool in_ramp = false;
    bool found = slope_find(px, py, hw, look, best, &in_ramp);
    static bool dbg = GetFileAttributesA("mods/pizzarivals_slopedbg.txt") != INVALID_FILE_ATTRIBUTES;
    if (dbg) { char m[220]; sprintf_s(m, "slope: px=%.1f py=%.1f found=%d best=%.1f cur_y=%.1f free=%.0f vsp=%.1f hsp=%.1f state=%.0f", px, py, (int)found, best, iget(g_player, "y") + g_oy, iget(g_player, "free", 1), iget(g_player, "vsp"), look, iget(g_player, "state")); roa::log(m); }
    if (!found) {
        for (size_t i : g_clipped) if (i < g_pool_solid.size() && i < g_pool_geom.size()) place_block(g_pool_solid[i], g_pool_geom[i]);
        g_clipped.clear();
        park(g_slope_block);
        return;
    }
    double bw = 48.0 + 2.0 * std::fabs(look);                    // wider ahead of a fast runner
    iset(g_slope_block, "image_xscale", bw / 32.0); iset(g_slope_block, "image_yscale", 24.0 / 32.0);
    iset(g_slope_block, "x", std::floor(px - bw / 2 - g_ox));
    double target = std::floor(best - g_oy);                      // RoA-space surface height under him, whole pixels
    double cur = iget(g_player, "y");
    double rng = 14 + std::fabs(look) * 1.6;                      // a fast runner climbs more than 14px in a frame
    bool nearish = (target - cur) < rng && (target - cur) > -rng;
    if ((nearish || in_ramp) && (iget(g_player, "free", 1) < 0.5 || iget(g_player, "vsp") >= -2 || in_ramp)) {   // follow the line exactly: no "land" every frame
        iset(g_player, "y", target); iset(g_player, "yprevious", target);
    }
    iset(g_slope_block, "y", target);

    // Running down a ramp, the rear of his body is still over the higher ground behind him (a plateau, the fill under the
    // previous piece...). That corner is "inside" the ground once he has followed the slope down, and RoA pushes him back up
    // (the fall / land / fall cycle at speed). Cut away the part of such blocks that sticks above the surface he is now on.
    for (size_t i : g_clipped) if (i < g_pool_solid.size() && i < g_pool_geom.size()) place_block(g_pool_solid[i], g_pool_geom[i]);
    g_clipped.clear();
    {
        double sgn = look > 0.5 ? 1 : (look < -0.5 ? -1 : 0);
        double feet = best;                                       // PT space
        if (sgn != 0) {
            for (size_t i = 0; i < g_pool_solid.size() && i < g_pool_geom.size(); ++i) {
                const PRSolid& g = g_pool_geom[i];
                if (g.w <= 0 || g.h <= 0) continue;
                double l = g.x, r = g.x + g.w, t = g.y, bot = g.y + g.h;
                if (r < px - hw - 6 || l > px + hw + 6) continue;              // not under the body
                bool behind = (sgn > 0) ? (r <= px + 2) : (l >= px - 2);       // entirely behind the centre (relative to his motion)
                if (!behind) continue;
                if (t >= feet - 1 || t < feet - 48 || bot <= feet) continue;   // only a lip of ground sticking a little above the surface
                double h = bot - feet;
                iset(g_pool_solid[i], "image_yscale", h / 32.0);
                iset(g_pool_solid[i], "y", std::floor(feet - g_oy));
                g_clipped.push_back(i);
            }
        }
    }
}


// Pizza Tower's slopes end in small ledges and steps that its own player climbs without noticing; RoA's character stops dead at them.
// Pushing against a ledge of up to ~26px (with room for the body above it) lifts him onto it.
static bool blocked_at(double x, double y) { return roa::call_real("position_meeting", { x, y, g_obj_parblock }) > 0.5; }
static bool blocked_front(double x, double y, int d, double hw) { for (int i = 0; i < 5; ++i) if (blocked_at(x + d * (hw + 3 + 3 * i), y)) return true; return false; }
static void step_up_assist() {
    if (iget(g_player, "free", 1) > 0.5 || iget(g_player, "hitstun") > 0 || iget(g_player, "attack") != 0) return;
    double sx = garr_get("pc_left_stick_xpos", kSlot); int d = sx > 0.3 ? 1 : (sx < -0.3 ? -1 : 0);
    if (!d) { if (garr_get("pc_right_down", kSlot) > 0.5) d = 1; else if (garr_get("pc_left_down", kSlot) > 0.5) d = -1; }
    if (!d) return;
    if (std::fabs(iget(g_player, "hsp")) > 1.2) return;                 // moving: not stuck
    double x = iget(g_player, "x"), y = iget(g_player, "y"), hw = iget(g_player, "char_width", 36) / 2.0;
    {   // stuck at the top of a ramp? log what is around him (a few times per session)
        static int stuck = 0, logged = 0; if (std::fabs(iget(g_player, "hsp")) < 0.3) ++stuck; else stuck = 0;
        if (stuck == 30 && logged++ < 12) { std::string o = "stuck: x=" + std::to_string(x) + " y=" + std::to_string(y) + " dir=" + std::to_string(d) + " hw=" + std::to_string(hw) + " blocked ahead at heights:"; for (int k : { 3, 10, 18, 26, 34, 44, 54 }) o += " " + std::to_string(k) + "=" + std::to_string((int)blocked_front(x, y - k, d, hw)); o += " state=" + std::to_string((int)iget(g_player, "state")); roa::log(o); }
    }
    static int pushing = 0; if (blocked_front(x, y - 3, d, hw)) ++pushing; else pushing = 0;
    if (pushing < 6) return;                                            // only when really pushing against it (no jitter at walls)
    for (int k = 4; k <= 26; k += 2) {
        if (blocked_front(x, y - 3 - k, d, hw)) continue;
        for (double yy = y - k - 50; yy <= y - k - 6; yy += 8) for (double xx = x - hw + 2; xx <= x + hw - 2; xx += 8) if (blocked_at(xx, yy)) return;     // no room for him up there
        iset(g_player, "y", y - k); iset(g_player, "yprevious", y - k); iset(g_player, "vsp", 0); iset(g_player, "x", x + d * 2);
        return;
    }
}

// Juggled CPUs get the same ramp treatment as the player: a step block under their feet on the slope's line.
static std::map<double, double> g_cpu_slope_block;
static void update_cpu_slopes() {
    if (g_slopes.empty()) return;
    for (auto& kv : g_dstate) {
        double id = kv.first; DummyState& ds = kv.second;
        auto bit = g_cpu_slope_block.find(id);
        bool active = ds.mode == 1 && ds.launch == 0 && ds.enemy;
        if (!active) { if (bit != g_cpu_slope_block.end() && bit->second >= 0) park(bit->second); continue; }
        if (bit == g_cpu_slope_block.end()) { double blk = make_block(g_obj_solid); if (blk >= 0) park(blk); bit = g_cpu_slope_block.emplace(id, blk).first; }
        double blk = bit->second; if (blk < 0) continue;
        double hsp = iget(id, "hsp"), px = iget(id, "x") + hsp + g_ox, py = iget(id, "y") + g_oy, best = 0;
        if (!slope_find(px, py, 18, hsp, best)) { park(blk); continue; }
        iset(blk, "image_xscale", 48.0 / 32.0); iset(blk, "image_yscale", 24.0 / 32.0);
        iset(blk, "x", std::floor(px - 24 - g_ox));
        double target = std::floor(best - g_oy), cur = iget(id, "y");
        if ((target - cur) < 14 && (target - cur) > -14 && iget(id, "free", 1) < 0.5) { iset(id, "y", target); iset(id, "yprevious", target); }
        iset(blk, "y", target);
    }
}

// Keep the player's *vertical* RoA position inside RoA's own stage range. RoA's recovery moves ("bring me back on
// screen", Kragg's up special) assume the small stage room, so a Pizza Tower pit that puts him thousands of pixels
// below it would launch him far above the level. y writes stick in RoA (x writes do not), so we shift y and
// move the world and the PT<->RoA offset by the same amount, which leaves PT-space positions unchanged.
// RoA derives its blast zone from the room size (-100..width+100, -100..height+184) and Kragg's rock pillar rises from a point
// derived from it too. A huge room moves the blast zone out of reach but sends the pillar on a very long trip, so the width is
// huge and the height simply follows the character: the bottom blast zone stays ~500px under him (as on a normal stage) and
// nothing he does can reach it.
// Is the player at (or heading into) a gap too low for his full body? A block over his head, but nothing in front of his body.
static bool gap_blocked(double px, double py, double hs) {
    double sd = iget(g_player, "spr_dir", 1), stick = garr_get("pc_left_stick_xpos", kSlot);   // the direction he is pushing (a backward roll goes against his facing)
    double dir = stick > 0.3 ? 1 : (stick < -0.3 ? -1 : (hs > 0.5 ? 1 : (hs < -0.5 ? -1 : (sd < 0 ? -1 : 1))));
    double ax1 = dir > 0 ? px + 6 : px - 34, ax2 = dir > 0 ? px + 34 : px - 6;
    // (collision_rectangle comes back as 0 whether it hit something or not through this interface: sample points with position_meeting instead)
    auto hit = [&](double x1, double y1, double x2, double y2) {
        for (double yy = y1; yy <= y2 + 0.1; yy += 6) for (double xx = x1; xx <= x2 + 0.1; xx += 6)
            if (roa::call_real("position_meeting", { xx, yy, g_obj_parblock }) > 0.5) return true;
        return false;
    };
    bool head = hit(px - 12, py - 62, px + 12, py - 36) || hit(ax1, py - 62, ax2, py - 36);
    bool wall_ahead = hit(ax1, py - 30, ax2, py - 6);
    static bool dbg = GetFileAttributesA("mods/pizzarivals_gapdbg.txt") != INVALID_FILE_ATTRIBUTES;
    if (dbg && (head || wall_ahead) && std::fabs(stick) > 0.3) { static int n = 0; if (n++ < 40) { double hid = roa::call_real("collision_rectangle", { px - 12, py - 62, px + 12, py - 36, g_obj_parblock, 0, 1 }), wid = roa::call_real("collision_rectangle", { ax1, py - 30, ax2, py - 6, g_obj_parblock, 0, 1 }); char m[300]; sprintf_s(m, "gap: px=%.0f py=%.0f hs=%.1f dir=%.0f head=%d wall_ahead=%d headid=%.0f (bbox %.0f..%.0f x %.0f..%.0f) wallid=%.0f (bbox %.0f..%.0f x %.0f..%.0f)", px, py, hs, dir, (int)head, (int)wall_ahead, hid, iget(hid, "bbox_left"), iget(hid, "bbox_right"), iget(hid, "bbox_top"), iget(hid, "bbox_bottom"), wid, iget(wid, "bbox_left"), iget(wid, "bbox_right"), iget(wid, "bbox_top"), iget(wid, "bbox_bottom")); roa::log(m); } }
    return head && !wall_ahead;
}

static bool body_blocked(double px, double py) {   // would the full-height body overlap a block right here?
    for (double yy = py - 64; yy <= py - 31 + 0.1; yy += 6) for (double xx = px - 18; xx <= px + 18 + 0.1; xx += 6)
        if (roa::call_real("position_meeting", { xx, yy, g_obj_parblock }) > 0.5) return true;
    return false;
}

static void apply_room_size() {
    if (g_player < 0) return;
    double y = iget(g_player, "y");
    double h1 = (y + 360.0 > 720.0) ? y + 360.0 : 720.0;           // the player's own view (Kragg's pillar rises from the bottom blast zone: it must stay near him)
    double ymax = y;
    for (double d : g_dummies) { double dy = iget(d, "y"); if (dy > ymax && dy < y + 20000) ymax = dy; }   // enemies far below the player must not look like they are out of bounds
    double h = (ymax + 360.0 > 720.0) ? ymax + 360.0 : 720.0;
    g_bottom_all = h + 184.0;
    gset("room_width", kRoomW); gset("room_height", h);
    iset(g_player, "room_width", kRoomW); iset(g_player, "room_height", h1);
    for (double d : g_dummies) { iset(d, "room_width", kRoomW); iset(d, "room_height", h); }
    if (roa::builtin("room_set_width")) { roa::call("room_set_width", { (double)g_match_room, kRoomW }); roa::call("room_set_height", { (double)g_match_room, h }); }
}

static void recenter() {
    // RoA destroys projectiles and cancels moves once the character is far below its (720px high) stage, so keep him in
    // range. Shift only while standing (a shift is invisible then) or when absurdly far out and not mid-attack.
    double y = iget(g_player, "y");
    double st = iget(g_player, "state");
    bool grounded = iget(g_player, "free", 1) < 0.5;
    bool attacking = (st == 5 || st == 6);
    double dy = 0;
    // Kragg's pillar rises to an absolute height (so he must stay near RoA's own stage height); everyone else gets headroom above
    // (RoA thinks enemies higher than ~400px above the stage top are about to be KO'd: huge hitstun)
    bool kragg = g_char_id.find("kragg") != std::string::npos || g_css_name == "Kragg";
    if (kragg) return;      // his pillar rises to an absolute height: he stays where RoA's own stage is
    const double T = 8000.0, band = 6000.0, far_ = 12000.0;     // lots of room above (the top blast zone) and the bottom follows the lowest fighter
    if (grounded && !attacking && std::fabs(y - T) > band) dy = y - T;
    else if (!attacking && std::fabs(y - T) > far_) dy = y - T;
    if (dy == 0) return;
    {   // never in the middle of a fight (it moved enemies, hit effects and particles under the player)
        bool combat = inst_count(g_obj_phitbox) > 0;
        for (auto& kv : g_dstate) if (kv.second.mode == 1 || kv.second.launch > 0) combat = true;
        if (combat && std::fabs(y - T) < 1500) return;
    }
    iset(g_player, "y", y - dy); iset(g_player, "yprevious", iget(g_player, "yprevious") - dy);
    garr_set("g_player_y", kSlot, y - dy);
    for (auto& kv : g_dstate) kv.second.py -= dy;      // (the enemies' previous positions move with the scene)
    g_oy += dy; ++g_epoch;
    // shift EVERY other instance (projectiles, articles, effects, hurtbox...) so nothing is left behind
    int n = (int)roa::call_real("instance_number", { -3 });
    std::vector<double> ids; ids.reserve(n);
    for (int i = 0; i < n; ++i) ids.push_back(roa::call_real("instance_find", { -3, i }));
    for (double id : ids) {
        if (id == g_player) continue;
        double iy = iget(id, "y", 1e9);
        if (iy > 1e8 || iy < -50000) continue;                  // parked blocks / non-positional objects
        iset(id, "y", iy - dy);
    }
    g_solids_dirty = true;
    center_view();      // the frame RoA renders next must already be looking at the shifted scene
}

// ------------------------------------------------------------------------------------------
// export
// ------------------------------------------------------------------------------------------
static void export_player() {
    PRPlayerState ps{};
    ps.frame_id = g_s->input.frame_id;
    ps.flags = PR_ROA_READY;
    double p = g_player;
    ps.x = (float)(iget(p, "x") + g_ox); ps.y = (float)(iget(p, "y") + g_oy);
    ps.hsp = (float)iget(p, "hsp"); ps.vsp = (float)iget(p, "vsp");
    ps.spr_dir = (int8_t)(iget(p, "spr_dir", 1) < 0 ? -1 : 1);
    ps.state = (int16_t)iget(p, "state"); ps.attack = (int16_t)iget(p, "attack");
    if (iget(p, "free", 1) < 0.5) ps.flags |= PR_ROA_ON_GROUND;
    ps.warp_gen = g_warp_gen;
    if (g_warp_pending) ps.flags |= PR_ROA_WARPING;
    update_pad_active();
    if (g_pad_active) ps.flags |= PR_ROA_PAD_ACTIVE;
    if (iget(p, "hitstun") > 0) ps.flags |= PR_ROA_HITSTUN;
    if (iget(p, "taunt_down") > 0.5 && iget(p, "hitstun") <= 0) ps.flags |= PR_ROA_TAUNT;
    { double stn = iget(p, "state"); if ((stn == 8 || stn == 33 || stn == 34) && iget(p, "window") == 1) ps.flags |= PR_ROA_INVULN; }
    ps.hurtbox_w = (int16_t)iget(p, "char_width", 36); ps.hurtbox_h = (int16_t)iget(p, "char_height", 52);
    ps.damage_percent = (int16_t)roa::call_real("script_execute", { asset("get_player_damage"), kSlot });
    // capture centre: whole RoA pixels (no sub-pixel shimmer); PT draws the frame at the same point + offset
    g_cap_cx = std::floor(iget(p, "x") + 0.5); g_cap_cy = std::floor(iget(p, "y") - ps.hurtbox_h / 2.0 + 0.5); g_cap_xf = iget(p, "x"); g_cap_yf = iget(p, "y"); g_cap_valid = true;
    ps.frame_cx = (float)(g_cap_cx + g_ox); ps.frame_cy = (float)(g_cap_cy + g_oy);
    ps.view_x = (float)(g_view_x + g_ox); ps.view_y = (float)(g_view_y + g_oy); ps.view_w = (float)g_view_w; ps.view_h = (float)g_view_h;
    {   // RoA's own view of the pad (native mode): Pizza Tower follows it so menus, doors and the pause button work without owning the pad
        uint16_t m = 0;
        static bool hl = false, hr = false, hu = false, hd = false;      // stick directions with hysteresis (no flicker around the threshold)
        for (int dev = 0; dev < 4; ++dev) {
            if (roa::call_real("gamepad_is_connected", { (double)dev }) < 0.5) continue;
            auto btn = [&](int id) { return roa::call_real("gamepad_button_check", { (double)dev, (double)id }) > 0.5; };
            double ax = roa::call_real("gamepad_axis_value", { (double)dev, 32785.0 }), ay = roa::call_real("gamepad_axis_value", { (double)dev, 32786.0 });
            hl = ax < (hl ? -0.35 : -0.6); hr = ax > (hr ? 0.35 : 0.6); hu = ay < (hu ? -0.35 : -0.6); hd = ay > (hd ? 0.35 : 0.6);
            if (hl || btn(32783)) m |= PR_BTN_LEFT;
            if (hr || btn(32784)) m |= PR_BTN_RIGHT;
            if (hu || btn(32781)) m |= PR_BTN_UP;
            if (hd || btn(32782)) m |= PR_BTN_DOWN;
            if (btn(32769)) m |= PR_BTN_JUMP;       // A
            if (btn(32770)) m |= PR_BTN_ATTACK;     // B
            if (btn(32771)) m |= PR_BTN_SPECIAL;    // X
            if (btn(32772)) m |= PR_BTN_SHIELD;     // Y
            if (btn(32778)) m |= PR_BTN_START;
            uint16_t full = 0;
            for (int id = 32769; id <= 32784; ++id) if (btn(id)) full |= (uint16_t)(1u << (id - 32769));
            ps.in_pad = full;
            for (int a = 0; a < 4; ++a) { double v = roa::call_real("gamepad_axis_value", { (double)dev, (double)(32785 + a) }); ps.pad_ax[a] = (int8_t)(v > 1 ? 100 : (v < -1 ? -100 : (int)(v * 100))); }
            break;
        }
        ps.in_buttons = m;
        static uint16_t last = 0; static int logged = 0;     // trace pad changes (hunting the controller-only multi-jump)
        if (m != last && logged++ < 400) { char t[120]; sprintf_s(t, "pad: %04x -> %04x frame=%d state=%.0f free=%.0f", last, m, g_frame, iget(g_player, "state"), iget(g_player, "free", 1)); roa::log(t); }
        last = m;
    }
    ps.roa_present_fps = (float)g_present_fps; ps.roa_tick_fps = (float)g_tick_fps;
    ps.bz_l = (float)(g_bz[0] + g_ox); ps.bz_r = (float)(g_bz[1] + g_ox); ps.bz_t = (float)(g_bz[2] + g_oy); ps.bz_b = (float)(g_bz[3] + g_oy);
    pr::slot_write(g_s->h_player, g_s->player, ps);
}

struct HitInfo { double x, y, fx, snd, stop; int frame; };
static std::unordered_map<uint32_t, HitInfo> g_hit_info;       // recent hitboxes, so a late "hit connected" can still play its effect
static void export_hitboxes() {
    PRHitboxSet hs{}; hs.frame_id = g_s->input.frame_id;
    int n = inst_count(g_obj_phitbox);
    for (int i = 0; i < n && hs.count < PR_MAX_HITBOXES; ++i) {
        double h = inst_find(g_obj_phitbox, i);
        if ((int)iget(h, "player") != kSlot) continue;
        double l = iget(h, "bbox_left"), r = iget(h, "bbox_right"), t = iget(h, "bbox_top"), b = iget(h, "bbox_bottom");
        PRHitbox& o = hs.hb[hs.count++];
        o.id = (uint32_t)h; o.x = (float)((l + r) / 2 + g_ox); o.y = (float)((t + b) / 2 + g_oy); o.w = (float)(r - l); o.h = (float)(b - t);
        o.damage = (int16_t)iget(h, "damage"); o.kb_angle = (int16_t)iget(h, "kb_angle"); o.kb_power = (float)iget(h, "kb_value");
        o.hitpause = (int16_t)iget(h, "hitpause"); o.kind = (iget(h, "type") == 2) ? 1 : 0;
        g_hit_info[o.id] = { (l + r) / 2, (t + b) / 2, iget(h, "hit_effect", -1), iget(h, "sound_effect", -1), iget(h, "hitpause", 0), g_frame };
    }
    for (auto it = g_hit_info.begin(); it != g_hit_info.end();) it = (g_frame - it->second.frame > 120) ? g_hit_info.erase(it) : std::next(it);
    pr::slot_write(g_s->h_hitbox, g_s->hitboxes, hs);
}

// ------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------
// dev commands: mods/pizzarivals_dev.txt is re-run whenever it changes (experiments without rebuilds)
//   call FUNC n n n...      call a builtin/script-builtin with numeric args, log the result
//   calls FUNC text         call with one string argument
//   g NAME VALUE            set a global to a number
//   hide NAME               instance_deactivate_object(asset NAME)
//   layers a b step         tile_layer_hide(depth) for depth a..b (hides room tile layers)
// ------------------------------------------------------------------------------------------
static int g_watch = 0;
static void run_dev_file() {
    static FILETIME last = {};
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA("mods/pizzarivals_dev.txt", GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &last) == 0) return;
    last = fa.ftLastWriteTime;
    std::ifstream f("mods/pizzarivals_dev.txt");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line); std::string op; is >> op;
        if (op.empty() || op[0] == '#') continue;
        roa::g_faulted = false;
        if (op == "call") { std::string fn; is >> fn; std::vector<roa::Arg> a; double d; while (is >> d) a.push_back(d); RValue r = roa::call(fn.c_str(), std::move(a)); roa::log("call " + fn + " -> " + r.toString()); }
        else if (op == "calls") { std::string fn, t; is >> fn; std::getline(is, t); if (!t.empty() && t[0] == ' ') t.erase(0, 1); RValue r = roa::call(fn.c_str(), { t }); roa::log("calls " + fn + " -> " + r.toString()); }
        else if (op == "g") { std::string n; double v; is >> n >> v; gset(n.c_str(), v); roa::log("set " + n); }
        else if (op == "hide") { std::string n; is >> n; double a = asset(n.c_str()); roa::call("instance_deactivate_object", { a }); roa::log("deactivated " + n + " (" + std::to_string(a) + ")"); }
        else if (op == "wsprobe") {
            std::string sc; is >> sc; if (sc != "-") { RValue r = roa::call("script_execute", { asset(sc.c_str()) }); roa::log("wsprobe " + sc + " -> " + r.toString()); }
            for (const char* g : { "workshop", "workshop_can_load", "workshop_mode", "workshop_pick", "workshop_picking", "workshop_search", "workshop_search_list", "workshop_load_rate", "workshop_grid_mode", "workshop_chars", "workshop_data", "workshop_id" }) {
                if (!roa::global_exists(g)) continue;
                RValue v = roa::global_get(g); std::string o = std::string("wsprobe ") + g + " = ";
                if (v.type == GML_TYPE_ARRAY) { o += "[array"; for (int i = 0; i < 8; ++i) { unsigned char* c = gml::array_cell(v, 0, i); if (!c) break; unsigned char bb[16]; if (peek(c, bb, 16)) o += " " + cell_str(bb); } o += "]"; } else o += v.toString();
                roa::log(o);
            }
            { RValue names = roa::global_get("charNames"); std::string all; for (int i = 20; i < 60; ++i) { unsigned char* c = array_cell(names, 0, i); unsigned char bb[16]; if (!c || !peek(c, bb, 16)) continue; all += std::to_string(i) + "=" + cell_str(bb) + " "; } roa::log("wsprobe charNames: " + all); }
        }
        else if (op == "inspect") {
            std::string nm; is >> nm; double obj = asset(nm.c_str()); int n = inst_count(obj);
            roa::log("inspect " + nm + " count=" + std::to_string(n));
            for (int i = 0; i < n && i < 12; ++i) {
                double id = inst_find(obj, i);
                char b[300]; sprintf_s(b, "  #%d id=%.0f x=%.0f y=%.0f visible=%.0f sprite=%.0f xs=%.2f ys=%.2f depth=%.0f", i, id, iget(id, "x"), iget(id, "y"), iget(id, "visible"), iget(id, "sprite_index", -9), iget(id, "image_xscale"), iget(id, "image_yscale"), iget(id, "depth"));
                roa::log(b);
            }
        }
        else if (op == "gdump") {   // names (and a peek at the values) of the game's globals that contain one of the given words
            std::vector<std::string> kws; std::string k; while (is >> k) kws.push_back(k);
            RValue nm = roa::call("variable_instance_get_names", { -5.0 });
            int found = 0;
            for (int i = 0; i < 6000; ++i) {
                unsigned char* c = array_cell(nm, 0, i); unsigned char bb[16];
                if (!c || !peek(c, bb, 16)) break;
                std::string s = cell_str(bb); if (s.size() > 1 && s[0] == '"') s = s.substr(1, s.size() - 2);
                std::string l = s; for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
                bool hit = false; for (auto& w : kws) if (l.find(w) != std::string::npos) hit = true;
                if (!hit) continue;
                ++found; RValue v = roa::global_get(s.c_str()); std::string o = "gdump " + s + " = " + v.toString();
                if (v.type == GML_TYPE_ARRAY) { o += " ["; for (int j = 0; j < 6; ++j) { unsigned char* cc = gml::array_cell(v, 0, j); if (!cc) break; unsigned char b2[16]; if (peek(cc, b2, 16)) o += " " + cell_str(b2); } o += " ]"; }
                roa::log(o);
            }
            roa::log("gdump found " + std::to_string(found));
        }
        else if (op == "setpos") { double x, y; is >> x >> y; set_player_pos(x, y); roa::log("setpos done"); }
        else if (op == "watch") { is >> g_watch; }
        else if (op == "players") {
            int n = inst_count(g_obj_oplayer); roa::log("players: " + std::to_string(n));
            for (int i = 0; i < n; ++i) { double id = inst_find(g_obj_oplayer, i); char m[300]; sprintf_s(m, "  oPlayer id=%.0f player=%.0f x=%.0f y=%.0f state=%.0f visible=%.0f hurtboxID=%.0f", id, iget(id, "player"), iget(id, "x"), iget(id, "y"), iget(id, "state"), iget(id, "visible"), iget(id, "hurtboxID")); roa::log(m); }
        }
        else if (op == "run") {   // run a GML script by name with numeric args: run <script> a b c...
            std::string nm; is >> nm; std::vector<roa::Arg> a; a.push_back(asset(nm.c_str())); double d; while (is >> d) a.push_back(d);
            RValue r = roa::call("script_execute", std::move(a)); roa::log("run " + nm + " -> " + r.toString()); }
        else if (op == "sethsp") { int idx; double v; is >> idx >> v; double id = inst_find(g_obj_oplayer, idx); iset(id, "hsp", v); roa::log("sethsp " + std::to_string(idx) + " " + std::to_string(v)); }
        else if (op == "watchp") { is >> g_watchp_idx >> g_watchp; }
        else if (op == "sprq") {   // size, origin and collision box of named sprites: sprq a b c
            std::string nm; while (is >> nm) { double s = asset(nm.c_str()); if (s < 0) { roa::log("sprq " + nm + ": unknown"); continue; }
                char m[260]; sprintf_s(m, "sprq %s (%.0f): size %.0fx%.0f origin (%.0f,%.0f) bbox L%.0f R%.0f T%.0f B%.0f", nm.c_str(), s, roa::call_real("sprite_get_width", { s }), roa::call_real("sprite_get_height", { s }), roa::call_real("sprite_get_xoffset", { s }), roa::call_real("sprite_get_yoffset", { s }), roa::call_real("sprite_get_bbox_left", { s }), roa::call_real("sprite_get_bbox_right", { s }), roa::call_real("sprite_get_bbox_top", { s }), roa::call_real("sprite_get_bbox_bottom", { s })); roa::log(m); } }
        else if (op == "dumpinst") {   // dump all variables (from the candidate name list) of the first instance of an object
            std::string nm; is >> nm; double o = asset(nm.c_str()); if (o < 0 || inst_count(o) < 1) { roa::log("dumpinst: none of " + nm); }
            else { double id = inst_find(o, 0); std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n, out; int cnt = 0;
                while (std::getline(cf, n)) { if (n.empty() || n.rfind("argument", 0) == 0) continue; if (roa::call_real("variable_instance_exists", { id, n.c_str() }) < 0.5) continue;
                    if (n.rfind("phy_", 0) == 0 || n == "id" || n.rfind("view_", 0) == 0 || n.rfind("os_", 0) == 0 || n.rfind("game_", 0) == 0) continue;
                    RValue v = roa::inst_get(id, n.c_str()); if (v.type == GML_TYPE_UNDEFINED) continue;
                    out += n + "=" + (v.type == GML_TYPE_ARRAY ? std::string("<arr>") : gml::cell_str((const unsigned char*)&v)) + " "; if (++cnt % 20 == 0) { roa::log("dump " + nm + ": " + out); out.clear(); } }
                roa::log("dump " + nm + ": " + out); } }
        else if (op == "mkobj") {   // create an object next to the player: mkobj <name> <dx> <dy>
            std::string nm; double dx, dy; is >> nm >> dx >> dy; double o = asset(nm.c_str());
            if (o < 0) roa::log("mkobj: unknown object " + nm);
            else { double id = roa::real_of(roa::call("script_execute", { asset("instance_create"), iget(g_player, "x") + dx, iget(g_player, "y") + dy, o }));
                   roa::log("mkobj " + nm + " -> id " + std::to_string((int)id) + " at " + std::to_string(iget(id, "x")) + "," + std::to_string(iget(id, "y")) + " sprite " + std::to_string(iget(id, "sprite_index", -9)) + " visible " + std::to_string(iget(id, "visible", -9))); } }
        else if (op == "stagearr") {   // print an array variable of obj_custom_stage
            std::string obj, nm; is >> obj >> nm; double o = asset(obj.c_str()); if (o < 0 || inst_count(o) < 1) { roa::log("stagearr: no instance of " + obj); }
            else { double id = inst_find(o, 0); RValue v = roa::inst_get(id, nm.c_str()); std::string s; if (v.type == GML_TYPE_ARRAY) for (int r = 0; r < 3; ++r) { s += "[" + std::to_string(r) + "]:"; for (int i = 0; i < 8; ++i) { unsigned char* c = gml::array_cell(v, r, i); if (!c) break; s += gml::cell_str(c) + " "; } } else s = v.toString(); roa::log("stagearr " + nm + " = " + s); } }
        else if (op == "scanp") {   // scalar variables of oPlayer #idx equal to any given value (arrays skipped: some fault)
            int idx; is >> idx; std::vector<double> want; double w; while (is >> w) want.push_back(w);
            double id = inst_find(g_obj_oplayer, idx); std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n, o; int scanned = 0;
            while (std::getline(cf, n)) { if (n.empty() || n.rfind("argument", 0) == 0) continue; roa::g_faulted = false;
                if (roa::call_real("variable_instance_exists", { id, n.c_str() }) < 0.5) continue; ++scanned;
                RValue v = roa::inst_get(id, n.c_str()); if (roa::g_faulted) { roa::g_faulted = false; continue; }
                if (v.type == GML_TYPE_REAL || v.type == GML_TYPE_INT32) { double d = roa::real_of(v); for (double x : want) if (std::fabs(d - x) < 0.6) o += n + "=" + std::to_string((int)d) + " "; } }
            roa::g_faulted = false; roa::log("scanp " + std::to_string(idx) + " (" + std::to_string(scanned) + " vars): " + o); }
        else if (op == "moveplayer") { int idx; double x, y; is >> idx >> x >> y; double id = inst_find(g_obj_oplayer, idx); iset(id, "x", x); iset(id, "y", y); iset(id, "xprevious", x); iset(id, "yprevious", y); { int sl = (int)iget(id, "player", 1); garr_set("g_player_x", sl, x); garr_set("g_player_y", sl, y); } roa::log("moved player index " + std::to_string(idx) + " to " + std::to_string(x) + "," + std::to_string(y)); }
        else if (op == "scanval") {   // find variables (globals, player, camera, stage objects) holding any of the given numbers
            std::vector<double> want; double w; while (is >> w) want.push_back(w);
            auto hit = [&](double d) { for (double x : want) if (std::fabs(d - x) < 0.51) return true; return false; };
            std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n; std::string og, op_;
            std::vector<std::string> insts = { "oPlayer", "camera_obj", "obj_stage_main", "obj_custom_stage", "pHitBox" };
            while (std::getline(cf, n)) {
                if (n.empty() || n.rfind("argument", 0) == 0) continue;
                if (roa::global_exists(n.c_str())) {
                    RValue v = roa::global_get(n.c_str());
                    if (v.type == GML_TYPE_REAL || v.type == GML_TYPE_INT32) { if (hit(roa::real_of(v))) og += n + "=" + std::to_string((int)roa::real_of(v)) + " "; }
                    else if (v.type == GML_TYPE_ARRAY) for (int i = 0; i < 64; ++i) { unsigned char* c = gml::array_cell(v, 0, i); if (!c) break; double d = gml::cell_real(c); if (hit(d) && std::fabs(d) > 0) og += n + "[" + std::to_string(i) + "]=" + std::to_string((int)d) + " "; }
                }
            }
            roa::log("scanval globals: " + og);
            for (const std::string& io : insts) {
                double obj = asset(io.c_str()); if (obj < 0 || inst_count(obj) < 1) continue;
                double id = inst_find(obj, 0); std::string o;
                std::ifstream cf2("mods/pizzarivals_candidates.txt");
                while (std::getline(cf2, n)) {
                    if (n.empty() || n.rfind("argument", 0) == 0) continue;
                    if (roa::call_real("variable_instance_exists", { id, n.c_str() }) < 0.5) continue;
                    RValue v = roa::inst_get(id, n.c_str());
                    if (v.type == GML_TYPE_REAL || v.type == GML_TYPE_INT32) { if (hit(roa::real_of(v))) o += n + "=" + std::to_string((int)roa::real_of(v)) + " "; }
                    else if (v.type == GML_TYPE_ARRAY) for (int i = 0; i < 64; ++i) { unsigned char* c = gml::array_cell(v, 0, i); if (!c) break; double d = gml::cell_real(c); if (hit(d)) o += n + "[" + std::to_string(i) + "]=" + std::to_string((int)d) + " "; }
                }
                roa::log("scanval " + io + ": " + o);
            }
        }
        else if (op == "scanx") {      // list every player variable whose value is within 6 of the player's x (or y with "scanx y")
            std::string axis = "x"; is >> axis;
            double ref = iget(g_player, axis.c_str());
            std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n; std::string out;
            while (std::getline(cf, n)) {
                if (n.empty() || n.rfind("argument", 0) == 0) continue;
                if (roa::call_real("variable_instance_exists", { g_player, n.c_str() }) < 0.5) continue;
                RValue v = roa::inst_get(g_player, n.c_str());
                if (v.type != GML_TYPE_REAL && v.type != GML_TYPE_INT32) continue;
                double d = roa::real_of(v);
                if (std::fabs(d - ref) < 6 && std::fabs(d) > 20) out += n + "=" + std::to_string((int)d) + " ";
            }
            roa::log("scan " + axis + " ref=" + std::to_string(ref) + ": " + out);
        }
        else if (op == "statenames") {
            for (const char* sc : { "get_state_name", "get_state_name_workshop" }) {
                double sid = asset(sc);
                std::string all;
                for (int i = 0; i < 60; ++i) { RValue r = roa::call("script_execute", { sid, i }); all += std::to_string(i) + "=" + r.toString() + " "; }
                roa::log(std::string(sc) + " (" + std::to_string(sid) + "): " + all);
            }
            roa::log("damage now: " + std::to_string(roa::call_real("script_execute", { asset("get_player_damage"), 1 })) + " via player_hit_percent[1]=" + std::to_string(garr_get("player_hit_percent", 1)) + " player_damage[1]=" + std::to_string(garr_get("player_damage", 1)));
        }
        else if (op == "scalestage") {      // experiment: multiply stage_data[row>=1][col 3..11] (stage geometry) by a factor
            double f; is >> f; int n = 0;
            RValue v = roa::global_get("stage_data");
            for (int r = 1; r < 40; ++r) for (int c = 3; c <= 11; ++c) {
                unsigned char* cell = array_cell(v, r, c); unsigned char buf[16];
                if (!cell || !peek(cell, buf, 16)) continue;
                double old = cell_real(buf); RValue nv; nv.setReal(old * f); poke(cell, &nv, 16); ++n;
            }
            roa::log("scaled " + std::to_string(n) + " stage_data cells by " + std::to_string(f));
        }
        else if (op == "stagedata") {
            std::string o;
            for (int i = 0; i < 80; ++i) { RValue r = roa::call("script_execute", { asset("get_stage_data"), i }); o += std::to_string(i) + "=" + r.toString() + " "; }
            roa::log("stagedata: " + o);
            std::string o2;
            for (const char* g : { "stage_data", "g_stage_data", "blastzone", "g_blastzone", "stage_blastzone" }) if (roa::global_exists(g)) o2 += std::string(g) + " ";
            roa::log("stagedata globals: " + o2);
            { RValue sd = roa::global_get("stage_data"); std::string o3 = "stage_data[row0]: ";
              for (int r = 0; r < 4; ++r) { o3 += "row" + std::to_string(r) + ": "; for (int c = 0; c < 40; ++c) { unsigned char* cell = gml::array_cell(sd, r, c); if (!cell) break; o3 += std::to_string(c) + "=" + gml::cell_str(cell) + " "; } }
              roa::log(o3); }
        }
        else if (op == "sprinfo") {
            std::string out;
            for (const char* v : { "mask_index", "hurtbox_spr", "crouchbox_spr", "sprite_index" }) {
                double s = iget(g_player, v, -1);
                out += std::string(v) + "=" + std::to_string((int)s) + " [h=" + std::to_string((int)roa::call_real("sprite_get_height", { s })) + " bbT=" + std::to_string((int)roa::call_real("sprite_get_bbox_top", { s })) + " bbB=" + std::to_string((int)roa::call_real("sprite_get_bbox_bottom", { s })) + " yo=" + std::to_string((int)roa::call_real("sprite_get_yoffset", { s })) + "]  ";
            }
            roa::log("sprinfo: " + out);
        }
        else if (op == "mkfx") {            // experiment: create a hit_fx_obj next to the player and list its variables
            double obj = asset("hit_fx_obj");
            double id = roa::real_of(roa::call("script_execute", { asset("instance_create"), iget(g_player, "x"), iget(g_player, "y") - 40, obj }));
            std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n, out;
            while (std::getline(cf, n)) {
                if (n.empty() || n.rfind("argument", 0) == 0 || n.find("background") == 0 || n.find("view_") == 0 || n.find("os_") == 0 || n.find("current_") == 0 || n.find("game_") == 0 || n.find("display_") == 0 || n.find("window_") == 0) continue;
                if (roa::call_real("variable_instance_exists", { id, n.c_str() }) < 0.5) continue;
                RValue v = roa::inst_get(id, n.c_str());
                out += n + "=" + (v.type == GML_TYPE_STRING ? std::string("\"") + v.getString() + "\"" : (v.type == GML_TYPE_ARRAY ? std::string("<array>") : v.toString())) + " ";
            }
            roa::log("hit_fx_obj " + std::to_string((int)id) + ": " + out);
        }
        else if (op == "has") { std::string n, s; while (is >> n) s += n + (roa::builtin(n.c_str()) ? "=yes " : "=NO "); roa::log("has: " + s); }
        else if (op == "bgvis") {            // background_visible[i] / background_foreground[i] via variable_global_array_set
            std::string nm; double v; is >> nm >> v;
            for (int i = 0; i < 8; ++i) roa::call("variable_global_array_set", { nm, i, v });
            roa::log("set " + nm + "[0..7] = " + std::to_string(v));
        }
        else if (op == "tilescan") {
            double n = roa::call_real("tile_get_count");
            roa::log("tile count: " + std::to_string(n));
            for (int i = 0; i < (int)n && i < 3000; ++i) { double id = roa::call_real("tile_get_id", { i }); roa::call("tile_set_visible", { id, 0 }); }
            roa::log("tiles hidden");
        }
        else if (op == "listobjs") {
            std::map<std::string, int> counts;
            int n = (int)roa::call_real("instance_number", { -3 });
            for (int i = 0; i < n; ++i) {
                double id = roa::call_real("instance_find", { -3, i });
                double oi = iget(id, "object_index", -1);
                counts[roa::call_string("object_get_name", { oi })]++;
            }
            std::string s; for (auto& kv : counts) s += kv.first + "=" + std::to_string(kv.second) + " ";
            roa::log("objects in room (" + std::to_string(n) + "): " + s);
        }
        else if (op == "keep") {   // deactivate every instance except the listed objects (plus anything already needed)
            std::vector<std::string> names; std::string t; while (is >> t) names.push_back(t);
            roa::call("instance_deactivate_all", { 0 });
            for (auto& nme : names) roa::call("instance_activate_object", { asset(nme.c_str()) });
            for (double id : g_pool_solid) roa::call("instance_activate_object", { id });
            roa::log("kept " + std::to_string(names.size()) + " object types");
        }
        else if (op == "layers") { int a, b, st; is >> a >> b >> st; for (int d = a; d <= b; d += st) roa::call("tile_layer_hide", { d }); roa::log("tile layers hidden"); }
        else roa::log("unknown dev command: " + line);
        if (roa::g_faulted) roa::log("  (fault: " + line + ")");
    }
}



// Pizza Tower says an RoA hitbox connected with an enemy: play what RoA would (hit effect, sound, hit pause).
static void manual_hit_fx(const HitInfo& hi) {
    double x = hi.x, y = hi.y, fx = hi.fx, snd = hi.snd, stop = hi.stop;
    if (fx >= 0) {                                                  // spawn RoA's hit effect object ourselves (the script needs a player context)
        double id = roa::real_of(roa::call("script_execute", { asset("instance_create"), x, y, asset("hit_fx_obj") }));
        if (id >= 0) {
            iset(id, "hit_fx", fx); iset(id, "sprite_index", fx); iset(id, "image_index", 0); iset(id, "image_speed", 0.4);
            iset(id, "player", kSlot); iset(id, "player_id", g_player); iset(id, "depth", -50);   // player_id: the effect takes the colours of the hitter's palette
        }
    }
    if (snd > 0) {                                                  // our own hit sounds play much quieter than the character's moves
        double h = roa::real_of(roa::call("audio_play_sound", { snd, 1, 0 }));
        if (h >= 0 && roa::builtin("audio_sound_gain")) roa::call("audio_sound_gain", { h, 0.12, 0 });
    }
    if (stop > 0 && stop <= 20) {                                   // brief freeze, like a real hit
        iset(g_player, "old_hsp", iget(g_player, "hsp")); iset(g_player, "old_vsp", iget(g_player, "vsp"));
        iset(g_player, "hitpause", 1); iset(g_player, "hitstop", stop); iset(g_player, "hitstop_full", stop);
    }
    static int n = 0; if (n++ < 6) { char m[160]; sprintf_s(m, "manual hit effect: fx=%.0f sound=%.0f hitpause=%.0f at (%.0f,%.0f)", fx, snd, stop, x, y); roa::log(m); }
}

// Pizza Tower says an enemy was hit. With stand-in targets RoA normally registers the same hit itself (effects, sound, hitpause);
// if it did not (the stand-in lagged behind a fast enemy, or the shapes differ a little) fall back to the hand-made effect.
struct PendingHit { HitInfo hi; int frame; };
static std::vector<PendingHit> g_pending_hits;
static void on_hit_connected(double hb) {
    auto it = g_hit_info.find((uint32_t)hb);
    if (it == g_hit_info.end()) return;
    if (g_target_count > 0) { g_pending_hits.push_back({ it->second, g_frame }); return; }
    manual_hit_fx(it->second);
}
static void process_pending_hits() {
    for (size_t i = 0; i < g_pending_hits.size();) {
        if (g_frame - g_pending_hits[i].frame < 3) { ++i; continue; }
        bool real = iget(g_player, "hitstop") > 0.5 || iget(g_player, "hitpause") > 0.5;   // RoA already froze the attacker: the hit went through its own pipeline
        if (!real) manual_hit_fx(g_pending_hits[i].hi);
        g_pending_hits.erase(g_pending_hits.begin() + i);
    }
}

// A Pizza Tower enemy hit the puppet: add percent through RoA's own damage scripts and launch the player
// into hitstun (PS_HITSTUN = 12) in the requested direction.
static void apply_hurt(const PRHurt& h) {
    if (h.damage < 0) {   // Pizza Tower says our parry worked: no damage, but the freeze and flash of a real parry
        double stop = 14;
        iset(g_player, "old_hsp", iget(g_player, "hsp")); iset(g_player, "old_vsp", iget(g_player, "vsp"));
        iset(g_player, "hitpause", 1); iset(g_player, "hitstop", stop); iset(g_player, "hitstop_full", stop);
        iset(g_player, "invincible", 1); iset(g_player, "invince_time", 30);
        roa::log("parry confirmed by Pizza Tower");
        return;
    }
    double cur = roa::call_real("script_execute", { asset("get_player_damage"), kSlot });
    roa::call("script_execute", { asset("set_player_damage"), kSlot, cur + h.damage });
    double rad = h.kb_angle * 3.14159265 / 180.0;
    double power = h.kb_power * (1.0 + cur * 0.012);     // knockback grows with percent (about 2.2x at 100%)
    int frames = (int)(8 + power * 3);
    iset(g_player, "hsp", std::cos(rad) * power);
    iset(g_player, "vsp", -std::sin(rad) * power);
    iset(g_player, "hitstun", frames); iset(g_player, "hitstun_full", frames);
    iset(g_player, "state", 12); iset(g_player, "state_timer", 0);
    iset(g_player, "free", 1);
    iset(g_player, "hit_player", kSlot); iset(g_player, "hit_player_obj", g_player); iset(g_player, "last_player_hit_me", kSlot); iset(g_player, "last_player_id", g_player);
    iset(g_player, "spr_dir", h.from_dir >= 0 ? -1 : 1);       // face the attacker
    char m[160]; sprintf_s(m, "hurt applied: +%d%% (now %.0f), launch %.1f at %d deg, hitstun %d", h.damage, cur + h.damage, power, (int)h.kb_angle, frames);
    roa::log(m);
}

// per-frame tick (called from inside the game's own draw/step script context)
// ------------------------------------------------------------------------------------------
static void tick() {
    if (g_s && (g_frame % 180 == 7)) { DWORD hb = g_s->hb_pt; if (g_frame == 7 || GetTickCount() - hb > 4000) restore_own_volume(); }   // (the first minutes: PT takes over a moment later if it is there)
    if (!g_s) return;
    { static DWORD w0 = 0; static int n = 0; DWORD now = GetTickCount(); ++n; if (!w0) w0 = now; if (now - w0 >= 500) { g_tick_fps = n * 1000.0 / (now - w0); n = 0; w0 = now; } }
    g_s->hb_roa = GetTickCount();
    PREvent e;
    while (pr::ring_pop(g_s->to_roa, e)) {
        if (e.type == PR_EV_SELECT_CHARACTER) {
            std::string id((const char*)e.payload, strnlen((const char*)e.payload, PR_CHAR_ID_LEN));
            roa::log("select request: " + id + " (current " + g_char_id + ", ready=" + std::to_string(g_ready) + " booting=" + std::to_string(g_booting) + " pending=" + std::to_string(g_pending_char) + ")");
            if (id == g_char_id && (g_ready || g_booting || g_pending_char >= 0)) { if (g_ready) send_event(PR_EV_CHAR_READY, g_char_id); }   // already loaded (or loading): nothing to do
            else { g_req_id = id; g_ready = false; }      // resolved below once RoA's character list exists
        } else if (e.type == PR_EV_SET_OPTION) {
            PRCmd c; memcpy(&c, e.payload, sizeof c);
            if ((int)c.a == 6) { g_floor_frames = (int)std::floor(c.b * 60.0 + 0.5); if (g_floor_frames < 6) g_floor_frames = 6; roa::log("floor timer frames " + std::to_string(g_floor_frames)); }
            if ((int)c.a == 8 && g_player >= 0 && g_ready) { iset(g_player, "vsp", c.b); iset(g_player, "free", 1); }   // bounce / lift: set the vertical speed (airborne, no hitstun)
            if ((int)c.a == 9 && g_player >= 0 && g_ready) iset(g_player, "hsp", c.b);
            if ((int)c.a == 10 && g_player >= 0 && g_ready) iset(g_player, "x", iget(g_player, "x") + c.b);   // conveyor belts: push sideways
            if ((int)c.a == 15 && g_player >= 0 && g_ready) iset(g_player, "y", iget(g_player, "y") + c.b);   // moving platforms: ride along
            if ((int)c.a == 13 && g_player >= 0 && g_ready) { iset(g_player, "state", 12); iset(g_player, "state_timer", 0); iset(g_player, "hitstun", 6); iset(g_player, "hitstun_full", 6); iset(g_player, "free", 1); iset(g_player, "hsp", 0); iset(g_player, "vsp", 0); }   // held by a Pizza Tower hazard: the hurt pose
            if ((int)c.a == 14 && g_boot_char >= 0 && !g_css_mode && g_player >= 0 && g_ready) {   // the palette dresser: next colour of this character, applied on the spot
                double mx = roa::call_real("script_execute", { asset("get_max_available_colors"), (double)kSlot });
                int m = (mx >= 2 && mx <= 40) ? (int)mx : 8;
                int nc = (g_boot_color + 1) % m;
                roa::log("palette " + std::to_string(g_boot_color) + " -> " + std::to_string(nc) + " (max " + std::to_string(mx) + ")");
                g_ready = false; boot_match(g_boot_char, nc);
            }
            if ((int)c.a == 7) { g_hover_hud = c.b > 0.5; roa::log(std::string("hover hud ") + (g_hover_hud ? "on" : "off")); }
            if ((int)c.a == 5) { g_pt_always = c.b > 0.5; roa::log(std::string("PT kills on hit ") + (g_pt_always ? "on" : "off")); }
            if ((int)c.a == 4) { g_pt_kill_pct = c.b < 0 ? 0 : c.b; roa::log("PT kill percent " + std::to_string(g_pt_kill_pct)); }
            if ((int)c.a == 3) { g_enemy_pct = c.b < 0 ? 0 : (c.b > 300 ? 300 : c.b); roa::log("enemy percent " + std::to_string(g_enemy_pct)); }
            if ((int)c.a == 2) { bool want = c.b > 0.5; if (want != g_cpu_targets) { g_cpu_targets = want; roa::log(std::string("real targets ") + (want ? "on" : "off") + ": restarting the match"); if (g_boot_char >= 0 && !g_css_mode) { g_ready = false; boot_match(g_boot_char, g_boot_color); } } }
            if ((int)c.a == 1) { g_speed_mult = c.b < 1 ? 1 : (c.b > 5 ? 5 : c.b); roa::log("speed multiplier " + std::to_string(g_speed_mult)); if (g_ready) apply_speed(); }
        } else if (e.type == PR_EV_RESPAWN && !g_css_mode && g_boot_char >= 0) {   // "reset rival": start the match over (some characters want inputs right at the start)
            roa::log("reset rival: restarting the match");
            g_ready = false; boot_match(g_boot_char, g_boot_color);
        } else if (e.type == PR_EV_OPEN_CSS) {
            for (int s2 = 2; s2 <= 4; ++s2) { if (garr_get("player_select", s2) >= 21) garr_set("player_select", s2, 2); garr_set("player_connected", s2, 0); }
            gset("training_mode", 0);   // the select screen sets it again when Practice is chosen   // a workshop character left in a CPU slot crashes the select screen
            g_css_mode = true; g_ready = false; g_booting = false; g_pending_char = -1; g_player = -4;
            int rm = (int)gget("room", -1);
            if (rm != kRoomMenu) roa::call("room_goto", { (double)kRoomMenu });
            show_roa_window();
            send_event(PR_EV_CSS_STATE, "1");
        } else if (e.type == PR_EV_CANCEL_CSS && g_css_mode) {
            g_css_mode = false; send_event(PR_EV_CSS_STATE, "0");
            if (g_boot_char >= 0) boot_match(g_boot_char, g_boot_color);
        } else if (e.type == PR_EV_HIT_CONNECTED && g_player >= 0 && g_ready) {
            uint32_t hid; memcpy(&hid, e.payload, 4);
            on_hit_connected((double)hid);
        } else if (e.type == PR_EV_PLAYER_HURT && g_player >= 0 && g_ready) {
            PRHurt h; memcpy(&h, e.payload, sizeof h);
            apply_hurt(h);
        }
    }
    int room = (int)gget("room", -1);
    if (g_ready && g_frame % 30 == 7 && g_view_w > 0 && g_bb_w && (g_bb_w != (UINT)(g_view_w * 2 + 0.5) || g_bb_h != (UINT)(g_view_h * 2 + 0.5))) {
        // RoA re-applies its own window size now and then; anything but exactly 2x makes the capture blurry
        // (not while the game is still starting up: resizing the window then crashed the first match of some characters; its own resize usually comes first)
        static int bad = 0; ++bad;
        if (bad >= 1) {
            bad = 0;
            static int fixes = 0; if (fixes++ < 50) roa::log("backbuffer is " + std::to_string(g_bb_w) + "x" + std::to_string(g_bb_h) + ": forcing 2x");
            if (roa::builtin("window_set_fullscreen")) roa::call("window_set_fullscreen", { 0 });
            if (roa::builtin("window_set_size")) roa::call("window_set_size", { g_view_w * 2, g_view_h * 2 });
        }
    }
    { static int last_room = -99999; if (room != last_room) { last_room = room; roa::log("room -> " + std::to_string(room) + " " + roa::call_string("room_get_name", { (double)room }) + " stock_table=" + std::to_string(g_stock_index.size())); } }
    if (g_stock_index.empty() && roa::global_exists("charNames")) build_stock_table();
    {   // start by ourselves from the logo screen with the last used character, so RoA is ready long before Pizza Tower is
        static bool auto_done = false;
        if (!auto_done && room == 1 && !g_stock_index.empty() && g_char_id.empty() && g_req_id.empty() && g_pending_char < 0) {
            auto_done = true;
            std::string id = "stock:zetterburn"; { std::ifstream lf("mods/pizzarivals_lastchar.txt"); std::string t; if (lf && std::getline(lf, t)) { while (!t.empty() && (t.back() == '' || t.back() == ' ')) t.pop_back(); if (!t.empty()) id = t; } }
            auto it = g_stock_index.find(id); if (it == g_stock_index.end()) it = g_stock_index.find("stock:zetterburn");
            if (it != g_stock_index.end()) { g_char_id = it->first; g_pending_char = it->second; roa::log("auto start from the logo screen: " + g_char_id); }
        }
    }
    if (!g_req_id.empty() && !g_stock_index.empty()) {
        auto it = g_stock_index.find(g_req_id);
        if (it == g_stock_index.end()) send_event(PR_EV_CHAR_FAILED, g_req_id + ": not available (workshop characters are not supported yet)");
        else { g_char_id = g_req_id; g_css_name.clear(); g_pending_char = it->second; }
        g_req_id.clear();
    }
    if (g_obj_oplayer < 0) {
        std::ifstream rf("mods/pizzarivals_room.txt"); std::string rn;
        if (rf && std::getline(rf, rn) && !rn.empty()) { while (!rn.empty() && (rn.back() == '' || rn.back() == ' ')) rn.pop_back(); g_match_room_name = rn; }
        g_match_room = (int)asset(g_match_room_name.c_str());
        roa::log("match room: " + g_match_room_name + " (" + std::to_string(g_match_room) + ")");
        g_obj_oplayer = asset("oPlayer"); g_obj_phitbox = asset("pHitBox");
        g_obj_solid = asset("solid_32_obj"); g_obj_plat = asset("jumpthrough_32_obj");
        g_obj_parblock = asset("par_block"); g_obj_parjump = asset("par_jumpthrough");
    }
    if (g_booting && room != g_match_room && room != 1 && g_boot_char >= 0 && g_frame > g_boot_frame + 30) {   // the logo room's own timer sent us to the menu: go again
        roa::log("boot was overridden by room " + std::to_string(room) + ": retrying"); boot_match(g_boot_char);
    }
    if (g_css_mode && g_frame % 90 == 0) { std::string o = "css slots:"; for (int s = 1; s <= 4; ++s) o += " P" + std::to_string(s) + "(sel=" + std::to_string((int)garr_get("player_select", s)) + " conn=" + std::to_string((int)garr_get("player_connected", s)) + " ai=" + std::to_string((int)garr_get("player_ai", s)) + ")"; o += " room=" + std::to_string(room); roa::log(o); }
    if (g_css_mode && room != g_match_room && room != 1 && room != kRoomMenu && g_obj_oplayer >= 0 && inst_count(g_obj_oplayer) > 0 && g_frame % 10 == 0) {
        // the player pressed Practice in RoA's own menus: take that character and skin over into our stage
        int sel = (int)garr_get("player_select", kSlot), col = (int)garr_get("player_color", kSlot);
        g_css_mode = false; g_css_name = char_name_of(sel);
        {   // the character's own name (workshop characters are all "MISSING TEXT" in the name table; get_char_info(player, INFO_STR_NAME = 4) knows)
            std::string nm = roa::call("script_execute", { asset("get_char_info"), kSlot, 4.0 }).toString();
            while (!nm.empty() && (nm.back() == '"' || nm.back() == ' ')) nm.pop_back(); while (!nm.empty() && (nm[0] == '"' || nm[0] == ' ')) nm.erase(0, 1);
            roa::log("character info name: " + nm);
            if (!nm.empty() && nm != "undefined" && nm.find("MISSING TEXT") == std::string::npos) g_css_name = nm;
        }
        g_char_id = "css:" + std::to_string(sel) + ":" + std::to_string(col);
        {   // a workshop character is known by its workshop id (the last folder of the path RoA loaded it from)
            std::string ps = roa::call("variable_global_array_get", { "player_ugc_path", kSlot }).toString(), digits;
            size_t e = ps.find_last_not_of("\\/\" ");
            while (e != std::string::npos && e < ps.size() && ps[e] >= '0' && ps[e] <= '9') { digits.insert(digits.begin(), ps[e]); if (e == 0) break; --e; }
            roa::log("workshop path: " + ps + " -> id " + digits);
            {   // what does RoA know about the character it just loaded? (names / paths / ids), for matching it in the music config
                for (const char* gn : { "player_ugc_path", "player_path", "player_workshop_sendvar", "player_workshop_random", "player_ugc", "player_ugc_id", "player_workshop_id", "player_workshop", "player_char_path", "player_workshop_path", "ugc_path", "workshop_id", "workshop_pick", "workshop_data", "workshop_chars" }) {
                    if (!roa::global_exists(gn)) continue;
                    for (int sl = 1; sl <= 4; ++sl) { RValue v = roa::call("variable_global_array_get", { gn, sl }); roa::log(std::string("css probe ") + gn + "[" + std::to_string(sl) + "] = " + v.toString()); }
                    roa::log(std::string("css probe ") + gn + " = " + roa::global_get(gn).toString());
                }
                double pl = -4; { int cnt = inst_count(g_obj_oplayer); for (int i = 0; i < cnt; ++i) { double id = inst_find(g_obj_oplayer, i); if ((int)iget(id, "player", -1) == kSlot) { pl = id; break; } } }
                roa::log("css probe player instance " + std::to_string(pl));
                RValue nm = roa::call("variable_instance_get_names", { pl });
                for (int i = 0; i < 600; ++i) {
                    unsigned char* c = array_cell(nm, 0, i); unsigned char bb[16];
                    if (!c || !peek(c, bb, 16)) break;
                    std::string s = cell_str(bb); if (s.size() > 1 && s[0] == '"') s = s.substr(1, s.size() - 2);
                    std::string l = s; for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
                    if (l.find("name") != std::string::npos || l.find("ugc") != std::string::npos || l.find("workshop") != std::string::npos || l.find("path") != std::string::npos || l.find("folder") != std::string::npos || l.find("author") != std::string::npos || l.find("char_") == 0)
                        roa::log("css probe player." + s + " = " + roa::call("variable_instance_get", { pl, s.c_str() }).toString());
                }
            }
            if (digits.size() >= 6) g_char_id = "workshop:" + digits;
        }
        roa::log("picked in RoA: " + g_css_name + " (index " + std::to_string(sel) + ", skin " + std::to_string(col) + ")");
        boot_match(sel, col);
    }
    if (g_pending_char >= 0 && (room == 1 || room == kRoomMenu || room == g_match_room)) { boot_match(g_pending_char); g_pending_char = -1; }
    if (room == g_match_room) {
        if (g_frame % 30 == 7) {
            remember_audio_settings();
            g_audio_changed = true;
            gset("display_names", g_hover_hud ? g_orig_names : 0);
            gset("sfx_volume", 1.0);
        }
        if (g_frame == 400 || g_frame == 401) {   // where does RoA keep the blast zone besides get_stage_data?
            static const char* names[] = { "blastzone_bottom", "blastzone_side", "blastzone_top", "blastzone_bottom_min", "blastzone_bottom_max", "blastzone_side_min", "blastzone_side_max", "blastzone_top_min", "blastzone_top_max" };
            std::string o = "blastzone vars:";
            for (const char* n : names) {
                if (roa::global_exists(n)) { RValue v = roa::global_get(n); o += std::string(" G.") + n + "=" + v.toString(); }
                for (const char* on : { "oPlayer", "obj_stage_main", "camera_obj", "obj_custom_stage" }) {
                    double ob = asset(on); if (ob < 0 || inst_count(ob) < 1) continue; double id = inst_find(ob, 0);
                    if (roa::call_real("variable_instance_exists", { id, n }) > 0.5) o += std::string(" ") + on + "." + n + "=" + roa::inst_get(id, n).toString();
                }
            }
            roa::log(o);
        }
        if (g_frame % 600 == 0) { char m[240]; sprintf_s(m, "census: instances=%.0f solid=%d platform=%d hit_fx=%d hitbox=%d player=(%.0f,%.0f) ox=%.0f oy=%.0f", gget("instance_count"), inst_count(g_obj_solid), inst_count(g_obj_plat), inst_count(asset("hit_fx_obj")), inst_count(g_obj_phitbox), iget(g_player, "x"), iget(g_player, "y"), g_ox, g_oy); roa::log(m); }
        if (g_frame % 60 == 0) { char m[200]; sprintf_s(m, "view: g_xview=%.0f g_yview=%.0f view_xview=%.0f view_yview=%.0f w=%.0f h=%.0f player=(%.0f,%.0f)", gget("g_xview"), gget("g_yview"), gget("view_xview"), gget("view_yview"), gget("g_wview"), gget("g_hview"), iget(g_player, "x"), iget(g_player, "y")); roa::log(m); }
        g_view_x = gget("g_xview"); g_view_y = gget("g_yview"); g_view_w = gget("g_wview", 960); g_view_h = gget("g_hview", 540);
        if (inst_count(g_obj_oplayer) > 0) {
            { double p1 = inst_find(g_obj_oplayer, 0); int n = inst_count(g_obj_oplayer); for (int i = 0; i < n; ++i) { double id = inst_find(g_obj_oplayer, i); if ((int)iget(id, "player", 0) == kSlot) { p1 = id; break; } } g_player = p1; }
            if (g_booting && g_frame > g_boot_frame + 40) {
                post_boot_setup();
                g_booting = false; g_ready = true;
                send_event(PR_EV_CHAR_READY, g_char_id + (g_css_name.empty() ? std::string() : "|" + g_css_name));
                send_event(PR_EV_CSS_STATE, "0");
                { std::ofstream lf("mods/pizzarivals_lastchar.txt"); lf << g_char_id << std::endl; }
                roa::log("match ready: " + g_char_id);
            }
            if (g_ready) {
                PRPtState pt; pr::slot_read(g_s->h_pt, g_s->pt, pt);
                g_recenter_on = (pt.flags & PR_PT_RECENTER) != 0;
                static bool last_reset = false;
                bool want_reset = (pt.flags & PR_PT_RESET_PERCENT) != 0;
                if (want_reset && !last_reset) { roa::call("script_execute", { asset("set_player_damage"), kSlot, 0 }); iset(g_player, "hitstun", 0); roa::log("percent reset (stage entered)"); }
                last_reset = want_reset;
                static uint8_t last_warp = 0;
                if (pt.warp != last_warp && !(pt.puppet_x == 0 && pt.puppet_y == 0)) { last_warp = pt.warp; g_warp_pending = true; }
                double st_now = iget(g_player, "state");
                if (g_warp_pending && st_now != 25 && st_now != 14) {   // not while RoA's spawn/respawn animation owns the position
                    // RoA undoes a teleport of its character (it snaps back), so never move him: re-derive the PT<->RoA offset so he *appears*
                    // at the puppet's position instead.
                    g_ox = std::floor(pt.puppet_x - iget(g_player, "x") + 0.5); g_oy = std::floor(pt.puppet_y - iget(g_player, "y") + 0.5);
                    iset(g_player, "vsp", 0);      // (the horizontal speed stays: a runner keeps running into the next room)
                    {   // keep him on the ground while the new room's solids are on their way
                        if (g_patch_block < 0) g_patch_block = make_block(g_obj_solid);
                        if (g_patch_block >= 0 && iget(g_player, "free", 1) < 0.5) { iset(g_patch_block, "image_xscale", 800.0 / 32.0); iset(g_patch_block, "image_yscale", 24.0 / 32.0); iset(g_patch_block, "x", std::floor(iget(g_player, "x") - 400)); iset(g_patch_block, "y", std::floor(iget(g_player, "y"))); }
                    }
                    g_world = pt.warp; g_solids_cache.count = 0; g_solid_rev_seen = 0xFFFFFFFFu; g_solids_ready = false; g_warp_frame = g_frame;      // the old room's solids are gone; the new ones come stamped with this world
                    g_solids_dirty = true; g_warp_pending = false; ++g_warp_gen; ++g_epoch;
                    roa::log("warp applied: PT (" + std::to_string(pt.puppet_x) + "," + std::to_string(pt.puppet_y) + ") offset (" + std::to_string(g_ox) + "," + std::to_string(g_oy) + ")");
                }
                if (!g_shift_pending && !g_warp_pending) {   // keep the scene far from RoA's left (and right) edge
                    double sx = iget(g_player, "x");
                    static bool noshift = GetFileAttributesA("mods/pizzarivals_noshift.txt") != INVALID_FILE_ATTRIBUTES;
                    bool win_ok = (g_bb_w == (UINT)(g_view_w * 2 + 0.5) && g_bb_h == (UINT)(g_view_h * 2 + 0.5)) || true;
                    if (!noshift && win_ok && (sx < kRoomW * 0.25 || sx > kRoomW * 0.75)) { g_shift_dx = kRoomW * 0.5 - sx; g_shift_pending = 1; }
                }
                {   // sound: silent while Pizza Tower is not in Rivals mode
                    static bool muted = false; bool want = (pt.flags & PR_PT_PAUSED) != 0;
                    if (want != muted) { muted = want; if (roa::builtin("audio_master_gain")) roa::call("audio_master_gain", { muted ? 0.0 : 1.0 }); }
                }
                if (pt.flags & PR_PT_PAUSED) {
                    double stn = iget(g_player, "state");
                    if (stn != 14 && stn != 25) { iset(g_player, "attack", 0); iset(g_player, "state", 1); iset(g_player, "state_timer", 0); iset(g_player, "hitpause", 0); iset(g_player, "hitstop", 0); }
                    iset(g_player, "hsp", 0); iset(g_player, "vsp", 0);
                    roa::call("audio_stop_all", {});
                }
                if (g_frame % 30 == 0) run_dev_file();
                if (g_watchp > 0) { --g_watchp; double wid = inst_find(g_obj_oplayer, g_watchp_idx); char wm[200]; sprintf_s(wm, "watchp f=%d x=%.1f y=%.1f xprev=%.1f state=%.0f hsp=%.1f vsp=%.1f free=%.0f", g_frame, iget(wid, "x"), iget(wid, "y"), iget(wid, "xprevious"), iget(wid, "state"), iget(wid, "hsp"), iget(wid, "vsp"), iget(wid, "free", -1)); roa::log(wm); }
                if (g_watch > 0) { --g_watch; char m[200]; sprintf_s(m, "watch: x=%.1f y=%.1f xprev=%.1f hsp=%.2f vsp=%.2f state=%.0f", iget(g_player, "x"), iget(g_player, "y"), iget(g_player, "xprevious"), iget(g_player, "hsp"), iget(g_player, "vsp"), iget(g_player, "state")); roa::log(m); }
                {   // Controller jumps ran two jumpsquats back to back (28 -> 3 -> 28 -> 3) while keyboard-injected ones ran one. The press was
                    // evidently still counted as fresh one frame after the jump started, so consume it as soon as the jump begins.
                    static double ps = -1; static int since = 1000; double st = iget(g_player, "state");
                    if (ps == 28 && st == 3) since = 0; else if (since < 1000) ++since;
                    if (ps == 28 && st == 3 && g_pad_active) g_jump_guard = 8;
                    if (since <= 12 || st == 28) {
                        static int n = 0; if (n++ < 400) { char m[260]; sprintf_s(m, "jumpwatch f=%d st=%.0f since=%d jump_pressed=%.0f jump_counter=%.0f jump_down=%.0f up_pressed=%.0f free=%.0f y=%.1f vsp=%.1f", g_frame, st, since, iget(g_player, "jump_pressed"), iget(g_player, "jump_counter", -1), iget(g_player, "jump_down"), iget(g_player, "up_pressed", -1), iget(g_player, "free", -1), iget(g_player, "y"), iget(g_player, "vsp")); roa::log(m); }
                    }
                    ps = st;
                }
                {   // trace state transitions (helps find what kills the player)
                    static double last_state = -1;
                    double st = iget(g_player, "state");
                    if (st != last_state) {
                        char m[300]; sprintf_s(m, "state %.0f -> %.0f at (%.0f,%.0f) vel(%.1f,%.1f) dying=%.0f dead_timer=%.0f spawn_timer=%.0f free=%.0f", last_state, st, iget(g_player, "x"), iget(g_player, "y"), iget(g_player, "hsp"), iget(g_player, "vsp"), iget(g_player, "dying"), iget(g_player, "dead_timer"), iget(g_player, "spawn_timer"), iget(g_player, "free"));
                        roa::log(m); last_state = st;
                        if (st == 13 || st == 28 || st == 19) {   // landing / jumpsquat: what is under the feet? (hunting the repeated-jump bug)
                            double px = iget(g_player, "x"), py = iget(g_player, "y");
                            double blk = roa::call_real("collision_rectangle", { px - 16, py, px + 16, py + 3, g_obj_parblock, 0, 1 });
                            double jt = roa::call_real("collision_rectangle", { px - 16, py, px + 16, py + 3, g_obj_parjump, 0, 1 });
                            char m2[300]; sprintf_s(m2, "  frame=%d feet: block=%.0f (%.0f,%.0f %.2fx%.2f) platform=%.0f (%.0f,%.0f) djumps=%.0f jump_down=%.0f", g_frame, blk, blk >= 0 ? iget(blk, "x") : 0, blk >= 0 ? iget(blk, "y") : 0, blk >= 0 ? iget(blk, "image_xscale") : 0, blk >= 0 ? iget(blk, "image_yscale") : 0, jt, jt >= 0 ? iget(jt, "x") : 0, jt >= 0 ? iget(jt, "y") : 0, iget(g_player, "djumps"), iget(g_player, "jump_down"));
                            roa::log(m2);
                        }
                    }
                }
                if (g_frame % 6 == 0 && GetFileAttributesA("mods/pizzarivals_dbg_articles.txt") != INVALID_FILE_ATTRIBUTES) {
                    // log every object type whose instance count changed (shows what a special spawns and when it vanishes)
                    static std::map<std::string, int> seen; static bool first = true;
                    std::map<std::string, int> now;
                    int n = (int)roa::call_real("instance_number", { -3 });
                    for (int i = 0; i < n; ++i) { double id = roa::call_real("instance_find", { -3, i }); now[roa::call_string("object_get_name", { iget(id, "object_index", -1) })]++; }
                    if (!first) for (auto& kv : now) { int old = seen.count(kv.first) ? seen[kv.first] : 0; if (kv.second != old && kv.first.find("solid") == std::string::npos && kv.first.find("jumpthrough") == std::string::npos) roa::log("obj " + kv.first + " " + std::to_string(old) + " -> " + std::to_string(kv.second) + " @frame " + std::to_string(g_frame)); }
                    if (!first) for (auto& kv : seen) if (!now.count(kv.first) && kv.first.find("solid") == std::string::npos) roa::log("obj " + kv.first + " " + std::to_string(kv.second) + " -> 0 @frame " + std::to_string(g_frame));
                    seen = now; first = false;
                }
                if (g_frame % 60 == 0) {
                    mute_music();
                    if (GetFileAttributesA("mods/pizzarivals_dbg_articles.txt") != INVALID_FILE_ATTRIBUTES) {
                        char m[200]; sprintf_s(m, "articles: a1=%d a2=%d a3=%d hitboxes=%d hurtboxes=%d fx=%d", inst_count(asset("obj_article1")), inst_count(asset("obj_article2")), inst_count(asset("obj_article3")), inst_count(g_obj_phitbox), inst_count(asset("pHurtBox")), inst_count(asset("hit_fx_obj")));
                        roa::log(m);
                    }
                }
                {   // dust effects index per-player colour arrays; a dust object owned by player -1 crashes RoA when drawn
                    static double dust = -2; if (dust == -2) dust = asset("new_dust_fx_obj");
                    int nd = dust >= 0 ? inst_count(dust) : 0;
                    static std::vector<double> dust_seen;
                    if (GetFileAttributesA("mods/pizzarivals_dbg_dust.txt") != INVALID_FILE_ATTRIBUTES) {
                        for (int i = 0; i < nd; ++i) {
                            double id = inst_find(dust, i);
                            if (std::find(dust_seen.begin(), dust_seen.end(), id) != dust_seen.end()) continue;
                            dust_seen.push_back(id);
                            std::ifstream cf("mods/pizzarivals_candidates.txt"); std::string n, out;
                            while (std::getline(cf, n)) {
                                if (n.empty() || n.rfind("argument", 0) == 0) continue;
                                if (roa::call_real("variable_instance_exists", { id, n.c_str() }) < 0.5) continue;
                                RValue v = roa::inst_get(id, n.c_str());
                                if (n.find("background") == 0 || n.find("view_") == 0 || n.find("os_") == 0 || n.find("current_") == 0 || n.find("game_") == 0 || n.find("display_") == 0 || n.find("browser") == 0 || n.find("window_") == 0) continue;
                                out += n + "=" + (v.type == GML_TYPE_STRING ? std::string("\"") + v.getString() + "\"" : (v.type == GML_TYPE_ARRAY ? std::string("<array>") : v.toString())) + " ";
                            }
                            roa::log("dust " + std::to_string((int)id) + " vars == -1: " + out);
                        }
                    }
                    for (int i = 0; i < nd; ++i) {
                        double id = inst_find(dust, i);
                        double pl = iget(id, "player", 1);
                        if (pl >= 2 && pl <= 4) iset(id, "visible", 0);
                        if (pl < 1 || pl > 4) { iset(id, "player", 1); static int logged = 0; if (logged++ < 10) roa::log("dust sanitized: player was " + std::to_string(pl)); }
                    }
                }
                {   // dodging squeezes through gaps one tile (32px) high: collide with the (trimmed) crouch mask, but only while dodging
                    // (plus a short grace period after it ends if the full-height body would not fit where the dodge stopped)
                    static double msk = -1, trimmed = -1; static bool isshort = false; static int grace = 0;
                    double st = iget(g_player, "state");
                    bool dodge = (st == 8 || st == 33 || st == 34);
                    {   // a dodge (or the frames after one) never moves the character far in a single frame: that is the engine shoving it out of a wall
                        static double ppx = 0, ppy = 0, phs = 0, pvs = 0; static int lastd = -100, pf = -1; static uint32_t pgen = 0;
                        double cx = iget(g_player, "x"), cy = iget(g_player, "y");
                        if (dodge || isshort) lastd = g_frame;
                        bool far_jump = g_frame > g_boot_frame + 600 && iget(g_player, "attack") == 0 && iget(g_player, "hitstun") <= 0 && (std::fabs(cx - ppx) > 220 || std::fabs(cy - ppy) > 220);   // (an attack may teleport; nothing else moves a character that far in one frame)
                        if (pf == g_frame - 1 && pgen == g_warp_gen + g_epoch && !g_warp_pending && ((g_frame - lastd <= 10 && (std::fabs(cx - ppx) > 80 || std::fabs(cy - ppy) > 80)) || far_jump)) {
                            static int ul = 0; if (ul++ < 40) roa::log("dodge jump undone: " + std::to_string(cx - ppx) + "," + std::to_string(cy - ppy));
                            iset(g_player, "x", ppx); iset(g_player, "y", ppy); iset(g_player, "xprevious", ppx); iset(g_player, "yprevious", ppy);
                            iset(g_player, "hsp", phs); iset(g_player, "vsp", pvs);        // (the speed he had stays: the shove only moved him)
                        } else { ppx = cx; ppy = cy; phs = iget(g_player, "hsp"); pvs = iget(g_player, "vsp"); }
                        pf = g_frame; pgen = g_warp_gen + g_epoch;
                    }
                    double cm = iget(g_player, "crouchbox_spr", -1);
                    bool nocrouch = cm < 0;
                    { static std::string last; std::string key = std::to_string(cm) + "/" + std::to_string(st); if (dodge && key != last) { last = key; roa::log("dodge: state=" + std::to_string(st) + " crouchbox=" + std::to_string(iget(g_player, "crouchbox_spr", -1)) + " hurtbox=" + std::to_string(iget(g_player, "hurtbox_spr", -1)) + " mask=" + std::to_string(iget(g_player, "mask_index", -1))); } }
                    if (dodge) { static int gl = 0; static double lastcm = -9; if (cm != lastcm) { lastcm = cm; gl = 0; } if (gl < 30 && g_frame % 2 == 0) { ++gl; double dpx = iget(g_player, "x"), dpy = iget(g_player, "y"); roa::log("dodge gap check: char=" + std::to_string(g_boot_char) + " state=" + std::to_string(st) + " blocked=" + std::to_string((int)gap_blocked(dpx, dpy, iget(g_player, "hsp"))) + " isshort=" + std::to_string((int)isshort) + " mask=" + std::to_string(iget(g_player, "mask_index", -1)) + " yscale=" + std::to_string(iget(g_player, "image_yscale", 1)) + " crouchbox=" + std::to_string(cm) + " invincible=" + std::to_string(iget(g_player, "invincible")) + " invince_time=" + std::to_string(iget(g_player, "invince_time")) + " intangible=" + std::to_string(iget(g_player, "intangible")) + " st_timer=" + std::to_string(iget(g_player, "state_timer")) + " window=" + std::to_string(iget(g_player, "window"))); } }
                    auto above = [&](double m) { return roa::call_real("sprite_get_yoffset", { m }) - roa::call_real("sprite_get_bbox_top", { m }); };      // how far the mask reaches above the origin (the feet)
                    auto crouch_ys = [&]() { double h = above(cm); static double lh = -1; if (h != lh) { lh = h; roa::log("crouch mask reaches " + std::to_string(h) + " above the origin (sprite " + std::to_string(cm) + ", height " + std::to_string(roa::call_real("sprite_get_bbox_bottom", { cm }) - roa::call_real("sprite_get_bbox_top", { cm }) + 1) + ")"); } return (h > 29.5) ? 29.0 / h : 0.8; };
                    bool air = (st == 8);
                    auto squash_air = [&]() { double m = iget(g_player, "mask_index", -1); if (m < 0) return;
                        double xo = roa::call_real("sprite_get_xoffset", { m });
                        double wext = std::max(std::fabs(roa::call_real("sprite_get_bbox_left", { m }) - xo), std::fabs(roa::call_real("sprite_get_bbox_right", { m }) + 1 - xo));
                        iset(g_player, "image_xscale", wext > 14 ? 14.0 / wext : 1.0); };
                    auto restore = [&]() { iset(g_player, "image_xscale", 1); { static int rl = 0; if (rl++ < 20) roa::log("dodge mask restore: saved=" + std::to_string(msk) + " current=" + std::to_string(iget(g_player, "mask_index", -1)) + " state=" + std::to_string(iget(g_player, "state")) + " y=" + std::to_string(iget(g_player, "y"))); } if (msk >= 0) iset(g_player, "mask_index", msk); iset(g_player, "image_yscale", 1); isshort = false; grace = 0; };
                    if (nocrouch) {   // no crouch box: squash the normal mask to a 30px height while dodging through a gap
                        double px = iget(g_player, "x"), py = iget(g_player, "y"), hs = iget(g_player, "hsp");
                        double ahead = hs * 2 + (hs > 0 ? 4 : (hs < 0 ? -4 : 0));
                        double x1 = px - 12 + (ahead < 0 ? ahead : 0), x2 = px + 12 + (ahead > 0 ? ahead : 0);
                        bool blocked = (dodge || isshort) && (gap_blocked(px, py, hs) || (dodge && air));
                        auto squash = [&]() { double m = iget(g_player, "mask_index", -1); double mh = m >= 0 ? above(m) : 0; { static double lm = -1; if (m != lm) { lm = m; roa::log("dodge mask " + std::to_string(m) + " reaches " + std::to_string(mh) + " above the origin, bottom " + std::to_string(m >= 0 ? roa::call_real("sprite_get_bbox_bottom", { m }) - roa::call_real("sprite_get_yoffset", { m }) : 0) + " below it"); } } iset(g_player, "image_yscale", mh > 29.5 ? 29.0 / mh : 1.0); };
                        if (dodge && blocked) { if (!isshort) { msk = iget(g_player, "mask_index", -1); isshort = true; } grace = 40; squash(); if (air) squash_air(); }
                        else if (isshort && !dodge && (blocked || body_blocked(px, py))) squash();
                        else if (isshort) restore();
                    } else if (cm >= 0) {
                        double px = iget(g_player, "x"), py = iget(g_player, "y"), hs = iget(g_player, "hsp");
                        double ahead = hs * 2 + (hs > 0 ? 4 : (hs < 0 ? -4 : 0));
                        double x1 = px - 12 + (ahead < 0 ? ahead : 0), x2 = px + 12 + (ahead > 0 ? ahead : 0);
                        bool blocked = (dodge || isshort) && (gap_blocked(px, py, hs) || (dodge && air));
                        if (dodge && blocked) {
                            if (!isshort) {
                                msk = iget(g_player, "mask_index", -1);
                                if (trimmed != cm) {      // the crouch mask is 36px tall: cut it to 30px so it fits a 32px gap
                                    double l = roa::call_real("sprite_get_bbox_left", { cm }), r = roa::call_real("sprite_get_bbox_right", { cm }), bt = roa::call_real("sprite_get_bbox_bottom", { cm });
                                    roa::call("sprite_collision_mask", { cm, 0, 2, l, bt - 29, r, bt, 1, 0 }); trimmed = cm;
                                }
                                isshort = true;
                            }
                            grace = 40;
                            iset(g_player, "mask_index", cm); iset(g_player, "image_yscale", crouch_ys()); if (air) squash_air();
                        } else if (isshort && !dodge && (blocked || body_blocked(px, py))) {
                            iset(g_player, "mask_index", cm); iset(g_player, "image_yscale", crouch_ys());      // still inside the gap: let him walk out
                        } else if (isshort) restore();
                    } else if (isshort) restore();
                }
                if (g_frame % 4 == 0 && GetFileAttributesA("mods/pizzarivals_wallwatch.txt") != INVALID_FILE_ATTRIBUTES) {
                    std::string o = "wallwatch st=" + std::to_string((int)iget(g_player, "state")) + " ";
                    for (const char* v : { "can_wall_jump", "can_wall_cling", "can_wall_tech", "has_walljump", "wall_frames", "wall_jump_timer", "djumps", "max_djumps", "free", "jump_pressed", "jump_down", "vsp", "hsp", "state_timer" })
                        o += std::string(v) + "=" + std::to_string(iget(g_player, v, -99)) + " ";
                    roa::log(o);
                }
                if (iget(g_player, "state") != 16) { iset(g_player, "can_wall_jump", 1); iset(g_player, "has_walljump", 1); iset(g_player, "wall_jump_timer", 0); }   // climb walls forever, like Peppino
                {   // diagnostic: a fast move that stops dead (wall cancel?) -> log which block is in front of the character
                    static double prev_hsp = 0; static int logged = 0;
                    double hs = iget(g_player, "hsp");
                    if (std::fabs(prev_hsp) > 6 && std::fabs(hs) < 1 && logged < 12) {
                        double dir = prev_hsp > 0 ? 1 : -1, px = iget(g_player, "x"), py = iget(g_player, "y");
                        double blk = roa::call_real("collision_rectangle", { px + dir * 14, py - 60, px + dir * 40, py - 4, g_obj_parblock, 0, 1 });
                        char m[300];
                        if (blk >= 0) sprintf_s(m, "STOPPED DEAD: state=%.0f attack=%.0f; block ahead id=%.0f obj=%s x=%.0f y=%.0f scale=%.2fx%.2f bbox=(%.0f,%.0f)-(%.0f,%.0f) player=(%.0f,%.0f)", iget(g_player, "state"), iget(g_player, "attack"), blk, roa::call_string("object_get_name", { iget(blk, "object_index") }).c_str(), iget(blk, "x"), iget(blk, "y"), iget(blk, "image_xscale"), iget(blk, "image_yscale"), iget(blk, "bbox_left"), iget(blk, "bbox_top"), iget(blk, "bbox_right"), iget(blk, "bbox_bottom"), px, py);
                        else sprintf_s(m, "STOPPED DEAD: state=%.0f attack=%.0f; no block ahead (player=(%.0f,%.0f))", iget(g_player, "state"), iget(g_player, "attack"), px, py);
                        roa::log(m); ++logged;
                    }
                    prev_hsp = hs;
                }
                speed_displace_step();
                sync_solids();
                sync_targets();
                process_pending_hits();
                update_slope_block(); update_cpu_slopes(); step_up_assist();
                apply_room_size();
                apply_speed();
                if (g_frame % 120 == 5 && !g_css_mode) enforce_hidden(false);
                recenter();
                export_player();
                export_hitboxes();
            }
        }
    }
}

// ------------------------------------------------------------------------------------------
// frame capture (Present callback: D3D only, no GML)
// ------------------------------------------------------------------------------------------
static ID3D11Texture2D* g_staging = nullptr;
static UINT g_sw = 0, g_sh = 0; static DXGI_FORMAT g_sfmt = DXGI_FORMAT_UNKNOWN;

static void capture(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!g_s || !g_ready || !g_cap_valid || !sc || !dev || !ctx) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    if (d.SampleDesc.Count != 1) { bb->Release(); return; }
    if (!g_staging || g_sw != d.Width || g_sh != d.Height || g_sfmt != d.Format) {
        if (g_staging) { g_staging->Release(); g_staging = nullptr; }
        D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &g_staging))) { bb->Release(); return; }
        g_sw = d.Width; g_sh = d.Height; g_sfmt = d.Format;
    }
    { static UINT lw = 0; if (lw != d.Width) { lw = d.Width; char m[100]; sprintf_s(m, "backbuffer %ux%u scale %.3f", d.Width, d.Height, d.Width / (g_view_w > 0 ? g_view_w : 960.0)); roa::log(m); } }
    g_bb_w = d.Width; g_bb_h = d.Height;
    ctx->CopyResource(g_staging, bb);
    bb->Release();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    bool bgra = (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    int n = (g_frame_out + 1) & 1;
    uint8_t* out = (uint8_t*)g_s + PR_PIXELS_OFFSET(n);
    double scale = g_view_w > 0 ? (double)d.Width / g_view_w : 1.0;
    // Sample relative to where the sprite was actually rasterised. RoA draws the character at a fractional position, so
    // its 2x2 pixel blocks land on odd or even backbuffer pixels depending on the camera phase; sampling on a fixed grid
    // flips between block halves while moving (1px jitter + haze). Anchor the grid on the character's own screen pixel.
    int ss = (int)(scale + 0.5); if (ss < 1) ss = 1;
    int baseX = (int)std::floor((g_cap_xf - g_view_x) * scale + 0.5);
    int baseY = (int)std::floor((g_cap_yf - g_view_y) * scale + 0.5);
    int offY = (int)(g_cap_cy - std::floor(g_cap_yf + 0.5));
    for (int oy = 0; oy < PR_FRAME_H; ++oy) {
        int sy = baseY + ss * (oy - PR_FRAME_H / 2 + offY);
        for (int ox = 0; ox < PR_FRAME_W; ++ox) {
            int sx = baseX + ss * (ox - PR_FRAME_W / 2);
            uint8_t* o = out + (oy * PR_FRAME_W + ox) * 4;
            if (sx < 0 || sy < 0 || sx >= (int)d.Width || sy >= (int)d.Height) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            const uint8_t* p = (const uint8_t*)m.pData + sy * m.RowPitch + sx * 4;
            uint8_t r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
            bool key = r > 235 && g < 24 && b > 235;
            o[0] = key ? 0 : r; o[1] = key ? 0 : g; o[2] = key ? 0 : b; o[3] = key ? 0 : 255;
        }
    }
    {   // diagnostics: how much of the frame is opaque (0 => the crop missed the character)
        static int cnt = 0;
        if (++cnt % 180 == 1) {
            int opq = 0; for (int i = 3; i < PR_FRAME_W * PR_FRAME_H * 4; i += 4) opq += out[i] ? 1 : 0;
            char mm[220]; sprintf_s(mm, "capture: opaque=%d centre=(%.0f,%.0f) view=(%.0f,%.0f %.0fx%.0f) bb=%ux%u", opq, g_cap_cx, g_cap_cy, g_view_x, g_view_y, g_view_w, g_view_h, d.Width, d.Height);
            roa::log(mm);
        }
    }
    ctx->Unmap(g_staging, 0);
    g_s->frame[n].cx = (float)(g_cap_cx + g_ox); g_s->frame[n].cy = (float)(g_cap_cy + g_oy);
    g_s->frame[n].dx = (float)(g_cap_xf - std::floor(g_cap_xf + 0.5)); g_s->frame[n].dy = (float)(g_cap_yf - std::floor(g_cap_yf + 0.5));
    g_s->frame[n].px = (float)(g_cap_xf + g_ox); g_s->frame[n].py = (float)(g_cap_yf + g_oy);
    g_s->frame[n].gen = g_warp_gen;
    g_s->frame[n].frame_id = ++g_frame_out;
    g_s->frame[n].ready = 1;
}

static void on_present(ID3D11RenderTargetView*, IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    ++g_frame;
    {   // how fast does RoA really run? (it must stay at 60 or the character moves too fast/slow in Pizza Tower)
        static DWORD t0 = 0; static int f0 = 0;
        DWORD now = GetTickCount();
        if (!t0) { t0 = now; f0 = g_frame; }
        { static DWORD w0 = 0; static int wf = 0; if (!w0) { w0 = now; wf = g_frame; } if (now - w0 >= 500) { g_present_fps = (g_frame - wf) * 1000.0 / (now - w0); w0 = now; wf = g_frame; } }
        if (now - t0 >= 5000) { char m[100]; sprintf_s(m, "roa frame rate: %.1f fps", (g_frame - f0) * 1000.0 / (now - t0)); roa::log(m); t0 = now; f0 = g_frame; }
    }
    InterlockedExchange(&g_tick_pending, 1);
    capture(sc, dev, ctx);
}

// ------------------------------------------------------------------------------------------
// hooks / boot
// ------------------------------------------------------------------------------------------
static void* g_floor_orig = nullptr;
static void floor_detour(RValue& out, CInstance* s, CInstance* o, int argc, RValue* args) {
    ((roa::FnA)g_floor_orig)(out, s, o, argc, args);
    if (InterlockedExchange(&g_tick_pending, 0)) tick();
}

static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    static LONG n = 0; if (InterlockedIncrement(&n) > 20) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m = nullptr; char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &m);
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    char buf[400]; const char* base = strrchr(path, '\\');
    sprintf_s(buf, "EXCEPTION 0x%X at %s+0x%X frame=%d guarded=%d", code, base ? base + 1 : path, (unsigned)((char*)ep->ExceptionRecord->ExceptionAddress - (char*)m), g_frame, (int)roa::g_faulted);
    roa::log(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI boot(LPVOID) {
    AddVectoredExceptionHandler(1, crash_logger);
    for (int i = 0; i < 600 && !roa::init(); ++i) Sleep(100);
    if (!roa::init()) { roa::log("bridge: loader init failed"); return 0; }
    g_s = pr::open_shared();
    if (!g_s) { roa::log("bridge: shared memory failed"); return 0; }
    g_s->pid_roa = GetCurrentProcessId();
    void* fl = roa::builtin("floor");
    if (!fl || roa::hook(fl, (void*)&floor_detour, &g_floor_orig) != 0) { roa::log("bridge: floor hook failed"); return 0; }
    void* cu = (char*)GetModuleHandleA(nullptr) + kControlsUpdateRva;
    if (roa::hook(cu, (void*)&controls_update_detour, (void**)&g_controls_update_orig) != 0) { roa::log("bridge: controls_update hook failed"); return 0; }
    void* ci = (char*)GetModuleHandleA(nullptr) + kControlsIntakeRva;
    if (roa::hook(ci, (void*)&controls_intake_detour, (void**)&g_controls_intake_orig) != 0) roa::log("bridge: controls_intake hook failed (continuing)");
    void* pc = (char*)GetModuleHandleA(nullptr) + kPlayerControlsUpdateRva;
    if (roa::hook(pc, (void*)&player_controls_update_detour, (void**)&g_pcu_orig) != 0) roa::log("bridge: player_controls_update hook failed (continuing)");
    void* cam = (char*)GetModuleHandleA(nullptr) + kCameraStep2Rva;
    if (roa::hook(cam, (void*)&camera_step2_detour, (void**)&g_cam_orig) != 0) roa::log("bridge: camera hook failed (continuing)");
    void* sd = (char*)GetModuleHandleA(nullptr) + kGetStageDataRva;
    if (roa::hook(sd, (void*)&stage_data_detour, (void**)&g_stage_data_orig) != 0) roa::log("bridge: get_stage_data hook failed (continuing)");
    void* du = (char*)GetModuleHandleA(nullptr) + kDeathUpdateRva;
    if (roa::hook(du, (void*)&death_update_detour, (void**)&g_death_update_orig) != 0) roa::log("bridge: death_update hook failed (continuing)");
    void* ko = (char*)GetModuleHandleA(nullptr) + kPlayerKoRva;
    if (roa::hook(ko, (void*)&ko_detour, (void**)&g_ko_orig) != 0) roa::log("bridge: KO hook failed (continuing)");
    void* sp = (char*)GetModuleHandleA(nullptr) + 0x94EA00;       // gml_Script_save_player_profiles (Ghidra 0x00D4EA00 - image base 0x400000)
    if (roa::hook(sp, (void*)&save_profiles_detour, (void**)&g_save_profiles_orig) != 0) roa::log("bridge: save_player_profiles hook failed (continuing)");
    roa::add_present_callback(&on_present);
    roa::log("bridge: ready");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, boot, nullptr, 0, nullptr));
    }
    return TRUE;
}
