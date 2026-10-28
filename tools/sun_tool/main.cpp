// Sun tool: tune the sun and the moon of each area while a game runs.
//
// The game (a build with the debug tools) answers TCP sun_area: the area on
// screen ([video.shadow]: a mod's map id or the scene_keys'), whether it is
// night (gl_renderer_set_night, the title's own detector), the light of that
// area and the light used. Each change here goes to the game at once (the
// live layer, gpu_shadow_light_set: the shadows move on the next picture);
// Apply writes the area's line in sun_areas.txt next to the game's exe, which
// the runtime reads at start (gpu_shadow_lights_load) over game.toml's
// sun_areas. Ctrl+S (system-wide) shows or hides the window; it stays on top.
//
//   psx_sun_tool.exe [--port 4370] [--dir <game folder>]
//   test: --shot out.png [--frames N]: draws off screen and saves a PNG
//
// The 3D view: X = azimuth 0 (the game's "east", the sun_areas azimuths),
// Z = azimuth 90, Y up; it looks the way the game's camera looks (TCP
// "camera") until the view is turned by hand (right button).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_FILE_PATHS
#include "stb_image_write.h"

namespace {

constexpr float k_pi = 3.14159265f, k_deg = k_pi / 180.0f;
enum { L_NONE = 0, L_AT = 1, L_OFF = 2 };   // the framework's GPU_LIGHT_*

struct Light {
    int mode = L_NONE;
    float az = 0.0f, el = 0.0f;
    bool operator==(const Light& o) const {
        return mode == o.mode && (mode != L_AT || (std::fabs(az - o.az) < 0.05f && std::fabs(el - o.el) < 0.05f));
    }
    bool operator!=(const Light& o) const { return !(*this == o); }
};

// ---- the game's answer -------------------------------------------------------------
struct GameState {
    bool ok = false;
    unsigned area = 0;
    float night = 0.0f;
    Light live[2], config[2];
    int off = 0;
    float used_az = 0, used_el = 0;
    int used_age = -1;
    float pass[4] = {135, 40, 0, 40};
    float cam_heading = 0, cam_pitch = 35;
};

const char* find_key(const char* s, const char* key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(s, pat);
    return p ? p + strlen(pat) : nullptr;
}
int read_nums(const char* p, float* out, int n) {   // [a,b,...] or a number
    if (!p) return 0;
    while (*p == ' ' || *p == '[') p++;
    int k = 0;
    while (k < n) {
        char* e;
        const float v = strtof(p, &e);
        if (e == p) break;
        out[k++] = v;
        p = e;
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
    }
    return k;
}
Light read_light(const char* obj, const char* layer) {
    float v[3] = {0, 0, 0};
    Light l;
    if (obj && read_nums(find_key(obj, layer), v, 3) == 3) {
        l.mode = (int)v[0];
        l.az = v[1];
        l.el = v[2];
    }
    return l;
}
bool parse_state(const std::string& r, GameState& g) {
    const char* s = r.c_str();
    if (!strstr(s, "\"ok\":true")) return false;
    const char* a = find_key(s, "area");
    if (!a) return false;
    while (*a == '"' || *a == ' ') a++;
    g.area = (unsigned)strtoul(a, nullptr, 16);
    read_nums(find_key(s, "night"), &g.night, 1);
    const char* sun = find_key(s, "sun");
    const char* moon = find_key(s, "moon");
    g.live[0] = read_light(sun, "live");
    g.config[0] = read_light(sun, "config");
    g.live[1] = read_light(moon, "live");
    g.config[1] = read_light(moon, "config");
    float off = 0;
    read_nums(find_key(s, "off"), &off, 1);
    g.off = (int)off;
    float u[3] = {0, 0, -1};
    read_nums(find_key(s, "used"), u, 3);
    g.used_az = u[0];
    g.used_el = u[1];
    g.used_age = (int)u[2];
    read_nums(find_key(s, "pass"), g.pass, 4);
    float c[2] = {0, 35};
    if (read_nums(find_key(s, "camera"), c, 2) == 2) {
        g.cam_heading = c[0];
        g.cam_pitch = c[1];
    }
    g.ok = true;
    return true;
}

// ---- the link to the game: one request per connection, on a worker thread ----------
class GameLink {
public:
    void start(int port) {
        port_ = port;
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        th_ = std::thread([this] { run(); });
    }
    void stop() {
        quit_ = true;
        if (th_.joinable()) th_.join();
    }
    // a change for one light of one area; a newer one for the same light replaces it
    void send_set(unsigned area, int which, const Light& l) {
        char v[64];
        if (l.mode == L_AT) snprintf(v, sizeof v, "%.1f %.1f", l.az, l.el);
        else snprintf(v, sizeof v, "%s", l.mode == L_OFF ? "off" : "none");
        char req[160];
        snprintf(req, sizeof req, "{\"cmd\":\"sun_area\",\"area\":\"0x%08x\",\"set\":\"%s\",\"v\":\"%s\"}", area,
                 which ? "moon" : "sun", v);
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : pending_)
            if (p.area == area && p.which == which) {
                p.req = req;
                return;
            }
        pending_.push_back({area, which, req});
    }
    bool state(GameState& out) {
        std::lock_guard<std::mutex> lk(mu_);
        out = state_;
        return connected_;
    }
    int port() const { return port_; }

private:
    struct Pending {
        unsigned area;
        int which;
        std::string req;
    };
    bool request(const std::string& line, std::string& resp) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        DWORD to = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof to);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port_);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = connect(s, (sockaddr*)&a, sizeof a) == 0;
        if (ok) {
            const std::string l = line + "\n";
            ok = send(s, l.data(), (int)l.size(), 0) == (int)l.size();
        }
        resp.clear();
        int depth = 0;
        bool in_str = false, esc = false, started = false;
        char buf[4096];
        while (ok) {
            const int n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) {
                ok = false;
                break;
            }
            bool done = false;
            for (int i = 0; i < n && !done; i++) {
                const char c = buf[i];
                resp += c;
                if (in_str) {
                    if (esc) esc = false;
                    else if (c == '\\') esc = true;
                    else if (c == '"') in_str = false;
                } else if (c == '"') in_str = true;
                else if (c == '{') {
                    depth++;
                    started = true;
                } else if (c == '}' && --depth == 0 && started) done = true;
            }
            if (done) break;
        }
        closesocket(s);
        return ok;
    }
    void run() {
        Uint64 last_read = 0;
        while (!quit_) {
            Pending p{0, 0, ""};
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!pending_.empty()) {
                    p = pending_.front();
                    pending_.erase(pending_.begin());
                }
            }
            std::string resp;
            const Uint64 now = SDL_GetTicks();
            if (!p.req.empty()) {
                if (!request(p.req, resp)) set_connected(false);
                continue;
            }
            if (now - last_read < 150) {
                SDL_Delay(10);
                continue;
            }
            last_read = now;
            GameState g;
            const bool ok = request("{\"cmd\":\"sun_area\"}", resp) && parse_state(resp, g);
            std::lock_guard<std::mutex> lk(mu_);
            connected_ = ok;
            if (ok) state_ = g;
            else if (!quit_) last_read = now + 850;   // no game: look again in ~1 s
        }
    }
    void set_connected(bool c) {
        std::lock_guard<std::mutex> lk(mu_);
        connected_ = c;
    }
    int port_ = 4370;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::mutex mu_;
    std::vector<Pending> pending_;
    GameState state_;
    bool connected_ = false;
};

// ---- sun_areas.txt ------------------------------------------------------------------
struct Entry {
    unsigned area = 0;
    Light light[2];
    std::string note;
};
struct SunFile {
    std::string path;
    std::vector<Entry> e;
    std::string error;
    Entry* find(unsigned area) {
        for (auto& x : e)
            if (x.area == area) return &x;
        return nullptr;
    }
    void load() {
        e.clear();
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string note;
            if (char* h = strchr(line, '#')) {
                note = h + 1;
                *h = '\0';
                while (!note.empty() && (note.back() == '\n' || note.back() == '\r' || note.back() == ' '))
                    note.pop_back();
                while (!note.empty() && note.front() == ' ') note.erase(note.begin());
            }
            char* p = line;
            char* t = strtok(p, " \t\r\n");
            if (!t) continue;
            Entry x;
            x.area = (unsigned)strtoul(t, nullptr, 0);
            if (!x.area) continue;
            while ((t = strtok(nullptr, " \t\r\n"))) {
                const int w = !strcmp(t, "moon") ? 1 : !strcmp(t, "sun") ? 0 : -1;
                if (w < 0) break;
                char* v = strtok(nullptr, " \t\r\n");
                if (!v) break;
                if (!strcmp(v, "off")) {
                    x.light[w].mode = L_OFF;
                    continue;
                }
                char* v2 = strtok(nullptr, " \t\r\n");
                if (!v2) break;
                x.light[w] = {L_AT, strtof(v, nullptr), strtof(v2, nullptr)};
            }
            x.note = note;
            e.push_back(x);
        }
        fclose(f);
    }
    bool save() {
        const std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) {
            error = "nao consegui gravar " + tmp;
            return false;
        }
        fprintf(f, "# sun_areas.txt: o sol e a lua de cada area (psx_sun_tool). O jogo le ao abrir;\n"
                   "# vence o [video.shadow] sun_areas do game.toml.\n"
                   "# 0xAREA sun AZ EL|off moon AZ EL|off   # nota\n");
        for (const auto& x : e) {
            if (x.light[0].mode == L_NONE && x.light[1].mode == L_NONE) continue;
            fprintf(f, "0x%08x", x.area);
            for (int w = 0; w < 2; w++) {
                const Light& l = x.light[w];
                if (l.mode == L_AT) fprintf(f, " %s %.1f %.1f", w ? "moon" : "sun", l.az, l.el);
                else if (l.mode == L_OFF) fprintf(f, " %s off", w ? "moon" : "sun");
            }
            if (!x.note.empty()) fprintf(f, "   # %s", x.note.c_str());
            fprintf(f, "\n");
        }
        const bool ok = fclose(f) == 0;
        if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = "nao consegui gravar " + path;
            return false;
        }
        error.clear();
        return true;
    }
};

// ---- 3D view (drawn with ImGui's draw list) -------------------------------------------
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : a;
}
V3 light_dir(float az, float el) {   // toward the light (the framework's u_sun)
    return {std::cos(el * k_deg) * std::cos(az * k_deg), std::sin(el * k_deg),
            std::cos(el * k_deg) * std::sin(az * k_deg)};
}

struct View {
    float heading = 45, pitch = 35, dist = 15;   // where the eye looks along, how far down
    ImVec2 c;                                    // the picture's centre
    float focal = 500;
    V3 eye, f, r, u;
    void setup(ImVec2 centre, float size) {
        c = centre;
        focal = size * 1.1f;
        f = {std::cos(pitch * k_deg) * std::cos(heading * k_deg), -std::sin(pitch * k_deg),
             std::cos(pitch * k_deg) * std::sin(heading * k_deg)};
        // right = up x forward (the game's view space: x right, y up, z ahead)
        r = norm(cross({0, 1, 0}, f));
        u = cross(f, r);
        eye = V3{0, 0.8f, 0} - f * dist;
    }
    bool project(V3 p, ImVec2& out, float* depth = nullptr) const {
        const V3 d = p - eye;
        const float z = dot(d, f);
        if (depth) *depth = z;
        if (z < 0.1f) return false;
        out = ImVec2(c.x + dot(d, r) / z * focal, c.y - dot(d, u) / z * focal);
        return true;
    }
    V3 ray(ImVec2 m) const { return norm(f + r * ((m.x - c.x) / focal) - u * ((m.y - c.y) / focal)); }
};

constexpr float k_dome = 6.0f;

struct Box {
    V3 lo, hi;
    ImU32 col;
};
const Box k_boxes[] = {
    {{-2.2f, 0, -0.6f}, {-1.0f, 3.0f, 0.6f}, IM_COL32(176, 150, 112, 255)},   // a pillar
    {{1.0f, 0, 0.6f}, {2.0f, 1.0f, 1.6f}, IM_COL32(150, 120, 90, 255)},       // a crate
    {{0.4f, 0, -1.9f}, {0.8f, 1.7f, -1.6f}, IM_COL32(90, 120, 170, 255)},     // someone standing
};

ImU32 shade(ImU32 c, float k) {
    const int r = (int)(((c >> IM_COL32_R_SHIFT) & 255) * k), g = (int)(((c >> IM_COL32_G_SHIFT) & 255) * k),
              b = (int)(((c >> IM_COL32_B_SHIFT) & 255) * k);
    return IM_COL32(std::min(r, 255), std::min(g, 255), std::min(b, 255), 255);
}

// convex hull of points on the ground (x, z), Andrew's monotone chain
std::vector<V3> hull(std::vector<V3> p) {
    std::sort(p.begin(), p.end(), [](V3 a, V3 b) { return a.x < b.x || (a.x == b.x && a.z < b.z); });
    auto turn = [](V3 o, V3 a, V3 b) { return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x); };
    std::vector<V3> h(p.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < p.size(); i++) {
        while (k >= 2 && turn(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) {
        while (k >= t && turn(h[k - 2], h[k - 1], p[i - 1]) <= 0) k--;
        h[k++] = p[i - 1];
    }
    h.resize(k > 0 ? k - 1 : 0);
    return h;
}

void draw_scene(ImDrawList* dl, const View& v, ImVec2 p0, ImVec2 p1, const V3& L, bool light_on, bool is_moon,
                const V3* other, bool other_moon, const V3* used) {
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, is_moon ? IM_COL32(18, 22, 40, 255) : IM_COL32(40, 52, 72, 255));
    const float G = 5.0f;   // the ground: a square of 10 x 10
    auto P = [&](V3 a) {
        ImVec2 s;
        v.project(a, s);
        return s;
    };
    const ImU32 ground = is_moon ? IM_COL32(70, 72, 92, 255) : IM_COL32(196, 170, 120, 255);
    ImVec2 g4[4] = {P({-G, 0, -G}), P({G, 0, -G}), P({G, 0, G}), P({-G, 0, G})};
    dl->AddConvexPolyFilled(g4, 4, ground);
    for (int i = -5; i <= 5; i++) {
        const ImU32 gc = i == 0 ? IM_COL32(0, 0, 0, 70) : IM_COL32(0, 0, 0, 30);
        dl->AddLine(P({(float)i, 0, -G}), P({(float)i, 0, G}), gc);
        dl->AddLine(P({-G, 0, (float)i}), P({G, 0, (float)i}), gc);
    }
    // the shadows on the ground: each box's corners pushed down along the light
    if (light_on && L.y > 0.01f) {
        for (const Box& b : k_boxes) {
            std::vector<V3> pts;
            for (int i = 0; i < 8; i++) {
                const V3 c{(i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y, (i & 4) ? b.hi.z : b.lo.z};
                pts.push_back(c - L * (c.y / L.y));
            }
            std::vector<V3> h = hull(pts);
            std::vector<ImVec2> s;
            for (const V3& q : h) s.push_back(P({q.x, 0.001f, q.z}));
            if (s.size() >= 3) dl->AddConvexPolyFilled(s.data(), (int)s.size(), IM_COL32(0, 0, 0, 110));
        }
    }
    // the dome the light moves on: rings at 0/30/60 degrees, meridians every 45
    auto dome_line = [&](V3 a, V3 b, ImU32 col) {
        ImVec2 sa, sb;
        float da, db;
        if (!v.project(a, sa, &da) || !v.project(b, sb, &db)) return;
        const float dc = dot(V3{0, 0, 0} - v.eye, v.f);
        const bool back = (da + db) * 0.5f > dc;
        dl->AddLine(sa, sb, back ? (col & 0x00FFFFFF) | 0x30000000 : col, 1.0f);
    };
    const ImU32 dc = IM_COL32(255, 255, 255, 110);
    for (int el = 0; el <= 60; el += 30)
        for (int a = 0; a < 360; a += 6)
            dome_line(light_dir((float)a, (float)el) * k_dome, light_dir((float)(a + 6), (float)el) * k_dome,
                      el ? dc : IM_COL32(255, 255, 255, 170));
    for (int a = 0; a < 360; a += 45)
        for (int el = 0; el < 90; el += 6)
            dome_line(light_dir((float)a, (float)el) * k_dome, light_dir((float)a, (float)(el + 6)) * k_dome, dc);
    for (int a = 0; a < 360; a += 90) {
        ImVec2 s;
        char t[16];
        snprintf(t, sizeof t, "%d°", a);
        if (v.project(light_dir((float)a, 0) * (k_dome * 1.1f), s))
            dl->AddText(ImVec2(s.x - 10, s.y - 8), IM_COL32(255, 255, 255, 220), t);
    }
    // the boxes, faces far to near (painter's), lit by the light
    struct Face {
        ImVec2 p[4];
        float depth;
        ImU32 col;
    };
    std::vector<Face> faces;
    const V3 Ld = light_on ? L : V3{0, 1, 0};
    for (const Box& b : k_boxes) {
        const V3 c[8] = {{b.lo.x, b.lo.y, b.lo.z}, {b.hi.x, b.lo.y, b.lo.z}, {b.hi.x, b.hi.y, b.lo.z},
                         {b.lo.x, b.hi.y, b.lo.z}, {b.lo.x, b.lo.y, b.hi.z}, {b.hi.x, b.lo.y, b.hi.z},
                         {b.hi.x, b.hi.y, b.hi.z}, {b.lo.x, b.hi.y, b.hi.z}};
        static const int fi[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
        static const V3 fn[6] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (int k = 0; k < 6; k++) {
            const V3 mid = (c[fi[k][0]] + c[fi[k][2]]) * 0.5f;
            if (dot(fn[k], v.eye - mid) <= 0) continue;
            Face f;
            bool ok = true;
            for (int i = 0; i < 4; i++) ok &= v.project(c[fi[k][i]], f.p[i]);
            if (!ok) continue;
            f.depth = dot(mid - v.eye, v.f);
            const float lit = light_on ? std::max(0.0f, dot(fn[k], Ld)) : 0.5f;
            f.col = shade(b.col, 0.42f + 0.58f * lit);
            faces.push_back(f);
        }
    }
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth > b.depth; });
    for (const Face& f : faces) {
        dl->AddConvexPolyFilled(f.p, 4, f.col);
        dl->AddPolyline(f.p, 4, IM_COL32(0, 0, 0, 90), ImDrawFlags_Closed, 1.0f);
    }
    // the other light (dim), the light used in the game (a ring), the light being edited
    auto marker = [&](const V3& d, bool moon, bool main) {
        ImVec2 s, o;
        if (!v.project(d * k_dome, s) || !v.project({0, 0, 0}, o)) return;
        const ImU32 col = moon ? IM_COL32(190, 210, 255, main ? 255 : 90) : IM_COL32(255, 210, 60, main ? 255 : 90);
        if (main) dl->AddLine(o, s, (col & 0x00FFFFFF) | 0x90000000, 2.0f);
        dl->AddCircleFilled(s, main ? 13.0f : 8.0f, col);
        if (moon) dl->AddCircleFilled(ImVec2(s.x + (main ? 5.0f : 3.0f), s.y - (main ? 3.0f : 2.0f)), main ? 10.0f : 6.0f,
                                      is_moon ? IM_COL32(18, 22, 40, 255) : IM_COL32(40, 52, 72, 255));
    };
    if (other) marker(*other, other_moon, false);
    if (used) {
        ImVec2 s;
        if (v.project(*used * k_dome, s)) dl->AddCircle(s, 18.0f, IM_COL32(255, 255, 255, 200), 0, 2.0f);
    }
    if (light_on) marker(L, is_moon, true);
    dl->PopClipRect();
}

// the point of the dome under the mouse (of its two, the one nearer `near`)
bool pick_dome(const View& v, ImVec2 m, const V3& nearp, V3& out) {
    const V3 d = v.ray(m), o = v.eye;
    const float b = dot(o, d), c = dot(o, o) - k_dome * k_dome, disc = b * b - c;
    if (disc < 0) {   // missed: the dome's rim nearest the ray
        out = norm(o - d * b) * k_dome;
    } else {
        const float s = std::sqrt(disc);
        const V3 a = o + d * (-b - s), z = o + d * (-b + s);
        out = dot(a - nearp, a - nearp) <= dot(z - nearp, z - nearp) ? a : z;
    }
    out = norm(out) * k_dome;
    return true;
}

// ---- the tool ----------------------------------------------------------------------------
struct Tool {
    GameLink link;
    SunFile file;
    GameState g;
    bool connected = false;
    unsigned area = 0;     // the area being edited
    Light edit[2];         // what the game is shown for it
    char note[96] = "";
    int which = 0;         // 0 sun, 1 moon
    bool follow_night = true, follow_cam = true, dragging = false;
    View view;
    std::string msg;
    bool hotkey = false;   // Ctrl+S registered

    Light saved(unsigned a, int w) {
        Entry* x = file.find(a);
        return x ? x->light[w] : Light{};
    }
    // the light the area has with no entry of its own here (game.toml's, else the preset's)
    Light fallback(int w) const {
        if (w == 0) {
            if (g.config[0].mode != L_NONE) return g.config[0];
            return {L_AT, g.pass[0] > 0 ? g.pass[0] : 135.0f, g.pass[1] > 0 ? g.pass[1] : 40.0f};
        }
        const Light s = fallback_sun_now();
        return {L_AT, std::fmod(s.az + g.pass[2] + 360.0f, 360.0f), g.pass[3] > 0 ? g.pass[3] : 40.0f};
    }
    Light fallback_sun_now() const {   // the sun the preset moon turns from
        if (edit[0].mode == L_AT) return edit[0];
        const Light f = g.config[0];
        if (f.mode == L_AT) return f;
        return {L_AT, g.pass[0] > 0 ? g.pass[0] : 135.0f, g.pass[1] > 0 ? g.pass[1] : 40.0f};
    }
    Light shown(int w) const { return edit[w].mode == L_NONE ? fallback(w) : edit[w]; }
    bool dirty(int w) { return edit[w] != saved(area, w); }

    void switch_area(unsigned a) {
        // what was not applied goes back to the file's in the area left
        if (area)
            for (int w = 0; w < 2; w++)
                if (dirty(w)) link.send_set(area, w, saved(area, w));
        area = a;
        for (int w = 0; w < 2; w++) edit[w] = saved(a, w);
        Entry* x = file.find(a);
        snprintf(note, sizeof note, "%s", x ? x->note.c_str() : "");
        msg.clear();
    }
    void change(int w, Light l) {
        if (!area) return;
        if (l.mode == L_AT) {
            l.az = std::fmod(std::fmod(l.az, 360.0f) + 360.0f, 360.0f);
            l.el = std::clamp(l.el, 1.0f, 89.0f);
        }
        if (l == edit[w]) return;
        edit[w] = l;
        link.send_set(area, w, l);
    }
    void apply() {
        Entry* x = file.find(area);
        if (!x) {
            file.e.push_back(Entry{area, {}, ""});
            x = &file.e.back();
        }
        x->light[0] = edit[0];
        x->light[1] = edit[1];
        x->note = note;
        if (x->light[0].mode == L_NONE && x->light[1].mode == L_NONE && !note[0])
            file.e.erase(file.e.begin() + (x - file.e.data()));
        msg = file.save() ? "Gravado em " + file.path : file.error;
    }
    void default_light(int w) {   // no entry of its own for this light: the file's line loses it
        change(w, Light{});
        Entry* x = file.find(area);
        if (x) {
            x->light[w] = Light{};
            if (x->light[0].mode == L_NONE && x->light[1].mode == L_NONE && x->note.empty())
                file.e.erase(file.e.begin() + (x - file.e.data()));
            msg = file.save() ? "Padrão gravado" : file.error;
        }
    }
    void revert() {
        for (int w = 0; w < 2; w++)
            if (dirty(w)) change(w, saved(area, w));
        Entry* x = file.find(area);
        snprintf(note, sizeof note, "%s", x ? x->note.c_str() : "");
    }

    void frame(float W, float H) {
        connected = link.state(g);
        if (connected && g.area && g.area != area) switch_area(g.area);
        if (follow_night && connected) which = g.night > 0.5f ? 1 : 0;
        if (follow_cam && connected && g.used_age >= 0) {
            view.heading = g.cam_heading;
            view.pitch = std::clamp(g.cam_pitch, 5.0f, 85.0f);
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float side = std::max(ImGui::GetFontSize() * 24.0f, W * 0.38f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 vs(W - side - ImGui::GetStyle().WindowPadding.x * 3, H - ImGui::GetStyle().WindowPadding.y * 2);
        scene(p0, vs);
        ImGui::SameLine();
        ImGui::BeginChild("##side", ImVec2(0, 0));
        panel();
        ImGui::EndChild();
        ImGui::End();
    }

    void scene(ImVec2 p0, ImVec2 size) {
        ImGui::InvisibleButton("##view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        view.setup(ImVec2(p0.x + size.x * 0.5f, p0.y + size.y * 0.55f), std::min(size.x, size.y));
        const Light cur = shown(which);
        const bool on = cur.mode == L_AT;
        V3 L = light_dir(cur.az, cur.el);
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && area) {
            V3 hit;
            const Light from = on ? cur : Light{L_AT, cur.az, 45.0f};
            pick_dome(view, io.MousePos, light_dir(from.az, from.el) * k_dome, hit);
            const float el = std::asin(std::clamp(hit.y / k_dome, -1.0f, 1.0f)) / k_deg;
            const float az = std::atan2(hit.z, hit.x) / k_deg;
            change(which, Light{L_AT, std::round(az * 2.0f) / 2.0f, std::round(std::clamp(el, 1.0f, 89.0f) * 2.0f) / 2.0f});
            L = light_dir(edit[which].az, edit[which].el);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)) {
            follow_cam = false;
            view.heading -= io.MouseDelta.x * 0.4f;
            view.pitch = std::clamp(view.pitch + io.MouseDelta.y * 0.3f, 5.0f, 85.0f);
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
            view.dist = std::clamp(view.dist * (io.MouseWheel > 0 ? 0.9f : 1.1f), 8.0f, 40.0f);
        const Light oth = shown(1 - which);
        const V3 od = light_dir(oth.az, oth.el);
        const V3 ud = light_dir(g.used_az, g.used_el);
        const bool used_here = connected && g.used_age >= 0 && g.used_age < 30 && !g.off;
        draw_scene(ImGui::GetWindowDrawList(), view, p0, p1, L, shown(which).mode == L_AT, which == 1,
                   oth.mode == L_AT ? &od : nullptr, which == 0, used_here ? &ud : nullptr);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p0.x + 10, p0.y + 8), IM_COL32(255, 255, 255, 200),
                    "Arraste com o botão esquerdo: move a luz.  Direito: gira a vista.  Roda: zoom.");
        if (!follow_cam)
            dl->AddText(ImVec2(p0.x + 10, p0.y + 8 + ImGui::GetTextLineHeightWithSpacing()), IM_COL32(255, 220, 120, 220),
                        "Vista girada à mão (Seguir a câmera do jogo: no painel)");
    }

    void light_controls(int w) {
        const char* name = w ? "a lua" : "o sol";
        Light cur = shown(w);
        const bool own = edit[w].mode != L_NONE;
        bool off = edit[w].mode == L_OFF;
        ImGui::PushID(w);
        ImGui::BeginDisabled(!area);
        if (ImGui::Checkbox(w ? "Sem sombras à noite (off)" : "Sem sol nesta área (off: caverna)", &off))
            change(w, off ? Light{L_OFF, 0, 0} : Light{L_AT, cur.mode == L_AT ? cur.az : fallback(w).az,
                                                         cur.mode == L_AT ? cur.el : fallback(w).el});
        ImGui::BeginDisabled(off);
        float az = cur.mode == L_AT ? cur.az : fallback(w).az, el = cur.mode == L_AT ? cur.el : fallback(w).el;
        const float lw = ImGui::CalcTextSize("Elevação  ").x;
        ImGui::SetNextItemWidth(-lw);
        bool ch = ImGui::SliderFloat("Azimute", &az, 0.0f, 360.0f, "%.1f°");
        ImGui::SetNextItemWidth(-lw);
        ch |= ImGui::SliderFloat("Elevação", &el, 1.0f, 89.0f, "%.1f°");
        if (ch) change(w, Light{L_AT, az, el});
        ImGui::EndDisabled();
        const Light sv = saved(area, w);
        if (sv.mode == L_AT) ImGui::TextDisabled("Gravado: %.1f° / %.1f°", sv.az, sv.el);
        else if (sv.mode == L_OFF) ImGui::TextDisabled("Gravado: off");
        else {
            const Light f = fallback(w);
            const char* src = w ? "preset do GI (u_moon)" : g.config[0].mode != L_NONE ? "game.toml" : "preset do GI (u_sun)";
            if (f.mode == L_OFF) ImGui::TextDisabled("Sem entrada: %s = off", src);
            else ImGui::TextDisabled("Sem entrada: %s = %.1f° / %.1f°", src, f.az, f.el);
        }
        if (own && !dirty(w)) ImGui::SameLine(), ImGui::TextDisabled("(próprio)");
        if (dirty(w)) ImGui::SameLine(), ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "(não aplicado)");
        if (ImGui::Button(w ? "Padrão da lua" : "Padrão do sol")) default_light(w);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tira a entrada desta área para %s (volta ao game.toml ou ao preset) e grava.", name);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    void panel() {
        if (hotkey) ImGui::TextDisabled("Ctrl+S: mostra/esconde esta janela (também com o jogo na frente)");
        else ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Ctrl+S indisponível (outro programa já usa)");
        if (!connected) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Jogo: sem conexão (porta %d)", link.port());
            ImGui::TextWrapped("Abra o jogo com --debug-port %d (build com as ferramentas de debug).", link.port());
        } else {
            ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "Jogo: conectado (porta %d)", link.port());
        }
        ImGui::Separator();
        if (area) ImGui::Text("Área: 0x%08x", area);
        else ImGui::TextDisabled("Área: nenhuma ainda (entre num lugar com chão 3D)");
        if (connected && g.area != area && area) ImGui::TextDisabled("(na tela: %s)", g.area ? "outra" : "sem area");
        if (connected) {
            const bool night = g.night > 0.5f;
            ImGui::Text("Agora: %s", night ? "NOITE (vale a lua)" : "DIA (vale o sol)");
            if (g.night > 0.0f && g.night < 1.0f) ImGui::SameLine(), ImGui::TextDisabled("(%.0f%%)", g.night * 100.0f);
            if (g.off) ImGui::Text("Valendo: sem sombras (off)");
            else if (g.used_age < 0 || g.used_age > 30)
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Sem mapa de sombras agora (GI e SS desligados, ou tela 2D)");
            else ImGui::Text("Valendo: %.1f° / %.1f°", g.used_az, g.used_el);
            ImGui::TextDisabled("A lua só vale onde o jogo detecta noite (no BoF4: Config > Lights > Night).");
        }
        ImGui::BeginDisabled(!area);
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Elevação  ").x);
        ImGui::InputText("Nota", note, sizeof note);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::Checkbox("Acompanhar dia/noite", &follow_night) && follow_night && connected)
            which = g.night > 0.5f ? 1 : 0;
        if (ImGui::RadioButton("Sol", which == 0)) which = 0, follow_night = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Lua", which == 1)) which = 1, follow_night = false;
        if (which == 1 && connected && g.night <= 0.5f)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "É dia na tela: a lua só aparece numa cena de noite.");
        ImGui::Spacing();
        light_controls(which);
        ImGui::Separator();
        ImGui::BeginDisabled(!area);
        if (ImGui::Button("Aplicar (gravar)")) apply();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty(0) && !dirty(1));
        if (ImGui::Button("Descartar")) revert();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
        ImGui::Checkbox("Seguir a câmera do jogo", &follow_cam);
        ImGui::Separator();
        ImGui::Text("Áreas gravadas (%d)", (int)file.e.size());
        if (ImGui::BeginTable("##areas", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, std::max(80.0f, ImGui::GetContentRegionAvail().y)))) {
            ImGui::TableSetupColumn("Área");
            ImGui::TableSetupColumn("Sol");
            ImGui::TableSetupColumn("Lua");
            ImGui::TableSetupColumn("Nota");
            ImGui::TableHeadersRow();
            for (const Entry& x : file.e) {
                ImGui::TableNextRow();
                if (x.area == area) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(80, 80, 30, 160));
                ImGui::TableNextColumn();
                ImGui::Text("%08x", x.area);
                for (int w = 0; w < 2; w++) {
                    ImGui::TableNextColumn();
                    const Light& l = x.light[w];
                    if (l.mode == L_AT) ImGui::Text("%.0f/%.0f", l.az, l.el);
                    else if (l.mode == L_OFF) ImGui::Text("off");
                    else ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(x.note.c_str());
            }
            ImGui::EndTable();
        }
    }
};

// framebuffer objects for --shot (GL 3.0, looked up at run time)
typedef void(APIENTRY* PFN_GenFB)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_BindFB)(GLenum, GLuint);
typedef void(APIENTRY* PFN_FBTex)(GLenum, GLenum, GLenum, GLuint, GLint);
constexpr GLenum k_fb = 0x8D40, k_color0 = 0x8CE0;
struct Shot {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    bool make(int pw, int ph) {
        auto gen = (PFN_GenFB)SDL_GL_GetProcAddress("glGenFramebuffers");
        auto bind = (PFN_BindFB)SDL_GL_GetProcAddress("glBindFramebuffer");
        auto attach = (PFN_FBTex)SDL_GL_GetProcAddress("glFramebufferTexture2D");
        if (!gen || !bind || !attach) return false;
        w = pw;
        h = ph;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gen(1, &fbo);
        bind(k_fb, fbo);
        attach(k_fb, k_color0, GL_TEXTURE_2D, tex, 0);
        return true;
    }
    bool save(const std::string& path) {
        std::vector<unsigned char> px((size_t)w * h * 4), flip(px.size());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < h; y++) memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
        int len = 0;
        unsigned char* png = stbi_write_png_to_mem(flip.data(), w * 4, w, h, 4, &len);
        if (!png) return false;
        FILE* f = fopen(path.c_str(), "wb");
        bool ok = false;
        if (f) {
            ok = fwrite(png, 1, (size_t)len, f) == (size_t)len;
            fclose(f);
        }
        STBIW_FREE(png);
        return ok;
    }
};

std::string exe_dir() {
    char p[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p, n);
    const size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "." : s.substr(0, k);
}

// Ctrl+S anywhere (the game in front too): shows or hides the window. A
// system-wide hotkey (RegisterHotKey); its WM_HOTKEY comes through SDL's
// message pump, seen here before SDL dispatches it.
constexpr int k_hotkey = 1;
std::atomic<int> g_toggles{0};
bool SDLCALL hotkey_hook(void*, MSG* m) {
    if (m->message == WM_HOTKEY && m->wParam == k_hotkey) g_toggles++;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int port = 4370, shot_frames = 60;
    std::string dir = exe_dir(), shot_path, test_set;
    bool test_apply = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
        else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--shot" && i + 1 < argc) shot_path = argv[++i];
        else if (a == "--frames" && i + 1 < argc) shot_frames = atoi(argv[++i]);
        // tests (with --shot): a change as if dragged ("sun AZ EL", "moon off",
        // "sun none" = the Default button), then Apply
        else if (a == "--test-set" && i + 1 < argc) test_set = argv[++i];
        else if (a == "--test-apply") test_apply = true;
    }
    const bool shot_mode = !shot_path.empty();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxA(nullptr, SDL_GetError(), "Sun tool", MB_ICONERROR);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    float cs = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (cs <= 0.0f) cs = 1.0f;
    // always on top of the game's window
    SDL_Window* win = SDL_CreateWindow("Sol e lua por área (Ctrl+S mostra/esconde)", (int)(1000 * cs),
                                       (int)(600 * cs),
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                           SDL_WINDOW_ALWAYS_ON_TOP);
    if (!win) return 1;
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(cs);
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(font, 17.0f * cs);
    else io.FontGlobalScale = cs;
    ImGui_ImplSDL3_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    Tool* t = new Tool();
    t->file.path = dir + "\\sun_areas.txt";
    t->file.load();
    t->link.start(port);
    Shot shot;
    if (shot_mode) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (!shot.make(pw, ph)) return 2;
    } else {
        SDL_ShowWindow(win);
        SDL_SetWindowsMessageHook(hotkey_hook, nullptr);
        t->hotkey = RegisterHotKey(nullptr, k_hotkey, MOD_CONTROL | MOD_NOREPEAT, 'S') != 0;
    }
    int frames = 0;
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        for (; g_toggles > 0; g_toggles--) {
            if (SDL_GetWindowFlags(win) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) {
                SDL_ShowWindow(win);
                SDL_RestoreWindow(win);
                SDL_RaiseWindow(win);
            } else {
                SDL_HideWindow(win);
            }
        }
        if (!shot_mode && (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) {
            SDL_WaitEventTimeout(nullptr, 50);   // hidden: nothing to draw (Ctrl+S still comes)
            continue;
        }
        int pw = 1, ph = 1;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        t->frame(io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Render();
        if (shot_mode) {
            auto bind = (PFN_BindFB)SDL_GL_GetProcAddress("glBindFramebuffer");
            bind(k_fb, shot.fbo);
        }
        glViewport(0, 0, pw, ph);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot_mode) {
            ++frames;
            if (frames == shot_frames / 3 && !test_set.empty() && t->area) {
                char w[8] = "", v[16] = "";
                float az = 0, el = 0;
                const int n = sscanf(test_set.c_str(), "%7s %15s %f", w, &v[0], &el);
                const int which = strcmp(w, "moon") == 0;
                t->follow_night = false;
                t->which = which;
                if (!strcmp(v, "off")) t->change(which, Light{L_OFF, 0, 0});
                else if (!strcmp(v, "none")) t->default_light(which);
                else if (n == 3) az = strtof(v, nullptr), t->change(which, Light{L_AT, az, el});
                printf("test-set %s on 0x%08x\n", test_set.c_str(), t->area);
            }
            if (frames == shot_frames * 2 / 3 && test_apply) {
                t->apply();
                printf("test-apply: %s\n", t->msg.c_str());
            }
            if (frames >= shot_frames) {
                const bool ok = shot.save(shot_path);
                printf("%s %s\n", ok ? "saved" : "FAILED", shot_path.c_str());
                quit = true;
            }
            SDL_Delay(16);
        } else {
            SDL_GL_SwapWindow(win);
        }
    }
    // what was not applied goes back to the file's before the tool closes
    if (t->area)
        for (int w = 0; w < 2; w++)
            if (t->dirty(w)) t->link.send_set(t->area, w, t->saved(t->area, w));
    SDL_Delay(400);   // the worker sends it
    if (t->hotkey) UnregisterHotKey(nullptr, k_hotkey);
    t->link.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    fflush(stdout);
    ExitProcess(0);
}
