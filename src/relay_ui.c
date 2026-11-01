/* The 2.0 window: tray, telemetry and settings. Included by hotas_relay.c
   right before main(), so it can read the relay's globals directly.

   Two threads on purpose. The shell thread owns the tray and lives the whole
   run with no graphics loaded. The window thread only exists while the window
   is open, and it releases everything it made when it closes, so closed to the
   tray means one wake a second and no drawing at all. Neither one ever takes
   g_output_lock, and the input loop never waits on either of them. */
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>

#define UI_W 760.f
#define UI_H 488.f
#define UI_CAP 32.f

/* ---------- Which face the relay is showing ---------- */

static int g_ui_windowed = 0;    /* 0 = console only, exactly as 1.x */
static int g_ui_start_tray = 0;  /* --tray, or start_tray=on */
static int g_ui_own_console = 0; /* a -- flag got its own console, so hold it open at the end */

static void ui_reopen_streams(int console) {
    freopen(console ? "CONOUT$" : "NUL", "w", stdout);
    freopen(console ? "CONOUT$" : "NUL", "w", stderr);
    freopen(console ? "CONIN$" : "NUL", "r", stdin);
}

/* Same console the relay had in 1.x, with two things 1.x didn't need: its X
   would quit the relay while the window is the host, and a stray click in
   QuickEdit would pause output and with it the input thread. */
static void ui_console_open(void) {
    if (!AllocConsole()) return;
    ui_reopen_streams(1);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE); /* AllocConsole resets the handler table */
    SetConsoleTitleA("Ruthless Controller Relay");
    HWND con = GetConsoleWindow();
    if (con && g_ui_windowed) DeleteMenu(GetSystemMenu(con, FALSE), SC_CLOSE, MF_BYCOMMAND);
    DWORD mode = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(in, &mode)) SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    g_console_done = 0; /* console_init_once sets the new one up on its next use */
}

static void ui_console_show(void) {
    if (g_console_on) return;
    ui_console_open();
    InterlockedExchange(&g_console_fresh, 1);
    InterlockedExchange(&g_console_on, 1);
}

/* A write to a console that's just gone only fails, so no handshake is needed
   with the input thread, just a moment for a redraw in progress to finish. A
   console menu is mid-flow on the input thread, so it has to finish first. */
static void ui_console_hide(void) {
    if (!g_console_on || g_relay_state == RELAY_STATE_MENU) return;
    InterlockedExchange(&g_console_on, 0);
    Sleep(150);
    ui_reopen_streams(0);
    FreeConsole();
}

static void ui_console_force(void) {
    if (g_console_on) return;
    ui_console_open();
    InterlockedExchange(&g_console_on, 1);
}

static HWND ui_owner_hwnd(void) {
    HWND con = GetConsoleWindow();
    return con ? con : g_ui_shell_hwnd;
}

static void ui_pause_at_exit(void) {
    printf("\nPress Enter to close this window...");
    fflush(stdout);
    getchar();
}

/* Sorts out the arguments before anything prints. Everything that took a
   console in 1.x still gets one; a plain launch gets the window. */
static void ui_early(int *argc, char **argv) {
    int console_mode = 0;
    char path[MAX_PATH + 32];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "console=on", 10)) console_mode = 1;
            else if (!strncmp(line, "start_tray=on", 13)) g_ui_start_tray = 1;
        }
        fclose(f);
    }
    if (*argc > 1) {
        const char *a = argv[1];
        int ours = 1;
        if (!strcmp(a, "--console")) console_mode = 1;
        else if (!strcmp(a, "--window")) console_mode = 0;
        else if (!strcmp(a, "--tray")) g_ui_start_tray = 1;
        else ours = 0;
        if (ours) {
            for (int i = 1; i < *argc - 1; i++) argv[i] = argv[i + 1];
            (*argc)--;
        }
    }
    if (*argc > 1 && !strncmp(argv[1], "--", 2)) {
        const char *a = argv[1];
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        int redirected = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
        int internal = !strcmp(a, "--test-config") || !strcmp(a, "--create-root-device") || !strcmp(a, "--configure-vjoy");
        if (!redirected && !internal) {
            ui_console_open();
            g_ui_own_console = 1;
            atexit(ui_pause_at_exit);
        } else if (!redirected) {
            ui_reopen_streams(0);
        }
        return;
    }
    if (console_mode) {
        ui_console_open();
        return;
    }
    g_ui_windowed = 1;
    InterlockedExchange(&g_console_on, 0);
    ui_reopen_streams(0);
}

/* ---------- Status: state in, words out. No globals, so --test-config can check it ---------- */

typedef struct {
    int relay;              /* RELAY_STATE_* */
    int ever, dropped_s, pad_kept, connecting, vjoy_busy, stalled;
    int mode;               /* -1 unset, 0 Normal, 1 HOTAS */
    int fl_on, head_mode, fl_state, head_st, no_face, cam_on, alt_held;
    int head_files, has_camera, has_game, game_up, cam_failed;
    int backend, is_bt, vigem_on, vigem_ds4, vigem_ok, hidden;
    int batt_src, batt_pct, batt_chg, batt_coarse;
    long fl_speed, head_speed;
    int hotkey_failed;
} ui_state_t;

enum { UI_QUIET = 0, UI_WARN_FILL, UI_WARN_LINE, UI_CRIT_FILL, UI_CRIT_LINE, UI_LOCK_FILL };
enum { UI_W_NORMAL = 0, UI_W_DIM, UI_W_OK, UI_W_WARN };
enum { UI_R_FLAG = 0, UI_R_HEAD, UI_R_STICK, UI_R_FROZEN };

typedef struct {
    wchar_t m_label[32], m_word[16], m_sub[48];
    int m_class, m_btn; /* m_btn: 0 none, 1 to Normal, 2 to HOTAS, 3 both */
    UINT32 bar;         /* 0 = no controller */
    wchar_t l_word[16], l_sub[64];
    int l_class, l_color, chip_cam, chip_alt;
    wchar_t k_word[24], k_sub[96];
    int k_class, take;
    wchar_t batt[16];
    int batt_kind, batt_pct, batt_chg, batt_level; /* kind: 0 none, 1 glyph, 2 wired, 3 unknown. level: 0 fine, 1 mid, 2 low */
    int flagged;        /* the sticks can't be trusted, so the rings say so */
    wchar_t flag[12];
    int r_follow, r_dash, r_dim, r_hollow, r_color; /* look ring. r_color: 0 accent, 1 ok, 2 lock */
    wchar_t r_title[12], r_flag[16];
    int ready;
    wchar_t summary[64]; /* "HOTAS, Tracking, DualSense 72%" for the title and the tray */
} ui_words_t;

static const wchar_t *ui_pad_short(int backend, int is_bt, int wired) {
    switch (backend) {
        case BACKEND_XINPUT: return wired ? L"Xbox pad, wired" : L"Xbox pad, wireless";
        case BACKEND_DS4_RAWHID: return L"DualShock 4, Bluetooth";
        case BACKEND_DUALSENSE_RAWHID: return L"DualSense, Bluetooth";
        case BACKEND_DS4_WIRED_RAWHID: return L"DualShock 4, USB";
        case BACKEND_DUALSENSE_WIRED_RAWHID: return L"DualSense, USB";
        default: return is_bt ? L"Controller, Bluetooth" : L"Controller, DirectInput";
    }
}

static void ui_status(const ui_state_t *s, ui_words_t *w) {
    memset(w, 0, sizeof(*w));
    int live = s->relay == RELAY_STATE_CONNECTED;
    w->ready = live && !s->stalled;
    const wchar_t *r1 = s->backend == BACKEND_XINPUT ? L"RB" : L"R1";
    int ps_pad = s->backend != BACKEND_XINPUT;

    /* Mode */
    wcscpy(w->m_label, L"Mode");
    if (s->mode < 0) {
        wcscpy(w->m_label, L"Mode: game sees both");
        wcscpy(w->m_word, L"Unset");
        w->m_class = UI_WARN_FILL;
        w->m_btn = 3;
    } else if (s->mode == 1) {
        wcscpy(w->m_word, L"HOTAS");
        wcscpy(w->m_sub, L"Game sees a stick");
        w->m_btn = 1;
    } else {
        wcscpy(w->m_word, L"Normal");
        if (!s->vigem_ok) {
            wcscpy(w->m_sub, L"No virtual pad: ViGEmBus missing");
            w->m_class = UI_WARN_LINE;
        } else {
            wcscpy(w->m_sub, s->vigem_ds4 ? L"Game sees a PlayStation pad" : L"Game sees an Xbox pad");
        }
        w->m_btn = 2;
    }
    if (s->hotkey_failed) {
        wcscpy(w->m_sub, L"Hotkey didn't register");
        if (!w->m_class) w->m_class = UI_WARN_LINE;
    }
    int low = s->batt_src == BATTERY_SRC_PERCENT && s->batt_pct <= 15 && !s->batt_chg;
    if (s->relay == RELAY_STATE_CONNECTED || s->relay == RELAY_STATE_MENU) {
        int native = ps_pad ? s->vigem_ds4 : !s->vigem_ds4;
        w->bar = low ? 0xFF0000 : s->mode < 0 ? 0xFF3C00 : s->mode == 1 ? 0x00FF00 : native ? 0x0064FF : 0xA000FF;
    }

    /* Look */
    if (s->mode == 0) {
        wcscpy(w->l_word, L"Off");
        wcscpy(w->l_sub, L"HOTAS mode only");
        w->l_color = UI_W_DIM;
    } else if (!s->fl_on) {
        wcscpy(w->l_word, L"Off");
        wcscpy(w->l_sub, L"Free look off");
        w->l_color = UI_W_DIM;
    } else if (s->fl_state == FREELOOK_LOCKED) {
        wcscpy(w->l_word, L"Locked");
        swprintf(w->l_sub, 64, L"Tap %ls or R3 to unlock", r1);
        w->l_class = UI_LOCK_FILL;
    } else if (!s->head_mode) {
        if (s->fl_state == FREELOOK_LOOKING) {
            wcscpy(w->l_word, L"Looking");
            wcscpy(w->l_sub, L"Let go = snap back. R3 = lock");
        } else if (s->fl_state == FREELOOK_LOOKING_LOCK_ARMED) {
            wcscpy(w->l_word, L"Lock set");
            wcscpy(w->l_sub, L"Let go, camera stays");
        } else {
            swprintf(w->l_word, 16, L"Hold %ls", r1);
            swprintf(w->l_sub, 64, L"Free look, speed %ld", s->fl_speed);
        }
    } else if (s->fl_state != FREELOOK_OFF) {
        wcscpy(w->l_word, L"Stick");
        wcscpy(w->l_sub, L"Stick has the camera");
    } else if (s->head_st == HEAD_ST_TRACKING) {
        wcscpy(w->l_word, L"Tracking");
        swprintf(w->l_sub, 64, L"Tap %ls to recenter. Speed %ld", r1, s->head_speed);
        w->l_color = UI_W_OK;
    } else if (s->head_st == HEAD_ST_PAUSED) {
        wcscpy(w->l_word, L"Paused");
        wcscpy(w->l_sub, L"Game doesn't have the mouse");
    } else if (s->no_face) {
        wcscpy(w->l_word, L"No face");
        wcscpy(w->l_sub, L"Camera on, can't see you");
        w->l_color = UI_W_WARN;
    } else {
        wcscpy(w->l_word, L"Waiting");
        w->l_color = UI_W_WARN;
        if (!s->head_files) wcscpy(w->l_sub, L"Files missing");
        else if (!s->has_camera) wcscpy(w->l_sub, L"No camera set");
        else if (!s->has_game) wcscpy(w->l_sub, L"No game set");
        else if (s->cam_failed) wcscpy(w->l_sub, L"Camera failed, retrying");
        else if (!s->game_up) { wcscpy(w->l_sub, L"Game not running"); w->l_color = UI_W_NORMAL; }
        else { wcscpy(w->l_sub, L"Camera starting"); w->l_color = UI_W_NORMAL; }
    }
    w->chip_cam = s->cam_on;
    w->chip_alt = s->alt_held;

    /* Link, and the takeover when Look and Link stop mattering */
    int wired_xbox = s->backend == BACKEND_XINPUT && s->batt_src == BATTERY_SRC_WIRED;
    if (s->relay == RELAY_STATE_STARTING) {
        w->take = 1;
        wcscpy(w->k_word, L"Getting ready");
        wcscpy(w->k_sub, L"Setting up vJoy. Up to 90 s is normal.");
    } else if (s->stalled) {
        w->take = 1;
        wcscpy(w->k_word, L"Relay not responding");
        wcscpy(w->k_sub, L"No new input for 2 s. Instruments are flagged until it comes back.");
        w->k_class = UI_CRIT_FILL;
    } else if (s->relay == RELAY_STATE_MENU) {
        w->take = 1;
        wcscpy(w->k_word, L"Input frozen");
        wcscpy(w->k_sub, L"Finish it in the console");
        w->k_class = UI_CRIT_FILL;
    } else if (s->vjoy_busy) {
        w->take = 1;
        wcscpy(w->k_word, L"vJoy busy");
        wcscpy(w->k_sub, L"Another program holds device 2. Retrying.");
        w->k_class = UI_CRIT_FILL;
    } else if (s->connecting) {
        w->take = 1;
        wcscpy(w->k_word, L"Connecting");
        wcscpy(w->k_sub, L"Hiding your controller from games");
    } else if (!live) {
        w->take = 1;
        wcscpy(w->k_word, L"No controller");
        if (!s->ever) {
            wcscpy(w->k_sub, L"Turn on any controller. Don't start your game until this says Ready.");
        } else if (s->pad_kept) {
            swprintf(w->k_sub, 96, L"Dropped %d s ago. Game keeps its virtual pad. It reconnects by itself.", s->dropped_s);
            w->k_class = UI_WARN_FILL;
        } else {
            swprintf(w->k_sub, 96, L"Dropped %d s ago. Game lost its virtual pad until you reconnect.", s->dropped_s);
            w->k_class = UI_CRIT_FILL;
        }
    } else {
        wcscpy(w->k_word, L"Ready");
        wcscpy(w->k_sub, ui_pad_short(s->backend, s->is_bt, wired_xbox));
        if (!s->hidden) {
            if (wired_xbox) {
                wcscpy(w->k_sub, L"Wired Xbox pad can't be hidden");
                w->k_class = UI_WARN_LINE;
            } else {
                wcscpy(w->k_word, L"Not hidden");
                wcscpy(w->k_sub, L"Real pad visible: game sees both");
                w->k_class = UI_WARN_FILL;
            }
        }
        if (low && !w->k_class) w->k_class = UI_CRIT_LINE;
    }

    /* Battery */
    if (live) {
        if (s->batt_src == BATTERY_SRC_PERCENT) {
            w->batt_kind = 1;
            w->batt_pct = s->batt_pct;
            w->batt_chg = s->batt_chg;
            w->batt_level = s->batt_chg ? 0 : s->batt_pct <= 15 ? 2 : s->batt_pct <= 40 ? 1 : 0;
            swprintf(w->batt, 16, L"%d%%", s->batt_pct);
        } else if (s->batt_src == BATTERY_SRC_COARSE) {
            w->batt_kind = 1;
            int c = s->batt_coarse;
            w->batt_pct = c == XINPUT_BATTERY_LEVEL_EMPTY ? 5 : c == XINPUT_BATTERY_LEVEL_LOW ? 25 : c == XINPUT_BATTERY_LEVEL_MEDIUM ? 66 : 100;
            w->batt_level = c == XINPUT_BATTERY_LEVEL_EMPTY ? 2 : c == XINPUT_BATTERY_LEVEL_LOW ? 1 : 0;
            wcscpy(w->batt, c == XINPUT_BATTERY_LEVEL_EMPTY ? L"Empty" : c == XINPUT_BATTERY_LEVEL_LOW ? L"Low" : c == XINPUT_BATTERY_LEVEL_MEDIUM ? L"Medium" : L"Full");
        } else if (s->batt_src == BATTERY_SRC_WIRED) {
            w->batt_kind = 2;
            wcscpy(w->batt, L"Wired");
        } else {
            w->batt_kind = 3;
            wcscpy(w->batt, L"Battery ?");
        }
    }

    /* The rings. A flagged ring can't pass for a good one. */
    w->flagged = !w->ready;
    wcscpy(w->flag, s->stalled ? L"No data" : L"No input");
    if (w->flagged) {
        w->r_dash = 1;
        wcscpy(w->r_flag, w->flag);
    } else if (!s->fl_on) {
        w->r_dash = 1;
        wcscpy(w->r_flag, L"Off");
    } else if (s->mode != 1) {
        w->r_dash = 1;
        wcscpy(w->r_flag, L"HOTAS only");
    } else {
        wcscpy(w->r_title, s->head_mode ? L"Head" : L"Free look");
        if (s->fl_state == FREELOOK_LOCKED) {
            w->r_follow = UI_R_FROZEN;
            w->r_color = 2;
        } else if (!s->head_mode) {
            if (s->fl_state == FREELOOK_OFF) swprintf(w->r_flag, 16, L"Hold %ls", r1);
            else w->r_follow = UI_R_STICK;
        } else if (s->no_face && s->head_st != HEAD_ST_TRACKING) {
            w->r_follow = UI_R_FROZEN;
            w->r_hollow = 1;
        } else if (s->head_st == HEAD_ST_NO_TRACKER) {
            w->r_dash = 1;
            wcscpy(w->r_flag, L"Waiting");
        } else {
            w->r_follow = UI_R_HEAD;
            if (s->head_st == HEAD_ST_TRACKING) w->r_color = 1;
            else w->r_dim = 1;
        }
    }

    swprintf(w->summary, 64, L"%ls, %ls, %ls%ls%ls", w->m_word, w->take ? L"-" : w->l_word, w->k_word,
             w->batt_kind == 1 ? L" " : L"", w->batt_kind == 1 ? w->batt : L"");
}

/* ---------- Reading the relay ---------- */

static volatile LONG g_ui_game_up = 0;
static volatile LONG g_ui_window_open = 0;

static void ui_read_state(ui_state_t *s) {
    static int ever, last_relay = -1, pad_kept, was_valid_tick, drop_tick;
    static LONG last_tick;
    static DWORD tick_seen;
    static int head_files = -1;
    static DWORD valid_seen;
    DWORD now = GetTickCount();
    memset(s, 0, sizeof(*s));
    s->relay = g_relay_state;
    if (s->relay == RELAY_STATE_CONNECTED) ever = 1;
    if (s->relay != last_relay) {
        if (last_relay == RELAY_STATE_CONNECTED && s->relay == RELAY_STATE_WAITING) {
            drop_tick = (int)now;
            pad_kept = g_vigem_target != NULL;
        }
        last_relay = s->relay;
    }
    s->ever = ever;
    s->dropped_s = ever ? (int)((now - (DWORD)drop_tick) / 1000) : 0;
    s->pad_kept = pad_kept;
    s->connecting = g_ui_connecting;
    s->vjoy_busy = g_ui_vjoy_busy;

    /* The stall check must not false-alarm: only while connected, and a gap in
       our own ticks (sleep, or a modal window move) restarts the wait. */
    if (g_ui_tick != last_tick || s->relay != RELAY_STATE_CONNECTED || now - tick_seen > 60000) {
        last_tick = g_ui_tick;
        tick_seen = now;
    }
    s->stalled = s->relay == RELAY_STATE_CONNECTED && now - tick_seen >= 2000;

    s->mode = g_hidden_mode;
    s->fl_on = g_freelook_enabled;
    s->head_mode = g_head_mode;
    s->fl_state = s->mode == 1 ? g_freelook_state : FREELOOK_OFF;
    s->head_st = head_status();
    s->cam_on = ht_running();
    if (s->cam_on && g_ht_raw_valid) valid_seen = now;
    s->no_face = s->cam_on && now - valid_seen >= 1000;
    (void)was_valid_tick;
    s->alt_held = g_freelook_alt_held;
    if (head_files < 0) head_files = ht_files_present();
    s->head_files = head_files;
    s->has_camera = g_head_camera[0] != 0;
    s->has_game = g_game_path[0] != 0;
    s->game_up = g_ui_game_up;
    s->cam_failed = g_ui_head_failed;
    s->backend = g_backend;
    s->is_bt = g_ps_output_is_bt;
    s->vigem_on = g_vigem_target != NULL;
    s->vigem_ds4 = g_vigem_emulate_ds4;
    s->vigem_ok = g_vigem_available;
    s->hidden = g_hidhide_registered;
    s->batt_src = g_battery_src;
    s->batt_pct = g_battery_percent;
    s->batt_chg = g_battery_charging;
    s->batt_coarse = g_battery_coarse;
    s->fl_speed = g_freelook_mouse_speed;
    s->head_speed = g_head_speed;
    s->hotkey_failed = g_ui_hotkey_failed;
}

/* ---------- Theme ---------- */

typedef struct {
    int dark, acrylic;
    D2D1_COLOR_F tint, flat, paper, border, subtle, fill, hover, text, sec, onacc, ring, cross, btnoff, thumb_off;
    D2D1_COLOR_F acc, thin, ok, warn, warn_fill, warn_ink, crit, crit_fill, crit_ink, lock, lock_fill, lock_ink;
    D2D1_COLOR_F symbol, face[4];
} ui_theme_t;

static D2D1_COLOR_F ui_rgb(UINT32 rgb, float a) {
    D2D1_COLOR_F c = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
    return c;
}

static D2D1_COLOR_F ui_alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

static int ui_reg_dword(const char *key, const char *value, DWORD fallback) {
    DWORD v = fallback, n = sizeof(v);
    RegGetValueA(HKEY_CURRENT_USER, key, value, RRF_RT_REG_DWORD, NULL, &v, &n);
    return (int)v;
}

static int g_backdrop_failed = 0; /* Windows older than build 22621 can't do it: solid colors from then on */

/* Follows Windows: light or dark, and its accent color. There's no picker. */
static void ui_load_theme(ui_theme_t *t) {
    const char *personal = "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    int dark = !ui_reg_dword(personal, "AppsUseLightTheme", 0);
    int transparency = ui_reg_dword(personal, "EnableTransparency", 1);
    const char *force = getenv("RCR_THEME");
    if (force) dark = strcmp(force, "light") != 0;
    int ps = g_backend != BACKEND_XINPUT;
    memset(t, 0, sizeof(*t));
    t->dark = dark;
    t->acrylic = transparency && !g_backdrop_failed;
    BYTE pal[32] = {0};
    DWORD n = sizeof(pal);
    int has_pal = RegGetValueA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                               "AccentPalette", RRF_RT_REG_BINARY, NULL, pal, &n) == ERROR_SUCCESS && n >= 20;
#define UI_PAL(i) (((UINT32)pal[(i) * 4] << 16) | ((UINT32)pal[(i) * 4 + 1] << 8) | pal[(i) * 4 + 2])
    if (dark) {
        t->tint = ui_rgb(0x202020, .6f);
        t->flat = ui_rgb(0x202020, 1);
        t->paper = ui_rgb(0x2C2C2C, 1);
        t->border = ui_rgb(0xFFFFFF, .28f);
        t->subtle = ui_rgb(0xFFFFFF, .09f);
        t->fill = ui_rgb(0xFFFFFF, .06f);
        t->hover = ui_rgb(0xFFFFFF, .09f);
        t->text = ui_rgb(0xFFFFFF, 1);
        t->sec = ui_rgb(0xFFFFFF, .784f);
        t->onacc = ui_rgb(0x000000, 1);
        t->ring = ui_rgb(0xFFFFFF, .35f);
        t->cross = ui_rgb(0xFFFFFF, .18f);
        t->btnoff = ui_rgb(0xFFFFFF, .09f);
        t->thumb_off = ui_rgb(0xCFCFCF, 1);
        t->acc = ui_rgb(has_pal ? UI_PAL(1) : 0x60CDFF, 1);
        t->thin = ui_rgb(has_pal ? UI_PAL(0) : 0x60CDFF, 1);
        t->ok = ui_rgb(0x6CCB5F, 1);
        t->warn = ui_rgb(0xFCE100, 1);
        t->warn_fill = ui_rgb(0xFCE100, 1);
        t->warn_ink = ui_rgb(0x000000, 1);
        t->crit = ui_rgb(0xFF99A4, 1);
        t->lock = ui_rgb(0xB69CFF, 1);
        t->lock_fill = ui_rgb(0x7A4FE0, 1);
        t->symbol = ui_rgb(0x17171A, 1);
        UINT32 xb[4] = {0xFFD23F, 0xFF4D4D, 0x3BD15C, 0x3B8BFF}, pl[4] = {0x4CE0B3, 0xFF6B6B, 0x8CC4FF, 0xFF96DA};
        for (int i = 0; i < 4; i++) t->face[i] = ui_rgb(ps ? pl[i] : xb[i], 1);
    } else {
        t->tint = ui_rgb(0xF3F3F3, .6f);
        t->flat = ui_rgb(0xF3F3F3, 1);
        t->paper = ui_rgb(0xF9F9F9, 1);
        t->border = ui_rgb(0x000000, .32f);
        t->subtle = ui_rgb(0x000000, .09f);
        t->fill = ui_rgb(0xFFFFFF, .70f);
        t->hover = ui_rgb(0xF9F9F9, .5f);
        t->text = ui_rgb(0x000000, .894f);
        t->sec = ui_rgb(0x000000, .608f);
        t->onacc = ui_rgb(0xFFFFFF, 1);
        t->ring = ui_rgb(0x000000, .35f);
        t->cross = ui_rgb(0x000000, .18f);
        t->btnoff = ui_rgb(0x000000, .06f);
        t->thumb_off = ui_rgb(0x5D5D5D, 1);
        t->acc = ui_rgb(has_pal ? UI_PAL(4) : 0x005FB8, 1);
        t->thin = t->acc;
        t->ok = ui_rgb(0x0F7B0F, 1);
        t->warn = ui_rgb(0x9D5D00, 1);
        t->warn_fill = ui_rgb(0x9D5D00, 1);
        t->warn_ink = ui_rgb(0xFFFFFF, 1);
        t->crit = ui_rgb(0xC42B1C, 1);
        t->lock = ui_rgb(0x6B3FD4, 1);
        t->lock_fill = ui_rgb(0x6B3FD4, 1);
        t->symbol = ui_rgb(0xFFFFFF, 1);
        UINT32 xb[4] = {0x9D7A00, 0xC42B1C, 0x107C10, 0x0F6CBD}, pl[4] = {0x00856F, 0xC42B1C, 0x2F6FB5, 0xB4238C};
        for (int i = 0; i < 4; i++) t->face[i] = ui_rgb(ps ? pl[i] : xb[i], 1);
    }
#undef UI_PAL
    t->crit_fill = ui_rgb(0xC42B1C, 1);
    t->crit_ink = ui_rgb(0xFFFFFF, 1);
    t->lock_ink = ui_rgb(0xFFFFFF, 1);
}

/* ---------- Drawing helpers. One target and one brush at a time. ---------- */

enum { F_WORD, F_TITLE, F_BODY, F_BODYB, F_SMALL, F_SMALLB, F_TAG, F_NOTE, F_FACE, F_ICON12, F_ICON18, F_COUNT };

typedef HRESULT(WINAPI *ui_d2d_create_t)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT(WINAPI *ui_dw_create_t)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);
typedef HRESULT(WINAPI *ui_dwm_attr_t)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI *ui_dwm_extend_t)(HWND, const MARGINS *);

static const GUID ui_IID_ID2D1Factory = {0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
static const GUID ui_IID_IDWriteFactory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dw;
static IDWriteTextFormat *g_fmt[F_COUNT];
static ID2D1StrokeStyle *g_dash;
static ui_dwm_attr_t g_dwm_attr;
static ui_dwm_extend_t g_dwm_extend;
static ui_theme_t g_th;

static ID2D1RenderTarget *g_t;
static ID2D1SolidColorBrush *g_b;

#define UI_BRUSH(col) (ID2D1SolidColorBrush_SetColor(g_b, &(col)), (ID2D1Brush *)g_b)

static void ui_fill(float x, float y, float w, float h, D2D1_COLOR_F c) {
    D2D1_RECT_F r = {x, y, x + w, y + h};
    ID2D1RenderTarget_FillRectangle(g_t, &r, UI_BRUSH(c));
}

static void ui_rr(float x, float y, float w, float h, float rad, D2D1_COLOR_F c) {
    D2D1_ROUNDED_RECT r = {{x, y, x + w, y + h}, rad, rad};
    ID2D1RenderTarget_FillRoundedRectangle(g_t, &r, UI_BRUSH(c));
}

static void ui_rr_line(float x, float y, float w, float h, float rad, D2D1_COLOR_F c, float th) {
    D2D1_ROUNDED_RECT r = {{x + th / 2, y + th / 2, x + w - th / 2, y + h - th / 2}, rad, rad};
    ID2D1RenderTarget_DrawRoundedRectangle(g_t, &r, UI_BRUSH(c), th, NULL);
}

static void ui_disc(float cx, float cy, float rad, D2D1_COLOR_F c) {
    D2D1_ELLIPSE e = {{cx, cy}, rad, rad};
    ID2D1RenderTarget_FillEllipse(g_t, &e, UI_BRUSH(c));
}

static void ui_circle(float cx, float cy, float rad, D2D1_COLOR_F c, float th, ID2D1StrokeStyle *st) {
    D2D1_ELLIPSE e = {{cx, cy}, rad, rad};
    ID2D1RenderTarget_DrawEllipse(g_t, &e, UI_BRUSH(c), th, st);
}

static void ui_line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float th) {
    D2D1_POINT_2F a = {x0, y0}, b = {x1, y1};
    ID2D1RenderTarget_DrawLine(g_t, a, b, UI_BRUSH(c), th, NULL);
}

static void ui_clip(float x, float y, float w, float h) {
    D2D1_RECT_F r = {x, y, x + w, y + h};
    ID2D1RenderTarget_PushAxisAlignedClip(g_t, &r, D2D1_ANTIALIAS_MODE_ALIASED);
}

static void ui_unclip(void) { ID2D1RenderTarget_PopAxisAlignedClip(g_t); }

/* align: 0 left, 1 center, 2 right. Vertically centered in the box. */
static void ui_txt(const wchar_t *s, float x, float y, float w, float h, int f, D2D1_COLOR_F c, int align) {
    IDWriteTextFormat *fmt = g_fmt[f];
    if (!fmt || !s) return;
    IDWriteTextFormat_SetTextAlignment(fmt, align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    D2D1_RECT_F r = {x, y, x + w, y + h};
    ID2D1RenderTarget_DrawText(g_t, s, (UINT32)wcslen(s), fmt, &r, UI_BRUSH(c), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

static float ui_txt_w(const wchar_t *s, int f) {
    IDWriteTextLayout *l = NULL;
    DWRITE_TEXT_METRICS m;
    float w = 0;
    if (g_fmt[f] && SUCCEEDED(IDWriteFactory_CreateTextLayout(g_dw, s, (UINT32)wcslen(s), g_fmt[f], 4000, 100, &l))) {
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(l, &m))) w = m.widthIncludingTrailingWhitespace;
        IDWriteTextLayout_Release(l);
    }
    return w;
}

static void ui_make_format(int i, const wchar_t *family, DWRITE_FONT_WEIGHT weight, float size, int wrap) {
    IDWriteFactory_CreateTextFormat(g_dw, family, NULL, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &g_fmt[i]);
    if (!g_fmt[i]) return;
    IDWriteTextFormat_SetParagraphAlignment(g_fmt[i], wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetWordWrapping(g_fmt[i], wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
}

/* Loaded the first time the window opens. Never unloaded: pulling the Direct2D
   stack out of the process that runs the input loop isn't worth the risk. What
   comes back on close is everything made from them. */
static int ui_load_graphics(void) {
    HMODULE d2d = LoadLibraryA("d2d1.dll"), dw = LoadLibraryA("dwrite.dll"), dwm = LoadLibraryA("dwmapi.dll");
    if (!d2d || !dw || !dwm) return 0;
    ui_d2d_create_t c1 = (ui_d2d_create_t)(void *)GetProcAddress(d2d, "D2D1CreateFactory");
    ui_dw_create_t c2 = (ui_dw_create_t)(void *)GetProcAddress(dw, "DWriteCreateFactory");
    g_dwm_attr = (ui_dwm_attr_t)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    g_dwm_extend = (ui_dwm_extend_t)(void *)GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
    if (!c1 || !c2 || !g_dwm_attr || !g_dwm_extend) return 0;
    if (FAILED(c1(D2D1_FACTORY_TYPE_SINGLE_THREADED, &ui_IID_ID2D1Factory, NULL, (void **)&g_d2d))) return 0;
    if (FAILED(c2(DWRITE_FACTORY_TYPE_SHARED, &ui_IID_IDWriteFactory, (IUnknown **)&g_dw))) return 0;

    const wchar_t *icons = L"Segoe Fluent Icons";
    IDWriteFontCollection *fonts = NULL;
    UINT32 idx;
    BOOL found = FALSE;
    if (SUCCEEDED(IDWriteFactory_GetSystemFontCollection(g_dw, &fonts, FALSE)) && fonts) {
        IDWriteFontCollection_FindFamilyName(fonts, icons, &idx, &found);
        IDWriteFontCollection_Release(fonts);
    }
    if (!found) icons = L"Segoe MDL2 Assets";
    const wchar_t *disp = L"Segoe UI Variable Display", *text = L"Segoe UI Variable Text";
    ui_make_format(F_WORD, disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 40, 0);
    ui_make_format(F_TITLE, disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20, 0);
    ui_make_format(F_BODY, text, DWRITE_FONT_WEIGHT_REGULAR, 14, 0);
    ui_make_format(F_BODYB, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, 0);
    ui_make_format(F_SMALL, text, DWRITE_FONT_WEIGHT_REGULAR, 12, 0);
    ui_make_format(F_SMALLB, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12, 0);
    ui_make_format(F_TAG, text, DWRITE_FONT_WEIGHT_REGULAR, 11, 0);
    ui_make_format(F_NOTE, text, DWRITE_FONT_WEIGHT_REGULAR, 12, 1);
    ui_make_format(F_FACE, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, 0);
    ui_make_format(F_ICON12, icons, DWRITE_FONT_WEIGHT_REGULAR, 12, 0);
    ui_make_format(F_ICON18, icons, DWRITE_FONT_WEIGHT_REGULAR, 18, 0);

    D2D1_STROKE_STYLE_PROPERTIES sp = {0};
    sp.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    sp.miterLimit = 10;
    float dashes[2] = {4, 4};
    ID2D1Factory_CreateStrokeStyle(g_d2d, &sp, dashes, 2, &g_dash);
    return 1;
}

static void ui_free_graphics(void) {
    for (int i = 0; i < F_COUNT; i++) {
        if (g_fmt[i]) IDWriteTextFormat_Release(g_fmt[i]);
        g_fmt[i] = NULL;
    }
    if (g_dash) ID2D1StrokeStyle_Release(g_dash);
    g_dash = NULL;
    if (g_dw) IDWriteFactory_Release(g_dw);
    g_dw = NULL;
    if (g_d2d) ID2D1Factory_Release(g_d2d);
    g_d2d = NULL;
}

/* ---------- Window state ---------- */

typedef struct {
    HWND hwnd;
    ID2D1HwndRenderTarget *rt;
    ID2D1SolidColorBrush *br;
    UINT px_w, px_h;
    float dpi;
} ui_surface_t;

enum {
    H_NONE = 0, H_MIN, H_MAX, H_CLOSE, H_TAB_TELE, H_TAB_DETAILS, H_UPDATE, H_BOX_MODE, H_BOX_LOOK, H_BOX_LINK,
    H_TO_OTHER, H_TO_HOTAS, H_TO_NORMAL, H_LAUNCH, H_GEAR, H_REPORT, H_HIT_COUNT
};
typedef struct { float x, y, w, h; int id; } ui_hit_t;

static ui_surface_t g_main;
static ID2D1BitmapRenderTarget *g_bake_rt;
static ID2D1Bitmap *g_bake;
static ID2D1Bitmap *g_icon_bmp;
static int g_bake_dirty = 1;
static int g_tab = 0;   /* 0 telemetry, 1 details */
static ui_words_t g_w;
static float g_scale = 1, g_ox = 0, g_oy = UI_CAP, g_win_w = 760, g_win_h = 520;
static ui_hit_t g_cap_hits[8], g_scene_hits[16];
static int g_cap_n, g_scene_n, g_hover = H_NONE, g_down = H_NONE;
static int g_rate_shown = 0, g_tick_flip = 0;
static int g_ps_ui = 0;

typedef struct { float lx, ly, rx, ry, lt, rt, hx, hy; } ui_live_t;
static ui_live_t g_ease, g_drawn;
static float g_btn[18]; /* fade per lit button */
typedef struct { float x, y; } ui_pt_t;
static ui_pt_t g_trail[3][5];
static DWORD g_trail_at;
static LARGE_INTEGER g_last_frame;
static int g_timer_ms = 0;
static DWORD g_ui_deactivated;

enum { B_A, B_B, B_X, B_Y, B_LB, B_RB, B_VIEW, B_MENU, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_GUIDE, B_TP, B_L3, B_R3, B_COUNT };

static void ui_hit_scene(float x, float y, float w, float h, int id) {
    if (g_scene_n < 16) { ui_hit_t t = {x, y, w, h, id}; g_scene_hits[g_scene_n++] = t; }
}

static void ui_hit_cap(float x, float y, float w, float h, int id) {
    if (g_cap_n < 8) { ui_hit_t t = {x, y, w, h, id}; g_cap_hits[g_cap_n++] = t; }
}

static int ui_hit_at(float x, float y) {
    for (int i = 0; i < g_cap_n; i++) {
        ui_hit_t *h = &g_cap_hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return h->id;
    }
    float sx = (x - g_ox) / g_scale, sy = (y - g_oy) / g_scale;
    for (int i = g_scene_n - 1; i >= 0; i--) {
        ui_hit_t *h = &g_scene_hits[i];
        if (sx >= h->x && sx < h->x + h->w && sy >= h->y && sy < h->y + h->h) return h->id;
    }
    return H_NONE;
}

/* ---------- Drawing: the parts that rarely change, baked into one bitmap ---------- */

static D2D1_COLOR_F ui_word_color(int cls, int col) {
    if (cls == UI_WARN_FILL) return g_th.warn_ink;
    if (cls == UI_CRIT_FILL) return g_th.crit_ink;
    if (cls == UI_LOCK_FILL) return g_th.lock_ink;
    if (col == UI_W_DIM) return g_th.sec;
    if (col == UI_W_OK) return g_th.ok;
    if (col == UI_W_WARN) return g_th.warn;
    return g_th.text;
}

static void ui_draw_box(float x, float w, int cls, int id, const wchar_t *label, const wchar_t *word, const wchar_t *sub, int word_col) {
    float y = 4, h = 100;
    D2D1_COLOR_F fill = cls == UI_WARN_FILL ? g_th.warn_fill : cls == UI_CRIT_FILL ? g_th.crit_fill : cls == UI_LOCK_FILL ? g_th.lock_fill : ui_rgb(0, 0);
    if (fill.a > 0) ui_rr(x, y, w, h, 8, fill);
    else if (g_hover == id) ui_rr(x, y, w, h, 8, g_th.hover);
    D2D1_COLOR_F line = cls == UI_WARN_LINE ? g_th.warn : cls == UI_CRIT_LINE ? g_th.crit : g_th.subtle;
    ui_rr_line(x, y, w, h, 8, line, 1);
    D2D1_COLOR_F ink = ui_word_color(cls, word_col), ink2 = (cls == UI_WARN_FILL || cls == UI_CRIT_FILL || cls == UI_LOCK_FILL) ? ink : g_th.sec;
    ui_txt(label, x + 14, y + 7, id == H_BOX_MODE && g_w.m_btn == 3 ? w - 20 : w - 110, 16, F_SMALLB, ink2, 0);
    ui_txt(word, x + 14, y + 19, w - 20, 52, F_WORD, ink, 0); /* taller than the line, or a g loses its tail */
    ui_txt(sub, x + 14, y + 67, w - 26, 20, F_BODY, ink2, 0);
    ui_hit_scene(x, y, w, h, id);
}

static void ui_draw_small_button(float x, float y, float w, const wchar_t *glyph, const wchar_t *label, int id, int on_fill) {
    D2D1_COLOR_F ink = on_fill ? g_th.warn_ink : g_th.text;
    ui_rr(x, y, w, 24, 4, on_fill ? ui_alpha(g_th.warn_ink, g_hover == id ? .18f : .1f) : g_hover == id ? g_th.hover : g_th.fill);
    ui_rr_line(x, y, w, 24, 4, on_fill ? ui_alpha(g_th.warn_ink, .35f) : g_th.subtle, 1);
    float tx = x + 8;
    if (glyph) {
        ui_txt(glyph, tx, y, 14, 24, F_ICON12, ink, 0);
        tx += 17;
    }
    ui_txt(label, tx, y, w - (tx - x) - 4, 24, F_SMALL, ink, glyph ? 0 : 1);
    ui_hit_scene(x, y, w, 24, id);
}

static void ui_draw_chip(float x, float y, float w, const wchar_t *glyph, const wchar_t *label) {
    ui_rr(x, y, w, 22, 4, g_th.fill);
    ui_rr_line(x, y, w, 22, 4, g_th.subtle, 1);
    float tx = x + 7;
    if (glyph) {
        ui_txt(glyph, tx, y, 14, 22, F_ICON12, g_th.sec, 0);
        tx += 16;
    }
    ui_txt(label, tx, y, w - (tx - x), 22, F_TAG, g_th.sec, 0);
}

static void ui_draw_battery(float right, float y) {
    if (!g_w.batt_kind) return;
    D2D1_COLOR_F ink = g_w.k_class == UI_WARN_FILL ? g_th.warn_ink : g_th.text;
    float tw = ui_txt_w(g_w.batt, F_SMALL) + 1;
    float tx = right - tw;
    if (g_w.batt_kind == 3) {
        ui_txt(g_w.batt, tx, y, tw, 20, F_SMALL, g_th.sec, 0);
        return;
    }
    if (g_w.batt_level == 2) ink = g_th.crit;
    ui_txt(g_w.batt, tx, y, tw, 20, F_SMALL, ink, 0);
    if (g_w.batt_kind == 2) {
        ui_txt(L"\xE83E", tx - 18, y, 16, 20, F_ICON12, ink, 0); /* plug */
        return;
    }
    D2D1_COLOR_F g = g_w.batt_level == 2 ? g_th.crit : g_w.batt_level == 1 ? g_th.warn : ink;
    float gx = tx - 30, gy = y + 4;
    ui_rr_line(gx, gy, 21, 12, 2.5f, g, 1);
    ui_rr(gx + 22, gy + 4, 2, 4, 1, g);
    ui_rr(gx + 2.5f, gy + 2.5f, 16 * (float)g_w.batt_pct / 100.f, 7, 1, g);
    if (g_w.batt_chg) ui_txt(L"\xE945", gx - 15, y, 14, 20, F_ICON12, ink, 0); /* bolt */
}

static const wchar_t *ui_btn_name(int b) {
    static const wchar_t *const xb[] = {L"A", L"B", L"X", L"Y", L"LB", L"RB"};
    static const wchar_t *const pl[] = {L"", L"", L"", L"", L"L1", L"R1"};
    return b < 6 ? (g_ps_ui ? pl[b] : xb[b]) : L"";
}

/* Face symbol: letters on Xbox, drawn shapes on PlayStation. i: 0 Y/Triangle, 1 B/Circle, 2 A/Cross, 3 X/Square. */
static void ui_face_mark(int i, float cx, float cy, D2D1_COLOR_F c) {
    static const wchar_t *const letters[] = {L"Y", L"B", L"A", L"X"};
    if (!g_ps_ui) {
        ui_txt(letters[i], cx - 15, cy - 15, 30, 30, F_FACE, c, 1);
        return;
    }
    float r = 5.5f, th = 1.7f;
    if (i == 0) {
        ui_line(cx, cy - r - 1, cx + r + 1, cy + r - 1, c, th);
        ui_line(cx + r + 1, cy + r - 1, cx - r - 1, cy + r - 1, c, th);
        ui_line(cx - r - 1, cy + r - 1, cx, cy - r - 1, c, th);
    } else if (i == 1) {
        ui_circle(cx, cy, r, c, th, NULL);
    } else if (i == 2) {
        ui_line(cx - r, cy - r, cx + r, cy + r, c, th);
        ui_line(cx + r, cy - r, cx - r, cy + r, c, th);
    } else {
        D2D1_RECT_F q = {cx - r, cy - r, cx + r, cy + r};
        ID2D1RenderTarget_DrawRectangle(g_t, &q, UI_BRUSH(c), th, NULL);
    }
}

/* i: 0 View, 1 Guide or PS, 2 Menu */
static void ui_center_mark(int i, float cx, float cy, D2D1_COLOR_F c) {
    if (i == 0) {
        D2D1_RECT_F a = {cx - 6, cy - 6, cx + 2, cy + 1}, b = {cx - 2, cy - 1, cx + 6, cy + 6};
        ID2D1RenderTarget_DrawRectangle(g_t, &a, UI_BRUSH(c), 1.4f, NULL);
        ID2D1RenderTarget_DrawRectangle(g_t, &b, UI_BRUSH(c), 1.4f, NULL);
    } else if (i == 2) {
        for (int k = -1; k <= 1; k++) ui_line(cx - 5.5f, cy + k * 4.f, cx + 5.5f, cy + k * 4.f, c, 1.5f);
    } else if (g_ps_ui) {
        ui_txt(L"PS", cx - 14, cy - 14, 28, 28, F_SMALLB, c, 1);
    } else {
        ui_circle(cx, cy, 5.5f, c, 1.4f, NULL);
        ui_disc(cx, cy, 1.6f, c);
    }
}

static const float UI_RING_X[3] = {132, 380, 628};
#define UI_RING_Y 234.f
#define UI_RING_R 75.f

static const struct { float x, y, w, h, rad; } UI_DPAD[4] = {
    {120, 343, 24, 26, 4}, {120, 399, 24, 26, 4}, {92, 372, 26, 24, 4}, {146, 372, 26, 24, 4}};
static const float UI_FACE_POS[4][2] = {{628, 355}, {657, 384}, {628, 413}, {599, 384}};
static const float UI_CENTER_X[3] = {336, 380, 424};
#define UI_CENTER_Y 359.f

static void ui_draw_static_telemetry(void) {
    int dead = g_w.flagged;
    D2D1_COLOR_F off = dead ? ui_alpha(g_th.btnoff, .5f) : g_th.btnoff;
    D2D1_COLOR_F lab = dead ? ui_alpha(g_th.sec, .5f) : g_th.text;

    /* Bumpers. Triggers are drawn live, because their fill moves. */
    ui_rr(169, 114, 64, 28, 14, off);
    ui_rr(527, 114, 64, 28, 14, off);
    ui_txt(ui_btn_name(B_LB), 169, 114, 64, 28, F_SMALLB, lab, 1);
    ui_txt(ui_btn_name(B_RB), 527, 114, 64, 28, F_SMALLB, lab, 1);

    /* Rings */
    for (int i = 0; i < 3; i++) {
        int flagged = i == 1 ? g_w.r_dash : dead;
        float cx = UI_RING_X[i];
        ui_circle(cx, UI_RING_Y, UI_RING_R, g_th.ring, 1.5f, flagged ? g_dash : NULL);
        if (!flagged) {
            ui_line(cx - UI_RING_R, UI_RING_Y, cx + UI_RING_R, UI_RING_Y, g_th.cross, 1);
            ui_line(cx, UI_RING_Y - UI_RING_R, cx, UI_RING_Y + UI_RING_R, g_th.cross, 1);
        }
        const wchar_t *flag = i == 1 ? g_w.r_flag : (dead ? g_w.flag : L"");
        if (flag[0]) ui_txt(flag, cx - UI_RING_R, UI_RING_Y - 12, UI_RING_R * 2, 24, F_BODYB, g_th.sec, 1);
    }
    if (g_w.r_title[0]) ui_txt(g_w.r_title, 280, 114, 200, 28, F_SMALLB, g_th.sec, 1);

    /* D-pad */
    for (int i = 0; i < 4; i++) ui_rr(UI_DPAD[i].x, UI_DPAD[i].y, UI_DPAD[i].w, UI_DPAD[i].h, UI_DPAD[i].rad, off);
    ui_fill(120, 372, 24, 24, ui_alpha(off, .45f));

    /* Face buttons: grey disc, symbol in its real color */
    for (int i = 0; i < 4; i++) {
        ui_disc(UI_FACE_POS[i][0], UI_FACE_POS[i][1], 15, off);
        ui_face_mark(i, UI_FACE_POS[i][0], UI_FACE_POS[i][1], dead ? ui_alpha(g_th.face[i], .45f) : g_th.face[i]);
    }

    /* View, Guide or PS, Menu, and the touchpad on a PlayStation pad */
    for (int i = 0; i < 3; i++) {
        ui_disc(UI_CENTER_X[i], UI_CENTER_Y, 14, off);
        ui_center_mark(i, UI_CENTER_X[i], UI_CENTER_Y, lab);
    }
    if (g_ps_ui) {
        ui_rr(320, 392, 120, 24, 6, off);
        ui_txt(L"Touchpad", 320, 392, 120, 24, F_TAG, lab, 1);
    }

    /* Tags */
    if (!dead && g_hidden_mode == 1 && g_freelook_enabled) {
        ui_txt(L"Look", 509, 143, 100, 16, F_TAG, g_th.sec, 1);
        ui_txt(L"R3 lock", 642, 293, 100, 16, F_TAG, g_th.sec, 1);
    }
    if (!dead && g_toggle_button_mask) {
        wchar_t sw[24];
        if (g_toggle_hold_ms) swprintf(sw, 24, L"Switch %.1f s", g_toggle_hold_ms / 1000.0);
        else wcscpy(sw, L"Switch");
        if (g_toggle_button_mask & PHYS_GUIDE_BUTTON) ui_txt(sw, 330, 373, 100, 16, F_TAG, g_th.sec, 1);
        if (g_toggle_button_mask & PHYS_PS_BUTTON) ui_txt(sw, 330, 373, 100, 16, F_TAG, g_th.sec, 1);
    }
}

static void ui_draw_details(void);

static void ui_draw_scene_static(void) {
    g_scene_n = 0;
    if (g_w.take) {
        ui_draw_box(252, 496, g_w.k_class, H_BOX_LINK, L"", g_w.k_word, g_w.k_sub, UI_W_NORMAL);
    } else {
        ui_draw_box(252, 248, g_w.l_class, H_BOX_LOOK, L"Look", g_w.l_word, g_w.l_sub, g_w.l_color);
        ui_draw_box(508, 240, g_w.k_class, H_BOX_LINK, L"Link", g_w.k_word, g_w.k_sub, UI_W_NORMAL);
        float cx = 252 + 248 - 8;
        if (g_w.chip_cam) { cx -= 68; ui_draw_chip(cx, 12, 68, L"\xE722", L"Camera"); }
        if (g_w.chip_alt) { cx -= 36; ui_draw_chip(cx, 12, 32, NULL, L"Alt"); }
        ui_draw_battery(508 + 240 - 8, 12);
    }
    ui_draw_box(12, 232, g_w.m_class, H_BOX_MODE, g_w.m_label, g_w.m_word, g_w.m_sub, UI_W_NORMAL);
    ui_clip(12, 4, 6, 100);
    ui_rr(12, 4, 40, 100, 8, g_w.bar ? ui_rgb(g_w.bar, 1) : g_th.subtle);
    ui_unclip();
    if (g_w.m_btn == 1) ui_draw_small_button(160, 12, 76, L"\xE8AB", L"Normal", H_TO_OTHER, 0);
    else if (g_w.m_btn == 2) ui_draw_small_button(160, 12, 76, L"\xE8AB", L"HOTAS", H_TO_OTHER, 0);
    else if (g_w.m_btn == 3) {
        ui_draw_small_button(26, 69, 62, NULL, L"HOTAS", H_TO_HOTAS, 1);
        ui_draw_small_button(94, 69, 62, NULL, L"Normal", H_TO_NORMAL, 1);
    }

    if (g_tab == 0) ui_draw_static_telemetry();
    else ui_draw_details();

    /* Footer */
    const wchar_t *lt;
    int game_up = g_ui_game_up, acc = 0, on = 1;
    if (!g_game_path[0]) lt = L"Choose game";
    else if (game_up) { lt = L"Game running"; on = 0; }
    else if (g_relay_state != RELAY_STATE_CONNECTED) { lt = L"Launch waits for Ready"; on = 0; }
    else { lt = L"Launch game"; acc = 1; }
    float lw = ui_txt_w(lt, F_BODY) + 24;
    D2D1_COLOR_F lfill = acc ? g_th.acc : g_hover == H_LAUNCH && on ? g_th.hover : g_th.fill;
    ui_rr(12, 444, lw, 32, 4, lfill);
    ui_rr_line(12, 444, lw, 32, 4, g_th.subtle, 1);
    ui_txt(lt, 12, 444, lw, 32, F_BODY, acc ? g_th.onacc : on ? g_th.text : g_th.sec, 1);
    if (on) ui_hit_scene(12, 444, lw, 32, H_LAUNCH);

    char hk[32], btn[48];
    format_hotkey(hk, sizeof(hk), g_hotkey_mods, g_hotkey_vk);
    format_button_combo(btn, sizeof(btn), g_toggle_button_mask, 0);
    wchar_t hint[128];
    if (g_relay_state != RELAY_STATE_CONNECTED) wcscpy(hint, L"Turn the pad on and it connects by itself.");
    else {
#ifdef RCR_OBS_REMINDER
        wcscpy(hint, L"OBS: F12 = clip + marker   Ctrl+Shift+F11 = record");
#else
        swprintf(hint, 128, L"%hs or %ls %hs switches mode", hk, g_toggle_hold_ms ? L"hold" : L"press", btn);
#endif
    }
    if (g_ui_had_crash) {
        ui_txt(L"Something went wrong last time. Click here to report it.", 180, 444, 400, 32, F_SMALL, g_hover == H_REPORT ? g_th.text : g_th.warn, 1);
        ui_hit_scene(180, 444, 400, 32, H_REPORT);
    } else {
        ui_txt(hint, 180, 444, 400, 32, F_SMALL, g_th.sec, 1);
    }

    if (g_hover == H_GEAR) ui_rr(716, 444, 32, 32, 4, g_th.hover);
    ui_txt(L"\xE713", 716, 444, 32, 32, F_ICON18, g_th.text, 1);
    ui_hit_scene(716, 444, 32, 32, H_GEAR);
}

static void ui_draw_caption(void) {
    g_cap_n = 0;
    float bw = 46, w = g_win_w;
    if (g_icon_bmp) {
        D2D1_RECT_F d = {8, 8, 24, 24};
        ID2D1RenderTarget_DrawBitmap(g_t, g_icon_bmp, &d, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    }
    ui_txt(L"Ruthless Controller Relay", 32, 0, 170, 32, F_SMALLB, g_th.text, 0);
    float tx = 206;
    static const wchar_t *const names[2] = {L"Telemetry", L"Details"};
    for (int i = 0; i < 2; i++) {
        float tw = ui_txt_w(names[i], F_SMALLB) + 16;
        ui_txt(names[i], tx, 0, tw, 32, g_tab == i ? F_SMALLB : F_SMALL, g_tab == i || g_hover == H_TAB_TELE + i ? g_th.text : g_th.sec, 1);
        if (g_tab == i) ui_fill(tx + 4, 28, tw - 8, 2, g_th.thin);
        ui_hit_cap(tx, 0, tw, 32, H_TAB_TELE + i);
        tx += tw;
    }
    float right = w - bw * 3;
    if (g_update_ready) {
        wchar_t up[40];
        swprintf(up, 40, L"Update %hs", g_update_tag);
        float uw = ui_txt_w(up, F_SMALL) + 16;
        ui_rr(right - uw - 8, 5, uw, 22, 4, g_hover == H_UPDATE ? g_th.acc : ui_alpha(g_th.acc, .8f));
        ui_txt(up, right - uw - 8, 5, uw, 22, F_SMALL, g_th.onacc, 1);
        ui_hit_cap(right - uw - 8, 5, uw, 22, H_UPDATE);
    }
    /* Own caption buttons: DWM's sit under our tint and read as washed out. */
    static const wchar_t *const glyph[3] = {L"\xE921", L"\xE922", L"\xE8BB"};
    for (int i = 0; i < 3; i++) {
        float x = right + bw * i;
        int id = H_MIN + i;
        if (g_hover == id) ui_fill(x, 0, bw, UI_CAP, id == H_CLOSE ? ui_rgb(0xC42B1C, 1) : g_th.hover);
        const wchar_t *g = i == 1 && IsZoomed(g_main.hwnd) ? L"\xE923" : glyph[i];
        ui_txt(g, x, 0, bw, UI_CAP, F_ICON12, g_hover == id && id == H_CLOSE ? ui_rgb(0xFFFFFF, 1) : g_th.text, 1);
        ui_hit_cap(x, 0, bw, UI_CAP, id);
    }
}

/* ---------- Drawing: the parts that move ---------- */

static void ui_draw_pill(float x, float pull, const wchar_t *name, int outer_left) {
    float w = 130, h = 28, y = 114;
    wchar_t num[8];
    swprintf(num, 8, L"%d", (int)(pull + .5f));
    int dead = g_w.flagged;
    D2D1_COLOR_F off = dead ? ui_alpha(g_th.btnoff, .5f) : g_th.btnoff, ink = dead ? ui_alpha(g_th.sec, .5f) : g_th.text;
    ui_rr(x, y, w, h, 14, off);
    float nx = outer_left ? x + 26.f / 255.f * w : x + w - 26.f / 255.f * w;
    ui_line(nx, y + 6, nx, y + h - 6, g_th.cross, 1);
    ui_txt(name, x + 22, y, w - 44, h, F_SMALLB, ink, outer_left ? 0 : 2);
    if (!dead) ui_txt(num, x + 14, y, w - 28, h, F_SMALL, g_th.sec, outer_left ? 2 : 0);
    float fw = pull / 255.f * w;
    if (fw < .5f || dead) return;
    /* Second pass over the fill, so the text flips color at its edge. */
    ui_clip(outer_left ? x : x + w - fw, y, fw, h);
    ui_rr(x, y, w, h, 14, g_th.acc);
    ui_txt(name, x + 22, y, w - 44, h, F_SMALLB, g_th.onacc, outer_left ? 0 : 2);
    ui_txt(num, x + 14, y, w - 28, h, F_SMALL, g_th.onacc, outer_left ? 2 : 0);
    ui_unclip();
}

static void ui_draw_dot(float cx, float cy, float x, float y, D2D1_COLOR_F c, float rest_dim, int hollow) {
    float m = hypotf(x, y), q = m > 100 ? 100 / m : 1;
    float px = cx + x * q * .75f, py = cy + y * q * .75f;
    float op = rest_dim ? .55f + .45f * (m / 8 > 1 ? 1 : m / 8) : 1;
    if (hollow) {
        ui_circle(px, py, 15, c, 2, NULL);
        return;
    }
    ui_disc(px, py, 16, ui_alpha(c, op));
    ui_disc(px, py, 7.3f, ui_rgb(0xFFFFFF, op));
}

static void ui_draw_trail(int which, float cx, float cy, D2D1_COLOR_F c) {
    for (int i = 0; i < 5; i++) {
        ui_pt_t p = g_trail[which][i];
        float m = hypotf(p.x, p.y), q = m > 100 ? 100 / m : 1;
        if (m < 1) continue;
        ui_disc(cx + p.x * q * .75f, cy + p.y * q * .75f, (3 + i) * 1.45f, ui_alpha(c, (30 + 25 * i) / 255.f));
    }
}

static void ui_draw_camera(void);

static void ui_draw_live(void) {
    wchar_t buf[40];
    /* Footer rate is always live, even on the Details tab */
    static int measured = -1;
    static float num_w;
    swprintf(buf, 40, L"%d/s", g_rate_shown);
    if (measured != g_rate_shown) {
        measured = g_rate_shown;
        num_w = ui_txt_w(buf, F_SMALL);
    }
    ui_txt(buf, 640, 444, 68, 32, F_SMALL, g_th.sec, 2);
    ui_disc(638 + 68 - num_w - 8, 460, 2.5f, g_tick_flip ? g_th.text : g_th.sec);
    if (g_tab != 0) return;

    int dead = g_w.flagged;
    ui_draw_pill(31, g_ease.lt, g_ps_ui ? L"L2" : L"LT", 1);
    ui_draw_pill(599, g_ease.rt, g_ps_ui ? L"R2" : L"RT", 0);
    if (dead) return;

    D2D1_COLOR_F dotc = g_th.thin;
    ui_draw_trail(0, UI_RING_X[0], UI_RING_Y, dotc);
    ui_draw_trail(2, UI_RING_X[2], UI_RING_Y, dotc);
    if (g_btn[B_L3] > .5f) ui_circle(UI_RING_X[0], UI_RING_Y, UI_RING_R, g_th.thin, 3, NULL);
    if (g_btn[B_R3] > .5f) ui_circle(UI_RING_X[2], UI_RING_Y, UI_RING_R, g_th.thin, 3, NULL);
    ui_draw_dot(UI_RING_X[0], UI_RING_Y, g_ease.lx, -g_ease.ly, dotc, 1, 0);
    ui_draw_dot(UI_RING_X[2], UI_RING_Y, g_ease.rx, -g_ease.ry, dotc, 1, 0);
    swprintf(buf, 40, L"X %+03d   Y %+03d", (int)g_ease.lx, (int)g_ease.ly);
    ui_txt(buf, 32, 313, 200, 16, F_SMALL, g_th.sec, 1);
    swprintf(buf, 40, L"X %+03d   Y %+03d", (int)g_ease.rx, (int)g_ease.ry);
    ui_txt(buf, 528, 313, 200, 16, F_SMALL, g_th.sec, 1);

    /* The look ring */
    ui_draw_camera();
    if (g_w.r_follow != UI_R_FLAG) {
        D2D1_COLOR_F c = g_w.r_color == 1 ? g_th.ok : g_w.r_color == 2 ? g_th.lock : g_th.thin;
        if (g_w.r_hollow) c = g_th.warn;
        if (g_w.r_dim) c = ui_alpha(c, .45f);
        if (g_w.r_follow == UI_R_STICK) {
            ui_draw_trail(2, UI_RING_X[1], UI_RING_Y, c);
            ui_draw_dot(UI_RING_X[1], UI_RING_Y, g_ease.rx, -g_ease.ry, c, 0, 0);
        } else {
            if (g_w.r_follow == UI_R_HEAD) ui_draw_trail(1, UI_RING_X[1], UI_RING_Y, c);
            ui_draw_dot(UI_RING_X[1], UI_RING_Y, g_ease.hx, g_ease.hy, c, 0, g_w.r_hollow);
        }
    }

    /* Whatever's lit */
    for (int i = 0; i < 4; i++) {
        int b = i == 0 ? B_Y : i == 1 ? B_B : i == 2 ? B_A : B_X;
        float a = g_btn[b];
        if (a <= 0) continue;
        ui_disc(UI_FACE_POS[i][0], UI_FACE_POS[i][1], 15, ui_alpha(g_th.face[i], a));
        ui_face_mark(i, UI_FACE_POS[i][0], UI_FACE_POS[i][1], ui_alpha(g_th.symbol, a));
    }
    if (g_btn[B_LB] > 0) {
        ui_rr(169, 114, 64, 28, 14, ui_alpha(g_th.acc, g_btn[B_LB]));
        ui_txt(ui_btn_name(B_LB), 169, 114, 64, 28, F_SMALLB, ui_alpha(g_th.onacc, g_btn[B_LB]), 1);
    }
    if (g_btn[B_RB] > 0) {
        ui_rr(527, 114, 64, 28, 14, ui_alpha(g_th.acc, g_btn[B_RB]));
        ui_txt(ui_btn_name(B_RB), 527, 114, 64, 28, F_SMALLB, ui_alpha(g_th.onacc, g_btn[B_RB]), 1);
    }
    for (int i = 0; i < 4; i++) {
        float a = g_btn[B_UP + i];
        if (a > 0) ui_rr(UI_DPAD[i].x, UI_DPAD[i].y, UI_DPAD[i].w, UI_DPAD[i].h, UI_DPAD[i].rad, ui_alpha(g_th.acc, a));
    }
    for (int i = 0; i < 3; i++) {
        float a = g_btn[i == 0 ? B_VIEW : i == 1 ? B_GUIDE : B_MENU];
        if (a <= 0) continue;
        ui_disc(UI_CENTER_X[i], UI_CENTER_Y, 14, ui_alpha(g_th.acc, a));
        ui_center_mark(i, UI_CENTER_X[i], UI_CENTER_Y, ui_alpha(g_th.onacc, a));
    }
    if (g_ps_ui && g_btn[B_TP] > 0) {
        ui_rr(320, 392, 120, 24, 6, ui_alpha(g_th.acc, g_btn[B_TP]));
        ui_txt(L"Touchpad", 320, 392, 120, 24, F_TAG, ui_alpha(g_th.onacc, g_btn[B_TP]), 1);
    }
}

/* ---------- Details tab: what doesn't belong in settings, and what this has done to your PC ---------- */

static void ui_details_row(float *y, const wchar_t *k, const wchar_t *v) {
    ui_txt(k, 24, *y, 150, 20, F_SMALLB, g_th.sec, 0);
    ui_txt(v, 180, *y, 560, 20, F_SMALL, g_th.text, 0);
    *y += 20;
}

static void ui_draw_details(void) {
    float y = 114;
    wchar_t v[160];
    ui_txt(L"This PC", 24, y, 300, 20, F_BODYB, g_th.text, 0);
    ui_draw_small_button(616, y - 2, 128, NULL, L"Report a problem", H_REPORT, 0);
    y += 22;
    ui_details_row(&y, L"vJoy", service_exists("vjoy") == 1 ? L"Installed. Your virtual joystick." : L"Not found");
    ui_details_row(&y, L"HidHide", service_exists("HidHide") == 1 ? L"Installed. Keeps games from seeing the real pad." : L"Not found");
    ui_details_row(&y, L"ViGEmBus", service_exists("ViGEmBus") == 1 ? L"Installed. Your virtual gamepad." : L"Not found");
    ui_details_row(&y, L"Your controller", g_hidhide_registered ? L"Hidden from games. Back to normal when you quit." : L"Not hidden right now.");
    ui_details_row(&y, L"Game Bar", g_gamebar_guide_disabled ? L"Guide button capture is off until you quit." : L"Not changed.");
    ui_details_row(&y, L"Internet", g_update_check ? L"Only GitHub, only for update checks. On." : L"Only GitHub, only for update checks. Off.");
    ui_details_row(&y, L"Camera", ht_running() ? L"On." : L"Off. It only turns on while your game runs in HOTAS mode.");
    y += 8;
    ui_txt(L"Right now", 24, y, 300, 20, F_BODYB, g_th.text, 0);
    y += 22;
    swprintf(v, 160, L"%hs", g_relay_state == RELAY_STATE_CONNECTED ? describe_controller(g_backend) : "None");
    ui_details_row(&y, L"Controller", v);
    swprintf(v, 160, L"vJoy device %u%ls. %ls", g_vjoy_rid, g_vjoy_pump_live ? L", pump running" : L"",
             g_vigem_target ? (g_hidden_mode == 1 ? L"Virtual pad muted." : L"Virtual pad live.") : L"No virtual pad.");
    ui_details_row(&y, L"Output", v);
    swprintf(v, 160, L"LX %d  LY %d  RX %d  RY %d  LT %d  RT %d", lastGamepad.sThumbLX, lastGamepad.sThumbLY, lastGamepad.sThumbRX,
             lastGamepad.sThumbRY, lastGamepad.bLeftTrigger, lastGamepad.bRightTrigger);
    ui_details_row(&y, L"Raw sticks", g_relay_state == RELAY_STATE_CONNECTED ? v : L"-");
    swprintf(v, 160, L"%.0f fps, %.1f ms, gate %ls", g_ht_fps, g_ht_net_ms, g_head_gate_open ? L"open" : L"shut");
    ui_details_row(&y, L"Head", ht_running() ? v : L"Camera off");
    ui_details_row(&y, L"Stream Deck", g_control_pipe_enabled ? L"Pipe on" : L"Pipe off");
    swprintf(v, 160, L"%d/s", g_rate_shown);
    ui_details_row(&y, L"Input changes", v);
}

/* ---------- Painting ---------- */

static void ui_release(void *p);

static void ui_release(void *p) {
    IUnknown **u = (IUnknown **)p;
    if (*u) (*u)->lpVtbl->Release(*u);
    *u = NULL;
}

static void ui_cam_drop(void);

static void ui_drop_target(void) {
    ui_cam_drop();
    ui_release(&g_bake);
    ui_release(&g_bake_rt);
    ui_release(&g_icon_bmp);
    ui_release(&g_main.br);
    ui_release(&g_main.rt);
    g_bake_dirty = 1;
}

/* Draws the app icon into a bitmap Direct2D can use, once per target. */
static void ui_make_icon_bitmap(ID2D1RenderTarget *rt) {
    int n = 32;
    HICON ic = (HICON)LoadImageA(GetModuleHandleA(NULL), "IDI_ICON1", IMAGE_ICON, n, n, LR_DEFAULTCOLOR);
    if (!ic) return;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = n;
    bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib) {
        HGDIOBJ old = SelectObject(dc, dib);
        DrawIconEx(dc, 0, 0, ic, n, n, 0, NULL, DI_NORMAL);
        SelectObject(dc, old);
        D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
        D2D1_SIZE_U sz = {(UINT32)n, (UINT32)n};
        ID2D1RenderTarget_CreateBitmap(rt, sz, bits, (UINT32)n * 4, &bp, &g_icon_bmp);
        DeleteObject(dib);
    }
    DeleteDC(dc);
    DestroyIcon(ic);
}

static int ui_ensure_target(void) {
    RECT r;
    GetClientRect(g_main.hwnd, &r);
    UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
    if (!w || !h) return 0;
    if (g_main.rt && (w != g_main.px_w || h != g_main.px_h)) {
        D2D1_SIZE_U s = {w, h};
        if (FAILED(ID2D1HwndRenderTarget_Resize(g_main.rt, &s))) ui_drop_target();
        ui_release(&g_bake);
        ui_release(&g_bake_rt);
        g_bake_dirty = 1;
    }
    if (!g_main.rt) {
        D2D1_RENDER_TARGET_PROPERTIES p = {0};
        p.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        p.dpiX = p.dpiY = g_main.dpi;
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = {0};
        hp.hwnd = g_main.hwnd;
        hp.pixelSize.width = w;
        hp.pixelSize.height = h;
        if (FAILED(ID2D1Factory_CreateHwndRenderTarget(g_d2d, &p, &hp, &g_main.rt))) return 0;
        D2D1_COLOR_F white = {1, 1, 1, 1};
        ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget *)g_main.rt, &white, NULL, &g_main.br);
        ui_make_icon_bitmap((ID2D1RenderTarget *)g_main.rt);
        g_bake_dirty = 1;
    }
    g_main.px_w = w;
    g_main.px_h = h;
    g_win_w = w * 96.f / g_main.dpi;
    g_win_h = h * 96.f / g_main.dpi;
    float s = fminf(g_win_w / UI_W, (g_win_h - UI_CAP) / UI_H);
    s = s < .55f ? .55f : s > 3 ? 3 : s;
    g_scale = s;
    g_ox = fmaxf(0, (g_win_w - UI_W * s) / 2);
    g_oy = UI_CAP + fmaxf(0, (g_win_h - UI_CAP - UI_H * s) / 2);
    return 1;
}

static void ui_scene_transform(ID2D1RenderTarget *t) {
    D2D1_MATRIX_3X2_F m = {g_scale, 0, 0, g_scale, g_ox, g_oy};
    ID2D1RenderTarget_SetTransform(t, &m);
}

static void ui_identity(ID2D1RenderTarget *t) {
    D2D1_MATRIX_3X2_F m = {1, 0, 0, 1, 0, 0};
    ID2D1RenderTarget_SetTransform(t, &m);
}

static void ui_bake_static(void) {
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_main.rt;
    if (!g_bake_rt) {
        if (FAILED(ID2D1RenderTarget_CreateCompatibleRenderTarget(rt, NULL, NULL, NULL, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &g_bake_rt))) return;
        ID2D1BitmapRenderTarget_GetBitmap(g_bake_rt, &g_bake);
    }
    ID2D1RenderTarget *b = (ID2D1RenderTarget *)g_bake_rt;
    g_t = b;
    g_b = g_main.br;
    ID2D1RenderTarget_BeginDraw(b);
    D2D1_COLOR_F none = {0, 0, 0, 0};
    ID2D1RenderTarget_Clear(b, &none);
    ID2D1RenderTarget_SetTextAntialiasMode(b, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ui_identity(b);
    ui_draw_caption();
    ui_scene_transform(b);
    ui_draw_scene_static();
    ID2D1RenderTarget_EndDraw(b, NULL, NULL);
    g_bake_dirty = 0;
}

static void ui_paint(void) {
    if (!ui_ensure_target()) return;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_main.rt;
    if (g_bake_dirty) ui_bake_static();
    g_t = rt;
    g_b = g_main.br;
    ID2D1RenderTarget_BeginDraw(rt);
    ID2D1RenderTarget_SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ui_identity(rt);
    if (g_th.acrylic) {
        D2D1_COLOR_F none = {0, 0, 0, 0};
        ID2D1RenderTarget_Clear(rt, &none);
        ui_fill(0, 0, g_win_w + 1, g_win_h + 1, g_th.tint);
    } else {
        ID2D1RenderTarget_Clear(rt, &g_th.flat);
    }
    if (g_bake) ID2D1RenderTarget_DrawBitmap(rt, g_bake, NULL, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, NULL);
    ui_scene_transform(rt);
    ui_draw_live();
    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) ui_drop_target(); /* a game's GPU reset lands here */
    g_drawn = g_ease;
}

/* ---------- The render tick ---------- */

static void ui_cam_sync(void);
static void ui_pop_tick(void);
static void ui_pop_theme_changed(void);
static void ui_apply_backdrop(HWND h);
static volatile LONG g_cam_seq;
static LONG g_cam_seq_drawn;

static float ui_ease(float v, float t, float dt, float tau, int *moved) {
    float d = t - v;
    if (fabsf(d) > .25f) v += d * (1 - expf(-dt / tau));
    else v = t;
    if (d != 0) *moved = 1;
    return v;
}

static void ui_btn_set(int b, int on, float dt, int *moved) {
    float a = g_btn[b];
    if (on) {
        if (a != 1) *moved = 1;
        a = 1;
    } else if (a > 0) {
        a -= dt / 120.f;
        if (a < 0) a = 0;
        *moved = 1;
    }
    g_btn[b] = a;
}

static void ui_test_crash(void);

static void ui_set_rate(int ms) {
    if (ms == g_timer_ms) return;
    g_timer_ms = ms;
    SetTimer(g_main.hwnd, 1, (UINT)ms, NULL);
}

/* Nothing draws at rest, and a minimized or fully covered window drops to once a second. */
static void ui_tick(void) {
    static DWORD last_rate, last_slow, moved_at;
    static int covered;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = (float)((double)(now.QuadPart - g_last_frame.QuadPart) * 1000.0 / (double)freq.QuadPart);
    g_last_frame = now;
    if (dt > 100) dt = 100;
    DWORD tick = GetTickCount();
    HWND fg = GetForegroundWindow();

    if (tick - last_slow >= 250) {
        last_slow = tick;
        RECT a, b;
        covered = 0;
        if (fg && fg != g_main.hwnd && GetWindowRect(fg, &a) && GetWindowRect(g_main.hwnd, &b))
            covered = a.left <= b.left && a.top <= b.top && a.right >= b.right && a.bottom >= b.bottom && !IsIconic(fg);
        ui_theme_t th;
        ui_load_theme(&th);
        if (memcmp(&th, &g_th, sizeof(th))) {
            g_th = th;
            g_bake_dirty = 1;
            ui_apply_backdrop(g_main.hwnd);
            ui_pop_theme_changed();
        }
    }
    if (IsIconic(g_main.hwnd) || covered) {
        ui_pop_tick();
        ui_set_rate(1000);
        return;
    }

    ui_state_t st;
    ui_words_t w;
    ui_read_state(&st);
    ui_status(&st, &w);
    if (memcmp(&w, &g_w, sizeof(w))) {
        g_w = w;
        g_bake_dirty = 1;
        wchar_t title[96];
        swprintf(title, 96, L"Ruthless Controller Relay - %ls", w.summary);
        SetWindowTextW(g_main.hwnd, title);
    }
    static LONG crash_seen;
    if (crash_seen != g_ui_had_crash) {
        crash_seen = g_ui_had_crash;
        g_bake_dirty = 1;
    }
    int ps = g_backend != BACKEND_XINPUT;
    if (ps != g_ps_ui) {
        g_ps_ui = ps;
        g_bake_dirty = 1;
    }
    if (g_tab == 1 && tick - last_rate >= 500) g_bake_dirty = 1; /* Details refreshes at 2 Hz, only while shown */

    /* The live data: every real change the input loop has seen */
    XINPUT_GAMEPAD gp = lastGamepad;
    if (!w.ready) memset(&gp, 0, sizeof(gp));
    WORD bt = gp.wButtons;
    int moved = 0;
    float tau = 20;
    g_ease.lx = ui_ease(g_ease.lx, gp.sThumbLX / 327.67f, dt, tau, &moved);
    g_ease.ly = ui_ease(g_ease.ly, gp.sThumbLY / 327.67f, dt, tau, &moved);
    g_ease.rx = ui_ease(g_ease.rx, gp.sThumbRX / 327.67f, dt, tau, &moved);
    g_ease.ry = ui_ease(g_ease.ry, gp.sThumbRY / 327.67f, dt, tau, &moved);
    g_ease.lt = ui_ease(g_ease.lt, gp.bLeftTrigger, dt, tau, &moved);
    g_ease.rt = ui_ease(g_ease.rt, gp.bRightTrigger, dt, tau, &moved);
    g_ease.hx = ui_ease(g_ease.hx, (float)g_head_view_x, dt, tau, &moved);
    g_ease.hy = ui_ease(g_ease.hy, (float)g_head_view_y, dt, tau, &moved);
    moved = 0;
    {
        const float *e = &g_ease.lx, *d = &g_drawn.lx;
        for (int i = 0; i < 8; i++) if (fabsf(e[i] - d[i]) >= (i >= 4 && i < 6 ? 1.f : .67f)) moved = 1;
    }
    ui_btn_set(B_A, (bt & XINPUT_GAMEPAD_A) != 0, dt, &moved);
    ui_btn_set(B_B, (bt & XINPUT_GAMEPAD_B) != 0, dt, &moved);
    ui_btn_set(B_X, (bt & XINPUT_GAMEPAD_X) != 0, dt, &moved);
    ui_btn_set(B_Y, (bt & XINPUT_GAMEPAD_Y) != 0, dt, &moved);
    ui_btn_set(B_LB, (bt & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0, dt, &moved);
    ui_btn_set(B_RB, (bt & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0, dt, &moved);
    ui_btn_set(B_VIEW, (bt & XINPUT_GAMEPAD_BACK) != 0, dt, &moved);
    ui_btn_set(B_MENU, (bt & XINPUT_GAMEPAD_START) != 0, dt, &moved);
    ui_btn_set(B_UP, (bt & XINPUT_GAMEPAD_DPAD_UP) != 0, dt, &moved);
    ui_btn_set(B_DOWN, (bt & XINPUT_GAMEPAD_DPAD_DOWN) != 0, dt, &moved);
    ui_btn_set(B_LEFT, (bt & XINPUT_GAMEPAD_DPAD_LEFT) != 0, dt, &moved);
    ui_btn_set(B_RIGHT, (bt & XINPUT_GAMEPAD_DPAD_RIGHT) != 0, dt, &moved);
    ui_btn_set(B_GUIDE, w.ready && ((bt & XINPUT_GAMEPAD_GUIDE) || g_ps_pressed), dt, &moved);
    ui_btn_set(B_TP, w.ready && g_touchpad_pressed, dt, &moved);
    ui_btn_set(B_L3, (bt & XINPUT_GAMEPAD_LEFT_THUMB) != 0, dt, &moved);
    ui_btn_set(B_R3, (bt & XINPUT_GAMEPAD_RIGHT_THUMB) != 0, dt, &moved);

    /* The number and its tick at 2 Hz: some pads jitter all the time, and that alone shouldn't keep drawing. */
    if (tick - last_rate >= 500) {
        last_rate = tick;
        int r = w.ready ? (int)g_ui_rate : 0;
        if (r != g_rate_shown) {
            g_rate_shown = r;
            moved = 1;
            if (r) g_tick_flip = !g_tick_flip;
        }
    }
    if (tick - g_trail_at >= 66) {
        g_trail_at = tick;
        float src[3][2] = {{g_ease.lx, -g_ease.ly}, {g_ease.hx, g_ease.hy}, {g_ease.rx, -g_ease.ry}};
        for (int r = 0; r < 3; r++) {
            ui_pt_t *t = g_trail[r];
            int unsettled = 0;
            for (int i = 0; i < 5; i++) if (fabsf(t[i].x - src[r][0]) > .5f || fabsf(t[i].y - src[r][1]) > .5f) unsettled = 1;
            if (unsettled) {
                memmove(t, t + 1, sizeof(ui_pt_t) * 4);
                t[4].x = src[r][0];
                t[4].y = src[r][1];
                moved = 1;
            }
        }
    }

    /* A new camera frame counts as moving, only while Show my camera is on. */
    ui_cam_sync();
    if (g_ui_show_camera && ht_running() && g_cam_seq != g_cam_seq_drawn) moved = 1;
    ui_pop_tick();

    if (moved || g_bake_dirty) {
        moved_at = tick;
        ui_paint();
    }
    /* As fast as it's worth while moving, 30 fps with something else in front, and a slow look at rest. */
    if (tick - moved_at > 1000) ui_set_rate(66);
    else ui_set_rate(fg == g_main.hwnd ? 16 : 33);
}

/* ---------- Actions ---------- */

static volatile LONG g_ui_dialog_open = 0;

static DWORD WINAPI ui_pick_game_thread(LPVOID a) {
    (void)a;
    InterlockedExchange(&g_ui_dialog_open, 1);
    run_set_game_flow();
    InterlockedExchange(&g_ui_dialog_open, 0);
    g_bake_dirty = 1;
    return 0;
}

static DWORD WINAPI ui_launch_thread(LPVOID a) {
    if (g_game_path[0]) launch_game();
    else ui_pick_game_thread(a);
    return 0;
}

static void ui_quit(void);

/* ---------- Report a problem: goes through the user's own browser and GitHub account, nothing is sent until they press Submit ---------- */

#define UI_REPO_URL "https://github.com/" RELAY_REPO_PATH
#define UI_PREV_LOG_FILE "driver_install_log.prev.txt"
static void ui_copy_text(const char *text);

static char g_url_file[MAX_PATH + 32];
static volatile LONG g_url_file_at = 0;

/* We run elevated. Going through Explorer opens it in their normal browser, not an admin one. Explorer cuts a link
   on its command line at 259 characters, so it goes in an internet shortcut instead, gone again a few seconds later.
   Measured 2026-11-01: that carries 1,816 characters whole, and by 3,316 the browser never opens. */
static void ui_open_url(const char *url) {
    char tmp[MAX_PATH], arg[MAX_PATH + 40];
    if (!GetTempPathA(sizeof(tmp), tmp)) return;
    snprintf(g_url_file, sizeof(g_url_file), "%sRuthlessRelayLink.url", tmp);
    FILE *f = fopen(g_url_file, "w");
    if (!f) return;
    fprintf(f, "[InternetShortcut]\nURL=%s\n", url);
    fclose(f);
    snprintf(arg, sizeof(arg), "\"%s\"", g_url_file);
    ShellExecuteA(NULL, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
    InterlockedExchange(&g_url_file_at, (LONG)GetTickCount());
}

static void ui_url_file_cleanup(int now) {
    if (!g_url_file_at || (!now && GetTickCount() - (DWORD)g_url_file_at < 15000)) return;
    DeleteFileA(g_url_file);
    InterlockedExchange(&g_url_file_at, 0);
}

/* The end of a log, with their user name taken out of any path. */
static void ui_log_tail(const char *file, char *out, size_t n, int max_bytes) {
    char path[MAX_PATH + 32], dir[MAX_PATH];
    out[0] = '\0';
    get_exe_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > max_bytes ? size - max_bytes : 0, SEEK_SET);
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = '\0';
    char *nl = size > max_bytes ? strchr(out, '\n') : NULL; /* start on a whole line */
    if (nl) memmove(out, nl + 1, strlen(nl));
    char profile[MAX_PATH] = "";
    GetEnvironmentVariableA("USERPROFILE", profile, sizeof(profile));
    size_t pl = strlen(profile);
    if (pl < 4) return;
    for (char *p = out; (p = strstr(p, profile)) != NULL;) {
        memcpy(p, "~", 1);
        memmove(p + 1, p + pl, strlen(p + pl) + 1);
    }
}

static void ui_report_head(char *out, size_t n) {
    OSVERSIONINFOW v = {sizeof(v)};
    typedef LONG(WINAPI * rtl_ver_t)(OSVERSIONINFOW *);
    rtl_ver_t ver = (rtl_ver_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (ver) ver(&v);
    char hk[32], btn[48];
    format_hotkey(hk, sizeof(hk), g_hotkey_mods, g_hotkey_vk);
    format_button_combo(btn, sizeof(btn), g_toggle_button_mask, g_toggle_hold_ms);
    ui_state_t st;
    ui_words_t w;
    ui_read_state(&st);
    ui_status(&st, &w);
    snprintf(out, n,
             "Version: " RELAY_VERSION "\nWindows build: %lu\nController: %s\nState: %ls\nHidden: %s\nDrivers: vJoy %s, HidHide %s, ViGEmBus %s\n"
             "Mode: %ld, free look %s, toggle %s or %s\nUpdates/sec: %ld\n%s",
             (unsigned long)v.dwBuildNumber, g_relay_state == RELAY_STATE_CONNECTED ? describe_controller(g_backend) : "none", w.summary,
             g_hidhide_registered ? "yes" : "no", service_exists("vjoy") == 1 ? "ok" : "missing", service_exists("HidHide") == 1 ? "ok" : "missing",
             service_exists("ViGEmBus") == 1 ? "ok" : "missing", (long)g_hidden_mode, g_head_mode ? "head" : g_freelook_enabled ? "stick" : "off", hk, btn,
             (long)g_ui_rate, g_ui_prev_crash ? "Crashed last time: yes\n" : "");
}

static void ui_report_body(char *out, size_t n, const char *head, const char *log) {
    snprintf(out, n, "**What happened?**\n\n\n\n**What did you expect?**\n\n\n\n<details><summary>Details the relay filled in</summary>\n\n```\n%s\nLast lines of the log:\n%s\n```\n</details>\n", head, log);
}

/* Returns the encoded length, or -1 if it doesn't fit. */
static int ui_url_encode(const char *src, char *dst, int cap) {
    int at = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (at + 4 >= cap) return -1;
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') dst[at++] = (char)*p;
        else at += sprintf(dst + at, "%%%02X", *p);
    }
    dst[at] = '\0';
    return at;
}

/* Assume they aren't signed in. GitHub sends them through sign-in with this whole link inside another one, and
   measured 2026-11-01 it survives that at 4,032 characters and gives a 500 error page at 5,976. The internet shortcut
   above is the tighter limit. So the link stays at 2,000, dropping log lines until it fits, and if even the basics
   don't fit it just opens the form. The whole report goes on the clipboard anyway. */
#define UI_REPORT_URL_MAX 2000
#define UI_ISSUE_URL UI_REPO_URL "/issues/new?title=&body="

static int ui_report_url(char *url, int cap, const char *head, const char *log) {
    static const int keep[] = {1200, 800, 500, 250, 0};
    int base = (int)strlen(UI_ISSUE_URL);
    if (cap > UI_REPORT_URL_MAX + 1) cap = UI_REPORT_URL_MAX + 1;
    size_t room = strlen(head) + strlen(log) + 300;
    char *body = (char *)malloc(room);
    if (!body) return -1;
    int result = -1;
    memcpy(url, UI_ISSUE_URL, (size_t)base + 1);
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]) && result < 0; i++) {
        size_t ll = strlen(log);
        const char *tail = ll > (size_t)keep[i] ? log + ll - keep[i] : log;
        if (tail != log) {
            const char *nl = strchr(tail, '\n');
            tail = nl ? nl + 1 : log + ll;
        }
        ui_report_body(body, room, head, tail);
        int n = ui_url_encode(body, url + base, cap - base);
        if (n >= 0) result = base + n;
    }
    if (result < 0) {
        int n = ui_url_encode("**What happened?**\n\n\n\nThe details are on your clipboard: press Ctrl+V here.\n", url + base, cap - base);
        result = n >= 0 ? base + n : base;
    }
    free(body);
    return result;
}

static void ui_report_send(int open) {
    char head[800], log[3000], full[4200], url[UI_REPORT_URL_MAX + 1];
    ui_report_head(head, sizeof(head));
    ui_log_tail(g_ui_prev_crash ? UI_PREV_LOG_FILE : INSTALL_LOG_FILE, log, sizeof(log), 2800);
    ui_report_body(full, sizeof(full), head, log);
    ui_copy_text(full);
    if (open && ui_report_url(url, sizeof(url), head, log) > 0) ui_open_url(url);
}

static void ui_copy_text(const char *text) {
    size_t n = strlen(text) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), text, n);
    GlobalUnlock(g);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else {
        GlobalFree(g);
    }
}

/* ---------- Show my camera: optional, off by default, and only while the window is open ---------- */

#define UI_CAM_N 150
static BYTE g_cam_gray[UI_CAM_N * UI_CAM_N];
static BYTE g_cam_bgra[UI_CAM_N * UI_CAM_N * 4];
static ID2D1Bitmap *g_cam_bmp;
static ID2D1BitmapBrush *g_cam_brush;

/* On the tracker's thread, once a frame, only while the picture is wanted. Mirrored, like a mirror. */
static void ui_cam_frame(const ht_image *im, double yaw, double pitch, double roll, int valid) {
    (void)yaw; (void)pitch; (void)roll; (void)valid;
    if (!im || !im->p || im->w <= 0 || im->h <= 0) return;
    int s = im->w < im->h ? im->w : im->h, ox = (im->w - s) / 2, oy = (im->h - s) / 2;
    for (int y = 0; y < UI_CAM_N; y++) {
        const unsigned char *row = im->p + (size_t)(oy + y * s / UI_CAM_N) * im->w;
        for (int x = 0; x < UI_CAM_N; x++) g_cam_gray[y * UI_CAM_N + x] = row[ox + (UI_CAM_N - 1 - x) * s / UI_CAM_N];
    }
    InterlockedIncrement(&g_cam_seq);
}

static void ui_cam_sync(void) {
    int want = g_ui_show_camera && g_tab == 0 && g_main.hwnd && !IsIconic(g_main.hwnd);
    g_ht_on_frame = want ? ui_cam_frame : NULL;
}

static void ui_cam_drop(void) { /* they belong to the render target */
    ui_release(&g_cam_brush);
    ui_release(&g_cam_bmp);
}

static void ui_cam_release(void) {
    g_ht_on_frame = NULL;
    ui_cam_drop();
}

/* Inside the head ring, under the dot. */
static void ui_draw_camera(void) {
    if (!g_ui_show_camera || !ht_running() || g_cam_seq == 0) return;
    ID2D1RenderTarget *rt = g_t;
    if (!g_cam_bmp) {
        D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE}, 96, 96};
        D2D1_SIZE_U sz = {UI_CAM_N, UI_CAM_N};
        if (FAILED(ID2D1RenderTarget_CreateBitmap(rt, sz, NULL, 0, &bp, &g_cam_bmp))) return;
        g_cam_seq_drawn = -1;
    }
    if (g_cam_seq != g_cam_seq_drawn) {
        g_cam_seq_drawn = g_cam_seq;
        for (int i = 0; i < UI_CAM_N * UI_CAM_N; i++) {
            BYTE v = g_cam_gray[i];
            g_cam_bgra[i * 4] = g_cam_bgra[i * 4 + 1] = g_cam_bgra[i * 4 + 2] = v;
            g_cam_bgra[i * 4 + 3] = 255;
        }
        ID2D1Bitmap_CopyFromMemory(g_cam_bmp, NULL, g_cam_bgra, UI_CAM_N * 4);
    }
    if (!g_cam_brush && FAILED(rt->lpVtbl->CreateBitmapBrush(rt, g_cam_bmp, NULL, NULL, &g_cam_brush))) return;
    float d = UI_RING_R * 2, k = d / UI_CAM_N;
    D2D1_MATRIX_3X2_F m = {k, 0, 0, k, UI_RING_X[1] - UI_RING_R, UI_RING_Y - UI_RING_R};
    ID2D1BitmapBrush_SetTransform(g_cam_brush, &m);
    ID2D1BitmapBrush_SetOpacity(g_cam_brush, .8f);
    D2D1_ELLIPSE e = {{UI_RING_X[1], UI_RING_Y}, UI_RING_R, UI_RING_R};
    ID2D1RenderTarget_FillEllipse(rt, &e, (ID2D1Brush *)g_cam_brush);
}

/* ---------- The settings popup. Its own window beside the panel, so you watch what you're changing ---------- */

enum { PG_NONE = 0, PG_RACK, PG_MODE, PG_PAD, PG_LOOK, PG_HEAD, PG_MAP, PG_GAME, PG_DECK, PG_UPDATES, PG_APP, PG_HELP, PG_LINK, PG_REPORT };
enum {
    P_NONE = 0, P_BACK, P_TILE, /* P_TILE + PG_* */
    P_QUIT = 40, P_QUIT_YES, P_QUIT_NO,
    P_SEG = 50,  /* P_SEG + n, what they mean depends on the page */
    P_HOTKEY = 80, P_HOTKEY_NONE, P_BUTTON, P_BUTTON_NONE, P_CANCEL,
    P_SPEED, P_SPEED_DOWN, P_SPEED_UP, P_YAW, P_PITCH, P_DEAD, P_FLIP_Y, P_FLIP_P, P_SHOW_CAM, P_RECENTER, P_CAM = 100, /* + camera index */
    P_CAMMODE = 120, /* + mode index */
    P_MAP = 130, P_MAP_RESET, P_GAME_CHANGE, P_GAME_LAUNCH, P_PIPE, P_DISCORD, P_DISCORD_NONE, P_UPDCHECK, P_UPDATE_NOW, P_UPDATE_RESTART,
    P_TRAY, P_CONSOLE, P_CONSOLE_MODE, P_REPORT, P_REPORT_OPEN, P_REPORT_COPY, P_OPENLOG, P_OPENINI, P_RELEASES
};
enum { CAP_NONE = 0, CAP_HOTKEY, CAP_BUTTON, CAP_DISCORD, CAP_MAP1, CAP_MAP2 };

static ui_surface_t g_pop;
static int g_page = PG_NONE, g_pop_dirty = 0, g_pop_hover = P_NONE, g_pop_down = P_NONE, g_drag = P_NONE, g_quit_ask = 0;
static float g_py, g_pop_scroll = 0, g_pop_h = 300, g_pop_max_h = 800;
static ui_hit_t g_pop_hits[80];
static int g_pop_n = 0;
static int g_cap = CAP_NONE;
static DWORD g_cap_end = 0, g_cap_saved = 0, g_cap_accum = 0, g_cap_first = 0, g_cap_since = 0;
static int g_cap_wait_release = 0;
static wchar_t g_pop_msg[160];
static char g_cams[16][128];
static volatile LONG g_cams_n = -1;
static volatile LONG g_update_stage = 0; /* 0 idle, 1 working, 2 done, 3 failed */
static char g_update_problem[160];
static DWORD g_save_at = 0;

#define PX 16.f
#define PW 328.f

static void p_hit(float x, float y, float w, float h, int id) {
    if (g_pop_n < 80) { ui_hit_t t = {x, y, w, h, id}; g_pop_hits[g_pop_n++] = t; }
}

static void p_title(const wchar_t *t) {
    if (g_page != PG_RACK) {
        if (g_pop_hover == P_BACK) ui_rr(PX - 6, g_py, 28, 28, 4, g_th.hover);
        ui_txt(L"\xE72B", PX - 6, g_py, 28, 28, F_ICON12, g_th.text, 1);
        p_hit(PX - 6, g_py, 28, 28, P_BACK);
        ui_txt(t, PX + 26, g_py, PW - 26, 28, F_TITLE, g_th.text, 0);
    } else {
        ui_txt(t, PX, g_py, PW, 28, F_TITLE, g_th.text, 0);
    }
    g_py += 38;
}

static void p_label(const wchar_t *t) {
    ui_txt(t, PX, g_py, PW, 18, F_SMALLB, g_th.sec, 0);
    g_py += 22;
}

static void p_note(const wchar_t *t) {
    IDWriteTextLayout *l = NULL;
    DWRITE_TEXT_METRICS m;
    float h = 18;
    if (SUCCEEDED(IDWriteFactory_CreateTextLayout(g_dw, t, (UINT32)wcslen(t), g_fmt[F_NOTE], PW, 400, &l))) {
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(l, &m))) h = m.height;
        IDWriteTextLayout_Release(l);
    }
    ui_txt(t, PX, g_py, PW, h + 2, F_NOTE, g_th.sec, 0);
    g_py += h + 10;
}

/* style: 0 plain, 1 accent, 2 quiet disabled */
static void p_button_at(float x, float y, float w, const wchar_t *t, int id, int style) {
    D2D1_COLOR_F fill = style == 1 ? g_th.acc : g_pop_hover == id && style != 2 ? g_th.hover : g_th.fill;
    ui_rr(x, y, w, 32, 4, fill);
    ui_rr_line(x, y, w, 32, 4, g_th.subtle, 1);
    ui_txt(t, x + 6, y, w - 12, 32, F_BODY, style == 1 ? g_th.onacc : style == 2 ? g_th.sec : g_th.text, 1);
    if (style != 2) p_hit(x, y, w, 32, id);
}

static void p_button(const wchar_t *t, int id, int style) {
    p_button_at(PX, g_py, PW, t, id, style);
    g_py += 40;
}

static void p_toggle(const wchar_t *t, int on, int id) {
    if (g_pop_hover == id) ui_rr(PX - 6, g_py - 3, PW + 12, 28, 4, g_th.hover);
    ui_txt(t, PX, g_py, PW - 50, 22, F_BODY, g_th.text, 0);
    float x = PX + PW - 40, y = g_py + 1;
    if (on) ui_rr(x, y, 40, 20, 10, g_th.acc);
    else ui_rr_line(x, y, 40, 20, 10, g_th.border, 1);
    ui_disc(on ? x + 30 : x + 10, y + 10, 6, on ? g_th.onacc : g_th.thumb_off);
    p_hit(PX - 6, g_py - 3, PW + 12, 28, id);
    g_py += 32;
}

static void p_segment(const wchar_t *const *items, int n, int sel, int base) {
    float w = PW / n;
    for (int i = 0; i < n; i++) {
        float x = PX + w * i;
        int id = base + i;
        D2D1_COLOR_F fill = i == sel ? g_th.acc : g_pop_hover == id ? g_th.hover : g_th.fill;
        ui_rr(x, g_py, w - 1, 30, 4, fill);
        ui_rr_line(x, g_py, w - 1, 30, 4, g_th.subtle, 1);
        ui_txt(items[i], x, g_py, w - 1, 30, F_BODY, i == sel ? g_th.onacc : g_th.text, 1);
        p_hit(x, g_py, w - 1, 30, id);
    }
    g_py += 40;
}

/* t is 0 to 1 along the rail */
static void p_slider(const wchar_t *label, float t, const wchar_t *value, int id) {
    ui_txt(label, PX, g_py, PW - 80, 20, F_BODY, g_th.text, 0);
    ui_txt(value, PX + PW - 80, g_py, 80, 20, F_SMALL, g_th.sec, 2);
    g_py += 24;
    float x = PX + 10, w = PW - 20, y = g_py + 10;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ui_rr(x, y - 2, w, 4, 2, g_th.border);
    ui_rr(x, y - 2, w * t, 4, 2, g_th.acc);
    ui_disc(x + w * t, y, 10, g_th.paper);
    ui_circle(x + w * t, y, 10, g_th.subtle, 1, NULL);
    ui_disc(x + w * t, y, g_pop_hover == id || g_drag == id ? 7 : 6, g_th.acc);
    p_hit(PX, g_py, PW, 20, id);
    g_py += 30;
}

static void p_info(const wchar_t *k, const wchar_t *v) {
    ui_txt(k, PX, g_py, 96, 20, F_SMALLB, g_th.sec, 0);
    ui_txt(v, PX + 100, g_py, PW - 100, 20, F_SMALL, g_th.text, 0);
    g_py += 22;
}

static void p_field(const wchar_t *value, int id_change, int id_none, int capturing, int capture_id) {
    float y = g_py;
    ui_rr(PX, y, PW, 32, 4, g_th.fill);
    ui_rr_line(PX, y, PW, 32, 4, capturing ? g_th.thin : g_th.subtle, 1);
    if (capturing) {
        wchar_t s[64];
        swprintf(s, 64, L"%ls  %d s", capture_id == CAP_HOTKEY ? L"Press the keys now. Esc cancels." : L"Press and hold on your pad. Esc cancels.", (int)((g_cap_end - GetTickCount()) / 1000) + 1);
        ui_txt(s, PX + 10, y, PW - 14, 32, F_SMALL, g_th.text, 0);
    } else {
        ui_txt(value, PX + 10, y, PW - 130, 32, F_BODY, g_th.text, 0);
        float bx = PX + PW - 116;
        if (g_pop_hover == id_change) ui_rr(bx, y + 4, 62, 24, 4, g_th.hover);
        ui_txt(L"Change", bx, y + 4, 62, 24, F_SMALL, g_th.text, 1);
        p_hit(bx, y + 4, 62, 24, id_change);
        if (id_none) {
            if (g_pop_hover == id_none) ui_rr(bx + 64, y + 4, 48, 24, 4, g_th.hover);
            ui_txt(L"None", bx + 64, y + 4, 48, 24, F_SMALL, g_th.text, 1);
            p_hit(bx + 64, y + 4, 48, 24, id_none);
        }
    }
    g_py += 40;
}

static int ui_map_changed(void) {
    int n = 0;
    for (int i = 0; i < NUM_VJOY_BUTTONS; i++) if (g_button_map[i] != DEFAULT_BUTTON_MAP[i]) n++;
    return n;
}

static void p_tile(float x, float y, const wchar_t *glyph, const wchar_t *title, const wchar_t *value, int page) {
    int id = P_TILE + page;
    ui_rr(x, y, 160, 58, 4, g_pop_hover == id ? g_th.hover : g_th.fill);
    ui_rr_line(x, y, 160, 58, 4, g_th.subtle, 1);
    ui_txt(glyph, x + 10, y + 7, 20, 22, F_ICON18, g_th.text, 0);
    ui_txt(title, x + 34, y + 7, 122, 22, F_BODY, g_th.text, 0);
    ui_txt(value, x + 12, y + 32, 144, 18, F_SMALL, g_th.sec, 0);
    p_hit(x, y, 160, 58, id);
}

static const wchar_t *ui_exe_name(wchar_t *buf, int n) {
    const char *s = strrchr(g_game_path, '\\');
    s = s ? s + 1 : g_game_path;
    if (!s[0]) return L"Not set";
    swprintf(buf, n, L"%hs", s);
    return buf;
}

static void ui_page_rack(void) {
    wchar_t v[10][64], up[32];
    char hk[32], btn[48];
    format_hotkey(hk, sizeof(hk), g_hotkey_mods, g_hotkey_vk);
    format_button_combo(btn, sizeof(btn), g_toggle_button_mask, 0);
    swprintf(v[0], 64, L"%hs, %hs", hk, btn);
    swprintf(v[1], 64, L"%ls, auto", g_vigem_emulate_ds4 ? L"PlayStation" : L"Xbox");
    if (g_head_mode) swprintf(v[2], 64, L"Head, speed %ld", (long)g_head_speed);
    else if (g_freelook_enabled) swprintf(v[2], 64, L"Stick, speed %ld", (long)g_freelook_mouse_speed);
    else wcscpy(v[2], L"Off");
    if (g_head_camera[0]) swprintf(v[3], 64, L"%hs", g_head_camera);
    else wcscpy(v[3], L"No camera set");
    if (ui_map_changed()) swprintf(v[4], 64, L"%d changed", ui_map_changed());
    else wcscpy(v[4], L"Default");
    swprintf(v[6], 64, L"Deck %ls", g_control_pipe_enabled ? L"on" : L"off");
    swprintf(v[7], 64, L"%ls, checks %ls", g_update_ready ? L"Update ready" : L"" RELAY_VERSION_W, g_update_check ? L"on" : L"off");
    wcscpy(v[8], g_ui_cfg_tray ? L"Starts in the tray" : L"Starts by hand");
    wcscpy(v[9], L"Report a problem");
    float x2 = PX + 168, y = g_py;
    p_tile(PX, y, L"\xE8AB", L"Mode", v[0], PG_MODE);
    p_tile(x2, y, L"\xE7FC", L"Virtual pad", v[1], PG_PAD);
    y += 66;
    p_tile(PX, y, L"\xE890", L"Look", v[2], PG_LOOK);
    p_tile(x2, y, L"\xE722", L"Head tracking", v[3], PG_HEAD);
    y += 66;
    p_tile(PX, y, L"\xE8FD", L"Button map", v[4], PG_MAP);
    p_tile(x2, y, L"\xE768", L"Game", ui_exe_name(up, 32), PG_GAME);
    y += 66;
    p_tile(PX, y, L"\xE7F4", L"Deck and Discord", v[6], PG_DECK);
    p_tile(x2, y, L"\xE895", L"Updates", v[7], PG_UPDATES);
    y += 66;
    p_tile(PX, y, L"\xE713", L"App", v[8], PG_APP);
    p_tile(x2, y, L"\xE897", L"Help", v[9], PG_HELP);
    g_py = y + 70;
    ui_fill(PX, g_py, PW, 1, g_th.subtle);
    g_py += 10;
    p_note(L"Closing the window keeps the relay running in the tray.");
    if (g_quit_ask) {
        p_note(L"Your game loses its controller. Your real controller comes back when the relay closes.");
        p_button_at(PX, g_py, 160, L"Keep running", P_QUIT_NO, 0);
        p_button_at(PX + 168, g_py, 160, L"Quit", P_QUIT_YES, 1);
        g_py += 40;
    } else {
        p_button(L"Quit the relay", P_QUIT, 0);
    }
}

static void ui_page_mode(void) {
    static const wchar_t *const m[] = {L"HOTAS", L"Normal"};
    p_segment(m, 2, g_hidden_mode == 1 ? 0 : g_hidden_mode == 0 ? 1 : -1, P_SEG);
    char hk[32], btn[48];
    wchar_t v[64];
    format_hotkey(hk, sizeof(hk), g_hotkey_mods, g_hotkey_vk);
    format_button_combo(btn, sizeof(btn), g_toggle_button_mask, 0);
    p_label(L"Keyboard shortcut");
    swprintf(v, 64, L"%hs", hk);
    p_field(v, P_HOTKEY, g_toggle_button_mask ? P_HOTKEY_NONE : 0, g_cap == CAP_HOTKEY, CAP_HOTKEY);
    p_note(L"Works anywhere, even in your game.");
    p_label(L"Controller button");
    swprintf(v, 64, L"%hs", btn);
    p_field(v, P_BUTTON, g_hotkey_vk ? P_BUTTON_NONE : 0, g_cap == CAP_BUTTON, CAP_BUTTON);
    p_label(L"Hold to switch");
    static const wchar_t *const h[] = {L"Off", L"0.3 s", L"0.5 s", L"1 s"};
    p_segment(h, 4, g_toggle_hold_ms == 0 ? 0 : g_toggle_hold_ms == 300 ? 1 : g_toggle_hold_ms == 500 ? 2 : g_toggle_hold_ms == 1000 ? 3 : -1, P_SEG + 4);
    p_note(L"Switch mode before launching a game.");
}

static void ui_page_pad(void) {
    static const wchar_t *const m[] = {L"Xbox", L"PlayStation"};
    p_segment(m, 2, g_vigem_emulate_ds4 ? 1 : 0, P_SEG);
    p_note(L"Matches your controller each time it connects. Changing it pulls the pad from a running game for a moment.");
}

static void ui_speed_row(void) {
    int head = g_head_mode != 0;
    long s = head ? g_head_speed : g_freelook_mouse_speed;
    long lo = head ? 1 : 200, hi = head ? 2000 : 40000;
    wchar_t v[24];
    swprintf(v, 24, L"%ld", s);
    p_slider(head ? L"Head speed" : L"Look speed", (float)(log((double)s / lo) / log((double)hi / lo)), v, P_SPEED);
}

static void ui_page_look(void) {
    static const wchar_t *const m[] = {L"Off", L"Stick", L"Head"};
    p_segment(m, 3, g_head_mode ? 2 : g_freelook_enabled ? 1 : 0, P_SEG);
    if (g_freelook_enabled) ui_speed_row();
    if (g_head_mode) {
        p_note(L"Turn your head and the camera follows. Tap RB to recenter. Hold RB to look with the stick. R3 while holding it locks the camera.");
    } else if (g_freelook_enabled) {
        p_note(L"Hold RB and the right stick looks around. Let go and it snaps back. R3 while holding it locks the camera. Tap RB or R3 to unlock.");
    }
    p_note(L"The relay holds Left Alt while looking, which is how the game's free look works. It's always released when you let go.");
}

static void ui_page_head(void) {
    wchar_t v[64];
    if (!ht_files_present()) {
        p_note(L"The head_tracker folder isn't next to the relay. It's in the full download.");
        return;
    }
    p_note(L"Your webcam tracks your head, and the camera in the game follows it. This sends your head movement to the game as mouse movement from software. Anti-cheat that studies mouse input could treat that as suspicious. One player using a head tracker has reported a ban for hacking, and the game studio's CEO said its detection may have seen it as inhuman mouse movement. There's no official rule or outcome yet, so nobody can promise it's safe. Use it at your own risk.");
    if (g_head_mode) ui_speed_row();
    swprintf(v, 64, L"%.2f", g_head_yaw < 0 ? -g_head_yaw : g_head_yaw);
    p_slider(L"Turn", (float)((g_head_yaw < 0 ? -g_head_yaw : g_head_yaw) / 3), v, P_YAW);
    p_toggle(L"Flip turn", g_head_yaw < 0, P_FLIP_Y);
    swprintf(v, 64, L"%.2f", g_head_pitch < 0 ? -g_head_pitch : g_head_pitch);
    p_slider(L"Nod", (float)((g_head_pitch < 0 ? -g_head_pitch : g_head_pitch) / 3), v, P_PITCH);
    p_toggle(L"Flip nod", g_head_pitch < 0, P_FLIP_P);
    swprintf(v, 64, L"%.1f\x00B0", g_head_deadzone);
    p_slider(L"Still zone", (float)(g_head_deadzone / 10), v, P_DEAD);
    p_toggle(L"Show my camera", g_ui_show_camera, P_SHOW_CAM);
    p_note(g_ui_show_camera ? L"Your picture shows in the head ring, in this window only. It isn't stored or sent anywhere."
                            : L"Off: the head ring shows just the dot.");
    p_button(L"Recenter", P_RECENTER, 0);
    p_label(L"Camera");
    if (g_cams_n < 0) {
        p_note(L"Looking for cameras...");
    } else if (g_cams_n == 0) {
        p_note(L"No camera found.");
    } else {
        for (int i = 0; i < g_cams_n; i++) {
            int on = !strcmp(g_cams[i], g_head_camera), id = P_CAM + i;
            if (g_pop_hover == id) ui_rr(PX - 6, g_py - 2, PW + 12, 28, 4, g_th.hover);
            ui_circle(PX + 8, g_py + 12, 7.5f, on ? g_th.thin : g_th.border, 1, NULL);
            if (on) ui_disc(PX + 8, g_py + 12, 4, g_th.thin);
            swprintf(v, 64, L"%hs", g_cams[i]);
            ui_txt(v, PX + 24, g_py, PW - 26, 24, F_BODY, g_th.text, 0);
            p_hit(PX - 6, g_py - 2, PW + 12, 28, id);
            g_py += 28;
        }
    }
    g_py += 6;
    p_label(L"Camera mode");
    static const struct { int w, h, fps; const wchar_t *t; } modes[] = {{640, 480, 60, L"640x480 at 60 fps"}, {640, 480, 30, L"640x480 at 30 fps"}, {1280, 720, 30, L"1280x720 at 30 fps"}};
    int known = 0;
    for (int i = 0; i < 3; i++) {
        int on = g_head_width == modes[i].w && g_head_height == modes[i].h && g_head_fps == modes[i].fps, id = P_CAMMODE + i;
        known |= on;
        if (g_pop_hover == id) ui_rr(PX - 6, g_py - 2, PW + 12, 28, 4, g_th.hover);
        ui_circle(PX + 8, g_py + 12, 7.5f, on ? g_th.thin : g_th.border, 1, NULL);
        if (on) ui_disc(PX + 8, g_py + 12, 4, g_th.thin);
        ui_txt(modes[i].t, PX + 24, g_py, PW - 26, 24, F_BODY, g_th.text, 0);
        p_hit(PX - 6, g_py - 2, PW + 12, 28, id);
        g_py += 28;
    }
    if (!known) {
        swprintf(v, 64, L"%dx%d at %d fps", g_head_width, g_head_height, g_head_fps);
        ui_circle(PX + 8, g_py + 12, 7.5f, g_th.thin, 1, NULL);
        ui_disc(PX + 8, g_py + 12, 4, g_th.thin);
        ui_txt(v, PX + 24, g_py, PW - 26, 24, F_BODY, g_th.text, 0);
        g_py += 28;
    }
    p_note(L"Applies when the camera next opens.");
}

static void ui_page_map(void) {
    wchar_t v[96];
    if (g_cap == CAP_MAP1) {
        swprintf(v, 96, L"Press and hold the button to change. Esc cancels.  %d s", (int)((g_cap_end - GetTickCount()) / 1000) + 1);
        p_note(v);
    } else if (g_cap == CAP_MAP2) {
        swprintf(v, 96, L"%hs: press and hold the button whose job it takes.  %d s", label_for_phys(g_cap_first, g_backend), (int)((g_cap_end - GetTickCount()) / 1000) + 1);
        p_note(v);
    } else {
        p_note(L"Picks two buttons and swaps their jobs, so nothing gets lost. Your game keeps getting input the whole time.");
        p_button(L"Change a button", P_MAP, 1);
    }
    if (g_pop_msg[0]) p_note(g_pop_msg);
    p_label(L"Joystick buttons");
    for (int i = 0; i < NUM_VJOY_BUTTONS; i++) {
        swprintf(v, 96, L"%d", i + 1);
        ui_txt(v, PX, g_py, 40, 18, F_SMALL, g_th.sec, 0);
        swprintf(v, 96, L"%hs", label_for_phys(g_button_map[i], g_backend));
        ui_txt(v, PX + 40, g_py, PW - 40, 18, F_SMALL, g_button_map[i] != DEFAULT_BUTTON_MAP[i] ? g_th.thin : g_th.text, 0);
        g_py += 18;
    }
    g_py += 10;
    p_button(L"Reset to default", P_MAP_RESET, ui_map_changed() ? 0 : 2);
}

static void ui_page_game(void) {
    wchar_t v[MAX_PATH + 8], nm[64];
    p_info(L"Game", ui_exe_name(nm, 64));
    swprintf(v, MAX_PATH + 8, L"%hs", g_game_path[0] ? g_game_path : "Not set");
    p_note(v);
    p_button(L"Change", P_GAME_CHANGE, 0);
    p_button(g_ui_game_up ? L"Game running" : L"Launch", P_GAME_LAUNCH, g_game_path[0] && !g_ui_game_up && g_relay_state == RELAY_STATE_CONNECTED ? 1 : 2);
    p_note(L"Head tracking runs in this game, and starts when it does.");
}

static void ui_page_deck(void) {
    p_toggle(L"Stream Deck control", g_control_pipe_enabled, P_PIPE);
    p_note(L"Applies next time the relay starts.");
    p_label(L"Discord button");
    wchar_t v[64];
    char btn[48];
    format_button_combo(btn, sizeof(btn), g_vjoy_passthrough, 0);
    swprintf(v, 64, L"%hs", g_vjoy_passthrough ? btn : "None");
    p_field(v, P_DISCORD, g_vjoy_passthrough ? P_DISCORD_NONE : 0, g_cap == CAP_DISCORD, CAP_DISCORD);
    if (g_vjoy_passthrough & g_toggle_button_mask) p_note(L"This shares a button with the mode switch, so switching can press it too.");
    p_note(L"Presses a joystick button in Normal mode, for a mute key in Discord.");
}

static void ui_page_updates(void) {
    wchar_t v[96];
    p_info(L"You have", L"" RELAY_VERSION_W);
    if (g_update_ready) {
        swprintf(v, 96, L"%hs", g_update_tag);
        p_info(L"Available", v);
    }
    p_toggle(L"Check for updates", g_update_check, P_UPDCHECK);
    p_note(L"Only asks GitHub, once when the relay starts. It never updates without you.");
    if (g_update_stage == 1) p_note(L"Downloading and testing the new version...");
    else if (g_update_stage == 2) {
        p_note(L"Updated. Restart the relay to use it.");
        p_button(L"Close the relay", P_UPDATE_RESTART, 1);
    } else if (g_update_stage == 3) {
        swprintf(v, 96, L"Nothing was changed: %hs", g_update_problem);
        p_note(v);
        p_button(L"Get it from GitHub", P_RELEASES, 0);
    } else if (g_update_ready) {
        if (g_ui_game_up) p_note(L"Your game is running. Installing closes the relay, and your game loses its controller.");
        p_button(L"Install now", P_UPDATE_NOW, 1);
    }
}

static void ui_page_app(void) {
    p_toggle(L"Start in the tray", g_ui_cfg_tray, P_TRAY);
    p_note(L"Starts with no window. Click the tray icon to open it.");
    p_toggle(L"Show console", g_console_on, P_CONSOLE);
    p_note(L"The real 1.x console: the same screen and keys, alongside this window.");
    p_toggle(L"Start in console mode", g_ui_cfg_console, P_CONSOLE_MODE);
    p_note(L"Next start: console only, no window, no tray, exactly how 1.x worked. Start it with --window to come back.");
}

static wchar_t g_report_text[3400];

static void ui_page_report(void) {
    p_note(L"Say what happened on GitHub, with the details below already filled in. Nothing is sent until you press Submit there.");
    p_note(L"You need a free GitHub account. If you aren't signed in, it asks you to sign in or make one first, then brings you back. The whole report is copied too, so if the box comes up empty, click in it and press Ctrl+V.");
    p_button(L"Copy it and open GitHub", P_REPORT_OPEN, 1);
    p_button(L"Just copy it", P_REPORT_COPY, 0);
    if (g_pop_msg[0]) p_note(g_pop_msg);
    p_label(L"What's in it");
    p_note(g_report_text);
    p_note(L"Your user name is taken out of file paths. You can change anything before you press Submit.");
}

static void ui_page_help(void) {
    p_button(L"Report a problem", P_REPORT, 1);
    p_button_at(PX, g_py, 160, L"Open log", P_OPENLOG, 0);
    p_button_at(PX + 168, g_py, 160, L"Open settings file", P_OPENINI, 0);
    g_py += 40;
    p_label(L"In flight");
    p_note(L"Hold RB to look, R3 while holding locks the camera, tap RB to unlock. In head mode, tap RB to recenter.");
    p_label(L"If something goes wrong");
    p_note(L"Controller stuck hidden after a crash: run RuthlessControllerRelay.exe --unhide. To remove everything: run it with --uninstall.");
    p_info(L"Version", L"" RELAY_VERSION_W);
}

static void ui_page_link(void) {
    wchar_t v[128];
    int live = g_relay_state == RELAY_STATE_CONNECTED;
    swprintf(v, 128, L"%hs", live ? describe_controller(g_backend) : "None");
    p_note(v);
    p_info(L"Hidden", !live ? L"-" : g_hidhide_registered ? L"Yes" : L"No");
    p_info(L"Battery", g_w.batt[0] ? g_w.batt : L"-");
    p_info(L"State", g_w.k_word);
    p_note(g_w.k_sub);
    p_label(L"Sticks doing nothing?");
    p_note(L"The top line should name your real controller. If it says generic DirectInput and you don't own a joystick, close the relay, turn your controller on, and start it again.");
    p_button(L"Report a problem", P_REPORT, 0);
}

static void ui_pop_size_and_place(void);

static void ui_pop_paint(void) {
    if (!g_pop.hwnd) return;
    if (!g_pop.rt) {
        RECT r;
        GetClientRect(g_pop.hwnd, &r);
        D2D1_RENDER_TARGET_PROPERTIES p = {0};
        p.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        p.dpiX = p.dpiY = g_pop.dpi;
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = {0};
        hp.hwnd = g_pop.hwnd;
        hp.pixelSize.width = (UINT)(r.right - r.left);
        hp.pixelSize.height = (UINT)(r.bottom - r.top);
        if (FAILED(ID2D1Factory_CreateHwndRenderTarget(g_d2d, &p, &hp, &g_pop.rt))) return;
        D2D1_COLOR_F white = {1, 1, 1, 1};
        ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget *)g_pop.rt, &white, NULL, &g_pop.br);
        g_pop.px_w = hp.pixelSize.width;
        g_pop.px_h = hp.pixelSize.height;
    }
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_pop.rt;
    ID2D1RenderTarget *save_t = g_t;
    ID2D1SolidColorBrush *save_b = g_b;
    g_t = rt;
    g_b = g_pop.br;
    ID2D1RenderTarget_BeginDraw(rt);
    ID2D1RenderTarget_SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ID2D1RenderTarget_Clear(rt, &g_th.paper);
    D2D1_MATRIX_3X2_F m = {1, 0, 0, 1, 0, -g_pop_scroll};
    ID2D1RenderTarget_SetTransform(rt, &m);
    g_pop_n = 0;
    g_py = 14;
    switch (g_page) {
        case PG_RACK: p_title(L"Settings"); ui_page_rack(); break;
        case PG_MODE: p_title(L"Mode"); ui_page_mode(); break;
        case PG_PAD: p_title(L"Virtual pad"); ui_page_pad(); break;
        case PG_LOOK: p_title(L"Look"); ui_page_look(); break;
        case PG_HEAD: p_title(L"Head tracking"); ui_page_head(); break;
        case PG_MAP: p_title(L"Button map"); ui_page_map(); break;
        case PG_GAME: p_title(L"Game"); ui_page_game(); break;
        case PG_DECK: p_title(L"Deck and Discord"); ui_page_deck(); break;
        case PG_UPDATES: p_title(L"Updates"); ui_page_updates(); break;
        case PG_APP: p_title(L"App"); ui_page_app(); break;
        case PG_HELP: p_title(L"Help"); ui_page_help(); break;
        case PG_LINK: p_title(L"Link"); ui_page_link(); break;
        case PG_REPORT: p_title(L"Report a problem"); ui_page_report(); break;
        default: break;
    }
    float content = g_py + 8;
    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        ui_release(&g_pop.br);
        ui_release(&g_pop.rt);
    }
    g_t = save_t;
    g_b = save_b;
    g_pop_dirty = 0;
    if (fabsf(content - g_pop_h) > .5f) {
        g_pop_h = content;
        ui_pop_size_and_place();
        g_pop_dirty = 1;
    }
}

/* Bottom flush with the window, on the side with room. It never covers what it changes. */
static void ui_pop_size_and_place(void) {
    if (!g_pop.hwnd) return;
    RECT w;
    GetWindowRect(g_main.hwnd, &w);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_main.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    float k = g_pop.dpi / 96.f;
    int pw = (int)(360 * k), gap = (int)(8 * k);
    g_pop_max_h = (float)(mi.rcWork.bottom - mi.rcWork.top - 2 * gap) / k;
    int ph = (int)((g_pop_h < g_pop_max_h ? g_pop_h : g_pop_max_h) * k);
    int x = w.right + gap;
    if (x + pw > mi.rcWork.right) {
        x = w.left - gap - pw;
        if (x < mi.rcWork.left) x = w.right - pw - gap;
    }
    int y = w.bottom - ph;
    if (y + ph > mi.rcWork.bottom - gap) y = mi.rcWork.bottom - gap - ph;
    if (y < mi.rcWork.top + gap) y = mi.rcWork.top + gap;
    SetWindowPos(g_pop.hwnd, NULL, x, y, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_pop.rt) {
        D2D1_SIZE_U s = {(UINT32)pw, (UINT32)ph};
        ID2D1HwndRenderTarget_Resize(g_pop.rt, &s);
    }
}

static DWORD WINAPI ui_list_cams_thread(LPVOID a) {
    (void)a;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int n = ht_list_cameras(g_cams, 16);
    InterlockedExchange(&g_cams_n, n);
    g_pop_dirty = 1;
    CoUninitialize();
    return 0;
}

static void ui_pop_go(int page) {
    g_page = page;
    g_pop_scroll = 0;
    g_quit_ask = 0;
    g_pop_msg[0] = 0;
    g_pop_hover = P_NONE;
    g_pop_dirty = 1;
    if (page == PG_REPORT) {
        char head[800], log[3000], all[3400];
        ui_report_head(head, sizeof(head));
        ui_log_tail(g_ui_prev_crash ? UI_PREV_LOG_FILE : INSTALL_LOG_FILE, log, sizeof(log), 1200);
        snprintf(all, sizeof(all), "%s\nLast lines of the log:\n%s", head, log);
        MultiByteToWideChar(CP_ACP, 0, all, -1, g_report_text, 3400);
        g_report_text[3399] = 0;
        InterlockedExchange(&g_ui_had_crash, 0); /* the footer's nudge has done its job */
        g_bake_dirty = 1;
    }
    if (page == PG_HEAD) {
        InterlockedExchange(&g_cams_n, -1);
        HANDLE t = CreateThread(NULL, 0, ui_list_cams_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
}

static void ui_cap_cancel(void) {
    if (g_cap == CAP_BUTTON) g_toggle_button_mask = g_cap_saved;
    if (g_cap == CAP_DISCORD) g_vjoy_passthrough = (WORD)g_cap_saved;
    g_cap = CAP_NONE;
    g_pop_dirty = 1;
}

static void ui_pop_close(void) {
    if (!g_pop.hwnd) return;
    if (g_cap) ui_cap_cancel();
    if (g_save_at) { /* a slider's save was still waiting */
        g_save_at = 0;
        g_ui_cfg_dirty = 1;
    }
    HWND h = g_pop.hwnd;
    g_pop.hwnd = NULL;
    ui_release(&g_pop.br);
    ui_release(&g_pop.rt);
    g_page = PG_NONE;
    DestroyWindow(h);
}

static void ui_save_soon(void) { g_save_at = GetTickCount() + 500; }

static void ui_slider_set(int id, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    if (id == P_SPEED) {
        int head = g_head_mode != 0;
        double lo = head ? 1 : 200, hi = head ? 2000 : 40000;
        LONG s = (LONG)(lo * pow(hi / lo, t) + .5);
        InterlockedExchange(head ? &g_head_speed : &g_freelook_mouse_speed, s);
    } else if (id == P_YAW) {
        double v = round(t * 60) / 20;
        g_head_yaw = g_head_yaw < 0 ? -v : v;
    } else if (id == P_PITCH) {
        double v = round(t * 60) / 20;
        g_head_pitch = g_head_pitch < 0 ? -v : v;
    } else if (id == P_DEAD) {
        g_head_deadzone = round(t * 100) / 10;
    }
    ui_save_soon();
    g_pop_dirty = 1;
}

/* Everything the pad is holding right now, in the relay's own button numbers. */
static DWORD ui_pad_held(void) {
    XINPUT_GAMEPAD gp = lastGamepad;
    DWORD m = gp.wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER |
                             XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_DPAD_UP |
                             XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
    if (gp.wButtons & XINPUT_GAMEPAD_GUIDE) m |= PHYS_GUIDE_BUTTON;
    if (gp.bLeftTrigger > TRIGGER_CLICK_THRESHOLD) m |= PHYS_TRIGGER_L;
    if (gp.bRightTrigger > TRIGGER_CLICK_THRESHOLD) m |= PHYS_TRIGGER_R;
    if (g_ps_pressed) m |= PHYS_PS_BUTTON;
    if (g_touchpad_pressed) m |= PHYS_TOUCHPAD;
    return m;
}

static int ui_one_bit(DWORD m) { return m && !(m & (m - 1)); }

/* While a capture waits. The buttons still reach the game: nothing here touches the input loop. */
static void ui_cap_tick(void) {
    static int prev_sec;
    if (!g_cap || g_cap == CAP_HOTKEY) {
        if (g_cap == CAP_HOTKEY && (int)(g_cap_end - GetTickCount()) <= 0) ui_cap_cancel();
        if (g_cap == CAP_HOTKEY && (int)(g_cap_end - GetTickCount()) / 1000 != prev_sec) { prev_sec = (int)(g_cap_end - GetTickCount()) / 1000; g_pop_dirty = 1; }
        return;
    }
    DWORD now = GetTickCount();
    if ((int)(now - g_cap_end) >= 0) {
        ui_cap_cancel();
        return;
    }
    if ((int)(g_cap_end - now) / 1000 != prev_sec) {
        prev_sec = (int)(g_cap_end - now) / 1000;
        g_pop_dirty = 1;
    }
    if (g_relay_state != RELAY_STATE_CONNECTED) return;
    /* What's pressed in the 300 ms after the first press is the answer, so a tap counts and two buttons can go together.
       More than two means the pad was busy, not choosing: wait for the next press. In MAP2 the first button must be
       let go first, or the same press would answer both questions. */
    DWORD held = ui_pad_held();
    if (!g_cap_accum) {
        if (g_cap_wait_release) {
            if (held) return;
            g_cap_wait_release = 0;
        }
        if (!held) return;
        g_cap_since = now;
    }
    g_cap_accum |= held;
    if (now - g_cap_since < 300) return;
    DWORD got = g_cap_accum;
    g_cap_accum = 0;
    int bits = 0;
    for (DWORD b = got; b; b &= b - 1) bits++;
    if (bits > 2) return;
    g_cap_wait_release = 1;
    if (g_cap == CAP_BUTTON) {
        g_toggle_button_mask = got;
        g_cap = CAP_NONE;
        save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    } else if (g_cap == CAP_DISCORD) {
        g_vjoy_passthrough = (WORD)(got & 0xFFFFu);
        g_cap = CAP_NONE;
        save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    } else if (g_cap == CAP_MAP1) {
        if (ui_one_bit(got)) {
            g_cap_first = got;
            g_cap = CAP_MAP2;
            g_cap_end = now + 10000;
        }
    } else if (g_cap == CAP_MAP2) {
        if (ui_one_bit(got) && got != g_cap_first) {
            int a = -1, b = -1;
            for (int i = 0; i < NUM_VJOY_BUTTONS; i++) {
                if (g_button_map[i] == g_cap_first) a = i;
                if (g_button_map[i] == got) b = i;
            }
            if (a >= 0 && b >= 0) { /* swap, don't duplicate -- both ends stay assigned */
                g_button_map[a] = got;
                g_button_map[b] = g_cap_first;
                swprintf(g_pop_msg, 160, L"%hs now acts like %hs, and the other way round.", label_for_phys(g_cap_first, g_backend), label_for_phys(got, g_backend));
                save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
            }
            g_cap = CAP_NONE;
        }
    }
    g_pop_dirty = 1;
}

static DWORD WINAPI ui_update_thread(LPVOID a) {
    (void)a;
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = '\0';
    const char *problem = update_install(g_update_tag, dir);
    if (problem) {
        strncpy(g_update_problem, problem, sizeof(g_update_problem) - 1);
        log_line("update: %s", problem);
        InterlockedExchange(&g_update_stage, 3);
    } else {
        log_line("update: installed %s", g_update_tag);
        InterlockedExchange(&g_update_stage, 2);
    }
    g_pop_dirty = 1;
    return 0;
}

static DWORD WINAPI ui_head_cycle_thread(LPVOID a) {
    (void)a; /* g_head_camera is only read when the camera opens, so a change has to close and reopen it */
    InterlockedExchange(&g_ipc_fl_want, g_freelook_enabled ? 1 : 0);
    Sleep(600);
    InterlockedExchange(&g_ipc_fl_want, 2);
    return 0;
}

static void ui_pop_action(int id) {
    if (id >= P_TILE && id < P_TILE + 20) { ui_pop_go(id - P_TILE); return; }
    if (id == P_BACK) { ui_pop_go(PG_RACK); return; }
    if (g_page == PG_MODE && id >= P_SEG && id < P_SEG + 2) { ipc_set_mode(id == P_SEG ? IPC_CMD_MODE_HOTAS : IPC_CMD_MODE_NORMAL); g_pop_dirty = 1; return; }
    if (g_page == PG_MODE && id >= P_SEG + 4 && id < P_SEG + 8) {
        static const DWORD holds[4] = {0, 300, 500, 1000};
        g_toggle_hold_ms = holds[id - P_SEG - 4];
        ui_save_soon();
        g_pop_dirty = 1;
        return;
    }
    if (g_page == PG_PAD && id >= P_SEG && id < P_SEG + 2) { InterlockedExchange(&g_ipc_pad_want, id - P_SEG); g_pop_dirty = 1; return; }
    if (g_page == PG_LOOK && id >= P_SEG && id < P_SEG + 3) {
        int v = id - P_SEG;
        InterlockedExchange(&g_ipc_fl_want, v);
        g_ui_cfg_dirty = 1;
        if (v == 2 && (!g_head_camera[0] || !g_game_path[0])) ui_pop_go(PG_HEAD); /* head with nothing set up goes straight to what's missing */
        g_pop_dirty = 1;
        return;
    }
    if (id >= P_CAM && id < P_CAM + 16) {
        strncpy(g_head_camera, g_cams[id - P_CAM], sizeof(g_head_camera) - 1);
        ui_save_soon();
        if (g_head_mode) { HANDLE t = CreateThread(NULL, 0, ui_head_cycle_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
        g_pop_dirty = 1;
        return;
    }
    if (id >= P_CAMMODE && id < P_CAMMODE + 3) {
        static const int modes[3][3] = {{640, 480, 60}, {640, 480, 30}, {1280, 720, 30}};
        g_head_width = modes[id - P_CAMMODE][0];
        g_head_height = modes[id - P_CAMMODE][1];
        g_head_fps = modes[id - P_CAMMODE][2];
        ui_save_soon();
        g_pop_dirty = 1;
        return;
    }
    char path[MAX_PATH + 32], dir[MAX_PATH];
    switch (id) {
        case P_QUIT:
            if (g_ui_game_up) { g_quit_ask = 1; g_pop_dirty = 1; }
            else ui_quit();
            break;
        case P_QUIT_NO: g_quit_ask = 0; g_pop_dirty = 1; break;
        case P_QUIT_YES: ui_quit(); break;
        case P_HOTKEY:
            g_cap = CAP_HOTKEY;
            g_cap_end = GetTickCount() + 10000;
            g_pop_dirty = 1;
            SetFocus(g_pop.hwnd);
            break;
        case P_HOTKEY_NONE:
            g_hotkey_vk = 0;
            g_hotkey_mods = 0;
            if (g_toggle_thread_id) PostThreadMessageA(g_toggle_thread_id, WM_APP_REMAP, 0, 0);
            ui_save_soon();
            g_pop_dirty = 1;
            break;
        case P_BUTTON:
            g_cap_wait_release = 1;
            g_cap_saved = g_toggle_button_mask;
            g_toggle_button_mask = 0; /* so the old button can't switch modes during the capture */
            g_cap_accum = 0;
            g_cap = CAP_BUTTON;
            g_cap_end = GetTickCount() + 10000;
            g_pop_dirty = 1;
            break;
        case P_BUTTON_NONE: g_toggle_button_mask = 0; ui_save_soon(); g_pop_dirty = 1; break;
        case P_FLIP_Y: g_head_yaw = -g_head_yaw; ui_save_soon(); g_pop_dirty = 1; break;
        case P_FLIP_P: g_head_pitch = -g_head_pitch; ui_save_soon(); g_pop_dirty = 1; break;
        case P_SHOW_CAM:
            g_ui_show_camera = !g_ui_show_camera;
            ui_cam_sync();
            ui_save_soon();
            g_pop_dirty = 1;
            break;
        case P_RECENTER: InterlockedExchange(&g_head_recenter, 1); break;
        case P_MAP:
            g_cap_wait_release = 1;
            g_cap = CAP_MAP1;
            g_cap_accum = 0;
            g_cap_end = GetTickCount() + 10000;
            g_pop_msg[0] = 0;
            g_pop_dirty = 1;
            break;
        case P_MAP_RESET:
            memcpy(g_button_map, DEFAULT_BUTTON_MAP, sizeof(g_button_map));
            ui_save_soon();
            g_pop_dirty = 1;
            break;
        case P_GAME_CHANGE:
        case P_GAME_LAUNCH: {
            if (id == P_GAME_CHANGE) g_game_path[0] = g_game_path[0]; /* the worker picks by whether a path is set */
            HANDLE t = CreateThread(NULL, 0, id == P_GAME_CHANGE ? ui_pick_game_thread : ui_launch_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            break;
        }
        case P_PIPE: InterlockedExchange(&g_control_pipe_enabled, !g_control_pipe_enabled); ui_save_soon(); g_pop_dirty = 1; break;
        case P_DISCORD:
            g_cap_wait_release = 1;
            g_cap_saved = g_vjoy_passthrough;
            g_vjoy_passthrough = 0;
            g_cap_accum = 0;
            g_cap = CAP_DISCORD;
            g_cap_end = GetTickCount() + 10000;
            g_pop_dirty = 1;
            break;
        case P_DISCORD_NONE: g_vjoy_passthrough = 0; ui_save_soon(); g_pop_dirty = 1; break;
        case P_UPDCHECK:
            InterlockedExchange(&g_update_check, !g_update_check);
            if (g_update_check) update_start_thread();
            ui_save_soon();
            g_pop_dirty = 1;
            break;
        case P_UPDATE_NOW: {
            InterlockedExchange(&g_update_stage, 1);
            HANDLE t = CreateThread(NULL, 0, ui_update_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            g_pop_dirty = 1;
            break;
        }
        case P_UPDATE_RESTART: ui_quit(); break;
        case P_RELEASES: ui_open_url(UI_REPO_URL "/releases"); break;
        case P_TRAY: g_ui_cfg_tray = !g_ui_cfg_tray; ui_save_soon(); g_pop_dirty = 1; break;
        case P_CONSOLE:
            if (g_console_on) ui_console_hide();
            else ui_console_show();
            g_pop_dirty = 1;
            break;
        case P_CONSOLE_MODE: g_ui_cfg_console = !g_ui_cfg_console; ui_save_soon(); g_pop_dirty = 1; break;
        case P_REPORT: ui_pop_go(PG_REPORT); break;
        case P_REPORT_OPEN:
            ui_report_send(1);
            wcscpy(g_pop_msg, L"Copied. GitHub is opening in your browser.");
            g_pop_dirty = 1;
            break;
        case P_REPORT_COPY:
            ui_report_send(0);
            wcscpy(g_pop_msg, L"Copied. Paste it wherever you need it.");
            g_pop_dirty = 1;
            break;
        case P_OPENLOG:
            get_exe_dir(dir, sizeof(dir));
            snprintf(path, sizeof(path), "%s%s", dir, INSTALL_LOG_FILE);
            ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
            break;
        case P_OPENINI:
            get_config_path(path, sizeof(path));
            ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
            break;
        default: break;
    }
}

static int ui_pop_hit_at(float x, float y) {
    y += g_pop_scroll;
    for (int i = g_pop_n - 1; i >= 0; i--) {
        ui_hit_t *h = &g_pop_hits[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return h->id;
    }
    return P_NONE;
}

static void ui_slider_drag(LPARAM lp) {
    float x = (short)LOWORD(lp) * 96.f / g_pop.dpi;
    ui_slider_set(g_drag, (x - (PX + 10)) / (PW - 20));
}

static LRESULT CALLBACK ui_pop_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_MOUSEMOVE: {
            if (g_drag) {
                ui_slider_drag(lp);
                return 0;
            }
            int id = ui_pop_hit_at((short)LOWORD(lp) * 96.f / g_pop.dpi, (short)HIWORD(lp) * 96.f / g_pop.dpi);
            if (id != g_pop_hover) {
                g_pop_hover = id;
                g_pop_dirty = 1;
            }
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_pop_hover) {
                g_pop_hover = P_NONE;
                g_pop_dirty = 1;
            }
            return 0;
        case WM_LBUTTONDOWN: {
            int id = ui_pop_hit_at((short)LOWORD(lp) * 96.f / g_pop.dpi, (short)HIWORD(lp) * 96.f / g_pop.dpi);
            g_pop_down = id;
            if (id == P_SPEED || id == P_YAW || id == P_PITCH || id == P_DEAD) {
                g_drag = id;
                SetCapture(h);
                ui_slider_drag(lp);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_drag) {
                g_drag = P_NONE;
                ReleaseCapture();
                return 0;
            }
            int id = ui_pop_hit_at((short)LOWORD(lp) * 96.f / g_pop.dpi, (short)HIWORD(lp) * 96.f / g_pop.dpi);
            if (id && id == g_pop_down) ui_pop_action(id);
            g_pop_down = P_NONE;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            float room = g_pop_h - g_pop_max_h;
            if (room > 0) {
                g_pop_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48;
                g_pop_scroll = g_pop_scroll < 0 ? 0 : g_pop_scroll > room ? room : g_pop_scroll;
                g_pop_dirty = 1;
            }
            return 0;
        }
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            UINT vk = (UINT)wp;
            if (vk == VK_ESCAPE) {
                if (g_cap) ui_cap_cancel();
                else if (g_page != PG_RACK) ui_pop_go(PG_RACK);
                else ui_pop_close();
                return 0;
            }
            if (g_cap == CAP_HOTKEY && vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN) {
                UINT mods = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if ((GetKeyState(VK_MENU) & 0x8000) && !g_freelook_alt_held) mods |= MOD_ALT; /* the relay's own held Alt isn't yours */
                if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                int plain = (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_F1 && vk <= VK_F12);
                if (plain && (mods || (vk >= VK_F1 && vk <= VK_F12))) {
                    g_hotkey_mods = mods;
                    g_hotkey_vk = vk;
                    g_cap = CAP_NONE;
                    if (g_toggle_thread_id) PostThreadMessageA(g_toggle_thread_id, WM_APP_REMAP, 0, 0);
                    ui_save_soon();
                    g_pop_dirty = 1;
                }
                return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            /* Clicking away closes it, except while it's waiting for you, or a file dialog is up. */
            if (LOWORD(wp) == WA_INACTIVE && !g_cap && !g_ui_dialog_open && g_pop.hwnd == h) PostMessageW(g_main.hwnd, WM_APP + 20, 0, 0);
            break;
        case WM_PAINT:
            ValidateRect(h, NULL);
            ui_pop_paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void ui_pop_open(int page) {
    if (g_pop.hwnd) {
        if (g_page == page) ui_pop_close();
        else ui_pop_go(page);
        return;
    }
    static int registered = 0;
    HINSTANCE hi = GetModuleHandleA(NULL);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = ui_pop_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"RuthlessRelayPopup";
        RegisterClassExW(&wc);
        registered = 1;
    }
    memset(&g_pop, 0, sizeof(g_pop));
    g_pop.dpi = g_main.dpi;
    g_pop_h = 300;
    g_pop.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"RuthlessRelayPopup", L"Settings", WS_POPUP, 0, 0, (int)(360 * g_main.dpi / 96), (int)(300 * g_main.dpi / 96), g_main.hwnd, NULL, hi, NULL);
    if (!g_pop.hwnd) return;
    int round_corners = 2; /* DWMWCP_ROUND */
    g_dwm_attr(g_pop.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round_corners, sizeof(round_corners));
    BOOL dark = g_th.dark;
    g_dwm_attr(g_pop.hwnd, 20, &dark, sizeof(dark));
    MARGINS mg = {1, 1, 1, 1}; /* gives it the shadow */
    g_dwm_extend(g_pop.hwnd, &mg);
    ui_pop_go(page);
    ui_pop_size_and_place();
    ui_pop_paint();
    if (g_pop_dirty) ui_pop_paint();
    ShowWindow(g_pop.hwnd, SW_SHOW);
    SetForegroundWindow(g_pop.hwnd); /* opening settings takes focus on purpose: that's the moment you aren't flying */
    SetFocus(g_pop.hwnd);
}

static void ui_pop_theme_changed(void) {
    if (!g_pop.hwnd) return;
    BOOL dark = g_th.dark;
    g_dwm_attr(g_pop.hwnd, 20, &dark, sizeof(dark));
    g_pop_dirty = 1;
}

static void ui_pop_tick(void) {
    if (!g_pop.hwnd) return;
    ui_cap_tick();
    if (g_save_at && (int)(GetTickCount() - g_save_at) >= 0) {
        g_save_at = 0;
        g_ui_cfg_dirty = 1;
    }
    static LONG last_mode, last_fl, last_head, last_pad;
    if (last_mode != g_hidden_mode || last_fl != g_freelook_enabled || last_head != g_head_mode || last_pad != g_vigem_emulate_ds4) {
        last_mode = g_hidden_mode;
        last_fl = g_freelook_enabled;
        last_head = g_head_mode;
        last_pad = g_vigem_emulate_ds4;
        g_pop_dirty = 1;
    }
    if (g_pop_dirty) ui_pop_paint();
}


static void ui_action(int id) {
    switch (id) {
        case H_TAB_TELE: g_tab = 0; g_bake_dirty = 1; break;
        case H_TAB_DETAILS: g_tab = 1; g_bake_dirty = 1; break;
        case H_TO_OTHER: ipc_set_mode(g_hidden_mode == 1 ? IPC_CMD_MODE_NORMAL : IPC_CMD_MODE_HOTAS); break;
        case H_TO_HOTAS: ipc_set_mode(IPC_CMD_MODE_HOTAS); break;
        case H_TO_NORMAL: ipc_set_mode(IPC_CMD_MODE_NORMAL); break;
        case H_LAUNCH: {
            HANDLE t = CreateThread(NULL, 0, ui_launch_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            break;
        }
        case H_REPORT: ui_pop_open(PG_REPORT); break;
        case H_GEAR: ui_pop_open(PG_RACK); break;
        case H_BOX_MODE: ui_pop_open(PG_MODE); break;
        case H_BOX_LOOK: ui_pop_open(g_head_mode ? PG_HEAD : PG_LOOK); break;
        case H_BOX_LINK: ui_pop_open(PG_LINK); break;
        case H_UPDATE: ui_pop_open(PG_UPDATES); break;
        default: break;
    }
}

/* ---------- The main window ---------- */

static void ui_mouse_to_dip(LPARAM lp, float *x, float *y) {
    *x = (short)LOWORD(lp) * 96.f / g_main.dpi;
    *y = (short)HIWORD(lp) * 96.f / g_main.dpi;
}

static void ui_set_hover(int id) {
    if (id != g_hover) {
        g_hover = id;
        g_bake_dirty = 1;
    }
}

static LRESULT CALLBACK ui_main_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_APP + 20) { /* the popup lost focus: closed from here, not from inside its own activation */
        if (GetForegroundWindow() != g_pop.hwnd) ui_pop_close();
        return 0;
    }
    switch (m) {
        case WM_NCCALCSIZE:
            if (wp) {
                if (IsZoomed(h)) { /* a maximized window hangs past the monitor by its frame */
                    NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                    UINT dpi = GetDpiForWindow(h);
                    int fx = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    int fy = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    p->rgrc[0].left += fx;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            RECT w;
            GetWindowRect(h, &w);
            int edge = (int)(6 * g_main.dpi / 96);
            if (!IsZoomed(h)) {
                int l = pt.x < w.left + edge, r = pt.x >= w.right - edge, t = pt.y < w.top + edge, b = pt.y >= w.bottom - edge;
                if (t && l) return HTTOPLEFT;
                if (t && r) return HTTOPRIGHT;
                if (b && l) return HTBOTTOMLEFT;
                if (b && r) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (b) return HTBOTTOM;
            }
            ScreenToClient(h, &pt);
            float x = pt.x * 96.f / g_main.dpi, y = pt.y * 96.f / g_main.dpi;
            int id = ui_hit_at(x, y);
            if (id == H_MIN) return HTMINBUTTON;
            if (id == H_MAX) return HTMAXBUTTON; /* what brings up the Snap Layouts flyout */
            if (id == H_CLOSE) return HTCLOSE;
            if (id == H_NONE && y < UI_CAP) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCMOUSEMOVE: {
            int id = wp == HTMINBUTTON ? H_MIN : wp == HTMAXBUTTON ? H_MAX : wp == HTCLOSE ? H_CLOSE : H_NONE;
            ui_set_hover(id);
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE | TME_NONCLIENT, h, 0};
            TrackMouseEvent(&t);
            break;
        }
        case WM_NCMOUSELEAVE:
        case WM_MOUSELEAVE:
            ui_set_hover(H_NONE);
            return 0;
        case WM_NCLBUTTONDOWN:
            if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) return 0; /* acted on at button up */
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMINBUTTON) { ShowWindow(h, SW_MINIMIZE); return 0; }
            if (wp == HTMAXBUTTON) { ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            if (wp == HTCLOSE) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
            break;
        case WM_MOUSEMOVE: {
            float x, y;
            ui_mouse_to_dip(lp, &x, &y);
            ui_set_hover(ui_hit_at(x, y));
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        /* A click on the panel doesn't take focus from the game. Dragging the caption still does. */
        case WM_MOUSEACTIVATE:
            if (LOWORD(lp) == HTCLIENT) return MA_NOACTIVATE;
            break;
        case WM_LBUTTONDOWN: {
            float x, y;
            ui_mouse_to_dip(lp, &x, &y);
            g_down = ui_hit_at(x, y);
            if (g_down == H_NONE && g_pop.hwnd && !g_cap) ui_pop_close(); /* clicking empty panel closes it */
            return 0;
        }
        case WM_LBUTTONUP: {
            float x, y;
            ui_mouse_to_dip(lp, &x, &y);
            int id = ui_hit_at(x, y);
            if (id && id == g_down) ui_action(id);
            g_down = H_NONE;
            return 0;
        }
        case WM_TIMER:
            ui_tick();
            return 0;
        case WM_PAINT:
            ValidateRect(h, NULL);
            ui_paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            g_bake_dirty = 1;
            if (wp != SIZE_MINIMIZED) ui_paint();
            return 0;
        case WM_DPICHANGED: {
            g_main.dpi = (float)HIWORD(wp);
            if (g_main.rt) ID2D1RenderTarget_SetDpi((ID2D1RenderTarget *)g_main.rt, g_main.dpi, g_main.dpi);
            RECT *r = (RECT *)lp;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ui_release(&g_bake);
            ui_release(&g_bake_rt);
            g_bake_dirty = 1;
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            UINT dpi = GetDpiForWindow(h);
            mm->ptMinTrackSize.x = MulDiv(420, (int)dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(286, (int)dpi, 96);
            return 0;
        }
        case WM_SYSCOMMAND:
            /* The relay holds Left Alt while looking: letting go shouldn't drop this window into menu mode. */
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) g_ui_deactivated = GetTickCount();
            break;
        case WM_CLOSE: {
            WINDOWPLACEMENT wpl = {sizeof(wpl)};
            if (GetWindowPlacement(h, &wpl)) { /* where it would restore to, even if it's maximized now */
                g_ui_win_x = wpl.rcNormalPosition.left;
                g_ui_win_y = wpl.rcNormalPosition.top;
                g_ui_win_w = wpl.rcNormalPosition.right - wpl.rcNormalPosition.left;
                g_ui_win_h = wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top;
                g_ui_cfg_dirty = 1;
            }
            DestroyWindow(h);
            return 0;
        }
        case WM_MOVE:
            if (g_pop.hwnd) ui_pop_size_and_place();
            break;
        case WM_DESTROY:
            ui_pop_close();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* Again whenever Windows changes: the frame has to agree with what we draw on it. */
static void ui_apply_backdrop(HWND h) {
    BOOL dark = g_th.dark;
    g_dwm_attr(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int kind = g_th.acrylic ? 3 : 1; /* DWMSBT_TRANSIENTWINDOW, or none */
    if (FAILED(g_dwm_attr(h, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &kind, sizeof(kind))) && g_th.acrylic) {
        g_backdrop_failed = 1;
        g_th.acrylic = 0;
    }
    int e = g_th.acrylic ? -1 : 0;
    MARGINS mg = {e, e, e, e};
    g_dwm_extend(h, &mg);
}

static DWORD WINAPI ui_window_thread(LPVOID arg) {
    int activate = (int)(INT_PTR)arg;
    InterlockedExchange(&g_ui_window_thread_id, (LONG)GetCurrentThreadId());
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!ui_load_graphics()) {
        log_line("window: couldn't load Direct2D, opening the console instead");
        ui_console_show();
        ui_free_graphics();
        return 1;
    }
    ui_load_theme(&g_th);

    static int registered = 0;
    HINSTANCE hi = GetModuleHandleA(NULL);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = ui_main_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = LoadIconA(hi, "IDI_ICON1");
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"RuthlessRelayWindow";
        RegisterClassExW(&wc);
        registered = 1;
    }
    memset(&g_main, 0, sizeof(g_main));
    memset(&g_w, 0, sizeof(g_w));
    g_bake_dirty = 1;
    g_hover = g_down = H_NONE;
    g_timer_ms = 0;
    g_main.dpi = 96;

    int x = CW_USEDEFAULT, y = 0, w = 760, h = 520;
    if (g_ui_win_w > 0 && g_ui_win_h > 0) {
        RECT r = {g_ui_win_x, g_ui_win_y, g_ui_win_x + 100, g_ui_win_y + 100};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            x = g_ui_win_x;
            y = g_ui_win_y;
            w = g_ui_win_w;
            h = g_ui_win_h;
        }
    }
    HWND win = CreateWindowExW(0, L"RuthlessRelayWindow", L"Ruthless Controller Relay", WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, x, y, w, h, NULL, NULL, hi, NULL);
    if (!win) {
        ui_free_graphics();
        ui_console_show();
        return 1;
    }
    g_main.hwnd = win;
    g_main.dpi = (float)GetDpiForWindow(win);
    if (g_ui_win_w <= 0) {
        int pw = MulDiv(760, (int)g_main.dpi, 96), ph = MulDiv(520, (int)g_main.dpi, 96);
        SetWindowPos(win, NULL, 0, 0, pw, ph, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ui_apply_backdrop(win);
    SetWindowPos(win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_ui_hwnd = win;
    InterlockedExchange(&g_ui_window_open, 1);
    QueryPerformanceCounter(&g_last_frame);
    {
        ui_state_t st;
        ui_read_state(&st);
        ui_status(&st, &g_w);
        g_ps_ui = g_backend != BACKEND_XINPUT;
    }
    ui_paint();
    ShowWindow(win, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(win);
    ui_set_rate(16);
    ui_test_crash();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InterlockedExchange(&g_ui_window_open, 0);
    InterlockedExchange(&g_ui_window_thread_id, 0);
    g_ui_hwnd = NULL;
    ui_cam_release();
    ui_drop_target();
    ui_free_graphics();
    return 0;
}

/* ---------- The shell: tray, show event, session end ---------- */

#define WM_UI_TRAY (WM_APP + 10)
#define WM_UI_OPEN (WM_APP + 11)

static HANDLE g_ui_show_event;
static HANDLE g_ui_win_thread;
static NOTIFYICONDATAW g_ui_nid;
static UINT g_ui_taskbar_msg;
static UINT32 g_ui_icon_key = 0xFFFFFFFF;

/* A deliberate fault in the window thread, to prove the relay survives one: RCR_TEST_UI_CRASH=1. */
static void ui_test_crash(void) {
    if (getenv("RCR_TEST_UI_CRASH")) *(volatile int *)0 = 1;
}

/* The window's whole drawing stack lives on its thread, so opening means starting it. */
static void ui_open_window(int activate) {
    if (g_ui_win_thread) {
        if (WaitForSingleObject(g_ui_win_thread, 0) == WAIT_TIMEOUT) {
            HWND h = g_ui_hwnd;
            if (h && activate) {
                if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
                SetForegroundWindow(h);
            }
            return;
        }
        CloseHandle(g_ui_win_thread);
        g_ui_win_thread = NULL;
    }
    g_ui_win_thread = CreateThread(NULL, 0, ui_window_thread, (LPVOID)(INT_PTR)activate, 0, NULL);
}

/* The hex icon, with a corner dot for the lightbar color, grey with no controller. */
static HICON ui_tray_icon(UINT32 color, int has_pad) {
    int n = GetSystemMetrics(SM_CXSMICON);
    HICON base = (HICON)LoadImageA(GetModuleHandleA(NULL), "IDI_ICON1", IMAGE_ICON, n, n, LR_DEFAULTCOLOR);
    if (!base) return LoadIcon(NULL, IDI_APPLICATION);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = n;
    bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD *px = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, NULL);
    HICON out = base;
    if (color_bmp && mask) {
        HGDIOBJ old = SelectObject(dc, color_bmp);
        DrawIconEx(dc, 0, 0, base, n, n, 0, NULL, DI_NORMAL);
        SelectObject(dc, old);
        float r = n * .22f, cx = n - r - .5f, cy = n - r - .5f;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                DWORD *p = &px[y * n + x];
                if (!has_pad) { /* grey the whole icon, keeping its shape */
                    BYTE a = (BYTE)(*p >> 24), g = (BYTE)((((*p >> 16) & 255) + ((*p >> 8) & 255) + (*p & 255)) / 3);
                    *p = ((DWORD)a << 24) | ((DWORD)g << 16) | ((DWORD)g << 8) | g;
                }
                float d = hypotf(x + .5f - cx, y + .5f - cy);
                float cover = r + .5f - d;
                if (cover <= 0 || !has_pad) continue;
                if (cover > 1) cover = 1;
                DWORD old_px = *p;
                float inv = 1 - cover;
                BYTE nr = (BYTE)(((color >> 16) & 255) * cover + ((old_px >> 16) & 255) * inv);
                BYTE ng = (BYTE)(((color >> 8) & 255) * cover + ((old_px >> 8) & 255) * inv);
                BYTE nb = (BYTE)((color & 255) * cover + (old_px & 255) * inv);
                BYTE na = (BYTE)(255 * cover + (old_px >> 24) * inv);
                *p = ((DWORD)na << 24) | ((DWORD)nr << 16) | ((DWORD)ng << 8) | nb;
            }
        }
        ICONINFO ii = {TRUE, 0, 0, mask, color_bmp};
        out = CreateIconIndirect(&ii);
        DestroyIcon(base);
    }
    if (color_bmp) DeleteObject(color_bmp);
    if (mask) DeleteObject(mask);
    DeleteDC(dc);
    return out;
}

static void ui_tray_add(HWND h) {
    memset(&g_ui_nid, 0, sizeof(g_ui_nid));
    g_ui_nid.cbSize = sizeof(g_ui_nid);
    g_ui_nid.hWnd = h;
    g_ui_nid.uID = 1;
    g_ui_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_ui_nid.uCallbackMessage = WM_UI_TRAY;
    g_ui_nid.hIcon = ui_tray_icon(0, 0);
    wcscpy(g_ui_nid.szTip, L"Ruthless Controller Relay");
    if (!Shell_NotifyIconW(NIM_ADD, &g_ui_nid)) log_line("tray: couldn't add the icon yet (error %lu)", (unsigned long)GetLastError());
    g_ui_icon_key = 0xFFFFFFFF;
}

static void ui_tray_note(const wchar_t *title, const wchar_t *text) {
    NOTIFYICONDATAW n = g_ui_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_NONE;
    wcsncpy(n.szInfoTitle, title, 63);
    wcsncpy(n.szInfo, text, 255);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

/* One cleanup for every way out. The console handler, Quit, the pipe's quit and session end all land here. */
static void ui_tray_remove(void) {
    if (g_ui_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_ui_nid);
    ui_url_file_cleanup(1);
}

static void ui_quit(void) {
    if (g_ui_cfg_dirty || g_save_at) save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    relay_cleanup();
    ExitProcess(0);
}

static void ui_tray_menu(HWND h) {
    enum { M_OPEN = 1, M_HOTAS, M_NORMAL, M_FL_OFF, M_FL_STICK, M_FL_HEAD, M_LAUNCH, M_UPDATE, M_CONSOLE, M_QUIT };
    ui_state_t st;
    ui_words_t w;
    ui_read_state(&st);
    ui_status(&st, &w);
    HMENU mode = CreatePopupMenu(), look = CreatePopupMenu(), menu = CreatePopupMenu();
    AppendMenuW(mode, MF_STRING | (st.mode == 1 ? MF_CHECKED : 0), M_HOTAS, L"HOTAS");
    AppendMenuW(mode, MF_STRING | (st.mode == 0 ? MF_CHECKED : 0), M_NORMAL, L"Normal");
    AppendMenuW(look, MF_STRING | (!st.fl_on ? MF_CHECKED : 0), M_FL_OFF, L"Off");
    AppendMenuW(look, MF_STRING | (st.fl_on && !st.head_mode ? MF_CHECKED : 0), M_FL_STICK, L"Stick");
    AppendMenuW(look, MF_STRING | (st.head_mode ? MF_CHECKED : 0), M_FL_HEAD, L"Head");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, w.summary);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, M_OPEN, L"Open");
    SetMenuDefaultItem(menu, M_OPEN, FALSE);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)mode, L"Mode");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)look, L"Look");
    AppendMenuW(menu, MF_STRING | (g_game_path[0] && g_relay_state == RELAY_STATE_CONNECTED ? 0 : MF_GRAYED), M_LAUNCH, L"Launch game");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_console_on ? MF_CHECKED : 0), M_CONSOLE, L"Show console");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, M_QUIT, L"Quit the relay");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(h); /* or the menu won't close when you click away */
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (cmd) {
        case M_OPEN: ui_open_window(1); break;
        case M_HOTAS: ipc_set_mode(IPC_CMD_MODE_HOTAS); break;
        case M_NORMAL: ipc_set_mode(IPC_CMD_MODE_NORMAL); break;
        case M_FL_OFF: InterlockedExchange(&g_ipc_fl_want, 0); g_ui_cfg_dirty = 1; break;
        case M_FL_STICK: InterlockedExchange(&g_ipc_fl_want, 1); g_ui_cfg_dirty = 1; break;
        case M_FL_HEAD: InterlockedExchange(&g_ipc_fl_want, 2); g_ui_cfg_dirty = 1; break;
        case M_LAUNCH: {
            HANDLE t = CreateThread(NULL, 0, ui_launch_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            break;
        }
        case M_CONSOLE:
            if (g_console_on) ui_console_hide();
            else ui_console_show();
            break;
        case M_QUIT: ui_quit(); break;
        default: break;
    }
}

/* Once a second: the tray, the remembered mode, and whether the game is up. */
static void ui_shell_second(HWND h) {
    (void)h;
    ui_state_t st;
    ui_words_t w;
    ui_read_state(&st);
    ui_status(&st, &w);
    wchar_t tip[128];
    swprintf(tip, 128, L"Relay " RELAY_VERSION_W L": %ls", w.summary);
    int changed = 0;
    if (wcscmp(tip, g_ui_nid.szTip)) {
        wcsncpy(g_ui_nid.szTip, tip, 127);
        changed = 1;
    }
    UINT32 key = w.bar | (w.bar ? 0x1000000u : 0);
    if (key != g_ui_icon_key) {
        HICON old = g_ui_nid.hIcon;
        g_ui_nid.hIcon = ui_tray_icon(w.bar, w.bar != 0);
        if (old) DestroyIcon(old);
        g_ui_icon_key = key;
        changed = 1;
    }
    if (changed) Shell_NotifyIconW(NIM_MODIFY, &g_ui_nid);

    /* The window's thread is gone. A fault there ends only that thread (see crash_filter). */
    if (g_ui_win_thread && WaitForSingleObject(g_ui_win_thread, 0) != WAIT_TIMEOUT) {
        DWORD code = 0;
        GetExitCodeThread(g_ui_win_thread, &code);
        CloseHandle(g_ui_win_thread);
        g_ui_win_thread = NULL;
        if (g_ui_window_open) {
            InterlockedExchange(&g_ui_window_open, 0);
            g_ui_hwnd = NULL;
        }
        if (code) {
            ui_tray_note(L"The window closed unexpectedly", L"The relay is still running and your controller still works. Click here in the tray to open it again.");
        } else if (!g_ui_seen_tray_note) {
            g_ui_seen_tray_note = 1;
            g_ui_cfg_dirty = 1;
            ui_tray_note(L"Still running here", L"The relay keeps your controller working from the tray. Right-click to quit.");
        }
    }
    ui_url_file_cleanup(0);
    if (g_ui_window_open || g_head_mode) {
        const char *slash = strrchr(g_game_path, '\\');
        InterlockedExchange(&g_ui_game_up, g_game_path[0] && head_process_running(slash ? slash + 1 : g_game_path));
    }
    if (g_hidden_mode != g_ui_cfg_mode && g_hidden_mode >= 0) {
        g_ui_cfg_mode = g_hidden_mode;
        g_ui_cfg_dirty = 1;
    }
    if (g_ui_cfg_dirty) {
        g_ui_cfg_dirty = 0;
        save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    }
}

static LRESULT CALLBACK ui_shell_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == g_ui_taskbar_msg && m) { /* Explorer restarted */
        ui_tray_add(h);
        return 0;
    }
    switch (m) {
        case WM_UI_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP) {
                HWND win = g_ui_hwnd;
                if (win && GetForegroundWindow() == win) PostMessageW(win, WM_CLOSE, 0, 0);
                else if (win && GetTickCount() - g_ui_deactivated < 300) PostMessageW(win, WM_CLOSE, 0, 0);
                else ui_open_window(1);
            } else if (LOWORD(lp) == WM_RBUTTONUP) {
                ui_tray_menu(h);
            }
            return 0;
        case WM_UI_OPEN:
            ui_open_window((int)wp);
            return 0;
        case WM_TIMER:
            ui_shell_second(h);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) ui_quit();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static DWORD WINAPI ui_shell_thread(LPVOID ready) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ui_shell_proc;
    wc.hInstance = hi;
    wc.lpszClassName = L"RuthlessRelayShell";
    RegisterClassExW(&wc);
    /* Top-level with no extended style and no title: session messages don't reach a message-only window. */
    HWND h = CreateWindowExW(0, L"RuthlessRelayShell", L"", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_ui_shell_hwnd = h;
    g_ui_taskbar_msg = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(h, g_ui_taskbar_msg, MSGFLT_ALLOW, NULL); /* we run elevated, Explorer doesn't */
    ui_tray_add(h);
    SetTimer(h, 1, 1000, NULL);
    SetEvent((HANDLE)ready);
    if (!g_ui_start_tray) ui_open_window(1);

    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_ui_show_event, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) ui_open_window(1);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

/* Right after the single-instance check, before any driver work, so the window can show it happening. */
static void ui_start(void) {
    if (!g_ui_windowed) return;
    g_ui_show_event = CreateEventA(NULL, FALSE, FALSE, "Global\\RuthlessControllerRelay.Show");
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, ui_shell_thread, ready, 0, NULL);
    if (!t) {
        ui_console_force();
        return;
    }
    CloseHandle(t);
    WaitForSingleObject(ready, 3000);
    CloseHandle(ready);
}

static int ui_reboot_marker_present(void) {
    char marker[MAX_PATH + 32], dir[MAX_PATH];
    get_exe_dir(dir, sizeof(dir));
    snprintf(marker, sizeof(marker), "%s%s", dir, REBOOT_PENDING_FILE);
    return GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
}

/* The log starts fresh each run, so what happened last time would be gone. Keep it, and notice a crash in it. */
static void ui_keep_last_log(const char *log_path) {
    char prev[MAX_PATH + 40], line[256];
    snprintf(prev, sizeof(prev), "%s", log_path);
    char *slash = strrchr(prev, '\\');
    if (!slash) return;
    snprintf(slash + 1, sizeof(prev) - (size_t)(slash + 1 - prev), "%s", UI_PREV_LOG_FILE);
    if (!MoveFileExA(log_path, prev, MOVEFILE_REPLACE_EXISTING)) return;
    FILE *f = fopen(prev, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "=== CRASH")) g_ui_prev_crash = 1;
    fclose(f);
    if (g_ui_prev_crash) {
        InterlockedExchange(&g_ui_had_crash, 1);
        if (g_ui_shell_hwnd) ui_tray_note(L"The relay closed unexpectedly last time", L"Open it and click the note at the bottom to report it, so it can get fixed.");
    }
}

/* A second launch brings up the running one and quietly exits. */
static int ui_signal_running(void) {
    HANDLE e = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Global\\RuthlessControllerRelay.Show");
    if (!e) return 0;
    SetEvent(e);
    CloseHandle(e);
    return 1;
}

/* --test-config's check of the words. */
static int ui_selftest(void) {
    int failed = 0;
    ui_state_t s;
    ui_words_t w;
#define UI_CHECK(cond, desc) do { if (cond) printf("  ok: %s\n", desc); else { printf("  FAIL: %s\n", desc); failed++; } } while (0)
    printf("== ui_status ==\n");
    memset(&s, 0, sizeof(s));
    s.relay = RELAY_STATE_WAITING;
    s.mode = 1;
    ui_status(&s, &w);
    UI_CHECK(w.take && !wcscmp(w.k_word, L"No controller") && w.flagged, "no controller takes over and flags the rings");
    s.ever = 1;
    s.dropped_s = 14;
    s.pad_kept = 0;
    ui_status(&s, &w);
    UI_CHECK(w.k_class == UI_CRIT_FILL, "dropped with the pad gone is red");
    s.pad_kept = 1;
    ui_status(&s, &w);
    UI_CHECK(w.k_class == UI_WARN_FILL, "dropped with the pad kept is amber");
    s.relay = RELAY_STATE_CONNECTED;
    s.hidden = 1;
    s.fl_on = 1;
    s.head_mode = 1;
    s.head_st = HEAD_ST_TRACKING;
    s.head_speed = 50;
    s.backend = BACKEND_XINPUT;
    ui_status(&s, &w);
    UI_CHECK(!w.take && !wcscmp(w.m_word, L"HOTAS") && !wcscmp(w.l_word, L"Tracking") && !wcscmp(w.k_word, L"Ready"), "HOTAS, Tracking, Ready");
    UI_CHECK(w.bar == 0x00FF00 && w.l_color == UI_W_OK && !wcsncmp(w.l_sub, L"Tap RB", 6), "green bar, green word, Xbox name for the button");
    s.hidden = 0;
    ui_status(&s, &w);
    UI_CHECK(!wcscmp(w.k_word, L"Not hidden") && w.k_class == UI_WARN_FILL, "hiding failed can't be missed");
    s.hidden = 1;
    s.stalled = 1;
    ui_status(&s, &w);
    UI_CHECK(w.take && w.flagged && !wcscmp(w.flag, L"No data"), "a stopped relay flags the instruments");
    s.stalled = 0;
    s.mode = 0;
    s.vigem_ok = 1;
    s.vigem_ds4 = 1;
    s.backend = BACKEND_DUALSENSE_WIRED_RAWHID;
    ui_status(&s, &w);
    UI_CHECK(!wcscmp(w.m_word, L"Normal") && !wcscmp(w.l_word, L"Off") && w.bar == 0x0064FF, "Normal on a native pad is blue, and Look is off");
    s.mode = -1;
    ui_status(&s, &w);
    UI_CHECK(!wcscmp(w.m_word, L"Unset") && w.m_btn == 3 && w.bar == 0xFF3C00, "unset offers both modes");
    s.mode = 1;
    s.batt_src = BATTERY_SRC_PERCENT;
    s.batt_pct = 12;
    ui_status(&s, &w);
    UI_CHECK(w.bar == 0xFF0000 && w.batt_level == 2 && w.k_class == UI_CRIT_LINE, "low battery turns the bar and the box red");
    s.batt_chg = 1;
    ui_status(&s, &w);
    UI_CHECK(w.bar != 0xFF0000 && !w.batt_level, "charging clears it");
    printf("== report link ==\n");
    char url[UI_REPORT_URL_MAX + 1], head[600], log[3000];
    memset(head, '#', sizeof(head) - 1); /* the worst case: every character takes three in a link */
    head[sizeof(head) - 1] = 0;
    memset(log, '\n', sizeof(log) - 1);
    log[sizeof(log) - 1] = 0;
    int n = ui_report_url(url, sizeof(url), head, log);
    UI_CHECK(n > 0 && n <= UI_REPORT_URL_MAX, "a huge report still fits a link GitHub's sign-in can carry");
    n = ui_report_url(url, sizeof(url), "Version: 1", "line one\nline two\n");
    UI_CHECK(n > 0 && strstr(url, "What%20happened") && strstr(url, "line%20one"), "a small report keeps its log lines");
    memset(head, ' ', sizeof(head) - 1);
    n = ui_report_url(url, sizeof(url), head, "");
    UI_CHECK(n > 0 && n <= UI_REPORT_URL_MAX && strstr(url, "clipboard"), "too big for a link: the form still opens, pointing at the clipboard");
    char tiny[8];
    UI_CHECK(ui_url_encode("abcdefghij", tiny, sizeof(tiny)) == -1, "encoding never runs past its buffer");
    UI_CHECK(ui_url_encode("a b&c", tiny, sizeof(tiny)) == -1 && ui_url_encode("a&", tiny, sizeof(tiny)) == 4 && !strcmp(tiny, "a%26"), "encodes what would break a link");
#undef UI_CHECK
    return failed;
}
