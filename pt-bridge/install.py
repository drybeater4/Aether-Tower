"""Apply the PizzaRivals patch set to a working copy of the OpenTower decomp (idempotent).

    python pt-bridge/install.py [C:\\pt]

Never point this at your pristine decomp: edit a copy. Each edit asserts that its anchor text
exists, so a decomp that differs from what we developed against fails loudly.
"""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEST = Path(sys.argv[1] if len(sys.argv) > 1 else r"C:\pt")


def read(p):
    return p.read_bytes().decode("utf-8")


def edit(rel, old, new, marker):
    """Replace `old` with `new` once. `marker` is text that proves the patch is already applied."""
    p = DEST / rel
    s = read(p)
    if marker in s:
        return
    nl = "\r\n" if "\r\n" in s else "\n"
    old, new = old.replace("\n", nl), new.replace("\n", nl)
    if old not in s:
        sys.exit(f"anchor not found in {rel}:\n{old}")
    p.write_bytes(s.replace(old, new, 1).encode("utf-8"))
    print("patched", rel)


# 1. new resources
for sub in ("scripts/scr_rivals", "objects/obj_rivals_controller"):
    shutil.copytree(HERE / "gml" / sub, DEST / sub, dirs_exist_ok=True)

yyp = "PizzaTower_GM2.yyp"
anchor = '    {"id":{"name":"obj_noisecredit","path":"objects/obj_noisecredit/obj_noisecredit.yy",},"order":0,},\n'
edit(yyp, anchor,
     '    {"id":{"name":"obj_rivals_controller","path":"objects/obj_rivals_controller/obj_rivals_controller.yy",},"order":0,},\n'
     '    {"id":{"name":"scr_rivals","path":"scripts/scr_rivals/scr_rivals.yy",},"order":0,},\n' + anchor,
     "obj_rivals_controller")

# 2. state enum + controller creation (both live in the title room's creation code)
title = "rooms/Realtitlescreen/RoomCreationCode.gml"
edit(title, "\tmachcancelstart,\n\tmachcancel\n}", "\tmachcancelstart,\n\tmachcancel,\n\trivals\n}", "\trivals\n}")
p = DEST / title
if "obj_rivals_controller" not in read(p):
    p.write_bytes((read(p).rstrip() + "\nif !instance_exists(obj_rivals_controller)\n\tinstance_create(0, 0, obj_rivals_controller);\n").encode("utf-8"))
    print("patched", title, "(controller)")

# 3. player state machine
step = "objects/obj_player/Step_0.gml"
edit(step, "\tcase states.debugstate:\n\t\tscr_player_debugstate();\n\t\tbreak;\n",
     "\tcase states.debugstate:\n\t\tscr_player_debugstate();\n\t\tbreak;\n\tcase states.rivals:\n\t\tscr_player_rivals();\n\t\tbreak;\n",
     "scr_player_rivals")
edit(step, "state != states.debugstate && state != states.titlescreen", "state != states.debugstate && state != states.rivals && state != states.titlescreen", "states.rivals && state != states.titlescreen")

# 4. PT enemy hits the puppet: PT's normal hurt tail (points loss etc.) runs, RoA takes percent + knockback
edit("scripts/scr_hurtplayer/scr_hurtplayer.gml",
     "\t\telse if state == states.ghost\n\t\t{\n\t\t}\n",
     "\t\telse if state == states.rivals\n\t\t{\n\t\t\trivals_on_hurt(other);\n\t\t\t_hurt = true;\n\t\t}\n\t\telse if state == states.ghost\n\t\t{\n\t\t}\n",
     "rivals_on_hurt")
# RoA's parry beats PT enemy attacks: no damage, the attacker is stunned
edit("scripts/scr_hurtplayer/scr_hurtplayer.gml",
     "\t\telse if state == states.rivals\n\t\t{\n\t\t\trivals_on_hurt(other);",
     "\t\telse if (state == states.rivals && rivals_parry(other))\n\t\t{\n\t\t}\n\t\telse if state == states.rivals\n\t\t{\n\t\t\trivals_on_hurt(other);",
     "rivals_parry(other)")
edit("scripts/scr_hurtplayer/scr_hurtplayer.gml",
     "\t\telse if (state == states.rivals && rivals_parry(other))",
     "\t\telse if (state == states.rivals && rivals_dodging())\n\t\t{\n\t\t}\n\t\telse if (state == states.rivals && rivals_parry(other))",
     "rivals_dodging()")
# hide the TV picture while the rival is out (the Pizza Time timer further down the same draw event stays)
edit("objects/obj_tv/Draw_64.gml",
     "// tv\nif room != strongcold_endscreen\n{",
     "// tv\nif (room != strongcold_endscreen && !(variable_global_exists(\"rivals\") && global.rivals.hide_tv))\n{",
     "global.rivals.hide_tv")
# input: in native mode RoA owns the pad, so Pizza Tower follows what RoA reads (menus, doors, the pause button)
edit("scripts/scr_getinput/scr_getinput.gml",
     "\tkey_shoot = false;\n\tkey_shoot2 = false;\n\tkey_chainsaw = false;\n\tkey_chainsaw2 = false;\n\tkey_left_axis = ",
     "\trivals_synth_player();\n\tkey_shoot = false;\n\tkey_shoot2 = false;\n\tkey_chainsaw = false;\n\tkey_chainsaw2 = false;\n\tkey_left_axis = ",
     "rivals_synth_player()")
edit("scripts/scr_getinput/scr_getinput.gml",
     "\tkey_quit2 = tdp_input_get(\"menu_quit\").pressed || tdp_input_get(\"menu_quitC\").pressed;\n",
     "\tkey_quit2 = tdp_input_get(\"menu_quit\").pressed || tdp_input_get(\"menu_quitC\").pressed;\n\trivals_synth_menu();\n",
     "rivals_synth_menu()")
edit("scripts/scr_getinput/scr_getinput.gml",
     "\trivals_synth_player();\n\tkey_shoot = false;",
     "\trivals_synth_player();\n\trivals_input_gate();\n\tkey_shoot = false;",
     "rivals_input_gate()")
edit("scripts/tdp_input/tdp_input.gml",
     "function tdp_input_update(gamepad = -1)\n{\n",
     "function tdp_input_update(gamepad = -1)\n{\n\tif (variable_global_exists(\"rivals_pad_block\") && global.rivals_pad_block)\n\t\tgamepad = -1;\n",
     "rivals_pad_block")
# Aether Tower: a pause-menu entry that opens a page built with PT's own option-menu framework
p = DEST / "objects/obj_option/Create_0.gml"
if "rivals_option_menus()" not in read(p):
    nl = "\r\n" if "\r\n" in read(p) else "\n"
    p.write_bytes((read(p).rstrip() + nl + "if (variable_global_exists(\"rivals\") && global.rivals.available)" + nl + "\trivals_option_menus();" + nl).encode("utf-8"))
    print("patched obj_option (aether menus)")
edit("objects/obj_option/Step_0.gml",
     "\tif menu == menu_pages.options\n\t{\n\t\tif (instance_exists(obj_mainmenuselect))",
     "\tif (menu == menu_pages.options || (variable_struct_exists(m, \"exit_on_back\") && m.exit_on_back))\n\t{\n\t\tif (instance_exists(obj_mainmenuselect))",
     "exit_on_back")
edit("objects/obj_pause/Create_0.gml",
     "ds_map_set(pause_menu_map, \"pause_restart\", [2, function()",
     "ds_map_set(pause_menu_map, \"pause_aether\", [6, function()\n{\n\trivals_pause_aether();\n}]);\nds_map_set(pause_menu_map, \"pause_restart\", [2, function()",
     "pause_aether")
edit("objects/obj_pause/Step_0.gml",
     "\t\tpause_menu = [\"pause_resume\", \"pause_options\"];\n",
     "\t\tpause_menu = [\"pause_resume\", \"pause_options\"];\n\t\tif (variable_global_exists(\"rivals\") && global.rivals.available)\n\t\t\tarray_push(pause_menu, \"pause_aether\");\n",
     "pause_aether")
edit("scripts/scr_pause/scr_pause.gml",
     "\tinstance_activate_object(obj_globaltimer);\n}",
     "\tinstance_activate_object(obj_globaltimer);\n\tinstance_activate_object(obj_rivals_controller);\n}",
     "obj_rivals_controller")
# the Aether Tower pause entry gets its own icon (index 6) so it does not share the options controller
edit("objects/obj_pause/Create_0.gml",
     "scr_pauseicon_add(spr_pauseicons, 8, 0, -12);\n",
     "scr_pauseicon_add(spr_pauseicons, 8, 0, -12);\nscr_pauseicon_add(spr_pauseicons, 7, 0, -10);\n",
     "spr_pauseicons, 7, 0, -10")
# Steamworks extension: AppID 480 (Spacewar) makes Steam map controllers to keyboard/mouse; use Pizza Tower's real id
edit("extensions/Steamworks/Steamworks.yy", '"defaultValue":"480","exportToINI":false', '"defaultValue":"2231450","exportToINI":false', '"defaultValue":"2231450"')
# 4b. level hazards the rival ignores or takes the RoA way (boost panels, lava, cows, hot wings, Mort, grave surfing, ghost mushroom, corked bottles)
edit("objects/obj_dashpad/Collision_obj_player.gml", "var t = id;\n", "if (other.state == states.rivals)\n\texit;\nvar t = id;\n", "other.state == states.rivals")
edit("objects/obj_boilingsauce/Step_0.gml", "var playerid = instance_place(x, y - 1, obj_player);\n",
     "var playerid = instance_place(x, y - 1, obj_player);\nif (playerid != noone && rivals_lava_touch(playerid))\n\tplayerid = noone;\n", "rivals_lava_touch")
edit("objects/obj_risingboilingsauce/Step_0.gml", "if (place_meeting(x, y - 1, obj_player))\n",
     "if (place_meeting(x, y - 1, obj_player) && !rivals_lava_touch(instance_place(x, y - 1, obj_player)))\n", "rivals_lava_touch")
edit("objects/obj_kentukykenny_projectile/Collision_obj_player.gml", "with other\n{\n\tif character == \"V\"\n\t\tscr_hurtplayer(object_index);",
     "if (other.state == states.rivals)\n\texit;\nwith other\n{\n\tif character == \"V\"\n\t\tscr_hurtplayer(object_index);", "other.state == states.rivals")
edit("objects/obj_donkey/Collision_obj_player.gml", "if cooldown == 0\n{\n\tnotification_push(notifs.cow_kick, [room]);",
     "if (other.state == states.rivals)\n{\n\tif cooldown == 0\n\t{\n\t\tcooldown = 100;\n\t\tfmod_event_one_shot_3d(\"event:/sfx/misc/cowkick\", other.x, other.y);\n\t\tfmod_event_one_shot_3d(\"event:/sfx/misc/cow\", x, y);\n\t\tsprite_index = spr_cowkick;\n\t\timage_index = 0;\n\t\trivals_env_hit(other, -image_xscale, (-image_xscale > 0) ? 40 : 140, 10);\n\t}\n\texit;\n}\nif cooldown == 0\n{\n\tnotification_push(notifs.cow_kick, [room]);",
     "other.state == states.rivals")
edit("objects/obj_mort/Collision_obj_player.gml", "if (sprite_index != spr_mortspawn && !instance_exists(obj_backtohub_fadeout)",
     "if (other.state == states.rivals)\n\texit;\nif (sprite_index != spr_mortspawn && !instance_exists(obj_backtohub_fadeout)", "other.state == states.rivals")
edit("objects/obj_gravesurfing/Collision_obj_player.gml", "if buffer <= 0 && other.state != states.ghost\n", "if buffer <= 0 && other.state != states.ghost && other.state != states.rivals\n", "other.state != states.rivals")
edit("objects/obj_ghosthazard/Collision_obj_player.gml", "with other\n{\n\tif character == \"V\"\n\t\tscr_hurtplayer(id);",
     "if (other.state == states.rivals)\n\texit;\nwith other\n{\n\tif character == \"V\"\n\t\tscr_hurtplayer(id);", "other.state == states.rivals")
edit("objects/obj_superspring/Collision_obj_player.gml", "var v = id;\nwith other\n{", "var v = id;\nif (other.state == states.rivals && image_yscale == -1)\n\texit;\nwith other\n{", "other.state == states.rivals")
# 4c. more level hazards: fans, water, barrels, mushrooms, gustavo, rockets, olives, golf, balloons, rails, jetpacks, presses, hands, pipes, cheese, Mr Pinch, the dresser
RIVAL_EXIT = "if (other.state == states.rivals)\n\texit;\n"
edit("objects/obj_ventilator/Collision_obj_player.gml", "with other\n{\n\tif vsp > -5",
     "if (other.state == states.rivals)\n{\n\trivals_fan(-5);\n\texit;\n}\nwith other\n{\n\tif vsp > -5", "rivals_fan")
edit("objects/obj_water/Step_0.gml", "\twith obj_player\n\t{\n\t\tif state != states.gotoplayer",
     "\twith obj_player\n\t{\n\t\tif (state == states.rivals)\n\t\t{\n\t\t\tif (place_meeting(x, y + 1, other))\n\t\t\t\trivals_lava_touch(id);\n\t\t\tcontinue;\n\t\t}\n\t\tif state != states.gotoplayer", "state == states.rivals")
edit("objects/obj_current/Step_0.gml", "\twith obj_player\n\t{\n\t\tif state != states.golf",
     "\twith obj_player\n\t{\n\t\tif (state == states.rivals)\n\t\t{\n\t\t\tif (place_meeting(x, y + 1, other))\n\t\t\t\trivals_nudge(sign(other.image_xscale) * 8);\n\t\t\tcontinue;\n\t\t}\n\t\tif state != states.golf", "rivals_nudge")
edit("objects/obj_barrel/Collision_obj_player.gml", "if active\n{\n\tif (place_meeting(x, y - 1, other)", RIVAL_EXIT + "if active\n{\n\tif (place_meeting(x, y - 1, other)", "other.state == states.rivals")
edit("objects/obj_woodbarrel/Collision_obj_player.gml", "if ((other.state == states.handstandjump || other.state == states.punch) && other.grounded == true)",
     RIVAL_EXIT + "if ((other.state == states.handstandjump || other.state == states.punch) && other.grounded == true)", "other.state == states.rivals")
edit("objects/obj_mushroom/Collision_obj_player.gml", "if other.cutscene == 0 && sprite_index != spr_bigmushroom_bounce",
     "if (other.state == states.rivals)\n{\n\tif (sprite_index != spr_bigmushroom_bounce)\n\t{\n\t\tfmod_event_one_shot_3d(\"event:/sfx/misc/mushroombounce\", x, y);\n\t\tsprite_index = spr_bigmushroom_bounce;\n\t\timage_index = 0;\n\t\trivals_bounce(-15);\n\t}\n\texit;\n}\nif other.cutscene == 0 && sprite_index != spr_bigmushroom_bounce", "rivals_bounce")
edit("objects/obj_brickgustavo/Collision_obj_player.gml", "with other\n{\n\tif key_up2", RIVAL_EXIT + "with other\n{\n\tif key_up2", "other.state == states.rivals")
edit("objects/obj_rocket/Collision_obj_player.gml", "if buffer > 0\n\texit;", RIVAL_EXIT + "if buffer > 0\n\texit;", "other.state == states.rivals")
edit("objects/obj_antigrav/Collision_obj_player.gml", "var p = other.id;\nwith other\n{\n\tif state != states.antigrav",
     "if (other.state == states.rivals)\n{\n\tif (cooldown == 0)\n\t\trivals_bubble_start(other, id);\n\texit;\n}\nvar p = other.id;\nwith other\n{\n\tif state != states.antigrav", "rivals_bubble_start")
edit("objects/obj_antigravbubble/Step_2.gml", "if (playerid.state != states.antigrav &&", "if (!(playerid.state == states.rivals && global.rivals.bubble) && playerid.state != states.antigrav &&", "global.rivals.bubble")
edit("objects/obj_bigcheese/Collision_obj_player.gml", "if state != states.throwing\n{", RIVAL_EXIT + "if state != states.throwing\n{", "other.state == states.rivals")
edit("objects/obj_pepgoblin_kickhitbox/Collision_obj_player.gml", "with other\n{\n\tif character == \"V\"\n\t\tscr_hurtplayer(object_index);\n\telse if (scr_transformationcheck())",
     RIVAL_EXIT + "with other\n{\n\tif character == \"V\"\n\t\tscr_hurtplayer(object_index);\n\telse if (scr_transformationcheck())", "other.state == states.rivals")
edit("objects/obj_pinballlauncher/Collision_obj_player.gml", "var _obj = id;\nwith other\n{", RIVAL_EXIT + "var _obj = id;\nwith other\n{", "other.state == states.rivals")
edit("objects/obj_pinballtrap/Collision_obj_player.gml", "if _used\n{\n\tvar _obj = id;", "if (other.state == states.rivals)\n\t_used = false;\nif _used\n{\n\tvar _obj = id;", "states.rivals)\n\t_used")
edit("objects/obj_superspring/Collision_obj_player.gml", "if (other.state == states.rivals && image_yscale == -1)\n\texit;",
     "if (other.state == states.rivals)\n{\n\tif (image_yscale == 1 && sprite_index != activatespr)\n\t{\n\t\tfmod_event_one_shot_3d(\"event:/sfx/misc/superspring\", x, y);\n\t\tsprite_index = activatespr;\n\t\timage_index = 0;\n\t\trivals_bounce(-26);\n\t}\n\texit;\n}", "rivals_bounce((room")
edit("objects/obj_sidesuperspring/Collision_obj_player.gml", "with other\n{\n\tif state != states.gotoplayer",
     "if (other.state == states.rivals)\n{\n\tif (sprite_index != spr_sidespringblock_bounce)\n\t{\n\t\tsprite_index = spr_sidespringblock_bounce;\n\t\timage_index = 0;\n\t\trivals_push(image_xscale * 20);\n\t}\n\texit;\n}\nwith other\n{\n\tif state != states.gotoplayer", "rivals_push")
edit("objects/obj_balloongrabbable/Collision_obj_player.gml", "if active\n{\n\tif other.ispeppino", RIVAL_EXIT + "if active\n{\n\tif other.ispeppino", "other.state == states.rivals")
edit("objects/obj_clownballoon_projectile/Collision_obj_player.gml", "if other.clowntimer <= 0", RIVAL_EXIT + "if other.clowntimer <= 0", "other.state == states.rivals")
edit("objects/obj_grindrail/Collision_obj_player.gml", "if (!other.ignore_grind &&", RIVAL_EXIT + "if (!other.ignore_grind &&", "other.state == states.rivals")
edit("objects/obj_pizzapepper/Collision_obj_player.gml", "if visible == true\n{\n\tGamepadSetVibration(0, 0.9, 0.9, 0.8);",
     "if (other.state == states.rivals)\n{\n\tif visible == true\n\t{\n\t\tfmod_event_one_shot_3d(\"event:/sfx/pep/jetpackjump\", x, y);\n\t\tvisible = false;\n\t\tgotowardsplayer = false;\n\t\ttimetovisible = 100;\n\t\trivals_bounce(-16);\n\t}\n\texit;\n}\nif visible == true\n{\n\tGamepadSetVibration(0, 0.9, 0.9, 0.8);", "rivals_bounce(-16)")
edit("objects/obj_noisejetpack/Collision_obj_player.gml", "if state == states.normal && other.grounded", RIVAL_EXIT + "if state == states.normal && other.grounded", "other.state == states.rivals")
edit("objects/obj_boxcrusher/Collision_obj_player.gml", "if other.state == states.gotoplayer\n\texit;", "if (other.state == states.gotoplayer || other.state == states.rivals)\n\texit;", "other.state == states.rivals")
edit("objects/obj_grabbiehand/Collision_obj_player.gml", "\t\tif tauntstoredstate != states.mach2 && tauntstoredstate != states.mach3\n\t\t{", "\t\tif tauntstoredstate != states.mach2 && tauntstoredstate != states.mach3 && tauntstoredstate != states.rivals\n\t\t{", "tauntstoredstate != states.rivals")
edit("objects/obj_grabbiehand/Step_0.gml", "\t\twith playerid\n\t\t{\n\t\t\thsp = 0;\n\t\t\tvsp = 0;\n\t\t\tx = other.x;", "\t\twith playerid\n\t\t{\n\t\t\trivals_drive(true);\n\t\t\thsp = 0;\n\t\t\tvsp = 0;\n\t\t\tx = other.x;", "rivals_drive")
edit("objects/obj_cheeseball/Collision_obj_player.gml", "if other.state != states.gotoplayer && other.state != states.chainsaw\n{\n\tif (other.state == states.knightpep",
     "if (other.state == states.rivals)\n{\n\trivals_env_hit(other, (hsp >= 0) ? 1 : -1, (hsp >= 0) ? 35 : 145, 9);\n\tinstance_destroy();\n\texit;\n}\nif other.state != states.gotoplayer && other.state != states.chainsaw\n{\n\tif (other.state == states.knightpep", "rivals_env_hit(other, (hsp")
edit("objects/obj_tubeexitmach/Step_0.gml", "\t\t\tmachhitAnim = false;\n\t\t\tstate = states.mach3;\n\t\t\tif movespeed < 14\n\t\t\t\tmovespeed = 14;",
     "\t\t\tmachhitAnim = false;\n\t\t\tif (rivals_active())\n\t\t\t\trivals_release(sign(other.image_xscale) * 14, 0);\n\t\t\telse\n\t\t\t{\n\t\t\t\tstate = states.mach3;\n\t\t\t\tif movespeed < 14\n\t\t\t\t\tmovespeed = 14;\n\t\t\t}", "rivals_release")
edit("objects/obj_tubeexitSjump/Step_0.gml", "\t\t\tsprite_index = spr_superspringplayer;\n\t\t\tstate = states.Sjump;\n\t\t\tvsp = -10;",
     "\t\t\tif (rivals_active())\n\t\t\t\trivals_release(0, -14);\n\t\t\telse\n\t\t\t{\n\t\t\t\tsprite_index = spr_superspringplayer;\n\t\t\t\tstate = states.Sjump;\n\t\t\t\tvsp = -10;\n\t\t\t}", "rivals_release")
edit("objects/obj_tubeexitdownexit/Step_0.gml", "\t\t\tstate = states.freefall;\n\t\t\tvsp = 10;\n\t\t\tsprite_index = spr_rockethitwall;",
     "\t\t\tif (rivals_active())\n\t\t\t\trivals_release(0, 10);\n\t\t\telse\n\t\t\t{\n\t\t\t\tstate = states.freefall;\n\t\t\t\tvsp = 10;\n\t\t\t\tsprite_index = spr_rockethitwall;\n\t\t\t}", "rivals_release")
# Mr Pinch lets go: the rival goes on with the momentum
edit("objects/obj_stringycheese/Step_0.gml", "\t\t\t\tsprite_index = spr_machfreefall;\n\t\t\t\tstate = states.jump;\n\t\t\t\twith other\n\t\t\t\t{\n\t\t\t\t\tstate = states.transition;",
     "\t\t\t\tsprite_index = spr_machfreefall;\n\t\t\t\tstate = states.jump;\n\t\t\t\tif (rivals_active())\n\t\t\t\t\trivals_release(movespeed, vsp);\n\t\t\t\twith other\n\t\t\t\t{\n\t\t\t\t\tstate = states.transition;", "rivals_release(hsp, vsp)")
edit("objects/obj_stringycheese/Step_0.gml", "\t\t\t\t\t\tsprite_index = spr_machfreefall;\n\t\t\t\t\t\tstate = states.jump;\n\t\t\t\t\t}",
     "\t\t\t\t\t\tsprite_index = spr_machfreefall;\n\t\t\t\t\t\tstate = states.jump;\n\t\t\t\t\t\tif (rivals_active())\n\t\t\t\t\t\t\trivals_release(hsp, min(vsp, 0));\n\t\t\t\t\t}", "rivals_release(hsp, min")
edit("objects/obj_palettedresser/Collision_obj_player.gml", "if other.key_up2 && other.ispeppino == ispeppino",
     "if (other.state == states.rivals)\n{\n\tif other.key_up2\n\t\trivals_next_palette();\n\texit;\n}\nif other.key_up2 && other.ispeppino == ispeppino", "rivals_next_palette")
# 4d. round 18: water bounce, gustavo portals as teleports, belts, banana, platforms, kids' party, war, bomb crash
edit("objects/obj_water/Step_0.gml", "rivals_lava_touch(id);", "rivals_water_bounce(id);", "rivals_water_bounce")
edit("objects/obj_railparent/Step_0.gml", "draw = bbox_in_camera(view_camera[0], 32);",
     "draw = bbox_in_camera(view_camera[0], 32);\nwith obj_player\n{\n\tif (state == states.rivals && place_meeting(x, y + 1, other))\n\t\trivals_nudge(other.dir * other.movespeed);\n}", "rivals_nudge")
edit("objects/obj_slipnslide/Collision_obj_player.gml", "with other\n{\n\tif state != states.trashroll",
     "if (other.state == states.rivals)\n{\n\tvar _d = (global.rivals.roa_hsp != 0) ? sign(global.rivals.roa_hsp) : global.rivals.roa_dir;\n\tfmod_event_one_shot_3d(\"event:/sfx/pep/slip\", x, y);\n\trivals_env_hit(other, _d, (_d > 0) ? 40 : 140, 10);\n\tdrop = true;\n\tinstance_destroy();\n\texit;\n}\nwith other\n{\n\tif state != states.trashroll", "rivals_env_hit(other, _d")
edit("objects/obj_movingplatform/Step_1.gml", "	with (instance_place(x, y - (2 + abs(v_velocity)), obj_player))\n	{\n",
     "	with (instance_place(x, y - (2 + abs(v_velocity)), obj_player))\n	{\n		if (state == states.rivals)\n			rivals_carry(other.hsp, other.vsp);\n", "rivals_carry")
edit("objects/obj_clownmato/Collision_obj_player.gml", "with other\n{\n\tif ((!instakillmove",
     "if (other.state == states.rivals)\n{\n\tif (state == states.walk)\n\t{\n\t\tfmod_event_one_shot_3d(\"event:/sfx/pep/bumpwall\", x, y);\n\t\tstate = states.bump;\n\t\tsprite_index = spr_clownmato_bounce;\n\t\timage_index = 0;\n\t\trivals_stun(24);\n\t}\n\texit;\n}\nwith other\n{\n\tif ((!instakillmove", "rivals_stun")
edit("objects/obj_stickycheese/Collision_obj_player.gml", "with other\n{\n\tif vsp < 0",
     "if (other.state == states.rivals)\n{\n\tif (global.rivals.roa_vsp < 0)\n\t{\n\t\trepeat 2\n\t\t\tcreate_debris(other.x, other.y + 43, spr_cheesechunk);\n\t\trivals_bounce(global.rivals.roa_vsp * 0.5);\n\t}\n\texit;\n}\nwith other\n{\n\tif vsp < 0", "rivals_bounce(global.rivals.roa_vsp * 0.5)")
for _o in ("obj_patroller", "obj_camerapatrol"):
    edit("objects/" + _o + "/Step_0.gml", "\t\tif state == states.backbreaker && sprite_index == spr_taunt\n\t\t\tp = true;",
         "\t\tif (state == states.backbreaker && sprite_index == spr_taunt) || (state == states.rivals && key_taunt)\n\t\t\tp = true;", "state == states.rivals && (")
edit("objects/obj_nuketerminal/Destroy_0.gml", "\tinstance_create_unique(0, 0, obj_wartimer);",
     "\tif (room == war_1 && !instance_exists(obj_wartimer) && rivals_active())\n\t{\n\t\twith obj_escapecollect\n\t\t\timage_alpha = 1;\n\t\twith obj_music\n\t\t{\n\t\t\tif music != -4\n\t\t\t\tfmod_event_instance_play(music.event);\n\t\t}\n\t}\n\tinstance_create_unique(0, 0, obj_wartimer);", "room == war_1 && !instance_exists")
edit("scripts/scr_collide/scr_collide.gml", "abs(platformid.v_velocity)", "abs(variable_instance_exists(platformid, \"v_velocity\") ? platformid.v_velocity : 0)", "variable_instance_exists(platformid")
edit("objects/obj_stringycheese/Step_0.gml", "rivals_release(movespeed, vsp);", "rivals_release(hsp, vsp);", "rivals_release(hsp, vsp)")
# 4e. round 19: bosses
edit("objects/obj_skateboardnoise/Collision_obj_player.gml", "if (playerid == noone && other.state != states.actor",
     "if (other.state == states.rivals)\n{\n\tif (playerid == noone)\n\t{\n\t\tvar _d = (hsp != 0) ? sign(hsp) : image_xscale;\n\t\trivals_env_hit(other, _d, (_d > 0) ? 25 : 155, 16);\n\t\tinstance_destroy();\n\t}\n\texit;\n}\nif (playerid == noone && other.state != states.actor", "rivals_env_hit(other, _d, (_d > 0) ? 25")
edit("scripts/scr_fakepepboss/scr_fakepepboss.gml", "\t\t\tif !hurted && state != states.grabthrow && state != states.tackle",
     "\t\t\tif (state == states.rivals)\n\t\t\t{\n\t\t\t\tif (!hurted)\n\t\t\t\t{\n\t\t\t\t\tscr_hurtplayer(id);\n\t\t\t\t\tother.state = states.walk;\n\t\t\t\t\tother.cooldown = 150;\n\t\t\t\t}\n\t\t\t}\n\t\t\telse if !hurted && state != states.grabthrow && state != states.tackle", "scr_hurtplayer(id);\n\t\t\t\t\tother.state")
edit("scripts/scr_collide_player/scr_collide_player.gml", "if platformid.v_velocity != 0", "if (variable_instance_exists(platformid, \"v_velocity\") && platformid.v_velocity != 0)", "variable_instance_exists(platformid")
edit("objects/obj_superspring/Collision_obj_player.gml", "rivals_bounce(-26);", "rivals_bounce((room == tower_outside) ? -52 : -26);", "tower_outside")
# 4f. round 20
edit("objects/obj_clownmato/Collision_obj_player.gml", "\tif (state == states.walk)\n\t{\n\t\tfmod_event_one_shot_3d(\"event:/sfx/pep/bumpwall\", x, y);\n\t\tstate = states.bump;\n\t\tsprite_index = spr_clownmato_bounce;\n\t\timage_index = 0;\n\t\trivals_stun(24);\n\t}",
     "\tif (state == states.walk && (!variable_instance_exists(id, \"rv_cd\") || rv_cd < global.rivals.frame))\n\t{\n\t\trv_cd = global.rivals.frame + 90;\n\t\tfmod_event_one_shot_3d(\"event:/sfx/pep/bumpwall\", x, y);\n\t\tstate = states.bump;\n\t\tsprite_index = spr_clownmato_bounce;\n\t\timage_index = 0;\n\t\trivals_stun(24);\n\t\trivals_push((other.x >= x ? 1 : -1) * 8);\n\t\trivals_bounce(-4);\n\t}", "rv_cd")
for _o in ("obj_patroller", "obj_camerapatrol"):
    edit("objects/" + _o + "/Step_0.gml", "(state == states.rivals && key_taunt)", "(state == states.rivals && (global.rivals.roa_flags & 512))", "roa_flags & 512")
edit("objects/obj_nuketerminal/Destroy_0.gml", "\t{\n\t\twith obj_escapecollect\n\t\t\timage_alpha = 1;", "\t{\n\t\tminutes = 2;\n\t\tseconds = 0;\n\t\twith obj_escapecollect\n\t\t\timage_alpha = 1;", "minutes = 2;")
# 4g. round 21: the phase-change cutscene waits for the rival's final hit; the fake Peppino backs off after a hit
G = "scripts/scr_boss_generics/scr_boss_generics.gml"
edit(G, "\t\tcamera_set_view_size(view_camera[0], SCREEN_WIDTH * camzoom, SCREEN_HEIGHT * camzoom);\n\t\twith player\n\t\t{\n\t\t\tif state != states.finishingblow\n\t\t\t{",
     "\t\tcamera_set_view_size(view_camera[0], SCREEN_WIDTH * camzoom, SCREEN_HEIGHT * camzoom);\n\t\tif (rivals_active())\n\t\t{\n\t\t\tif (!variable_instance_exists(id, \"rv_window\") || rv_window < 0)\n\t\t\t{\n\t\t\t\trv_window = 50;\n\t\t\t\trv_done = false;\n\t\t\t\twith player\n\t\t\t\t\trivals_release(0, 0);\n\t\t\t}\n\t\t\telse if (rv_window > 0)\n\t\t\t\trv_window--;\n\t\t\tx = px;\n\t\t\ty = py;\n\t\t\tif (!rv_done && rv_window > 0)\n\t\t\t\texit;\n\t\t\trv_done = true;\n\t\t}\n\t\twith player\n\t\t{\n\t\t\tif (state != states.finishingblow && !rivals_active())\n\t\t\t{", "rv_window = 50")
edit(G, "\t\tif floor(player.image_index >= 4)\n\t\t{", "\t\tif (rivals_active() ? rv_done : floor(player.image_index >= 4))\n\t\t{", "rivals_active() ? rv_done")
edit(G, "\t\t\t\tmovespeed = 4;\n\t\t\t\tstate = states.tackle;\n\t\t\t}\n\t\t\tcheck_grabbed_solid(player);", "\t\t\t\tmovespeed = 4;\n\t\t\t\tstate = rivals_active() ? states.rivals : states.tackle;\n\t\t\t}\n\t\t\tcheck_grabbed_solid(player);", "rivals_active() ? states.rivals")
edit(G, "\t\t\tstate = states.stun;\n\t\t\timage_xscale = -player.xscale;\n\t\t\tinstance_create(x, y, obj_slapstar);", "\t\t\tstate = states.stun;\n\t\t\trv_window = -1;\n\t\t\timage_xscale = -player.xscale;\n\t\t\tinstance_create(x, y, obj_slapstar);", "rv_window = -1;")
edit("objects/obj_fakepeppino/Collision_obj_player.gml", "scr_hurtplayer(other);",
     "if (other.state == states.rivals)\n{\n\tvar _before = other.invtime;\n\tscr_hurtplayer(other);\n\tif (_before <= 0 && other.invtime > 0)\n\t{\n\t\tds_queue_clear(followqueue);\n\t\tx += (other.x >= x) ? -220 : 220;\n\t}\n\texit;\n}\nscr_hurtplayer(other);", "ds_queue_clear(followqueue)")
# 4h. round 22: no bumping the rival away from the gun bosses, the escape chase backs off
edit("objects/obj_vigilanteboss/Step_0.gml", "if (!pizzahead && obj_player1.state != states.actor &&", "if (!pizzahead && obj_player1.state != states.rivals && obj_player1.state != states.actor &&", "obj_player1.state != states.rivals")
edit("objects/obj_pizzafaceboss_p2/Step_0.gml", "if (obj_player1.state != states.actor && obj_player1.y >= (y - 20)", "if (obj_player1.state != states.rivals && obj_player1.state != states.actor && obj_player1.y >= (y - 20)", "obj_player1.state != states.rivals")
edit("objects/obj_fakepepgianthead/Step_0.gml", "\t\t\t\tvar s = obj_player1.state;\n", "\t\t\t\tvar s = obj_player1.state;\n\t\t\t\tvar _inv = obj_player1.invtime;\n", "var _inv = obj_player1.invtime;")
edit("objects/obj_fakepepgianthead/Step_0.gml", "\t\t\t\tif (instance_exists(obj_swapmodeeffect) || (s != obj_player1.state || !obj_player1.ispeppino))\n\t\t\t\t{\n\t\t\t\t\tstate = states.fall;\n\t\t\t\t\ttarget_x = xx - 700;",
     "\t\t\t\tif (instance_exists(obj_swapmodeeffect) || (s != obj_player1.state || !obj_player1.ispeppino) || (obj_player1.state == states.rivals && _inv <= 0 && obj_player1.invtime > 0))\n\t\t\t\t{\n\t\t\t\t\tstate = states.fall;\n\t\t\t\t\ttarget_x = xx - ((obj_player1.state == states.rivals) ? 1000 : 700);", "? 1000 : 700")
# 4i. round 23: the rival really becomes Gustavo in the gustavo areas (the music follows the player's isgustavo)
# 4j. round 24: the gustavo portals run the game's own cutscene; at its end the rival (not a Gustavo model) takes over
edit("objects/obj_gustavoswitch/Step_0.gml", "\t\tscr_switchgustavo();\n\t\tx = obj_gustavoswitch.x;\n\t\ty = obj_gustavoswitch.y;",
     "\t\tif (rivals_active())\n\t\t\trivals_gus_switch(true);\n\t\telse\n\t\t\tscr_switchgustavo();\n\t\tx = obj_gustavoswitch.x;\n\t\ty = obj_gustavoswitch.y;\n\t\tif (rivals_active())\n\t\t\trivals_unstick();", "rivals_gus_switch(true)")
edit("objects/obj_peppinoswitch/Step_0.gml", "\t\tscr_switchpeppino();\n\t\tx = obj_peppinoswitch.x;\n\t\ty = obj_peppinoswitch.y;",
     "\t\tif (rivals_active())\n\t\t\trivals_gus_switch(false);\n\t\telse\n\t\t\tscr_switchpeppino();\n\t\tx = obj_peppinoswitch.x;\n\t\ty = obj_peppinoswitch.y;\n\t\tif (rivals_active())\n\t\t\trivals_unstick();", "rivals_gus_switch(false)")
edit("objects/obj_music/Create_0.gml", "\t\t\tif obj_player1.isgustavo || obj_player1.noisecrusher", "\t\t\tif obj_player1.isgustavo || obj_player1.noisecrusher || (variable_global_exists(\"rivals\") && global.rivals.gus)", "global.rivals.gus")
# 4k. round 27: Rivals sliders in Pizza Tower's audio menu
edit("objects/obj_option/Create_0.gml", "add_option_toggle(audio_menu, 4, \"option_unfocus\", function(val)",
     "if (variable_global_exists(\"rivals\") && global.rivals.available)\n\trivals_audio_menu(audio_menu);\nadd_option_toggle(audio_menu, (variable_global_exists(\"rivals\") && global.rivals.available) ? 6 : 4, \"option_unfocus\", function(val)", "rivals_audio_menu")
# 4l. round 31: a slider can have its own maximum (the Rivals effects volume goes to 150)
edit("objects/obj_option/Step_0.gml", "			option.value = clamp(option.value, 0, 100);", "			option.value = clamp(option.value, 0, variable_struct_exists(option, \"maxv\") ? option.maxv : 100);", "option.maxv")
edit("objects/obj_option/Draw_64.gml", "					var aw = w * (o.value / 100);", "					var aw = w * (o.value / (variable_struct_exists(o, \"maxv\") ? o.maxv : 100));", "o.maxv")
# 4m. round 33: the garbage cans of Oh Shit bounce the rival like mushrooms
edit("objects/obj_trash/Step_0.gml", "\t\twith obj_player\n\t\t{\n\t\t\tif (other.state == states.normal && state != states.trashjumpprep",
     "\t\twith obj_player\n\t\t{\n\t\t\tif (state == states.rivals)\n\t\t\t{\n\t\t\t\tif (other.state == states.normal && other.trashbuffer <= 0 && place_meeting(x, y, other))\n\t\t\t\t{\n\t\t\t\t\tscr_fmod_soundeffect(global.snd_trashjump1, other.x, other.y);\n\t\t\t\t\tother.trashbuffer = 30;\n\t\t\t\t\trivals_bounce(-22);\n\t\t\t\t}\n\t\t\t\tcontinue;\n\t\t\t}\n\t\t\tif (other.state == states.normal && state != states.trashjumpprep", "rivals_bounce(-22)")
# 4n. round 34: the pepperoni goblin / bat kick and the Fun Farm torch guys hurt the rival (damage + knockback)
edit("objects/obj_pepgoblin_kickhitbox/Collision_obj_player.gml", "if (other.state == states.rivals)\n\texit;\nwith other\n{\n\tif character == \"V\"",
     "if (other.state == states.rivals)\n{\n\tif (instance_exists(baddieID) && baddieID.kickbuffer <= 0)\n\t{\n\t\tvar _kd = (baddieID.image_xscale >= 0) ? 1 : -1;\n\t\tif (rivals_env_hit(other, _kd, (_kd > 0) ? 35 : 145, 9))\n\t\t{\n\t\t\twith baddieID\n\t\t\t{\n\t\t\t\tkickbuffer = 100;\n\t\t\t\tinvtime = 50;\n\t\t\t}\n\t\t}\n\t}\n\texit;\n}\nwith other\n{\n\tif character == \"V\"", "var _kd = (baddieID")
edit("objects/obj_farmerbaddie3/Collision_obj_player.gml", "\twith other\n\t{\n\t\tif state != states.boots",
     "\twith other\n\t{\n\t\tif (state == states.rivals)\n\t\t{\n\t\t\trivals_env_hit(id, (other.image_xscale >= 0) ? 1 : -1, (other.image_xscale >= 0) ? 40 : 140, 10);\n\t\t}\n\t\telse if state != states.boots", "rivals_env_hit(id, (other.image_xscale")
edit("objects/obj_farmerbaddie3_projectile/Collision_obj_player.gml", "with other\n{\n\tif state != states.boots",
     "if (other.state == states.rivals)\n{\n\tvar _td = (other.x >= x) ? 1 : -1;\n\tif (rivals_env_hit(other, _td, (_td > 0) ? 60 : 120, 8))\n\t\tinstance_destroy();\n\texit;\n}\nwith other\n{\n\tif state != states.boots", "var _td = (other.x")

# 4o. round 35: Pizza Tower's gamepad inputs also read RoA's view of the pad (while the rival is off)
edit("scripts/tdp_input_classes/tdp_input_classes.gml", "\t\t\t\t\taxis_value = gamepad_button_check(_gamepad, value[1]) - gamepad_button_check(_gamepad, value[0]);\n\t\t\t\t\tpressed = (gamepad_button_check_pressed(_gamepad, value[1]) - gamepad_button_check_pressed(_gamepad, value[0])) != 0;",
     "\t\t\t\t\taxis_value = rivals_pad_check(_gamepad, value[1]) - rivals_pad_check(_gamepad, value[0]);\n\t\t\t\t\tpressed = (rivals_pad_check_pressed(_gamepad, value[1]) - rivals_pad_check_pressed(_gamepad, value[0])) != 0;", "rivals_pad_check(_gamepad, value[1])")
edit("scripts/tdp_input_classes/tdp_input_classes.gml", "\t\t\t\t\tpressed = gamepad_button_check_pressed(_gamepad, value);\n\t\t\t\t\theld = gamepad_button_check(_gamepad, value);\n\t\t\t\t\treleased = gamepad_button_check_released(_gamepad, value);",
     "\t\t\t\t\tpressed = rivals_pad_check_pressed(_gamepad, value);\n\t\t\t\t\theld = rivals_pad_check(_gamepad, value);\n\t\t\t\t\treleased = rivals_pad_check_released(_gamepad, value);", "held = rivals_pad_check(_gamepad, value)")
edit("scripts/tdp_input_classes/tdp_input_classes.gml", "\t\t\t\taxis_value = gamepad_axis_value(_gamepad, value);", "\t\t\t\taxis_value = rivals_pad_axis(_gamepad, value);", "rivals_pad_axis(_gamepad, value)")

# 4p. round 38: the flying bat (obj_pepbat) has no kickbuffer until it kicks: read it safely
edit("objects/obj_pepgoblin_kickhitbox/Collision_obj_player.gml", "if (instance_exists(baddieID) && baddieID.kickbuffer <= 0)",
     "if (instance_exists(baddieID) && (!variable_instance_exists(baddieID, \"kickbuffer\") || baddieID.kickbuffer <= 0))", "variable_instance_exists(baddieID, \"kickbuffer\")")
edit("objects/obj_pepgoblin_kickhitbox/Collision_obj_player.gml", "\t\t\twith baddieID\n\t\t\t{\n\t\t\t\tkickbuffer = 100;\n\t\t\t\tinvtime = 50;\n\t\t\t}",
     "\t\t\tif (baddieID.object_index == obj_pepbat)\n\t\t\t\tbaddieID.hit = true;\n\t\t\twith baddieID\n\t\t\t{\n\t\t\t\tkickbuffer = 100;\n\t\t\t\tinvtime = 50;\n\t\t\t}", "baddieID.hit = true;\n\t\t\twith baddieID")

# 5. diagnostics: when pizzarivals_trace_on.txt exists, log pause-menu input (why can't the menu be navigated?)
p = DEST / "objects/obj_pause/Step_0.gml"
if "rivals_pause_trace" not in read(p):
    nl = "\r\n" if "\r\n" in read(p) else "\n"
    snippet = (HERE / "gml" / "pause_trace.gml").read_text(encoding="utf-8").replace("\n", nl)
    p.write_bytes((read(p).rstrip() + nl + snippet).encode("utf-8"))
    print("patched obj_pause (trace)")
print("done ->", DEST)
