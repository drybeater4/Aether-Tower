// the game is closing: stop the audio bridge, which gives Rivals of Aether's own volume back (Windows remembers it per program)
if (variable_global_exists("rivals") && global.rivals.available)
	external_call(global.rivals.fn.astop);
