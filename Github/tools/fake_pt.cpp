// Stand-in for Pizza Tower so the RoA bridge can be tested alone. Selects a character, mirrors one
// floor block, plays a scripted input sequence, logs the state RoA reports, and saves received
// frames as raw RGBA (convert with tools/rgba2png.py).
#include "../protocol/pr_shm.h"
#include <cstdio>
#include <string>
#include <cstring>

static void save_frame(PRShared* s, const char* path) {
    int best = -1; uint32_t bf = 0;
    for (int n = 0; n < 2; ++n) if (s->frame[n].ready && s->frame[n].frame_id >= bf) { best = n; bf = s->frame[n].frame_id; }
    if (best < 0) { puts("  (no frame yet)"); return; }
    FILE* f = nullptr; fopen_s(&f, path, "wb");
    if (f) { fwrite((uint8_t*)s + PR_PIXELS_OFFSET(best), 1, (size_t)PR_FRAME_W * PR_FRAME_H * 4, f); fclose(f); printf("  saved frame %u -> %s\n", bf, path); }
}

int main(int argc, char** argv) {
    const char* who = argc > 1 ? argv[1] : "stock:zetterburn";
    bool slope_test = argc > 2 && !strcmp(argv[2], "slope_test");   // ramp up to a plateau
    bool pit_test = argc > 2 && !strcmp(argv[2], "pit_test");   // run off a ledge into an endless pit
    bool sideb_test = argc > 2 && !strcmp(argv[2], "sideb_test");   // only Side+B, standing still
    bool far_test = argc > 2 && !strcmp(argv[2], "far_test");   // run far to the right, then Up+B and Side+B
    bool special_test = argc > 2 && !strcmp(argv[2], "special_test");   // fire each special/strong in turn
    bool speed_test = argc > 2 && !strcmp(argv[2], "speed_test");
    bool css_test = argc > 2 && !strcmp(argv[2], "css_test");
    bool big_slope = argc > 2 && !strcmp(argv[2], "bigslope_test");   // argv[3] = width, argv[4] = height of a ramp up, a plateau and a ramp down
    int bsw = (big_slope && argc > 3) ? atoi(argv[3]) : 300, bsh = (big_slope && argc > 4) ? atoi(argv[4]) : 300;
    bool warp_test = argc > 2 && !strcmp(argv[2], "warp_test");   // stand still, request a warp at t=100, log every frame
    bool jab_test = argc > 2 && !strcmp(argv[2], "jab_test");   // stand still and jab every 40 frames
    bool rock_test = argc > 2 && !strcmp(argv[2], "rock_test");   // argv[3] = frames to run right before neutral B
    bool wall_test = argc > 2 && !strcmp(argv[2], "wall_test");   // run into a wall, then press jump repeatedly while holding toward it
    bool jump_test = argc > 2 && !strcmp(argv[2], "jump_test");   // tap jump (argv[3] frames), log densely
    bool tunnel_test = argc > 2 && !strcmp(argv[2], "tunnel_test");   // roll (shield+direction) through a 32px high gap
    int rock_run = (rock_test && argc > 3) ? atoi(argv[3]) : 0;
    PRShared* s = pr::open_shared();
    if (!s) { puts("no shm"); return 1; }
    s->pid_pt = GetCurrentProcessId();
    printf("waiting for RoA bridge...\n");
    while (!s->pid_roa || GetTickCount() - s->hb_roa > 3000) { s->hb_pt = GetTickCount(); Sleep(200); }
    printf("RoA bridge alive; selecting %s\n", who);
    char id[PR_CHAR_ID_LEN] = {}; strncpy_s(id, who, PR_CHAR_ID_LEN - 1);
    pr::ring_push(s->to_roa, PR_EV_SELECT_CHARACTER, id, sizeof id);
    bool ready = false; uint32_t frame = 0; int t0 = -1;
    PRSolidSet solids{}; solids.count = 1; solids.solids[0] = { 0, 520, 6000, 64, PR_SOLID_BLOCK };
    if (slope_test) { solids.count = 4; solids.solids[3] = { 2300, 420, 200, 100, PR_SOLID_SLOPE_UP_L }; solids.solids[1] = { 1500, 420, 200, 100, PR_SOLID_SLOPE_UP_R }; solids.solids[2] = { 1700, 420, 600, 100, PR_SOLID_BLOCK }; }
    if (wall_test) { solids.count = 2; solids.solids[1] = { 1100, -2000, 64, 2520, PR_SOLID_BLOCK }; }
    if (tunnel_test) { solids.count = 2; solids.solids[1] = { 700, 300, 300, (argc > 3 ? 520 - atoi(argv[3]) - 300 : 188), PR_SOLID_BLOCK }; }   // ceiling bottom at 488: a 32px high gap above the floor (top 520)
    if (pit_test) solids.solids[0] = { 0, 520, 1300, 64, PR_SOLID_BLOCK };
    if (rock_test) { int fy = argc > 4 ? atoi(argv[4]) : 520; solids.solids[0] = { -20000, fy, 40000, 64, PR_SOLID_BLOCK }; }
    if (big_slope) {
        // a ramp built like Pizza Tower builds them: many slope pieces (pw x ph) chained diagonally, rectangular fill below each piece
        int pw = argc > 5 ? atoi(argv[5]) : 96, ph = argc > 6 ? atoi(argv[6]) : 64; bool fill = argc > 7;
        int n = bsw / pw; if (n < 1) n = 1;
        solids.solids[0] = { 0, 520, 30000, 64, PR_SOLID_BLOCK };
        uint32_t c = 1;
        for (int i = 0; i < n; ++i) {
            solids.solids[c++] = { 1000 + i * pw, 520 - (i + 1) * ph, pw, ph, PR_SOLID_SLOPE_UP_R };
            if (fill && i > 0) solids.solids[c++] = { 1000 + i * pw, 520 - i * ph, pw, i * ph, PR_SOLID_BLOCK };
        }
        int top = 520 - n * ph;
        solids.solids[c++] = { 1000 + n * pw, top, 1500, n * ph + 64, PR_SOLID_BLOCK };
        for (int i = 0; i < n; ++i) {
            int x0 = 1000 + n * pw + 1500 + i * pw;
            solids.solids[c++] = { x0, top + i * ph, pw, ph, PR_SOLID_SLOPE_UP_L };
            if (fill && i < n - 1) solids.solids[c++] = { x0, top + (i + 1) * ph, pw, (n - i - 1) * ph, PR_SOLID_BLOCK };
        }
        solids.count = c;
    }
    if (jab_test) { solids.solids[solids.count++] = { 600, -2000, 60, 2520, PR_SOLID_BLOCK }; }
    solids.revision = 1;
    for (;;) {
        s->hb_pt = GetTickCount();
        PREvent e;
        while (pr::ring_pop(s->to_pt, e)) {
            if (e.type == PR_EV_CHAR_READY) { printf("CHAR_READY %s\n", (char*)e.payload); ready = true; t0 = (int)frame; }
            else if (e.type == PR_EV_CSS_STATE) puts((std::string("CSS_STATE ") + (char*)e.payload).c_str());
            else if (e.type == PR_EV_CHAR_FAILED) printf("CHAR_FAILED %s\n", (char*)e.payload);
        }
        ++frame;
        int t = ready ? (int)frame - t0 : -1;
        uint16_t b = 0;
        if (!special_test && !sideb_test && t >= 120 && t < (slope_test ? 800 : (far_test ? 700 : (pit_test ? 300 : 600)))) b |= PR_BTN_RIGHT;
        if (!special_test && !slope_test && !far_test && !pit_test && !sideb_test && ((t >= 240 && t < 250) || (t >= 400 && t < 410))) b |= PR_BTN_JUMP;
        
        
        if (!special_test) { if (t >= 700 && t < 760) b |= PR_BTN_LEFT; if (t >= 780 && t < 790) b |= PR_BTN_ATTACK; }
        if (pit_test && argc > 3) { if (t >= 330 && t < 336) b |= PR_BTN_SPECIAL | PR_BTN_UP; }   // pit_b: Up+B while falling (argv[3] present)
        if (sideb_test) { b = 0; if (t >= 100 && t < 104) b |= PR_BTN_SPECIAL | PR_BTN_RIGHT; }
        if (far_test) { b = 0; if (t >= 120 && t < 700) b |= PR_BTN_RIGHT; if (t >= 760 && t < 766) b |= PR_BTN_SPECIAL | PR_BTN_UP; if (t >= 1000 && t < 1006) b |= PR_BTN_SPECIAL | PR_BTN_RIGHT; }
        if (special_test) {   // neutral B, down B, side B, up B, spaced 120 frames apart, standing still
            b = 0;
            if (t >= 100 && t < 106) b |= PR_BTN_SPECIAL;
            if (t >= 220 && t < 226) b |= PR_BTN_SPECIAL | PR_BTN_DOWN;
            if (t >= 340 && t < 346) b |= PR_BTN_SPECIAL | PR_BTN_RIGHT;
            if (t >= 460 && t < 466) b |= PR_BTN_SPECIAL | PR_BTN_UP;
        }
        if (rock_test) { b = 0; uint16_t dirb = (argc > 5 && !strcmp(argv[5], "left")) ? PR_BTN_LEFT : PR_BTN_RIGHT; if (t >= 120 && t < 120 + rock_run) b |= dirb; int tb = 140 + rock_run; if (t >= tb && t < tb + 5) b |= PR_BTN_SPECIAL; if (t >= tb + 100 && t < tb + 105) b |= PR_BTN_SPECIAL | dirb; }
        if (wall_test) { b = 0; if (t >= 120 && t < 700) b |= PR_BTN_RIGHT; if (t >= 330 && t < 700 && ((t - 330) % 20) < 3) b |= PR_BTN_JUMP; }
        if (jump_test) { b = 0; int hold = argc > 3 ? atoi(argv[3]) : 8; if (t >= 200 && t < 200 + hold) b |= PR_BTN_JUMP; }
        if (tunnel_test) { b = 0; if (t >= 120 && t < 195) b |= PR_BTN_RIGHT; if (t >= 196 && ((t - 196) % 45) < 3) b |= PR_BTN_SHIELD | PR_BTN_RIGHT; }
        if (speed_test) { b = 0; if (t >= 120 && t < 300) b |= PR_BTN_RIGHT; if (t == 100) { PRCmd c{ 1.0f, (float)(argc > 3 ? atof(argv[3]) : 2.0) }; pr::ring_push(s->to_roa, PR_EV_SET_OPTION, &c, sizeof c); } }
        if (css_test) { b = 0; static bool o1 = false, o2 = false; if (t >= 60 && !o1) { o1 = true; pr::ring_push(s->to_roa, PR_EV_OPEN_CSS, nullptr, 0); puts("sent OPEN_CSS"); } if (o1 && !o2 && frame > 420 && !getenv("CSS_HOLD")) { o2 = true; pr::ring_push(s->to_roa, PR_EV_CANCEL_CSS, nullptr, 0); puts("sent CANCEL_CSS"); } }
        if (big_slope) { b = 0; if (t >= 120) b |= PR_BTN_RIGHT; if (t >= 1500) b = 0; if (t == 100 && argc > 9) { PRCmd c{ 1.0f, (float)atof(argv[9]) }; pr::ring_push(s->to_roa, PR_EV_SET_OPTION, &c, sizeof c); } }
        if (jab_test && argc > 3 && !strcmp(argv[3], "pinata") && t == 20) { PRCmd c{ 2.0f, 0.0f }; pr::ring_push(s->to_roa, PR_EV_SET_OPTION, &c, sizeof c); }
        if (jab_test) { b = 0; if (t >= 100 && ((t - 100) % 40) < 3) b |= PR_BTN_ATTACK; }
        if (jab_test && argc > 3 && !strcmp(argv[3], "spec")) { b = 0; if (t >= 100) { int ph = ((t - 100) / 120) % 4, k = (t - 100) % 120; if (k < 4) { b |= PR_BTN_SPECIAL; if (ph == 1) b |= PR_BTN_DOWN; if (ph == 2) b |= PR_BTN_RIGHT; if (ph == 3) b |= PR_BTN_UP; } } }
        if (jab_test) { PRTargetSet ts{}; ts.revision = frame; ts.count = 1; ts.world = (t >= 0 && t < 3) ? 1 : 0; PRPlayerState ps2; pr::slot_read(s->h_player, s->player, ps2); ts.t[0] = { ps2.x + 70.f, ps2.y - 30.f, 64.f, 64.f, 7777u, 0.f, 0.f }; pr::slot_write(s->h_target, s->targets, ts); }
        if (jab_test) { PRFeedbackSet fs; pr::slot_read(s->h_fb, s->feedback, fs); static int lastm = -9; int m = fs.count ? fs.f[0].mode : -9; if (m != lastm) { lastm = m; printf("FB t=%d count=%u mode=%d cx=%.0f cy=%.0f" "\n", t, fs.count, m, fs.count ? fs.f[0].cx : 0.f, fs.count ? fs.f[0].cy : 0.f); } }
        PRInput in{}; in.frame_id = frame; in.buttons = b;
        pr::slot_write(s->h_input, s->input, in);
        PRPtState pt{}; pt.frame_id = frame; pt.flags = 0; pt.room_id = 1; pt.puppet_x = 400; pt.puppet_y = 400; pt.warp = (t >= 0 && t < 3) ? 1 : 0; if (warp_test) pt.warp = (t >= 0 && t < 3) ? 1 : (t >= 100 ? 2 : 0);
        pr::slot_write(s->h_pt, s->pt, pt);
        if (solids.world != pt.warp) { solids.world = pt.warp; ++solids.revision; }
        pr::slot_write(s->h_solid, s->solids, solids);
        if (ready && (t % 60 == 0 || (t >= 780 && t <= 800 && t % 3 == 0) || (slope_test && t % 10 == 0 && t > 300) || (far_test && t >= 700 && t % 8 == 0) || (sideb_test && t >= 96 && t <= 330 && t % 4 == 0) || (special_test && t >= 455 && t % 6 == 0) || (pit_test && t >= 150 && t % 15 == 0) || (rock_test && t >= 140 + rock_run && t % (argc > 6 ? atoi(argv[6]) : 6) == 0) || (wall_test && t >= 300 && t % 5 == 0) || (jump_test && t >= 196 && t < 300 && t % 2 == 0) || (tunnel_test && t >= 190 && t % 4 == 0) || (speed_test && t % 40 == 0) || (warp_test && t >= 90 && t < 260) || (jab_test && t % 20 == 0) || (big_slope && t >= 150 && t % (argc > 8 ? atoi(argv[8]) : 4) == 0))) {
            PRPlayerState ps; pr::slot_read(s->h_player, s->player, ps);
            PRHitboxSet hb; pr::slot_read(s->h_hitbox, s->hitboxes, hb);
            printf("t=%4d buttons=%03x  pos=(%.0f,%.0f) vel=(%.1f,%.1f) dir=%d state=%d attack=%d ground=%d hitboxes=%u\n", t, b, ps.x, ps.y, ps.hsp, ps.vsp, ps.spr_dir, ps.state, ps.attack, (ps.flags & PR_ROA_ON_GROUND) ? 1 : 0, hb.count);
            for (uint32_t i = 0; i < hb.count; ++i) printf("    hitbox id=%u rect=(%.0f,%.0f %.0fx%.0f) dmg=%d kb=%d/%.1f\n", hb.hb[i].id, hb.hb[i].x, hb.hb[i].y, hb.hb[i].w, hb.hb[i].h, hb.hb[i].damage, hb.hb[i].kb_angle, hb.hb[i].kb_power);
        }
        if (ready && t == 300 && !special_test && !big_slope) { PRHurt hh{}; hh.damage = 12; hh.kb_angle = 40; hh.kb_power = 7.5f; hh.from_dir = -1; pr::ring_push(s->to_roa, PR_EV_PLAYER_HURT, &hh, sizeof hh); puts("PLAYER_HURT test sent"); }
        if (ready && !slope_test && t >= 780 && t < 800) { PRHitboxSet hb2; pr::slot_read(s->h_hitbox, s->hitboxes, hb2); static uint32_t sent = 0; if (hb2.count && sent != hb2.hb[0].id) { sent = hb2.hb[0].id; uint32_t idv = sent; pr::ring_push(s->to_roa, PR_EV_HIT_CONNECTED, &idv, sizeof idv); puts("HIT_CONNECTED test sent"); } }
        if (ready && (t == 60 || t == 300 || t == 500 || t == 785)) { char p[64]; sprintf_s(p, "C:\\ptbuild\\roa_frame_%d.rgba", t); save_frame(s, p); }
        if (ready && t > (jab_test ? 1500 : warp_test ? 270 : big_slope ? 1500 : css_test ? (getenv("CSS_HOLD") ? 20000 : 700) : speed_test ? 330 : tunnel_test ? 520 : jump_test ? 300 : wall_test ? 700 : rock_test ? 300 + rock_run : sideb_test ? 330 : special_test ? 900 : (slope_test ? 900 : (far_test ? 1250 : (pit_test ? (argc > 3 ? 900 : 700) : 840))))) { puts("sequence done"); return 0; }
        fflush(stdout);
        Sleep(16);
    }
}
