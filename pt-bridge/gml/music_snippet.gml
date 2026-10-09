
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
}

function rivals_music_lookup(kind, name1, name2)
{
	var R = global.rivals;
	for (var i = 0; i < array_length(R.mus_cfg); i++)
	{
		var e = R.mus_cfg[i];
		if (e.kind == kind && (e.name == name1 || (name2 != "" && e.name == name2)))
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
		if (file_exists(spec))
			r.loop = spec;
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
	var _kind = "";
	var _spec = "";
	var _target = noone;
	if (global.panic)
	{
		if (room == tower_finalhallway || obj_music.exitmusic || !fmod_event_instance_is_playing(obj_music.panicmusicID))
			return undefined;
		_kind = global.lap ? "lap2" : "escape";
		_spec = rivals_music_lookup(_kind, _n1, _n2);
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
		audio_sound_gain(M.inst, global.option_master_volume * global.option_music_volume, 0);
	}
}
