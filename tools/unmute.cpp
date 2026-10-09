#include "../pt-bridge/extension/audio_loopback.h"
#include <cstdio>
int main(int argc, char** argv) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); praudio::Bridge b; b.pid = (DWORD)atoi(argv[1]); b.set_session_volume(1.0f, true); printf("unmuted %lu\n", (unsigned long)b.pid); return 0; }
