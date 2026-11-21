/* The page's frame-rate overlay reads the game's own counters from here
 * (HOST_KIND_STATUS, layout in status_proto.h).  A small thread refreshes
 * the renderer's counters four times a second; other parts of the web
 * runtime may set their own with host_status_set().  With the null renderer
 * "frames" still counts the game's presents, so the overlay shows the game's
 * frame rate rather than the page's display rate. */
#include "cpu.h"
#include "status_proto.h"
#include "host_abi.h"
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

long metal_frames(void); long metal_tris(void); long metal_tex_created(void);
unsigned long metal_tex_released(void); long metal_frames_dropped(void);

static HostStatus g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int host_status_slot(const char *name){
    pthread_mutex_lock(&g_mu);
    int i = 0;
    for (; i < (int)g_st.count; i++) if (!strncmp(g_st.item[i].name, name, HOST_STATUS_NAME)) break;
    if (i == (int)g_st.count){
        if (i >= HOST_STATUS_MAX){ pthread_mutex_unlock(&g_mu); return -1; }
        __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
        strncpy(g_st.item[i].name, name, HOST_STATUS_NAME);
        g_st.item[i].value = 0;
        __atomic_store_n(&g_st.count, (uint32_t)i + 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_mu);
    return i;
}

void host_status_set(int slot, double value){ host_status_set_n(&slot, &value, 1); }

/* several counters in one sequence-lock section, so the page never sees one
 * of them newer than another (frames and frames_t below) */
void host_status_set_n(const int *slot, const double *value, int n){
    pthread_mutex_lock(&g_mu);
    __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < n; i++) if (slot[i] >= 0 && slot[i] < HOST_STATUS_MAX) g_st.item[slot[i]].value = value[i];
    __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
}

/* "frames_t" is the time "frames" was read, in ms on CLOCK_MONOTONIC (the
 * page's performance.timeOrigin + now(), the same clock).  The page divides
 * the change in frames by the change in frames_t: its own sampling times
 * (animation frames, about every 250 ms) do not line up with this thread's
 * updates, and dividing by them misread a steady 60 fps as 56 or 75 (phase-1
 * app review, finding 1). */
static void *status_main(void *unused){
    (void)unused;
    int slot[5] = { host_status_slot("frames"), host_status_slot("frames_t"), host_status_slot("frames_dropped"),
                    host_status_slot("tris"), host_status_slot("textures") };
    for (;;){
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double v[5] = { (double)metal_frames(), ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6, (double)metal_frames_dropped(),
                        (double)metal_tris(), (double)metal_tex_created() - (double)metal_tex_released() };
        host_status_set_n(slot, v, 5);
        usleep(g_st.period_ms * 1000u);
    }
    return 0;
}

/* "drive": what the page's touch controls (web/src/app/touch.ts) need of the
 * game, set at every Present (autotest.c): 1 while the player drives in a
 * race, 2 in free roam, 0 elsewhere, plus 4 with a manual gearbox and 8
 * during a cutscene (the touch controls' level horizon waits).  Driving
 * is GameFlow 6 with a race HUD (HUD_*.fng) the top package; the pause menu
 * (UI_pause.fng), the results, the world's loading screen and the Engage
 * dialog over the HUD are not, nor is a cutscene in free roam (NIS, as
 * runtime/discord.c tells one).  A race's start is one too, the camera's
 * flight round the grid and the countdown until a moment after the start, so
 * a race drives during a cutscene: the player revs on the line and goes at
 * once (a player's report: the controls came seconds after the others left).
 * The gearbox is manual in a drag race (the game asks nothing there and sets
 * the car's gearbox to manual), else as the race's Transmission choice set
 * it: the player's setting 1 (0x4a7b30), kept at player 0 (0x8900ac) +0x20
 * and handed to the car's gearbox (0x407280: 0 and 2 shift by themselves, 1
 * and 3 keep the gear).  The gearbox itself is no good to read: in a drag
 * race the car's controls (+0x30) become the game's lane steering once it
 * rolls.  This is NFSU2-only: FlatOut has different game-state structures. */
#ifndef FLATOUT2
#define G_PLAYER0    0x008900acu
#define PL_GEARBOX   0x20u
#define G_RACE_DRAG  0x0089e7d8u      /* race parameters (discord.c G_RACE) + R_DRAG */
const char *autotest_top_fng(void);
int career_fe_active(const char *name);
int roam_exploring(void);
static int manual_gearbox(int race){
    if (race && MR8(G_RACE_DRAG)) return 1;
    uint32_t p = mem_is_committed(G_PLAYER0 + 3u) ? MR32(G_PLAYER0) : 0u;
    if (!p || !mem_is_committed(p + PL_GEARBOX)) return 0;
    uint8_t g = MR8(p + PL_GEARBOX);
    return g == 1u || g == 3u;
}
static int drive_state(void){
    unsigned flow = mem_is_committed(0x8654a7u) ? MR32(0x8654a4u) : 0u;
    const char *top = autotest_top_fng();
    int hud = top && !strncasecmp(top, "HUD_", 4) && strcasecmp(top, "HUD_World_Loading.fng");
    int nis = mem_is_committed(0x827ecbu) && (MR32(0x827e98u) | MR32(0x827eb0u) | MR32(0x827ec8u));
    int engage = career_fe_active("UI_EngageMessageDialog.fng");
    int roam = roam_exploring();
    int v = flow == 6u && hud && !engage && !(roam && nis) ? (roam ? 2 : 1) : 0;
    if (v && manual_gearbox(v == 1)) v |= 4;
    if (v && nis) v |= 8;
    /* a line at each change, for a player's log */
    static unsigned l_flow = ~0u; static int l_hud = -1, l_nis = -1, l_engage = -1, l_v = -1;
    if (flow != l_flow || hud != l_hud || nis != l_nis || engage != l_engage || v != l_v){
        l_flow = flow; l_hud = hud; l_nis = nis; l_engage = engage; l_v = v;
        fprintf(stderr, "[drive] f=%ld flow=%u top=%s nis=%d engage=%d -> %d\n", metal_frames(), flow, top ? top : "-", nis, engage, v);
    }
    return v;
}
#else
/* FlatOut 2 has its own classifier below.  Do not read any inherited NFSU2
 * flow, HUD, package, or gearbox fields here: they can only produce
 * misleading state.  FO2_TOUCH_PROBE=1 records FlatOut's DirectInput,
 * Direct3D and menu-toggle signatures used to establish this classifier. */
#define FO2_DATA_LO 0x00681000u
#define FO2_DATA_HI 0x00903be4u
typedef struct { uint32_t addr, off, on; } Fo2Toggle;
static uint8_t *g_fo2_touch_snap;
static Fo2Toggle *g_fo2_touch_toggle;
static unsigned g_fo2_touch_ntoggle, g_fo2_touch_event, g_fo2_touch_wait;
static unsigned g_fo2_menu_wait;
static int g_fo2_touch_race_render, g_fo2_race_menu;
static long g_fo2_player_frame = -1000000;

static int fo2_touch_probe_on(void){
    static int on = -1;
    if (on < 0){
        const char *e = getenv("FO2_TOUCH_PROBE");
        on = e && *e && *e != '0';
    }
    return on;
}

/* com.c supplies the completed frame's actual FlatOut draw callers before
 * Present.  0x55ad588 is absent throughout the frontend/loading sequence and
 * appears with the full world render at race entry; the paused race continues
 * to render that world behind its menu.  This gate prevents an Escape used in
 * a frontend screen from becoming the toggle probe's baseline. */
void fo2_touch_render_frame(uint32_t draws, const uint32_t *site, unsigned nsite){
    int world = 0;
    for (unsigned i = 0; i < nsite; i++)
        if (site[i] == 0x005ad588u){ world = 1; break; }
    /* The extra world draw caller establishes race entry before the local
     * player callback starts. It is never used to end a race: stationary and
     * alternate-camera frames can legitimately omit it. */
    if (!g_fo2_touch_race_render)
        g_fo2_touch_race_render = world && draws > 500u;
    if (!g_fo2_touch_race_render){ g_fo2_race_menu = 0; g_fo2_menu_wait = 0; }
}

/* Called by the wrapped FlatOut local-player control writer.  Unlike draw
 * volume this continues every frame while the car is stationary. */
void fo2_touch_player_frame(void){
    g_fo2_player_frame = metal_frames();
    g_fo2_touch_race_render = 1;
}

/* Called from FlatOut's real WH_KEYBOARD delivery path.  Escape is merely the
 * trigger for a before/after snapshot; the resulting classifier will use the
 * game-owned state field that survives repeated open/close reversals, not the
 * key itself. */
void fo2_touch_key_event(uint32_t vk, uint32_t flags){
    if (flags & 0x80000000u) return;
    /* 0x8dbe34 also changes for look-back, so it is not safe to sample every
     * frame.  Read it only after FlatOut has consumed a race-menu action:
     * Escape opens/closes it; Enter can activate Continue or enter a submenu. */
    if (g_fo2_touch_race_render && (vk == 0x1bu || (g_fo2_race_menu && vk == 0x0du)))
        g_fo2_menu_wait = 2u;
    if (!fo2_touch_probe_on() || !g_fo2_touch_race_render || vk != 0x1bu) return;
    size_t n = FO2_DATA_HI - FO2_DATA_LO;
    if (!g_fo2_touch_snap) g_fo2_touch_snap = malloc(n);
    if (!g_fo2_touch_toggle) g_fo2_touch_toggle = calloc(4096u, sizeof *g_fo2_touch_toggle);
    if (!g_fo2_touch_snap || !g_fo2_touch_toggle) return;
    memcpy(g_fo2_touch_snap, g_mem + FO2_DATA_LO, n);
    g_fo2_touch_event++;
    g_fo2_touch_wait = 2u;
    fprintf(stderr, "[fo2-touch-key] f=%ld event=%u vk=%02x flags=%08x\n",
            metal_frames(), g_fo2_touch_event, vk, flags);
}

static void fo2_touch_toggle_frame(void){
    if (!g_fo2_touch_wait || !g_fo2_touch_snap || !g_fo2_touch_toggle) return;
    if (--g_fo2_touch_wait) return;
    if (g_fo2_touch_event == 1u){
        g_fo2_touch_ntoggle = 0;
        for (uint32_t a = FO2_DATA_LO; a + 3u < FO2_DATA_HI; a += 4u){
            uint32_t oldv, newv = MR32(a);
            memcpy(&oldv, g_fo2_touch_snap + a - FO2_DATA_LO, sizeof oldv);
            /* Pause/menu state is expected to be a small scalar or enum.
             * Filtering out pointers, floats and clocks keeps this bounded. */
            if (oldv == newv || oldv > 32u || newv > 32u) continue;
            if (g_fo2_touch_ntoggle < 4096u)
                g_fo2_touch_toggle[g_fo2_touch_ntoggle++] =
                    (Fo2Toggle){ a, oldv, newv };
        }
    } else {
        unsigned out = 0;
        int opening = (g_fo2_touch_event & 1u) != 0;
        for (unsigned i = 0; i < g_fo2_touch_ntoggle; i++){
            Fo2Toggle t = g_fo2_touch_toggle[i];
            uint32_t before;
            memcpy(&before, g_fo2_touch_snap + t.addr - FO2_DATA_LO, sizeof before);
            uint32_t after = MR32(t.addr);
            uint32_t want_before = opening ? t.off : t.on;
            uint32_t want_after  = opening ? t.on : t.off;
            if (before == want_before && after == want_after)
                g_fo2_touch_toggle[out++] = t;
        }
        g_fo2_touch_ntoggle = out;
    }
    fprintf(stderr, "[fo2-touch-toggle] f=%ld event=%u candidates=%u",
            metal_frames(), g_fo2_touch_event, g_fo2_touch_ntoggle);
    unsigned shown = g_fo2_touch_ntoggle < 40u ? g_fo2_touch_ntoggle : 40u;
    for (unsigned i = 0; i < shown; i++)
        fprintf(stderr, " %08x:%u/%u", g_fo2_touch_toggle[i].addr,
                g_fo2_touch_toggle[i].off, g_fo2_touch_toggle[i].on);
    fputc('\n', stderr);
}

static int drive_state(void){
    fo2_touch_toggle_frame();
    if (g_fo2_touch_race_render && metal_frames() - g_fo2_player_frame > 5){
        g_fo2_touch_race_render = 0;
        g_fo2_race_menu = 0;
        g_fo2_menu_wait = 0;
    }
    if (g_fo2_menu_wait && !--g_fo2_menu_wait)
        g_fo2_race_menu = mem_is_committed(0x008dbe37u) && MR32(0x008dbe34u) != 0u;
    /* The menu condition also changes during look-back.  g_fo2_race_menu is
     * therefore latched only after Escape/Enter above, rather than following
     * that condition continuously. */
    int paused = g_fo2_race_menu;
    int v = g_fo2_touch_race_render && !g_fo2_race_menu ? 1 : 0;
    static int last_race = -1, last_pause = -1, last_v = -1;
    if (g_fo2_touch_race_render != last_race || paused != last_pause || v != last_v){
        last_race = g_fo2_touch_race_render; last_pause = paused; last_v = v;
        fprintf(stderr, "[fo2-drive] f=%ld world=%d menu=%d -> %d\n",
                metal_frames(), g_fo2_touch_race_render, paused, v);
    }
    return v;
}
#endif
#ifndef FLATOUT2
/* "rate": the frame rate the game paces to now (framerate.c content_rate:
 * the mode's rate, 30 in a cutscene, at most 120 in a network race; 0 =
 * uncapped), so the page can tell whether the game keeps up with it
 * (web/src/app/rendercheck.ts) */
int hfr_rate_now(void);
/* "pause": 1 while the game's pause menu (UI_Pause.fng) is the top package,
 * not its options or a question over it, so the page can point to its own
 * menu under the panel (web/src/app/pausehint.ts) */
static int pause_state(void){
    const char *top = autotest_top_fng();
    return top && !strcasecmp(top, "UI_Pause.fng");
}
/* "roam_cars", "roam_friends": friends' cars the game holds and friends
 * connected in free roam (roam_game.c), for the page's copy of the log
 * (web/src/app/lastgame.ts): what a phone held when Safari stopped the page.
 * The friends take the session lock, so every 30th frame. */
int roam_game_cars(void); int roam_game_friends(void);
static void roam_state(void){
    static int slot[2] = { -2, -2 }, n;
    static double last[2] = { -1, -1 };
    if (slot[0] == -2){ slot[0] = host_status_slot("roam_cars"); slot[1] = host_status_slot("roam_friends"); }
    double v[2] = { (double)roam_game_cars(), n++ % 30 ? last[1] : (double)roam_game_friends() };
    if (v[0] != last[0] || v[1] != last[1]){ last[0] = v[0]; last[1] = v[1]; host_status_set_n(slot, v, 2); }
}
#endif
void status_web_frame(void){
#ifdef FLATOUT2
    static int slot = -2, last = -1;
    if (slot == -2) slot = host_status_slot("drive");
    int v = drive_state();
    if (v != last && slot >= 0){ last = v; host_status_set(slot, v); }
#else
    static int slot = -2, last = -1, rslot = -2, rlast = -1, pslot = -2, plast = -1;
    if (slot == -2){ slot = host_status_slot("drive"); rslot = host_status_slot("rate"); pslot = host_status_slot("pause"); }
    int v = drive_state();
    if (v != last && slot >= 0){ last = v; host_status_set(slot, v); }
    int r = hfr_rate_now();
    if (r != rlast && rslot >= 0){ rlast = r; host_status_set(rslot, r); }
    int p = pause_state();
    if (p != plast && pslot >= 0){ plast = p; host_status_set(pslot, p); }
    roam_state();
#endif
}

/* main_web.c, before the guest starts */
void status_web_start(void){
    g_st.magic = HOST_STATUS_MAGIC;
    g_st.period_ms = 250;
    nfsu2_register(HOST_KIND_STATUS, &g_st, sizeof g_st);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u * 1024u);
    pthread_t th;
    if (pthread_create(&th, &at, status_main, 0) == 0) pthread_detach(th);
}
