// x64 DLL loaded by the modded Pizza Tower with external_define(). Everything is doubles /
// C strings / raw pointers (a GML buffer's address) so GML can call it without wrappers.
#include "../../protocol/pr_shm.h"
#include <string>
#include "audio_loopback.h"
#include "music_extract.h"
#pragma comment(lib, "user32.lib")

static PRShared* g_s = nullptr;
static PREvent   g_ev;                       // last event popped by pr_event_poll
static std::string g_str;                    // scratch for string returns
static PRPlayerState g_ps;                   // snapshot used by pr_player_get
static PRHitboxSet   g_hb;                   // snapshot used by pr_hitbox_*
static PRSolidSet    g_solids_stage;         // built by pr_solid_add, published by pr_solids_commit
static uint32_t      g_solid_rev = 0;
static uint32_t      g_pixel_frame = 0;
static float         g_frame_meta[7] = {};

#define EXPORT extern "C" __declspec(dllexport)

// ---- RoA's sound, played by this process (so one application carries both games' audio) ----
static praudio::Bridge g_audio;
static DWORD g_audio_pid = 0;
EXPORT double pr_audio_start() {
    if (!g_s || !g_s->pid_roa) return 0;
    if (g_audio.run && g_audio_pid != g_s->pid_roa) g_audio.stop();
    if (!g_audio.run) { g_audio_pid = g_s->pid_roa; g_audio.start(g_audio_pid); }
    return g_audio.state.load();
}
EXPORT double pr_audio_gain(double g) { g_audio.gain = (float)g; return 1; }
EXPORT double pr_audio_stop() { g_audio.stop(); return 1; }
// songs from the player's own copy of Rivals of Aether, exported in the background (see music_extract.h)
EXPORT double pr_music_extract(const char* hint, const char* outdir, const char* csv) { musicx::start(hint ? hint : "", (g_s && g_s->pid_roa) ? (DWORD)g_s->pid_roa : 0, outdir ? outdir : "", csv ? csv : ""); return 1; }
// the game's file functions cannot reach outside its own folders: external .ogg songs are copied in first
EXPORT double pr_file_copy(const char* src, const char* dst) {
    std::string d = dst ? dst : ""; size_t sl = d.find_last_of("\\/");
    if (sl != std::string::npos) CreateDirectoryA(d.substr(0, sl).c_str(), nullptr);
    return CopyFileA(src ? src : "", dst ? dst : "", FALSE) ? 1 : 0;
}
EXPORT double pr_music_state() { return musicx::g_state.load(); }
EXPORT double pr_music_written() { return musicx::g_written.load(); }

// Gives this process's window the keyboard / controller focus back (RoA's window had it for a moment).
static BOOL CALLBACK find_wnd(HWND h, LPARAM lp) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}
EXPORT double pr_focus_window() {
    HWND h = nullptr; EnumWindows(find_wnd, (LPARAM)&h);
    if (!h) return 0;
    keybd_event(VK_MENU, 0, 0, 0); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);   // lets SetForegroundWindow through
    SetForegroundWindow(h);
    return 1;
}


EXPORT double pr_open() {
    if (g_s) return 1;
    g_s = pr::open_shared();
    if (g_s) g_s->pid_pt = GetCurrentProcessId();
    return g_s ? 1 : 0;
}
EXPORT double pr_close() { g_s = nullptr; return 1; }   // mapping lives until process exit

// 1 while the RoA bridge has beaten its heartbeat within 2 s.
EXPORT double pr_peer_alive() {
    if (!g_s || !g_s->pid_roa) return 0;
    return (GetTickCount() - g_s->hb_roa) < 2000 ? 1 : 0;
}
EXPORT double pr_heartbeat() { if (g_s) g_s->hb_pt = GetTickCount(); return 1; }

// ---- PT -> RoA ----
EXPORT double pr_send_input(double frame, double buttons, double sx, double sy) {
    if (!g_s) return 0;
    PRInput i{}; i.frame_id = (uint32_t)frame; i.buttons = (uint16_t)buttons; i.stick_x = (int8_t)sx; i.stick_y = (int8_t)sy;
    pr::slot_write(g_s->h_input, g_s->input, i);
    return 1;
}
EXPORT double pr_send_state(double frame, double flags, double room, double camx, double camy, double px, double py, double warp) {
    if (!g_s) return 0;
    PRPtState p{}; p.frame_id = (uint32_t)frame; p.flags = (uint32_t)flags; p.room_id = (int32_t)room;
    p.cam_x = (float)camx; p.cam_y = (float)camy; p.puppet_x = (float)px; p.puppet_y = (float)py; p.warp = (uint8_t)warp;
    pr::slot_write(g_s->h_pt, g_s->pt, p);
    return 1;
}
EXPORT double pr_solids_begin() { g_solids_stage.count = 0; return 1; }
EXPORT double pr_solid_add(double x, double y, double w, double h, double kind) {
    if (g_solids_stage.count >= PR_MAX_SOLIDS) return 0;
    PRSolid& s = g_solids_stage.solids[g_solids_stage.count++];
    s.x = (int32_t)x; s.y = (int32_t)y; s.w = (int32_t)w; s.h = (int32_t)h; s.kind = (uint8_t)kind;
    return 1;
}
EXPORT double pr_solids_commit(double world) {
    if (!g_s) return 0;
    g_solids_stage.world = (uint32_t)world;
    // only publish (and bump the revision RoA diffs on) when the set actually changed
    static PRSolidSet last; static bool have = false;
    if (have && last.count == g_solids_stage.count && last.world == g_solids_stage.world &&
        !memcmp(last.solids, g_solids_stage.solids, sizeof(PRSolid) * g_solids_stage.count)) return 1;
    last = g_solids_stage; have = true;
    g_solids_stage.revision = ++g_solid_rev;
    pr::slot_write(g_s->h_solid, g_s->solids, g_solids_stage);
    return 1;
}
static PRTargetSet g_targets_stage; static uint32_t g_target_rev = 0;
EXPORT double pr_targets_begin() { g_targets_stage.count = 0; return 1; }
EXPORT double pr_target_add(double x, double y, double w, double h, double id, double vx, double vy) {
    if (g_targets_stage.count >= PR_MAX_TARGETS) return 0;
    PRTarget& t = g_targets_stage.t[g_targets_stage.count++];
    t.x = (float)x; t.y = (float)y; t.w = (float)w; t.h = (float)h; t.id = (uint32_t)id; t.vx = (float)vx; t.vy = (float)vy;
    return 1;
}
EXPORT double pr_targets_commit(double world) {
    if (!g_s) return 0;
    g_targets_stage.world = (uint32_t)world;
    g_targets_stage.revision = ++g_target_rev;
    pr::slot_write(g_s->h_target, g_s->targets, g_targets_stage);
    return 1;
}
static PRDebugSet g_dbg;
EXPORT double pr_debug_snapshot() { if (!g_s) return 0; pr::slot_read(g_s->h_debug, g_s->debug, g_dbg); return g_dbg.count; }
EXPORT double pr_debug_get(double i, double field) {
    if (i < 0 || i >= g_dbg.count || i >= 8) return 0;
    const PRDebugBox& d = g_dbg.b[(int)i];
    switch ((int)field) { case 0: return d.x0; case 1: return d.y0; case 2: return d.x1; case 3: return d.y1; case 4: return d.kind; case 5: return d.enemy; }
    return 0;
}
static PRFeedbackSet g_fb;
EXPORT double pr_fb_snapshot() { if (!g_s) return 0; pr::slot_read(g_s->h_fb, g_s->feedback, g_fb); return g_fb.count > 8 ? 0 : g_fb.count; }
EXPORT double pr_fb_get(double i, double field) {
    if (i < 0 || i >= g_fb.count || i >= 8) return 0;
    const PRFeedback& f = g_fb.f[(int)i];
    switch ((int)field) { case 0: return f.enemy; case 1: return f.mode; case 2: return f.cx; case 3: return f.cy; case 4: return f.pct; case 5: return f.angle; case 6: return f.power; case 7: return f.dmg; case 8: return f.dir; }
    return 0;
}
EXPORT double pr_select_character(const char* id) {
    if (!g_s) return 0;
    char buf[PR_CHAR_ID_LEN] = {};
    strncpy_s(buf, id, PR_CHAR_ID_LEN - 1);
    return pr::ring_push(g_s->to_roa, PR_EV_SELECT_CHARACTER, buf, sizeof(buf)) ? 1 : 0;
}
EXPORT double pr_send_hurt(double damage, double angle, double power, double from_dir) {
    if (!g_s) return 0;
    PRHurt h{}; h.damage = (int16_t)damage; h.kb_angle = (int16_t)angle; h.kb_power = (float)power; h.from_dir = (int8_t)from_dir;
    return pr::ring_push(g_s->to_roa, PR_EV_PLAYER_HURT, &h, sizeof(h)) ? 1 : 0;
}
EXPORT double pr_send_cmd(double type, double a, double b) { PRCmd c{ (float)a, (float)b }; return g_s && pr::ring_push(g_s->to_roa, (uint16_t)type, &c, sizeof(c)) ? 1 : 0; }
EXPORT double pr_send_simple(double type) { return g_s && pr::ring_push(g_s->to_roa, (uint16_t)type, nullptr, 0) ? 1 : 0; }
EXPORT double pr_send_hit_connected(double hitbox_id) {
    uint32_t id = (uint32_t)hitbox_id;
    return g_s && pr::ring_push(g_s->to_roa, PR_EV_HIT_CONNECTED, &id, sizeof(id)) ? 1 : 0;
}

// ---- RoA -> PT: player state (snapshot, then read fields by index) ----
EXPORT double pr_player_snapshot() { if (!g_s) return 0; pr::slot_read(g_s->h_player, g_s->player, g_ps); return g_ps.frame_id; }
EXPORT double pr_player_get(double field) {
    switch ((int)field) {
    case 0: return g_ps.x;        case 1: return g_ps.y;
    case 2: return g_ps.hsp;      case 3: return g_ps.vsp;
    case 4: return g_ps.spr_dir;  case 5: return g_ps.state;
    case 6: return g_ps.attack;   case 7: return g_ps.flags;
    case 8: return g_ps.hurtbox_w; case 9: return g_ps.hurtbox_h;
    case 10: return g_ps.damage_percent;
    case 11: return g_ps.frame_cx;   case 12: return g_ps.frame_cy;
    case 13: return g_ps.view_x;     case 14: return g_ps.view_y;
    case 15: return g_ps.view_w;     case 16: return g_ps.view_h;
    case 17: return g_ps.bz_l;       case 18: return g_ps.bz_r;
    case 19: return g_ps.bz_t;       case 20: return g_ps.bz_b;
    case 23: return g_ps.in_buttons;
    case 25: return g_ps.in_pad;
    case 26: case 27: case 28: case 29: return g_ps.pad_ax[(int)field - 26];
    case 24: return g_ps.warp_gen;
    case 21: return g_ps.roa_present_fps; case 22: return g_ps.roa_tick_fps;
    }
    return 0;
}

// ---- RoA -> PT: hitboxes ----
EXPORT double pr_hitbox_snapshot() { if (!g_s) return 0; pr::slot_read(g_s->h_hitbox, g_s->hitboxes, g_hb); return g_hb.count; }
EXPORT double pr_hitbox_get(double i, double field) {
    if (i < 0 || i >= g_hb.count) return 0;
    const PRHitbox& h = g_hb.hb[(int)i];
    switch ((int)field) {
    case 0: return h.id;     case 1: return h.x;      case 2: return h.y;
    case 3: return h.w;      case 4: return h.h;      case 5: return h.damage;
    case 6: return h.kb_angle; case 7: return h.kb_power; case 8: return h.hitpause; case 9: return h.kind;
    }
    return 0;
}

// ---- RoA -> PT: pixels. dst = buffer_get_address() of a PR_FRAME_W*PR_FRAME_H*4 GML buffer. ----
// Returns 1 if a newer frame was copied (RGBA8, premultiplication left as RoA produced it).
EXPORT double pr_copy_frame(void* dst) {
    if (!g_s || !dst) return 0;
    // hand out frames in order, oldest unseen first: the GML side keeps a short queue so the two 60 Hz clocks can drift without stutter
    int best = -1; uint32_t bf = 0;
    for (int n = 0; n < 2; ++n)
        if (g_s->frame[n].ready && g_s->frame[n].frame_id > g_pixel_frame && (best < 0 || g_s->frame[n].frame_id < bf))
            { best = n; bf = g_s->frame[n].frame_id; }
    if (best < 0) return 0;
    memcpy(dst, (uint8_t*)g_s + PR_PIXELS_OFFSET(best), (size_t)PR_FRAME_W * PR_FRAME_H * 4);
    g_pixel_frame = bf;
    g_frame_meta[0] = g_s->frame[best].cx; g_frame_meta[1] = g_s->frame[best].cy; g_frame_meta[2] = g_s->frame[best].dx; g_frame_meta[3] = g_s->frame[best].dy; g_frame_meta[4] = g_s->frame[best].px; g_frame_meta[5] = g_s->frame[best].py; g_frame_meta[6] = (float)g_s->frame[best].gen;
    return 1;
}
EXPORT double pr_frame_meta(double i) { return (i >= 0 && i < 7) ? g_frame_meta[(int)i] : 0; }
EXPORT double pr_frame_w() { return PR_FRAME_W; }
EXPORT double pr_frame_h() { return PR_FRAME_H; }

// ---- RoA -> PT: events. Poll returns the type (0 = none); then read the payload. ----
EXPORT double pr_event_poll() {
    if (!g_s) return 0;
    return pr::ring_pop(g_s->to_pt, g_ev) ? g_ev.type : 0;
}
EXPORT const char* pr_event_string() { g_str.assign((const char*)g_ev.payload, strnlen((const char*)g_ev.payload, g_ev.len)); return g_str.c_str(); }
EXPORT double pr_event_u32() { uint32_t v = 0; if (g_ev.len >= 4) memcpy(&v, g_ev.payload, 4); return v; }
