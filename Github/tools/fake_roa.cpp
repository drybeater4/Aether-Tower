// Stand-in for the RoA bridge so the Pizza Tower side can be tested alone (like SkyCraft's
// fake_skyrim.py). Acts as a trivial "character": a magenta box that moves with the input, falls
// onto the mirrored PT solids, and spawns a hitbox in front of it while ATTACK is held.
#include "../protocol/pr_shm.h"
#include <cstdio>
#include <vector>
#include <cmath>

int main() {
    PRShared* s = pr::open_shared();
    if (!s) { puts("cannot open shared memory"); return 1; }
    s->pid_roa = GetCurrentProcessId();
    float x = 200, y = 100, hsp = 0, vsp = 0; int dir = 1; bool ground = false; bool ready = false;
    uint32_t frame_out = 0, hbid = 1, last_rev = 0;
    std::vector<PRSolid> solids;
    uint32_t tick = 0;
    for (;;) {
        s->hb_roa = GetTickCount();
        PREvent e;
        while (pr::ring_pop(s->to_roa, e)) {
            if (e.type == PR_EV_SELECT_CHARACTER) {
                printf("select character: %s\n", (char*)e.payload);
                ready = false; Sleep(300);
                char id[PR_CHAR_ID_LEN] = {}; memcpy(id, e.payload, PR_CHAR_ID_LEN - 1);
                pr::ring_push(s->to_pt, PR_EV_CHAR_READY, id, (uint16_t)strlen(id) + 1);
                ready = true;
            } else if (e.type == PR_EV_PLAYER_HURT) {
                PRHurt h; memcpy(&h, e.payload, sizeof h);
                printf("PLAYER_HURT dmg=%d angle=%d power=%.1f dir=%d\n", h.damage, h.kb_angle, h.kb_power, h.from_dir);
                float a = h.kb_angle * 3.14159f / 180; hsp = cosf(a) * h.kb_power; vsp = -sinf(a) * h.kb_power;
            } else if (e.type == PR_EV_HIT_CONNECTED) {
                uint32_t id; memcpy(&id, e.payload, 4); printf("HIT_CONNECTED hitbox %u\n", id);
            }
        }
        PRInput in; pr::slot_read(s->h_input, s->input, in);
        PRPtState pt; pr::slot_read(s->h_pt, s->pt, pt);
        PRSolidSet* ss = &s->solids;
        if (ss->revision != last_rev) { last_rev = ss->revision; solids.assign(ss->solids, ss->solids + ss->count); printf("solids rev %u: %zu\n", last_rev, solids.size()); }
        if (pt.warp) { x = pt.puppet_x; y = pt.puppet_y; hsp = vsp = 0; }
        if (ready && !(pt.flags & PR_PT_PAUSED)) {
            float mv = ((in.buttons & PR_BTN_RIGHT) ? 1 : 0) - ((in.buttons & PR_BTN_LEFT) ? 1 : 0);
            if (mv) dir = mv > 0 ? 1 : -1;
            if (ground) hsp = mv * 5; else hsp += mv * 0.3f;
            if (ground && (in.buttons & PR_BTN_JUMP)) { vsp = -11; ground = false; }
            vsp += 0.5f; if (vsp > 14) vsp = 14;
            // crude AABB collision (box 24x48 centred on x, feet at y)
            auto hit = [&](float nx, float ny) {
                for (auto& b : solids) if (b.kind == PR_SOLID_BLOCK &&
                    nx + 12 > b.x && nx - 12 < b.x + b.w && ny > b.y && ny - 48 < b.y + b.h) return true;
                return false; };
            if (!hit(x + hsp, y)) x += hsp; else hsp = 0;
            ground = false;
            if (!hit(x, y + vsp)) y += vsp; else { if (vsp > 0) ground = true; vsp = 0; }
        }
        PRPlayerState ps{}; ps.frame_id = in.frame_id; ps.flags = PR_ROA_READY | (ground ? PR_ROA_ON_GROUND : 0);
        ps.x = x; ps.y = y; ps.hsp = hsp; ps.vsp = vsp; ps.spr_dir = (int8_t)dir; ps.hurtbox_w = 24; ps.hurtbox_h = 48;
        pr::slot_write(s->h_player, s->player, ps);
        PRHitboxSet hb{}; hb.frame_id = in.frame_id;
        if (ready && (in.buttons & PR_BTN_ATTACK)) {
            hb.count = 1; PRHitbox& h = hb.hb[0]; h.id = hbid; h.x = x + dir * 36; h.y = y - 24; h.w = 48; h.h = 40;
            h.damage = 9; h.kb_angle = 45; h.kb_power = 6; h.hitpause = 6;
        } else if (hb.count == 0) hbid++;
        pr::slot_write(s->h_hitbox, s->hitboxes, hb);
        // pixels: transparent frame with a 24x48 magenta box at the centre (RoA camera centred on the player)
        int n = (frame_out + 1) & 1;
        uint8_t* px = (uint8_t*)s + PR_PIXELS_OFFSET(n);
        memset(px, 0, (size_t)PR_FRAME_W * PR_FRAME_H * 4);
        if (ready) for (int yy = -48; yy < 0; ++yy) for (int xx = -12; xx < 12; ++xx) {
            uint8_t* p = px + (((PR_FRAME_H / 2 + yy) * PR_FRAME_W) + PR_FRAME_W / 2 + xx) * 4;
            p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
        }
        s->frame[n].frame_id = ++frame_out; s->frame[n].ready = 1;
        if (++tick % 120 == 0) printf("t=%u in.frame=%u buttons=%x room=%d puppet=(%.0f,%.0f) me=(%.0f,%.0f) solids=%zu pt_alive=%d\n",
            tick, in.frame_id, in.buttons, pt.room_id, pt.puppet_x, pt.puppet_y, x, y, solids.size(), (int)((GetTickCount() - s->hb_pt) < 2000));
        fflush(stdout);
        Sleep(16);
    }
}
