#include "../pt-bridge/extension/music_extract.h"
#include <cstdio>
int main(int argc, char** argv) {
    std::string roa = musicx::find_roa(argc > 3 ? argv[3] : "", 0);
    printf("roa dir: %s\n", roa.c_str());
    std::set<std::string> s; s.insert(argc > 2 ? argv[2] : "music_plasma");
    int n = musicx::extract(roa, argv[1], s);
    printf("written %d %s\n", n, musicx::g_log.c_str());
    return 0;
}
