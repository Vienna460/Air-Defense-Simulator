#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// CONSTANTS

static const int WIN_W   = 960;
static const int WIN_H   = 600;
static const int PANEL_W = 215;   // left stage-panel width

static const int RAD_CX  = 590;   // radar circle centre x
static const int RAD_CY  = 252;   // radar circle centre y
static const int RAD_R   = 230;   // radar circle radius

static const int LOG_Y   = 502;   // top of event-log strip

// Weapon system positions (fixed, absolute screen coords)
struct WpnDef { float x, y; const char* type; float range; };
static const WpnDef WEAPONS[4] = {
    {RAD_CX - 118, RAD_CY - 108, "SAM",  132},
    {RAD_CX + 122, RAD_CY - 103, "SAM",  132},
    {RAD_CX -  88, RAD_CY + 112, "CIWS",  76},
    {RAD_CX +  90, RAD_CY + 112, "CIWS",  76},
};

// Threat catalogue
static const char*  T_NAME[4]  = { "Fighter Jet", "Cruise Missile",
                                    "Ballistic Missile", "UAV Drone" };
static const float  T_SPEED[4] = { 0.37f, 0.54f, 0.72f, 0.24f };

// Stage durations in ms; -1 = event-driven (sweep detection / interceptor hit)
static const float  S_DUR[8]  = { -1, 950, 1400, 1050, 850, 700, -1, 1150 };
static const char*  S_NAME[8] = { "SENSE",    "DETECT",   "TRACK",
                                   "CLASSIFY", "EVALUATE", "ASSIGN",
                                   "ENGAGE",   "ASSESS" };
static const char*  S_DESC[8] = {
    "Radar sweep covers area",
    "Contact blip confirmed",
    "Track file established",
    "Threat type identified",
    "Priority & TTI computed",
    "Weapon system allocated",
    "Interceptor in flight",
    "Evaluating outcome",
};

static const float PI = 3.14159265358979f;


// DATA TYPES

struct V2 { float x, y; };

struct Threat {
    int   id;
    float x, y;
    int   typeIdx;
    float speed;
    int   priority;   // 0 LOW | 1 MED | 2 HIGH
    int   stage;      // -1 not started, 0-7 active, 8 done
    float stTimer;    // ms remaining in current timed stage
    std::vector<V2> trail;
    int   wpnIdx;     // assigned weapon (-1 = none)
    bool  visible;    // detected?
    bool  dead;       // removed from play
    bool  hit;        // intercepted, running assess timer
    bool  swSeen;     // radar sweep has passed over it
};

struct Interceptor {
    float x, y;
    int   targetId;
    float speed;
    std::vector<V2> trail;
    bool  done;
};

struct Explosion {
    float x, y;
    float elapsed;    // seconds since creation
    float duration;
};

struct Ping {          // detect-pulse ring
    float x, y;
    float elapsed;
};

struct LogEntry {
    std::string ts;
    std::string msg;
    int cls;          // 0 normal | 1 warn | 2 ok
};


// GLOBALS


static std::vector<Threat>      g_threats;
static std::vector<Interceptor> g_intercepts;
static std::vector<Explosion>   g_explosions;
static std::vector<Ping>        g_pings;
static std::deque<LogEntry>     g_logs;

static float g_sweep  = -PI / 2.0f;
static bool  g_paused = false;
static int   g_selId  = -1;
static int   g_nextId = 0;
static int   g_kills  = 0;

static std::mt19937 g_rng;

// UTILITY

static std::string nowStr() {
    time_t t = time(nullptr);
    struct tm* tm_ = localtime(&t);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             tm_->tm_hour, tm_->tm_min, tm_->tm_sec);
    return buf;
}

static void addLog(const std::string& msg, int cls = 0) {
    g_logs.push_front({ nowStr(), msg, cls });
    if (g_logs.size() > 30) g_logs.pop_back();
}

static float frand(float lo = 0.f, float hi = 1.f) {
    return lo + std::uniform_real_distribution<float>(0.f, 1.f)(g_rng) * (hi - lo);
}

static int irand(int n) {        // [0, n)
    return std::uniform_int_distribution<int>(0, n - 1)(g_rng);
}

static Threat* findThreat(int id) {
    for (auto& t : g_threats) if (t.id == id) return &t;
    return nullptr;
}

static std::string stageMsg(const Threat& th) {
    char buf[256];
    const char* id  = nullptr;
    char idbuf[16];
    snprintf(idbuf, sizeof(idbuf), "TGT-%03d", th.id);
    id = idbuf;

    const char* pri = th.priority == 2 ? "HIGH" :
                      th.priority == 1 ? "MEDIUM" : "LOW";

    switch (th.stage) {
      case 0: snprintf(buf,sizeof(buf),"[SENSE] %s — radar sweep, no contact",id); break;
      case 1: snprintf(buf,sizeof(buf),"[DETECT] %s — contact brg %03d° rng %dkm",
                       id, (int)frand(0,360), (int)frand(50,160)); break;
      case 2: snprintf(buf,sizeof(buf),"[TRACK] %s — track file established",id); break;
      case 3: snprintf(buf,sizeof(buf),"[CLASSIFY] %s — %s",id,T_NAME[th.typeIdx]); break;
      case 4: snprintf(buf,sizeof(buf),"[EVALUATE] %s — %s priority, TTI %ds",
                       id, pri, (int)frand(15,40)); break;
      case 5: snprintf(buf,sizeof(buf),"[ASSIGN] %s — %s battery assigned",
                       id, th.wpnIdx>=0 ? WEAPONS[th.wpnIdx].type : "?"); break;
      case 6: snprintf(buf,sizeof(buf),"[ENGAGE] %s — interceptor away",id); break;
      case 7: snprintf(buf,sizeof(buf),"[ASSESS] %s — evaluating BDA",id); break;
      default: return "";
    }
    return buf;
}


// SIMULATION

static void advanceStage(Threat& th) {
    if (th.dead) return;
    th.stage++;

    if (th.stage >= 8) {
        th.dead = true;
        char buf[64]; snprintf(buf,sizeof(buf),"[KILL] TGT-%03d — kill confirmed",th.id);
        addLog(buf, 2);
        g_kills++;
        return;
    }

    th.stTimer = S_DUR[th.stage];   // -1 means event-driven, won't tick

    if (th.stage == 1) {
        th.visible = true;
        g_pings.push_back({ th.x, th.y, 0.f });
    }

    if (th.stage == 5) {
        float best = 1e9f; int wi = 0;
        for (int i = 0; i < 4; i++) {
            float d = hypotf(WEAPONS[i].x - th.x, WEAPONS[i].y - th.y);
            if (d < best) { best = d; wi = i; }
        }
        th.wpnIdx = wi;
    }

    if (th.stage == 6) {
        const WpnDef& w = WEAPONS[th.wpnIdx];
        g_intercepts.push_back({ w.x, w.y, th.id, 2.4f, {}, false });
    }

    addLog(stageMsg(th), th.stage >= 6 ? 2 : 0);
}

static void spawnThreat() {
    float angle  = frand(0.f, 2.f * PI);
    int   ti     = irand(4);
    float rv     = frand();

    Threat th{};
    th.id       = ++g_nextId;
    th.x        = RAD_CX + cosf(angle) * (RAD_R - 14);
    th.y        = RAD_CY + sinf(angle) * (RAD_R - 14);
    th.typeIdx  = ti;
    th.speed    = T_SPEED[ti];
    th.priority = rv < 0.35f ? 2 : rv < 0.65f ? 1 : 0;
    th.stage    = -1;
    th.stTimer  = 0.f;
    th.wpnIdx   = -1;
    th.visible  = false;
    th.dead     = false;
    th.hit      = false;
    th.swSeen   = false;

    g_threats.push_back(th);
    g_selId = th.id;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[ALERT] Inbound — TGT-%03d (%s)", th.id, T_NAME[ti]);
    addLog(buf, 1);

    advanceStage(g_threats.back());   // → stage 0 (SENSE)
}

static void update(float dt) {
    if (g_paused) return;

    g_sweep += dt * 1.0f;   // 1 rad/s → one revolution every ~6.3 s

    //  Stage timers 
    for (auto& th : g_threats) {
        if (th.dead || th.hit) continue;

        // Stage 0: wait for radar sweep to pass over threat's position
        if (th.stage == 0 && !th.swSeen) {
            float ta = atan2f(th.y - RAD_CY, th.x - RAD_CX);
            float d  = fmodf(g_sweep - ta + PI * 4.f, PI * 2.f);
            if (d < 0.13f) { th.swSeen = true; advanceStage(th); }
            continue;
        }

        if (th.stTimer <= 0.f) continue;   // -1 or already expired
        th.stTimer -= dt * 1000.f;
        if (th.stTimer <= 0.f) advanceStage(th);
    }

    //  Move threats 
    for (auto& th : g_threats) {
        if (th.dead || th.hit || !th.visible) continue;
        float dx = RAD_CX - th.x, dy = RAD_CY - th.y;
        float d  = hypotf(dx, dy);
        if (d < 9.f) {
            th.dead = true;
            g_explosions.push_back({ th.x, th.y, 0.f, 0.7f });
            char buf[64];
            snprintf(buf,sizeof(buf),"[BREACH] TGT-%03d — HVA compromised!", th.id);
            addLog(buf, 1);
            continue;
        }
        float spd = th.speed * dt * 60.f;
        th.x += (dx / d) * spd;
        th.y += (dy / d) * spd;
        if (th.stage >= 2) {
            th.trail.push_back({ th.x, th.y });
            if (th.trail.size() > 45) th.trail.erase(th.trail.begin());
        }
    }

    //  Move interceptors 
    for (auto& ic : g_intercepts) {
        if (ic.done) continue;
        Threat* th = findThreat(ic.targetId);
        if (!th || th->dead || th->hit) { ic.done = true; continue; }

        float dx = th->x - ic.x, dy = th->y - ic.y;
        float d  = hypotf(dx, dy);
        if (d < 10.f) {
            ic.done = true;
            g_explosions.push_back({ th->x, th->y, 0.f, 0.7f });
            th->hit     = true;
            th->stage   = 7;
            th->stTimer = 1150.f;
            char buf[64];
            snprintf(buf,sizeof(buf),
                     "[HIT] TGT-%03d — impact confirmed, assessing", th->id);
            addLog(buf, 2);
            continue;
        }
        float spd = ic.speed * dt * 60.f;
        ic.x += (dx / d) * spd;
        ic.y += (dy / d) * spd;
        ic.trail.push_back({ ic.x, ic.y });
        if (ic.trail.size() > 26) ic.trail.erase(ic.trail.begin());
    }

    //  Assess timers (hit threats) 
    for (auto& th : g_threats) {
        if (!th.hit || th.dead) continue;
        th.stTimer -= dt * 1000.f;
        if (th.stTimer <= 0.f) {
            th.dead = true;
            g_kills++;
            char buf[64];
            snprintf(buf,sizeof(buf),"[ASSESS] TGT-%03d — kill confirmed",th.id);
            addLog(buf, 2);
        }
    }

    //  Tick FX 
    for (auto& e : g_explosions) e.elapsed += dt;
    for (auto& p : g_pings)      p.elapsed += dt;

    //  Cleanup 
    auto deadNonSel = [](const Threat& t){ return t.dead && t.id != g_selId; };
    g_threats.erase(
        std::remove_if(g_threats.begin(), g_threats.end(), deadNonSel),
        g_threats.end());
    g_intercepts.erase(
        std::remove_if(g_intercepts.begin(), g_intercepts.end(),
                       [](const Interceptor& i){ return i.done; }),
        g_intercepts.end());
    g_explosions.erase(
        std::remove_if(g_explosions.begin(), g_explosions.end(),
                       [](const Explosion& e){ return e.elapsed >= e.duration; }),
        g_explosions.end());
    g_pings.erase(
        std::remove_if(g_pings.begin(), g_pings.end(),
                       [](const Ping& p){ return p.elapsed >= 0.9f; }),
        g_pings.end());
}


// RENDERING HELPERS


static void setColor(SDL_Renderer* r, Uint8 red, Uint8 g, Uint8 b, Uint8 a = 255) {
    SDL_SetRenderDrawColor(r, red, g, b, a);
}

// Bresenham circle outline
static void drawCircle(SDL_Renderer* r, int cx, int cy, int rad) {
    int x = rad, y = 0, err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(r, cx+x, cy+y); SDL_RenderDrawPoint(r, cx+y, cy+x);
        SDL_RenderDrawPoint(r, cx-y, cy+x); SDL_RenderDrawPoint(r, cx-x, cy+y);
        SDL_RenderDrawPoint(r, cx-x, cy-y); SDL_RenderDrawPoint(r, cx-y, cy-x);
        SDL_RenderDrawPoint(r, cx+y, cy-x); SDL_RenderDrawPoint(r, cx+x, cy-y);
        y++;
        err += 2 * y + 1;
        if (2 * err - 1 > 2 * x) { x--; err -= 2 * x + 1; }
    }
}

// Filled circle (scanlines)
static void fillCircle(SDL_Renderer* r, int cx, int cy, int rad) {
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)sqrtf((float)(rad * rad - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// Radar sweep wedge (fan of alpha-blended lines)
static void drawSweep(SDL_Renderer* r, float angle, float arcWidth) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    int steps = 80;
    for (int i = 0; i <= steps; i++) {
        float a   = angle - arcWidth + arcWidth * i / steps;
        float t   = (float)i / steps;
        Uint8 alp = (Uint8)(t * 30);
        SDL_SetRenderDrawColor(r, 78, 194, 78, alp);
        SDL_RenderDrawLine(r, RAD_CX, RAD_CY,
                           RAD_CX + (int)(cosf(a) * RAD_R),
                           RAD_CY + (int)(sinf(a) * RAD_R));
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    // Bright sweep line (draw 2px wide)
    setColor(r, 78, 200, 78, 220);
    int ex = RAD_CX + (int)(cosf(angle) * RAD_R);
    int ey = RAD_CY + (int)(sinf(angle) * RAD_R);
    SDL_RenderDrawLine(r, RAD_CX,   RAD_CY,   ex,   ey);
    SDL_RenderDrawLine(r, RAD_CX+1, RAD_CY,   ex+1, ey);
}

// Filled + outlined rotated triangle (threat marker)
static void drawThreat(SDL_Renderer* r, float tx, float ty,
                       float faceAngle, bool selected) {
    // Triangle in local space, tip pointing up (-Y)
    const float SZ = 7.5f;
    float lx[3] = { 0.f,       -SZ * 0.65f,  SZ * 0.65f };
    float ly[3] = { -SZ,        SZ * 0.60f,   SZ * 0.60f };

    // Rotate by faceAngle + π/2 so tip points toward centre
    float ang = faceAngle + PI / 2.f;
    float ca = cosf(ang), sa = sinf(ang);
    float wx[3], wy[3];
    for (int i = 0; i < 3; i++) {
        wx[i] = tx + lx[i] * ca - ly[i] * sa;
        wy[i] = ty + lx[i] * sa + ly[i] * ca;
    }

    // Scanline fill
    setColor(r, 239, 68, 68);
    float ymin = std::min({ wy[0], wy[1], wy[2] });
    float ymax = std::max({ wy[0], wy[1], wy[2] });
    for (int py = (int)ymin; py <= (int)ymax; py++) {
        std::vector<float> xs;
        for (int e = 0; e < 3; e++) {
            int ne = (e + 1) % 3;
            float ey0 = wy[e], ey1 = wy[ne];
            if (fabsf(ey1 - ey0) < 0.001f) continue;
            if ((ey0 <= py && ey1 > py) || (ey1 <= py && ey0 > py)) {
                float t = (py - ey0) / (ey1 - ey0);
                xs.push_back(wx[e] + t * (wx[ne] - wx[e]));
            }
        }
        if (xs.size() >= 2) {
            std::sort(xs.begin(), xs.end());
            SDL_RenderDrawLine(r, (int)xs.front(), py, (int)xs.back(), py);
        }
    }

    // Outline
    if (selected) setColor(r, 252, 165, 165);
    else          setColor(r, 127,  29,  29);
    for (int e = 0; e < 3; e++) {
        int ne = (e + 1) % 3;
        SDL_RenderDrawLine(r, (int)wx[e], (int)wy[e], (int)wx[ne], (int)wy[ne]);
    }

    // Selection halo
    if (selected) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        setColor(r, 252, 165, 165, 90);
        drawCircle(r, (int)tx, (int)ty, 15);
        drawCircle(r, (int)tx, (int)ty, 14);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    }
}

// SDL2_ttf text helper
static void drawText(SDL_Renderer* r, TTF_Font* f,
                     const char* txt, int x, int y,
                     Uint8 red, Uint8 g, Uint8 b, Uint8 a = 255,
                     bool centreX = false) {
    SDL_Color c = { red, g, b, a };
    SDL_Surface* s = TTF_RenderText_Blended(f, txt, c);
    if (!s) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!tex) return;
    SDL_SetTextureAlphaMod(tex, a);
    SDL_Rect dst = { x - (centreX ? w / 2 : 0), y, w, h };
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

// RENDER

static void render(SDL_Renderer* r, TTF_Font* fNorm, TTF_Font* fSmall) {
    char buf[256];

    //  Background 
    setColor(r, 3, 8, 3);
    SDL_RenderClear(r);

    
    // LEFT PANEL
    
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    setColor(r, 4, 12, 4, 230);
    SDL_Rect pnl = { 0, 0, PANEL_W, WIN_H };
    SDL_RenderFillRect(r, &pnl);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    setColor(r, 20, 60, 20);
    SDL_RenderDrawLine(r, PANEL_W, 0, PANEL_W, WIN_H);

    const int PX = 10;
    int py = 12;

    // Header
    drawText(r, fNorm,  "AIR DEFENSE",       PX, py,     78, 200,  78);  py += 18;
    drawText(r, fSmall, "TACOPS Kill Chain",  PX, py,     55, 140,  55);  py += 16;

    // Stats
    int active = 0;
    for (auto& t : g_threats) if (!t.dead && !t.hit) active++;
    snprintf(buf, sizeof(buf), "Active: %d    Kills: %d", active, g_kills);
    drawText(r, fSmall, buf,  PX, py,  90, 160,  90);  py += 15;

    const char* liveStr = g_paused ? "[ HOLD ]" : "[ LIVE ]";
    drawText(r, fSmall, liveStr, PX, py,
             g_paused ? 200 :  78,
             g_paused ? 120 : 200,
             g_paused ?  40 :  78);
    py += 16;

    // Divider
    setColor(r, 22, 65, 22);
    SDL_RenderDrawLine(r, PX, py, PANEL_W - 10, py);  py += 10;

    // Selected threat ID
    Threat* sel = findThreat(g_selId);
    snprintf(buf, sizeof(buf), "TGT-%03d", sel ? sel->id : 0);
    drawText(r, fSmall, sel ? buf : "TGT----", PX, py, 78, 194, 78); py += 16;

    //  Stage list 
    for (int i = 0; i < 8; i++) {
        bool isAct  = sel && sel->stage == i;
        bool isDone = sel && sel->stage >  i;

        // Dot indicator
        if (isAct) {
            setColor(r, 78, 200, 78);
            fillCircle(r, PX + 4, py + 6, 4);
        } else if (isDone) {
            setColor(r, 28, 90, 28);
            fillCircle(r, PX + 4, py + 6, 3);
        } else {
            setColor(r, 40, 40, 40);
            fillCircle(r, PX + 4, py + 6, 3);
        }

        // Stage number
        snprintf(buf, sizeof(buf), "0%d", i + 1);
        Uint8 numA = isAct ? 200 : isDone ? 140 : 70;
        drawText(r, fSmall, buf, PX + 12, py, 78, 180, 78, numA);

        // Stage name
        Uint8 nr = 110, ng = 110, nb = 110, na = 70;
        if (isAct)  { nr=78;  ng=220; nb=78;  na=255; }
        if (isDone) { nr=55;  ng=130; nb=55;  na=170; }
        drawText(r, fNorm, S_NAME[i], PX + 28, py, nr, ng, nb, na);

        // Stage description
        drawText(r, fSmall, S_DESC[i], PX + 28, py + 17,
                 50, 80, 50, isAct ? 200 : isDone ? 140 : 55);

        py += 50;
    }

    // Controls hint
    setColor(r, 20, 55, 20);
    SDL_RenderDrawLine(r, PX, WIN_H - 68, PANEL_W - 10, WIN_H - 68);
    drawText(r, fSmall, "SPACE: spawn threat",  PX, WIN_H - 60, 45, 95, 45);
    drawText(r, fSmall, "P: pause   Q: quit",   PX, WIN_H - 45, 45, 95, 45);
    drawText(r, fSmall, "Click radar: select",  PX, WIN_H - 30, 45, 95, 45);

    
    // RADAR DISPLAY
    

    // Dark radar background
    setColor(r, 1, 9, 1);
    fillCircle(r, RAD_CX, RAD_CY, RAD_R);

    // Grid rings
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 1; i <= 4; i++) {
        setColor(r, 25, 105, 25, 40);
        drawCircle(r, RAD_CX, RAD_CY, RAD_R * i / 4);
    }
    // Crosshairs
    setColor(r, 22, 85, 22, 24);
    SDL_RenderDrawLine(r, RAD_CX - RAD_R, RAD_CY, RAD_CX + RAD_R, RAD_CY);
    SDL_RenderDrawLine(r, RAD_CX, RAD_CY - RAD_R, RAD_CX, RAD_CY + RAD_R);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // Range labels
    for (int i = 1; i <= 3; i++) {
        snprintf(buf, sizeof(buf), "%dkm", i * 25);
        drawText(r, fSmall, buf,
                 RAD_CX + 3, RAD_CY - RAD_R * i / 4 + 2, 22, 88, 22, 120);
    }

    // Sweep
    drawSweep(r, g_sweep, 0.52f);

    // Detect pings
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (auto& p : g_pings) {
        float pr   = p.elapsed / 0.9f;
        Uint8 alp  = (Uint8)((1.f - pr) * 180);
        setColor(r, 78, 194, 78, alp);
        drawCircle(r, (int)p.x, (int)p.y, (int)(pr * 32));
        drawCircle(r, (int)p.x, (int)p.y, (int)(pr * 32) + 1);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // Weapon systems
    for (int i = 0; i < 4; i++) {
        const WpnDef& w = WEAPONS[i];
        // Range ring
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        setColor(r, 40, 90, 200, 28);
        drawCircle(r, (int)w.x, (int)w.y, (int)w.range);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        // Square icon
        setColor(r, 10, 24, 72);
        SDL_Rect sq = { (int)w.x - 4, (int)w.y - 4, 8, 8 };
        SDL_RenderFillRect(r, &sq);
        setColor(r, 42, 80, 170);
        SDL_RenderDrawRect(r, &sq);
        // Label
        int lw = (int)strlen(w.type) * 5;
        drawText(r, fSmall, w.type, (int)w.x - lw / 2, (int)w.y + 8,
                 60, 100, 190, 175);
    }

    // Interceptors
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (auto& ic : g_intercepts) {
        if (ic.done) continue;
        for (int i = 1; i < (int)ic.trail.size(); i++) {
            float t = (float)i / ic.trail.size();
            setColor(r, 80, 160, 255, (Uint8)(t * 100));
            SDL_RenderDrawLine(r,
                (int)ic.trail[i-1].x, (int)ic.trail[i-1].y,
                (int)ic.trail[i  ].x, (int)ic.trail[i  ].y);
        }
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    for (auto& ic : g_intercepts) {
        if (ic.done) continue;
        setColor(r, 96, 165, 250);
        fillCircle(r, (int)ic.x, (int)ic.y, 3);
    }

    // Threats
    for (auto& th : g_threats) {
        if (th.dead || th.hit || !th.visible) continue;
        bool isSel = (th.id == g_selId);

        // Trail
        if (th.stage >= 2 && th.trail.size() > 1) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            for (int i = 1; i < (int)th.trail.size(); i++) {
                float t = (float)i / th.trail.size();
                setColor(r, 215, 50, 50, (Uint8)(t * 65));
                SDL_RenderDrawLine(r,
                    (int)th.trail[i-1].x, (int)th.trail[i-1].y,
                    (int)th.trail[i  ].x, (int)th.trail[i  ].y);
            }
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        }

        // Assignment line
        if (th.stage >= 5 && th.wpnIdx >= 0) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            setColor(r, 78, 200, 78, 70);
            SDL_RenderDrawLine(r,
                (int)WEAPONS[th.wpnIdx].x, (int)WEAPONS[th.wpnIdx].y,
                (int)th.x, (int)th.y);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        }

        // Triangle marker
        float ang = atan2f(RAD_CY - th.y, RAD_CX - th.x);
        drawThreat(r, th.x, th.y, ang, isSel);

        // ID label
        snprintf(buf, sizeof(buf), "TGT-%03d", th.id);
        int lx = (int)th.x + 12, labelY = (int)th.y - 3;
        if (isSel) drawText(r, fSmall, buf, lx, labelY, 252, 165, 165);
        else       drawText(r, fSmall, buf, lx, labelY, 200,  70,  70, 210);

        if (th.stage >= 3) {
            std::string tn = T_NAME[th.typeIdx];
            size_t sp = tn.find(' ');
            if (sp != std::string::npos) tn = tn.substr(0, sp);
            for (auto& c : tn) c = (char)toupper(c);
            drawText(r, fSmall, tn.c_str(), lx, labelY + 13, 252, 204, 20, 210);
        }
        if (th.stage >= 4) {
            const char* ps = th.priority == 2 ? "HIGH" :
                             th.priority == 1 ? "MED"  : "LOW";
            Uint8 pr = th.priority==2?239:th.priority==1?251:250;
            Uint8 pg = th.priority==2? 68:th.priority==1?146:204;
            Uint8 pb = th.priority==2? 68:th.priority==1? 60: 21;
            drawText(r, fSmall, ps, lx, labelY + 26, pr, pg, pb, 215);
        }
    }

    // Explosions
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (auto& e : g_explosions) {
        float p  = e.elapsed / e.duration;
        if (p > 1.f) continue;
        setColor(r, 255, 210, 50, (Uint8)((1.f - p) * 220));
        drawCircle(r, (int)e.x, (int)e.y, (int)(p * 28));
        drawCircle(r, (int)e.x, (int)e.y, (int)(p * 28) + 1);
        setColor(r, 255, 100, 50, (Uint8)((1.f - p) * 130));
        drawCircle(r, (int)e.x, (int)e.y, (int)(p * 15));
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // HVA — high-value asset at centre
    {
        float tp = fmodf((float)SDL_GetTicks() / 2000.f, 1.f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        setColor(r, 78, 194, 78, (Uint8)(150 * (1.f - tp)));
        drawCircle(r, RAD_CX, RAD_CY, (int)(5 + tp * 20));
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        setColor(r, 78, 200, 78);
        fillCircle(r, RAD_CX, RAD_CY, 5);
        drawText(r, fSmall, "HVA", RAD_CX - 11, RAD_CY + 9, 78, 200, 78, 170);
    }

    // Radar border
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    setColor(r, 40, 130, 40, 110);
    drawCircle(r, RAD_CX, RAD_CY, RAD_R);
    drawCircle(r, RAD_CX, RAD_CY, RAD_R - 1);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // Compass letters
    drawText(r, fSmall, "N", RAD_CX - 4, RAD_CY - RAD_R - 16, 78, 194, 78, 110);
    drawText(r, fSmall, "S", RAD_CX - 4, RAD_CY + RAD_R +  4, 78, 194, 78, 110);
    drawText(r, fSmall, "W", RAD_CX - RAD_R - 16, RAD_CY - 6, 78, 194, 78, 110);
    drawText(r, fSmall, "E", RAD_CX + RAD_R +  4, RAD_CY - 6, 78, 194, 78, 110);

    // BRG readout
    float deg = fmodf((g_sweep + PI / 2.f) / (PI * 2.f) * 360.f + 360.f, 360.f);
    snprintf(buf, sizeof(buf), "BRG %03d", (int)deg);
    drawText(r, fSmall, buf,
             RAD_CX - RAD_R + 6, RAD_CY - RAD_R + 8, 78, 194, 78, 95);
    drawText(r, fSmall, g_paused ? "HOLD" : "LIVE",
             RAD_CX - RAD_R + 6, RAD_CY - RAD_R + 22, 78, 194, 78, 95);

    
    // EVENT LOG (bottom strip, full width)
    
    setColor(r, 5, 14, 5);
    SDL_Rect logRect = { 0, LOG_Y, WIN_W, WIN_H - LOG_Y };
    SDL_RenderFillRect(r, &logRect);
    setColor(r, 18, 55, 18);
    SDL_RenderDrawLine(r, 0, LOG_Y, WIN_W, LOG_Y);

    drawText(r, fSmall, "COMBAT LOG", 10, LOG_Y + 4, 55, 130, 55, 170);

    int logEntryY = LOG_Y + 20;
    for (int i = 0; i < (int)g_logs.size() && logEntryY + 14 < WIN_H; i++) {
        const LogEntry& lg = g_logs[i];
        snprintf(buf, sizeof(buf), "%s  %s", lg.ts.c_str(), lg.msg.c_str());
        Uint8 mr = 110, mg = 110, mb = 110;
        if (lg.cls == 1) { mr = 248; mg = 113; mb = 113; }
        if (lg.cls == 2) { mr =  78; mg = 210; mb = 110; }
        drawText(r, fSmall, buf, 10, logEntryY, mr, mg, mb, 200);
        logEntryY += 15;
    }

    SDL_RenderPresent(r);
}


// MAIN


int main(int /*argc*/, char* /*argv*/[]) {
    g_rng.seed((unsigned)time(nullptr));

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit(); return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "Air Defense — 8-Stage TACOPS Simulation",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    //  Font discovery 
    static const char* FONT_PATHS[] = {
        // Linux
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        // macOS
        "/System/Library/Fonts/Menlo.ttc",
        "/Library/Fonts/Courier New.ttf",
        // Windows (compile with MinGW or WSL)
        "C:\\Windows\\Fonts\\cour.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
        nullptr
    };

    TTF_Font* fNorm  = nullptr;
    TTF_Font* fSmall = nullptr;

    for (int i = 0; FONT_PATHS[i]; i++) {
        fNorm = TTF_OpenFont(FONT_PATHS[i], 13);
        if (fNorm) {
            fSmall = TTF_OpenFont(FONT_PATHS[i], 10);
            printf("Font: %s\n", FONT_PATHS[i]);
            break;
        }
    }
    if (!fNorm) {
        fprintf(stderr,
                "Could not open a monospace font. "
                "Install DejaVu fonts:\n"
                "  sudo apt install fonts-dejavu-core\n"
                "TTF error: %s\n", TTF_GetError());
        return 1;
    }

    //  Initial log messages 
    addLog("[SYSTEM] Air Defense TACOPS online");
    addLog("[SYSTEM] Radar sweep active — SPACE to spawn threat");

    Uint32 lastTick = SDL_GetTicks();
    bool   running  = true;

    while (running) {
        //  Events 
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
              case SDL_QUIT:
                running = false;
                break;

              case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                  case SDLK_q:
                  case SDLK_ESCAPE:
                    running = false;
                    break;
                  case SDLK_SPACE:
                    spawnThreat();
                    break;
                  case SDLK_p:
                    g_paused = !g_paused;
                    addLog(g_paused ? "[SYSTEM] Simulation paused"
                                    : "[SYSTEM] Simulation resumed");
                    break;
                }
                break;

              case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int mx = ev.button.x, my = ev.button.y;
                    float best = 22.f; int bestId = -1;
                    for (auto& th : g_threats) {
                        if (th.dead || !th.visible) continue;
                        float d = hypotf(th.x - mx, th.y - my);
                        if (d < best) { best = d; bestId = th.id; }
                    }
                    if (bestId >= 0) g_selId = bestId;
                }
                break;
            }
        }

        //  Update 
        Uint32 now = SDL_GetTicks();
        float  dt  = std::min((now - lastTick) / 1000.f, 0.05f);
        lastTick   = now;
        update(dt);

        //  Render 
        render(ren, fNorm, fSmall);
    }

    TTF_CloseFont(fSmall);
    TTF_CloseFont(fNorm);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
