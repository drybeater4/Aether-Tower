var R = global.rivals;
if (!(instance_exists(obj_pause) && obj_pause.pause) && R.enabled && room != Realtitlescreen && R.has_frame && surface_exists(R.surface) && instance_exists(obj_player1) && (obj_player1.state == states.rivals || R.drive_vis))
{
	var _w = surface_get_width(R.surface), _h = surface_get_height(R.surface);
	gpu_set_blendmode_ext(bm_one, bm_inv_src_alpha);
	// Pizza Tower's camera is locked to the puppet (cam = puppet - half a screen), so a sprite drawn at the puppet's position stays
	// perfectly still on screen. Do the same with the RoA frame: put the character's origin exactly on the puppet instead of on the
	// position stored with the (possibly newer) frame, otherwise it sits one frame of movement off the camera and flickers by a pixel.
	var _p = obj_player1;
	var _ox = _p.x - _w / 2;
	var _oy = (_p.y + R.roa_hh) + (R.fcy - _h / 2 - floor(R.fpy + 0.5));
	draw_surface(R.surface, _ox, _oy);
	gpu_set_blendmode(bm_normal);
}

if (!(instance_exists(obj_pause) && obj_pause.pause) && R.enabled && R.show_bounds && R.connected && R.ready && room != Realtitlescreen && instance_exists(obj_player1))
{
	// debug overlay: RoA's blast zone (red) and RoA's camera view (cyan), in Pizza Tower room coordinates
	draw_set_alpha(1);
	draw_set_color(c_red);
	draw_rectangle(R.bzl, R.bzt, R.bzr, R.bzb, true);
	draw_rectangle(R.bzl + 1, R.bzt + 1, R.bzr - 1, R.bzb - 1, true);
	draw_set_color(c_aqua);
	draw_rectangle(R.vx, R.vy, R.vx + R.vw, R.vy + R.vh, true);
	draw_rectangle(R.vx + 1, R.vy + 1, R.vx + R.vw - 1, R.vy + R.vh - 1, true);
	// the stand-in CPUs as RoA really has them: magenta = on an enemy, dark = parked; the enemies Pizza Tower reports are green
	var _dn = external_call(R.fn.dsnap);
	for (var _i = 0; _i < _dn; _i++)
	{
		var _k = external_call(R.fn.dget, _i, 4);
		draw_set_color(_k == 1 ? c_fuchsia : c_dkgray);
		var _x0 = external_call(R.fn.dget, _i, 0), _y0 = external_call(R.fn.dget, _i, 1), _x1 = external_call(R.fn.dget, _i, 2), _y1 = external_call(R.fn.dget, _i, 3);
		if (_k == 1 || (_x1 > _x0 && _y1 > _y0 && abs(_y0 - obj_player1.y) < 800))
			draw_rectangle(_x0, _y0, _x1, _y1, true);
	}
	draw_set_color(c_lime);
	with obj_baddie
	{
		if (visible && abs(x - obj_player1.x) < 900 && abs(y - obj_player1.y) < 600)
			draw_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, true);
	}
	draw_set_color(c_white);
}

// the percent of every enemy that Rivals is standing a CPU in for (red once the next hit is the finishing hit)
if (!(instance_exists(obj_pause) && obj_pause.pause) && R.enabled && R.show_pct && R.connected && R.ready && room != Realtitlescreen && ds_map_size(R.cpu_pct) > 0)
{
	var _of = draw_get_font();
	var _oh = draw_get_halign();
	var _ov = draw_get_valign();
	draw_set_font(fnt_caption);
	draw_set_halign(fa_center);
	draw_set_valign(fa_bottom);
	with obj_baddie
	{
		if (!ds_map_exists(R.cpu_pct, id) || !visible)
			continue;
		var _pc = floor(ds_map_find_value(R.cpu_pct, id));
		draw_set_color((_pc >= R.kill_pct) ? c_red : c_white);
		draw_text(floor((bbox_left + bbox_right) / 2), bbox_top - 6, string(_pc));
	}
	draw_set_color(c_white);
	draw_set_font(_of);
	draw_set_halign(_oh);
	draw_set_valign(_ov);
}
