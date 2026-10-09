// Extracts Rivals of Aether songs from the player's own copy of the game (data.win + audiogroupN.dat) into .ogg files.
// Nothing from the game is shipped with the mod: this runs on the player's machine, for the songs the config asks for.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "advapi32.lib")

namespace musicx {

inline std::atomic<int> g_state{ 0 };       // 0 idle, 1 running, 2 finished
inline std::atomic<int> g_written{ 0 };
inline std::string g_log;

static inline bool file_exists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static inline bool read_all(const std::string& p, std::vector<char>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg(); f.seekg(0);
    out.resize((size_t)n);
    return (bool)f.read(out.data(), n);
}

static inline bool find_chunk(const std::vector<char>& b, const char* id, size_t& off, size_t& size) {
    if (b.size() < 12 || memcmp(b.data(), "FORM", 4) != 0) return false;
    size_t pos = 8, end = 8 + *(const uint32_t*)(b.data() + 4);
    if (end > b.size()) end = b.size();
    while (pos + 8 <= end) {
        uint32_t sz = *(const uint32_t*)(b.data() + pos + 4);
        if (memcmp(b.data() + pos, id, 4) == 0) { off = pos + 8; size = sz; return true; }
        pos += 8 + (size_t)sz;
    }
    return false;
}

static inline std::string dir_of_exe_dir(const std::string& d) { return d; }

// where is Rivals of Aether? a hint (the config), the running game, Steam's own library list, then the usual places
static inline std::string find_roa(const std::string& hint, DWORD roa_pid) {
    auto ok = [](const std::string& d) { return !d.empty() && file_exists(d + "\\data.win"); };
    std::string h = hint; while (!h.empty() && (h.back() == '\\' || h.back() == '/')) h.pop_back();
    if (ok(h)) return h;
    if (roa_pid) {
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, roa_pid);
        if (ph) {
            char buf[MAX_PATH * 2]; DWORD n = sizeof buf;
            if (QueryFullProcessImageNameA(ph, 0, buf, &n)) { std::string p(buf, n); size_t s = p.find_last_of("\\/"); if (s != std::string::npos) { p.resize(s); if (ok(p)) { CloseHandle(ph); return p; } } }
            CloseHandle(ph);
        }
    }
    std::vector<std::string> libs;
    char steam[MAX_PATH]; DWORD sz = sizeof steam; HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "SteamPath", nullptr, nullptr, (LPBYTE)steam, &sz) == ERROR_SUCCESS) libs.push_back(std::string(steam));
        RegCloseKey(k);
    }
    for (const char* d : { "C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam" }) libs.push_back(d);
    std::vector<std::string> more;
    for (auto& l : libs) {          // libraryfolders.vdf lists every library: "path"   "G:\\games"
        std::string vdf = l + "\\steamapps\\libraryfolders.vdf"; std::vector<char> t;
        if (!read_all(vdf, t)) continue;
        std::string s(t.begin(), t.end()); size_t pos = 0;
        while ((pos = s.find("\"path\"", pos)) != std::string::npos) {
            size_t a = s.find('"', pos + 6); if (a == std::string::npos) break;
            size_t e = s.find('"', a + 1); if (e == std::string::npos) break;
            std::string p = s.substr(a + 1, e - a - 1), q;
            for (size_t i = 0; i < p.size(); ++i) { if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i; q += p[i]; }
            more.push_back(q); pos = e + 1;
        }
    }
    for (auto& m : more) libs.push_back(m);
    for (auto& l : libs) { std::string d = l + "\\steamapps\\common\\Rivals of Aether"; if (ok(d)) return d; }
    return std::string();
}

struct SoundEntry { std::string name; int group; int audio; };

static inline bool read_sond(const std::vector<char>& win, std::vector<SoundEntry>& out) {
    size_t base, size;
    if (!find_chunk(win, "SOND", base, size)) return false;
    uint32_t n = *(const uint32_t*)(win.data() + base);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t off = *(const uint32_t*)(win.data() + base + 4 + 4 * i);
        if ((size_t)off + 36 > win.size()) continue;
        const uint32_t* w = (const uint32_t*)(win.data() + off);
        uint32_t name_p = w[0];
        if (name_p >= win.size()) continue;
        SoundEntry e; e.name = win.data() + name_p; e.group = (int)w[7]; e.audio = (int)w[8];
        out.push_back(e);
    }
    return true;
}

static inline bool audo_entry(const std::vector<char>& buf, int index, const char*& data, uint32_t& len) {
    size_t base, size;
    if (!find_chunk(buf, "AUDO", base, size)) return false;
    uint32_t n = *(const uint32_t*)(buf.data() + base);
    if (index < 0 || (uint32_t)index >= n) return false;
    uint32_t off = *(const uint32_t*)(buf.data() + base + 4 + 4 * (size_t)index);
    if ((size_t)off + 4 > buf.size()) return false;
    len = *(const uint32_t*)(buf.data() + off);
    if ((size_t)off + 4 + len > buf.size()) return false;
    data = buf.data() + off + 4;
    return true;
}

// names: the songs asked for ("music_plasma"); every sound called <name>, <name>_open, <name>_intro, <name>_loop, <name>_loop_i is written
static inline int extract(const std::string& roa_dir, const std::string& out_dir, const std::set<std::string>& songs) {
    std::vector<char> win;
    if (!read_all(roa_dir + "\\data.win", win)) { g_log += "cannot read data.win; "; return 0; }
    std::vector<SoundEntry> sounds;
    if (!read_sond(win, sounds)) { g_log += "no SOND chunk; "; return 0; }
    std::set<std::string> wanted;
    for (auto& s : songs) for (const char* suf : { "", "_open", "_intro", "_loop", "_loop_i" }) wanted.insert(s + suf);
    CreateDirectoryA(out_dir.c_str(), nullptr);
    int written = 0;
    std::vector<char> groups[8]; bool loaded[8] = {};
    for (auto& e : sounds) {
        if (!wanted.count(e.name) || e.audio < 0) continue;
        std::string target = out_dir + "\\" + e.name + ".ogg";
        if (file_exists(target)) continue;
        int g = e.group; if (g < 0 || g > 7) continue;
        const std::vector<char>* src = &win;
        if (g > 0) {
            if (!loaded[g]) { loaded[g] = true; read_all(roa_dir + "\\audiogroup" + std::to_string(g) + ".dat", groups[g]); }
            src = &groups[g];
        }
        const char* data; uint32_t len;
        if (!audo_entry(*src, e.audio, data, len) || len < 4 || memcmp(data, "OggS", 4) != 0) continue;
        std::ofstream f(target, std::ios::binary);
        if (!f) continue;
        f.write(data, len); ++written;
    }
    return written;
}

// runs in the background; poll g_state
static inline void start(const std::string& hint, DWORD roa_pid, const std::string& out_dir, const std::string& csv) {
    if (g_state.load() == 1) return;
    g_state = 1; g_written = 0; g_log.clear();
    std::set<std::string> songs; std::string cur;
    for (char c : csv + ",") { if (c == ',') { if (!cur.empty()) songs.insert(cur); cur.clear(); } else cur += c; }
    std::thread([=] {
        std::string roa = find_roa(hint, roa_pid);
        if (roa.empty()) { g_log = "Rivals of Aether not found; "; g_state = 2; return; }
        g_written = extract(roa, out_dir, songs);
        g_state = 2;
    }).detach();
}

}  // namespace musicx
