if (pause && variable_global_exists("rivals_trace_on") && global.rivals_trace_on) // rivals_pause_trace
{
	var _tf = file_text_open_append(working_directory + "pizzarivals_pause_trace.txt");
	file_text_write_string(_tf, string(current_time) + " focus=" + string(window_has_focus()) + " up2=" + string(key_up2) + " down2=" + string(key_down2) + " jump=" + string(key_jump) + " sel=" + string(selected) + " opt=" + string(instance_exists(obj_option)) + " a3=" + string(alarm[3]) + " dev=" + string(obj_inputAssigner.player_input_device[0]) + " players=" + string(instance_number(obj_player)));
	file_text_writeln(_tf);
	file_text_close(_tf);
}
