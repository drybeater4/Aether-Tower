// PizzaRivals: Pizza Tower side of the bridge. See docs/DESIGN.md.
// RoA (separate x86 process) owns the character; this file mirrors PT's world to it, moves the
// puppet (obj_player1 in states.rivals) to where RoA says the character is, spawns RoA's
// hitboxes as damage to PT enemies, and sends PT enemy hits back to RoA.

#macro RIVALS_PUPPET_YOFF 24        // puppet centre sits this many px above RoA's feet position
#macro RIVALS_SOLID_RADIUS 1400     // mirror solids within this distance of the player
#macro RIVALS_HURT_DAMAGE 25        // RoA percent added per PT hit
#macro RIVALS_HURT_POWER 7.5
#macro RIVALS_METAL_DAMAGE 9      // RoA hitbox damage needed to break iron blocks        // RoA knockback power for PT hits

// protocol constants (protocol/pizzarivals_protocol.h)
#macro PRB_LEFT 1
#macro PRB_RIGHT 2
#macro PRB_UP 4
#macro PRB_DOWN 8
#macro PRB_JUMP 16
#macro PRB_ATTACK 32
#macro PRB_SPECIAL 64
#macro PRB_SHIELD 128
#macro PRB_TAUNT 256
#macro PRB_START 32768
#macro PRF_PAUSED 1
#macro PRF_HIDDEN 2
#macro PRF_NATIVE_INPUT 8
#macro PRF_RESET_PERCENT 16
#macro PRF_RECENTER 32   // RoA reads the controller itself (right stick, analog, charge smashes...)
#macro RIVALS_MACH_SPEED 5.5      // horizontal RoA speed at which running breaks blocks
#macro PRE_CHAR_READY 100
#macro PRE_CHAR_FAILED 101

function rivals_init()
{
	global.rivals_pad_block = false;
	global.rivals_room_info = "";
	global.rivals_trace_on = file_exists(working_directory + "pizzarivals_trace_on.txt");
	global.rivals_trace = -1;
	var dll = working_directory + "pizzarivals_pt.dll";
	global.rivals =
	{
		available: file_exists(dll),
		enabled: true, connected: false, ready: false, loading: false,
		frame: 0, retry: 0, requested: false, status: "", char_id: "", char_name: "",
		catalog: [], menu_open: false, menu_sel: 0, menu_scroll: 0,
		surface: -1, buf: -1, has_frame: false,
		roa_fx: 0, roa_fy: 0, roa_hh: 46, roa_x: 0, roa_y: 0, roa_hsp: 0, roa_vsp: 0, roa_dir: 1, roa_state: 0, roa_flags: 0, roa_percent: 0,
		dev_forced: false, reset_until: 0, puppet_room: -1, recenter: false, takeover_wait: 0, yield_cd: 0, input_mode: 2, last_px: 0, last_py: 0, warp_until: 0, warp_serial: 0, last_room: -1, last_level: -999, cpu_ids: ds_map_create(), juggled: ds_map_create(),  cpu_targets: true,  gen: 0, gen_req: 0, warp_pending: false, warp_since: 0,  speed: 1.5, saved_sig: "", cpu_recent: ds_map_create(), run_hsp: 0, run_dir: 1, run_hold: 0, percent: 0, kill_pct: 10, volume: 1, pending_pick: -2, pending_toggle: false, floor_time: 0.25, show_hud: true, finished: ds_map_create(), cpu_lastpct: ds_map_create(), last_native_press: -100, pt_always: false, show_debug: false, pending_peppino: false, npad_stamp: -1, npad_now: false, npad_t: -10000, pad_stamp: -1, pad_now: 0, pad_prev: 0, pad_ax: [0, 0, 0, 0], sw_n: 0, sw_x: 0, sw_y: 0, sw_room: -1, sw_tries: 0, elev_active: false, elev_id: noone, elev_wait: 0, elev_sel: 0, show_pct: true, cpu_pct: ds_map_create(), use_queue: false, speed_sent: false, css: false, pending_reset: false, start_x: 0, start_y: 0, start_room: -1, fq: [], fpool: [], fstall: 0, fcx: 0, fcy: 0, fdx: 0, fdy: 0, fpx: 0, fpy: 0, fvalid: false, in_now: 0, in_prev: 0, in_poll_t: -1000, rfps: 60, rtick: 60, slow_until: 0, hide_tv: false, show_bounds: false, vx: 0, vy: 0, vw: 960, vh: 540, bzl: 0, bzr: 0, bzt: 0, bzb: 0, shake_frames: 0, prev_up: false, solid_timer: 0, env_cd: 0, was_rival: false, bubble: false, bub_v: 0, drive_until: -10, drive_hurt: false, drive_prev: false, drive_vis: false, rel_h: 0, rel_v: 0, rel_pending: false, pal_cd: 0, safe_x: 0, safe_y: 0, safe_room: -1, safe_t: 0, stun_until: -10, water_cd: 0, enemy_atk: 10, music_fix: 0, gus: false, oob_n: 0, warp_frame: -100, warp_x: 0, warp_y: 0, net_frame: -1000, push_n: 0, dn_last: 0, music_volume: 1, mus_ext_wait: false, mus_roadir: "", mus_log_key: "", boss_log_n: 0, mus: { key: "", kind: "", intro_s: -1, loop_s: -1, inst: -1, phase: 0, paused: false, target: noone, gain_t: -100 }, mus_cfg: [], mus_cache: ds_map_create(), mus_loaded: false, mus_room: -1, weak_metal: false, dodge_frame: -1000, gun_pend: ds_map_create(), music_until: 0, boss_cd: ds_map_create(), hit_pairs: ds_map_create(), fn: {}
	};
	var R = global.rivals;
	rivals_register_lang();
	if !R.available
	{
		R.status = "pizzarivals_pt.dll missing";
		return;
	}
	var f = R.fn;
	f.open = external_define(dll, "pr_open", dll_cdecl, ty_real, 0);
	f.alive = external_define(dll, "pr_peer_alive", dll_cdecl, ty_real, 0);
	f.beat = external_define(dll, "pr_heartbeat", dll_cdecl, ty_real, 0);
	f.input = external_define(dll, "pr_send_input", dll_cdecl, ty_real, 4, ty_real, ty_real, ty_real, ty_real);
	f.state = external_define(dll, "pr_send_state", dll_cdecl, ty_real, 8, ty_real, ty_real, ty_real, ty_real, ty_real, ty_real, ty_real, ty_real);
	f.sbegin = external_define(dll, "pr_solids_begin", dll_cdecl, ty_real, 0);
	f.sadd = external_define(dll, "pr_solid_add", dll_cdecl, ty_real, 5, ty_real, ty_real, ty_real, ty_real, ty_real);
	f.scommit = external_define(dll, "pr_solids_commit", dll_cdecl, ty_real, 1, ty_real);
	f.select = external_define(dll, "pr_select_character", dll_cdecl, ty_real, 1, ty_string);
	f.cmd = external_define(dll, "pr_send_cmd", dll_cdecl, ty_real, 3, ty_real, ty_real, ty_real);
	f.tbegin = external_define(dll, "pr_targets_begin", dll_cdecl, ty_real, 0);
	f.tadd = external_define(dll, "pr_target_add", dll_cdecl, ty_real, 7, ty_real, ty_real, ty_real, ty_real, ty_real, ty_real, ty_real);
	f.fbsnap = external_define(dll, "pr_fb_snapshot", dll_cdecl, ty_real, 0);
	f.fbget = external_define(dll, "pr_fb_get", dll_cdecl, ty_real, 2, ty_real, ty_real);
	f.tcommit = external_define(dll, "pr_targets_commit", dll_cdecl, ty_real, 1, ty_real);
	f.dsnap = external_define(dll, "pr_debug_snapshot", dll_cdecl, ty_real, 0);
	f.dget = external_define(dll, "pr_debug_get", dll_cdecl, ty_real, 2, ty_real, ty_real);
	f.hurt = external_define(dll, "pr_send_hurt", dll_cdecl, ty_real, 4, ty_real, ty_real, ty_real, ty_real);
	f.hitok = external_define(dll, "pr_send_hit_connected", dll_cdecl, ty_real, 1, ty_real);
	f.psnap = external_define(dll, "pr_player_snapshot", dll_cdecl, ty_real, 0);
	f.pget = external_define(dll, "pr_player_get", dll_cdecl, ty_real, 1, ty_real);
	f.hsnap = external_define(dll, "pr_hitbox_snapshot", dll_cdecl, ty_real, 0);
	f.hget = external_define(dll, "pr_hitbox_get", dll_cdecl, ty_real, 2, ty_real, ty_real);
	f.copyframe = external_define(dll, "pr_copy_frame", dll_cdecl, ty_real, 1, ty_string);
	f.fw = external_define(dll, "pr_frame_w", dll_cdecl, ty_real, 0);
	f.fh = external_define(dll, "pr_frame_h", dll_cdecl, ty_real, 0);
	f.fmeta = external_define(dll, "pr_frame_meta", dll_cdecl, ty_real, 1, ty_real);
	f.focus = external_define(dll, "pr_focus_window", dll_cdecl, ty_real, 0);
	f.mext = external_define(dll, "pr_music_extract", dll_cdecl, ty_real, 3, ty_string, ty_string, ty_string);
	f.mstate = external_define(dll, "pr_music_state", dll_cdecl, ty_real, 0);
	f.fcopy = external_define(dll, "pr_file_copy", dll_cdecl, ty_real, 2, ty_string, ty_string);
	f.astart = external_define(dll, "pr_audio_start", dll_cdecl, ty_real, 0);
	f.again = external_define(dll, "pr_audio_gain", dll_cdecl, ty_real, 1, ty_real);
	f.astop = external_define(dll, "pr_audio_stop", dll_cdecl, ty_real, 0);
	f.evpoll = external_define(dll, "pr_event_poll", dll_cdecl, ty_real, 0);
	f.evstr = external_define(dll, "pr_event_string", dll_cdecl, ty_string, 0);
	rivals_load_catalog();
	rivals_load_settings();
	R.status = "waiting for Rivals of Aether bridge";
	// dev aid: pizzarivals_dev.txt = name of a room to jump to at startup (enables Rivals mode too)
	R.dev_room = "";
	var dev = working_directory + "pizzarivals_dev.txt";
	if file_exists(dev)
	{
		var fh = file_text_open_read(dev);
		R.dev_room = string_trim(file_text_readln(fh));
		file_text_close(fh);
	}
}

function rivals_load_catalog()
{
	var R = global.rivals;
	var path = working_directory + "pizzarivals_catalog.json";
	if !file_exists(path)
		return;
	var fh = file_text_open_read(path);
	var s = "";
	while !file_text_eof(fh)
		s += file_text_readln(fh);
	file_text_close(fh);
	var j = json_parse(s);
	R.catalog = j.characters;
	if array_length(R.catalog) > 0 && R.char_id == ""
	{
		var _zi = 0;
		for (var _z = 0; _z < array_length(R.catalog); _z++)
		{
			if R.catalog[_z].id == "stock:zetterburn"
				_zi = _z;
		}
		R.char_id = R.catalog[_zi].id;
		R.char_name = R.catalog[_zi].name;
		if file_exists("pizzarivals_last.txt")
		{
			var _lf = file_text_open_read("pizzarivals_last.txt");
			var _last = string_trim(file_text_readln(_lf));
			file_text_close(_lf);
			for (var _i = 0; _i < array_length(R.catalog); _i++)
			{
				if R.catalog[_i].id == _last
				{
					R.char_id = _last;
					R.char_name = R.catalog[_i].name;
				}
			}
		}
	}
}

function rivals_catalog_index(id_)
{
	var cat = global.rivals.catalog;
	for (var i = 0; i < array_length(cat); i++)
	{
		if cat[i].id == id_
			return i;
	}
	return 0;
}

function rivals_select(index)
{
	var R = global.rivals;
	var c = R.catalog[index];
	R.char_id = c.id;
	R.char_name = c.name;
	R.ready = false;
	R.loading = true;
	R.requested = true;
	var _wf = file_text_open_write("pizzarivals_last.txt");
	file_text_write_string(_wf, c.id);
	file_text_close(_wf);
	R.status = "loading " + c.name + "...";
	if R.connected
		external_call(R.fn.select, c.id);
}

// The RoA character cannot be teleported (its x snaps back), so a "teleport" re-derives the Pizza Tower<->RoA offset on the RoA
// side. Until RoA has done that, every frame it sends still uses the OLD offset and would drop the puppet at the old coordinates
// (out of bounds in the new room, back at the start after a cutscene...). So: remember a generation number, freeze the puppet,
// and ignore frames until the state RoA reports has moved on to a newer generation.
function rivals_request_warp(_force = false)
{
	var R = global.rivals;
	var _p = obj_player1;
	if (instance_exists(_p))
	{
		// several things ask for a warp for one teleport (the room change, the first puppet step, the takeover...): one is enough
		if (!_force && R.frame - R.warp_frame < 12 && abs(_p.x - R.warp_x) < 24 && abs(_p.y - R.warp_y) < 24)
		{
			if (R.warp_pending)
				R.warp_until = R.frame + 3;
			return;
		}
		R.warp_x = _p.x;
		R.warp_y = _p.y;
	}
	R.warp_frame = R.frame;
	R.warp_until = R.frame + 3;
	R.warp_serial = (R.warp_serial + 1) mod 256;
	R.gen_req = R.gen;
	R.warp_pending = true;
	R.warp_since = R.frame;
	R.fvalid = false;
	while (array_length(R.fq) > 0)
	{
		array_push(R.fpool, R.fq[0].buf);
		array_delete(R.fq, 0, 1);
	}
}

// Peppino has a different collision box than the puppet: lift him out of any floor he would otherwise spawn inside.
function rivals_unstick()
{
	var _n = 0;
	while ((place_meeting(x, y, obj_solid) || place_meeting(x, y, obj_slope)) && _n < 160)
	{
		y--;
		_n++;
	}
	if _n >= 160
		y += _n;
	else if _n > 0
		y -= 1;
}

function rivals_settings_sig()
{
	var R = global.rivals;
	return string(R.enabled) + "|" + string(R.speed) + "|" + string(R.percent) + "|" + string(R.kill_pct) + "|" + string(R.show_pct) + "|" + string(R.pt_always) + "|" + string(R.show_debug) + "|" + string(R.use_queue) + "|" + string(R.cpu_targets) + "|" + string(R.show_bounds) + "|" + string(R.recenter) + "|" + string(R.input_mode) + "|" + string(R.floor_time) + "|" + string(R.show_hud) + "|" + string(R.volume) + "|" + string(R.music_volume) + "|" + string(R.enemy_atk) + "|" + string(R.weak_metal);
}

function rivals_save_settings()
{
	var R = global.rivals;
	ini_open(working_directory + "pizzarivals_settings.ini");
	ini_write_real("rivals", "enabled", R.enabled ? 1 : 0);
	ini_write_real("rivals", "speed", R.speed);
	ini_write_real("rivals", "percent", R.percent);
	ini_write_real("rivals", "kill_pct", R.kill_pct);
	ini_write_real("rivals", "enemy_atk", R.enemy_atk);
	ini_write_real("rivals", "music_volume", R.music_volume);
	ini_write_real("rivals", "weak_metal", R.weak_metal ? 1 : 0);
	ini_write_real("rivals", "show_pct", R.show_pct ? 1 : 0);
	ini_write_real("rivals", "pt_always", R.pt_always ? 1 : 0);
	ini_write_real("rivals", "show_debug", R.show_debug ? 1 : 0);
	ini_write_real("rivals", "use_queue", R.use_queue ? 1 : 0);
	ini_write_real("rivals", "cpu_targets", R.cpu_targets ? 1 : 0);
	ini_write_real("rivals", "show_bounds", R.show_bounds ? 1 : 0);
	ini_write_real("rivals", "keep_height", R.recenter ? 1 : 0);
	ini_write_real("rivals", "input_mode", R.input_mode);
	ini_write_real("rivals", "floor_time", R.floor_time);
	ini_write_real("rivals", "volume", R.volume);
	ini_write_real("rivals", "show_hud", R.show_hud ? 1 : 0);
	ini_close();
	R.saved_sig = rivals_settings_sig();
}

function rivals_load_settings()
{
	var R = global.rivals;
	if !file_exists(working_directory + "pizzarivals_settings.ini")
	{
		R.saved_sig = "";
		return;
	}
	ini_open(working_directory + "pizzarivals_settings.ini");
	R.enabled = ini_read_real("rivals", "enabled", R.enabled ? 1 : 0) != 0;
	R.speed = ini_read_real("rivals", "speed", R.speed);
	R.percent = ini_read_real("rivals", "percent", R.percent);
	R.kill_pct = ini_read_real("rivals", "kill_pct", R.kill_pct);
	R.enemy_atk = ini_read_real("rivals", "enemy_atk", R.enemy_atk);
	R.music_volume = ini_read_real("rivals", "music_volume", R.music_volume);
	R.weak_metal = ini_read_real("rivals", "weak_metal", R.weak_metal ? 1 : 0) != 0;
	R.show_pct = ini_read_real("rivals", "show_pct", R.show_pct ? 1 : 0) != 0;
	R.pt_always = ini_read_real("rivals", "pt_always", R.pt_always ? 1 : 0) != 0;
	R.show_debug = ini_read_real("rivals", "show_debug", R.show_debug ? 1 : 0) != 0;
	R.use_queue = ini_read_real("rivals", "use_queue", R.use_queue ? 1 : 0) != 0;
	R.cpu_targets = ini_read_real("rivals", "cpu_targets", R.cpu_targets ? 1 : 0) != 0;
	R.show_bounds = ini_read_real("rivals", "show_bounds", R.show_bounds ? 1 : 0) != 0;
	R.recenter = ini_read_real("rivals", "keep_height", R.recenter ? 1 : 0) != 0;
	R.input_mode = ini_read_real("rivals", "input_mode", R.input_mode);
	R.floor_time = ini_read_real("rivals", "floor_time", R.floor_time);
	R.volume = ini_read_real("rivals", "volume", R.volume);
	R.show_hud = ini_read_real("rivals", "show_hud", R.show_hud ? 1 : 0) != 0;
	ini_close();
	R.saved_sig = rivals_settings_sig();
}

function rivals_toggle()
{
	var R = global.rivals;
	if !R.available
		return;
	R.enabled = !R.enabled;
	R.hide_tv = false;
	if R.enabled
	{
		R.status = R.connected ? "enabled" : "waiting for Rivals of Aether bridge";
		if R.connected && array_length(R.catalog) > 0 && !R.ready && !R.loading
			rivals_select(rivals_catalog_index(R.char_id));
	}
	else
	{
		R.css = false;
		global.rivals_pad_block = false;
		R.in_now = 0;
		R.in_prev = 0;
		R.last_native_press = -100;
		if (instance_exists(obj_inputAssigner))
		{
			if (obj_inputAssigner.deactivated)
				rivals_reconnect_pad();
			else if (obj_inputAssigner.player_input_device[0] < 0)
			{
				// the pad slot was lost: take the first connected pad again
				for (var _gi = 0; _gi < gamepad_get_device_count(); _gi++)
				{
					if gamepad_is_connected(_gi)
					{
						obj_inputAssigner.player_input_device[0] = _gi;
						obj_inputAssigner.device_selected[0] = true;
						break;
					}
				}
			}
		}
		with obj_player1
		{
			if state == states.rivals
			{
				state = states.normal;
				visible = true;
				hsp = 0;
				vsp = 0;
				rivals_unstick();
			}
		}
	}
}

// PT keys -> protocol button mask
function rivals_pack_buttons(p)
{
	var b = 0;
	with p
	{
		if key_left < 0 b |= PRB_LEFT;
		if key_right > 0 b |= PRB_RIGHT;
		if key_up b |= PRB_UP;
		if key_down b |= PRB_DOWN;
		if key_jump2 b |= PRB_JUMP;
		if key_slap b |= PRB_ATTACK;
		if key_attack b |= PRB_SPECIAL;
		if key_groundpound2 b |= PRB_SHIELD;
		if key_taunt b |= PRB_TAUNT;
	}
	return b;
}

// Controller users get RoA's own controls (right stick smashes, charging, their RoA bindings); keyboard users get
// In native mode RoA owns the pad. Pizza Tower follows what RoA reads (shared memory), so menus, doors and the pause button
// work even when Pizza Tower itself is not receiving the controller. Called from the end of scr_getinput / scr_menu_getinput.
function rivals_poll_input()
{
	var R = global.rivals;
	if (!R.available || !R.connected || !R.enabled || !R.ready || !rivals_native_input())
	{
		R.in_now = 0;
		R.in_prev = 0;
		return false;
	}
	var _t = current_time;
	if (_t - R.in_poll_t < 8)
		return true;
	R.in_poll_t = _t;
	if (external_call(R.fn.psnap) != 0)
	{
		R.in_prev = R.in_now;
		R.in_now = external_call(R.fn.pget, 23);
	}
	return true;
}

function rivals_in_held(bit)
{
	return (global.rivals.in_now & bit) != 0;
}

function rivals_in_pressed(bit)
{
	return ((global.rivals.in_now & bit) != 0) && ((global.rivals.in_prev & bit) == 0);
}

function rivals_input_gate()
{
	var R = global.rivals;
	if (!R.css || !R.connected)
		exit;
	key_up = false; key_up2 = false; key_down = false; key_down2 = false;
	key_left = 0; key_left2 = 0; key_right = 0; key_right2 = 0;
	key_jump = false; key_jump2 = false; key_attack = false; key_attack2 = false;
	key_slap = false; key_slap2 = false; key_taunt = false; key_taunt2 = false;
	key_start = false; key_back = false; key_groundpound = false; key_groundpound2 = false;
	key_superjump = false; key_dash = false; key_dash2 = false;
}

function rivals_note_native()
{
	var R = global.rivals;
	if (key_up2 || key_down2 || key_left2 != 0 || key_right2 || key_jump || key_start || key_back)
		R.last_native_press = current_time;
}

function rivals_synth_player()
{
	if (global.rivals_pad_block)
		exit;
	if (player_index != 0 || !rivals_poll_input())
		return;
	if (current_time - global.rivals.last_native_press < 300 || rivals_native_pad_recent())
		return;
	if rivals_in_held(PRB_UP) key_up = true;
	if rivals_in_pressed(PRB_UP) key_up2 = true;
	if rivals_in_held(PRB_DOWN) key_down = true;
	if rivals_in_pressed(PRB_DOWN) key_down2 = true;
	if rivals_in_held(PRB_LEFT) key_left = -1;
	if rivals_in_pressed(PRB_LEFT) key_left2 = -1;
	if rivals_in_held(PRB_RIGHT) key_right = 1;
	if rivals_in_pressed(PRB_RIGHT) key_right2 = 1;
	if rivals_in_pressed(PRB_JUMP) key_jump = true;
	if rivals_in_held(PRB_JUMP) key_jump2 = true;
	if rivals_in_pressed(PRB_START) key_start = true;
}

function rivals_synth_menu()
{
	if (global.rivals_pad_block)
		exit;
	if !rivals_poll_input()
		return;
	rivals_note_native();
	if (current_time - global.rivals.last_native_press < 300 || rivals_native_pad_recent())
		return;
	if rivals_in_held(PRB_UP) key_up = true;
	if rivals_in_pressed(PRB_UP) key_up2 = true;
	if rivals_in_held(PRB_DOWN) key_down = true;
	if rivals_in_pressed(PRB_DOWN) key_down2 = true;
	if rivals_in_held(PRB_LEFT) key_left = -1;
	if rivals_in_pressed(PRB_LEFT) key_left2 = -1;
	if rivals_in_held(PRB_RIGHT) key_right = 1;
	if rivals_in_pressed(PRB_RIGHT) key_right2 = 1;
	if (rivals_in_pressed(PRB_JUMP)) key_jump = true;
	if (rivals_in_pressed(PRB_SPECIAL))
	{
		key_quit2 = true;       // X is Pizza Tower's "quit" button on a pad; it used to count as "select" too and started the save file
		key_quit = true;
	}
	if rivals_in_held(PRB_JUMP) key_jump2 = true;
	if rivals_in_pressed(PRB_ATTACK) key_back = true;
	if rivals_in_pressed(PRB_START) key_start = true;
}

// PT's keys translated. F8 cycles: 0 = auto (by device), 1 = always native, 2 = always translated.
function rivals_native_input()
{
	return ((global.rivals.roa_flags & 128) != 0);      // RoA saw the controller: it owns it, and Pizza Tower follows what RoA reads
	var m = global.rivals.input_mode;
	if m == 1
		return true;
	if m == 2
		return false;
	// auto: RoA tells us whether it saw gamepad activity (it polls the hardware itself, so this works even when Pizza Tower never got the pad)
	return ((global.rivals.roa_flags & 128) != 0);
}

function rivals_pt_flags(p)
{
	var fl = 0;
	// obj_pause always exists (persistent); only its .pause flag means the game is paused
	var _menu = global.rivals.menu_open || global.rivals.css || (instance_exists(obj_pause) && obj_pause.pause) || instance_exists(obj_option) || instance_exists(obj_mainmenu)
		|| room == Mainmenu || room == Realtitlescreen || room == Longintro || room == Finalintro || room == characterselect || room == Endingroom || room == Creditsroom;
	if _menu
		fl |= PRF_PAUSED;
	if rivals_native_input()
		fl |= PRF_NATIVE_INPUT;
	if global.rivals.recenter
		fl |= PRF_RECENTER;
	if global.rivals.frame <= global.rivals.reset_until
		fl |= PRF_RESET_PERCENT;
	if p.state != states.rivals
	{
		fl |= PRF_HIDDEN;
		if !(p.state == states.normal && !_menu)
			fl |= PRF_PAUSED;
	}
	return fl;
}

// Mirror nearby PT geometry into RoA (SkyCraft's CollisionField). The shared set holds a limited number of
// solids, so collect every candidate near the player and send the nearest ones first.
#macro RIVALS_MAX_SOLIDS 500
function rivals_export_solids(p)
{
	var f = global.rivals.fn;
	var pq = ds_priority_create();
	var px = p.x, py = p.y;
	var r = RIVALS_SOLID_RADIUS;
	var wl = px - r, wr = px + r, wt = py - r, wb = py + r;
	// many PT collision objects are one tile stretched over a long distance: test the bounding box, not the origin
	var _mq = ds_priority_create();
	with obj_solid
	{
		if bbox_right < wl || bbox_left > wr || bbox_bottom < wt || bbox_top > wb
			continue;
		if object_index == obj_slope || object_is_ancestor(object_index, obj_slope)
			continue;
		var _hh = bbox_bottom - bbox_top + 1;
		ds_priority_add(_mq, [bbox_left, bbox_top, bbox_right - bbox_left + 1, _hh], ((bbox_top + 100000) * 1000 + min(_hh, 999)) * 1000000 + (bbox_left + 100000));
	}
	var _cur = -1;
	while true
	{
		var _nx = ds_priority_empty(_mq) ? -1 : ds_priority_delete_min(_mq);
		if (is_array(_cur) && is_array(_nx) && _nx[1] == _cur[1] && _nx[3] == _cur[3] && _nx[0] <= _cur[0] + _cur[2])
		{
			_cur[2] = max(_cur[2], _nx[0] + _nx[2] - _cur[0]);
			continue;
		}
		if is_array(_cur)
		{
			var _d = point_distance(px, py, clamp(px, _cur[0], _cur[0] + _cur[2]), clamp(py, _cur[1], _cur[1] + _cur[3]));
			ds_priority_add(pq, [_cur[0], _cur[1], _cur[2], _cur[3], 0], _d);
		}
		if !is_array(_nx)
			break;
		_cur = _nx;
	}
	ds_priority_destroy(_mq);
	with obj_slope
	{
		if bbox_right < wl || bbox_left > wr || bbox_bottom < wt || bbox_top > wb
			continue;
		var _d = point_distance(px, py, clamp(px, bbox_left, bbox_right), clamp(py, bbox_top, bbox_bottom));
		// kind 2: rises to the right (xscale > 0), 3: rises to the left. Slopes are few and a missing piece breaks a ramp: they go first.
		ds_priority_add(pq, [bbox_left, bbox_top, bbox_right - bbox_left + 1, bbox_bottom - bbox_top + 1, (image_xscale > 0) ? 2 : 3], _d - 100000);
	}
	with obj_platform
	{
		if bbox_right < wl || bbox_left > wr || bbox_bottom < wt || bbox_top > wb
			continue;
		var _d = point_distance(px, py, clamp(px, bbox_left, bbox_right), clamp(py, bbox_top, bbox_bottom));
		var _mx = 0, _my = 0;
		if (object_index == obj_movingplatform)
		{
			_mx = hsp;
			_my = vsp;
		}
		ds_priority_add(pq, [bbox_left + _mx, bbox_top + _my, bbox_right - bbox_left + 1, bbox_bottom - bbox_top + 1, 1], _d);
	}
	with obj_grindrail
	{
		if bbox_right < wl || bbox_left > wr || bbox_bottom < wt || bbox_top > wb
			continue;
		var _d = point_distance(px, py, clamp(px, bbox_left, bbox_right), clamp(py, bbox_top, bbox_bottom));
		ds_priority_add(pq, [bbox_left, bbox_top, bbox_right - bbox_left + 1, max(8, bbox_bottom - bbox_top + 1), 1], _d);
	}
	with obj_morthook
	{
		if bbox_right < wl || bbox_left > wr || bbox_bottom < wt || bbox_top > wb
			continue;
		var _d = point_distance(px, py, clamp(px, bbox_left, bbox_right), clamp(py, bbox_top, bbox_bottom));
		ds_priority_add(pq, [bbox_left, bbox_top, bbox_right - bbox_left + 1, bbox_bottom - bbox_top + 1, 1], _d);
	}
	external_call(f.sbegin);
	var n = min(RIVALS_MAX_SOLIDS, ds_priority_size(pq));
	repeat n
	{
		var e = ds_priority_delete_min(pq);
		external_call(f.sadd, e[0], e[1], e[2], e[3], e[4]);
	}
	ds_priority_destroy(pq);
	external_call(f.scommit, global.rivals.warp_serial);
}

// Mirror the enemies near the puppet into RoA (invisible hittable stand-ins), so RoA's own hit code runs when its attacks touch them.
function rivals_export_targets(p)
{
	var R = global.rivals;
	var f = R.fn;
	external_call(f.tbegin);
	// nearest first: RoA gives its stand-in CPUs to the first few
	var _pq = ds_priority_create();
	with obj_baddie
	{
		var _bs = rivals_is_boss(id);
		if (_bs && (invincible || rivals_gun_boss(id)) && ds_map_exists(R.finished, id))
			ds_map_delete(R.finished, id);
		if (ds_map_exists(R.finished, id))
			continue;
		var _gb2 = (_bs && rivals_gun_boss(id));
		if (!_gb2 && !ds_map_exists(R.juggled, id) && (!visible || abs(x - p.x) > 900 || abs(y - p.y) > 600))
			continue;
		if (_gb2 && (abs(x - p.x) > 1800 || abs(y - p.y) > 1000))
			continue;
		if (!_gb2 && (state == states.grabbed || state == states.hit))
			continue;
		if (_bs && !ds_map_exists(R.juggled, id) && !rivals_gun_boss(id) && (!rivals_boss_vulnerable(id) || rivals_boss_poke_state(id)))
			continue;
		ds_priority_add(_pq, id, point_distance(x, y, p.x, p.y));
	}
	var _n = 0;
	while (!ds_priority_empty(_pq) && _n < 16)
	{
		var _e = ds_priority_delete_min(_pq);
		with _e
		{
			var _w = max(16, bbox_right - bbox_left + 1), _h = max(16, bbox_bottom - bbox_top + 1);
			var _hs = variable_instance_exists(id, "hsp") ? hsp : 0;
			var _vs = variable_instance_exists(id, "vsp") ? vsp : 0;
			var _tid = real(id);
			if (rivals_is_boss(id))
				_tid += 2147483648 + (rivals_gun_boss(id) ? 1073741824 : ((variable_instance_exists(id, "pizzahead") && pizzahead) ? 536870912 : 0));
			external_call(f.tadd, (bbox_left + bbox_right) / 2, (bbox_top + bbox_bottom) / 2, _w, _h, _tid, _hs, _vs);
		}
		_n++;
	}
	ds_priority_destroy(_pq);
	external_call(f.tcommit, R.warp_serial);
}

function rivals_kill_enemy(_id)
{
	if !instance_exists(_id)
		return;
	if (rivals_is_boss(_id))
	{
		with _id
			stunned = 1;
		return;
	}
	with _id
	{
		instance_create(x, y, obj_bangeffect);
		fmod_event_one_shot_3d("event:/sfx/enemies/kill", x, y);
		global.combotime = 60;
		global.heattime = 60;
		global.style += 3;
		instance_destroy();
	}
}

// What RoA did with the enemies it stands CPUs in for: juggled enemies follow their CPU, and are killed when the combo is over
// (also when RoA lets go of one mid-combo, so no enemy is left frozen).
function rivals_apply_feedback()
{
	var R = global.rivals;
	var f = R.fn;
	ds_map_clear(R.cpu_ids);
	ds_map_clear(R.cpu_pct);
	var _now = ds_map_create();
	var _n = external_call(f.fbsnap);
	for (var _i = 0; _i < _n; _i++)
	{
		var _id = external_call(f.fbget, _i, 0), _mode = external_call(f.fbget, _i, 1);
		_id = _id - floor(_id / 536870912) * 536870912;
		var _cx = external_call(f.fbget, _i, 2), _cy = external_call(f.fbget, _i, 3);
		var _pct = external_call(f.fbget, _i, 4);
		if !instance_exists(_id)
			continue;
		ds_map_set(R.cpu_ids, _id, true);
		ds_map_set(R.cpu_recent, _id, current_time);
		ds_map_set(R.cpu_pct, _id, _pct);
		if (_mode >= 1)
		{
			// every hit on an enemy refills Pizza Tower's combo countdown (no combo is added)
			var _lp = ds_map_exists(R.cpu_lastpct, _id) ? ds_map_find_value(R.cpu_lastpct, _id) : -1;
			if (_lp < 0 || _pct > _lp + 0.01)
			{
				global.combotime = 60;
				global.heattime = 60;
			}
			ds_map_set(R.cpu_lastpct, _id, _pct);
		}
		if _mode == 1
		{
			ds_map_set(_now, _id, true);
			with _id
			{
				if (state != states.stun)
				{
					state = states.stun;
					thrown = false;
					linethrown = false;
				}
				stunned = 300;
				hsp = 0;
				vsp = 0;
				invtime = 0;
				var _dx = _cx - ((bbox_left + bbox_right) / 2), _dy = _cy - ((bbox_top + bbox_bottom) / 2);
				var _sx = x, _sy = y;
				// the enemy follows its CPU, but never through the real level: a pull (a grab bringing it to a hand, a launch) stops at the
				// first solid on the way, and a start inside a solid is pushed out first (ceiling: down, floor: up)
				if (place_meeting(x, y, obj_solid))
				{
					var _k = 0;
					var _py = y;
					while (place_meeting(x, y, obj_solid) && _k < 30)
					{
						y += 2;
						_k++;
					}
					if (place_meeting(x, y, obj_solid))
					{
						y = _py;
						_k = 0;
						while (place_meeting(x, y, obj_solid) && _k < 30)
						{
							y -= 2;
							_k++;
						}
					}
					if (place_meeting(x, y, obj_solid))
					{
						x = _sx;
						y = _sy;
					}
					_sx = x;
					_sy = y;
				}
				var _tx = _sx + _dx, _ty = _sy + _dy;
				var _steps = max(1, ceil(point_distance(_sx, _sy, _tx, _ty) / 8));
				for (var _st = 1; _st <= _steps; _st++)
				{
					var _qx = lerp(_sx, _tx, _st / _steps), _qy = lerp(_sy, _ty, _st / _steps);
					if (place_meeting(_qx, _qy, obj_solid))
						break;
					x = _qx;
					y = _qy;
				}
			}
		}
		else if _mode == 2
			rivals_kill_enemy(_id);
		else if _mode == 3
		{
			// the finishing hit: Pizza Tower's own reaction (fly off, then die)
			var _ang = external_call(f.fbget, _i, 5), _pw = external_call(f.fbget, _i, 6), _dm = external_call(f.fbget, _i, 7), _dr = external_call(f.fbget, _i, 8);
			if (_ang > 360 || _ang < 0)
				_ang = 45;
			with _id
			{
				if (state == states.stun && !rivals_gun_boss(id))
					state = states.normal;
			}
			rivals_hit_any(_id, max(_dm, 8), _ang, max(_pw, 7), (_dr >= 0) ? 1 : -1);
		}
	}
	var _k = ds_map_find_first(R.juggled);
	while !is_undefined(_k)
	{
		var _nk = ds_map_find_next(R.juggled, _k);
		if !ds_map_exists(_now, _k)
		{
			ds_map_delete(R.juggled, _k);
			if instance_exists(_k) && _k.state == states.stun
				rivals_kill_enemy(_k);
		}
		_k = _nk;
	}
	_k = ds_map_find_first(_now);
	while !is_undefined(_k)
	{
		ds_map_set(R.juggled, _k, true);
		_k = ds_map_find_next(_now, _k);
	}
	ds_map_destroy(_now);
}

// A RoA hitbox hurts a PT enemy using the same state PT's own attacks use (see scr_pistolcollision).
function rivals_blog(s)
{
	var _f = file_text_open_append(working_directory + "pizzarivals_boss.log");
	file_text_write_string(_f, string(global.rivals.frame) + " " + s);
	file_text_writeln(_f);
	file_text_close(_f);
}

// ---- bosses ----
// A boss only takes damage while Pizza Tower itself would let Peppino hurt it (it flashes white: its "invincible" is off).
function rivals_is_boss(b)
{
	var _o = b.object_index;
	return (_o == obj_pepperman || _o == obj_vigilanteboss || _o == obj_noiseboss || _o == obj_fakepepboss || _o == obj_pizzafaceboss || _o == obj_pizzafaceboss_p2 || _o == obj_pizzafaceboss_p3);
}

// the fights that need Peppino's gun: the rival does 10% and a finishing blow, which then counts as a shot
function rivals_gun_boss(b)
{
	return ((b.object_index == obj_vigilanteboss && !b.pizzahead) || b.object_index == obj_pizzafaceboss_p2);
}

function rivals_boss_vulnerable(b)
{
	return (!b.invincible && b.state != states.hit && b.state != states.grabbed && b.state != states.phase1hurt);
}

// states in which a boss reacts to a Peppino body attack by itself (Pepperman shrunk or thinking, the fake Peppino)
function rivals_boss_poke_state(b)
{
	if (b.object_index == obj_pepperman)
		return ((b.state == states.mini && b.ministate != states.transition) || (!b.pizzahead && b.wastedhits == 9 && b.phase == 1 && b.state == states.contemplate));
	// the hit that starts a phase change (Noise, Pizzaface): Peppino's body attack triggers the cutscene
	if (b.object_index == obj_noiseboss)
		return (!b.pizzahead && (b.state == states.walk || (b.state == states.stun && !b.savedthrown)) && b.flickertime <= 0 && b.wastedhits == 7);
	if (b.object_index == obj_pizzafaceboss)
		return (b.elitehit == 1 && b.state == states.stun && b.savedthrown == b.thrown && !b.savedthrown);
	return false;
}

// Make Pizza Tower run an object's own "the player attacks me" reaction, as if Peppino had hit it with a body attack
function rivals_poke(inst, frames)
{
	var R = global.rivals;
	if (ds_map_exists(R.boss_cd, inst) && R.frame < ds_map_find_value(R.boss_cd, inst))
		return false;
	ds_map_set(R.boss_cd, inst, R.frame + frames);
	with obj_player1
	{
		var _ik = instakillmove;
		instakillmove = true;
		state = states.handstandjump;
		with (inst)
			event_perform(ev_collision, obj_player);
		instakillmove = _ik;
		if (state != states.phase1hurt && state != states.actor)
			state = states.rivals;
	}
	return true;
}

// a shot from Peppino's gun, for the things that only the gun hurts
function rivals_shoot(inst, dmg, dir)
{
	var _ok = false;
	var _bl = instance_create(inst.x, inst.y, obj_pistolbullet);
	_bl.image_xscale = (dir >= 0) ? 1 : -1;
	with _bl
	{
		_ok = scr_pistolhit(inst, dmg);
	}
	if instance_exists(_bl)
		instance_destroy(_bl);
	return _ok;
}

// the blow of a gun boss: a normal hit (Vigilante's health goes down through his own hit state), or the gun shot that starts a phase change
function rivals_gun_try(b, dir)
{
	var R = global.rivals;
	if (b.object_index == obj_vigilanteboss && !(b.elitehit <= 1 && b.phase == 1))
	{
		var _ok = rivals_hit_baddie(b, 24, 40, 12, dir);
		ds_map_delete(R.finished, b);
		return _ok;
	}
	return rivals_shoot(b, 99, dir);
}

function rivals_fakepep_pokable(b)
{
	return (b.staggerbuffer <= 0 && b.flickertime <= 0 && b.visible && (b.state == states.walk || (b.state == states.jump && b.sprite_index == spr_fakepeppino_bodyslamstart) || (b.state == states.freefall && b.sprite_index == spr_fakepeppino_bodyslamland) || (b.state == states.mach2 && b.attackspeed < 18) || b.state == states.Sjumpprep || (b.state == states.throwing && b.sprite_index != spr_fakepeppino_flailing)));
}

function rivals_hit_any(b, dmg, ang, pw, dir)
{
	var R = global.rivals;
	if (!rivals_is_boss(b))
		return rivals_hit_baddie(b, dmg, ang, pw, dir);
	if (R.boss_log_n < 300)
	{
		R.boss_log_n++;
		rivals_blog(object_get_name(b.object_index) + " state=" + string(b.state) + " inv=" + string(b.invincible) + " pizzahead=" + string(variable_instance_exists(b, "pizzahead") ? b.pizzahead : -1) + " elitehit=" + string(b.elitehit) + " vulnerable=" + string(rivals_boss_vulnerable(b)) + " poke=" + string(rivals_boss_poke_state(b)));
	}
	if (variable_instance_exists(b, "pizzahead") && b.pizzahead && !rivals_gun_boss(b) && rivals_boss_vulnerable(b))
	{
		// the final fight is won with a barrage: hand the boss to the game's own super-grab (zoom, punches, finishing blow, cutscenes)
		obj_player1.baddiegrabbedID = b;
		b.grabbedby = 1;
		with b
			scr_boss_grabbed();
		return true;
	}
	// the cutscene before a phase change waits for the final hit: the rival gets a short window to land it
	if (b.state == states.phase1hurt)
	{
		if (variable_instance_exists(b, "rv_window") && b.rv_window > 0 && !b.rv_done)
		{
			b.rv_done = true;
			return true;
		}
		return false;
	}
	if (rivals_boss_poke_state(b))
		return rivals_poke(b, 24);
	if (b.object_index == obj_fakepepboss && !rivals_boss_vulnerable(b))
	{
		if (!rivals_fakepep_pokable(b))
			return false;
		return rivals_poke(b, 24);
	}
	// the boss rush: Vigilante is normally stunned by a thrown Gustavo; any hit of the rival does that to him
	if (b.object_index == obj_vigilanteboss && variable_instance_exists(b, "pizzahead") && b.pizzahead && !rivals_boss_vulnerable(b) && b.state != states.KO && b.state != states.supergrab && b.state != states.hit)
	{
		if (ds_map_exists(R.boss_cd, b) && R.frame < ds_map_find_value(R.boss_cd, b))
			return false;
		ds_map_set(R.boss_cd, b, R.frame + 40);
		with b
		{
			state = states.stun;
			stunned = 1000;
			thrown = false;
			savedthrown = false;
			image_xscale = -((dir >= 0) ? 1 : -1);
			hsp = -image_xscale * 8;
			vsp = -4;
		}
		return true;
	}
	if (!rivals_gun_boss(b) && !rivals_boss_vulnerable(b))
		return false;
	if (rivals_gun_boss(b))
	{
		if (!rivals_gun_try(b, dir))
			ds_map_set(R.gun_pend, b, [dir, R.frame + 150]);
		return true;
	}
	if (variable_instance_exists(b, "pizzahead") && b.pizzahead)
		b.pizzahead_subhp = 0;
	return rivals_hit_baddie(b, dmg, ang, pw, dir);
}

// ---- per-level fixes ----
// Called once whenever a room starts. Add a case per room (room_get_name(room)) for anything that needs special handling there.
function rivals_level_fixes()
{
	var R = global.rivals;
	var _rn = room_get_name(room);
	var _info = _rn + ":";
	var _watch = [obj_door, obj_doornexthub, obj_spaceshuttle, obj_destructibles, obj_metalblock, obj_ratblock, obj_rattumble, obj_slope, obj_platform, obj_hubelevator, obj_forknight, obj_boxofpizza, obj_rocket];
	var _names = ["door", "elevator", "shuttle", "destructibles", "metalblock", "ratblock", "rattumble", "slope", "platform", "hubelevator", "forknight", "box", "rocket"];
	for (var _i = 0; _i < array_length(_watch); _i++)
	{
		var _n = instance_number(_watch[_i]);
		if (_n > 0)
			_info += " " + _names[_i] + "=" + string(_n);
	}
	global.rivals_room_info = _info;
	// priests take transformations away: the rival has none, so they become the flying point rats
	with obj_priest
	{
		var _cx = (bbox_left + bbox_right) / 2, _cy = (bbox_top + bbox_bottom) / 2;
		instance_create(_cx, _cy, obj_ratfairy);
		instance_destroy();
	}
	switch (_rn)
	{
		case "war_1":
			with obj_shotgun
			{
				instance_create(x, y, obj_nuketerminal);
				instance_destroy();
			}
			break;
		default:
			break;
	}
}

function rivals_hit_baddie(b, damage, angle, power_, facing)
{
	ds_map_set(global.rivals.finished, b, true);
	with b
	{
		if state == states.hit || state == states.grabbed || invtime > 0
			return false;
		instance_create(x, y, obj_bangeffect);
		fmod_event_one_shot_3d("event:/sfx/enemies/kill", x, y);
		state = states.hit;
		linethrown = true;
		hitX = x;
		hitY = y;
		hitLag = 4 + min(damage, 20) * 0.4;
		thrown = true;
		mach2 = false;
		hp -= max(1, damage div 8);
		var ang = (facing > 0) ? angle : 180 - angle;
		var spd = clamp(power_ * 2.4, 6, 28);
		hithsp = lengthdir_x(spd, ang);
		hitvsp = -abs(lengthdir_y(spd, ang)) - 3;
		image_xscale = (hithsp >= 0) ? -1 : 1;
		global.combotime = 60;
		global.heattime = 60;
		global.style += 3;
	}
	return true;
}

function rivals_process_hitboxes()
{
	var R = global.rivals;
	var _mt = R.weak_metal ? 0 : RIVALS_METAL_DAMAGE;
	var f = R.fn;
	var n = external_call(f.hsnap);
	if (R.bubble && n > 0)
		rivals_bubble_pop();
	var live = ds_map_create();
	for (var i = 0; i < n; i++)
	{
		var hid = external_call(f.hget, i, 0);
		var hx = external_call(f.hget, i, 1), hy = external_call(f.hget, i, 2);
		var hw = external_call(f.hget, i, 3), hh = external_call(f.hget, i, 4);
		var dmg = external_call(f.hget, i, 5), ang = external_call(f.hget, i, 6), pw = external_call(f.hget, i, 7);
		var connected = false;
		// destructible blocks (bricks, iron blocks...): any hitbox breaks them; iron needs a strong hit
		with obj_destructibles
		{
			if rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0
			{
				if (object_index == obj_metalblock && dmg < _mt)
					continue;
				instance_destroy();
			}
		}
		with obj_metalblock
		{
			if (dmg >= _mt && rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		// rat blocks ("stupid rats", rat tumbles and their variants) are normally killed by explosions: a strong hit does it too
		if (dmg >= _mt)
		{
			var _rats = [obj_ratblock, obj_rattumble, obj_rattumble_big];
			for (var _ri = 0; _ri < array_length(_rats); _ri++)
			{
				with _rats[_ri]
				{
					if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
						instance_destroy();
				}
			}
		}
		// TNT goes off to any attack; the timed walls and the sausage shop keepers give way too
		with obj_tntblock
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		with obj_iceblock_breakable
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
			{
				instance_destroy();
				R.solid_timer = 0;
			}
		}
		with obj_bazooka
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		with obj_tvtrap
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		with obj_nuketerminal
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		// the toppin monsters (kids' party): only once they are awake
		var _mons = [obj_monster, obj_robotmonster, obj_blobmonster, obj_pineapplemonster, obj_hillbillymonster, obj_puppetmonster];
		for (var _mi = 0; _mi < array_length(_mons); _mi++)
		{
			with _mons[_mi]
			{
				if (variable_instance_exists(id, "state") && state != states.robotidle && state != states.robotintro && rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
					instance_destroy();
			}
		}
		with obj_timedgate
		{
			if (activated && rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
			{
				instance_destroy();
				R.solid_timer = 0;
			}
		}
		with obj_electricpotato
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		with obj_clerk
		{
			if (!death && rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
			{
				death = true;
				fmod_event_one_shot_3d("event:/sfx/enemies/kill", x, y);
				ds_list_add(global.baddieroom, id);
				global.combotime = 60;
				instance_create(x, y, obj_bangeffect);
				instance_create(x, y, obj_genericpoofeffect);
				with (instance_create(x, y, obj_sausageman_dead))
				{
					image_xscale = -R.roa_dir;
					sprite_index = spr_clerkdead;
					hsp = R.roa_dir * 10;
				}
				instance_destroy();
			}
		}
		// the cheese grater barrier and ghost blocks break like iron blocks
		if (dmg >= _mt)
		{
			var _gb = [obj_ghostwall, obj_ghostblock, obj_hungrypillar, obj_shotgunblock, obj_asteroid];
			for (var _gi = 0; _gi < array_length(_gb); _gi++)
			{
				with _gb[_gi]
				{
					if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
					{
						instance_destroy();
						R.solid_timer = 0;
					}
				}
			}
		}
		// the targets of the tutorial tower (their block goes with them)
		with obj_tutorialtarget
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
			{
				instance_destroy();
				R.solid_timer = 0;
			}
		}
		// Pepperman's painters and chisel blocks, the vigilante's ghost and cutouts
		with obj_peppermanartdude
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				instance_destroy();
		}
		with obj_pepper_marbleblock
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0)
				rivals_poke(id, 14);
		}
		var _gt = [obj_johnecheese, obj_targetguy, obj_vigilantecow];
		for (var _gi2 = 0; _gi2 < array_length(_gt); _gi2++)
		{
			with _gt[_gi2]
			{
				if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0 && !ds_map_exists(R.boss_cd, id))
				{
					ds_map_set(R.boss_cd, id, R.frame + 10);
					rivals_shoot(id, 99, R.roa_dir);
				}
			}
		}
		with obj_baddie
		{
			if ds_map_exists(R.cpu_ids, id) || (ds_map_exists(R.cpu_recent, id) && current_time - ds_map_find_value(R.cpu_recent, id) < 700)
			{
				continue;
			}
			if (rivals_is_boss(id) && rivals_gun_boss(id))
				continue;
			var key = string(hid) + "_" + string(id);
			if ds_map_exists(R.hit_pairs, key)
			{
				ds_map_set(live, key, true);
				continue;
			}
			if rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2) != 0
			{
				if rivals_hit_any(id, dmg, ang, pw, R.roa_dir)
				{
					ds_map_set(R.hit_pairs, key, true);
					ds_map_set(live, key, true);
					connected = true;
				}
			}
		}
		if connected
			external_call(f.hitok, hid);
	}
	// forget pairs whose hitbox is gone so a later attack can hit again
	var k = ds_map_find_first(R.hit_pairs);
	while !is_undefined(k)
	{
		var nk = ds_map_find_next(R.hit_pairs, k);
		if !ds_map_exists(live, k)
			ds_map_delete(R.hit_pairs, k);
		k = nk;
	}
	ds_map_destroy(live);
}

// States Pizza Tower's hazards switch the player into (transformations, boosts): the rival never takes them.
function rivals_forbidden_state(s)
{
	return (s == states.fireass || s == states.firemouth || s == states.ghost || s == states.mort || s == states.mortjump || s == states.mortattack || s == states.morthook
		|| s == states.slipbanan || s == states.trashroll || s == states.trashjump || s == states.trashjumpprep || s == states.mach1 || s == states.mach2 || s == states.mach3
		|| s == states.golf || s == states.tumble || s == states.rocket || s == states.rocketslide || s == states.barrel || s == states.barreljump || s == states.barrelslide
		|| s == states.barrelclimbwall || s == states.slipnslide || s == states.boxxedpep || s == states.boxxedpepjump || s == states.boxxedpepspin || s == states.cheeseball
		|| s == states.cheesepep || s == states.cheesepepjump || s == states.cheesepepstick || s == states.knightpep || s == states.knightpepslopes || s == states.ratmount
		|| s == states.ratmountjump || s == states.ratmountgrind || s == states.ratmountladder || s == states.ratmountballoon || s == states.antigrav || s == states.grind
		|| s == states.bombpep || s == states.jetpackjump);
}

function rivals_active()
{
	return (global.rivals.enabled && global.rivals.ready);
}

// commands to RoA's character: 8 vertical speed, 9 horizontal speed, 10 nudge sideways, 13 hurt pose, 14 next palette
function rivals_cmd(a, b)
{
	if (global.rivals.connected)
		external_call(global.rivals.fn.cmd, 6, a, b);
}

// springs, mushrooms, jetpacks: airborne at that speed, no damage, no hitstun
function rivals_bounce(v)
{
	rivals_cmd(8, v);
}

function rivals_push(h)
{
	rivals_cmd(9, h);
}

// fans and conveyor belts: keep pushing
function rivals_fan(v)
{
	if (global.rivals.roa_vsp > v)
		rivals_cmd(8, v);
}

function rivals_nudge(dx)
{
	rivals_cmd(10, dx);
}

// riding a moving platform: go where it goes
function rivals_carry(h, v)
{
	if (h != 0)
		rivals_cmd(10, h);
	if (v != 0)
		rivals_cmd(15, v);
}

// a stun without damage or knockback (the red tomato guys)
function rivals_stun(frames)
{
	global.rivals.stun_until = global.rivals.frame + frames;
}

// the mach-run water: springs the rival up like a mushroom
function rivals_water_bounce(p)
{
	var R = global.rivals;
	if (R.water_cd > R.frame)
		return;
	R.water_cd = R.frame + 20;
	fmod_event_one_shot("event:/sfx/misc/watersplash");
	instance_create(p.x, p.y + 20, obj_piranneapplewater);
	rivals_bounce(-15);
}

// Pizza Tower moves the puppet itself (hands, Mr Pinch): show the RoA character where the puppet is, in the hurt pose
function rivals_drive(hurt)
{
	var R = global.rivals;
	if (!rivals_active())
		return;
	R.drive_until = R.frame + 2;
	R.drive_hurt = hurt;
}

// the rival is "Gustavo" in the gustavo areas (the music asks for it); it looks and plays like the rival all the same
function rivals_gus_switch(on)
{
	var R = global.rivals;
	R.gus = on;
	rivals_release(0, 0);
}

// Back to the rival after a pipe / hazard: the RoA side follows the puppet's new place and gets this speed
function rivals_release(h, v)
{
	var R = global.rivals;
	if (state == states.rivals)
		exit;
	R.rel_h = h;
	R.rel_v = v;
	R.rel_pending = true;
	R.drive_prev = true;
	state = states.rivals;
	visible = true;
}

function rivals_next_palette()
{
	var R = global.rivals;
	if (R.pal_cd > R.frame)
		return;
	R.pal_cd = R.frame + 40;
	rivals_cmd(14, 1);
}

// the olives: the rival floats up in a bubble until any attack pops it
function rivals_bubble_start(p, olive)
{
	var R = global.rivals;
	if (R.bubble)
		return;
	R.bubble = true;
	R.bub_v = 0;
	olive.cooldown = 50;
	fmod_event_one_shot("event:/sfx/antigrav/start");
	fmod_event_one_shot_3d("event:/sfx/misc/bubblestation", p.x, p.y);
	with (instance_create(p.x, p.y, obj_antigravbubble))
		playerid = p;
}

function rivals_bubble_pop()
{
	var R = global.rivals;
	if (!R.bubble)
		return;
	R.bubble = false;
	with obj_antigravbubble
		instance_destroy();
	fmod_event_one_shot_3d("event:/sfx/antigrav/end", obj_player1.x, obj_player1.y);
}

// An environment hazard (lava, cows) hurts the rival the RoA way: percent plus a knockback of our choosing.
function rivals_env_hit(p, dirx, ang, power_)
{
	var R = global.rivals;
	if (R.env_cd > 0 || rivals_dodging() || p.invtime > 0)
		return false;
	R.env_cd = 60;
	external_call(R.fn.hurt, R.enemy_atk, ang, power_, dirx);
	with p
	{
		instance_create(x, y, obj_bangeffect);
		invtime = 60;
		flash = true;
		alarm[8] = 60;
	}
	return true;
}

// Lava: true when the toucher is the rival (it takes 25% and flies up instead of burning into Peppino's fireass).
function rivals_lava_touch(p)
{
	if (p == noone || !instance_exists(p) || p.state != states.rivals)
		return false;
	rivals_env_hit(p, 0, 90, 11);
	return true;
}

// Called from obj_rivals_controller End Step, after the player stepped.
function rivals_step()
{
	var R = global.rivals;
	if !R.available
		return;
	rivals_music_step();
	if R.dev_room != "" && R.frame >= 0 && instance_exists(obj_player1) && !instance_exists(obj_fadeout) && current_time > 6000
	{
		var _r = asset_get_index(R.dev_room);
		R.dev_room = "";
		R.enabled = true;
		R.dev_forced = true;
		with obj_player
		{
			targetDoor = "A";
			lastroom = room;
			targetRoom = _r;
		}
		instance_create(0, 0, obj_fadeout);
	}
	if keyboard_check_pressed(vk_f6)
		rivals_toggle();
	if keyboard_check_pressed(vk_f9)
		R.recenter = !R.recenter;
	if keyboard_check_pressed(vk_f10)
		R.show_bounds = !R.show_bounds;
	if keyboard_check_pressed(vk_f7)
		R.menu_open = !R.menu_open && array_length(R.catalog) > 0;
	if R.menu_open
		rivals_menu_input();
	if !R.connected
	{
		if --R.retry <= 0
		{
			R.retry = 60;
			if external_call(R.fn.open)
			{
				R.connected = true;
				R.status = "connected";
				if R.enabled && array_length(R.catalog) > 0
					rivals_select(rivals_catalog_index(R.char_id));
			}
		}
		return;
	}
	var f = R.fn;
	external_call(f.beat);
	R.frame++;
	// events from RoA
	repeat 8
	{
		var ev = external_call(f.evpoll);
		if ev == 0
			break;
		if ev == PRE_CSS_STATE
		{
			var _s = external_call(f.evstr);
			R.css = (_s == "1");
			if R.css
			{
				R.ready = false;
				R.status = "pick a character in the Rivals window (Practice to confirm)";
			}
		}
		else if ev == PRE_CHAR_READY
		{
			var _id = external_call(f.evstr);
			var _bar = string_pos("|", _id);
			if _bar > 0
			{
				R.char_name = string_copy(_id, _bar + 1, string_length(_id) - _bar);
				R.char_id = string_copy(_id, 1, _bar - 1);
			}
			var _wi = rivals_catalog_index(R.char_id);
			if (array_length(R.catalog) > _wi && R.catalog[_wi].id == R.char_id)
				R.char_name = R.catalog[_wi].name;
			R.css = false;
			R.requested = true;
			rivals_set_speed(R.speed);
			rivals_set_percent(R.percent);
			rivals_set_kill_percent(R.kill_pct);
			external_call(R.fn.cmd, 6, 5, R.pt_always ? 1 : 0);
			external_call(R.fn.cmd, 6, 6, R.floor_time);
			external_call(R.fn.cmd, 6, 7, R.show_hud ? 1 : 0);
			if !R.cpu_targets
				external_call(R.fn.cmd, 6, 2, 0);
			R.ready = true;
			R.loading = false;
			external_call(R.fn.focus);      // RoA's window just appeared (and went away again): give Pizza Tower the controller back
			R.status = "playing as " + R.char_name;
			if (instance_exists(obj_player1))
			{
				R.sw_x = obj_player1.x;
				R.sw_y = obj_player1.y;
				R.sw_room = room;
				R.sw_n = 240;
				R.sw_tries = 0;
			}
			rivals_request_warp(true);
		}
		else if ev == PRE_CHAR_FAILED
		{
			R.loading = false;
			R.status = "failed to load: " + external_call(f.evstr);
		}
	}
	global.rivals_pad_block = R.css;
	if R.enabled && !R.requested && array_length(R.catalog) > 0
		rivals_select(rivals_catalog_index(R.char_id));
	if !instance_exists(obj_option)
	{
		if (R.pending_pick != -2)
		{
			var _pp = R.pending_pick;
			R.pending_pick = -2;
			if (_pp == -1)
				rivals_open_css();
			else if (_pp != rivals_catalog_index(R.char_id) || !R.ready)
				rivals_menu_pick(_pp);
		}
		if (R.pending_toggle && !(instance_exists(obj_pause) && obj_pause.pause))
		{
			R.pending_toggle = false;
			rivals_toggle();
		}
	}
	if (R.frame mod 20 == 0)
	{
		external_call(f.astart);
		var _g = (R.enabled && R.ready) ? global.option_master_volume * global.option_sfx_volume * R.volume * 0.6 : 0;
		external_call(f.again, _g);
	}
	if (instance_exists(obj_inputAssigner) && obj_inputAssigner.deactivated)
		rivals_reconnect_pad();
	if (R.pending_peppino)
	{
		R.pending_peppino = false;
		if (instance_exists(obj_player1))
		{
			with obj_player1
			{
				x = roomstartx;
				y = roomstarty;
				hsp = 0;
				vsp = 0;
				xprevious = x;
				yprevious = y;
			}
			R.last_px = obj_player1.x;
			R.last_py = obj_player1.y;
			R.safe_x = obj_player1.x;
			R.safe_y = obj_player1.y;
			R.safe_room = room;
			R.oob_n = 0;
			R.rel_pending = false;
			if (R.enabled && R.connected)
				rivals_request_warp(true);
		}
	}
	if !R.enabled || !instance_exists(obj_player1)
	{
		// no player (title screen, menus): RoA must not act on the controller
		if (R.connected)
		{
			external_call(f.input, R.frame, 0, 0, 0);
			external_call(f.state, R.frame, PRF_PAUSED | PRF_HIDDEN, room, 0, 0, 0, 0, R.warp_serial);
		}
		return;
	}
	var p = obj_player1;
	if (R.env_cd > 0)
		R.env_cd--;
	if (R.was_rival && rivals_forbidden_state(p.state))
	{
		p.state = states.rivals;
		if (point_distance(p.x, p.y, R.last_px, R.last_py) > 24)
			rivals_request_warp();
	}
	R.was_rival = (p.state == states.rivals);
	// Pizza Tower moves the puppet itself (hands, Mr Pinch: shown in the hurt pose; pipes, teleporters: hidden): the RoA side re-finds it afterwards
	var _inA = (p.state != states.rivals && (p.state == states.stringfall || p.state == states.stringjump || p.state == states.stringfling || R.drive_until >= R.frame));
	var _inB = (p.state == states.tube || p.state == states.teleport);
	R.drive_vis = (R.enabled && R.ready && _inA);
	if (_inA || _inB)
		R.drive_prev = true;
	if (R.frame < R.stun_until && R.ready)
		rivals_cmd(13, 1);
	if (R.ready && (room == boss_vigilante || room == boss_pizzaface) && instance_exists(obj_pistolpickup))
	{
		// the rival has no hands for it: the gun box just goes away and the fight begins (the rival's hits count as shots)
		with obj_pistolpickup
			instance_destroy();
		global.pistol = true;
		fmod_event_one_shot_3d("event:/sfx/misc/breakblock", p.x, p.y);
		if (room == boss_pizzaface)
		{
			with obj_music
			{
				if music != -4
					fmod_event_instance_set_parameter(music.event, "state", 1.4, true);
			}
		}
	}
	if ((R.frame mod 10) == 0 && p.state == states.rivals)
	{
		// a boss grabbed for Peppino's barrage that never comes: let it go
		with obj_baddie
		{
			if (state == states.supergrab && rivals_is_boss(id))
			{
				state = states.stun;
				stunned = 1;
				thrown = false;
			}
		}
	}
	var _gk = ds_map_find_first(R.gun_pend);
	while (!is_undefined(_gk))
	{
		var _gn = ds_map_find_next(R.gun_pend, _gk);
		var _ge = ds_map_find_value(R.gun_pend, _gk);
		if (!instance_exists(_gk) || R.frame > _ge[1] || rivals_gun_try(_gk, _ge[0]))
			ds_map_delete(R.gun_pend, _gk);
		_gk = _gn;
	}
	if (R.bubble)
	{
		if (p.state != states.rivals || !R.ready)
			R.bubble = false;
		else
		{
			R.bub_v = max(R.bub_v - 0.35, -12);
			if (place_meeting(p.x, p.y - 28, obj_solid))
				R.bub_v = 3;
			rivals_bounce(R.bub_v);
		}
	}
	if (instance_exists(obj_pause) && obj_pause.pause)
	{
		// game paused (our controller stays alive so events from RoA, like a character picked in its window, still arrive)
	external_call(f.tbegin);    // no enemies while paused: the stand-in CPUs must not stay attached to the last ones
	external_call(f.tcommit, R.warp_serial);
		external_call(f.input, R.frame, 0, 0, 0);
		external_call(f.state, R.frame, rivals_pt_flags(p), room, camera_get_view_x(view_camera[0]), camera_get_view_y(view_camera[0]), p.x, p.y + R.roa_hh, R.warp_serial);
		return;
	}
	if (R.pending_reset)
	{
		R.pending_reset = false;
		if (R.start_room == room && p.state == states.rivals)
		{
			with p
			{
				x = R.start_x;
				y = R.start_y;
				hsp = 0;
				vsp = 0;
			}
			R.last_px = R.start_x;
			R.last_py = R.start_y;
			rivals_request_warp();
			while (array_length(R.fq) > 0)
			{
				array_push(R.fpool, R.fq[0].buf);
				array_delete(R.fq, 0, 1);
			}
		}
	}
	R.hide_tv = (p.state == states.rivals);     // obj_tv's draw event skips the TV picture (the Pizza Time timer stays)
	// a camera shake that outlives its cause (PT's own states ended it for Peppino, the puppet never does): cap it
	if (p.state == states.rivals && instance_exists(obj_camera) && !global.panic)
	{
		if obj_camera.shake_mag > 0
			R.shake_frames++;
		else
			R.shake_frames = 0;
		if R.shake_frames > 45
		{
			obj_camera.shake_mag = 0;
			R.shake_frames = 0;
		}
	}
	if (global.rivals_trace_on)
	{
		if (global.rivals_trace == -1)
			global.rivals_trace = file_text_open_append(working_directory + "pizzarivals_trace_out.txt");
		file_text_write_string(global.rivals_trace, string(R.frame) + " st=" + string(p.state) + " keys=" + string(rivals_pack_buttons(p)) + " flags=" + string(R.roa_flags) + " roa=(" + string(R.roa_x) + "," + string(R.roa_y) + ") hsp=" + string(R.roa_hsp) + " pt=(" + string(p.x) + "," + string(p.y) + ") cam=(" + string(camera_get_view_x(view_camera[0])) + "," + string(camera_get_view_y(view_camera[0])) + ") ready=" + string(R.ready) + " ptflags=" + string(rivals_pt_flags(p)) + " pause=" + string(instance_exists(obj_pause)) + " menu=" + string(R.menu_open));
		file_text_writeln(global.rivals_trace);
		if (R.frame mod 60 == 0)
		{
			file_text_close(global.rivals_trace);
			global.rivals_trace = -1;
		}
	}
	// take over once PT hands control back (also after doors, cutscenes)
	if R.takeover_wait > 0
		R.takeover_wait--;
	if R.yield_cd > 0
		R.yield_cd--;
	if R.ready && R.takeover_wait <= 0 && room != Realtitlescreen && (p.state == states.normal || (R.dev_forced && p.state == states.titlescreen)) && !R.menu_open
	{
		if (R.start_room != room)
		{
			R.start_room = room;
			R.start_x = p.x;
			R.start_y = p.y;
		}
		p.state = states.rivals;
		if (point_distance(p.x, p.y, R.last_px, R.last_py) > 24)
		{
			rivals_request_warp();
		}
	}
	if (R.frame mod 30 == 0 && rivals_settings_sig() != R.saved_sig)
		rivals_save_settings();
	if room != R.last_room
	{
		R.last_room = room;
		R.takeover_wait = 25;      // let PT place the player at the entrance before we take over
		R.elev_active = false;
		R.bubble = false;
		R.drive_prev = false;
		ds_map_clear(R.finished);
		ds_map_clear(R.cpu_lastpct);
		if (p.state == states.actor)
			p.state = states.normal;
		// percent only resets when a level starts or ends (global.leveltorestart changes, or this is the level's first room)
		if (global.leveltorestart != R.last_level || room == global.leveltorestart)
		{
			R.reset_until = R.frame + 6;
			R.gus = false;
		}
		R.last_level = global.leveltorestart;
		R.solid_timer = 0;
		rivals_level_fixes();
		rivals_request_warp();
	}
	// PT teleported the puppet (door exits, scripted moves): follow it
	if p.state == states.rivals && R.ready && (abs(p.x - R.last_px) > 24 || abs(p.y - R.last_py) > 24) && R.frame > R.warp_until + 4 && !instance_exists(obj_fadeout)
	{
		rivals_request_warp();
	}
	rivals_export_targets(p);
	var _mpn = false;
	with obj_movingplatform
	{
		if (abs(x - p.x) < 700 && abs(y - p.y) < 500)
			_mpn = true;
	}
	var _dn = instance_number(obj_destructibles) + instance_number(obj_metalblock) + instance_number(obj_ratblock) + instance_number(obj_iceblock_breakable) + instance_number(obj_timedgate) + instance_number(obj_ghostblock) + instance_number(obj_ghostwall);
	if (_dn != R.dn_last)
	{
		R.dn_last = _dn;
		R.solid_timer = 0;
	}
	if --R.solid_timer <= 0
	{
		R.solid_timer = _mpn ? 1 : 4;
		rivals_export_solids(p);
	}
	var warp = R.warp_serial;
	external_call(f.input, R.frame, rivals_pack_buttons(p), 0, 0);
	external_call(f.state, R.frame, rivals_pt_flags(p), room, camera_get_view_x(view_camera[0]), camera_get_view_y(view_camera[0]), p.x, p.y + R.roa_hh, warp);
	if external_call(f.psnap) != 0
	{
		R.roa_x = external_call(f.pget, 0);
		R.roa_y = external_call(f.pget, 1);
		R.roa_hsp = external_call(f.pget, 2);
		R.roa_vsp = external_call(f.pget, 3);
		R.roa_dir = external_call(f.pget, 4);
		R.roa_state = external_call(f.pget, 5);
		R.roa_flags = external_call(f.pget, 7);
		R.roa_percent = external_call(f.pget, 10);
		R.roa_fx = external_call(f.pget, 11);
		R.roa_fy = external_call(f.pget, 12);
		R.vx = external_call(f.pget, 13);
		R.vy = external_call(f.pget, 14);
		R.vw = external_call(f.pget, 15);
		R.vh = external_call(f.pget, 16);
		R.bzl = external_call(f.pget, 17);
		R.bzr = external_call(f.pget, 18);
		R.bzt = external_call(f.pget, 19);
		R.bzb = external_call(f.pget, 20);
		R.rfps = external_call(f.pget, 21);
		R.rtick = external_call(f.pget, 22);
		R.gen = external_call(f.pget, 24);
		if (R.warp_pending && (R.gen > R.gen_req || R.frame - R.warp_since > 240))
			R.warp_pending = false;
		if (R.ready && (R.rfps < 56 || R.rtick < 56))
			R.slow_until = current_time + 2500;
	}
	if (R.rel_pending && p.state == states.rivals && !R.warp_pending && R.frame > R.warp_until + 2)
	{
		R.rel_pending = false;
		rivals_push(R.rel_h);
		rivals_bounce(R.rel_v);
	}
	// out of the level (a dodge that went wrong, a pit): back to the last safe spot
	if (p.state == states.rivals && R.ready && !R.warp_pending && R.frame > R.warp_until + 4)
	{
		if (R.safe_room != room)
		{
			R.safe_room = room;
			R.safe_x = p.x;
			R.safe_y = p.y;
		}
		if (R.roa_state == 8 || R.roa_state == 33 || R.roa_state == 34)
			R.dodge_frame = R.frame;
		var _near_exit = false;
		with p
		{
			if ((instance_exists(obj_hallway) && distance_to_object(instance_nearest(x, y, obj_hallway)) < 900) || (instance_exists(obj_verticalhallway) && distance_to_object(instance_nearest(x, y, obj_verticalhallway)) < 900))
				_near_exit = true;
		}
		var _oob = (!_near_exit && (p.x < -160 || p.x > room_width + 160 || p.y > room_height + 400 || p.y < -1200));
		R.oob_n = _oob ? R.oob_n + 1 : 0;
		if (R.oob_n >= 45 && R.frame - R.net_frame > 120)
		{
			R.net_frame = R.frame;
			R.oob_n = 0;
			rivals_blog("net: puppet (" + string(p.x) + "," + string(p.y) + ") room " + string(room_width) + "x" + string(room_height) + " -> safe (" + string(R.safe_x) + "," + string(R.safe_y) + ")");
			p.x = R.safe_x;
			p.y = R.safe_y;
			R.rel_h = 0;
			R.rel_v = 0;
			R.rel_pending = true;
			rivals_request_warp();
		}
		else if ((R.roa_flags & 2) && R.frame - R.safe_t > 20 && p.x > 0 && p.x < room_width && p.y > 0 && p.y < room_height)
		{
			R.safe_t = R.frame;
			R.safe_x = p.x;
			R.safe_y = p.y - 4;
		}
	}
	if (R.sw_n > 0)
	{
		R.sw_n--;
		if (p.state == states.rivals && R.ready && !R.warp_pending && R.frame > R.warp_until + 2 && room == R.sw_room && R.sw_tries < 4)
		{
			if (p.x < -50 || p.x > room_width + 50 || p.y < -300 || p.y > room_height + 300)
			{
				R.sw_tries++;
				rivals_blog("character switch: puppet (" + string(p.x) + "," + string(p.y) + ") is outside the room, back to (" + string(R.sw_x) + "," + string(R.sw_y) + ")");
				p.x = R.sw_x;
				p.y = R.sw_y;
				p.hsp = 0;
				p.vsp = 0;
				R.last_px = p.x;
				R.last_py = p.y;
				R.oob_n = 0;
				rivals_request_warp(true);
			}
		}
	}
	rivals_apply_feedback();
	if p.state == states.rivals && R.ready && !(R.roa_flags & 64)
		rivals_process_hitboxes();
	// pixels
	var fw = external_call(f.fw), fh = external_call(f.fh);
	// Jitter buffer. RoA and Pizza Tower are two free-running 60 Hz clocks: now and then PT would see no new RoA frame (the
	// character stalls) and the next step two (it skips ahead). Take every RoA frame in order into a small queue and show one per
	// step, keeping one in reserve, so the beat between the clocks is absorbed instead of shown.
	repeat 2
	{
		var _b = array_length(R.fpool) > 0 ? array_pop(R.fpool) : buffer_create(fw * fh * 4, buffer_fixed, 1);
		if external_call(f.copyframe, buffer_get_address(_b))
		{
			var _fg = external_call(f.fmeta, 6);
			if (R.warp_pending || _fg < R.gen)
				array_push(R.fpool, _b);      // from before the last warp: its coordinates are in the old frame of reference
			else
				array_push(R.fq, { buf: _b, cx: external_call(f.fmeta, 0), cy: external_call(f.fmeta, 1), dx: external_call(f.fmeta, 2), dy: external_call(f.fmeta, 3), px: external_call(f.fmeta, 4), py: external_call(f.fmeta, 5) });
		}
		else
		{
			array_push(R.fpool, _b);
			break;
		}
	}
	while (array_length(R.fq) > 4)
	{
		var _old = R.fq[0];
		array_delete(R.fq, 0, 1);
		array_push(R.fpool, _old.buf);
	}
	if (!R.use_queue)
	{
		while (array_length(R.fq) > 1)
		{
			var _drop = R.fq[0];
			array_delete(R.fq, 0, 1);
			array_push(R.fpool, _drop.buf);
		}
	}
	var _qn = array_length(R.fq);
	if ((!R.use_queue && _qn >= 1) || _qn >= 2 || (_qn == 1 && R.fstall >= 2))
	{
		var _e = R.fq[0];
		array_delete(R.fq, 0, 1);
		if !surface_exists(R.surface)
			R.surface = surface_create(fw, fh);
		buffer_set_surface(_e.buf, R.surface, 0);
		array_push(R.fpool, _e.buf);
		R.fcx = _e.cx;
		R.fcy = _e.cy;
		R.fdx = _e.dx;
		R.fdy = _e.dy;
		R.fpx = _e.px;
		R.fpy = _e.py;
		R.fvalid = true;
		R.has_frame = true;
		R.fstall = 0;
	}
	else
		R.fstall++;
}

// Puppet state (obj_player Step_0 -> case states.rivals).
function scr_player_rivals()
{
	var R = global.rivals;
	visible = true;               // followers (toppins, Gerome) copy the player's visibility, so stay visible and draw nothing instead
	sprite_index = spr_null;
	image_blend = c_white;
	if (room != R.puppet_room)
	{
		// first step in a new room: PT just put us at the entrance. Keep that position and make RoA's offset follow it.
		R.puppet_room = room;
		rivals_request_warp();
		R.last_px = x;
		R.last_py = y;
		hsp = 0;
		vsp = 0;
		return;
	}
	if (R.drive_prev)
	{
		// Pizza Tower carried the puppet somewhere (hand, Mr Pinch, pipe, teleporter): the RoA side takes this place and goes on from here
		R.drive_prev = false;
		rivals_request_warp();
		R.last_px = x;
		R.last_py = y;
		hsp = 0;
		vsp = 0;
		return;
	}
	if (R.ready && !(R.roa_flags & 64))
	{
		hsp = R.roa_hsp;
		vsp = R.roa_vsp;
		// the puppet follows the same RoA frame whose pixels get drawn, so the camera and the drawn character never disagree
		if (R.warp_pending)
		{
			hsp = 0;     // hold the spot Pizza Tower chose until RoA has re-derived its offset
			vsp = 0;
		}
		else
		{
			x = R.fvalid ? R.fpx : R.roa_x;
			y = (R.fvalid ? R.fpy : R.roa_y) - R.roa_hh;
		}
		if R.roa_dir != 0
			xscale = R.roa_dir;
	}
	else
	{
		hsp = 0;
		vsp = 0;
	}
	R.last_px = x;
	R.last_py = y;
	grounded = (R.roa_flags & 2) != 0;
	if grounded
		vsp = 0;     // steady for PT's camera, which reads vsp/grounded
	// running into destructible blocks (bricks, iron) at speed breaks them, like PT's mach states
	// RoA's own solid for the block stops the character the very frame it touches it: remember how fast he was running a moment ago
	if (abs(R.roa_hsp) >= RIVALS_MACH_SPEED)
	{
		R.run_hsp = abs(R.roa_hsp);
		R.run_dir = sign(R.roa_hsp);
		R.run_hold = 8;
	}
	else if (R.run_hold > 0)
		R.run_hold--;
	else
		R.run_hsp = 0;
	if ((abs(R.roa_hsp) >= RIVALS_MACH_SPEED || R.run_hold > 0) && !(R.roa_flags & 64))
	{
		var _b = instance_place(x + R.run_dir * (14 + min(R.run_hsp, 16)), y, obj_destructibles);
		if _b != noone
		{
			with _b
				instance_destroy();
			R.solid_timer = 0;
		}
		// iron blocks and rats are dense: running into them does nothing, they need a strong hit (see rivals_process_hitboxes), unless "weak metal" is on
		if (R.weak_metal)
		{
			var _mm = [obj_metalblock, obj_ratblock, obj_rattumble, obj_rattumble_big, obj_ghostwall, obj_ghostblock, obj_shotgunblock, obj_asteroid];
			for (var _mi2 = 0; _mi2 < array_length(_mm); _mi2++)
			{
				var _mb = instance_place(x + R.run_dir * (14 + min(R.run_hsp, 16)), y, _mm[_mi2]);
				if (_mb != noone)
				{
					with _mb
						instance_destroy();
					R.solid_timer = 0;
				}
			}
		}
	}
	// pushing against a block for a moment breaks it too (a standstill never reaches running speed)
	var _in = ((key_right > 0) ? 1 : 0) - ((key_left < 0) ? 1 : 0);
	if (_in != 0 && R.ready && !(R.roa_flags & 64))
	{
		var _pb = instance_place(x + _in * 24, y, obj_destructibles);
		if (_pb != noone)
		{
			R.push_n++;
			if (R.push_n >= 5)
			{
				with _pb
					instance_destroy();
				R.push_n = 0;
				R.solid_timer = 0;
			}
		}
		else
			R.push_n = 0;
	}
	else
		R.push_n = 0;
	R.prev_up = key_up;
	// running through bricks and cheese while dodging breaks them (iron blocks and rats still need a strong hit)
	if (R.roa_state == 8 || R.roa_state == 33 || R.roa_state == 34)
	{
		var _dir = (R.roa_hsp != 0) ? sign(R.roa_hsp) : R.roa_dir;
		with obj_destructibles
		{
			if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, other.x - 24 + _dir * 36, other.y - 30, other.x + 24 + _dir * 36, other.y + 40) != 0)
				instance_destroy();
		}
		R.solid_timer = 0;
		var _rats = [obj_ratblock, obj_rattumble, obj_rattumble_big];
		if (R.weak_metal)
			array_push(_rats, obj_metalblock, obj_ghostwall, obj_ghostblock, obj_shotgunblock, obj_asteroid);
		for (var _ri = 0; _ri < array_length(_rats); _ri++)
		{
			with _rats[_ri]
			{
				if (rectangle_in_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, other.x - 24 + _dir * 36, other.y - 30, other.x + 24 + _dir * 36, other.y + 40) != 0)
					instance_destroy();
			}
		}
	}
	// doors, gates and pipe boxes: PT only lets a *normal* (or crouching) player enter them, so hand control back
	// for a moment while the matching key is held, then take over again if PT did not use the chance
	if (R.yield_cd <= 0)
	{
		var _yield = false;
		if (key_up && grounded)
		{
			var _doors = [obj_door, obj_exitgate, obj_keydoor, obj_bossdoor, obj_startgate, obj_geromedoor, obj_doornexthub, obj_spaceshuttle, obj_taxi];
			for (var _i = 0; _i < array_length(_doors) && !_yield; _i++)
			{
				var _d = instance_place(x, y, _doors[_i]);
				if _d != noone
				{
					if object_is_ancestor(_d.object_index, obj_door) || _d.object_index == obj_door || _d.object_index == obj_doornexthub
						y = _d.y + 50;    // PT's door test requires exactly this
					state = states.normal;
					_yield = true;
				}
			}
		}
		if (!_yield && key_down && grounded && place_meeting(x, y + 1, obj_boxofpizza))
		{
			state = states.crouch;       // the box needs a crouching player standing on it
			_yield = true;
		}
		if (!_yield && key_up)
		{
			var _bx = instance_place(x, y - 40, obj_boxofpizza);
			if (_bx != noone && _bx.image_yscale == -1)
			{
				var _n = 0;
				while (!place_meeting(x, y - 10, _bx) && _n < 40)
				{
					y--;
					_n++;
				}
				state = states.normal;
				_yield = true;
			}
		}
		if _yield
		{
			visible = true;
			R.yield_cd = 45;
			R.takeover_wait = 20;    // PT's door/box objects run later this frame: give them a window before we take over again
			hsp = 0;
			vsp = 0;
		}
	}
	movespeed = abs(hsp);
}

// The hub elevator list, driven by us: up/down choose, jump confirms, attack/back cancels.
function rivals_elevator_update(p)
{
	var R = global.rivals;
	if !R.elev_active
		return;
	if !instance_exists(R.elev_id)
	{
		R.elev_active = false;
		if (p.state == states.actor)
			p.state = states.normal;
		return;
	}
	if instance_exists(obj_fadeout)
		return;
	p.state = states.actor;
	p.hsp = 0;
	p.vsp = 0;
	if R.elev_wait > 0
	{
		R.elev_wait--;
		return;
	}
	var _n = array_length(R.elev_id.hub_array);
	var _up = keyboard_check_pressed(vk_up) || p.key_up2 || rivals_in_pressed(PRB_UP);
	var _dn = keyboard_check_pressed(vk_down) || p.key_down2 || rivals_in_pressed(PRB_DOWN);
	var _ok = keyboard_check_pressed(vk_enter) || p.key_jump || rivals_in_pressed(PRB_JUMP);
	var _no = keyboard_check_pressed(vk_escape) || p.key_back || rivals_in_pressed(PRB_ATTACK);
	if _up
		R.elev_sel++;
	if _dn
		R.elev_sel--;
	R.elev_sel = clamp(R.elev_sel, 0, _n - 1);
	R.elev_id.selected = R.elev_sel;
	if _no
	{
		R.elev_active = false;
		p.state = states.normal;
		R.takeover_wait = 10;
		return;
	}
	if _ok
	{
		var _t = R.elev_id.hub_array[R.elev_sel][0];
		if (_t == room)
		{
			R.elev_active = false;
			p.state = states.normal;
			R.takeover_wait = 10;
			return;
		}
		with obj_player
		{
			targetRoom = _t;
			targetDoor = "A";
			if targetRoom == hub_farmland
				targetDoor = "F";
		}
		instance_create(p.x, p.y, obj_fadeout);
		R.elev_active = false;
		R.takeover_wait = 120;
	}
}

function rivals_draw_elevator()
{
	var R = global.rivals;
	if !R.elev_active || !instance_exists(R.elev_id) || instance_exists(obj_fadeout)
		return;
	var _arr = R.elev_id.hub_array;
	var _of = draw_get_font();
	draw_set_font(fnt_caption);
	draw_set_halign(fa_center);
	draw_set_valign(fa_bottom);
	var _w = 0, _h = 0, _pad = 16;
	for (var _i = 0; _i < array_length(_arr); _i++)
	{
		_w = max(_w, string_width(_arr[_i][1]));
		_h += string_height(_arr[_i][1]);
	}
	_w += _pad;
	_h += _pad;
	var _cx = display_get_gui_width() / 2, _cy = display_get_gui_height() / 2;
	draw_set_color(c_black);
	draw_rectangle(_cx - _w / 2, _cy - _h / 2, _cx + _w / 2, _cy + _h / 2, false);
	var _yy = _cy + _h / 2 - _pad / 2;
	for (_i = 0; _i < array_length(_arr); _i++)
	{
		draw_set_color((R.elev_sel == _i) ? c_white : c_gray);
		draw_text(_cx, _yy, _arr[_i][1]);
		_yy -= string_height(_arr[_i][1]);
	}
	draw_set_color(c_white);
	draw_set_font(_of);
}

// Called from scr_hurtplayer when a PT enemy hits the puppet: PT loses points (the normal
// _hurt tail does that) and RoA takes percent + knockback.
// RoA's parry (PS_PARRY_START 30 / PS_PARRY 9) blocks a Pizza Tower hit like Peppino's own parry: no damage, the attacker and
// everything within reach is knocked away, and the RoA side freezes for a moment.
function rivals_dodging()
{
	return (global.rivals.roa_flags & 256) != 0;
}

function rivals_parry(attacker)
{
	var R = global.rivals;
	if (R.roa_state != 30 && R.roa_state != 9)
		return false;
	var _dir = (attacker != noone && instance_exists(attacker) && attacker.x != x) ? sign(attacker.x - x) : R.roa_dir;
	var _me = id;
	var _att = attacker;
	with obj_baddie
	{
		if (!rivals_is_boss(id) && (_att == id || distance_to_object(_me) <= 84) && state != states.hit && state != states.grabbed && state != states.stun)
		{
			rivals_hit_baddie(id, 14, 35, 9, _dir);
			stunned = 100;
			instance_create(x, y, obj_parryeffect);
		}
	}
	if (attacker != noone && instance_exists(attacker) && !object_is_ancestor(attacker.object_index, obj_baddie) && attacker.object_index != obj_baddie)
	{
		// projectiles: send them back
		with attacker
		{
			if variable_instance_exists(id, "hsp")
				hsp = -hsp;
			image_xscale = -image_xscale;
		}
	}
	external_call(R.fn.hurt, -1, 0, 0, 0);
	fmod_event_one_shot_3d("event:/sfx/pep/parry", x, y);
	with (instance_create(x, y, obj_parryeffect))
		image_xscale = _dir;
	global.combotime = 60;
	invtime = 20;
	flash = true;
	alarm[8] = 20;
	return true;
}

function rivals_on_hurt(attacker)
{
	var R = global.rivals;
	var dirx = (attacker != noone && instance_exists(attacker) && attacker.x != x) ? sign(x - attacker.x) : -R.roa_dir;
	var ang = (dirx > 0) ? 40 : 140;
	external_call(R.fn.hurt, R.enemy_atk, ang, RIVALS_HURT_POWER, dirx);
	invtime = 60;
	hurted = true;
	alarm[7] = 90;
	flash = true;
	alarm[8] = 60;
}

function rivals_menu_input()
{
	var R = global.rivals;
	var n = array_length(R.catalog);
	if keyboard_check_pressed(vk_down) R.menu_sel = (R.menu_sel + 1) % n;
	if keyboard_check_pressed(vk_up) R.menu_sel = (R.menu_sel - 1 + n) % n;
	if keyboard_check_pressed(vk_pagedown) R.menu_sel = min(n - 1, R.menu_sel + 10);
	if keyboard_check_pressed(vk_pageup) R.menu_sel = max(0, R.menu_sel - 10);
	if keyboard_check_pressed(vk_enter)
	{
		rivals_select(R.menu_sel);
		R.menu_open = false;
		if !R.enabled
			rivals_toggle();
	}
	if keyboard_check_pressed(vk_escape)
		R.menu_open = false;
}

function rivals_draw_menu()
{
	var R = global.rivals;
	if !R.menu_open
		return;
	var _oldfont = draw_get_font();
	draw_set_font(fnt_caption);
	var n = array_length(R.catalog), rows = 14;
	R.menu_scroll = clamp(R.menu_sel - rows div 2, 0, max(0, n - rows));
	draw_set_alpha(0.85);
	draw_set_color(c_black);
	draw_rectangle(40, 40, 520, 80 + rows * 26, false);
	draw_set_alpha(1);
	draw_set_color(c_white);
	draw_set_halign(fa_left);
	draw_text(56, 48, "Rivals of Aether character  (Up/Down, PgUp/PgDn, Enter, Esc)");
	for (var i = 0; i < rows && R.menu_scroll + i < n; i++)
	{
		var idx = R.menu_scroll + i;
		var c = R.catalog[idx];
		draw_set_color(idx == R.menu_sel ? c_yellow : c_white);
		draw_text(64, 84 + i * 26, (c.id == R.char_id ? "* " : "  ") + c.name + (c.source == "stock" ? "  [stock]" : ""));
	}
	draw_set_color(c_white);
	draw_set_font(_oldfont);
}

function rivals_draw_status()
{
	var R = global.rivals;
	if !R.available
		return;
	if !R.show_debug
		return;
	var _oldfont = draw_get_font();
	draw_set_font(fnt_caption);
	draw_set_halign(fa_right);
	draw_set_valign(fa_top);
	draw_set_color(R.enabled ? c_lime : c_gray);
	var _modes = ["auto", "native pad", "PT keys"];
	draw_text(display_get_gui_width() - 8, 6, "F6 Rivals: " + (R.enabled ? "ON " : "off ") + R.status + "   F10: bounds   F7: characters   F8: input " + _modes[R.input_mode] + (rivals_native_input() ? " (RoA reads pad)" : " (PT keys)"));
	draw_text(display_get_gui_width() - 8, 86, global.rivals_room_info);
	if R.enabled && instance_exists(obj_player1)
		draw_text(display_get_gui_width() - 8, 26, "roa=(" + string(floor(R.roa_x)) + "," + string(floor(R.roa_y)) + ") flags=" + string(R.roa_flags) + " frame=" + string(R.has_frame) + " f=(" + string(R.roa_fx) + "," + string(R.roa_fy) + ") surf=" + string(surface_exists(R.surface)) + " pt=(" + string(floor(obj_player1.x)) + "," + string(floor(obj_player1.y)) + ") st=" + string(obj_player1.state));
	if (R.enabled && R.show_bounds && instance_exists(obj_player1))
	{
		// debug: which special objects the puppet currently overlaps (for hunting interactions that do not trigger)
		var _near = "";
		var _chk = [obj_rocket, obj_hubelevator, obj_geromedoor, obj_door, obj_boxofpizza, obj_exitgate];
		var _nm = ["rocket", "elevator", "geromedoor", "door", "box", "exitgate"];
		for (var _i = 0; _i < array_length(_chk); _i++)
		{
			var _o = instance_place(obj_player1.x, obj_player1.y, _chk[_i]);
			if (_o != noone)
				_near += _nm[_i] + " ";
		}
		draw_set_halign(fa_right);
		draw_set_color(c_aqua);
		draw_text(display_get_gui_width() - 8, 66, "touching: " + _near + " state=" + string(obj_player1.state) + " grounded=" + string(obj_player1.grounded));
	}
	if (R.enabled && R.ready)
	{
		draw_set_halign(fa_right);
		draw_set_color(current_time < R.slow_until ? c_red : c_gray);
		draw_text(display_get_gui_width() - 8, 46, "rivals " + string(round(R.rfps)) + "/" + string(round(R.rtick)) + " fps   pt " + string(fps));
		if (current_time < R.slow_until)
		{
			draw_set_halign(fa_center);
			draw_set_color(c_red);
			draw_text(display_get_gui_width() / 2, 70, "RIVALS IS RUNNING SLOW (render " + string(round(R.rfps)) + ", logic " + string(round(R.rtick)) + ", should be 60)");
		}
	}
	draw_set_halign(fa_left);
	draw_set_color(c_white);
	draw_set_font(_oldfont);
}

// ---------------------------------------------------------------------------------------------------------------------
// "Aether Tower": the pause-menu page for everything Rivals related. Built with PT's own menu framework (scr_menu), so it
// looks and behaves like the video / controls pages.
// ---------------------------------------------------------------------------------------------------------------------
#macro RIVALS_PAGE_ROOT 100
#macro RIVALS_PAGE_ENEMY 99
#macro RIVALS_PAGE_CHARS 101   // 101.. = character pages
#macro PRE_CSS_STATE 104
#macro RIVALS_CHARS_PER_PAGE 6

function rivals_register_lang()
{
	if !variable_global_exists("lang_map")
		return;
	var en = ds_map_find_value(global.lang_map, "en");
	if is_undefined(en)
		return;
	ds_map_set(en, "pause_aether", "AETHER TOWER");
	ds_map_set(en, "aether_enabled", "RIVALS");
	ds_map_set(en, "aether_character", "CHANGE CHARACTER");
	ds_map_set(en, "aether_reset", "RESET POSITION");
	ds_map_set(en, "aether_resetpeppino", "RESET PEPPINO");
	ds_map_set(en, "aether_speed", "RIVAL SPEED");
	ds_map_set(en, "aether_percent", "ENEMY START PERCENT");
	ds_map_set(en, "aether_killpct", "PT KILL PERCENT");
	ds_map_set(en, "aether_showpct", "SHOW ENEMY PERCENTS");
	ds_map_set(en, "aether_killground", "PT KILLS ON HIT");
	ds_map_set(en, "aether_enemyatk", "ENEMY ATTACK");
	ds_map_set(en, "aether_weakmetal", "WEAK METAL");
	ds_map_set(en, "aether_sfxvol", "RIVALS SOUND EFFECTS");
	ds_map_set(en, "aether_musicvol", "RIVALS MUSIC");
	ds_map_set(en, "aether_floortime", "SECONDS ON GROUND");
	ds_map_set(en, "aether_hud", "PLAYER NAME");
	ds_map_set(en, "aether_debuginfo", "DEBUG INFO");
	ds_map_set(en, "aether_height", "HEIGHT RECENTER");
	ds_map_set(en, "aether_volume", "RIVALS VOLUME");
	ds_map_set(en, "aether_debug", "DEBUG TEXT");
	ds_map_set(en, "aether_enemy", "ENEMY OPTIONS");
	ds_map_set(en, "aether_bounds", "SHOW BOUNDS");
	ds_map_set(en, "aether_recenter", "KEEP IN STAGE");
	ds_map_set(en, "aether_input", "INPUT");
	ds_map_set(en, "aether_workshop", "WORKSHOP OR SKINS");
	ds_map_set(en, "aether_queue", "FRAME QUEUE");
	ds_map_set(en, "aether_targets", "REAL TARGETS");
	ds_map_set(en, "aether_resetrival", "RESET RIVAL");
	ds_map_set(en, "aether_more", "MORE");
}

function rivals_set_speed(mult)
{
	var R = global.rivals;
	R.speed = mult;
	if R.connected
		external_call(R.fn.cmd, 6, 1, mult);
}

function rivals_set_percent(pct)
{
	var R = global.rivals;
	R.percent = pct;
	if R.connected
		external_call(R.fn.cmd, 6, 3, pct);
}

function rivals_set_kill_percent(pct)
{
	var R = global.rivals;
	R.kill_pct = pct;
	if R.connected
		external_call(R.fn.cmd, 6, 4, pct);
}

function rivals_open_css()
{
	var R = global.rivals;
	if !R.connected
		exit;
	R.css = true;
	R.ready = false;
	R.status = "pick a character in the Rivals window (Practice to confirm)";
	external_call(R.fn.cmd, 7, 0, 0);
}

// restarts the RoA match from scratch (workshop characters often expect inputs at the very start of a match)
function rivals_reset_rival()
{
	var R = global.rivals;
	if !R.connected
		exit;
	R.ready = false;
	R.status = "resetting...";
	external_call(R.fn.cmd, 3, 0, 0);
}

function rivals_request_reset()
{
	global.rivals.pending_reset = true;
}

function rivals_menu_pick(idx)
{
	var R = global.rivals;
	if !R.enabled
		rivals_toggle();
	R.css = false;
	rivals_select(idx);
}

// builds the Aether Tower pages inside obj_option (called at the end of its Create event)
function rivals_option_menus()
{
	var R = global.rivals;
	if !R.available
		exit;
	var root = create_menu_fixed(RIVALS_PAGE_ROOT, menu_anchor.left, 150, 40, 0);
	root.exit_on_back = true;
	add_option_press(root, 0, "option_back", function()
	{
		instance_destroy();
	});
	// the cast (left / right) and "workshop or skin": nothing happens until this menu is closed
	var _cv = [];
	var _ci = 0;
	var _cur = rivals_catalog_index(R.char_id);
	for (var i = 0; i < array_length(R.catalog); i++)
	{
		if (R.catalog[i].source != "stock")
			continue;
		array_push(_cv, create_option_value(string_upper(R.catalog[i].name), i, false));
		if (i == _cur)
			_ci = array_length(_cv) - 1;
	}
	array_push(_cv, create_option_value("WORKSHOP OR SKIN", -1, false));
	add_option_multiple(root, 1, "aether_character", _cv, function(val)
	{
		global.rivals.pending_pick = val;
	}).value = _ci;
	add_option_press(root, 2, "aether_resetrival", function()
	{
		rivals_reset_rival();
		rivals_resume_game();
	});
	add_option_press(root, 3, "aether_reset", function()
	{
		rivals_request_reset();
		rivals_resume_game();
	});
	var _speeds = [1, 1.5, 2, 2.5, 3, 3.5, 5];
	var _sv = [];
	var _si = 0;
	for (var i = 0; i < array_length(_speeds); i++)
	{
		array_push(_sv, create_option_value(string(_speeds[i]) + "X", _speeds[i], false));
		if (_speeds[i] == R.speed)
			_si = i;
	}
	add_option_press(root, 4, "aether_resetpeppino", function()
	{
		rivals_resume_game();
		rivals_reset_peppino();
	});
	add_option_multiple(root, 5, "aether_speed", _sv, function(val)
	{
		rivals_set_speed(val);
	}).value = _si;
	add_option_toggle(root, 6, "aether_enabled", function(val)
	{
		global.rivals.pending_toggle = (global.rivals.enabled != val);      // applied once the game is running again
	}).value = R.enabled;
	add_option_press(root, 7, "aether_enemy", function()
	{
		menu_goto(RIVALS_PAGE_ENEMY);
	});
	add_option_toggle(root, 8, "aether_debuginfo", function(val)
	{
		global.rivals.show_debug = val;
		global.rivals.show_bounds = val;
	}).value = (R.show_debug && R.show_bounds);
	add_option_toggle(root, 9, "aether_hud", function(val)
	{
		global.rivals.show_hud = val;
		external_call(global.rivals.fn.cmd, 6, 7, val ? 1 : 0);
	}).value = R.show_hud;
	add_option_toggle(root, 10, "aether_weakmetal", function(val)
	{
		global.rivals.weak_metal = val;
	}).value = R.weak_metal;
	array_push(menus, root);
	// enemy options page
	var em = create_menu_fixed(RIVALS_PAGE_ENEMY, menu_anchor.left, 150, 40, RIVALS_PAGE_ROOT);
	var eb = add_option_press(em, 0, "option_back", -4);
	eb.func = method(em, function()
	{
		with (obj_option)
			menu_goto(RIVALS_PAGE_ROOT);
	});
	add_option_toggle(em, 1, "aether_targets", function(val)
	{
		global.rivals.cpu_targets = val;
		external_call(global.rivals.fn.cmd, 6, 2, val ? 1 : 0);
	}).value = R.cpu_targets;
	var _pcts = [0, 25, 50, 75, 100];
	var _pv = [];
	var _pi = 0;
	for (var i = 0; i < array_length(_pcts); i++)
	{
		array_push(_pv, create_option_value(string(_pcts[i]), _pcts[i], false));
		if (_pcts[i] == R.percent)
			_pi = i;
	}
	add_option_multiple(em, 2, "aether_percent", _pv, function(val)
	{
		rivals_set_percent(val);
	}).value = _pi;
	var _kills = [0, 10, 25, 50, 75, 100];
	var _kv = [];
	var _ki = 2;
	for (var i = 0; i < array_length(_kills); i++)
	{
		array_push(_kv, create_option_value(string(_kills[i]), _kills[i], false));
		if (_kills[i] == R.kill_pct)
			_ki = i;
	}
	add_option_multiple(em, 3, "aether_killpct", _kv, function(val)
	{
		rivals_set_kill_percent(val);
	}).value = _ki;
	add_option_toggle(em, 4, "aether_killground", function(val)
	{
		global.rivals.pt_always = val;
		external_call(global.rivals.fn.cmd, 6, 5, val ? 1 : 0);
	}).value = R.pt_always;
	var _ft = [0.25, 0.5, 0.75, 1];
	var _fv = [];
	var _fi = 3;
	for (var i = 0; i < array_length(_ft); i++)
	{
		array_push(_fv, create_option_value(string(_ft[i]), _ft[i], false));
		if (_ft[i] == R.floor_time)
			_fi = i;
	}
	add_option_multiple(em, 5, "aether_floortime", _fv, function(val)
	{
		global.rivals.floor_time = val;
		external_call(global.rivals.fn.cmd, 6, 6, val);
	}).value = _fi;
	add_option_toggle(em, 6, "aether_showpct", function(val)
	{
		global.rivals.show_pct = val;
	}).value = R.show_pct;
	var _ats = [0, 10, 25, 50, 100];
	var _av = [];
	var _ai = 2;
	for (var i = 0; i < array_length(_ats); i++)
	{
		array_push(_av, create_option_value(string(_ats[i]), _ats[i], false));
		if (_ats[i] == R.enemy_atk)
			_ai = i;
	}
	add_option_multiple(em, 7, "aether_enemyatk", _av, function(val)
	{
		global.rivals.enemy_atk = val;
	}).value = _ai;
	array_push(menus, em);
}

// two sliders for Pizza Tower's audio menu (called from obj_option's Create)
function rivals_audio_menu(m)
{
	var R = global.rivals;
	var _sv = add_option_slide(m, 4, "aether_sfxvol", function(val)
	{
		global.rivals.volume = val / 100;
	}, function(val)
	{
		global.rivals.volume = val / 100;
	}, "event:/sfx/ui/slidersfx");
	_sv.maxv = 150;
	_sv.value = clamp(R.volume * 100, 0, 150);
	add_option_slide(m, 5, "aether_musicvol", function(val)
	{
		global.rivals.music_volume = val / 100;
	}, function(val)
	{
		global.rivals.music_volume = val / 100;
	}, "event:/sfx/ui/slidermusic").value = R.music_volume * 100;
}

// The controller of Rivals of Aether, forwarded: while the rival is off, RoA's reading of the pad stands in for whatever Pizza Tower does not get itself.
// Is Pizza Tower itself receiving the pad right now (or a moment ago)? Then RoA's reading of the same pad is not used at all:
// two copies of one press are what made menus move twice.
function rivals_native_pad_recent()
{
	var R = global.rivals;
	if (R.npad_stamp != R.frame)
	{
		R.npad_stamp = R.frame;
		R.npad_now = false;
		for (var _d = 0; _d < gamepad_get_device_count() && !R.npad_now; _d++)
		{
			if (!gamepad_is_connected(_d))
				continue;
			for (var _b = 32769; _b <= 32784 && !R.npad_now; _b++)
			{
				if (gamepad_button_check(_d, _b))
					R.npad_now = true;
			}
			for (var _a = 32785; _a <= 32788 && !R.npad_now; _a++)
			{
				if (abs(gamepad_axis_value(_d, _a)) > 0.5)
					R.npad_now = true;
			}
		}
		if (R.npad_now)
			R.npad_t = current_time;
	}
	return R.npad_now || (current_time - R.npad_t < 2500);
}

function rivals_pad_refresh()
{
	var R = global.rivals;
	if (R.pad_stamp == R.frame)
		return;
	R.pad_stamp = R.frame;
	R.pad_prev = R.pad_now;
	R.pad_now = 0;
	R.pad_ax = [0, 0, 0, 0];
	if (!R.available || !R.connected || R.enabled || rivals_native_pad_recent())
		return;
	if (external_call(R.fn.psnap) == 0)
		return;
	if ((external_call(R.fn.pget, 7) & 128) == 0)
		return;
	R.pad_now = external_call(R.fn.pget, 25);
	for (var _i = 0; _i < 4; _i++)
	{
		var _v = external_call(R.fn.pget, 26 + _i) / 100;
		R.pad_ax[_i] = (abs(_v) < 0.2) ? 0 : _v;
	}
}

function rivals_pad_bit(btn)
{
	if (!variable_global_exists("rivals") || btn < 32769 || btn > 32784)
		return [false, false];
	rivals_pad_refresh();
	var R = global.rivals;
	var _m = 1 << (btn - 32769);
	return [(R.pad_now & _m) != 0, (R.pad_prev & _m) != 0];
}

function rivals_pad_check(dev, btn)
{
	if (gamepad_button_check(dev, btn))
		return true;
	return rivals_pad_bit(btn)[0];
}

function rivals_pad_check_pressed(dev, btn)
{
	if (gamepad_button_check_pressed(dev, btn))
		return true;
	var _b = rivals_pad_bit(btn);
	return _b[0] && !_b[1];
}

function rivals_pad_check_released(dev, btn)
{
	if (gamepad_button_check_released(dev, btn))
		return true;
	var _b = rivals_pad_bit(btn);
	return !_b[0] && _b[1];
}

function rivals_pad_axis(dev, ax)
{
	var _v = gamepad_axis_value(dev, ax);
	if (_v != 0 || ax < 32785 || ax > 32788 || !variable_global_exists("rivals"))
		return _v;
	rivals_pad_refresh();
	return global.rivals.pad_ax[ax - 32785];
}

// Pizza Tower pauses with "controller disconnected" when the pad's device index changes (RoA starting up does that); take the pad back by ourselves
function rivals_reconnect_pad()
{
	with obj_inputAssigner
	{
		var _dev = -1;
		for (var _i = 0; _i < gamepad_get_device_count(); _i++)
		{
			if gamepad_is_connected(_i)
			{
				_dev = _i;
				break;
			}
		}
		player_input_device[device_to_reconnect] = _dev;
		deactivated = false;
		scr_pause_activate_objects(false);
		alarm[0] = 1;
	}
}

// leaves the options and the pause menu exactly like "resume" does
function rivals_resume_game()
{
	if (instance_exists(obj_pause))
	{
		with obj_pause
		{
			scr_pause_activate_objects();
			pause_unpause_music();
		}
	}
	instance_destroy(obj_option);
	instance_destroy(obj_keyconfig);
}

// Peppino falls out of the level, the way he does at a pit: the "technical difficulty" screen puts him back at the room's start
function rivals_reset_peppino()
{
	var R = global.rivals;
	if (!instance_exists(obj_player1))
		return;
	var p = obj_player1;
	if (room == Mainmenu || room == Realtitlescreen || room == Longintro || room == Endingroom || room == Creditsroom || room == Johnresurrectionroom || room == rank_room || room == boss_pizzaface || room == boss_pizzafacefinale || instance_exists(obj_technicaldifficulty))
		return;
	with p
	{
		state = states.actor;
		visible = false;
		hsp = 0;
		vsp = 0;
		fmod_event_one_shot_3d("event:/sfx/pep/groundpound", x, y);
		with (instance_create(x, y + 540, obj_technicaldifficulty))
		{
			playerid = other.id;
			if !other.ispeppino
				noise = true;
			if !noise
			{
				if !other.isgustavo
					sprite = choose(spr_technicaldifficulty1, spr_technicaldifficulty2, spr_technicaldifficulty3);
				else
					sprite = spr_technicaldifficulty4;
			}
			else
				sprite = choose(spr_technicaldifficulty5, spr_technicaldifficulty6, spr_technicaldifficulty7);
		}
	}
	R.rel_pending = false;
	R.oob_n = 0;
}

function rivals_pause_aether()
{
	fmod_event_one_shot("event:/sfx/ui/select");
	with (instance_create(x, y, obj_option))
	{
		depth = other.depth - 1;
		if (variable_global_exists("rivals") && global.rivals.available)
			menu_goto(RIVALS_PAGE_ROOT);
	}
}

// ================= custom music =================
// pizzarivals_music.cfg (next to the game) links levels and characters to songs:
//   LEVEL  "Entrance" (music_holy)          the music of a level (any level name, or its short name: entrance, medieval, ruin...)
//   ESCAPE "Clairen"  (music_plasma)        the Pizza Time music while playing that character
//   LAP2   "Clairen"  (music_abyss)         the lap 2 music while playing that character
//   ESCAPE "Wren"     (C:\path\song.ogg)    an .ogg file instead of a Rivals of Aether song
// Anything not filled in plays Pizza Tower's own music. Rivals songs play their intro and then loop; .ogg files just loop.
// Pizza Tower keeps deciding when music plays (pauses, secrets, the end of a level): its own track is silenced and ours follows.
function rivals_music_norm(s)
{
	var o = "";
	s = string_lower(s);
	for (var i = 1; i <= string_length(s); i++)
	{
		var ch = string_char_at(s, i);
		var oc = ord(ch);
		if ((oc >= 97 && oc <= 122) || (oc >= 48 && oc <= 57))
			o += ch;
	}
	return o;
}

function rivals_music_level_key(name)
{
	var n = rivals_music_norm(name);
	switch (n)
	{
		case "johngutter": return "entrance";
		case "pizzascape": return "medieval";
		case "ancientcheese": return "ruin";
		case "bloodsaucedungeon": return "dungeon";
		case "oreganodesert": case "desert": return "badland";
		case "wasteyard": return "graveyard";
		case "funfarm": return "farm";
		case "fastfoodsaloon": return "saloon";
		case "crustcove": case "beach": return "plage";
		case "gnomeforest": return "forest";
		case "deepdish9": return "space";
		case "golf": return "minigolf";
		case "pigcity": return "street";
		case "ohshit": return "sewer";
		case "rrf": case "refrigeratorrefrigeradorfreezerator": return "freezer";
		case "factory": return "industrial";
		case "pizzascare": return "chateau";
		case "dontmakeasound": return "kidsparty";
	}
	return n;
}

function rivals_mlog(s)
{
	var _f = file_text_open_append(working_directory + "pizzarivals_music.log");
	file_text_write_string(_f, string(global.rivals.frame) + " " + s);
	file_text_writeln(_f);
	file_text_close(_f);
}

function rivals_music_load()
{
	var R = global.rivals;
	R.mus_cfg = [];
	ds_map_clear(R.mus_cache);
	var _fn = working_directory + "pizzarivals_music.cfg";
	if (!file_exists(_fn))
		return;
	var _f = file_text_open_read(_fn);
	while (!file_text_eof(_f))
	{
		var _l = file_text_read_string(_f);
		file_text_readln(_f);
		var _s0 = 1;
		while (_s0 <= string_length(_l) && string_char_at(_l, _s0) == " ")
			_s0++;
		var _c0 = string_char_at(_l, _s0);
		if (_c0 == "#" || _c0 == "/" || _c0 == ";")
			continue;
		var _q1 = string_pos("\"", _l);
		if (_q1 <= 0)
			continue;
		var _rest = string_copy(_l, _q1 + 1, string_length(_l));
		var _q2 = string_pos("\"", _rest);
		if (_q2 <= 0)
			continue;
		var _nm = string_copy(_rest, 1, _q2 - 1);
		var _kw = string_upper(rivals_music_norm(string_copy(_l, 1, _q1 - 1)));
		_kw = string_upper(_kw);
		var _after = string_copy(_rest, _q2 + 1, string_length(_rest));
		if (_kw == "ROADIR")
		{
			R.mus_roadir = _nm;
			continue;
		}
		var _p1 = string_pos("(", _after);
		if (_p1 <= 0)
			continue;
		var _p2 = 0;
		for (var _i = string_length(_after); _i > _p1; _i--)
		{
			if (string_char_at(_after, _i) == ")")
			{
				_p2 = _i;
				break;
			}
		}
		if (_p2 <= 0)
			continue;
		var _spec = string_copy(_after, _p1 + 1, _p2 - _p1 - 1);
		while (string_length(_spec) > 0 && string_char_at(_spec, 1) == " ")
			_spec = string_delete(_spec, 1, 1);
		while (string_length(_spec) > 0 && string_char_at(_spec, string_length(_spec)) == " ")
			_spec = string_delete(_spec, string_length(_spec), 1);
		if (_spec == "")
			continue;
		var _kind = "";
		if (_kw == "LEVEL")
			_kind = "level";
		else if (_kw == "ESCAPE")
			_kind = "escape";
		else if (_kw == "LAP2")
			_kind = "lap2";
		if (_kind == "")
			continue;
		array_push(R.mus_cfg, { kind: _kind, name: (_kind == "level") ? rivals_music_level_key(_nm) : rivals_music_norm(_nm), spec: _spec });
	}
	file_text_close(_f);
	// songs from Rivals of Aether that are not on disk yet are exported from the player's own copy of the game (in the background)
	var _need = "";
	for (var _k = 0; _k < array_length(R.mus_cfg); _k++)
	{
		var _sp = R.mus_cfg[_k].spec;
		var _lsp = string_lower(_sp);
		if (string_pos("\\", _sp) > 0 || string_pos("/", _sp) > 0 || string_pos(":", _sp) > 0 || string_pos(".ogg", _lsp) > 0)
			continue;
		var _d = working_directory + "pizzarivals_music\\";
		if (file_exists(_d + _sp + "_loop_i.ogg") || file_exists(_d + _sp + "_loop.ogg") || file_exists(_d + _sp + ".ogg"))
			continue;
		_need += _sp + ",";
	}
	if (_need != "" && !R.mus_ext_wait && external_call(R.fn.mstate) != 1)
	{
		external_call(R.fn.mext, R.mus_roadir, working_directory + "pizzarivals_music", _need);
		R.mus_ext_wait = true;
		rivals_mlog("exporting songs from Rivals of Aether: " + _need);
	}
}

function rivals_music_lookup(kind, name1, name2, name3 = "")
{
	var R = global.rivals;
	for (var i = 0; i < array_length(R.mus_cfg); i++)
	{
		var e = R.mus_cfg[i];
		if (e.kind == kind && (e.name == name1 || (name2 != "" && e.name == name2) || (name3 != "" && e.name == name3)))
			return e.spec;
	}
	return "";
}

// a song name -> its intro file and loop file (extracted from Rivals of Aether into pizzarivals_music), or an .ogg path
function rivals_music_resolve(spec)
{
	var R = global.rivals;
	if (ds_map_exists(R.mus_cache, spec))
		return ds_map_find_value(R.mus_cache, spec);
	var r = { intro: "", loop: "" };
	var _ls = string_lower(spec);
	if (string_pos("\\", spec) > 0 || string_pos("/", spec) > 0 || string_pos(":", spec) > 0 || string_pos(".ogg", _ls) > 0)
	{
		var _dst = working_directory + "pizzarivals_music\\ext_" + md5_string_utf8(spec) + ".ogg";
		if (!file_exists(_dst))
			external_call(R.fn.fcopy, spec, _dst);
		if (file_exists(_dst))
			r.loop = _dst;
		else
			rivals_mlog("song file not found: " + spec);
	}
	else
	{
		var d = working_directory + "pizzarivals_music\\";
		var lc = [spec + "_loop_i", spec + "_loop", spec];
		for (var i = 0; i < array_length(lc); i++)
		{
			if (file_exists(d + lc[i] + ".ogg"))
			{
				r.loop = d + lc[i] + ".ogg";
				break;
			}
		}
		var ic = [spec + "_open", spec + "_intro"];
		for (var i = 0; i < array_length(ic); i++)
		{
			if (r.loop != "" && file_exists(d + ic[i] + ".ogg"))
			{
				r.intro = d + ic[i] + ".ogg";
				break;
			}
		}
	}
	ds_map_set(R.mus_cache, spec, r);
	return r;
}

function rivals_music_stop()
{
	var R = global.rivals;
	var M = R.mus;
	if (M.key == "")
		return;
	if (M.inst >= 0)
		audio_stop_sound(M.inst);
	if (M.intro_s >= 0)
		audio_destroy_stream(M.intro_s);
	if (M.loop_s >= 0)
		audio_destroy_stream(M.loop_s);
	// give Pizza Tower's own track back (unless the game itself has it paused)
	if (instance_exists(obj_music) && M.target != noone)
	{
		var _pt_paused = (instance_exists(obj_pause) && obj_pause.pause) || obj_music.secret;
		if (!_pt_paused)
			fmod_event_instance_set_paused(M.target, false);
	}
	M.key = "";
	M.inst = -1;
	M.intro_s = -1;
	M.loop_s = -1;
	M.phase = 0;
	M.paused = false;
	M.target = noone;
}

// what should play right now: { key, spec, kind, target } or undefined
function rivals_music_pick()
{
	var R = global.rivals;
	if (!R.enabled || !R.ready || !instance_exists(obj_music) || array_length(R.mus_cfg) == 0)
		return undefined;
	var _n1 = rivals_music_norm(R.char_name);
	var _n2 = rivals_music_norm(R.char_id);
	var _cix = rivals_catalog_index(R.char_id);
	var _n3 = (array_length(R.catalog) > _cix && R.catalog[_cix].id == R.char_id) ? rivals_music_norm(R.catalog[_cix].name) : "";
	var _colon = string_pos(":", R.char_id);
	var _n4 = (_colon > 0) ? rivals_music_norm(string_copy(R.char_id, _colon + 1, string_length(R.char_id))) : "";
	if (R.mus_log_key != R.char_id + "|" + R.char_name)
	{
		R.mus_log_key = R.char_id + "|" + R.char_name;
		rivals_mlog("character: name=" + R.char_name + " id=" + R.char_id + " (matches: " + _n1 + ", " + _n2 + ", " + _n3 + ", " + _n4 + "); " + string(array_length(R.mus_cfg)) + " config entries");
	}
	var _kind = "";
	var _spec = "";
	var _target = noone;
	if (global.panic)
	{
		if (room == tower_finalhallway || obj_music.exitmusic || !fmod_event_instance_is_playing(obj_music.panicmusicID))
			return undefined;
		_kind = global.lap ? "lap2" : "escape";
		_spec = rivals_music_lookup(_kind, _n1, _n2, _n3);
		if (_spec == "")
			_spec = rivals_music_lookup(_kind, _n4, "");
		_target = obj_music.panicmusicID;
	}
	else
	{
		if (obj_music.music == noone || obj_music.music == -4 || !fmod_event_instance_is_playing(obj_music.music.event))
			return undefined;
		var _rn = room_get_name(room);
		var _u = string_pos("_", _rn);
		var _lv = string_lower((_u > 0) ? string_copy(_rn, 1, _u - 1) : _rn);
		_kind = "level";
		_spec = rivals_music_lookup("level", _lv, "");
		_target = obj_music.music.event;
	}
	if (_spec == "")
		return undefined;
	return { key: _kind + "|" + _spec, spec: _spec, kind: _kind, target: _target };
}

function rivals_music_step()
{
	var R = global.rivals;
	var M = R.mus;
	if (!R.mus_loaded || room != R.mus_room)
	{
		R.mus_loaded = true;
		R.mus_room = room;
		rivals_music_load();
	}
	if (R.mus_ext_wait)
	{
		if (external_call(R.fn.mstate) == 2)
		{
			R.mus_ext_wait = false;
			ds_map_clear(R.mus_cache);
			rivals_mlog("export finished");
		}
		else
		{
			rivals_music_stop();
			return;
		}
	}
	var want = rivals_music_pick();
	if (is_undefined(want))
	{
		rivals_music_stop();
		return;
	}
	if (M.key != want.key)
	{
		rivals_music_stop();
		var tr = rivals_music_resolve(want.spec);
		if (tr.loop == "")
			return;
		M.loop_s = audio_create_stream(tr.loop);
		if (tr.intro != "")
			M.intro_s = audio_create_stream(tr.intro);
		if (M.loop_s < 0)
		{
			rivals_music_stop();
			return;
		}
		if (M.intro_s >= 0)
		{
			M.inst = audio_play_sound(M.intro_s, 100, false);
			M.phase = 1;
		}
		else
		{
			M.inst = audio_play_sound(M.loop_s, 100, true);
			M.phase = 2;
		}
		M.key = want.key;
		M.target = want.target;
		M.paused = false;
		M.gain_t = -100;
	}
	M.target = want.target;
	// Pizza Tower's own track stays silent (the game decides when it plays, we only hide it)
	fmod_event_instance_set_paused(M.target, true);
	var _pause = (instance_exists(obj_pause) && obj_pause.pause) || obj_music.secret || fmod_event_instance_is_playing(obj_music.kidspartychaseID);
	if (_pause != M.paused)
	{
		if (M.inst >= 0)
		{
			if _pause
				audio_pause_sound(M.inst);
			else
				audio_resume_sound(M.inst);
		}
		M.paused = _pause;
	}
	if (M.phase == 1 && !M.paused && !audio_is_playing(M.inst))
	{
		M.inst = audio_play_sound(M.loop_s, 100, true);
		M.phase = 2;
		M.gain_t = -100;
	}
	if (R.frame - M.gain_t >= 10 && M.inst >= 0)
	{
		M.gain_t = R.frame;
		audio_sound_gain(M.inst, global.option_master_volume * global.option_music_volume * R.music_volume * 0.45, 0);
	}
}
