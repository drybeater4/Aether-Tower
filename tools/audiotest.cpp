#include "../pt-bridge/extension/audio_loopback.h"
#include <cstdio>
int main(int argc, char** argv) {
    DWORD pid = argc > 1 ? (DWORD)atoi(argv[1]) : 0; int secs = argc > 2 ? atoi(argv[2]) : 20; bool mute = argc > 3 ? atoi(argv[3]) != 0 : true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    praudio::Bridge b; b.mute_target = mute; if (argc > 4) b.target_volume = (float)atof(argv[4]); b.gain = 1.0f; b.pid = pid; b.start(pid);
    for (int i = 0; i < secs * 2; ++i) {
        Sleep(500); float r = b.rms.exchange(0.0f); float sp = b.session_peak();
        printf("t=%.1f state=%d hr=%lx rms=%.4f sessionpeak=%.3f pk=%ld silent=%ld frames=%ld room0=%ld maxabs=%d\n", i * 0.5, (int)b.state, (unsigned long)b.hr_fail, r, sp, b.npk.load(), b.nsilent.load(), b.nframes.load(), b.nroom0.load(), b.maxabs.exchange(0));
        fflush(stdout);
    }
    b.stop(); return 0;
}
