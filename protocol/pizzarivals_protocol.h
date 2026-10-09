// PizzaRivals shared-memory protocol, v1.
// Included by BOTH the x86 RoA bridge (roa-bridge/) and the x64 Pizza Tower extension
// (pt-bridge/extension/). Both sides map "Local\PizzaRivals_v1". Little-endian, packed,
// fixed-size, no pointers. Latest-value slots use a seqlock (writer: seq++ (odd), write,
// seq++ (even); reader: retry while odd or changed). Do not reorder fields: bump
// PR_PROTOCOL_VERSION instead.
#pragma once
#include <stdint.h>

#define PR_PROTOCOL_VERSION 1
#define PR_MAGIC            0x31525250u   // 'PRR1'
#define PR_SHM_NAME         "Local\\PizzaRivals_v1"

#define PR_FRAME_W          960           // PT's native resolution; RoA renders its sprite layer at this size
#define PR_FRAME_H          540
#define PR_MAX_HITBOXES     32
#define PR_MAX_SOLIDS       512
#define PR_MAX_TARGETS      16            // Pizza Tower enemies mirrored into RoA as hittable stand-ins
#define PR_EVENT_RING_SIZE  256           // power of two
#define PR_CHAR_ID_LEN      64            // catalog id, e.g. "workshop:1866016173" or "stock:kragg"

#pragma pack(push, 1)

// ---- Input: PT -> RoA, every PT step (RoA runs at 60 fps like PT) ----------------------
enum PRButton : uint16_t {
    PR_BTN_LEFT = 1 << 0,  PR_BTN_RIGHT = 1 << 1, PR_BTN_UP = 1 << 2,  PR_BTN_DOWN = 1 << 3,
    PR_BTN_JUMP = 1 << 4,  PR_BTN_ATTACK = 1 << 5, PR_BTN_SPECIAL = 1 << 6, PR_BTN_SHIELD = 1 << 7,
    PR_BTN_TAUNT = 1 << 8, PR_BTN_STRONG = 1 << 9, PR_BTN_PARRY = 1 << 10,
    PR_BTN_START = 1 << 15,
    PR_BTN_CSTICK_L = 1 << 11, PR_BTN_CSTICK_R = 1 << 12, PR_BTN_CSTICK_U = 1 << 13, PR_BTN_CSTICK_D = 1 << 14,
};
struct PRInput {
    uint32_t frame_id;
    uint16_t buttons;          // PRButton bitmask
    int8_t   stick_x, stick_y; // -100..100, for analog-aware characters
};

// ---- PT world state the RoA side needs ------------------------------------------------
enum PRPtFlags : uint32_t {
    PR_PT_PAUSED   = 1 << 0,   // PT menu / cutscene / loading: RoA freezes
    PR_PT_HIDDEN   = 1 << 1,   // PT wants the character invisible (door, tube, pizzaface cutscene...)
    PR_PT_INVINCIBLE = 1 << 2,
    PR_PT_NATIVE_INPUT = 1 << 3, // RoA reads the player's controller itself: do not inject PT's input
    PR_PT_RECENTER = 1 << 5,     // keep RoA's vertical position inside its own stage range (shifts the whole RoA world)
    PR_PT_RESET_PERCENT = 1 << 4,// held a few frames when a stage is entered: RoA resets the character's percent
};
struct PRPtState {
    uint32_t frame_id;
    uint32_t flags;            // PRPtFlags
    int32_t  room_id;          // PT room index; changes => RoA rebuilds its collision mirror
    float    cam_x, cam_y;     // PT camera top-left (room px)
    float    puppet_x, puppet_y; // where PT currently has its player puppet (used only to warp RoA on room change / teleport)
    uint8_t  warp;             // warp serial: every time this value changes, RoA must teleport its player to puppet_x/y once it can
};

// ---- Collision mirror: PT -> RoA (SkyCraft's CollisionField) ---------------------------
enum PRSolidKind : uint8_t { PR_SOLID_BLOCK = 0, PR_SOLID_PLATFORM = 1, PR_SOLID_SLOPE_UP_R = 2, PR_SOLID_SLOPE_UP_L = 3 };
struct PRSolid { int32_t x, y, w, h; uint8_t kind; uint8_t pad[3]; };   // room px, top-left
// A Pizza Tower enemy: RoA places an invisible hittable object (an abyss piñata) over it so RoA's own hit pipeline runs for real
// (hit effects, sounds, hitpause, projectiles reacting). Centre and size in PT space.
struct PRTarget { float x, y, w, h; uint32_t id; float vx, vy; };
struct PRTargetSet { uint32_t revision; uint32_t count; uint32_t world; PRTarget t[PR_MAX_TARGETS]; };   // world = the warp serial the coordinates belong to
// Debug: the hurtbox of each stand-in CPU as RoA really has it (PT space). kind 0 = parked, 1 = standing on an enemy.
// RoA -> PT: what became of each enemy that has a stand-in CPU. mode 0 = standing in (PT does nothing), 1 = RoA is juggling it (PT puts the
// enemy where the CPU is, frozen), 2 = the juggle is over (PT kills the enemy now).
struct PRFeedback { uint32_t enemy; int32_t mode; float cx, cy; float pct, angle, power, dmg; int32_t dir; };   // mode 3: the finishing hit (Pizza Tower's own reaction): angle/power/dmg/dir describe the hit
struct PRFeedbackSet { uint32_t count; PRFeedback f[8]; };
struct PRDebugBox { float x0, y0, x1, y1; int32_t kind; int32_t enemy; };
struct PRDebugSet { uint32_t count; PRDebugBox b[8]; };
struct PRSolidSet {
    uint32_t revision;         // bumped on every change; RoA diffs and (re)creates par_block instances
    uint32_t count;
    uint32_t world;            // the warp serial these coordinates belong to (RoA ignores a set that is not of its current world)
    PRSolid  solids[PR_MAX_SOLIDS];   // only the ones within ~2 screens of the puppet
};

// ---- RoA -> PT: authoritative player state ----------------------------------------------
enum PRRoaFlags : uint32_t {
    PR_ROA_READY      = 1 << 0,   // character loaded, match running
    PR_ROA_ON_GROUND  = 1 << 1,
    PR_ROA_HITSTUN    = 1 << 2,
    PR_ROA_DEAD       = 1 << 3,   // RoA says KO'd (PT decides what that means)
    PR_ROA_LOADING    = 1 << 4,   // character switch in progress
    PR_ROA_ERROR      = 1 << 5,
    PR_ROA_PAD_ACTIVE = 1 << 7,   // a gamepad was used within the last ~1.5 s (RoA polls the hardware itself)
    PR_ROA_INVULN     = 1 << 8,   // a dodge is in its invulnerable window: Pizza Tower's attacks pass through
    PR_ROA_TAUNT      = 1 << 9,   // the rival is taunting (Pizza Tower's alarms listen for it)
    PR_ROA_WARPING    = 1 << 6,   // RoA has a pending warp request it has not applied yet (e.g. still in its spawn animation)
};
struct PRPlayerState {
    uint32_t frame_id;         // echo of the PRInput frame this was simulated from
    uint32_t flags;            // PRRoaFlags
    float    x, y;             // RoA world px, scaled to PT px by PR_SCALE in the extension
    float    hsp, vsp;
    int8_t   spr_dir;          // +1 right, -1 left
    int8_t   pad0[3];
    int16_t  state;            // RoA PS_* constant (idle, attack_ground, hitstun...)
    int16_t  attack;           // AT_* currently running, 0 if none
    int16_t  hurtbox_w, hurtbox_h; // for PT puppet's collision rect
    int16_t  damage_percent;   // RoA percent, mirrored to PT HP model by the PT side
    int16_t  pad1;
    float    frame_cx, frame_cy; // PT-space point (whole pixels) at the centre of the pixel frame
    float    view_x, view_y, view_w, view_h;   // RoA camera rectangle, PT space (debug overlay)
    uint32_t warp_gen;                  // bumped every time RoA re-derives the Pizza Tower<->RoA offset (frames from before a warp are stale)
    uint16_t in_buttons, in_pad;        // what RoA itself reads from the pad (PRButton mask): lets Pizza Tower follow it for menus and doors
    float    roa_present_fps, roa_tick_fps;     // how fast RoA really renders / is advanced (should both be 60)
    float    bz_l, bz_r, bz_t, bz_b;           // RoA blast zone edges as the stage defines them, PT space (debug overlay)
    int8_t   pad_ax[4];                // left stick x, y and right stick x, y of the pad RoA sees (-100..100); in_pad has every button, bit n = gp button 32769 + n
};

// RoA hitbox -> PT damage volume. One per active RoA hitbox this frame.
struct PRHitbox {
    uint32_t id;               // stable while the hitbox lives (so PT doesn't hit twice)
    float    x, y, w, h;       // PT-space rect centre + size (ellipse hitboxes are approximated)
    int16_t  damage;
    int16_t  kb_angle;         // RoA degrees
    float    kb_power;
    int16_t  hitpause;
    uint8_t  kind;             // 0 melee, 1 projectile, 2 explosion
    uint8_t  pad;
};
struct PRHitboxSet { uint32_t frame_id; uint32_t count; PRHitbox hb[PR_MAX_HITBOXES]; };

// ---- Frame pixels: RoA -> PT. Transparent RGBA8, centred on the player ------------------
// RoA camera is locked on its player, so PT draws this at (puppet - W/2, puppet - H/2).
struct PRFrameSlot { uint32_t frame_id; uint32_t ready; float cx, cy, dx, dy, px, py; uint32_t gen; };   // cx,cy: PT-space centre of the pixels; dx,dy: sub-pixel remainder of the character origin; px,py: the origin itself   // ready=1 after pixels fully written
// pixels live outside the struct: shm[PR_FRAME_OFFSET(n)], n in {0,1} (double buffered)

// ---- Events (SPSC rings) ----------------------------------------------------------------
enum PREventType : uint16_t {
    PR_EV_NONE = 0,
    // PT -> RoA
    PR_EV_SELECT_CHARACTER = 1,   // payload: char[PR_CHAR_ID_LEN] catalog id
    PR_EV_PLAYER_HURT      = 2,   // payload: PRHurt  (PT enemy hit the puppet)
    PR_EV_RESPAWN          = 3,
    PR_EV_SHUTDOWN         = 4,
    PR_EV_SET_OPTION       = 6,   // payload: PRCmd {a = option id, b = value}   (1 = speed multiplier)
    PR_EV_OPEN_CSS         = 7,   // show RoA's own window so the player can pick any character/skin (incl. workshop) in its menus
    PR_EV_CANCEL_CSS       = 8,   // hide it again and go back to the current character
    // RoA -> PT
    PR_EV_CHAR_READY       = 100, // payload: char id that finished loading
    PR_EV_CHAR_FAILED      = 101, // payload: char id + error string
    PR_EV_HIT_CONNECTED    = 102, // payload: uint32 hitbox id that PT confirmed hit; RoA applies hitpause
    PR_EV_SOUND            = 103, // payload: PRSound (optional audio bridge)
    PR_EV_CSS_STATE        = 104, // payload: "1" RoA's own window is open for character picking, "0" it closed again
};
struct PRCmd { float a, b; };
struct PRHurt { int16_t damage; int16_t kb_angle; float kb_power; int8_t from_dir; uint8_t pad[3]; };
struct PRSound { char name[48]; float volume; float pan; };
struct PREvent { uint16_t type; uint16_t len; uint8_t payload[124]; };    // 128 bytes
struct PRRing { volatile uint32_t head, tail; PREvent ev[PR_EVENT_RING_SIZE]; };

// ---- Shared block ----------------------------------------------------------------------
struct PRSlotHdr { volatile uint32_t seq; };
struct PRShared {
    uint32_t magic, version;
    uint32_t pid_pt, pid_roa;
    volatile uint32_t hb_pt, hb_roa;           // heartbeats (ms tick); a stale one means the peer died
    PRSlotHdr h_input, h_pt, h_player, h_hitbox, h_solid, h_target, h_debug, h_fb;
    PRInput       input;
    PRPtState     pt;
    PRPlayerState player;
    PRHitboxSet   hitboxes;
    PRSolidSet    solids;
    PRTargetSet   targets;
    PRDebugSet    debug;
    PRFeedbackSet feedback;
    PRFrameSlot   frame[2];
    PRRing        to_roa, to_pt;
    // followed by: uint8_t pixels[2][PR_FRAME_W * PR_FRAME_H * 4]
};
#define PR_PIXELS_OFFSET(n) (sizeof(PRShared) + (size_t)(n) * PR_FRAME_W * PR_FRAME_H * 4)
#define PR_SHM_SIZE         PR_PIXELS_OFFSET(2)

#pragma pack(pop)
