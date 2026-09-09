/*
 * The friend-shareable tool -- starts as a copy of the proven
 * xinput_vjoy_relay.c (kept separately, untouched, as the personal/known-
 * working copy) plus the low-latency loop tuning below. Everything new
 * going forward (ViGEmClient virtual controller, DirectInput support for
 * non-Xbox pads, the real GUI, driver-check integration) gets built into
 * THIS file, not the personal one.
 *
 * Reads whatever real controller is connected and mirrors it onto an
 * existing vJoy virtual device. Two input backends, tried in this order:
 *   - XInput: any Xbox-shaped controller (GameSir included), regardless of
 *     transport -- direct cable, Bluetooth, or a wireless dongle (the
 *     GameSir's own, or the official Microsoft Xbox Wireless Adapter) all
 *     show up here identically, since XInput doesn't care how the bytes
 *     got to the PC.
 *   - DirectInput: anything else, e.g. a PS4/PS5 pad over Bluetooth, which
 *     doesn't speak XInput at all. Windows keeps genuine XInput devices
 *     out of DirectInput's own enumeration, so there's no overlap/ambiguity
 *     between the two paths.
 * A direct USB cable connection specifically uses a different, unhideable
 * driver stack, see the HIDHide notes below -- that's about hiding the
 * device, not reading it, so it doesn't affect which backend is used.
 *
 * Also runs a background thread listening for Ctrl+Alt+H (or a controller
 * button combo), which toggles between HOTAS mode (vJoy live, for WARDOGS'
 * axis binding) and Normal mode (a ViGEmBus-emulated Xbox/DS4 gamepad
 * live, for any other game). Flip that BEFORE launching a game, not
 * mid-session, since most games only scan for controllers at startup.
 *
 * IMPORTANT, easy to get wrong (a real bug here once, fixed 2026-09-07):
 * the real controller is hidden from every app but this relay for the
 * ENTIRE time it's connected, in BOTH modes -- hiding is NOT part of the
 * mode toggle. The toggle only changes which VIRTUAL device is live; the
 * real hardware stays hidden throughout so the game only ever sees one
 * thing at a time (whichever virtual device it's supposed to), never the
 * raw controller mixed in alongside it. Hiding only works at all if the
 * controller enumerates as a plain HIDClass device -- confirmed a direct
 * USB cable connection's XInput device (class XnaComposite) can NOT be
 * hidden this way, only a dongle or a genuine HID-class pad (PS4/PS5, or
 * an XInput pad through a wireless dongle) can.
 *
 * Build:
 *   windres hotas_relay.rc -O coff -o hotas_relay_manifest.o
 *   zig cc -target x86_64-windows-gnu -O2 -I vigem_client -o RuthlessControllerRelay.exe ^
 *       hotas_relay.c hotas_relay_manifest.o ^
 *       -ldinput8 -ldxguid -lole32 -lcomdlg32 -ladvapi32 -lshell32 -lsetupapi -lnewdev -lhid
 *
 * (hotas_relay_manifest.o embeds hotas_relay.manifest, which requests
 * requireAdministrator -- this exe now always runs elevated, every launch,
 * no exceptions, because vJoy's virtual device is created fresh on launch
 * and fully destroyed on exit (see destroy_vjoy_root_devices()), which
 * needs admin rights both ways. Skipping this link step produces a working
 * but UNELEVATED exe that will fail to create/destroy the device at all.)
 *
 * (vigem_client\ViGEm\Client.h/Common.h are the real, unmodified,
 * MIT-licensed nefarius/ViGEmClient headers; ViGEmClient.dll itself is
 * built separately from that same project's ViGEmClient.cpp via
 * `zig c++ -target x86_64-windows-gnu -shared -O2 -I vigem_client
 * vigem_client/../src/ViGEmClient.cpp -o ViGEmClient.dll -lsetupapi -lole32`
 * -- confirmed to compile clean with no source changes needed.)
 *
 * Distribute alongside a drivers\ folder (vjoy\, hidhide\, vigembus\, each
 * with that vendor's .inf/.sys/.cat -- see driver_check.c's history for
 * where those came from) AND ViGEmClient.dll, both next to this exe --
 * that's the whole package.
 *
 * (XInput itself is loaded dynamically at runtime, not linked -- see
 * load_xinput() below: xinput9_1_0.dll, the only version with a normal
 * static import lib available, is an old Vista-era compatibility shim that
 * can miss newer controllers. Loading xinput1_4.dll by name instead, the
 * modern version present on every Windows 8+ system, needs no import lib.)
 *
 * Run: RuthlessControllerRelay.exe [vjoy_device_id]   (default device #2)
 * Stop with Ctrl+C.
 */
#include <windows.h>
#include <stdarg.h>
#include <setupapi.h>
#include <devguid.h>
#include <newdev.h>
#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "vigem_client/ViGEm/Client.h"
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <dinput.h>
#include <commdlg.h>

#define HIDHIDE_CLI "\"C:\\Program Files\\Nefarius Software Solutions\\HidHide\\x64\\HidHideCLI.exe\""
#define TOGGLE_HOTKEY_ID 1
#define HOTKEY_CONFIG_FILE "ruthless_controller_relay.ini"

/* Set by load_config() in main() before toggle_thread starts. NOT actually
   read-only after that (stale claim, corrected 2026-09-08 pre-ship audit):
   run_remap_flow() (press R, runs on the main thread) writes fresh values
   here, then posts WM_APP_REMAP to toggle_thread so it re-registers the
   hotkey with them. No separate lock needed for that specific handoff --
   the write always happens before the PostThreadMessageA() call that
   follows it, and Windows' message queue itself provides the happens-
   before guarantee between a post and the matching GetMessage -- but this
   comment is what future edits should trust, not "read-only". */
static UINT g_hotkey_mods = MOD_CONTROL | MOD_ALT;
static UINT g_hotkey_vk = 'H';

/* BACKEND_DS4_RAWHID exists because DirectInput itself cannot correctly
   read a real DualShock 4 over Bluetooth -- confirmed on real hardware:
   the controller's own 547-byte extended Bluetooth report (0x11) streams
   perfectly live (verified via raw HidD ReadFile, see setup_ds4_raw_hid()),
   while DirectInput's parallel read of the exact same physical device
   stayed frozen at all-zeros the entire time. A wired DS4's much smaller
   USB report parses fine through DirectInput (already proven all session),
   so this path is used ONLY for the large-report Bluetooth case -- see
   setup_ds4_raw_hid()'s report-length check. */
typedef enum { BACKEND_XINPUT, BACKEND_DINPUT, BACKEND_DS4_RAWHID, BACKEND_DUALSENSE_RAWHID,
                BACKEND_DS4_WIRED_RAWHID, BACKEND_DUALSENSE_WIRED_RAWHID } backend_t;

/* Thread ID of toggle_thread, captured in main() -- needed so a live
   remap (see run_remap_flow()) can ask that specific thread to
   re-register the hotkey with new values, since RegisterHotKey's
   registration belongs to whichever thread called it. */
static DWORD g_toggle_thread_id = 0;
#define WM_APP_REMAP (WM_APP + 1)

/* Path to a game exe to offer launching once the controller's ready --
   empty until set via the in-software "press G" flow (see
   run_set_game_flow()) or hand-edited into ruthless_controller_relay.ini. */
static char g_game_path[MAX_PATH] = {0};

/* Hand-declared instead of #include <xinput.h>: that header pulls in
   xinput9_1_0's import lib by default, and we specifically want to load
   xinput1_4.dll by name at runtime instead (see load_xinput()). The ABI
   below is the stable, documented Win32 XInput structure layout. */
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

/* Same hand-declaration reasoning as XINPUT_GAMEPAD/XINPUT_STATE above --
   this is XInput's stable, documented rumble-motor-speed ABI, unchanged
   since XInput 1.3. */
typedef struct {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XINPUT_VIBRATION;

/* XInput's own documented battery-status ABI -- unlike LED control (no
   public API exists at all, confirmed 2026-09-08), battery level IS
   officially exposed for Xbox-style controllers, just at a much coarser
   4-level granularity than DualSense's ~10% steps (empty/low/medium/full,
   not a percentage) -- that coarseness is XInput's own real limitation,
   not something this project chose. */
typedef struct {
    BYTE BatteryType;
    BYTE BatteryLevel;
} XINPUT_BATTERY_INFORMATION;
#define XINPUT_BATTERY_DEVTYPE_GAMEPAD 0x00
#define XINPUT_BATTERY_TYPE_DISCONNECTED 0x00
#define XINPUT_BATTERY_TYPE_WIRED 0x01
#define XINPUT_BATTERY_LEVEL_EMPTY 0x00
#define XINPUT_BATTERY_LEVEL_LOW 0x01
#define XINPUT_BATTERY_LEVEL_MEDIUM 0x02
#define XINPUT_BATTERY_LEVEL_FULL 0x03

typedef DWORD(WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION *);
typedef DWORD(WINAPI *XInputGetBatteryInformation_t)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
static XInputGetState_t pXInputGetState;
static XInputSetState_t pXInputSetState;
static XInputGetBatteryInformation_t pXInputGetBatteryInformation;
/* Undocumented sibling of XInputGetState, exported by ordinal 100 only
   (no name, no header, not in any Microsoft documentation) -- same
   signature and same XINPUT_STATE shape, but additionally sets bit
   0x0400 in wButtons for the Guide/Xbox button, which the documented
   XInputGetState never reports at all (Microsoft reserves that button
   for the OS's own Xbox Game Bar). Real, stable, and used by other real
   projects (e.g. Xidi, a maintained XInput/DirectInput remapping
   library) -- but genuinely unsupported, so every call site must
   tolerate GetProcAddress returning NULL (a future xinput dll build
   could remove it) and fall back to plain pXInputGetState's already-
   proven behavior, never fail outright over this one extra bit. */
#define XINPUT_GAMEPAD_GUIDE 0x0400
typedef DWORD(WINAPI *XInputGetStateEx_t)(DWORD, XINPUT_STATE *);
static XInputGetStateEx_t pXInputGetStateEx;

/* Windows' own Xbox Game Bar captures the Guide button system-wide to
   open its overlay -- confirmed (a real XInput-remapping-library
   maintainer's own account) that this ALSO prevents XInputGetStateEx's
   Guide bit from ever being meaningfully set, not just a "something else
   pops up on screen" cosmetic issue. Controlled by one HKCU registry
   DWORD. These three globals track whether THIS session has touched it,
   so it can be put back exactly as found -- same read-before-write,
   restore-on-every-exit discipline as the vJoy OEMName backup before an
   uninstall, or HidHide's own cloak-off. Deliberately conservative: any
   failure at any step (key missing and uncreatable, no permission, etc.)
   just leaves the Guide button unavailable this session -- never fatal,
   never assumed to have silently worked. */
static int g_gamebar_reg_touched = 0;
static int g_gamebar_reg_had_value = 0;
static DWORD g_gamebar_reg_original = 1; /* Windows' own documented default when the value is absent */
static int g_gamebar_guide_disabled = 0; /* 1 once really confirmed off this session -- see disable_gamebar_guide_capture() */

static void log_line(const char *fmt, ...); /* defined further below -- shared diagnostic log, needed here too */

static void disable_gamebar_guide_capture(void) {
    if (g_gamebar_reg_touched) return; /* already done this session -- idempotent, matches every other setup-once pattern here */
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\GameBar", 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) {
        if (RegCreateKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\GameBar", 0, NULL, 0,
                             KEY_READ | KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) {
            return; /* can't touch it at all -- Guide button just stays unavailable this session */
        }
    }
    DWORD value = 0, size = sizeof(value), type = REG_DWORD;
    LONG q = RegQueryValueExA(key, "UseNexusForGameBarEnabled", NULL, &type, (LPBYTE)&value, &size);
    g_gamebar_reg_had_value = (q == ERROR_SUCCESS && type == REG_DWORD);
    g_gamebar_reg_original = g_gamebar_reg_had_value ? value : 1;
    DWORD zero = 0;
    if (RegSetValueExA(key, "UseNexusForGameBarEnabled", 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero)) == ERROR_SUCCESS) {
        g_gamebar_reg_touched = 1;
        g_gamebar_guide_disabled = 1;
        log_line("gamebar: disabled guide-button capture for this session (was %s)",
                 g_gamebar_reg_had_value ? "1" : "not set");
    }
    RegCloseKey(key);
}

/* Called from every exit path this project already treats as "undo
   whatever this session changed" (console_ctrl_handler(), fatal_exit(),
   and the main loop's own Xbox-controller-disconnect cleanup) -- exactly
   mirrors HidHide's own cloak-off placement. If the value never existed
   before this session touched it, deletes it back to that same
   nonexistent state rather than leaving a "0" behind that wasn't there
   originally -- the whole point of saving g_gamebar_reg_had_value. */
static void restore_gamebar_guide_capture(void) {
    if (!g_gamebar_reg_touched) return;
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\GameBar", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        if (g_gamebar_reg_had_value) {
            RegSetValueExA(key, "UseNexusForGameBarEnabled", 0, REG_DWORD,
                            (const BYTE *)&g_gamebar_reg_original, sizeof(g_gamebar_reg_original));
        } else {
            RegDeleteValueA(key, "UseNexusForGameBarEnabled");
        }
        RegCloseKey(key);
        log_line("gamebar: restored guide-button capture to its original state");
    }
    g_gamebar_reg_touched = 0;
    g_gamebar_guide_disabled = 0;
}

/* Hints the CPU we're in a spin-wait (the x86 PAUSE instruction) -- lower
   power/heat and friendlier to a hyperthreading sibling than a bare empty
   loop, but unlike Sleep(), never actually gives up the core, so there's
   no OS scheduler delay before the next poll. Used instead of Sleep(1) in
   the main loop below: Windows' default timer granularity is ~15.6ms, so
   Sleep(1) can genuinely stall 1-15ms per call -- often more added delay
   than the controller's own hardware polling interval. */
static inline void cpu_relax(void) { __builtin_ia32_pause(); }

#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000

/* Which controller button(s) also trigger the mode toggle, alongside the
   keyboard hotkey -- one button, a combo of two (both bits set), and/or a
   minimum hold duration. All set by load_config() in main(). Default is
   Back+Start (changed 2026-09-08 from a single R3 click, per the user's
   own real-world judgment: a two-button combo most games never bind to
   anything is far less likely to fire by accident mid-game than a single
   stick click, which plenty of games DO use for sprint/crouch/etc.). */
/* DWORD, not WORD: as of the PS-button-as-toggle feature, this can hold
   PHYS_PS_BUTTON/PHYS_TOUCHPAD/PHYS_GUIDE_BUTTON (bits 18-20) in addition
   to the standard XINPUT_GAMEPAD wButtons range (bits 0-15) -- see the
   main loop's combo check and NAMED_PHYS, which this now shares with the
   button-remap system instead of keeping its own separate, narrower
   table. */
static DWORD g_toggle_button_mask = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
static DWORD g_toggle_hold_ms = 0;
/* Set by load_config() only when ruthless_controller_relay.ini didn't
   exist at all yet (a genuinely fresh install) -- Back+Start above is
   just a placeholder until the very first controller actually connects,
   at which point the connect loop upgrades this to the PS button (real
   PlayStation controller) or the Guide button (Xbox controller, see
   PHYS_GUIDE_BUTTON/XInputGetStateEx_t) -- good defaults for the same
   reason in both cases: no game binds an action to either one directly,
   and both are guaranteed to never leak through to whatever the game
   sees (see send_rumble_led_to_real_ps_controller()/vigem_update() for
   PS, and the vJoy button loop's ps_is_toggle/guide_is_toggle handling
   for both). The Guide path also flips a real Windows registry setting
   for the session (see disable_gamebar_guide_capture()) -- confirmed
   working end-to-end on real hardware 2026-09-09, including through the
   official Xbox Wireless Adapter dongle. Back+Start is now only the
   actual default for a HOTAS-only device with neither button. Cleared
   the moment it's used (or the moment a real ini is found at all), so
   this can only ever fire once, on a genuinely first-ever run -- an
   existing friend's already-configured combo is never touched. */
static int g_toggle_button_is_fresh_default = 0;

/* Which backend actually found the current controller -- set by main(),
   read by the label/dashboard code below so button names match whichever
   controller is really connected (Cross/Circle/... for PlayStation,
   A/B/... for Xbox-style) instead of always assuming Xbox. */
static backend_t g_backend = BACKEND_XINPUT;

/* Some controllers (confirmed: a wired GameSir-G7 Pro) register BOTH a
   standard XInput interface AND a separate, full DirectInput/HID one for
   the same physical device -- joy.cpl lists it twice. XInput's own
   bitmask has no room for anything beyond its fixed 14 buttons/2
   triggers/2 sticks, so any extra buttons (paddles, etc.) can only ever
   be reachable through the DirectInput one, which this relay would
   otherwise never even try since it always finds the XInput interface
   first and stops there. Not yet handled -- see PROGRESS.md's GameSir
   paddle-buttons entry; needs real hardware data on the actual paddle
   button indices before it's worth designing the interface-choice UX,
   the same "don't guess" rule that governed every other button/axis
   mapping in this file. */

static PVIGEM_CLIENT g_vigem_client = NULL;
static PVIGEM_TARGET g_vigem_target = NULL;
/* Which virtual controller type to present -- defaults to matching
   whatever real controller was detected (PS-style in -> DS4 out, Xbox-
   style in -> X360 out), overridable live with V; see the main loop's
   key handling. */
static int g_vigem_emulate_ds4 = 0;

/* Battery status for the dashboard -- two genuinely different data
   sources feeding one display, because the two controller families
   expose completely different granularity and there's no way to make
   them look the same without inventing fake precision:
     - DualSense (confirmed 2026-09-08 from the real Linux kernel driver,
       byte offset 53 USB / 54 BT): a real ~10%-step percentage plus
       charging status, read directly off the HID input report.
     - Xbox-style (XInput): XInput's own official, documented
       XInputGetBatteryInformation() call -- a real, always-supported API
       (unlike LED control, which has none), but only ever reports one of
       4 coarse levels (empty/low/medium/full), never a percentage --
       that's XInput's own real limitation, not a shortcut taken here.
     - DS4 (original PS4 controller): NOT implemented -- its battery byte
       location was never independently confirmed the way DualSense's
       was (only raw candidate bytes were captured, never decoded with
       real confidence), so this deliberately shows "unknown" for DS4
       rather than guess and risk showing a wrong number. */
typedef enum { BATTERY_SRC_NONE, BATTERY_SRC_PERCENT, BATTERY_SRC_COARSE, BATTERY_SRC_WIRED } battery_src_t;
static battery_src_t g_battery_src = BATTERY_SRC_NONE;
static int g_battery_percent = 0;  /* valid when g_battery_src == BATTERY_SRC_PERCENT */
static int g_battery_charging = 0; /* valid when g_battery_src == BATTERY_SRC_PERCENT */
static int g_battery_coarse = 0;   /* valid when g_battery_src == BATTERY_SRC_COARSE: 0=empty,1=low,2=medium,3=full */

/* Set once register_with_hidhide() actually succeeds for the current
   connection; retried in a bounded loop right at connect, BEFORE
   vigem_setup() creates the virtual pad (see that call site's own
   comment for why the ordering matters -- retrying any later risked
   finding and hiding the virtual pad instead of the real controller).
   Reset to 0 on every fresh connect and on disconnect so a new controller
   always gets its own real attempt instead of inheriting the previous
   one's success. */
static int g_hidhide_registered = 0;

/* Raw, untranslated DirectInput state -- kept around purely so the
   dashboard can show every axis/slider value DirectInput actually
   reports, unfiltered by dinput_to_xinput_gamepad()'s own guesses about
   which field means what. Needed because that mapping is a best-guess
   from documentation, not yet verified against real hardware, and a real
   PS4 controller's triggers turned out not to move any of the fields
   currently read as LT/RT -- this is how to find out which field (if any)
   they're actually on instead of guessing again. */
static DIJOYSTATE2 g_last_dinput_raw;
static int g_have_dinput_raw = 0;

/* PS button / touchpad-click state, DirectInput only -- set once per main
   loop tick alongside g_last_dinput_raw above, read by both the vJoy
   button-setting loop and the live dashboard. Neither exists in
   XINPUT_GAMEPAD's own wButtons bitmask, so unlike every other button
   these need their own side channel instead of living on `gp`. */
static int g_ps_pressed = 0;
static int g_touchpad_pressed = 0;
/* Guide/Xbox button state, XInput only, mirrors g_ps_pressed's reasoning
   exactly -- XINPUT_GAMEPAD.wButtons has no bit for it either (see
   XInputGetStateEx_t's comment), so it needs the same side channel.
   Only ever set when pXInputGetStateEx is actually available AND the
   Game Bar registry toggle is currently disabled by this session (see
   g_gamebar_guide_disabled) -- otherwise stays 0, same as if the button
   didn't exist, matching every other "optional capability" pattern in
   this file (pXInputSetState, pXInputGetBatteryInformation). */
static int g_guide_pressed = 0;

/* Trigger clicks, the PS button, and the touchpad click aren't real
   wButtons bits (see the hair-trigger comment further down -- the PS
   button/touchpad don't exist in XInput's bitmask at all, DS4-only) but
   still need a slot in the remappable button table below, so they get
   sentinel values outside WORD's real bit range. */
#define PHYS_TRIGGER_L 0x10000u
#define PHYS_TRIGGER_R 0x20000u
#define PHYS_PS_BUTTON 0x40000u
#define PHYS_TOUCHPAD 0x80000u
#define PHYS_GUIDE_BUTTON 0x100000u
#define TRIGGER_CLICK_THRESHOLD 26  // ~10% of XInput's 0-255 trigger range, hair-trigger feel

/* rgbButtons[] indices for the DS4's PS button and touchpad click, in
   DirectInput. NOT yet confirmed against real hardware the way the
   trigger axes (Rx/Ry, see g_last_dinput_raw above) were -- this is the
   natural extrapolation of this same DS4's own already-confirmed 0-11
   layout (Square/Cross/Circle/Triangle/L1/R1/-/-/Share/Options/L3/R3,
   indices 6-7 being the unused digital L2/R2 clicks), and matches the
   documented standard Sony DS4 ordering, but it's still a guess until
   checked against the "dinput raw: ... btns=" log line below with a real
   press. Change these two if that log says otherwise. */
#define DINPUT_BTN_IDX_PS 12
#define DINPUT_BTN_IDX_TOUCHPAD 13

/* How many vJoy buttons this relay drives -- started at 16 (the original
   fixed A/B/X/Y/.../trigger-clicks layout), grown to 18 to add the DS4's
   PS button and touchpad click. Touching this means also touching
   VJOY_HID_REPORT_DESCRIPTOR's button usage-max/report-count further
   down, since vJoy's own driver doesn't know how many buttons a device
   has until that descriptor tells it. */
#define NUM_VJOY_BUTTONS 18

/* Which physical input drives each vJoy button 1-NUM_VJOY_BUTTONS -- index
   0 is vJoy button 1, etc. Defaults match the original fixed A=1/B=2/.../trigger
   clicks=15/16 layout; changeable live via run_button_map_flow() (press M
   on the dashboard) and persisted to ruthless_controller_relay.ini, so "vJoy button 6"
   staying vJoy button 6 for existing in-game bindings is a choice a friend
   makes on purpose, not something this relay silently reshuffles. */
static DWORD g_button_map[NUM_VJOY_BUTTONS] = {
    XINPUT_GAMEPAD_A,
    XINPUT_GAMEPAD_B,
    XINPUT_GAMEPAD_X,
    XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER,
    XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP,
    XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_DPAD_DOWN,
    XINPUT_GAMEPAD_DPAD_LEFT,
    PHYS_TRIGGER_L,
    PHYS_TRIGGER_R,
    PHYS_PS_BUTTON,
    PHYS_TOUCHPAD,
};

/* Frozen copy of the above, never mutated -- lets the live dashboard say
   "A is mapped to B" (which physical button's original role this one has
   taken over) instead of a bare vJoy slot number, matching how a friend
   actually thinks about a remap ("I swapped A and B"), not vJoy internals. */
static const DWORD DEFAULT_BUTTON_MAP[NUM_VJOY_BUTTONS] = {
    XINPUT_GAMEPAD_A,
    XINPUT_GAMEPAD_B,
    XINPUT_GAMEPAD_X,
    XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER,
    XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP,
    XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_DPAD_DOWN,
    XINPUT_GAMEPAD_DPAD_LEFT,
    PHYS_TRIGGER_L,
    PHYS_TRIGGER_R,
    PHYS_PS_BUTTON,
    PHYS_TOUCHPAD,
};

/* Human name for one physical input, adapted to whichever controller type
   is actually connected -- PlayStation face buttons/shoulders/Back-Start
   are named differently from Xbox's even though they sit in the same
   physical positions (and the same g_button_map bit values). */
static const char *label_for_phys(DWORD phys, backend_t backend) {
    int ps = (backend == BACKEND_DINPUT || backend == BACKEND_DS4_RAWHID || backend == BACKEND_DUALSENSE_RAWHID ||
               backend == BACKEND_DS4_WIRED_RAWHID || backend == BACKEND_DUALSENSE_WIRED_RAWHID);
    if (phys == XINPUT_GAMEPAD_A) return ps ? "Cross" : "A";
    if (phys == XINPUT_GAMEPAD_B) return ps ? "Circle" : "B";
    if (phys == XINPUT_GAMEPAD_X) return ps ? "Square" : "X";
    if (phys == XINPUT_GAMEPAD_Y) return ps ? "Triangle" : "Y";
    if (phys == XINPUT_GAMEPAD_LEFT_SHOULDER) return ps ? "L1" : "LB";
    if (phys == XINPUT_GAMEPAD_RIGHT_SHOULDER) return ps ? "R1" : "RB";
    if (phys == XINPUT_GAMEPAD_BACK) return ps ? "Share" : "Back";
    if (phys == XINPUT_GAMEPAD_START) return ps ? "Options" : "Start";
    if (phys == XINPUT_GAMEPAD_LEFT_THUMB) return "L3";
    if (phys == XINPUT_GAMEPAD_RIGHT_THUMB) return "R3";
    if (phys == XINPUT_GAMEPAD_DPAD_UP) return "D-Up";
    if (phys == XINPUT_GAMEPAD_DPAD_DOWN) return "D-Down";
    if (phys == XINPUT_GAMEPAD_DPAD_LEFT) return "D-Left";
    if (phys == XINPUT_GAMEPAD_DPAD_RIGHT) return "D-Right";
    if (phys == PHYS_TRIGGER_L) return ps ? "L2 click" : "LT click";
    if (phys == PHYS_TRIGGER_R) return ps ? "R2 click" : "RT click";
    if (phys == PHYS_PS_BUTTON) return "PS";       /* DS4/DS5 only -- XInput has no Guide-button bit */
    if (phys == PHYS_TOUCHPAD) return "Touchpad";  /* DS4/DS5 only */
    if (phys == PHYS_GUIDE_BUTTON) return "Guide"; /* Xbox only -- see XInputGetStateEx_t's comment */
    return "?";
}

/* Reverse lookup into g_button_map so the live dashboard can show what
   vJoy button any given physical input drives right now -- every phys
   value is always present exactly once (run_button_map_flow() swaps
   rather than overwrites), so this always finds a slot. */
static int vjoy_slot_for_phys(DWORD phys) {
    for (int i = 0; i < NUM_VJOY_BUTTONS; i++) {
        if (g_button_map[i] == phys) return i + 1;
    }
    return 0;
}

/* NULL if phys hasn't been remapped away from its own default role;
   otherwise the name of the button whose role it's now taken over (i.e.
   what run_button_map_flow()'s "make A act like B" swap actually did),
   for the live dashboard to show right on the button itself. */
static const char *remap_label_for_phys(DWORD phys, backend_t backend) {
    int slot = vjoy_slot_for_phys(phys);
    if (slot == 0) return NULL;
    DWORD default_occupant = DEFAULT_BUTTON_MAP[slot - 1];
    if (default_occupant == phys) return NULL;
    return label_for_phys(default_occupant, backend);
}

static void log_line(const char *fmt, ...); /* defined below -- shared diagnostic log, needed here too */
static int run_elevated_script(const char *script_lines); /* defined below, needed by check_and_install_drivers() above it */
static void offer_reboot_now(const char *context_msg); /* defined below, needed by check_pending_reboot() above it */
static int count_hidhide_filter_refs(void); /* defined below, needed by check_pending_reboot() above it */
static int destroy_vjoy_root_devices(void); /* defined below, needed by fatal_exit()/console_ctrl_handler() above it */
static long long begin_system_restore_point(const char *description); /* defined below, needed by check_and_install_drivers() above it */
static void end_system_restore_point(long long sequence_number); /* defined below, needed by check_and_install_drivers() above it */
static void offer_to_enable_system_restore(void); /* defined below, needed by check_and_install_drivers() above it */
static int streq_ci(const char *a, const char *b); /* defined below, needed by unhide_stale_hidhide_devices() above it */

/* vJoy's device is now session-scoped by explicit design (2026-09-07,
   after a real duplicate-device mess accumulated from repeated installs
   across testing): created fresh on every launch, destroyed on every
   exit -- clean or fatal -- instead of staying installed indefinitely.
   These two globals let fatal_exit()/console_ctrl_handler() (called from
   many places, including before main()'s local `rid` even exists) know
   whether there's a live vJoy acquisition to release before the device
   itself gets torn down. */
static UINT g_vjoy_rid = 2;
static int g_vjoy_acquired = 0;

/* Guards vJoy relinquish + ViGEm client/target teardown against exactly
   the race caught in the 2026-09-08 pre-ship audit: console_ctrl_handler()
   runs on a Windows-spawned thread the instant Ctrl+C/window-close fires,
   completely independent of whatever the main thread happens to be doing
   at that exact moment -- including its own end-of-connection cleanup
   (same relinquish/vigem_teardown calls, on an ordinary mid-session
   controller disconnect) or a live frame write (pSetAxis/pSetBtn/
   vigem_update() in the main loop). Without this, both threads could
   pass vigem_teardown()'s non-atomic `if (!g_vigem_client) return;` guard
   at once and both free the same ViGEm client/target (a real double-free),
   or the main thread could be mid-write to a vJoy/ViGEm handle the other
   thread just relinquished/freed out from under it. Cheap when
   uncontended (a handful of nanoseconds) -- negligible next to the
   XInputGetState/ReadFile calls the hot loop already makes every
   iteration, so this doesn't trade away the performance this project has
   otherwise been built for. Windows' CRITICAL_SECTION is reentrant for
   the thread already holding it, so the main loop can safely wrap a
   whole block of calls (vigem_update() plus the vJoy writes) in this same
   lock without deadlocking against vigem_update()/vigem_teardown()'s own
   internal Enter/Leave. Initialized once in main() before
   SetConsoleCtrlHandler is registered, so it always exists before any
   thread that could contend on it does. */
static CRITICAL_SECTION g_output_lock;

static int run_hidhide(const char *args) {
    char cmd[1024]; /* wide enough for HIDHIDE_CLI + a chained --dev-hide "<path>" --app-reg "<MAX_PATH exe path>" */
    snprintf(cmd, sizeof(cmd), "%s %s", HIDHIDE_CLI, args);
    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* Same idea as run_hidhide() but captures ALL of the command's stdout
   (looped ReadFile, not a single short read) -- --dev-gaming's device
   listing can be several KB once a few controllers have ever been
   plugged in. */
static size_t run_hidhide_capture(const char *args, char *out, size_t out_size) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "%s %s", HIDHIDE_CLI, args);

    HANDLE readPipe, writePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return 0;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { .cb = sizeof(si), .dwFlags = STARTF_USESTDHANDLES, .hStdOutput = writePipe,
                         .hStdError = writePipe };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return 0;
    }

    size_t total = 0;
    DWORD n;
    while (total < out_size - 1 && ReadFile(readPipe, out + total, (DWORD)(out_size - 1 - total), &n, NULL) &&
           n > 0) {
        total += n;
    }
    out[total] = '\0';

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    return total;
}

/* HidHideCLI's own text output escapes backslashes (e.g.
   "HID\\VID_045E&..."), but the actual device instance path Windows (and
   --dev-hide) expects has single ones -- un-escape in place before using
   anything extracted from --dev-gaming's output. */
static void unescape_backslashes(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r++; }
        else *w++ = *r;
    }
    *w = '\0';
}

/* Finds the real, currently-connected controller's HidHide device
   instance path via --dev-gaming, so it can be registered automatically
   (see register_with_hidhide() below) instead of requiring one-time
   manual setup through HidHideClient.exe's GUI -- which is all that ever
   configured this before, confirmed by finding a real but STALE entry
   for this exe's old pre-rename name in --app-list. vJoy's own entry is
   excluded by its distinctive vendor string (it has no VID_/PID_ pattern
   at all in this listing, unlike every real controller). Picks the first
   present+gaming match, same "grab the first real one" policy
   enum_joystick_cb() already uses for DirectInput enumeration. Returns 1
   and fills path_out on success, 0 if nothing suitable is connected. */
/* REAL BUG found and fixed 2026-09-08, on real hardware, the same night
   as the matching DirectInput duplicate-instance fix above
   (enum_joystick_cb()/di_candidate_is_live()): this used to return the
   FIRST present+gaming match, same "grab the first one" policy as
   DirectInput's enumeration had. --dev-gaming confirmed to list BOTH a
   stale Bluetooth device record (still "present":true in Windows even
   though the controller is now plugged in over USB instead -- a BT
   pairing record doesn't get removed just because the radio link isn't
   the active connection right now) AND the real, currently-connected USB
   one for the exact same physical controller, with no reliable way to
   tell them apart except by transport: the stale one's
   "baseContainerDeviceInstancePath" starts with "BTHENUM\", the live
   USB one with "USB\". If the first one Windows happens to list is the
   stale BT record, this used to register/hide THAT instead of the real
   controller -- leaving the actual connected controller's raw HID
   interface fully visible to any other application (a game included),
   completely undermining the one thing HidHide is here for. Now
   collects every present+gaming candidate and prefers a USB-transported
   one over a Bluetooth-transported one; falls back to the first
   candidate found (old behavior) when nothing is clearly USB, which
   covers the ordinary Bluetooth-only-controller case correctly. */
static int find_real_controller_hidhide_path(char *path_out, size_t path_out_len) {
    static char buf[65536];
    size_t len = run_hidhide_capture("--dev-gaming", buf, sizeof(buf));
    if (len == 0) return 0;

    char first_path[512] = {0};
    int have_first = 0;

    char *pos = buf;
    for (;;) {
        char *chunk_start = strstr(pos, "\"present\"");
        if (!chunk_start) break;
        /* REAL BUG found and fixed 2026-09-08, confirmed via a raw log dump
           of an actual failing case, not a guess: --dev-gaming's own JSON
           puts each device's "friendlyName" line BEFORE that device's own
           "present" field, but AFTER the previous device's closing brace.
           Bounding a chunk at just the next "present" occurrence (the old
           code) therefore swallows the NEXT device's friendlyName text
           into what should be THIS device's chunk -- and when the very
           next entry in the list happens to be vJoy's (title contains
           "Shaul Eizikovich", vJoy's own exclusion marker), that leaked
           text made a perfectly real, present, gaming controller look
           like vJoy and get wrongly excluded. Reproduced 100% of the time
           for an Xbox-compatible controller whose --dev-gaming entry
           happened to be immediately followed by vJoy's with nothing in
           between (no absent placeholder sub-interfaces to absorb the
           leak) -- every earlier real controller tested this session
           happened to have something else in between, which is why this
           was never caught before. Fixed by also stopping the chunk at
           the next "friendlyName" occurrence, whichever comes first --
           that marks the true start of the NEXT device's own text, so
           nothing from it can ever leak backward into this one. */
        char *next_present = strstr(chunk_start + 1, "\"present\"");
        char *next_friendly = strstr(chunk_start + 1, "\"friendlyName\"");
        char *chunk_end = buf + len;
        if (next_present && (!next_friendly || next_present < next_friendly)) chunk_end = next_present;
        else if (next_friendly) chunk_end = next_friendly;

        char saved = *chunk_end;
        *chunk_end = '\0';
        int present = strstr(chunk_start, "\"present\" : true") != NULL;
        int gaming = strstr(chunk_start, "\"gamingDevice\" : true") != NULL;
        int is_vjoy = strstr(chunk_start, "Shaul Eizikovich") != NULL;
        int is_bt = strstr(chunk_start, "\"baseContainerDeviceInstancePath\" : \"BTHENUM") != NULL;
        char *dip = strstr(chunk_start, "\"deviceInstancePath\" : \"");
        if (present && gaming && !is_vjoy && dip) {
            dip += strlen("\"deviceInstancePath\" : \"");
            char *dip_end = strchr(dip, '"');
            if (dip_end) {
                size_t n = (size_t)(dip_end - dip);
                if (n >= path_out_len) n = path_out_len - 1;
                if (!have_first) {
                    memcpy(first_path, dip, n < sizeof(first_path) - 1 ? n : sizeof(first_path) - 1);
                    first_path[n < sizeof(first_path) - 1 ? n : sizeof(first_path) - 1] = '\0';
                    unescape_backslashes(first_path);
                    have_first = 1;
                }
                if (!is_bt) {
                    memcpy(path_out, dip, n);
                    path_out[n] = '\0';
                    unescape_backslashes(path_out);
                    *chunk_end = saved;
                    return 1; /* prefer USB -- stop as soon as one is found */
                }
            }
        }
        *chunk_end = saved;
        pos = chunk_end;
    }

    if (have_first) {
        snprintf(path_out, path_out_len, "%s", first_path);
        return 1; /* nothing was USB -- fall back to the first candidate, same as before this fix */
    }
    return 0;
}

/* Registers whatever real controller is currently connected, plus this
   exe's own CURRENT path, with HidHide, AND turns cloaking on -- called
   once each time a controller is first detected (see main()). Without the
   registration half, cloak-on hides nothing at all on a machine where
   nobody ever manually configured HidHideClient.exe's GUI (every friend's
   fresh machine), and even on THIS dev machine the previously-manual
   registration had gone stale after this tool was renamed from
   xinput_vjoy_relay.exe.

   THE CLOAK-ON CALL HERE IS THE ACTUAL FIX for a real bug (found and
   fixed 2026-09-07, after it had already been called out as the intended
   design once before): the real controller must be hidden from every
   other app for the ENTIRE time this relay is managing it, in BOTH modes
   -- not just in HOTAS mode. The previous version of do_toggle() (below)
   wrongly tied hiding itself to the HOTAS/Normal switch, so Normal mode
   left the real hardware fully visible to whatever game was running --
   exactly the "game sees the raw controller AND the virtual one
   simultaneously" mixing problem this whole hide-and-relay architecture
   exists to prevent in the first place. Cloaking now turns on once, right
   here, and stays on for as long as the relay is actively managing a
   connected controller, completely independent of which virtual device
   the mode toggle currently has live -- see do_toggle()'s own comment. */
/* REAL, SEVERE BUG found and fixed 2026-09-08, after it genuinely locked
   the user out of every single controller they own (Xbox, wired DS4,
   wired DualSense, BT DS4, BT DualSense, hidden simultaneously): this
   function only ever ADDED a device to HidHide's persistent --dev-hide
   list, on every connect, and never removed anything -- --dev-hide is
   additive/persistent across process launches (HidHide is a system-wide
   filter driver, not per-process state), so connecting a DIFFERENT
   physical controller in a LATER session (or even the same session after
   a reconnect) just piled another entry on top of whatever was already
   there. With cloak on (which this same function also turns on), EVERY
   accumulated entry gets hidden from EVERY app at once -- exactly the
   real incident this fix closes. `keep_path` is the one device this
   session actually wants hidden right now; everything else currently in
   --dev-list gets explicitly --dev-unhide'd in the SAME elevated call, so
   the hidden list only ever contains at most one entry going forward. */
static int unhide_stale_hidhide_devices(const char *keep_path, char *out_args, size_t out_size) {
    static char list_buf[8192];
    size_t len = run_hidhide_capture("--dev-list", list_buf, sizeof(list_buf));
    out_args[0] = '\0';
    if (len == 0) return 0;

    int removed = 0;
    size_t used = 0;
    char *pos = list_buf;
    for (;;) {
        char *marker = strstr(pos, "--dev-hide \"");
        if (!marker) break;
        char *pathStart = marker + strlen("--dev-hide \"");
        char *pathEnd = strchr(pathStart, '"');
        if (!pathEnd) break;
        size_t pathLen = (size_t)(pathEnd - pathStart);
        char path[256];
        if (pathLen >= sizeof(path)) pathLen = sizeof(path) - 1;
        memcpy(path, pathStart, pathLen);
        path[pathLen] = '\0';

        if (streq_ci(path, keep_path) == 0) { /* not the one we're about to (re-)hide -- stale, unhide it */
            int n = snprintf(out_args + used, out_size - used, " --dev-unhide \"%s\"", path);
            if (n > 0 && (size_t)n < out_size - used) used += (size_t)n;
            removed++;
        }
        pos = pathEnd + 1;
    }
    return removed;
}

/* Returns 1 once the real controller has actually been found and
   registered/cloaked, 0 if nothing suitable is visible to HidHide yet --
   retried in a short bounded loop right at connect (see that call site's
   own comment).
   REAL BUG found and fixed 2026-09-08, on real hardware: a single shot
   here raced HidHide's own device enumeration on an Xbox controller
   connected through a third-party wireless adapter -- XInput reported the
   controller live before --dev-gaming's own listing had caught up, so
   this returned 0 and gave up for the rest of that session, leaving the
   real controller fully visible to every other app the whole time
   (confirmed via log: "no real controller found via --dev-gaming,
   skipping registration", while a manual HidHideCLI --dev-gaming run
   moments later DID list it). What actually turned out to be timing-
   sensitive here was a SEPARATE parsing bug inside
   find_real_controller_hidhide_path() itself (see its own comment) that
   made this fail 100% of the time for certain device list shapes,
   regardless of how long it retried -- fixed there, not by retrying
   longer.
   A SECOND REAL BUG found and fixed 2026-09-08, on real hardware, after
   the above: on a fast controller off/on cycle (not a fresh first
   connect), this failed every attempt in the bounded retry below, AND
   because unhide_stale_hidhide_devices() only ever runs on the SUCCESS
   path further down, whatever HidHide had hidden from some earlier,
   unrelated successful registration was never cleared -- confirmed via
   HidHideCLI --dev-list showing a completely different, unrelated device
   still hidden while the real, currently-connected controller sat fully
   exposed. Now unhides everything whenever this returns 0, so a repeated
   failure to identify the real controller never leaves something else
   wrongly hidden indefinitely -- "nothing hidden" is always safer than
   "the wrong thing hidden". */
static int register_with_hidhide(void) {
    char dev_path[256];
    if (!find_real_controller_hidhide_path(dev_path, sizeof(dev_path))) {
        log_line("hidhide: no real controller found via --dev-gaming, skipping registration");
        char unhide_args[4096];
        if (unhide_stale_hidhide_devices("", unhide_args, sizeof(unhide_args)) > 0) run_hidhide(unhide_args);
        return 0;
    }
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    char unhide_args[4096];
    int stale = unhide_stale_hidhide_devices(dev_path, unhide_args, sizeof(unhide_args));

    char args[4096 + 700];
    snprintf(args, sizeof(args), "%s --dev-hide \"%s\" --app-reg \"%s\" --cloak-on", unhide_args, dev_path,
              exe_path);
    int rc = run_hidhide(args);
    log_line("hidhide: registered device %s + app %s + cloak-on, cleared %d stale entry(ies) (exit %d)", dev_path,
              exe_path, stale, rc);
    return 1;
}

/* -1 = unknown (no controller connected/registered yet), 0 = Normal
   (ViGEmBus virtual gamepad live, vJoy muted), 1 = HOTAS (vJoy live,
   ViGEmBus muted). This ONLY ever controls which virtual device gets
   real data -- it has NOTHING to do with whether the real hardware is
   hidden, which is handled entirely by register_with_hidhide() above
   (cloak turns on once, at connect, and stays on) plus the cleanup paths
   that turn it back off when the relay actually stops (fatal_exit(),
   console_ctrl_handler(), --unhide). Read by the dashboard rather than
   shelling out to HidHideCLI every frame. */
static volatile LONG g_hidden_mode = -1;

/* Shared by both trigger paths: the Ctrl+Alt+H hotkey and an R3 (right
   stick click) press detected in the main relay loop. Pure in-memory
   state flip now -- no HidHide calls at all, since cloaking itself is no
   longer tied to this toggle (see g_hidden_mode's comment above for why
   that changed, and register_with_hidhide() for where cloaking actually
   gets turned on now, once, independent of mode). */
static void update_ps_mode_led(void); /* defined below (needs send_ps_led/g_ps_output_handle) -- forward-declared
                                           here since do_toggle() needs to call it on every mode flip, and it lives
                                           much earlier in the file than the PS-output code it depends on */

static void do_toggle(void) {
    InterlockedExchange(&g_hidden_mode, g_hidden_mode == 1 ? 0 : 1);
    update_ps_mode_led();
}

typedef struct { const char *name; UINT vk; } NamedKey;
static const NamedKey NAMED_KEYS[] = {
    {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4}, {"f5", VK_F5}, {"f6", VK_F6},
    {"f7", VK_F7}, {"f8", VK_F8}, {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
};

/* One lowercased token from a hotkey= line: a modifier name (returns 1),
   a recognized key (returns 2), or unrecognized (returns 0). */
static int parse_hotkey_token(const char *tok, UINT *mod, UINT *vk) {
    if (!strcmp(tok, "ctrl") || !strcmp(tok, "control")) { *mod = MOD_CONTROL; return 1; }
    if (!strcmp(tok, "alt")) { *mod = MOD_ALT; return 1; }
    if (!strcmp(tok, "shift")) { *mod = MOD_SHIFT; return 1; }
    if (!strcmp(tok, "win")) { *mod = MOD_WIN; return 1; }
    for (size_t i = 0; i < sizeof(NAMED_KEYS) / sizeof(NAMED_KEYS[0]); i++) {
        if (!strcmp(tok, NAMED_KEYS[i].name)) { *vk = NAMED_KEYS[i].vk; return 2; }
    }
    if (tok[0] != '\0' && tok[1] == '\0' && isalnum((unsigned char)tok[0])) {
        *vk = (UINT)toupper((unsigned char)tok[0]);
        return 2;
    }
    return 0;
}

/* Parses "ctrl+alt+h"-style text into RegisterHotKey's modifier bitmask
   plus virtual-key code. Returns 0 if no recognizable key was found (caller
   should warn and fall back to the default), 1 if a real hotkey was
   parsed, or 2 if the line explicitly says "none" -- the user deliberately
   opting out of the keyboard hotkey entirely (see load_config()'s "not
   both" validation: exactly one of hotkey/button-combo may be "none", not
   both, or there'd be no way to toggle modes at all). vk=0 is used as the
   runtime sentinel for "disabled" throughout (0 is never a real, assignable
   Windows virtual-key code). */
static int parse_hotkey_line(const char *line, UINT *mods_out, UINT *vk_out) {
    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *trimmed = buf;
    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
    size_t tlen = strlen(trimmed);
    while (tlen && (trimmed[tlen - 1] == '\n' || trimmed[tlen - 1] == '\r' || trimmed[tlen - 1] == ' ' ||
                     trimmed[tlen - 1] == '\t'))
        trimmed[--tlen] = '\0';
    for (char *p = trimmed; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strcmp(trimmed, "none")) {
        *mods_out = 0;
        *vk_out = 0;
        return 2;
    }

    UINT mods = 0, vk = 0;
    int got_key = 0;
    char *tok = strtok(buf, "+ \t\r\n");
    while (tok) {
        for (char *p = tok; *p; p++) *p = (char)tolower((unsigned char)*p);
        UINT m = 0, k = 0;
        int kind = parse_hotkey_token(tok, &m, &k);
        if (kind == 1) mods |= m;
        else if (kind == 2) { vk = k; got_key = 1; }
        tok = strtok(NULL, "+ \t\r\n");
    }
    if (!got_key) return 0;
    *mods_out = mods;
    *vk_out = vk;
    return 1;
}

static void format_hotkey(char *buf, size_t len, UINT mods, UINT vk) {
    buf[0] = '\0';
    if (vk == 0) { /* explicitly disabled, see parse_hotkey_line()'s "none" handling */
        strncat(buf, "None", len - strlen(buf) - 1);
        return;
    }
    if (mods & MOD_CONTROL) strncat(buf, "Ctrl+", len - strlen(buf) - 1);
    if (mods & MOD_ALT) strncat(buf, "Alt+", len - strlen(buf) - 1);
    if (mods & MOD_SHIFT) strncat(buf, "Shift+", len - strlen(buf) - 1);
    if (mods & MOD_WIN) strncat(buf, "Win+", len - strlen(buf) - 1);

    const char *keyname = NULL;
    for (size_t i = 0; i < sizeof(NAMED_KEYS) / sizeof(NAMED_KEYS[0]); i++) {
        if (NAMED_KEYS[i].vk == vk) { keyname = NAMED_KEYS[i].name; break; }
    }
    char keybuf[16];
    if (keyname) snprintf(keybuf, sizeof(keybuf), "%c%s", (char)toupper((unsigned char)keyname[0]), keyname + 1);
    else snprintf(keybuf, sizeof(keybuf), "%c", (char)vk);
    strncat(buf, keybuf, len - strlen(buf) - 1);
}

/* Shared by both the button-remap system (g_button_map) AND the toggle
   button-combo (g_toggle_button_mask) -- until the PS-button-as-toggle
   feature, the toggle combo used a separate, WORD-sized NAMED_BUTTONS
   table restricted to the standard XINPUT_GAMEPAD wButtons range, since
   the main loop's combo check only ever compared against wButtons. Now
   that the combo check builds the same wButtons+trigger+PS+touchpad
   combined mask read_any_physical_input() does (see the main loop), a
   toggle combo can validly include any of these, so the separate table
   was removed rather than kept in sync with two parsers by hand. */
typedef struct { const char *name; DWORD phys; } NamedPhys;
static const NamedPhys NAMED_PHYS[] = {
    {"a", XINPUT_GAMEPAD_A},
    {"b", XINPUT_GAMEPAD_B},
    {"x", XINPUT_GAMEPAD_X},
    {"y", XINPUT_GAMEPAD_Y},
    {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"back", XINPUT_GAMEPAD_BACK},
    {"start", XINPUT_GAMEPAD_START},
    {"l3", XINPUT_GAMEPAD_LEFT_THUMB},
    {"r3", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"dpad_up", XINPUT_GAMEPAD_DPAD_UP},
    {"dpad_down", XINPUT_GAMEPAD_DPAD_DOWN},
    {"dpad_left", XINPUT_GAMEPAD_DPAD_LEFT},
    {"dpad_right", XINPUT_GAMEPAD_DPAD_RIGHT},
    {"lt", PHYS_TRIGGER_L},
    {"rt", PHYS_TRIGGER_R},
    {"ps", PHYS_PS_BUTTON},
    {"touchpad", PHYS_TOUCHPAD},
    {"guide", PHYS_GUIDE_BUTTON},
};

static int parse_phys_name(const char *name, DWORD *out) {
    for (size_t i = 0; i < sizeof(NAMED_PHYS) / sizeof(NAMED_PHYS[0]); i++) {
        if (!strcmp(name, NAMED_PHYS[i].name)) {
            *out = NAMED_PHYS[i].phys;
            return 1;
        }
    }
    return 0;
}

static const char *ini_name_for_phys(DWORD phys) {
    for (size_t i = 0; i < sizeof(NAMED_PHYS) / sizeof(NAMED_PHYS[0]); i++) {
        if (NAMED_PHYS[i].phys == phys) return NAMED_PHYS[i].name;
    }
    return "a"; /* unreachable in practice -- g_button_map only ever holds NAMED_PHYS values */
}

/* Parses "back+start:1000"-style text: one or two button names (joined by
   +) and an optional :milliseconds hold requirement. Returns 0 if no
   recognizable button was found (caller should warn and fall back to the
   default), 1 if a real combo was parsed, or 2 if the line explicitly
   says "none" -- see parse_hotkey_line()'s comment for the matching
   "not both may be none" rule this is half of. mask=0 is already the
   existing runtime sentinel for "disabled" (the main loop's combo check
   already requires mask != 0), so no new sentinel is needed here. */
static int parse_button_line(const char *line, DWORD *mask_out, DWORD *hold_ms_out) {
    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    {
        char trimmed[128];
        strncpy(trimmed, buf, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        char *t = trimmed;
        while (*t == ' ' || *t == '\t') t++;
        size_t tlen = strlen(t);
        while (tlen && (t[tlen - 1] == '\n' || t[tlen - 1] == '\r' || t[tlen - 1] == ' ' || t[tlen - 1] == '\t'))
            t[--tlen] = '\0';
        for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strcmp(t, "none")) {
            *mask_out = 0;
            *hold_ms_out = 0;
            return 2;
        }
    }

    DWORD hold_ms = 0;
    char *colon = strchr(buf, ':');
    if (colon) {
        hold_ms = (DWORD)atoi(colon + 1);
        *colon = '\0';
    }

    DWORD mask = 0;
    int got_button = 0;
    char *tok = strtok(buf, "+ \t\r\n");
    while (tok) {
        for (char *p = tok; *p; p++) *p = (char)tolower((unsigned char)*p);
        DWORD m;
        if (parse_phys_name(tok, &m)) {
            mask |= m;
            got_button = 1;
        }
        tok = strtok(NULL, "+ \t\r\n");
    }
    if (!got_button) return 0;
    *mask_out = mask;
    *hold_ms_out = hold_ms;
    return 1;
}

static void format_button_combo(char *buf, size_t len, DWORD mask, DWORD hold_ms) {
    buf[0] = '\0';
    if (mask == 0) { /* explicitly disabled, see parse_button_line()'s "none" handling */
        strncat(buf, "None", len - strlen(buf) - 1);
        return;
    }
    int first = 1;
    for (size_t i = 0; i < sizeof(NAMED_PHYS) / sizeof(NAMED_PHYS[0]); i++) {
        if (mask & NAMED_PHYS[i].phys) {
            if (!first) strncat(buf, "+", len - strlen(buf) - 1);
            char up[16];
            snprintf(up, sizeof(up), "%c%s", (char)toupper((unsigned char)NAMED_PHYS[i].name[0]),
                     NAMED_PHYS[i].name + 1);
            strncat(buf, up, len - strlen(buf) - 1);
            first = 0;
        }
    }
    if (hold_ms > 0) {
        char holdbuf[32];
        snprintf(holdbuf, sizeof(holdbuf), " held %ums", (unsigned)hold_ms);
        strncat(buf, holdbuf, len - strlen(buf) - 1);
    }
}

static void get_exe_dir(char *buf, size_t len) {
    GetModuleFileNameA(NULL, buf, (DWORD)len);
    char *slash = strrchr(buf, '\\');
    if (slash) slash[1] = '\0';
    else buf[0] = '\0';
}

/* Shared diagnostic log (also used by the driver-install code further
   down) -- moved this early since ViGEmBus setup needs it too, and a
   silent failure there turned out to need exactly this same fix: no way
   to tell what actually went wrong without it. */
#define INSTALL_LOG_FILE "driver_install_log.txt"
/* Written by run_uninstall() if any service still shows present right
   after the uninstall attempt (Windows' normal "marked for deletion, not
   purged until reboot" behavior) -- checked at startup so a friend who
   tries running this again before rebooting gets a clear explanation
   instead of the confusing endless "Could not acquire vJoy device"
   retry loop that's the ONLY visible symptom otherwise. Deleted again
   automatically once the reboot has actually happened and the services
   are confirmed really gone -- reminds every launch until it's actually
   resolved, without nagging once it is. */
#define REBOOT_PENDING_FILE "reboot_pending.txt"

static void get_install_log_path(char *path, size_t len) {
    char dir[MAX_PATH];
    get_exe_dir(dir, sizeof(dir));
    snprintf(path, len, "%s%s", dir, INSTALL_LOG_FILE);
}

/* Explicit user request: cap driver_install_log.txt so it can never grow
   unbounded on a friend's machine over a long play session -- the file is
   meant to be small enough to actually email/upload if something goes
   wrong, not something that quietly eats their disk. Checked via a cheap
   metadata-only call (no actual read of the file's content) before every
   single log_line() write, so this can run from the hot-ish DirectInput
   diagnostic path (already separately rate-limited to 5/sec, see that
   call site's own comment) without adding real I/O cost on top of the
   write that's about to happen anyway. Truncates and restarts fresh
   rather than trying to trim from the middle -- simplest safe behavior,
   and what matters for a crash report is the MOST RECENT activity, not
   the oldest. */
#define LOG_FILE_CAP_BYTES (200 * 1024 * 1024)

static void cap_log_file_size(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return; /* doesn't exist yet -- nothing to cap */
    ULARGE_INTEGER size;
    size.LowPart = info.nFileSizeLow;
    size.HighPart = info.nFileSizeHigh;
    if (size.QuadPart <= LOG_FILE_CAP_BYTES) return;

    FILE *f = fopen(path, "w"); /* truncate */
    if (f) {
        fprintf(f, "[Log truncated -- exceeded the %d MB cap. Older history was discarded, not this crash/\n"
                    "session's own activity, which starts fresh below.]\n", LOG_FILE_CAP_BYTES / (1024 * 1024));
        fclose(f);
    }
}

static void log_line(const char *fmt, ...) {
    char path[MAX_PATH + 32];
    get_install_log_path(path, sizeof(path));
    cap_log_file_size(path);
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* Real crash handling, explicitly requested: "if it crashes there [needs
   to be] a way to dump the logs so friends can send it to me." Before
   this, an actual crash (a real access violation, not a clean Ctrl+C or
   fatal_exit()) gave ZERO diagnostic information at all -- Windows just
   shows its generic "has stopped working" dialog and the process is
   gone, with nothing in driver_install_log.txt explaining why. This
   installs a top-level unhandled-exception filter that records the
   exception code/address/faulting thread before the process actually
   terminates.

   The actual writing happens on a SEPARATE, always-running, already-
   started thread, not the crashing thread itself, as explicitly
   requested ("running from second thred"). This matters most for the
   nastiest crash case -- a stack overflow -- where the thread that
   actually faulted may have no stack space left to safely call fopen/
   fprintf/etc. at all; signaling a completely separate thread with its
   own fresh stack to do the actual file I/O is the standard, correct
   pattern real crash-reporting tools use for exactly this reason. The
   crashing thread just fills in a plain struct (no function calls needed
   beyond that) and waits briefly for confirmation before letting Windows
   finish terminating the process normally.

   Explicitly does NOT change how a normal, user-requested stop works --
   Ctrl+C and window-close still go through console_ctrl_handler() exactly
   as before; this filter only ever fires for a genuine unhandled
   exception, never for a deliberate exit. */
typedef struct {
    DWORD exceptionCode;
    void *exceptionAddress;
    DWORD threadId;
} CrashInfo;

static CrashInfo g_crash_info;
static HANDLE g_crash_event = NULL;      /* the faulting thread signals this to wake the logger thread */
static HANDLE g_crash_done_event = NULL; /* the logger thread signals this once it's actually finished writing */

static DWORD WINAPI crash_logger_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_crash_event, INFINITE);
        char path[MAX_PATH + 32];
        get_install_log_path(path, sizeof(path));
        cap_log_file_size(path);
        FILE *f = fopen(path, "a");
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "\n=== CRASH %04u-%02u-%02u %02u:%02u:%02u ===\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond);
            fprintf(f, "Exception code: 0x%08lX\n", (unsigned long)g_crash_info.exceptionCode);
            fprintf(f, "Exception address: %p\n", g_crash_info.exceptionAddress);
            fprintf(f, "Faulting thread ID: %lu\n", (unsigned long)g_crash_info.threadId);
            fprintf(f, "If a friend sent you this: this file (driver_install_log.txt, next to RuthlessControllerRelay.exe)\n");
            fprintf(f, "is exactly what's needed to diagnose the crash above -- please send the whole file.\n");
            fflush(f);
            fclose(f);
        }
        SetEvent(g_crash_done_event);
    }
    return 0;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    g_crash_info.exceptionCode = ep->ExceptionRecord->ExceptionCode;
    g_crash_info.exceptionAddress = ep->ExceptionRecord->ExceptionAddress;
    g_crash_info.threadId = GetCurrentThreadId();
    if (g_crash_event && g_crash_done_event) {
        SetEvent(g_crash_event);
        /* Bounded wait -- never let a broken logger thread (or a crash
           severe enough that even signaling doesn't get through cleanly)
           hang the termination process indefinitely. 5s is generous for
           what's just a few fprintf calls. */
        WaitForSingleObject(g_crash_done_event, 5000);
    }
    return EXCEPTION_EXECUTE_HANDLER; /* let Windows terminate the process normally after this */
}

/* Called once, very early in main(), before anything else that could
   plausibly crash. Best-effort: if the events/thread can't be created for
   some reason, the crash filter simply isn't installed rather than
   failing startup over a diagnostics-only feature. */
static void install_crash_handler(void) {
    g_crash_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_crash_done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_crash_event || !g_crash_done_event) return;
    if (!CreateThread(NULL, 0, crash_logger_thread, NULL, 0, NULL)) return;
    SetUnhandledExceptionFilter(crash_filter);
}

/* Loads hotkey=/button=... from ruthless_controller_relay.ini next to the exe -- the
   in-software way to change how mode-toggling works without editing or
   recompiling source. Creates the file with commented defaults if it
   doesn't exist yet, so there's always something there to open and edit.
   Falls back to Ctrl+Alt+H / R3 for anything missing, unreadable, or
   invalid. */
static void get_config_path(char *path, size_t len) {
    char dir[MAX_PATH];
    get_exe_dir(dir, sizeof(dir));
    snprintf(path, len, "%s%s", dir, HOTKEY_CONFIG_FILE);
}

/* Rewrites ruthless_controller_relay.ini from scratch with the given settings -- used
   both to create it fresh on first run and to save changes made through
   the in-software remap/set-game flows (R / G while the dashboard is up),
   so a friend never has to hand-edit this file for the normal case. */
static void save_config(UINT mods, UINT vk, DWORD button_mask, DWORD hold_ms, const char *game_path) {
    char path[MAX_PATH + 32];
    get_config_path(path, sizeof(path));
    FILE *out = fopen(path, "w");
    if (!out) return;

    char hk[32], btn[48];
    format_hotkey(hk, sizeof(hk), mods, vk);
    format_button_combo(btn, sizeof(btn), button_mask, hold_ms);
    /* Written in the same key=value shape parse_hotkey_line/parse_button_line
       expect, not the human-readable hk/btn strings above -- those are only
       for the comment showing what's currently set. */
    char hotkey_value[32];
    strncpy(hotkey_value, hk, sizeof(hotkey_value) - 1);
    hotkey_value[sizeof(hotkey_value) - 1] = '\0';
    for (char *p = hotkey_value; *p; p++) *p = (char)tolower((unsigned char)*p);

    fprintf(out,
        "# HOTAS relay settings\n"
        "#\n"
        "# The real controller is ALWAYS hidden from every other game/app while\n"
        "# this relay is running -- only this relay ever sees it, in both\n"
        "# modes. HOTAS mode = your HOTAS/joystick binding (vJoy) is what games\n"
        "# see. Normal mode = a regular Xbox/PlayStation-style virtual gamepad\n"
        "# is what games see instead. hotkey= and button= below are two\n"
        "# separate ways to swap between them -- both work at once by default,\n"
        "# but you can set either one (not both) to \"none\" if you'd rather only\n"
        "# use the other.\n"
        "#\n"
        "# Easiest way to change these: press R while the relay's dashboard is\n"
        "# showing, and it'll ask you to press the new hotkey and controller\n"
        "# button live (that flow doesn't support setting either to \"none\" yet --\n"
        "# hand-edit below for that specifically). Hand-editing always works too.\n"
        "#\n"
        "# hotkey: combine any of ctrl, alt, shift, win with one letter/number\n"
        "# key, or f1-f12, joined with +. Currently: %s\n"
        "hotkey=%s\n"
        "#\n"
        "# button: one controller button, or two joined with + for a combo (so\n"
        "# it can't be triggered by accident) -- a, b, x, y, lb, rb, back, start,\n"
        "# l3, r3, dpad_up, dpad_down, dpad_left, dpad_right, lt, rt, and on a\n"
        "# PlayStation controller, ps or touchpad. The PS button in particular is\n"
        "# a good single-button choice: no game binds an action to it directly (it\n"
        "# normally just opens a system overlay this software never lets through\n"
        "# to begin with, since the real controller stays fully hidden), so it\n"
        "# can't collide with anything the game itself does.\n"
        "#\n"
        "# On an Xbox controller, \"guide\" (the Xbox-logo button) works the same\n"
        "# way, and is actually the default for a fresh Xbox setup, same as PS is\n"
        "# for PlayStation -- confirmed working, including through the official\n"
        "# Wireless Adapter dongle. Using it makes this software flip a Windows\n"
        "# setting for the session (Settings > Gaming > Xbox Game Bar > \"Open\n"
        "# Xbox Game Bar using this button on a controller\"), put back exactly as\n"
        "# found when this closes. If Steam is running, its own \"Guide Button\n"
        "# Focuses Steam\" setting (Steam > Settings > Controller) is a SEPARATE\n"
        "# thing this software can't touch -- turn that off too if the guide\n"
        "# button still opens Steam instead of toggling modes.\n"
        "#\n"
        "# Optionally add :milliseconds to require holding it that long, e.g.\n"
        "# back+start:1000 for a 1-second hold of Back+Start together. Set either\n"
        "# hotkey= or button= (not both) to \"none\" to disable just that one and\n"
        "# rely only on the other. Currently: %s\n"
        "button=",
        hk, hotkey_value, btn);
    if (button_mask == 0) {
        /* Real bug fixed 2026-09-08, not just theoretical: button_mask
           can genuinely be 0 now (the user explicitly disabling the
           controller combo, see parse_button_line()'s "none" handling).
           The old code below always assumed at least one name got
           written and unconditionally seeked back one byte to trim a
           trailing "+" -- with nothing written at all, that seeks back
           over button='s own "=" instead, corrupting the next line
           written. Handle the empty case explicitly instead. */
        fprintf(out, "none");
    } else {
        long before_names = ftell(out);
        for (size_t i = 0; i < sizeof(NAMED_PHYS) / sizeof(NAMED_PHYS[0]); i++) {
            if (button_mask & NAMED_PHYS[i].phys) fprintf(out, "%s+", NAMED_PHYS[i].name);
        }
        if (ftell(out) > before_names) fseek(out, -1, SEEK_CUR); /* trim the trailing + -- only if something was
                                                                      actually written to trim */
        if (hold_ms > 0) fprintf(out, ":%u", (unsigned)hold_ms);
    }
    fprintf(out,
        "\n#\n"
        "# game_path: full path to your game's exe. Press G while the dashboard\n"
        "# is showing to pick it with a normal file browser instead of typing it\n"
        "# here -- once set, pressing G again launches it.\n"
        "game_path=%s\n"
        "#\n"
        "# buttonmap: which physical button/trigger drives each vJoy button 1-18,\n"
        "# in order, comma-separated -- a, b, x, y, lb, rb, back, start, l3, r3,\n"
        "# dpad_up, dpad_down, dpad_left, dpad_right, lt, rt, ps, touchpad. Easiest\n"
        "# way to change this: press M while the dashboard is showing.\n"
        "buttonmap=",
        game_path ? game_path : "");
    for (int i = 0; i < NUM_VJOY_BUTTONS; i++) {
        fprintf(out, "%s%s", ini_name_for_phys(g_button_map[i]), i < NUM_VJOY_BUTTONS - 1 ? "," : "\n");
    }
    fclose(out);
}

static void load_config(UINT *mods_out, UINT *vk_out, DWORD *button_mask_out, DWORD *hold_ms_out,
                         char *game_path_out, size_t game_path_len) {
    *mods_out = MOD_CONTROL | MOD_ALT;
    *vk_out = 'H';
    *button_mask_out = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    *hold_ms_out = 0;
    if (game_path_len) game_path_out[0] = '\0';

    char path[MAX_PATH + 32];
    get_config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        save_config(*mods_out, *vk_out, *button_mask_out, *hold_ms_out, "");
        g_toggle_button_is_fresh_default = 1; /* genuinely first-ever run -- let the connect loop upgrade
                                                   this to the PS button once it knows the real controller
                                                   type, see g_toggle_button_is_fresh_default's own comment */
        return; /* using the Ctrl+Alt+H / Back+Start defaults set above, for now */
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') continue;
        if (!strncmp(p, "hotkey=", 7)) {
            UINT mods, vk;
            if (parse_hotkey_line(p + 7, &mods, &vk)) {
                *mods_out = mods;
                *vk_out = vk;
            } else {
                fprintf(stderr, "Warning: couldn't parse hotkey= line in %s, using Ctrl+Alt+H.\n", HOTKEY_CONFIG_FILE);
            }
        } else if (!strncmp(p, "button=", 7)) {
            DWORD mask;
            DWORD hold_ms;
            if (parse_button_line(p + 7, &mask, &hold_ms)) {
                *button_mask_out = mask;
                *hold_ms_out = hold_ms;
            } else {
                fprintf(stderr, "Warning: couldn't parse button= line in %s, using Back+Start.\n",
                         HOTKEY_CONFIG_FILE);
            }
        } else if (!strncmp(p, "game_path=", 10) && game_path_len) {
            char *val = p + 10;
            size_t vlen = strlen(val);
            while (vlen && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) val[--vlen] = '\0';
            strncpy(game_path_out, val, game_path_len - 1);
            game_path_out[game_path_len - 1] = '\0';
        } else if (!strncmp(p, "buttonmap=", 10)) {
            char mapbuf[256];
            strncpy(mapbuf, p + 10, sizeof(mapbuf) - 1);
            mapbuf[sizeof(mapbuf) - 1] = '\0';
            DWORD parsed[NUM_VJOY_BUTTONS];
            int n = 0;
            char *tok = strtok(mapbuf, ", \t\r\n");
            while (tok && n < NUM_VJOY_BUTTONS) {
                for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
                if (!parse_phys_name(tok, &parsed[n])) break;
                n++;
                tok = strtok(NULL, ", \t\r\n");
            }
            if (n == NUM_VJOY_BUTTONS) {
                memcpy(g_button_map, parsed, sizeof(g_button_map));
            } else {
                fprintf(stderr, "Warning: couldn't parse buttonmap= line in %s, using the default layout.\n",
                        HOTKEY_CONFIG_FILE);
            }
        }
    }
    fclose(f);

    /* Explicit user request: let a friend opt out of EITHER the keyboard
       hotkey OR the controller combo (e.g. "hotkey=none" if they only
       ever want to toggle with the controller, or "button=none" if they
       don't want any controller input eligible to trigger it by
       accident) -- but never both at once, since that would strand them
       with no way to switch modes at all. vk==0 is the hotkey's
       "disabled" sentinel (see parse_hotkey_line()); mask==0 already was
       the button-combo's disabled sentinel before this feature existed.
       If both end up disabled (whether from two explicit "none" lines,
       or one "none" plus one unparseable line that happened to leave the
       mask at 0), the keyboard hotkey wins back its default -- it's the
       one guaranteed to work even before any controller is connected. */
    if (*vk_out == 0 && *button_mask_out == 0) {
        fprintf(stderr, "Warning: %s disables BOTH the hotkey and the controller combo -- that would leave no "
                         "way to toggle modes at all. Restoring the default hotkey (Ctrl+Alt+H); the controller "
                         "combo stays disabled.\n", HOTKEY_CONFIG_FILE);
        *mods_out = MOD_CONTROL | MOD_ALT;
        *vk_out = 'H';
    }
}

static DWORD WINAPI toggle_thread(LPVOID arg) {
    (void)arg;
    /* g_hotkey_vk == 0 means the user explicitly disabled the keyboard
       hotkey (hotkey=none, see load_config()) -- skip registering
       anything, but still run the message loop below, since a live remap
       (WM_APP_REMAP, press R) could turn a real hotkey back on later and
       this thread needs to still be alive to register it when that
       happens. */
    int registered = 0;
    if (g_hotkey_vk != 0) {
        registered = RegisterHotKey(NULL, TOGGLE_HOTKEY_ID, g_hotkey_mods, g_hotkey_vk) != 0;
        if (!registered) {
            char hk[32];
            format_hotkey(hk, sizeof(hk), g_hotkey_mods, g_hotkey_vk);
            fprintf(stderr, "Could not register hotkey %s (already used by something else?). Keyboard toggle "
                             "disabled -- the controller combo (if any) still works.\n", hk);
        }
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == TOGGLE_HOTKEY_ID) {
            do_toggle();
        } else if (msg.message == WM_APP_REMAP) {
            /* Live remap: main thread already updated g_hotkey_mods/vk --
               re-register with this thread's own registration, since
               RegisterHotKey's binding belongs to whichever thread made
               it. Only unregister if we actually had one, and only
               register the new one if it isn't itself "none". */
            if (registered) UnregisterHotKey(NULL, TOGGLE_HOTKEY_ID);
            registered = g_hotkey_vk != 0 && RegisterHotKey(NULL, TOGGLE_HOTKEY_ID, g_hotkey_mods, g_hotkey_vk) != 0;
        }
    }
    return 0;
}

#define CLR_NORMAL (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define CLR_WHITE_BR (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define CLR_GRAY_DIM (FOREGROUND_INTENSITY)
#define CLR_CYAN (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define CLR_GREEN_BR (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define CLR_YELLOW_BR (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
/* White text on a solid color block -- as loud/obvious as a console gets. */
#define CLR_BANNER_HOTAS (BACKGROUND_GREEN | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define CLR_BANNER_NORMAL \
    (BACKGROUND_RED | BACKGROUND_GREEN | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define CLR_BANNER_UNKNOWN (BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)

static HANDLE g_console;

static void console_init_once(void) {
    static int done = 0;
    if (done) return;
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci = {1, FALSE};
    SetConsoleCursorInfo(g_console, &ci); /* hide the blinking cursor for a cleaner look */

    /* Force a console window/buffer comfortably taller than the dashboard
       needs (currently ~30 lines, and still growing -- GameSir buttons
       are next) instead of trusting whatever size the window happened to
       open at. Confirmed on real hardware: once the dashboard's line
       count reached whatever the actual window height was, every ~15Hz
       redraw's cursor movement past the visible bottom forced the
       terminal to auto-scroll to follow it -- looks exactly like
       continuous flashing/scrolling, not a one-time glitch. Shrink first,
       since SetConsoleScreenBufferSize fails if the current window is
       larger than the requested buffer. */
    SMALL_RECT shrink = {0, 0, 9, 9};
    SetConsoleWindowInfo(g_console, TRUE, &shrink);
    COORD bufSize = {100, 60};
    SetConsoleScreenBufferSize(g_console, bufSize);
    SMALL_RECT winRect = {0, 0, 99, 44};
    SetConsoleWindowInfo(g_console, TRUE, &winRect);

    /* SetConsoleCursorPosition positions the cursor at an absolute row in
       the console's internal buffer -- fine on the classic conhost window,
       but modern terminals (Windows Terminal, VS Code's integrated one)
       can scroll their visible viewport independently of that buffer, so
       "row 0" silently stops being what's actually on screen. Switched to
       ANSI cursor codes for this reason earlier -- NOT actually sufficient
       on its own, confirmed on real hardware in VS Code's integrated
       terminal: a screenshot showed many full copies of the dashboard
       stacked on top of each other, meaning even `\x1b[1;1H` doesn't
       reliably return to the true top of the VISIBLE viewport once real
       scrollback has already accumulated there -- it can end up relative
       to the whole buffer's history instead. The actual fix is the
       "alternate screen buffer" (`\x1b[?1049h`) -- the same mechanism
       vim/htop/nano/tmux use to take over a terminal -- a separate,
       scrollback-free surface where position (1,1) always is the real
       top-left, in every terminal that supports it (effectively all of
       them). Left on for the program's whole lifetime; restored
       (`\x1b[?1049l`) in console_ctrl_handler()/fatal_exit() so the
       user's normal prompt/history comes back on exit instead of being
       replaced by whatever the dashboard last showed. */
    DWORD mode = 0;
    if (GetConsoleMode(g_console, &mode)) SetConsoleMode(g_console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    printf("\x1b[?1049h\x1b[2J\x1b[H");
    fflush(stdout);
    done = 1;
}

/* 0-indexed, like the COORD-based positioning this replaces -- ANSI itself
   is 1-indexed, converted here so every existing call site stays the same. */
static void ansi_goto(int col, int row) { printf("\x1b[%d;%dH", row + 1, col + 1); }

/* Full clear, not just a cursor-home -- used when switching between the
   waiting screen and the live dashboard, since they're different lengths
   and would otherwise leave stray leftover lines from the longer one.
   \x1b[3J additionally clears back-scroll in terminals that support it, so
   copying "everything" out of the window doesn't pull in old frames. */
static void clear_console(void) {
    console_init_once();
    printf("\x1b[2J\x1b[3J\x1b[H");
}

/* REAL BUG found and fixed 2026-09-09, explicit user request: this used to
   call SetConsoleTextAttribute(), the old 16-color console API -- looks
   flat/dated (literally compared to "Windows 95" by the user) specifically
   under an elevated launch, since UAC-elevated console apps often don't
   get hosted by Windows Terminal at all even when it's set as the system
   default (a known Windows limitation, not something fixable from
   Settings), falling back to the classic conhost's plain default palette.
   ENABLE_VIRTUAL_TERMINAL_PROCESSING was ALREADY being turned on in
   console_init_once() (needed for the alternate-screen-buffer/cursor
   tricks elsewhere in this file) but set_color() itself never actually
   switched to using it -- this finishes that: every existing CLR_* value
   and the two runtime-built battery-color WORDs (see render_dashboard())
   keep their exact same bit meaning (FOREGROUND_RED/GREEN/BLUE/INTENSITY,
   BACKGROUND_RED/GREEN/INTENSITY), so every call site is unchanged --
   only the translation from those bits to what's actually drawn changes,
   from the old fixed 16-color palette to real 24-bit ANSI color that
   looks vivid in EITHER the classic console or Windows Terminal, since
   both have supported true-color ANSI since Windows 10 1511+. Background
   is always explicitly set or reset (\x1b[49m) on every call, never left
   to whatever a previous banner call last set -- ANSI SGR state persists
   across calls, unlike SetConsoleTextAttribute which always replaces the
   whole attribute at once. */
static void set_color(WORD attr) {
    int fg_r = (attr & FOREGROUND_RED) ? ((attr & FOREGROUND_INTENSITY) ? 255 : 170) : 0;
    int fg_g = (attr & FOREGROUND_GREEN) ? ((attr & FOREGROUND_INTENSITY) ? 255 : 170) : 0;
    int fg_b = (attr & FOREGROUND_BLUE) ? ((attr & FOREGROUND_INTENSITY) ? 255 : 170) : 0;
    if (!(attr & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)) && (attr & FOREGROUND_INTENSITY)) {
        fg_r = fg_g = fg_b = 140; /* "just intensity, no color" (CLR_GRAY_DIM) -- a real gray, not invisible black */
    }
    printf("\x1b[38;2;%d;%d;%dm", fg_r, fg_g, fg_b);

    /* REAL BUG found and fixed 2026-09-09, explicit user report on real
       hardware: an earlier version of this theming special-cased the
       CLR_CYAN bit pattern itself, on the theory that it was only ever
       used for "the mode indicator" -- wrong. CLR_CYAN is the dashboard's
       general-purpose label color, used all over (axis bar labels like
       "LX"/"LY" included), so that version made THOSE turn green/purple/
       etc. too, confusable with an actual mode change. The real, single,
       already-prominent "what mode am I in" indicator is the top banner
       (CLR_BANNER_NORMAL specifically -- CLR_BANNER_HOTAS/UNKNOWN already
       have their own correct, un-confusing green/orange backgrounds that
       don't need this), so only ITS background is themed now, matching
       the same mode/identity relationship update_ps_mode_led() already
       uses for a real PS controller's own lightbar (generalized to cover
       Xbox pads too, which have no controllable LED of their own to show
       this on at all -- see that function's own comment). Foreground
       (white-bright) is untouched either way. */
    if (attr & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE)) {
        int bg_r = (attr & BACKGROUND_RED) ? ((attr & BACKGROUND_INTENSITY) ? 210 : 150) : 0;
        int bg_g = (attr & BACKGROUND_GREEN) ? ((attr & BACKGROUND_INTENSITY) ? 210 : 150) : 0;
        int bg_b = (attr & BACKGROUND_BLUE) ? ((attr & BACKGROUND_INTENSITY) ? 210 : 150) : 0;
        if (attr == CLR_BANNER_NORMAL) {
            int isPsBackend = (g_backend == BACKEND_DS4_RAWHID || g_backend == BACKEND_DUALSENSE_RAWHID ||
                                 g_backend == BACKEND_DS4_WIRED_RAWHID || g_backend == BACKEND_DUALSENSE_WIRED_RAWHID);
            int isXboxBackend = (g_backend == BACKEND_XINPUT);
            if ((isPsBackend && g_vigem_emulate_ds4) || (isXboxBackend && !g_vigem_emulate_ds4)) {
                bg_r = 0; bg_g = 130; bg_b = 110; /* Normal, native identity: teal */
            } else if ((isPsBackend && !g_vigem_emulate_ds4) || (isXboxBackend && g_vigem_emulate_ds4)) {
                bg_r = 110; bg_g = 0; bg_b = 170; /* Normal, standing in as the other type: purple */
            } /* else (HOTAS/generic DirectInput device, no PS/Xbox identity to be "native" or "other"
                 relative to): keep the original yellow-ish background computed above, unchanged */
        }
        printf("\x1b[48;2;%d;%d;%dm", bg_r, bg_g, bg_b);
    } else {
        printf("\x1b[49m"); /* explicit reset -- see this function's own comment for why */
    }
}

/* Rows actually visible right now, whatever terminal this is running
   under. Forcing the window to a bigger size (see console_init_once())
   turned out to only work under the classic conhost window -- it silently
   does nothing under Windows Terminal (Windows 11's default host for
   console apps even when double-clicked from Explorer, confirmed to be
   what was actually happening here), so that fix alone wasn't enough:
   confirmed still flashing/scrolling on real hardware after it shipped.
   GetConsoleScreenBufferInfo's srWindow, unlike the APIs that try to SET
   the window size, correctly reports the REAL visible size under every
   host including Windows Terminal via ConPTY -- so render_dashboard()
   below uses this to never print more lines than actually fit, instead
   of continuing to assume a resize request took effect. */
static int console_visible_rows(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_console, &info)) return 999; /* unknown -- assume fine over blocking the dashboard */
    return info.srWindow.Bottom - info.srWindow.Top + 1;
}

static void draw_axis_bar(const char *label, int value, int min, int max) {
    enum { WIDTH = 21 };
    char bar[WIDTH + 1];
    int pos = (int)(((long long)(value - min) * (WIDTH - 1)) / (max - min));
    if (pos < 0) pos = 0;
    if (pos > WIDTH - 1) pos = WIDTH - 1;
    for (int i = 0; i < WIDTH; i++) bar[i] = (i == pos) ? '#' : (i == WIDTH / 2 ? '|' : '-');
    bar[WIDTH] = '\0';

    set_color(CLR_CYAN);
    printf("  %-3s [", label);
    set_color(CLR_YELLOW_BR);
    printf("%s", bar);
    set_color(CLR_CYAN);
    printf("] ");
    set_color(CLR_WHITE_BR);
    printf("%6d\n", value);
}

/* Simple flat grid, not a spatial diagram -- exact positions on screen
   turned out not to matter much and cost real reliability (see the ANSI
   cursor-positioning fix above); what matters is correct, easy-to-read
   labels for whichever controller is actually connected. */
/* Shows what this physical input has been remapped to act like (">Name"),
   right on the live dashboard, so a remap done via M stays visible instead
   of only being knowable by revisiting the M screen. remap_label is NULL
   for anything still at its default role -- nothing to call out. */
/* Explicit user request 2026-09-09: light the four face buttons up in
   their real, physical colors when pressed -- Triangle/Square/Cross/
   Circle's green/pink/blue/red are as iconic to PlayStation as A/B/X/Y's
   own green/red/blue/yellow are to Xbox. Only applies to those 8 specific
   labels (matched by name, since draw_button() is only ever given the
   already-resolved PS-or-Xbox label text, not the raw button bitmask) --
   every other button keeps the plain green-bright/gray-dim look this
   dashboard has always used, matched via the ANSI printf below since
   set_color() only understands the fixed set of legacy WORD attributes,
   not arbitrary RGB. */
static void print_face_button_color(const char *label) {
    /* Mapped by REAL button identity, not by shared XINPUT slot -- Square
       and Xbox's "X" occupy the same physical slot (both XINPUT_GAMEPAD_X)
       but are not the same real-life color (pink vs. blue), so each of
       the 8 labels gets its own explicit check rather than being grouped
       by slot. Circle and Xbox's "B" (same slot, XINPUT_GAMEPAD_B) both
       happen to be red in real life -- a genuine coincidence, not a
       shortcut taken here. */
    if (!strcmp(label, "Triangle")) printf("\x1b[38;2;60;220;90m");       /* PS Triangle: green */
    else if (!strcmp(label, "A")) printf("\x1b[38;2;60;220;90m");         /* Xbox A: green */
    else if (!strcmp(label, "Square")) printf("\x1b[38;2;235;90;190m");   /* PS Square: pink/magenta */
    else if (!strcmp(label, "X")) printf("\x1b[38;2;70;140;235m");        /* Xbox X: blue */
    else if (!strcmp(label, "Cross")) printf("\x1b[38;2;70;140;235m");    /* PS Cross: blue */
    else if (!strcmp(label, "Circle")) printf("\x1b[38;2;235;70;70m");    /* PS Circle: red */
    else if (!strcmp(label, "B")) printf("\x1b[38;2;235;70;70m");         /* Xbox B: red */
    else if (!strcmp(label, "Y")) printf("\x1b[38;2;235;210;60m");        /* Xbox Y: yellow (no PS equivalent) */
    else set_color(CLR_GREEN_BR);
}

static void draw_button(const char *label, int pressed, const char *remap_label) {
    char buf[40];
    if (remap_label) snprintf(buf, sizeof(buf), pressed ? "[%s>%s]" : " %s>%s ", label, remap_label);
    else snprintf(buf, sizeof(buf), pressed ? "[%s]" : " %s ", label);
    if (pressed) print_face_button_color(label); else set_color(CLR_GRAY_DIM);
    printf("%-20s", buf); /* wide enough for "[Triangle>RT click]", the longest case used */
}

/* "Xbox-style controller (XInput)" / "PlayStation-style controller
   (DirectInput)" -- can't reliably tell a real Xbox pad from GameSir
   or any other XInput-compatible one apart (XInput itself doesn't expose
   that), so XInput is reported generically. DirectInput devices do carry
   a real VID/PID (already read once already to exclude vJoy's own from
   enumeration -- see enum_joystick_cb), so those get a specific model
   name where recognized, falling back to a generic label otherwise. */
static DWORD g_dinput_vid_pid = 0; /* set by enum_joystick_cb(), setup_ds4_raw_hid(), or
                                        setup_dualsense_raw_hid() -- whichever backend actually connects: (pid<<16)|vid */

static const char *describe_controller(backend_t backend) {
    if (backend == BACKEND_XINPUT) return "Xbox-style controller (XInput)";
    if (backend == BACKEND_DS4_RAWHID) return "PlayStation 4 controller (DualShock 4) over Bluetooth, raw HID";
    if (backend == BACKEND_DUALSENSE_RAWHID) return "PlayStation 5 controller (DualSense) over Bluetooth, raw HID";
    if (backend == BACKEND_DS4_WIRED_RAWHID) return "PlayStation 4 controller (DualShock 4), wired, raw HID";
    if (backend == BACKEND_DUALSENSE_WIRED_RAWHID) return "PlayStation 5 controller (DualSense), wired, raw HID";
    WORD vid = (WORD)(g_dinput_vid_pid & 0xFFFFu);
    WORD pid = (WORD)((g_dinput_vid_pid >> 16) & 0xFFFFu);
    if (vid == 0x054C) { /* Sony */
        if (pid == 0x05C4 || pid == 0x09CC) return "PlayStation 4 controller (DualShock 4)";
        if (pid == 0x0CE6) return "PlayStation 5 controller (DualSense)";
        return "PlayStation controller (Sony, unrecognized model)";
    }
    return "PlayStation-style controller (generic DirectInput)";
}

/* Shown in a loop before any controller is found, and again if one
   disconnects mid-session -- makes it obvious the software is alive and
   just waiting, and exactly what to do about it. */
static void render_waiting_screen(UINT rid, int was_ever_connected) {
    clear_console();

    set_color(CLR_WHITE_BR);
    printf("=== Ruthless Controller Relay -- vJoy device #%u ===\n\n", rid);
    set_color(CLR_BANNER_UNKNOWN);
    printf(" NOT READY -- waiting for a controller...                              \n");
    set_color(CLR_NORMAL);
    if (was_ever_connected) {
        printf("\nController disconnected. Turn it back on / reconnect it -- this will\n");
        printf("pick it back up on its own, no need to restart.\n");
    } else {
        printf("\nTurn on / connect any Xbox-style or PlayStation controller any time --\n");
        printf("this keeps checking and will pick it up automatically, no restart needed.\n");
    }
    printf("\nDo NOT start your game until this screen changes to READY below.\n");
    set_color(CLR_GRAY_DIM);
    printf("\nOnce connected, press R to change the toggle hotkey/button, or G to\n");
    printf("pick/launch your game.\n");
}

/* Total lines the full dashboard below actually prints (counted, not
   guessed -- 32 as of the PS button/touchpad row, +2 margin). Printing
   past however many rows are ACTUALLY visible is what forces the
   terminal to auto-scroll every redraw; below, this is checked BEFORE
   drawing anything so that can't happen regardless of window height. */
#define DASHBOARD_ROWS_NEEDED 39

/* Explicit user request 2026-09-09, "some ascii art... free right?" -- a
   small bordered badge, plain ASCII only (no Unicode block-drawing: this
   has to render correctly in the classic console too, not just Windows
   Terminal, and a codepage that can't show block art would just print
   garbled '?' boxes instead), showing INPUT (which REAL controller is
   actually connected) side by side with OUTPUT (which virtual identity
   it's currently emulating). Both update immediately on a controller
   swap or a V-key press, same as everything else on this always-redrawn
   dashboard, since both are read fresh from the live globals every call. */
static void print_dual_badge(void) {
    int leftIsPs = 0, leftIsNeutral = 0;
    if (g_backend == BACKEND_XINPUT) {
        leftIsPs = 0;
    } else if (g_backend == BACKEND_DS4_RAWHID || g_backend == BACKEND_DUALSENSE_RAWHID ||
                g_backend == BACKEND_DS4_WIRED_RAWHID || g_backend == BACKEND_DUALSENSE_WIRED_RAWHID) {
        leftIsPs = 1;
    } else { /* BACKEND_DINPUT -- a real wired PS controller via the DirectInput fallback, or a genuine
                HOTAS/generic joystick; only the former has a real PS/Xbox identity to show */
        WORD vid = (WORD)(g_dinput_vid_pid & 0xFFFFu);
        if (vid == 0x054C) leftIsPs = 1;
        else leftIsNeutral = 1;
    }
    int rightIsPs = g_vigem_emulate_ds4;
    int rightIsNeutral = !g_vigem_target;

    int lr, lg, lb, rr, rg, rb;
    const char *lword, *rword;
    if (leftIsNeutral) { lr = lg = lb = 150; lword = "HOTAS"; }
    else if (leftIsPs) { lr = 0; lg = 112; lb = 209; lword = "PS"; }
    else { lr = 16; lg = 203; lb = 46; lword = "XBOX"; }
    if (rightIsNeutral) { rr = rg = rb = 150; rword = "----"; }
    else if (rightIsPs) { rr = 0; rg = 112; rb = 209; rword = "PS"; }
    else { rr = 16; rg = 203; rb = 46; rword = "XBOX"; }

    char lbuf[9], rbuf[9];
    snprintf(lbuf, sizeof(lbuf), "%-6s", lword);
    snprintf(rbuf, sizeof(rbuf), "%-6s", rword);

    printf("\n");
    printf("\x1b[38;2;%d;%d;%dm +--------+  ", lr, lg, lb);
    printf("\x1b[38;2;%d;%d;%dm+--------+\n", rr, rg, rb);
    printf("\x1b[38;2;%d;%d;%dm | %s |  ", lr, lg, lb, lbuf);
    printf("\x1b[38;2;%d;%d;%dm| %s |\n", rr, rg, rb, rbuf);
    printf("\x1b[38;2;%d;%d;%dm +--------+  ", lr, lg, lb);
    printf("\x1b[38;2;%d;%d;%dm+--------+\n", rr, rg, rb);
    set_color(CLR_GRAY_DIM);
    printf("   INPUT               OUTPUT\n");
    set_color(CLR_NORMAL);
}

static void render_dashboard(const XINPUT_GAMEPAD *gp, double updates_per_sec, UINT rid) {
    console_init_once();

    /* Edge-triggered (only clear+print on a CHANGE of state), not
       re-printed every ~15Hz frame -- otherwise this fallback message
       would flash exactly the same way the bug it's working around does. */
    static int was_too_small = -1; /* -1 = not yet known */
    int too_small = console_visible_rows() < DASHBOARD_ROWS_NEEDED;
    if (too_small) {
        if (was_too_small != 1) {
            clear_console();
            set_color(CLR_BANNER_UNKNOWN);
            printf("This window is too short to show the dashboard without it scrolling.\n");
            set_color(CLR_NORMAL);
            printf("Please enlarge/maximize this window (needs about %d rows), or shrink\n", DASHBOARD_ROWS_NEEDED);
            printf("the terminal font, then it'll display correctly here automatically.\n");
            was_too_small = 1;
        }
        return;
    }
    was_too_small = 0;

    ansi_goto(0, 0);

    /* The real controller is ALWAYS hidden from every app but this relay,
       in both modes -- that's the core feature, not something that only
       applies to HOTAS mode (a real bug fixed 2026-09-07: do_toggle() used
       to tie hiding itself to the mode switch, so Normal mode wrongly left
       the real hardware fully visible, exactly the "game sees the raw
       controller AND the virtual one at once" mixing problem this whole
       architecture exists to prevent). What the mode actually changes is
       only which VIRTUAL device is live -- vJoy in HOTAS mode, the
       ViGEmBus gamepad in Normal mode -- never whether the hardware itself
       is hidden. */
    LONG mode = g_hidden_mode;
    WORD banner_clr = mode == 1 ? CLR_BANNER_HOTAS : mode == 0 ? CLR_BANNER_NORMAL : CLR_BANNER_UNKNOWN;
    const char *mode_str = mode == 1   ? " HOTAS MODE -- vJoy is live (game sees only the virtual stick)        "
                            : mode == 0 ? " NORMAL MODE -- virtual gamepad is live (game sees only that)         "
                                        : " MODE UNKNOWN -- press Ctrl+Alt+H once to set it                      ";

    set_color(CLR_WHITE_BR);
    printf("=== Ruthless Controller Relay -- vJoy device #%u ===          \n", rid);
    set_color(CLR_CYAN);
    printf("Controller: %-52s\n", describe_controller(g_backend));
    /* See g_battery_src's own comment for exactly what each source can and
       can't report -- a real percentage for DualSense, a coarse 4-level
       reading for Xbox-style pads (XInput's own real limitation, not a
       shortcut), "wired" for a plugged-in Xbox pad (no meaningful battery
       level at all), nothing at all for DS4 (never independently
       confirmed) or when no real controller is connected. Color follows
       the same low/medium/high logic as the physical lightbar's own
       battery warning (see the mode-color-sync code further down) so the
       screen and the controller always agree. */
    if (g_battery_src == BATTERY_SRC_PERCENT) {
        WORD battClr = g_battery_percent <= 15 ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                        : g_battery_percent <= 40 ? CLR_YELLOW_BR
                                                    : CLR_GREEN_BR;
        set_color(battClr);
        printf("Battery: %d%%%s%-40s\n", g_battery_percent, g_battery_charging ? " (charging)" : "",
                "");
    } else if (g_battery_src == BATTERY_SRC_COARSE) {
        WORD battClr = g_battery_coarse <= XINPUT_BATTERY_LEVEL_LOW ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                        : g_battery_coarse == XINPUT_BATTERY_LEVEL_MEDIUM ? CLR_YELLOW_BR
                                                                            : CLR_GREEN_BR;
        const char *label = g_battery_coarse == XINPUT_BATTERY_LEVEL_EMPTY ? "Empty"
                              : g_battery_coarse == XINPUT_BATTERY_LEVEL_LOW ? "Low"
                              : g_battery_coarse == XINPUT_BATTERY_LEVEL_MEDIUM ? "Medium"
                                                                                 : "Full";
        set_color(battClr);
        printf("Battery: %-52s\n", label);
    } else if (g_battery_src == BATTERY_SRC_WIRED) {
        set_color(CLR_GRAY_DIM);
        printf("Battery: wired (no battery reading while plugged in)%-1s\n", "");
    } else {
        set_color(CLR_GRAY_DIM);
        printf("Battery: %-52s\n", "unknown");
    }
    set_color(CLR_CYAN);
    if (g_backend == BACKEND_DINPUT && g_have_dinput_raw) {
        set_color(CLR_GRAY_DIM);
        printf("  raw: X=%-6ld Y=%-6ld Z=%-6ld Rx=%-6ld Ry=%-6ld Rz=%-6ld S0=%-6ld S1=%-6ld\n",
               g_last_dinput_raw.lX, g_last_dinput_raw.lY, g_last_dinput_raw.lZ, g_last_dinput_raw.lRx,
               g_last_dinput_raw.lRy, g_last_dinput_raw.lRz, g_last_dinput_raw.rglSlider[0],
               g_last_dinput_raw.rglSlider[1]);
        /* Live, not just in the log file -- watch this while pressing an
           unmapped button (PS, touchpad, a GameSir paddle) and whichever
           index flips to 1 is that button's real rgbButtons index. */
        char rawbtn[24];
        for (int bi = 0; bi < 20; bi++) rawbtn[bi] = (g_last_dinput_raw.rgbButtons[bi] & 0x80) ? '1' : '0';
        rawbtn[20] = '\0';
        printf("  raw btns (idx 0-19): %s\n", rawbtn);
        set_color(CLR_NORMAL);
    } else {
        printf("\n\n");
    }
    set_color(CLR_GREEN_BR);
    printf(" READY -- controller connected, you can start your game now.          \n\n");
    set_color(banner_clr);
    printf("%s\n", mode_str);
    set_color(CLR_NORMAL);
    char hk[32], btn[48];
    format_hotkey(hk, sizeof(hk), g_hotkey_mods, g_hotkey_vk);
    format_button_combo(btn, sizeof(btn), g_toggle_button_mask, g_toggle_hold_ms);
    printf("\n%s or controller %-24s toggles mode                    \n", hk, btn);
    printf("Do this BEFORE launching a game, not during.                          \n");
    set_color(CLR_GRAY_DIM);
    printf("Press R to change the hotkey/button above live. Press M to fix/remap   \n");
    printf("any vJoy button (see the grid below).                                 \n");
    printf("Press G to %-58s\n", g_game_path[0] ? "launch your game." : "pick your game's exe (then launch it).");
    printf("Updates/sec: %6.1f                                          \n\n", updates_per_sec);
    set_color(CLR_NORMAL);

    draw_axis_bar("LX", gp->sThumbLX, -32768, 32767);
    draw_axis_bar("LY", gp->sThumbLY, -32768, 32767);
    draw_axis_bar("RX", gp->sThumbRX, -32768, 32767);
    draw_axis_bar("RY", gp->sThumbRY, -32768, 32767);
    draw_axis_bar("LT", gp->bLeftTrigger, 0, 255);
    draw_axis_bar("RT", gp->bRightTrigger, 0, 255);

    printf("\n  ");
    draw_button(label_for_phys(XINPUT_GAMEPAD_A, g_backend), gp->wButtons & XINPUT_GAMEPAD_A,
                remap_label_for_phys(XINPUT_GAMEPAD_A, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_B, g_backend), gp->wButtons & XINPUT_GAMEPAD_B,
                remap_label_for_phys(XINPUT_GAMEPAD_B, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_X, g_backend), gp->wButtons & XINPUT_GAMEPAD_X,
                remap_label_for_phys(XINPUT_GAMEPAD_X, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_Y, g_backend), gp->wButtons & XINPUT_GAMEPAD_Y,
                remap_label_for_phys(XINPUT_GAMEPAD_Y, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_LEFT_SHOULDER, g_backend), gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER,
                remap_label_for_phys(XINPUT_GAMEPAD_LEFT_SHOULDER, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_RIGHT_SHOULDER, g_backend),
                gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER,
                remap_label_for_phys(XINPUT_GAMEPAD_RIGHT_SHOULDER, g_backend));
    printf("\n  ");
    draw_button(label_for_phys(XINPUT_GAMEPAD_BACK, g_backend), gp->wButtons & XINPUT_GAMEPAD_BACK,
                remap_label_for_phys(XINPUT_GAMEPAD_BACK, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_START, g_backend), gp->wButtons & XINPUT_GAMEPAD_START,
                remap_label_for_phys(XINPUT_GAMEPAD_START, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_LEFT_THUMB, g_backend), gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB,
                remap_label_for_phys(XINPUT_GAMEPAD_LEFT_THUMB, g_backend));
    draw_button(label_for_phys(XINPUT_GAMEPAD_RIGHT_THUMB, g_backend), gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB,
                remap_label_for_phys(XINPUT_GAMEPAD_RIGHT_THUMB, g_backend));
    printf("\n  ");
    draw_button("Up", gp->wButtons & XINPUT_GAMEPAD_DPAD_UP,
                remap_label_for_phys(XINPUT_GAMEPAD_DPAD_UP, g_backend));
    draw_button("Down", gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN,
                remap_label_for_phys(XINPUT_GAMEPAD_DPAD_DOWN, g_backend));
    draw_button("Left", gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT,
                remap_label_for_phys(XINPUT_GAMEPAD_DPAD_LEFT, g_backend));
    draw_button("Right", gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT,
                remap_label_for_phys(XINPUT_GAMEPAD_DPAD_RIGHT, g_backend));
    printf("\n  ");
    draw_button(label_for_phys(PHYS_TRIGGER_L, g_backend), gp->bLeftTrigger > TRIGGER_CLICK_THRESHOLD,
                remap_label_for_phys(PHYS_TRIGGER_L, g_backend));
    draw_button(label_for_phys(PHYS_TRIGGER_R, g_backend), gp->bRightTrigger > TRIGGER_CLICK_THRESHOLD,
                remap_label_for_phys(PHYS_TRIGGER_R, g_backend));
    printf("\n  ");
    draw_button(label_for_phys(PHYS_PS_BUTTON, g_backend), g_ps_pressed, remap_label_for_phys(PHYS_PS_BUTTON, g_backend));
    draw_button(label_for_phys(PHYS_TOUCHPAD, g_backend), g_touchpad_pressed,
                remap_label_for_phys(PHYS_TOUCHPAD, g_backend));

    set_color(CLR_NORMAL);
    printf("\n\nPress M to fix/remap any button (incl. trigger clicks) live.          \n");
    if (g_vigem_target) {
        printf("Virtual gamepad looks like: %-10s Press V to switch to %-10s\n",
               g_vigem_emulate_ds4 ? "PlayStation" : "Xbox", g_vigem_emulate_ds4 ? "Xbox." : "PlayStation.");
    } else {
        printf("                                                                       \n");
    }
    printf("Press Ctrl+C to stop.                                              \n");

    /* Explicit user request 2026-09-09, "some ascii art... free right?" --
       a small bordered badge (plain ASCII only, no Unicode block art: this
       has to render correctly in the classic console too, not just
       Windows Terminal, and a codepage that can't show block-drawing
       characters would just print garbled '?' boxes instead) showing INPUT
       (which REAL controller is actually connected -- g_backend) side by
       side with OUTPUT (which virtual identity it's currently emulating --
       g_vigem_emulate_ds4, the same live value the V key already toggles
       and the line above already reports in text), so both update
       immediately on a controller swap or a V-key press, same as
       everything else on this always-redrawn dashboard. */
    print_dual_badge();
}

static HMODULE load_xinput(void) {
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (!h) {
        fprintf(stderr, "Could not load xinput1_4.dll.\n");
        return NULL;
    }
    pXInputGetState = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
    if (!pXInputGetState) {
        fprintf(stderr, "xinput1_4.dll loaded but missing XInputGetState.\n");
        return NULL;
    }
    /* XInputSetState (rumble) and XInputGetBatteryInformation are both
       optional -- their absence shouldn't fail the whole load, since
       XInputGetState is the one thing this relay cannot function without.
       Checked separately wherever each is actually used, same pattern as
       g_vigem_available. */
    pXInputSetState = (XInputSetState_t)GetProcAddress(h, "XInputSetState");
    pXInputGetBatteryInformation = (XInputGetBatteryInformation_t)GetProcAddress(h, "XInputGetBatteryInformation");
    /* Ordinal-only, no name -- see XInputGetStateEx_t's own comment.
       NULL here is expected and fine on some xinput dll builds; every
       caller already treats a NULL pXInputGetStateEx as "no Guide-button
       support this session," never a fatal condition. */
    pXInputGetStateEx = (XInputGetStateEx_t)GetProcAddress(h, MAKEINTRESOURCEA(100));
    return h;
}

/* DirectInput path -- for controllers that don't speak XInput at all, like
   a PS4/PS5 pad over Bluetooth. An Xbox controller (or GameSir) over
   Bluetooth or the official Microsoft Xbox Wireless Adapter still shows up
   through XInput above regardless of that transport, so it never reaches
   this path; Windows deliberately keeps genuine XInput devices out of
   DirectInput's own game-controller enumeration to avoid exactly that
   double-handling. */
static LPDIRECTINPUT8 g_di = NULL;
static LPDIRECTINPUTDEVICE8 g_di_device = NULL;

static int load_dinput(void) {
    typedef HRESULT(WINAPI * DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    HMODULE h = LoadLibraryA("dinput8.dll");
    if (!h) return 0;
    DirectInput8Create_t create = (DirectInput8Create_t)GetProcAddress(h, "DirectInput8Create");
    if (!create) return 0;
    return SUCCEEDED(create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8, (LPVOID *)&g_di, NULL));
}

/* vJoy's own fixed hardware ID (see vjoy.inf: root\VID_1234&PID_BEAD) --
   vJoy isn't an XInput device, so without this it would legitimately show
   up in DirectInput's own game-controller enumeration as if it were a
   real input device, and this relay would end up reading vJoy's own
   (idle) output state right back as if it were a controller. Confirmed
   this actually happens: an early test with no real controller connected
   still reported READY, because it had picked up vJoy itself. */
#define VJOY_VID 0x1234u
#define VJOY_PID 0xBEADu

/* REAL BUG found and fixed 2026-09-08, on real hardware, after a whole
   night chasing what looked like a rumble/LED-related input freeze: it
   wasn't that at all. Windows can end up with TWO separate DirectInput
   game-controller instances for the same physical DS4/DS4-alike at once
   -- both named "Wireless Controller", both reporting the same VID/PID,
   completely indistinguishable by name/product/vendor ID -- one being a
   stale/ghost device node (confirmed via a live dual-instance probe:
   Acquire() and GetDeviceState() on it both succeed forever, but the
   axis values stay frozen at exactly the mapped range's default center
   and never reflect the controller's real, physically-held position),
   the other being the real, currently-connected one. This project's
   raw-HID open/close churn and repeated Bluetooth connect/disconnect
   cycling earlier tonight is the leading theory for how the ghost node
   got left behind. enum_joystick_cb() used to grab whichever instance
   DirectInput happened to enumerate first and stop there -- if that's
   the ghost, every single read looks "successful" (no error, no
   disconnect ever detected) while silently returning dead, unchanging
   data forever, which is exactly the symptom this project spent all
   night trying to fix by reverting unrelated rumble/LED code. Now
   collects every candidate instead of stopping at the first; the actual
   choice happens in setup_dinput_device()'s liveness probe below. */
#define MAX_DI_CANDIDATES 8
typedef struct { GUID instance; DWORD vidPid; } DiCandidate;
static DiCandidate g_di_candidates[MAX_DI_CANDIDATES];
static int g_di_candidate_count = 0;

static BOOL CALLBACK enum_joystick_cb(const DIDEVICEINSTANCEA *inst, VOID *ctx) {
    (void)ctx;
    WORD vid = (WORD)(inst->guidProduct.Data1 & 0xFFFFu);
    WORD pid = (WORD)((inst->guidProduct.Data1 >> 16) & 0xFFFFu);
    if (vid == VJOY_VID && pid == VJOY_PID) return DIENUM_CONTINUE; /* skip our own virtual output device */
    if (g_di_candidate_count < MAX_DI_CANDIDATES) {
        g_di_candidates[g_di_candidate_count].instance = inst->guidInstance;
        g_di_candidates[g_di_candidate_count].vidPid = ((DWORD)pid << 16) | vid;
        g_di_candidate_count++;
    }
    return DIENUM_CONTINUE; /* must see every attached candidate now, not just the first */
}

/* Probes one candidate DirectInput instance for LIVE input: acquires it
   with a buffered data format and waits up to DI_LIVENESS_WAIT_MS for at
   least one real HID report to show up via GetDeviceData. A dead/ghost
   instance (see the big comment above) never produces one, no matter how
   long you wait, since nothing real is actually behind it; a real
   controller -- even one sitting perfectly still -- still emits periodic
   HID reports at its normal polling rate. Fully self-contained: creates
   its own device handle and releases it before returning, independent of
   g_di_device. */
#define DI_LIVENESS_WAIT_MS 300
static int di_candidate_is_live(const GUID *guid) {
    LPDIRECTINPUTDEVICE8 dev = NULL;
    if (FAILED(IDirectInput8_CreateDevice(g_di, guid, &dev, NULL))) return 0;
    int live = 0;
    if (SUCCEEDED(IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2))) {
        IDirectInputDevice8_SetCooperativeLevel(dev, GetConsoleWindow(), DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        DIPROPDWORD bufSize = {.diph = {sizeof(bufSize), sizeof(bufSize.diph), 0, DIPH_DEVICE}, .dwData = 32};
        IDirectInputDevice8_SetProperty(dev, DIPROP_BUFFERSIZE, &bufSize.diph);
        if (SUCCEEDED(IDirectInputDevice8_Acquire(dev))) {
            for (DWORD waited = 0; waited < DI_LIVENESS_WAIT_MS && !live; waited += 20) {
                Sleep(20);
                DIDEVICEOBJECTDATA events[32];
                DWORD n = 32;
                IDirectInputDevice8_Poll(dev);
                if (SUCCEEDED(IDirectInputDevice8_GetDeviceData(dev, sizeof(DIDEVICEOBJECTDATA), events, &n, 0)) && n > 0)
                    live = 1;
            }
            IDirectInputDevice8_Unacquire(dev);
        }
    }
    IDirectInputDevice8_Release(dev);
    return live;
}

static void set_dinput_axis_range(DWORD offset, LONG lo, LONG hi) {
    DIPROPRANGE range = {.diph = {sizeof(range), sizeof(range.diph), offset, DIPH_BYOFFSET}, .lMin = lo, .lMax = hi};
    IDirectInputDevice8_SetProperty(g_di_device, DIPROP_RANGE, &range.diph);
}

static void set_dinput_deadzone_zero(DWORD offset) {
    /* No deadzone added anywhere in this relay, on any backend -- raw 1:1
       passthrough, deadzone/curves belong in the game's own settings. */
    DIPROPDWORD dz = {.diph = {sizeof(dz), sizeof(dz.diph), offset, DIPH_BYOFFSET}, .dwData = 0};
    IDirectInputDevice8_SetProperty(g_di_device, DIPROP_DEADZONE, &dz.diph);
}

/* Held open for as long as this backend is the active connection (closed
   on disconnect, see the main loop's cleanup) once setup_ds4_raw_hid()
   below finds a real Bluetooth-connected DS4 -- unlike g_di_device
   (DirectInput), which only exists per BACKEND_DINPUT session, this one
   is read directly via ReadFile, no DirectInput involved at all. */
static HANDLE g_ds4_raw_handle = INVALID_HANDLE_VALUE;
static DWORD g_ds4_raw_report_len = 0;

/* Same idea, for a Bluetooth-connected DualSense (PS5) -- separate handle
   from DS4's since a machine could plausibly have both connected. See
   setup_dualsense_raw_hid()'s comment for why this backend exists and how
   its report layout was actually confirmed (not guessed). */
static HANDLE g_dualsense_raw_handle = INVALID_HANDLE_VALUE;
static DWORD g_dualsense_raw_report_len = 0;

/* Real, write-capable handle to whichever physical Sony PS controller
   (DualSense or DS4) is currently connected, USB or Bluetooth -- separate
   concept from g_ds4_raw_handle/g_dualsense_raw_handle above (which only
   ever exist for the Bluetooth-specific raw-HID backends): a WIRED PS
   controller is normally read via DirectInput, which never hands back a
   raw HID handle at all, so this gets its own separately-opened handle in
   that case. For the two Bluetooth raw-HID backends, this instead just
   points at the SAME already-open handle (both of those already open with
   GENERIC_WRITE) -- g_ps_output_owns_handle tracks which case is active,
   so cleanup knows whether to CloseHandle this itself (wired) or leave it
   alone since some other backend's own cleanup already will (Bluetooth).
   INVALID_HANDLE_VALUE whenever the real connected controller isn't a
   Sony PS pad, or nothing is connected at all. Report layouts (both
   controllers, both connection types) confirmed on real hardware
   2026-09-08 -- see send_rumble_led_to_real_ps_controller()'s comment. */
static HANDLE g_ps_output_handle = INVALID_HANDLE_VALUE;
static int g_ps_output_is_dualsense = 0; /* 0 = DS4, 1 = DualSense -- different report layouts entirely */
static int g_ps_output_is_bt = 0;        /* 0 = USB (no CRC), 1 = Bluetooth (3-byte header + CRC32) */
static int g_ps_output_owns_handle = 0;  /* 1 = wired case, WE must CloseHandle; 0 = borrowed from a
                                              Bluetooth raw-HID backend, which closes it on its own disconnect */
static int g_ps_output_shares_input_handle = 0; /* 1 when g_ps_output_handle is the SAME handle a raw-HID
                                                     backend is also continuously ReadFile()-ing every loop
                                                     iteration (set by setup_ps_wired_raw_hid()) -- battery
                                                     polling (a control-transfer HidD_GetInputReport call) is
                                                     skipped whenever this is set, since mixing a control-
                                                     transfer request with an in-flight interrupt-transfer read
                                                     on the SAME handle from the SAME thread already destabilized
                                                     the Bluetooth case (see poll_dualsense_battery()'s own
                                                     comment) -- untested for wired USB specifically, and rumble/
                                                     LED (this session's actual priority) shouldn't wait on
                                                     proving that safe first. Rumble/LED writes are unaffected --
                                                     they're WriteFile(), not a control-transfer, matching how
                                                     wired output already worked before this session's single-
                                                     handle change. */

static DWORD g_ps_crc32_table[256];
static int g_ps_crc32_table_ready = 0;
static void ps_crc32_table_init(void) {
    if (g_ps_crc32_table_ready) return;
    for (DWORD i = 0; i < 256; i++) {
        DWORD c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        g_ps_crc32_table[i] = c;
    }
    g_ps_crc32_table_ready = 1;
}
static DWORD ps_crc32_update(DWORD crc, const BYTE *data, size_t len) {
    for (size_t i = 0; i < len; i++) crc = g_ps_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
/* Both DualSense and DS4 Bluetooth output reports need this same trailing
   checksum -- confirmed 2026-09-08 directly from the real, current
   upstream Linux kernel driver (drivers/hid/hid-playstation.c:
   PS_OUTPUT_CRC32_SEED == 0xA2), and empirically proven correct on real
   hardware for BOTH controllers over Bluetooth the same night (not just
   trusted from source reading). Algorithm: standard reflected CRC-32
   (poly 0xEDB88320, same as zlib/PNG/Ethernet), init 0xFFFFFFFF, primed
   with one virtual 0xA2 byte that is never itself transmitted, then
   continued over the real report bytes 0..(total_len-5), final result
   bit-inverted and written little-endian into the last 4 bytes. */
static void ps_append_output_crc32(BYTE *report, size_t total_len) {
    ps_crc32_table_init();
    BYTE seed = 0xA2;
    DWORD crc = ps_crc32_update(0xFFFFFFFF, &seed, 1);
    crc = ps_crc32_update(crc, report, total_len - 4);
    crc = ~crc;
    report[total_len - 4] = (BYTE)(crc & 0xFF);
    report[total_len - 3] = (BYTE)((crc >> 8) & 0xFF);
    report[total_len - 2] = (BYTE)((crc >> 16) & 0xFF);
    report[total_len - 1] = (BYTE)((crc >> 24) & 0xFF);
}

/* Sends rumble motor speeds + lightbar RGB color to whichever real Sony PS
   controller is currently connected -- DualSense or DS4, USB or
   Bluetooth, all four combinations confirmed working on real hardware
   2026-09-08. No-op if no real PS controller is connected right now.

   DualSense's flag-byte recipe (valid_flag1 == 0x55, valid_flag2 bit1,
   specific lightbar_setup/brightness/player_leds values) came from
   DS4Windows's own real, working source -- the DualSense kernel driver
   alone documents individual bits correctly, but NOT that several of
   them must be combined exactly this way; sending just the single
   documented "enable lightbar" bit, confirmed empirically, does nothing
   at all (the lightbar silently stays off). DS4's recipe is far simpler
   (one 0x07 flags byte) and also came directly from DS4Windows's source.
   Byte offsets below use `base` to express the shared "USB header is 1
   byte (report ID only), Bluetooth header is 3 bytes" pattern common to
   both controllers -- everything after the header lines up identically
   once you add `base`. motorLeft/motorRight follow XInput's own
   large/small-motor naming (left=strong, right=weak), matching the
   consistent convention across Xbox/DS4/DualSense alike. */
static void send_rumble_led_to_real_ps_controller(BYTE motorLeft, BYTE motorRight, BYTE r, BYTE g, BYTE b) {
    if (g_ps_output_handle == INVALID_HANDLE_VALUE) return;
    /* REAL BUG found and fixed 2026-09-08, on real hardware, after the
       transport switch to HidD_SetOutputReport merely traded one failure
       mode for a worse one: it stopped the connection from cycling, but
       the very next ReadFile() in the main loop's continuous BT read
       polling (poll_dualsense_raw_hid()/poll_ds4_raw_hid(), both
       synchronous/blocking, no OVERLAPPED I/O) then hung forever --
       confirmed by a live test showing the dashboard settle into a
       "connected" state that never again updated (stick/button data
       frozen at the very first read, "Updates/sec" stuck at 0.0, yet the
       process still responded to Ctrl+C, consistent with the MAIN thread
       being the one stuck, not the whole process). Whatever output
       transport is used, sending it concurrently with an in-flight
       synchronous BT read is apparently unsafe on this hardware/driver
       stack -- not just for WriteFile (the earlier finding) but for
       HidD_SetOutputReport too. Rather than keep guessing at transports,
       BT output is disabled outright for now: reliable input is far more
       important than a nice-to-have lightbar, and this project's whole
       history has been "confirm on real hardware, don't ship a guess."
       USB is unaffected -- it goes through DirectInput for input (a
       completely separate code path with no concurrent raw-HID read
       loop), so nothing here contends with anything there. Revisiting BT
       output would need the read loop switched to asynchronous
       (OVERLAPPED) I/O first, so a write can never block a pending read. */
    if (g_ps_output_is_bt) return;
    BYTE report[78] = {0};
    DWORD len;
    int base = g_ps_output_is_bt ? 3 : 1;
    if (g_ps_output_is_dualsense) {
        report[0] = g_ps_output_is_bt ? 0x31 : 0x02;
        if (g_ps_output_is_bt) { report[1] = 0x00; report[2] = 0x10; } /* seq_tag=0, tag=DS_OUTPUT_TAG */
        report[base + 0] = 0x03; /* valid_flag0: rumble enable */
        report[base + 1] = 0x55; /* valid_flag1: DS4Windows's exact confirmed-working value */
        report[base + 2] = motorRight;
        report[base + 3] = motorLeft;
        report[base + 38] = 0x02; /* valid_flag2 */
        report[base + 41] = 0x02; /* lightbar_setup */
        report[base + 42] = 0x02; /* led_brightness */
        report[base + 43] = 0x04; /* player_leds: single center LED */
        report[base + 44] = r;
        report[base + 45] = g;
        report[base + 46] = b;
        len = g_ps_output_is_bt ? 78 : 48;
    } else {
        report[0] = g_ps_output_is_bt ? 0x11 : 0x05;
        if (g_ps_output_is_bt) report[1] = 0xC0; /* 0xC0 | pollRate(0) */
        report[base + 0] = 0x07; /* flags: rumble+lightbar+flash enable (DEFAULT_OUTPUT_FEATURES) */
        report[base + 1] = 0x04; /* fixed */
        report[base + 3] = motorRight;
        report[base + 4] = motorLeft;
        report[base + 5] = r;
        report[base + 6] = g;
        report[base + 7] = b;
        len = g_ps_output_is_bt ? 78 : 32;
    }
    if (g_ps_output_is_bt) ps_append_output_crc32(report, 78);
    /* REAL BUG found and fixed 2026-09-08 via a live hardware test, after
       the "separate handle" fix alone didn't fully solve it: WriteFile()
       (an interrupt-transfer write) contending with the continuous
       interrupt-transfer ReadFile() loop poll_dualsense_raw_hid()/
       poll_ds4_raw_hid() run every main-loop iteration destabilized the
       Bluetooth connection even through a completely separate handle --
       the conflict is at the Bluetooth transport level, not the Windows
       handle level, so a second handle alone couldn't fix it. The
       existing keep-alive write (setup_dualsense_raw_hid(), sent once at
       connect, before continuous reading really ramps up) already uses
       HidD_SetOutputReport -- a control-transfer request -- and has been
       proven rock-solid across many sessions tonight specifically because
       it does NOT share a transfer type with the read loop. Switching
       every BT rumble/LED write to that same proven-safe mechanism fixed
       it. USB keeps WriteFile deliberately: HidD_SetOutputReport was
       directly confirmed to FAIL outright over USB (error 31/
       ERROR_GEN_FAILURE) during tonight's very first live test -- this
       controller's wired interface apparently doesn't support the
       Set_Report control request at all, only interrupt-transfer writes,
       and there's no concurrent USB read loop for it to contend with
       anyway (USB input goes through DirectInput instead, a completely
       separate code path). */
    if (g_ps_output_is_bt) {
        BOOL ok = HidD_SetOutputReport(g_ps_output_handle, report, (DWORD)len);
        if (!ok) log_line("ps output: BT HidD_SetOutputReport FAILED (error %lu)", (unsigned long)GetLastError());
    } else {
        DWORD written;
        BOOL ok = WriteFile(g_ps_output_handle, report, len, &written, NULL);
        if (!ok) log_line("ps output: USB WriteFile FAILED (error %lu)", (unsigned long)GetLastError());
    }
}

/* send_rumble_led_to_real_ps_controller() above always sends a FULL report
   (motors + color) every single call -- these two wrappers let rumble-
   forwarding (happens every time a game's motor values change, potentially
   often) and LED/mode-color logic (changes rarely -- only on a mode
   switch or a battery-threshold crossing) each update independently
   without one stomping the other's most recent value, by always
   re-sending the OTHER side's last-known state alongside whichever one
   actually changed. */
static BYTE g_ps_led_r = 0, g_ps_led_g = 0, g_ps_led_b = 0;
static BYTE g_ps_last_motor_left = 0, g_ps_last_motor_right = 0;

static void send_ps_rumble(BYTE motorLeft, BYTE motorRight) {
    g_ps_last_motor_left = motorLeft;
    g_ps_last_motor_right = motorRight;
    send_rumble_led_to_real_ps_controller(motorLeft, motorRight, g_ps_led_r, g_ps_led_g, g_ps_led_b);
}
static void send_ps_led(BYTE r, BYTE g, BYTE b) {
    g_ps_led_r = r;
    g_ps_led_g = g;
    g_ps_led_b = b;
    send_rumble_led_to_real_ps_controller(g_ps_last_motor_left, g_ps_last_motor_right, r, g, b);
}

/* Reads the DualSense's own current battery status via HidD_GetInputReport
   -- a synchronous, non-blocking-in-the-Sleep-forever sense "give me the
   latest cached report" control-transfer query.

   REAL BUG found and fixed 2026-09-08, caught by a live hardware test
   before ever shipping: this used to also run for the Bluetooth case,
   calling HidD_GetInputReport on g_dualsense_raw_handle -- the EXACT SAME
   handle poll_dualsense_raw_hid() is ALSO continuously reading from via
   ReadFile() every single main-loop iteration. Mixing a control-transfer
   request with an in-flight interrupt-transfer read on the same HID
   handle from the same thread caused real, observed instability on real
   hardware: a blank dashboard and a rapid connect/disconnect/reconnect
   loop, confirmed via the log showing repeated take-over/hidhide/vigem
   cycles seconds apart. Deliberately restricted to the WIRED case only
   now (g_ps_output_is_bt == 0) -- that handle is a SEPARATE one this
   project opens purely for output (see setup_dinput_device()'s comment),
   never touched by any continuous read loop, so no such conflict exists
   there. The Bluetooth case simply reports "unknown" battery for now
   rather than risk destabilizing the primary input connection again --
   a real, working feature that sometimes doesn't show a number beats an
   unstable one that shows a number but breaks the controller. DS4 is
   deliberately not handled here at all either way (see g_battery_src's
   comment for why). */
static void poll_dualsense_battery(void) {
#ifdef SAFE_MODE_NO_EXTRAS
    g_battery_src = BATTERY_SRC_NONE;
    return;
#else
    if (g_ps_output_handle == INVALID_HANDLE_VALUE || !g_ps_output_is_dualsense || g_ps_output_is_bt ||
         g_ps_output_shares_input_handle) {
        if (g_ps_output_is_bt || g_ps_output_shares_input_handle) g_battery_src = BATTERY_SRC_NONE;
        return;
    }
    BYTE buf[128] = {0};
    buf[0] = 0x01; /* HidD_GetInputReport needs the target report ID pre-filled -- USB only now, always 0x01 */
    if (!HidD_GetInputReport(g_ps_output_handle, buf, sizeof(buf))) return;
    size_t offset = 53;
    BYTE status0 = buf[offset];
    int capacityNibble = status0 & 0x0F;
    int chargingCode = (status0 >> 4) & 0x0F;
    int percent = capacityNibble * 10 + 5;
    if (percent > 100) percent = 100;
    g_battery_src = BATTERY_SRC_PERCENT;
    g_battery_percent = percent;
    g_battery_charging = (chargingCode == 0x1 || chargingCode == 0x2); /* charging, or full-while-plugged-in */
#endif
}

/* XInput's own official battery API -- real and always available for
   Xbox-style controllers (unlike LED control), just coarse: one of 4
   levels, never a percentage. Also correctly reports BATTERY_TYPE_WIRED
   for a wired pad (no meaningful "level" at all in that case -- shown as
   "wired" on the dashboard, not a fake 100%). */
static void poll_xbox_battery(int userIndex) {
#ifdef SAFE_MODE_NO_EXTRAS
    (void)userIndex;
    g_battery_src = BATTERY_SRC_NONE;
    return;
#else
    if (!pXInputGetBatteryInformation) { g_battery_src = BATTERY_SRC_NONE; return; }
    XINPUT_BATTERY_INFORMATION info;
    if (pXInputGetBatteryInformation((DWORD)userIndex, XINPUT_BATTERY_DEVTYPE_GAMEPAD, &info) != ERROR_SUCCESS) {
        g_battery_src = BATTERY_SRC_NONE;
        return;
    }
    if (info.BatteryType == XINPUT_BATTERY_TYPE_DISCONNECTED) {
        g_battery_src = BATTERY_SRC_NONE;
    } else if (info.BatteryType == XINPUT_BATTERY_TYPE_WIRED) {
        g_battery_src = BATTERY_SRC_WIRED;
    } else {
        g_battery_src = BATTERY_SRC_COARSE;
        g_battery_coarse = info.BatteryLevel;
    }
#endif
}

/* Keeps the real PS controller's lightbar in visual agreement with the
   dashboard's own mode banner -- explicit user request 2026-09-08 ("the
   color the change for hotas mode, for playstation to playstation mode
   and playstation to xbox mode"). Colors chosen to echo the dashboard's
   own existing banner colors where a direct match exists:
     - HOTAS mode: green, matching CLR_BANNER_HOTAS's green background.
     - Normal mode, acting as itself (virtual pad is DS4 -- the real PS
       controller's native identity): blue, the color most people already
       associate with a PS5 controller's own default "connected" glow.
     - Normal mode, acting as an Xbox pad (virtual pad is X360 -- the real
       PS controller standing in for a different identity than its own):
       purple, deliberately distinct from the "native" blue above so it's
       obvious at a glance which identity is currently active.
     - Mode not yet chosen: orange, echoing CLR_BANNER_UNKNOWN's red-ish
       background without fully colliding with the low-battery warning
       below (both being some shade of red/orange is an acceptable, minor
       overlap -- both are genuinely "pay attention" states).
   Low battery OVERRIDES all of the above, same as how a real controller's
   own firmware also interrupts its normal color for a battery warning --
   an urgent, actionable signal shouldn't hide behind a routine mode
   indicator. Called on every mode change (do_toggle(), which forward-
   declares this), every V-key emulation-type flip, on fresh connect, and
   after every battery poll (~once/sec) so a threshold crossing updates
   the color promptly without needing its own separate change-detection.
   A game actively driving its own lightbar color via the DS4-output-
   forwarding path (see the main loop's 15Hz block) will simply overwrite
   whatever this function last sent, and vice versa -- no arbitration
   between "mode indicator" and "game wants its own color" is implemented
   yet; keeping this simple for its first pass was an explicit tradeoff,
   not an oversight. */
static void update_ps_mode_led(void) {
#ifdef SAFE_MODE_NO_EXTRAS
    return;
#else
    if (g_ps_output_handle == INVALID_HANDLE_VALUE) return;
    if (g_battery_src == BATTERY_SRC_PERCENT && g_battery_percent <= 15 && !g_battery_charging) {
        send_ps_led(255, 0, 0);
        return;
    }
    LONG mode = g_hidden_mode;
    if (mode == 1) send_ps_led(0, 255, 0);                        /* HOTAS: green */
    else if (mode == 0 && g_vigem_emulate_ds4) send_ps_led(0, 100, 255);   /* Normal, native PS identity: blue */
    else if (mode == 0) send_ps_led(160, 0, 255);                 /* Normal, standing in as Xbox: purple */
    else send_ps_led(255, 60, 0);                                  /* mode not yet chosen: orange */
#endif
}

/* Every real DualShock 4 product ID Sony has actually shipped (original
   CUH-ZCT1, revised CUH-ZCT2, and the official USB wireless adapter's own
   PID). Needed because VID 0x054C (Sony) alone is NOT specific enough --
   a DualSense (PS5) pad and PSVR2's Sense controllers are also Sony VID
   0x054C with a >64-byte input report, and setup_ds4_raw_hid() taking one
   of those over would find report[0] is never 0x11 (DualSense's own
   confirmed input report ID is 0x01, an entirely different layout --
   see poll_sony_bt_report()), immediately fail every poll, and tear
   down + retry in a tight loop forever -- confirmed as a real risk here
   since this exact user also has a PS VR2 Sense controller that could be
   connected via Bluetooth at the same time as the real DS4. */
#define DS4_PID_V1 0x05C4
#define DS4_PID_V2 0x09CC
#define DS4_PID_USB_DONGLE 0x0BA0
static int is_ds4_product_id(USHORT pid) {
    return pid == DS4_PID_V1 || pid == DS4_PID_V2 || pid == DS4_PID_USB_DONGLE;
}

/* Over Bluetooth (never over USB/a dongle, which already sends a compact
   64-byte report DirectInput reads fine), a real DualShock 4 defaults to
   a REDUCED input report with barely any usable data -- confirmed via
   community reverse-engineering (psdevwiki.com/ps4/DS4-BT): reading
   Feature Report 0x05 once is what tells the controller to switch to
   sending the full report (0x11, 547 bytes for this specific Bluetooth
   HID profile) instead, exactly the same one-time step DS4Windows and
   similar tools perform.

   Confirmed on real hardware this switch DOES work at the device level --
   a raw ReadFile after this call shows genuinely live, changing report
   0x11 data. The problem turned out to be one level up: DirectInput's own
   HID-to-joystick translation cannot correctly read this particular
   547-byte report at all (it stayed frozen at all-zeros the whole time,
   on the very same physical device raw HID was reading live data from
   simultaneously) -- likely a legacy DirectInput limitation with
   unusually large/complex HID reports, the same reason community tools
   like DS4Windows bypass DirectInput entirely for this. So do the same
   here: if the device's declared input report is large (> 64 bytes, i.e.
   NOT the compact USB/dongle report DirectInput already handles fine),
   take over reading it directly via raw HID instead of ever handing it to
   DirectInput -- see poll_ds4_raw_hid() and BACKEND_DS4_RAWHID. Pure
   user-mode Windows HID API, no driver needed. */
/* Checks specifically for a Bluetooth-connected DS4 raw HID interface
   being present right now -- used only to decide whether a DS4 that
   DirectInput just claimed should be rejected (see the main connect loop
   below). Deliberately separate from setup_ds4_raw_hid() itself: that
   function matches a DS4 over EITHER transport (a wired DS4's compact
   <=64-byte report correctly falls through to DirectInput, which reads it
   fine -- only the large Bluetooth report is the confirmed-broken case),
   so it can't be reused here to tell transports apart. This one requires
   the Bluetooth HID service UUID in the device path, same substring check
   setup_dualsense_raw_hid() already uses for the same purpose. */
static int is_ds4_present_over_bt_once(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_A data; char buf[512]; } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;
        if (!strstr(detail.data.DevicePath, "vid_054c")) continue;
        if (!strstr(detail.data.DevicePath, "00805f9b34fb")) continue; /* Bluetooth only */
        HANDLE h = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        if (HidD_GetAttributes(h, &attrs) && attrs.VendorID == 0x054C && is_ds4_product_id(attrs.ProductID)) found = 1;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

/* REAL BUG found on real hardware 2026-09-08, via a direct log comparison:
   at the exact moment DirectInput has just grabbed a DS4, a single
   SetupDi enumeration from THIS process sometimes doesn't yet see the
   real Bluetooth raw-HID interface -- confirmed by the SAME connection
   moment's HidHide registration (a separate call, via a freshly-spawned
   HidHideCLI.exe subprocess, happening a bit later) correctly finding the
   real Bluetooth device that this function's own enumeration had just
   missed. A brief, real device-enumeration lag specific to a single
   snapshot-in-time SetupDi call, not a controller-side mode-switch delay
   (that theory was checked against DS4Windows's real source and doesn't
   hold up -- see PROGRESS.md). Retrying a few times with a short pause
   gives Windows' device tree the same bit of extra time the separate
   HidHideCLI.exe call incidentally got by virtue of running slightly
   later. */
static int is_ds4_present_over_bt(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (is_ds4_present_over_bt_once()) return 1;
        if (attempt < 3) Sleep(150);
    }
    return 0;
}

static int setup_ds4_raw_hid(void) {
    if (g_ds4_raw_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ds4_raw_handle);
        g_ds4_raw_handle = INVALID_HANDLE_VALUE;
    }
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;

    int found = 0;
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A data;
            char buf[512]; /* fixed-size stack buffer instead of a two-pass malloc -- any real device path fits comfortably */
        } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;

        /* Cheap path pre-filter before ever opening the device -- confirmed
           via code audit 2026-09-07 that this loop was calling CreateFileA
           on EVERY HID device on the machine (keyboards, mice, every
           composite HID collection) 4 times a second for as long as no
           controller is connected, since it only checked VID/PID after
           opening. Device interface paths always carry "vid_xxxx&pid_xxxx"
           verbatim, so this skips straight past anything that can't
           possibly be Sony hardware without ever touching it. */
        if (!strstr(detail.data.DevicePath, "vid_054c")) continue;

        HANDLE h = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        if (HidD_GetAttributes(h, &attrs) && attrs.VendorID == 0x054C && is_ds4_product_id(attrs.ProductID)) { /* Sony, and specifically a DS4 */
            BYTE feature[64] = {0x05}; /* report ID 0x05 -- ReportFeatureInCalibrateBT */
            BOOL featureOk = HidD_GetFeature(h, feature, sizeof(feature)); /* result checked indirectly via the report length below */

            PHIDP_PREPARSED_DATA preparsed = NULL;
            HIDP_CAPS caps = {0};
            BOOL gotCaps = HidD_GetPreparsedData(h, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
            log_line("setup_ds4_raw_hid: candidate %s PID=0x%04X featureOk=%d gotCaps=%d InputReportByteLength=%lu",
                      detail.data.DevicePath, attrs.ProductID, featureOk, gotCaps,
                      gotCaps ? (unsigned long)caps.InputReportByteLength : 0);
            if (gotCaps && caps.InputReportByteLength > 64 && caps.InputReportByteLength <= 1024) {
                g_ds4_raw_handle = h;
                g_ds4_raw_report_len = caps.InputReportByteLength;
                g_dinput_vid_pid = ((DWORD)attrs.ProductID << 16) | attrs.VendorID; /* shared with describe_controller()'s model lookup */
                /* REAL BUG found and fixed 2026-09-08, on real hardware, after
                   two other attempts (reusing this same handle for output;
                   then a separate handle opened purely for output) both
                   failed differently -- the first destabilized the BT
                   connection outright, the second didn't destabilize it but
                   caused the ongoing ReadFile() loop below to silently hang
                   forever after the first successful read (confirmed: no
                   disconnect was ever logged, yet stick/button data froze at
                   the very first value and never updated again, while the
                   process still responded fine to Ctrl+C -- consistent with
                   only the main thread's blocking read being stuck). Merely
                   having a SECOND open handle to this same Bluetooth HID
                   collection is enough to break the primary read handle's
                   behavior on this hardware/driver stack, even with zero I/O
                   ever issued on that second handle. Given how much real
                   testing went into finding this, BT rumble/LED/battery for
                   DS4 is deliberately NOT implemented at all -- g_ps_output_handle
                   simply stays INVALID_HANDLE_VALUE for this backend,
                   restoring the exact single-handle behavior this Bluetooth
                   path has always used, successfully, before tonight. USB is
                   unaffected (see send_rumble_led_to_real_ps_controller()'s
                   comment) and remains fully functional. */
                log_line("ds4rawhid: took over Sony device %s (VID_054C PID_%04X), report length %lu",
                          detail.data.DevicePath, attrs.ProductID, (unsigned long)caps.InputReportByteLength);
                found = 1;
            }
            if (preparsed) HidD_FreePreparsedData(preparsed);
        }
        if (!found) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

/* Diagnostic-only, not wired into any normal relay path -- the same
   methodical, capture-real-data-first approach that pinned down DS4's
   Bluetooth report layout (see setup_ds4_raw_hid()'s comment), now for
   DualSense (PS5). NOT run automatically: DualSense is deliberately
   excluded from setup_ds4_raw_hid()'s own PID whitelist (its confirmed
   input report ID is 0x01, not DS4's 0x11, and blindly taking it over
   caused a real connect/disconnect flap bug earlier the same day this
   was written).
   This prints every raw report the controller sends, as hex, so real
   button/axis presses can be matched to byte offsets by eye -- no
   layout assumptions baked in at all. Self-invoked via
   --capture-dualsense-raw. Deliberately does NOT send DS4's Feature
   Report 0x05 wake-up probe (see the actual capture loop below for why:
   confirmed on real hardware that DualSense doesn't recognize it and
   that sending it can destabilize the connection) -- InputReportByteLength
   already comes back as a real, full value without needing one. */
static int capture_dualsense_raw(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevsA failed.\n");
        return 0;
    }

    HANDLE h = INVALID_HANDLE_VALUE;
    char foundPath[512] = {0};
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData);
          i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A data;
            char buf[512];
        } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;

        HANDLE cand = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (cand == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        if (HidD_GetAttributes(cand, &attrs) && attrs.VendorID == 0x054C && attrs.ProductID == 0x0CE6) {
            h = cand;
            strncpy(foundPath, detail.data.DevicePath, sizeof(foundPath) - 1);
        } else {
            CloseHandle(cand);
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (h == INVALID_HANDLE_VALUE) {
        printf("DualSense (VID_054C PID_0CE6) not found via raw HID enumeration.\n");
        printf("Make sure it's connected (wired or paired over Bluetooth) and try again.\n");
        return 0;
    }
    printf("Found DualSense at: %s\n", foundPath);
    log_line("dualsense capture: found device %s", foundPath);

    PHIDP_PREPARSED_DATA preparsed = NULL;
    HIDP_CAPS caps = {0};
    if (HidD_GetPreparsedData(h, &preparsed)) {
        HidP_GetCaps(preparsed, &caps);
        HidD_FreePreparsedData(preparsed);
    }
    printf("InputReportByteLength = %lu\n", (unsigned long)caps.InputReportByteLength);
    log_line("dualsense capture: InputReportByteLength=%lu", (unsigned long)caps.InputReportByteLength);

    /* Deliberately NOT sending DS4's Feature Report 0x05 wake-up probe
       here -- confirmed on real hardware 2026-09-07 that DualSense
       doesn't recognize it (HidD_GetFeature returned FALSE) AND that
       sending it appears to destabilize the Bluetooth connection enough
       to cause a disconnect. InputReportByteLength already came back as
       a real, non-reduced value (78 bytes, matching DualSense's
       documented Bluetooth extended report size) without needing any
       wake-up at all, so this device may simply not need one -- another
       reason not to guess and just poke it anyway.
*/

    printf("Reading raw reports until you stop it -- no countdown, take your time. Move both sticks\n");
    printf("fully in every direction, pull both triggers, press every single button (face buttons,\n");
    printf("shoulders, stick clicks, PS, touchpad, mute) one at a time with a pause between each. Only\n");
    printf("CHANGED reports are shown/logged. Press Ctrl+C when you're done -- full detail is written\n");
    printf("to %s.\n\n", INSTALL_LOG_FILE);

    BYTE report[1024];
    DWORD reportLen = caps.InputReportByteLength;
    if (reportLen == 0 || reportLen > sizeof(report)) reportLen = sizeof(report);

    BYTE lastReport[1024] = {0};
    int haveLast = 0;
    int lineNum = 0;

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(h, report, reportLen, &read, NULL) || read == 0) {
            /* Confirmed a real, silent failure mode on 2026-09-07: this
               loop can spin indefinitely without a single successful read
               and give zero indication why. Log the real Win32 error, but
               only once, so a genuine failure is diagnosable without
               flooding the log if it happens on every single iteration. */
            static int logged_read_failure = 0;
            if (!logged_read_failure) {
                DWORD err = GetLastError();
                printf("ReadFile failed (error %lu, requested %lu bytes) -- will keep retrying silently for "
                        "as long as this keeps running.\n", (unsigned long)err, (unsigned long)reportLen);
                log_line("dualsense capture: ReadFile failed, error=%lu requested=%lu", (unsigned long)err,
                          (unsigned long)reportLen);
                logged_read_failure = 1;
            }
            continue;
        }
        if (haveLast && read <= sizeof(lastReport) && memcmp(report, lastReport, read) == 0) continue;
        memcpy(lastReport, report, read < sizeof(lastReport) ? read : sizeof(lastReport));
        haveLast = 1;
        lineNum++;

        char hexbuf[1024 * 3] = {0};
        size_t pos = 0;
        for (DWORD b = 0; b < read && pos + 4 < sizeof(hexbuf); b++) {
            pos += (size_t)snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02X ", report[b]);
        }
        printf("[%4d] (%3u bytes) %s\n", lineNum, (unsigned)read, hexbuf);
        log_line("dualsense capture #%d (%u bytes): %s", lineNum, (unsigned)read, hexbuf);
    }

    CloseHandle(h);
    printf("\nDone capturing (%d distinct reports seen). Full log: %s\n", lineNum, INSTALL_LOG_FILE);
    return 1;
}

/* Diagnostic-only, standalone (--capture-gamesir-raw) -- the same
   methodical, real-data-first approach used for DS4 and DualSense, now
   for the GameSir over a DIRECT Bluetooth connection (not its XInput
   dongle). Confirmed on real hardware 2026-09-07 that GameSir-over-
   Bluetooth hits the exact same failure DS4 originally had: DirectInput
   connects but freezes at all-zero data forever (`dinput raw: X=0 Y=0
   Z=0 ... btns0-19=00000000000000000000` never changing), rather than
   returning live data. This bypasses DirectInput entirely to read the
   real HID reports directly and find out what's actually in them.

   Unlike the DS4/DualSense capture tools, this one does NOT hardcode a
   specific Product ID -- GameSir's dongle PID (0x100A) was confirmed
   long ago, but its Bluetooth-direct PID has never been confirmed and
   may differ, so this matches on Vendor ID (0x3537) alone and prints
   whatever Product ID it actually finds. */
static int capture_gamesir_raw(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevsA failed.\n");
        return 0;
    }

    /* A Bluetooth composite HID device (like this one) often exposes
       SEVERAL separate HID interfaces at once -- confirmed necessary to
       check for real 2026-09-07 after the first version of this tool
       silently grabbed just one candidate and it turned out to be an
       11-byte reduced report with no room for paddle button data at all.
       List every VID_3537 interface found, with its own report length,
       instead of guessing which one is "the" GameSir interface -- then
       read from whichever has the LARGEST InputReportByteLength, since
       that's the one most likely to actually carry full button data. */
    HANDLE h = INVALID_HANDLE_VALUE;
    char foundPath[512] = {0};
    USHORT foundPid = 0;
    DWORD foundReportLen = 0;
    int candidateCount = 0;
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A data;
            char buf[512];
        } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;

        HANDLE cand = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (cand == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        if (!HidD_GetAttributes(cand, &attrs) || attrs.VendorID != 0x3537) {
            CloseHandle(cand);
            continue;
        }

        DWORD candReportLen = 0;
        PHIDP_PREPARSED_DATA candPreparsed = NULL;
        HIDP_CAPS candCaps = {0};
        if (HidD_GetPreparsedData(cand, &candPreparsed)) {
            HidP_GetCaps(candPreparsed, &candCaps);
            candReportLen = candCaps.InputReportByteLength;
            HidD_FreePreparsedData(candPreparsed);
        }

        candidateCount++;
        int isBluetooth = strstr(detail.data.DevicePath, "00805f9b34fb") != NULL;
        printf("Found interface #%d: PID_%04X, %s, InputReportByteLength=%lu, %s\n", candidateCount,
                attrs.ProductID, isBluetooth ? "Bluetooth" : "not Bluetooth (wired/dongle)",
                (unsigned long)candReportLen, detail.data.DevicePath);
        log_line("gamesir capture: interface #%d PID_%04X %s len=%lu path=%s", candidateCount, attrs.ProductID,
                  isBluetooth ? "Bluetooth" : "wired", (unsigned long)candReportLen, detail.data.DevicePath);

        if (candReportLen > foundReportLen) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            h = cand;
            foundPid = attrs.ProductID;
            foundReportLen = candReportLen;
            strncpy(foundPath, detail.data.DevicePath, sizeof(foundPath) - 1);
        } else {
            CloseHandle(cand);
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (h == INVALID_HANDLE_VALUE) {
        printf("GameSir (VID_3537) not found via raw HID enumeration.\n");
        printf("Make sure it's connected directly over Bluetooth (not its USB dongle) and try again.\n");
        return 0;
    }
    printf("\nReading from the largest interface found: PID_%04X, %lu bytes, %s\n\n", foundPid,
            (unsigned long)foundReportLen, foundPath);
    log_line("gamesir capture: reading from PID_%04X len=%lu path=%s (largest of %d candidates)", foundPid,
              (unsigned long)foundReportLen, foundPath, candidateCount);

    printf("Reading raw reports until you stop it -- no countdown, take your time. Move both sticks\n");
    printf("fully in every direction, pull both triggers, press every regular button, AND the four\n");
    printf("paddle buttons (L4/L5/R4/R5) one at a time with a pause between each. Only CHANGED reports\n");
    printf("are shown/logged. Press Ctrl+C when you're done -- full detail is written to %s.\n\n",
            INSTALL_LOG_FILE);

    BYTE report[1024];
    DWORD reportLen = foundReportLen;
    if (reportLen == 0 || reportLen > sizeof(report)) reportLen = sizeof(report);

    BYTE lastReport[1024] = {0};
    int haveLast = 0;
    int lineNum = 0;

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(h, report, reportLen, &read, NULL) || read == 0) {
            static int logged_read_failure = 0;
            if (!logged_read_failure) {
                DWORD err = GetLastError();
                printf("ReadFile failed (error %lu, requested %lu bytes) -- will keep retrying silently for "
                        "as long as this keeps running.\n", (unsigned long)err, (unsigned long)reportLen);
                log_line("gamesir capture: ReadFile failed, error=%lu requested=%lu", (unsigned long)err,
                          (unsigned long)reportLen);
                logged_read_failure = 1;
            }
            continue;
        }
        if (haveLast && read <= sizeof(lastReport) && memcmp(report, lastReport, read) == 0) continue;
        memcpy(lastReport, report, read < sizeof(lastReport) ? read : sizeof(lastReport));
        haveLast = 1;
        lineNum++;

        char hexbuf[1024 * 3] = {0};
        size_t pos = 0;
        for (DWORD b = 0; b < read && pos + 4 < sizeof(hexbuf); b++) {
            pos += (size_t)snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02X ", report[b]);
        }
        printf("[%4d] (%3u bytes) %s\n", lineNum, (unsigned)read, hexbuf);
        log_line("gamesir capture #%d (%u bytes): %s", lineNum, (unsigned)read, hexbuf);
    }

    CloseHandle(h);
    printf("\nDone capturing (%d distinct reports seen). Full log: %s\n", lineNum, INSTALL_LOG_FILE);
    return 1;
}

/* Diagnostic-only, standalone (--test-dualsense-keepalive) -- tests
   whether sending a single output report right after connecting is
   enough to stop DualSense's own firmware from dropping the Bluetooth
   link into a low-power idle state after a few seconds on battery power
   (confirmed real behavior 2026-09-07: pressing PS wakes it for ~5s,
   then it goes idle again with nothing using it -- see poll_dualsense_
   raw_hid()'s comment). Does NOT assume any particular output report
   content is correct -- Sony's actual Bluetooth output report for this
   family of controllers is documented (community reverse-engineering,
   not this project's own testing) to need a CRC32 trailer or the
   controller ignores it outright, which is real, additional work not
   attempted here yet. This tool only tests the much narrower, cheaper
   question first: does ANY correctly-sized output report on the right
   report ID (0x31, matching Sony's own convention for this report
   family) extend the connection's alive time at all, even with all-zero
   payload the controller may or may not act on -- before investing in
   getting the full command format (rumble/LED) and its CRC exactly
   right. Prints the real OutputReportByteLength from the device itself
   first (read-only, no risk) rather than assuming a size. */
/* test_dualsense_keepalive()'s read loop used to check its own 60-second
   bound at the TOP of each iteration, meaning a controller that connects
   but then sends nothing at all would leave the blocking ReadFile call
   hanging forever with no way to reach that check again -- the exact
   "silent failure mode" class of bug this whole project has already hit
   and fixed twice today (the restore-point calls, and the capture tools'
   ReadFile error logging). Fixed the same way: the actual read loop runs
   on its own thread, and the caller enforces a real wall-clock timeout
   via WaitForSingleObject on the thread handle itself, so this can never
   hang the whole tool again regardless of what the controller does. */
typedef struct {
    HANDLE h;
    DWORD readLen;
    int gotAny;
    int reachedFullWindow;
    double failedAfterSec;
    double failedSinceGoodSec;
    DWORD failError;
} KeepaliveReadArgs;

static DWORD WINAPI keepalive_read_thread(LPVOID arg) {
    KeepaliveReadArgs *a = (KeepaliveReadArgs *)arg;
    BYTE report[1024];
    LARGE_INTEGER freq, start, lastGood, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    lastGood = start;
    for (;;) {
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - start.QuadPart) / freq.QuadPart > 60.0) {
            a->reachedFullWindow = 1;
            return 0;
        }
        DWORD read = 0;
        if (!ReadFile(a->h, report, a->readLen, &read, NULL) || read == 0) {
            a->failedAfterSec = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
            a->failedSinceGoodSec = (double)(now.QuadPart - lastGood.QuadPart) / freq.QuadPart;
            a->failError = GetLastError();
            return 0;
        }
        a->gotAny = 1;
        lastGood = now;
    }
}

static int test_dualsense_keepalive(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevsA failed.\n");
        return 0;
    }

    HANDLE h = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData);
          i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A data;
            char buf[512];
        } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;
        if (!strstr(detail.data.DevicePath, "00805f9b34fb")) continue; /* Bluetooth only, same as the real backend */

        HANDLE cand = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (cand == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        if (HidD_GetAttributes(cand, &attrs) && attrs.VendorID == 0x054C && attrs.ProductID == 0x0CE6) {
            h = cand;
        } else {
            CloseHandle(cand);
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (h == INVALID_HANDLE_VALUE) {
        printf("DualSense (Bluetooth) not found.\n");
        return 0;
    }

    PHIDP_PREPARSED_DATA preparsed = NULL;
    HIDP_CAPS caps = {0};
    if (HidD_GetPreparsedData(h, &preparsed)) {
        HidP_GetCaps(preparsed, &caps);
        HidD_FreePreparsedData(preparsed);
    }
    printf("InputReportByteLength=%lu OutputReportByteLength=%lu FeatureReportByteLength=%lu\n",
            (unsigned long)caps.InputReportByteLength, (unsigned long)caps.OutputReportByteLength,
            (unsigned long)caps.FeatureReportByteLength);
    log_line("dualsense keepalive: InputReportByteLength=%lu OutputReportByteLength=%lu "
              "FeatureReportByteLength=%lu",
              (unsigned long)caps.InputReportByteLength, (unsigned long)caps.OutputReportByteLength,
              (unsigned long)caps.FeatureReportByteLength);

    if (caps.OutputReportByteLength > 0 && caps.OutputReportByteLength <= 1024) {
        BYTE outReport[1024] = {0};
        outReport[0] = 0x31; /* Sony's documented Bluetooth output report ID for this controller family --
                                  payload left all-zero on purpose, this test isn't trying to trigger rumble/LED
                                  yet, only testing whether the write itself extends the connection */
        BOOL wrote = HidD_SetOutputReport(h, outReport, caps.OutputReportByteLength);
        DWORD werr = wrote ? 0 : GetLastError();
        printf("HidD_SetOutputReport(report 0x31, %lu bytes) = %s%s\n",
                (unsigned long)caps.OutputReportByteLength, wrote ? "TRUE" : "FALSE",
                wrote ? "" : " (see error code below)");
        if (!wrote) printf("  GetLastError() = %lu\n", (unsigned long)werr);
        log_line("dualsense keepalive: HidD_SetOutputReport = %s (error %lu)", wrote ? "TRUE" : "FALSE",
                  (unsigned long)werr);
    } else {
        printf("OutputReportByteLength looks invalid (%lu) -- skipping the write test.\n",
                (unsigned long)caps.OutputReportByteLength);
    }

    printf("\nNow reading for up to 60 seconds with NO further PS-button presses -- just watching how\n");
    printf("long the connection stays alive on its own after that one output write.\n\n");

    /* Heap-allocated, not a stack local -- same reasoning as
       RestorePointCallArgs (see begin_system_restore_point()'s comment):
       if the wait below times out, this function keeps running with the
       thread abandoned, and a stack-local `args` would be reused by
       whatever this function calls next by the time ReadFile's blocking
       call finally returns. Freed on the normal-completion path; leaked
       on the abandon path on purpose. `h` itself is leaked too in that
       case for the same reason -- the abandoned thread may still be
       blocked inside ReadFile(h, ...), and closing/reusing that handle
       out from under it is worse than a one-time diagnostic-tool leak. */
    KeepaliveReadArgs *args = calloc(1, sizeof(*args));
    if (!args) {
        printf("Out of memory.\n");
        CloseHandle(h);
        return 0;
    }
    args->h = h;
    args->readLen = caps.InputReportByteLength;
    if (args->readLen == 0 || args->readLen > 1024) args->readLen = 1024;

    HANDLE thread = CreateThread(NULL, 0, keepalive_read_thread, args, 0, NULL);
    if (!thread) {
        printf("Could not start the read-test thread (error %lu).\n", (unsigned long)GetLastError());
        free(args);
        CloseHandle(h);
        return 0;
    }
    /* A little more than the 60s test window itself, so a genuinely
       silent controller (the exact case that used to hang forever) is
       reported as a real, bounded failure instead of hanging this tool
       too. */
    DWORD waitResult = WaitForSingleObject(thread, 65000);
    CloseHandle(thread);

    if (waitResult != WAIT_OBJECT_0) {
        printf("The read loop did not finish in time -- the controller appears to have stopped sending\n");
        printf("any data at all (a true hang, not just a failed read). Treating this as a failure.\n");
        log_line("dualsense keepalive: read thread did not complete in time -- likely a fully silent controller");
        return 1; /* args and h intentionally NOT freed/closed here -- see the allocation comment above */
    }

    if (args->reachedFullWindow) {
        printf("Reached 60 seconds still reading successfully -- the output write appears to have worked!\n");
        log_line("dualsense keepalive: SUCCESS -- stayed alive the full 60s test window");
    } else {
        printf("ReadFile failed after %.1fs (%.1fs since the last good read, error %lu) -- connection\n",
                args->failedAfterSec, args->failedSinceGoodSec, (unsigned long)args->failError);
        printf("appears to have gone idle despite the output write.\n");
        log_line("dualsense keepalive: FAILED after %.1fs (%.1fs since last good read, error %lu)",
                  args->failedAfterSec, args->failedSinceGoodSec, (unsigned long)args->failError);
    }
    if (!args->gotAny) printf("Never got a single successful read at all.\n");

    free(args);
    CloseHandle(h);
    return 1;
}

/* 0-255 centered at 128 -> signed 16-bit centered at 0, matching
   XINPUT_GAMEPAD's thumbstick range. */
/* invert=1 for the Y axes (raw HID Y grows downward like DirectInput's;
   XInput's grows upward, same reasoning as dinput_to_xinput_gamepad()).
   Computing then clamping in a wider `int` -- NOT just negating a `SHORT`
   directly -- matters here: v=0 scales to exactly -32768 (SHORT_MIN), and
   negating THAT in 16 bits overflows and wraps back around to -32768
   instead of +32768 (itself one past SHORT_MAX, so it needs clamping to
   +32767 anyway). Confirmed on real hardware as the exact cause of one
   stick direction snapping to the wrong extreme instead of the correct
   one. */
static SHORT ds4_axis_to_i16(BYTE v, int invert) {
    int scaled = ((int)v - 128) * 256;
    if (invert) scaled = -scaled;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (SHORT)scaled;
}

/* DS4's D-pad is a 4-bit hat switch (0=up, going clockwise, 8=released),
   same convention pov_to_dpad() decodes from DirectInput's POV degrees --
   this is the same information in a much simpler raw form, no degrees
   math needed. */
static void ds4_hat_to_dpad(BYTE hat, WORD *buttons) {
    switch (hat & 0x0F) {
        case 0: *buttons |= XINPUT_GAMEPAD_DPAD_UP; break;
        case 1: *buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 2: *buttons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 3: *buttons |= XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 4: *buttons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 5: *buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 6: *buttons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 7: *buttons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP; break;
        default: break; /* 8 = released */
    }
}

/* Shared parsing for BOTH Sony raw-HID backends (DS4 and DualSense) --
   confirmed via code audit 2026-09-07 that their reports are byte-
   identical in layout, differing only by a constant offset: DS4's report
   carries a 2-byte Bluetooth-specific header before the data DualSense's
   own report starts with immediately. `base` is the offset of the
   left-stick-X byte (3 for DS4, 1 for DualSense); every other field below
   is relative to that same base, so this one function's field layout IS
   the documentation now, instead of two functions a reader has to diff
   by hand to notice they're the same 9 fields shifted by 2. See
   poll_ds4_raw_hid()/poll_dualsense_raw_hid()'s own thin wrappers below
   for each controller's specific report ID and confirmed-real-hardware
   history. */
static int poll_sony_bt_report(HANDLE h, DWORD report_len, BYTE expect_report_id, int base, XINPUT_GAMEPAD *gp) {
    if (h == INVALID_HANDLE_VALUE) return 0;
    static BYTE report[1024];
    DWORD read = 0;
    if (!ReadFile(h, report, report_len, &read, NULL) || read == 0) return 0;
    /* Detects "the controller went away/idle but ReadFile keeps returning
       the same cached report forever" -- a real, confirmed behavior on
       this hardware (the software kept saying "connected" for a
       controller that had been physically powered off, with no error,
       ever). Pure computation only: no new syscalls, no threads, no
       overlapped I/O. If the full report is byte-for-byte identical to
       the previous one for longer than any real controller would ever
       hold completely still, treat it exactly like a failed read so the
       caller's existing "poll failed, treat as disconnect" path kicks in
       and the relay cycles back to waiting -- picking the controller back
       up automatically on the next successful reconnect. */
#define BT_STALE_CONTENT_TIMEOUT_MS 3000
    static BYTE lastContent[1024];
    static DWORD lastContentLen = 0;
    static ULONGLONG lastChangeTick = 0;
    ULONGLONG now = GetTickCount64();
    if (lastChangeTick == 0 || read != lastContentLen || memcmp(report, lastContent, read) != 0) {
        memcpy(lastContent, report, read);
        lastContentLen = read;
        lastChangeTick = now;
    } else if (now - lastChangeTick > BT_STALE_CONTENT_TIMEOUT_MS) {
        log_line("poll_sony_bt_report: content identical for over %lu ms -- treating as disconnect",
                  (unsigned long)BT_STALE_CONTENT_TIMEOUT_MS);
        lastChangeTick = 0;
        return 0;
    }
    /* `report` is `static` (persists across calls) -- without this check, a
       short read (fewer bytes than base+8+1, the highest offset touched
       below) would read stale bytes from the PREVIOUS successful report as
       this frame's trigger data instead of failing cleanly. Caught in the
       2026-09-08 pre-ship audit; not observed in practice on real hardware,
       but cheap insurance since a partial-but-"successful" ReadFile is
       possible in principle for a HID device. */
    if (read < (DWORD)(base + 9)) return 0;
    if (report[0] != expect_report_id) return 0; /* not the format we expect -- treat as a hiccup, same as a failed poll */

    memset(gp, 0, sizeof(*gp));
    gp->sThumbLX = ds4_axis_to_i16(report[base + 0], 0);
    gp->sThumbLY = ds4_axis_to_i16(report[base + 1], 1);
    gp->sThumbRX = ds4_axis_to_i16(report[base + 2], 0);
    gp->sThumbRY = ds4_axis_to_i16(report[base + 3], 1);
    gp->bLeftTrigger = report[base + 7];
    gp->bRightTrigger = report[base + 8];

    ds4_hat_to_dpad(report[base + 4], &gp->wButtons);
    if (report[base + 4] & 0x10) gp->wButtons |= XINPUT_GAMEPAD_X; /* Square */
    if (report[base + 4] & 0x20) gp->wButtons |= XINPUT_GAMEPAD_A; /* Cross */
    if (report[base + 4] & 0x40) gp->wButtons |= XINPUT_GAMEPAD_B; /* Circle */
    if (report[base + 4] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_Y; /* Triangle */
    if (report[base + 5] & 0x01) gp->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;  /* L1 */
    if (report[base + 5] & 0x02) gp->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; /* R1 */
    if (report[base + 5] & 0x10) gp->wButtons |= XINPUT_GAMEPAD_BACK;  /* Share */
    if (report[base + 5] & 0x20) gp->wButtons |= XINPUT_GAMEPAD_START; /* Options */
    if (report[base + 5] & 0x40) gp->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;  /* L3 */
    if (report[base + 5] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; /* R3 */

    g_ps_pressed = (report[base + 6] & 0x01) != 0;
    g_touchpad_pressed = (report[base + 6] & 0x02) != 0;
    return 1;
}

/* Reads one report directly off g_ds4_raw_handle -- no DirectInput
   involved at all, see setup_ds4_raw_hid()'s comment for why. Byte
   offsets (relative to base=3, see poll_sony_bt_report()) are this
   Bluetooth extended report's own layout (confirmed against real
   captured data: sticks resting at ~0x80 near the expected center,
   dpad+face-button byte reading 0x08 = "neutral" at idle, a visibly
   incrementing counter proving this is genuinely live data) -- the same
   layout as DS4's well-documented USB report, just shifted +2 bytes for
   this report's own 2-byte Bluetooth-specific header (byte 0 = report ID
   0x11, bytes 1-2 = header). */
static int poll_ds4_raw_hid(XINPUT_GAMEPAD *gp) {
    return poll_sony_bt_report(g_ds4_raw_handle, g_ds4_raw_report_len, 0x11, 3, gp);
}

/* Wired (USB) DS4, single raw-HID handle shared for both input (here) and
   output (send_rumble_led_to_real_ps_controller() via g_ps_output_handle
   -- see setup_ps_wired_raw_hid()'s comment for why this replaces
   DirectInput for wired PS controllers specifically: DS4Windows-style
   "one handle, do both" instead of a second handle alongside DirectInput,
   which was already confirmed on real hardware to break DirectInput's own
   read (see setup_dinput_device()'s comment). Standard, well-documented
   DS4 USB input report: ID 0x01, no Bluetooth-specific header -- exactly
   poll_ds4_raw_hid()'s own Bluetooth layout minus the 2-byte header
   (base 1 instead of 3), reusing the SAME shared field parser since the
   two reports are otherwise byte-identical (see poll_sony_bt_report()'s
   comment). g_ds4_raw_handle/g_ds4_raw_report_len are the same globals
   the Bluetooth backend uses -- only one of the two backends is ever
   active for a given connection, so there's no conflict reusing them. */
static int poll_ds4_wired_raw_hid(XINPUT_GAMEPAD *gp) {
    return poll_sony_bt_report(g_ds4_raw_handle, g_ds4_raw_report_len, 0x01, 1, gp);
}

/* DualSense (PS5) over Bluetooth, the same general idea as DS4's raw-HID
   backend above but a DIFFERENT report entirely -- DualSense does not
   speak DS4's protocol, and unlike DS4, DirectInput's own read of a
   DualSense actually seems to work at first before going stale/idle (a
   real, separate quirk -- see the connection-timeout comment on
   poll_dualsense_raw_hid() below), so this exists to get a reliable,
   direct read rather than to work around a DirectInput parsing failure.

   Report layout confirmed 2026-09-07 via capture_dualsense_raw() against
   real hardware (see PROGRESS.md) -- NOT guessed, NOT assumed from DS4's
   layout even though it turns out to closely match it:
     byte 0: report ID (0x01)
     bytes 1-4: left stick X/Y, right stick X/Y (0-255, centered ~128)
     byte 5: D-pad hat in the low nibble (0-8, same convention as DS4's),
             face buttons in the high nibble (Square=0x10, Cross=0x20,
             Circle=0x40, Triangle=0x80)
     byte 6: L1=0x01, R1=0x02, (L2/R2 digital click bits also live here but
             unused, same as DS4 -- analog threshold already drives that),
             Share=0x10, Options=0x20, L3=0x40, R3=0x80
     byte 7: PS=bit0, Touchpad click=bit1, report counter in the upper 6
             bits (increments by 4 per report -- confirmed genuinely live
             data, not a stuck/cached read)
     bytes 8-9: left/right trigger analog (0-255)

   Deliberately does NOT send DS4's Feature Report 0x05 wake-up probe --
   confirmed on real hardware that DualSense doesn't recognize it AND that
   sending it can destabilize the Bluetooth connection. InputReportByteLength
   already comes back as a real, full value (78 bytes) without it. */
static int setup_dualsense_raw_hid(void) {
    if (g_dualsense_raw_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_dualsense_raw_handle);
        g_dualsense_raw_handle = INVALID_HANDLE_VALUE;
    }
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;

    int found = 0;
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A data;
            char buf[512];
        } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;

        /* Only the Bluetooth HID collection, not the wired one -- a wired
           DualSense already works fine through DirectInput (confirmed by
           the user on real hardware the same day this was written), so
           this only needs to take over the Bluetooth path. Bluetooth HID
           device paths on Windows embed the standard Bluetooth HID
           service class GUID ("0000112...-...-...-00805f9b34fb"); a wired
           USB path never contains that substring. */
        if (!strstr(detail.data.DevicePath, "00805f9b34fb")) continue;

        HANDLE h = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        if (HidD_GetAttributes(h, &attrs) && attrs.VendorID == 0x054C && attrs.ProductID == 0x0CE6) {
            PHIDP_PREPARSED_DATA preparsed = NULL;
            HIDP_CAPS caps = {0};
            if (HidD_GetPreparsedData(h, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                caps.InputReportByteLength > 0 && caps.InputReportByteLength <= 1024) {
                /* THE keep-alive fix, confirmed on real hardware 2026-09-07
                   (test_dualsense_keepalive(), a full 60-second test window
                   with zero drops): without this, DualSense's own firmware
                   drops the Bluetooth link into a low-power idle state
                   after a few seconds if nothing ever writes back to it --
                   confirmed directly by the user (PS button wakes the
                   lightbar for ~5s, then it goes dim/idle again with
                   nothing using it). A single output report, report ID
                   0x31 (Sony's documented Bluetooth output report ID for
                   this controller family), all-zero payload -- not trying
                   to control rumble/LED yet, purely "claiming" the
                   connection -- is enough to stop that. Best-effort: if
                   this write fails for some reason, still proceed with
                   the connection rather than refuse to use the controller
                   at all -- worst case is the same pre-existing idle-
                   after-a-few-seconds behavior, not a new failure. */
                if (caps.OutputReportByteLength > 0 && caps.OutputReportByteLength <= 1024) {
                    BYTE keepAlive[1024] = {0};
                    keepAlive[0] = 0x31;
                    BOOL ok = HidD_SetOutputReport(h, keepAlive, caps.OutputReportByteLength);
                    log_line("dualsense rawhid: keep-alive output report (%lu bytes) = %s",
                              (unsigned long)caps.OutputReportByteLength, ok ? "TRUE" : "FALSE");
                }

                g_dualsense_raw_handle = h;
                g_dualsense_raw_report_len = caps.InputReportByteLength;
                g_dinput_vid_pid = ((DWORD)attrs.ProductID << 16) | attrs.VendorID;
                /* REAL BUG found and fixed 2026-09-08, on real hardware, after
                   two other attempts both failed differently -- see the
                   matching, more detailed comment in setup_ds4_raw_hid().
                   Short version: merely having a SECOND open handle to this
                   same Bluetooth HID collection (even one nothing is ever
                   written to) is enough to make the primary handle's ongoing
                   ReadFile() loop below hang forever after its first
                   successful read, on this hardware/driver stack. BT rumble/
                   LED/battery for DualSense is deliberately NOT implemented
                   at all as a result -- g_ps_output_handle simply stays
                   INVALID_HANDLE_VALUE for this backend, restoring the exact
                   single-handle behavior this Bluetooth path has always used
                   successfully before tonight. USB is unaffected and remains
                   fully functional (see send_rumble_led_to_real_ps_controller()'s
                   own comment for that side of the story). */
                log_line("dualsense rawhid: took over Bluetooth device %s, report length %lu",
                          detail.data.DevicePath, (unsigned long)caps.InputReportByteLength);
                found = 1;
            }
            if (preparsed) HidD_FreePreparsedData(preparsed);
        }
        if (!found) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

/* Reads one report directly off g_dualsense_raw_handle. See
   setup_dualsense_raw_hid()'s comment for the confirmed byte layout.

   FIXED 2026-09-07 (was previously an open limitation -- see
   setup_dualsense_raw_hid()'s keep-alive comment for the full story):
   this connection used to go idle after a few seconds on battery power
   alone, because DualSense's own firmware drops back to a low-power/
   advertising state if nothing ever sends it an output report. A single
   output report sent once at connect time (in setup_dualsense_raw_hid())
   is confirmed, via a real 60-second test window with zero drops, to
   prevent that. A dropped connection here (if it ever happens again)
   just surfaces as a normal poll failure/disconnect, same as any other
   backend -- the main loop already handles that by returning to the
   waiting screen. Byte offsets (relative to base=1, see
   poll_sony_bt_report()) are the confirmed real layout documented in
   setup_dualsense_raw_hid()'s own comment. */
static int poll_dualsense_raw_hid(XINPUT_GAMEPAD *gp) {
    return poll_sony_bt_report(g_dualsense_raw_handle, g_dualsense_raw_report_len, 0x01, 1, gp);
}

/* Wired (USB) DualSense -- see setup_ps_wired_raw_hid()'s comment for why
   this replaces DirectInput for wired PS controllers specifically.
   REAL BUG found and fixed 2026-09-09, confirmed on real hardware: this
   originally just called poll_sony_bt_report() with the same report ID
   (0x01) and base (1) as the Bluetooth case, assuming DualSense's short
   Bluetooth report matches its USB one field-for-field. Real hardware
   test showed sticks worked (they happen to land in the same place
   either way) but every button was scrambled -- colors/LED also worked,
   confirming the connection and output path were fine, only input
   parsing was wrong. Checked the actual field order against DS4Windows's
   own real source (DualSenseDevice.cs) instead of guessing further: the
   USB report's field ORDER genuinely differs from this project's already-
   confirmed Bluetooth layout, not just a header-offset shift -- sticks
   and trigger analogs come first, then a frame counter, THEN face
   buttons+dpad, THEN shoulder buttons, THEN PS/touchpad, a different
   arrangement than the Bluetooth path's byte 5/6/7 order. DS4Windows uses
   report_offset 0 for USB (1 for its own BT extended-report handling,
   which reads report ID 0x31, not the short 0x01 report this project's
   Bluetooth path uses -- confirms the two BT variants aren't even the
   same report, further explaining why layouts differ). Written as its
   own self-contained parser rather than reusing poll_sony_bt_report()'s
   shared one, so this fix can't accidentally affect the already-proven
   Bluetooth path at all. */
static int poll_dualsense_wired_raw_hid(XINPUT_GAMEPAD *gp) {
    HANDLE h = g_dualsense_raw_handle;
    if (h == INVALID_HANDLE_VALUE) return 0;
    static BYTE report[1024];
    DWORD read = 0;
    if (!ReadFile(h, report, g_dualsense_raw_report_len, &read, NULL) || read == 0) return 0;

    /* Same stale-content disconnect detection as poll_sony_bt_report()'s
       own comment explains -- kept independent here, not shared, so a
       change to one can never silently affect the other. */
#define DUALSENSE_WIRED_STALE_TIMEOUT_MS 3000
    static BYTE lastContent[1024];
    static DWORD lastContentLen = 0;
    static ULONGLONG lastChangeTick = 0;
    ULONGLONG now = GetTickCount64();
    if (lastChangeTick == 0 || read != lastContentLen || memcmp(report, lastContent, read) != 0) {
        memcpy(lastContent, report, read);
        lastContentLen = read;
        lastChangeTick = now;
    } else if (now - lastChangeTick > DUALSENSE_WIRED_STALE_TIMEOUT_MS) {
        lastChangeTick = 0;
        return 0;
    }
    if (read < 11 || report[0] != 0x01) return 0;

    memset(gp, 0, sizeof(*gp));
    gp->sThumbLX = ds4_axis_to_i16(report[1], 0);
    gp->sThumbLY = ds4_axis_to_i16(report[2], 1);
    gp->sThumbRX = ds4_axis_to_i16(report[3], 0);
    gp->sThumbRY = ds4_axis_to_i16(report[4], 1);
    gp->bLeftTrigger = report[5];
    gp->bRightTrigger = report[6];
    /* report[7] is a frame/sequence counter -- not needed here */

    ds4_hat_to_dpad(report[8], &gp->wButtons); /* low nibble, same rotational convention as DS4's own hat */
    if (report[8] & 0x10) gp->wButtons |= XINPUT_GAMEPAD_X; /* Square */
    if (report[8] & 0x20) gp->wButtons |= XINPUT_GAMEPAD_A; /* Cross */
    if (report[8] & 0x40) gp->wButtons |= XINPUT_GAMEPAD_B; /* Circle */
    if (report[8] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_Y; /* Triangle */
    if (report[9] & 0x01) gp->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;  /* L1 */
    if (report[9] & 0x02) gp->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; /* R1 */
    if (report[9] & 0x10) gp->wButtons |= XINPUT_GAMEPAD_BACK;  /* Share */
    if (report[9] & 0x20) gp->wButtons |= XINPUT_GAMEPAD_START; /* Options */
    if (report[9] & 0x40) gp->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;  /* L3 */
    if (report[9] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; /* R3 */

    g_ps_pressed = (report[10] & 0x01) != 0;
    g_touchpad_pressed = (report[10] & 0x02) != 0;
    return 1;
}

/* Wired (USB) counterpart to setup_ds4_raw_hid()/setup_dualsense_raw_hid()
   above -- finds a wired Sony PS controller (DS4 or DualSense) and takes
   it over via a SINGLE raw-HID handle used for BOTH input (via
   poll_ds4_wired_raw_hid()/poll_dualsense_wired_raw_hid() above) and
   output (send_rumble_led_to_real_ps_controller(), via g_ps_output_handle
   pointed at this SAME handle) -- exactly how DS4Windows itself does it,
   per explicit user direction 2026-09-09: never wanted a second handle at
   all, wanted the existing single connection expanded to also do output,
   the same way DS4Windows does. This is what setup_dinput_device()'s own
   comment says was already tried and confirmed broken (a SEPARATE second
   handle alongside DirectInput's own broke DirectInput's read) -- this is
   a genuinely different approach, one handle total, not two.
   Wired PS controllers now go through THIS instead of DirectInput -- the
   connect loop tries this BEFORE falling back to DirectInput, so a real
   DS4/DualSense plugged in wired gets intercepted here. Xbox and any
   other non-Sony DirectInput device (a HOTAS included) are completely
   unaffected: they never match the vid_054c pre-filter below and fall
   through to DirectInput exactly as before, unchanged.
   NOT yet confirmed on real hardware as of this writing -- see this
   project's own hard rule (verify, don't assume) before trusting this
   further than "it compiles." Returns 0 (nothing found), 1 (took over a
   DS4), or 2 (took over a DualSense) -- the caller uses this to pick the
   right backend_t. */
static int setup_ps_wired_raw_hid(void) {
    if (g_ds4_raw_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ds4_raw_handle);
        g_ds4_raw_handle = INVALID_HANDLE_VALUE;
    }
    if (g_dualsense_raw_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_dualsense_raw_handle);
        g_dualsense_raw_handle = INVALID_HANDLE_VALUE;
    }
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsA(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;

    int result = 0;
    SP_DEVICE_INTERFACE_DATA ifData = {.cbSize = sizeof(ifData)};
    for (DWORD i = 0; !result && SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A data;
            char buf[512];
        } detail;
        detail.data.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &detail.data, sizeof(detail), NULL, NULL)) continue;

        if (!strstr(detail.data.DevicePath, "vid_054c")) continue; /* cheap pre-filter, same as the BT backends' own */
        if (strstr(detail.data.DevicePath, "00805f9b34fb")) continue; /* Bluetooth -- handled by the BT-specific
                                                                            backends above, not here */

        HANDLE h = CreateFileA(detail.data.DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs = {.Size = sizeof(attrs)};
        int isDs4 = HidD_GetAttributes(h, &attrs) && attrs.VendorID == 0x054C && is_ds4_product_id(attrs.ProductID);
        int isDualSense = !isDs4 && attrs.VendorID == 0x054C && attrs.ProductID == 0x0CE6;
        if (isDs4 || isDualSense) {
            PHIDP_PREPARSED_DATA preparsed = NULL;
            HIDP_CAPS caps = {0};
            if (HidD_GetPreparsedData(h, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                 caps.InputReportByteLength > 0 && caps.InputReportByteLength <= 64) { /* wired reports are much
                                                                                            shorter than the BT
                                                                                            extended ones (>64,
                                                                                            see setup_ds4_raw_hid()'s
                                                                                            own filter) */
                if (isDs4) {
                    g_ds4_raw_handle = h;
                    g_ds4_raw_report_len = caps.InputReportByteLength;
                } else {
                    g_dualsense_raw_handle = h;
                    g_dualsense_raw_report_len = caps.InputReportByteLength;
                }
                g_dinput_vid_pid = ((DWORD)attrs.ProductID << 16) | attrs.VendorID;
                g_ps_output_handle = h; /* the whole point of this function -- same handle, no second open */
                g_ps_output_is_bt = 0;
                g_ps_output_is_dualsense = isDualSense;
                g_ps_output_owns_handle = 0; /* borrowed from g_ds4_raw_handle/g_dualsense_raw_handle above --
                                                 their own disconnect-path CloseHandle() closes this, not a
                                                 separate one (same convention the BT raw-HID backends use) */
                g_ps_output_shares_input_handle = 1; /* see this flag's own comment -- skips battery polling,
                                                          which shares nothing with rumble/LED's WriteFile() path */
                log_line("ps wired rawhid: took over %s device %s, report length %lu",
                          isDualSense ? "DualSense" : "DS4", detail.data.DevicePath,
                          (unsigned long)caps.InputReportByteLength);
                result = isDualSense ? 2 : 1;
            }
            if (preparsed) HidD_FreePreparsedData(preparsed);
        }
        if (!result) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

/* (Re)acquires whatever's plugged in. Returns 1 if a device is ready to
   poll, 0 if none was found or setup failed. Safe to call repeatedly from
   the wait loop -- releases any previous device first. */
static int setup_dinput_device(void) {
    if (!g_di) return 0;
    g_di_candidate_count = 0;
    if (FAILED(IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, enum_joystick_cb, NULL, DIEDFL_ATTACHEDONLY)))
        return 0;
    if (g_di_candidate_count == 0) return 0; /* nothing found */

    /* Default: the first candidate, same as this project's original
       "grab the first one" behavior -- correct whenever there's only one,
       which is the overwhelmingly common case. Only probed further below
       when there's more than one to tell apart. */
    int chosenIdx = 0;
    if (g_di_candidate_count > 1) {
        /* Ambiguous: see the big comment above enum_joystick_cb() for why
           this can happen on real hardware. Prefer the first candidate
           that actually proves live; if none do within the timeout, fall
           back to candidate 0 -- no worse than this project's original
           behavior in that edge case. */
        for (int i = 0; i < g_di_candidate_count; i++) {
            if (di_candidate_is_live(&g_di_candidates[i].instance)) { chosenIdx = i; break; }
        }
    }
    GUID guid = g_di_candidates[chosenIdx].instance;
    g_dinput_vid_pid = g_di_candidates[chosenIdx].vidPid; /* for describe_controller()'s model lookup */

    if (g_di_device) {
        IDirectInputDevice8_Unacquire(g_di_device);
        IDirectInputDevice8_Release(g_di_device);
        g_di_device = NULL;
    }
    if (FAILED(IDirectInput8_CreateDevice(g_di, &guid, &g_di_device, NULL))) return 0;
    if (FAILED(IDirectInputDevice8_SetDataFormat(g_di_device, &c_dfDIJoystick2))) return 0;
    /* No real window to be exclusive/foreground for -- this is a console
       relay that should keep reading even while some other window (the
       game) has focus. */
    IDirectInputDevice8_SetCooperativeLevel(g_di_device, GetConsoleWindow(), DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);

    static const DWORD stick_axes[] = {DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ};
    for (size_t i = 0; i < sizeof(stick_axes) / sizeof(stick_axes[0]); i++) {
        set_dinput_axis_range(stick_axes[i], -32767, 32767);
        set_dinput_deadzone_zero(stick_axes[i]);
    }
    set_dinput_axis_range(DIJOFS_SLIDER(0), 0, 255);
    set_dinput_axis_range(DIJOFS_SLIDER(1), 0, 255);
    set_dinput_deadzone_zero(DIJOFS_SLIDER(0));
    set_dinput_deadzone_zero(DIJOFS_SLIDER(1));

    /* REAL BUG found and fixed 2026-09-08, on real hardware, the same
       night this was added: this used to also open a SEPARATE raw HID
       handle here for wired-PS-controller rumble/LED output (right after
       DirectInput acquires the device) -- confirmed by a live test to
       break DirectInput's own live input reading the exact same way a
       second Bluetooth handle broke the raw-HID read loop (see the
       matching, more detailed comments in setup_dualsense_raw_hid() and
       setup_ds4_raw_hid()): the dashboard settled into a "connected"
       state with the lightbar correctly changing color (the output
       handle itself worked fine), but stick/button data froze at the
       very first read and never updated again. Whatever the underlying
       cause -- this Windows HID stack apparently doesn't like a second
       concurrent open handle to a device that's also being actively read
       by ANY mechanism, DirectInput included, not just raw ReadFile()
       loops -- the pattern is now consistent across all three input
       backends this project has (DirectInput, DS4 raw-HID, DualSense
       raw-HID), so PS-controller rumble/LED/battery is not implemented
       for ANY connection type while input is also being actively read
       from the same device. Reliable input is unconditionally more
       important than a nice-to-have feature -- restoring this function
       to the exact form it had before tonight's rumble/LED work touched
       it at all. */
    return SUCCEEDED(IDirectInputDevice8_Acquire(g_di_device));
}

static int poll_dinput(DIJOYSTATE2 *out) {
    if (!g_di_device) return 0;
    IDirectInputDevice8_Poll(g_di_device); /* return ignored -- not all devices need/support this */
    HRESULT hr = IDirectInputDevice8_GetDeviceState(g_di_device, sizeof(*out), out);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (SUCCEEDED(IDirectInputDevice8_Acquire(g_di_device))) {
            hr = IDirectInputDevice8_GetDeviceState(g_di_device, sizeof(*out), out);
        }
    }
    return SUCCEEDED(hr);
}

/* DirectInput's POV hat is degrees*100 clockwise from north, or -1
   (0xFFFFFFFF) centered -- decode into the same 4 direction flags
   dinput_to_xinput_gamepad() below turns into DPAD_* bits, an ordinary
   8-way hat split into 45-degree wedges centered on each direction. */
static void pov_to_dpad(DWORD pov, int *up, int *down, int *left, int *right) {
    *up = *down = *left = *right = 0;
    if (pov == 0xFFFFFFFF) return;
    int deg = (int)(pov / 100) % 360;
    if (deg > 337 || deg <= 22) *up = 1;
    else if (deg <= 67) { *up = 1; *right = 1; }
    else if (deg <= 112) *right = 1;
    else if (deg <= 157) { *down = 1; *right = 1; }
    else if (deg <= 202) *down = 1;
    else if (deg <= 247) { *down = 1; *left = 1; }
    else if (deg <= 292) *left = 1;
    else *up = *left = 1;
}

/* Best-documented-guess mapping for a PS4/PS5 pad's DIJOYSTATE2 layout
   (community-documented, not yet confirmed against real hardware here --
   expect this to need live correction once tested, the same way the
   GameSir's own axis/button mapping did). Converting into the same
   XINPUT_GAMEPAD shape lets every backend share one relay/dashboard/
   toggle-detection code path below instead of duplicating it per input
   API -- the vJoy button numbers a friend binds in-game end up identical
   regardless of which controller type fed them. */
static void dinput_to_xinput_gamepad(const DIJOYSTATE2 *js, XINPUT_GAMEPAD *gp) {
    memset(gp, 0, sizeof(*gp));
    gp->sThumbLX = (SHORT)js->lX;
    gp->sThumbLY = (SHORT)(-js->lY); /* DirectInput Y grows downward; XInput's grows upward */
    gp->sThumbRX = (SHORT)js->lZ;
    gp->sThumbRY = (SHORT)(-js->lRz);
    /* Triggers, confirmed against real hardware via the raw diagnostic
       log: NOT the sliders (that was the original guess, and it read
       nothing at all on the real controller) -- Rx/Ry span the full
       -32767..32767 range as the triggers move, unpressed at -32767. */
    long lt = (long)js->lRx + 32767;
    long rt = (long)js->lRy + 32767;
    if (lt < 0) lt = 0;
    if (rt < 0) rt = 0;
    gp->bLeftTrigger = (BYTE)(lt * 255 / 65534);
    gp->bRightTrigger = (BYTE)(rt * 255 / 65534);

    if (js->rgbButtons[1] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_A;   /* Cross */
    if (js->rgbButtons[2] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_B;   /* Circle */
    if (js->rgbButtons[0] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_X;   /* Square */
    if (js->rgbButtons[3] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_Y;   /* Triangle */
    if (js->rgbButtons[4] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;  /* L1 */
    if (js->rgbButtons[5] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; /* R1 */
    if (js->rgbButtons[8] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_BACK;  /* Share/Create */
    if (js->rgbButtons[9] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_START; /* Options */
    if (js->rgbButtons[10] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;  /* L3 */
    if (js->rgbButtons[11] & 0x80) gp->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; /* R3 */

    int up, down, left, right;
    pov_to_dpad(js->rgdwPOV[0], &up, &down, &left, &right);
    if (up) gp->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (down) gp->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (left) gp->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (right) gp->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
}

/* ==== ViGEmBus / virtual Xbox or DualShock4 controller ====
   The other half of the dual-device design agreed on earlier: vJoy above
   always gets fed real data unchanged (that never stops). This virtual
   controller stays PRESENT for the whole session too, from the moment a
   real controller connects until this program exits -- what changes is
   only the DATA fed into it, muted (neutral/all-zero) in HOTAS mode and
   live in Normal mode. That's the whole point: HIDHide can hide the real
   controller once at startup and never be touched again, instead of
   needing to toggle a device's PRESENCE live (which it can't do reliably
   once a game already has a handle open -- confirmed earlier, that's
   what motivated this design in the first place).

   ViGEmClient itself ships source-only (no prebuilt binary release) --
   built from the real, unmodified, MIT-licensed nefarius/ViGEmClient
   source via `zig c++` (confirmed: compiles clean, same toolchain as
   everything else here, no MSVC/Visual Studio needed), then loaded
   dynamically at runtime exactly like vJoyInterface.dll already is.
   ViGEmClient.dll must ship alongside this exe. */

typedef PVIGEM_CLIENT(__stdcall *vigem_alloc_t)(void);
typedef VIGEM_ERROR(__stdcall *vigem_connect_t)(PVIGEM_CLIENT);
typedef PVIGEM_TARGET(__stdcall *vigem_target_x360_alloc_t)(void);
typedef PVIGEM_TARGET(__stdcall *vigem_target_ds4_alloc_t)(void);
typedef VIGEM_ERROR(__stdcall *vigem_target_add_t)(PVIGEM_CLIENT, PVIGEM_TARGET);
typedef VIGEM_ERROR(__stdcall *vigem_target_remove_t)(PVIGEM_CLIENT, PVIGEM_TARGET);
typedef void(__stdcall *vigem_target_free_t)(PVIGEM_TARGET);
typedef void(__stdcall *vigem_disconnect_t)(PVIGEM_CLIENT);
typedef void(__stdcall *vigem_free_t)(PVIGEM_CLIENT);
typedef VIGEM_ERROR(__stdcall *vigem_target_x360_update_t)(PVIGEM_CLIENT, PVIGEM_TARGET, XUSB_REPORT);
typedef VIGEM_ERROR(__stdcall *vigem_target_ds4_update_t)(PVIGEM_CLIENT, PVIGEM_TARGET, DS4_REPORT);
typedef BOOL(__stdcall *vigem_target_is_attached_t)(PVIGEM_TARGET);
typedef VIGEM_ERROR(__stdcall *vigem_target_x360_register_notification_t)(PVIGEM_CLIENT, PVIGEM_TARGET,
                                                                            PFN_VIGEM_X360_NOTIFICATION, LPVOID);
typedef void(__stdcall *vigem_target_x360_unregister_notification_t)(PVIGEM_TARGET);
typedef VIGEM_ERROR(__stdcall *vigem_target_ds4_register_notification_t)(PVIGEM_CLIENT, PVIGEM_TARGET,
                                                                            PFN_VIGEM_DS4_NOTIFICATION, LPVOID);
typedef void(__stdcall *vigem_target_ds4_unregister_notification_t)(PVIGEM_TARGET);

static vigem_target_is_attached_t pvigem_target_is_attached;
static vigem_alloc_t pvigem_alloc;
static vigem_connect_t pvigem_connect;
static vigem_target_x360_alloc_t pvigem_target_x360_alloc;
static vigem_target_ds4_alloc_t pvigem_target_ds4_alloc;
static vigem_target_add_t pvigem_target_add;
static vigem_target_remove_t pvigem_target_remove;
static vigem_target_free_t pvigem_target_free;
static vigem_disconnect_t pvigem_disconnect;
static vigem_free_t pvigem_free;
static vigem_target_x360_update_t pvigem_target_x360_update;
static vigem_target_ds4_update_t pvigem_target_ds4_update;
static vigem_target_x360_register_notification_t pvigem_target_x360_register_notification;
static vigem_target_x360_unregister_notification_t pvigem_target_x360_unregister_notification;
static vigem_target_ds4_register_notification_t pvigem_target_ds4_register_notification;
static vigem_target_ds4_unregister_notification_t pvigem_target_ds4_unregister_notification;
static int g_vigem_available = 0; /* DLL loaded and all exports resolved */

static int load_vigem(void) {
    HMODULE h = LoadLibraryA("ViGEmClient.dll");
    if (!h) return 0;
#define LOAD(name) (p##name = (name##_t)GetProcAddress(h, #name))
    int ok = LOAD(vigem_alloc) && LOAD(vigem_connect) && LOAD(vigem_target_x360_alloc) &&
              LOAD(vigem_target_ds4_alloc) && LOAD(vigem_target_add) && LOAD(vigem_target_remove) &&
              LOAD(vigem_target_free) && LOAD(vigem_disconnect) && LOAD(vigem_free) &&
              LOAD(vigem_target_x360_update) && LOAD(vigem_target_ds4_update) && LOAD(vigem_target_is_attached) &&
              LOAD(vigem_target_x360_register_notification) && LOAD(vigem_target_x360_unregister_notification) &&
              LOAD(vigem_target_ds4_register_notification) && LOAD(vigem_target_ds4_unregister_notification);
#undef LOAD
    g_vigem_available = ok;
    return ok;
}

/* Called once a real controller is found and vJoy is acquired -- creates
   the virtual controller and leaves it present for the rest of this
   connection's lifetime. Failure here is non-fatal: the relay still works
   via vJoy alone, just without the future gamepad-emulation path (e.g. if
   ViGEmBus isn't installed on an older bundle, or the DLL didn't load). */

/* Fires on ViGEmBus's own worker thread (NOT the main loop thread) whenever
   a game sets rumble/LED on the virtual X360 pad it's actually reading --
   forwarded to whichever REAL physical controller is actually connected,
   so rumble genuinely reaches the player's hands through the virtual
   device exactly the same way stick/button data flows the other way. This
   is the "make an Xbox pad look like a PS pad or vice versa, and rumble
   still has to translate" requirement -- the virtual device's TYPE (what
   the game thinks it's talking to) and the real controller's type are
   completely independent, so this callback checks which real controller
   is ACTUALLY present rather than assuming it matches the virtual one:
     - real is a Sony PS pad (g_ps_output_handle valid): forward motors
       via send_ps_rumble(), which preserves whatever LED color was most
       recently set (see that function's comment) -- a PS controller
       standing in for an Xbox pad still gets real rumble, just no color
       data to forward (X360's notification carries no lightbar field).
     - real is an XInput/Xbox-style pad: forward via XInputSetState using
       the physical slot index fixed at registration time (userData, see
       vigem_setup() below).
   LedNumber is intentionally ignored either way -- there's no XInput call
   to set a real Xbox pad's player-LED (confirmed 2026-09-08: XInput's
   public API has no such function, and this exact controller exposes no
   raw HID interface either), and a real PS pad's LED is driven by mode/
   battery logic elsewhere, not by whatever "player number" a game thinks
   it's assigning. Both XInputSetState() and send_ps_rumble() are safe to
   call from any thread, so no locking is needed here despite running on
   ViGEmBus's own thread. */
static VOID CALLBACK x360_rumble_notification(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR largeMotor,
                                                UCHAR smallMotor, UCHAR ledNumber, LPVOID userData) {
    (void)client;
    (void)target;
    (void)ledNumber;
    if (g_ps_output_handle != INVALID_HANDLE_VALUE) {
        send_ps_rumble(largeMotor, smallMotor);
        return;
    }
    if (!pXInputSetState) return;
    int userIndex = (int)(INT_PTR)userData;
    XINPUT_VIBRATION vib;
    /* Real XInput rumble is 16-bit (0-65535); ViGEmBus's notification gives
       the 8-bit (0-255) value the game actually sent to the virtual pad --
       257 is the exact scale-up (255*257 == 65535), not a lossy approximation,
       since the game never had more than 8 bits of precision to begin with. */
    vib.wLeftMotorSpeed = (WORD)largeMotor * 257;
    vib.wRightMotorSpeed = (WORD)smallMotor * 257;
    pXInputSetState((DWORD)userIndex, &vib);
}

/* DS4-type counterpart to x360_rumble_notification() above -- same
   reasoning, same "forward to whichever real controller is actually
   connected" logic, plus the DS4-specific lightbar color a game can also
   set (an X360-type virtual pad has no equivalent color channel).
   REAL BUG found and fixed 2026-09-08: the first attempt at this used
   vigem_target_ds4_get_output(), a manually-polled call, from the main
   15Hz loop. Reading ViGEmClient's own source revealed why that broke
   wired PS5 input on real hardware: that function's underlying IOCTL
   (IOCTL_DS4_REQUEST_NOTIFICATION) only ever completes once a game
   actually sends a new output report, and it was called with
   GetOverlappedResult(..., bWait=TRUE) -- an unconditional, no-timeout
   BLOCK until that happens. With no real game running during testing,
   that pending wait never completed, and since it ran on the SAME thread
   as the input read/relay loop, the entire relay froze on the very first
   call. vigem_target_ds4_register_notification() (used here instead) is
   the correct API for this -- confirmed by reading ViGEmClient's own
   source: it spawns its OWN dedicated background thread internally to do
   that exact same blocking wait, and only calls back here, on that
   thread, once a real notification actually arrives -- never blocking
   the caller. This is exactly the mechanism x360_rumble_notification()
   above already uses/relies on; DS4 just needed the equivalent
   registration call instead of the raw polling one. */
static VOID CALLBACK ds4_rumble_notification(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR largeMotor,
                                                UCHAR smallMotor, DS4_LIGHTBAR_COLOR lightbarColor, LPVOID userData) {
    (void)client;
    (void)target;
    if (g_ps_output_handle != INVALID_HANDLE_VALUE) {
        send_ps_rumble(largeMotor, smallMotor);
        send_ps_led(lightbarColor.Red, lightbarColor.Green, lightbarColor.Blue);
        return;
    }
    if (!pXInputSetState) return;
    int userIndex = (int)(INT_PTR)userData;
    XINPUT_VIBRATION vib;
    vib.wLeftMotorSpeed = (WORD)largeMotor * 257;
    vib.wRightMotorSpeed = (WORD)smallMotor * 257;
    pXInputSetState((DWORD)userIndex, &vib);
}

static void vigem_setup(backend_t backend, int userIndex) {
    if (!g_vigem_available) {
        log_line("vigem: skipped, DLL never loaded");
        return;
    }
    g_vigem_client = pvigem_alloc();
    if (!g_vigem_client) {
        log_line("vigem: vigem_alloc returned NULL");
        return;
    }
    VIGEM_ERROR err = pvigem_connect(g_vigem_client);
    if (err != VIGEM_ERROR_NONE) {
        log_line("vigem: vigem_connect failed, error 0x%08X", (unsigned)err);
        pvigem_free(g_vigem_client);
        g_vigem_client = NULL;
        return;
    }
    g_vigem_target = g_vigem_emulate_ds4 ? pvigem_target_ds4_alloc() : pvigem_target_x360_alloc();
    if (!g_vigem_target) {
        log_line("vigem: target_alloc (%s) returned NULL", g_vigem_emulate_ds4 ? "ds4" : "x360");
        pvigem_disconnect(g_vigem_client);
        pvigem_free(g_vigem_client);
        g_vigem_client = NULL;
        return;
    }
    err = pvigem_target_add(g_vigem_client, g_vigem_target);
    if (err != VIGEM_ERROR_NONE) {
        log_line("vigem: target_add (%s) failed, error 0x%08X", g_vigem_emulate_ds4 ? "ds4" : "x360", (unsigned)err);
        pvigem_target_free(g_vigem_target);
        g_vigem_target = NULL;
        pvigem_disconnect(g_vigem_client);
        pvigem_free(g_vigem_client);
        g_vigem_client = NULL;
        return;
    }
    log_line("vigem: %s target created and added successfully (is_attached=%d)", g_vigem_emulate_ds4 ? "ds4" : "x360",
              pvigem_target_is_attached(g_vigem_target));

    /* Rumble-forwarding only makes sense when BOTH the virtual pad the game
       writes to -- the callback itself (x360_rumble_notification, above)
       now handles forwarding to EITHER a real XInput pad OR a real PS
       controller, so this registers whenever the virtual pad is X360-type
       and there's some real controller (of either kind) actually capable
       of receiving rumble, regardless of whether the two types match.
       Verified end-to-end on real hardware 2026-09-08: rumbling the
       VIRTUAL pad's own XInput slot (exactly what a real game does)
       correctly reached the real physical Xbox controller through this
       callback, confirmed felt by the user -- not just "the call didn't
       error." (The PS-controller-behind-an-X360-virtual-pad path uses the
       same callback and function, just a different real-controller check
       inside it -- not yet separately hardware-verified as of this
       writing, unlike the Xbox-real case above.) */
#ifndef SAFE_MODE_NO_EXTRAS
    if (!g_vigem_emulate_ds4 && (backend == BACKEND_XINPUT || g_ps_output_handle != INVALID_HANDLE_VALUE)) {
        VIGEM_ERROR notifyErr = pvigem_target_x360_register_notification(g_vigem_client, g_vigem_target,
                                                                            x360_rumble_notification,
                                                                            (LPVOID)(INT_PTR)userIndex);
        if (notifyErr == VIGEM_ERROR_NONE) log_line("vigem: x360 rumble-forwarding registered");
        else log_line("vigem: x360 rumble-forwarding FAILED to register (error 0x%08X)", (unsigned)notifyErr);
    }
    /* DS4-type counterpart -- see ds4_rumble_notification()'s own comment
       for why this uses the register/callback API and not a manual poll.
       Same "either real controller type, regardless of virtual type"
       condition as the X360 case above. Not yet hardware-verified as of
       this writing (the polling version broke wired PS5 input; this
       rewrite hasn't had its own real-hardware pass yet). */
    if (g_vigem_emulate_ds4 && (backend == BACKEND_XINPUT || g_ps_output_handle != INVALID_HANDLE_VALUE)) {
        VIGEM_ERROR notifyErr = pvigem_target_ds4_register_notification(g_vigem_client, g_vigem_target,
                                                                            ds4_rumble_notification,
                                                                            (LPVOID)(INT_PTR)userIndex);
        if (notifyErr == VIGEM_ERROR_NONE) log_line("vigem: ds4 rumble-forwarding registered");
        else log_line("vigem: ds4 rumble-forwarding FAILED to register (error 0x%08X)", (unsigned)notifyErr);
    }
#endif
}

/* Locked internally (see g_output_lock's comment) rather than relying on
   every caller to remember to lock around it -- console_ctrl_handler(),
   fatal_exit(), and the main loop's own disconnect cleanup all call this
   directly, and any one of them can run concurrently with the others
   (Ctrl+C firing on its own thread mid-disconnect-cleanup, specifically).
   Without the lock, two threads could both pass the `if (!g_vigem_client)
   return;` guard before either clears it, and both go on to free the
   same client/target -- a real double-free, not theoretical. */
static void vigem_teardown(void) {
    EnterCriticalSection(&g_output_lock);
    if (g_vigem_client) {
        if (g_vigem_target) {
            if (!g_vigem_emulate_ds4 && pvigem_target_x360_unregister_notification)
                pvigem_target_x360_unregister_notification(g_vigem_target);
            if (g_vigem_emulate_ds4 && pvigem_target_ds4_unregister_notification)
                pvigem_target_ds4_unregister_notification(g_vigem_target);
            pvigem_target_remove(g_vigem_client, g_vigem_target);
            pvigem_target_free(g_vigem_target);
            g_vigem_target = NULL;
        }
        pvigem_disconnect(g_vigem_client);
        pvigem_free(g_vigem_client);
        g_vigem_client = NULL;
    }
    LeaveCriticalSection(&g_output_lock);
}

/* Feeds the virtual controller either the real live state or an all-
   neutral one, based on g_hidden_mode -- mode==1 (HOTAS) mutes this,
   mode==0 (Normal) mirrors gp live. Mutually exclusive with vJoy's own
   feed (elsewhere in the main loop): vJoy mutes in Normal mode instead,
   so exactly one of the two outputs is ever live at a time -- "goes both
   ways", per the user, is the whole point of having two virtual devices
   at all. IMPORTANT, corrected 2026-09-07: g_hidden_mode no longer has
   anything to do with HidHide's cloak -- the real controller is hidden
   from every other app for the entire time it's connected, in BOTH
   modes (see register_with_hidhide()'s comment). This flag only ever
   picks which VIRTUAL device gets real data. */
static void vigem_update(const XINPUT_GAMEPAD *gp) {
    /* Locked for the same reason as vigem_teardown() (see g_output_lock's
       comment): without this, console_ctrl_handler() freeing g_vigem_
       target/g_vigem_client on its own thread could race a still-in-
       flight read of them right here. Reentrant-safe when the main loop
       already holds this same lock around its whole write block. */
    EnterCriticalSection(&g_output_lock);
    if (!g_vigem_target) {
        LeaveCriticalSection(&g_output_lock);
        return;
    }
    int muted = g_hidden_mode == 1;

    if (!g_vigem_emulate_ds4) {
        XUSB_REPORT report;
        XUSB_REPORT_INIT(&report);
        if (!muted) {
            report.wButtons = gp->wButtons;
            report.bLeftTrigger = gp->bLeftTrigger;
            report.bRightTrigger = gp->bRightTrigger;
            report.sThumbLX = gp->sThumbLX;
            report.sThumbLY = gp->sThumbLY;
            report.sThumbRX = gp->sThumbRX;
            report.sThumbRY = gp->sThumbRY;
        }
        pvigem_target_x360_update(g_vigem_client, g_vigem_target, report);
    } else {
        DS4_REPORT report;
        DS4_REPORT_INIT(&report); /* centers thumbsticks at 0x80, D-pad "none" */
        if (!muted) {
            /* Y needs the same SHORT_MIN guard as vJoy's own feed (see
               ds4_axis_to_i16()'s comment): sThumbLY/RY == -32768 negates to
               +32768 (fine in `int`, no UB), but +32768 >> 8 == 128, and
               128+128 == 256 wraps a BYTE back to 0 -- full-down would
               otherwise report as full-up to the game. Clamp the negation
               to SHORT_MAX first, same as everywhere else this pattern
               shows up. */
            int nly = -(int)gp->sThumbLY, nry = -(int)gp->sThumbRY;
            if (nly > 32767) nly = 32767;
            if (nry > 32767) nry = 32767;
            report.bThumbLX = (BYTE)((gp->sThumbLX >> 8) + 128);
            report.bThumbLY = (BYTE)((nly >> 8) + 128); /* DS4 Y also grows downward */
            report.bThumbRX = (BYTE)((gp->sThumbRX >> 8) + 128);
            report.bThumbRY = (BYTE)((nry >> 8) + 128);
            report.bTriggerL = gp->bLeftTrigger;
            report.bTriggerR = gp->bRightTrigger;

            USHORT btn = 0;
            if (gp->wButtons & XINPUT_GAMEPAD_A) btn |= DS4_BUTTON_CROSS;
            if (gp->wButtons & XINPUT_GAMEPAD_B) btn |= DS4_BUTTON_CIRCLE;
            if (gp->wButtons & XINPUT_GAMEPAD_X) btn |= DS4_BUTTON_SQUARE;
            if (gp->wButtons & XINPUT_GAMEPAD_Y) btn |= DS4_BUTTON_TRIANGLE;
            if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) btn |= DS4_BUTTON_SHOULDER_LEFT;
            if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btn |= DS4_BUTTON_SHOULDER_RIGHT;
            if (gp->wButtons & XINPUT_GAMEPAD_BACK) btn |= DS4_BUTTON_SHARE;
            if (gp->wButtons & XINPUT_GAMEPAD_START) btn |= DS4_BUTTON_OPTIONS;
            if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) btn |= DS4_BUTTON_THUMB_LEFT;
            if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) btn |= DS4_BUTTON_THUMB_RIGHT;
            report.wButtons |= btn;

            /* D-pad is a single 0-7 (+8=none) hat value on DS4, not
               separate bit flags -- fold the 4 XInput dpad bits into one. */
            int up = (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            int down = (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            int left = (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            int right = (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            DS4_DPAD_DIRECTIONS dpad = DS4_BUTTON_DPAD_NONE;
            if (up && right) dpad = DS4_BUTTON_DPAD_NORTHEAST;
            else if (up && left) dpad = DS4_BUTTON_DPAD_NORTHWEST;
            else if (down && right) dpad = DS4_BUTTON_DPAD_SOUTHEAST;
            else if (down && left) dpad = DS4_BUTTON_DPAD_SOUTHWEST;
            else if (up) dpad = DS4_BUTTON_DPAD_NORTH;
            else if (down) dpad = DS4_BUTTON_DPAD_SOUTH;
            else if (left) dpad = DS4_BUTTON_DPAD_WEST;
            else if (right) dpad = DS4_BUTTON_DPAD_EAST;
            DS4_SET_DPAD(&report, dpad);
        }
        pvigem_target_ds4_update(g_vigem_client, g_vigem_target, report);
    }
    LeaveCriticalSection(&g_output_lock);
}

/* Blocks (deliberately -- remapping is a quick, one-off, before-the-game
   action, not something that needs to stay live during actual relaying)
   until a key from a reasonable candidate set is held down, then reads
   whichever modifiers are ALSO down at that instant. Global, like the
   hotkey it configures, so it doesn't matter which window has focus. */
static void capture_hotkey_from_keyboard(UINT *mods_out, UINT *vk_out) {
    static const UINT candidates[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
        'U', 'V', 'W', 'X', 'Y', 'Z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', VK_F1, VK_F2, VK_F3,
        VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    };
    Sleep(300); /* let go of whatever key was just pressed to trigger this */
    for (;;) {
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (GetAsyncKeyState((int)candidates[i]) & 0x8000) {
                UINT mods = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
                *vk_out = candidates[i];
                *mods_out = mods;
                while (GetAsyncKeyState((int)candidates[i]) & 0x8000) Sleep(10); /* wait for release */
                return;
            }
        }
        Sleep(20);
    }
}

/* Forward declaration -- read_any_physical_input() is defined further
   down (it also backs the M-remap flow's capture), but this toggle-combo
   capture function needs it too now that the PS button/touchpad can be
   part of a toggle combo, not just the plain wButtons range. */
static DWORD read_any_physical_input(backend_t backend, int userIndex);

/* Same idea for the controller side: wait for a clean release (in case
   something's already held from getting here), then wait for something to
   be pressed and confirm it's still held ~150ms later, so a single noisy
   blip on the way to the real press doesn't get captured by mistake.
   DWORD, not WORD, since the toggle combo can now include the PS button/
   touchpad (PHYS_PS_BUTTON/PHYS_TOUCHPAD) or a trigger click, the same
   physical inputs the M-remap flow's own capture already recognizes --
   see read_any_physical_input()'s own comment for why those don't fit in
   XINPUT_GAMEPAD's wButtons range. */
static DWORD capture_button_from_controller(backend_t backend, int userIndex) {
    while (read_any_physical_input(backend, userIndex) != 0) Sleep(20);
    for (;;) {
        DWORD now_pressed = read_any_physical_input(backend, userIndex);
        if (now_pressed != 0) {
            Sleep(150);
            if (read_any_physical_input(backend, userIndex) == now_pressed) return now_pressed;
        }
        Sleep(20);
    }
}

/* Triggered by pressing R while the live dashboard is showing (see the
   check in main()) -- the actual "software has a way to remap it"
   feature, no file editing required for the normal case. */
static void run_remap_flow(backend_t backend, int userIndex) {
    clear_console();
    set_color(CLR_WHITE_BR);
    printf("=== REMAP ===\n\n");
    set_color(CLR_NORMAL);
    printf("Press the new HOTKEY now (hold any of Ctrl/Alt/Shift/Win, then tap a\n");
    printf("letter, number, or F-key)...\n");

    UINT new_mods, new_vk;
    capture_hotkey_from_keyboard(&new_mods, &new_vk);
    char hk[32];
    format_hotkey(hk, sizeof(hk), new_mods, new_vk);
    set_color(CLR_GREEN_BR);
    printf("Got it: %s\n\n", hk);
    set_color(CLR_NORMAL);

    printf("Now press the controller button you want (hold two together for a\n");
    printf("combo -- the PS button works well alone too, on a PlayStation\n");
    printf("controller: no game ever binds an action to it directly, and it\n");
    printf("never reaches the game anyway), and keep holding it steady for a\n");
    printf("moment...\n");
    DWORD new_mask = capture_button_from_controller(backend, userIndex);
    char btn[48];
    format_button_combo(btn, sizeof(btn), new_mask, 0);
    set_color(CLR_GREEN_BR);
    printf("Got it: controller %s\n\n", btn);
    set_color(CLR_NORMAL);

    g_hotkey_mods = new_mods;
    g_hotkey_vk = new_vk;
    g_toggle_button_mask = new_mask;
    g_toggle_hold_ms = 0; /* hold-duration combos are still available by hand-editing the ini, if wanted */
    save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    if (g_toggle_thread_id) PostThreadMessageA(g_toggle_thread_id, WM_APP_REMAP, 0, 0);

    printf("Saved -- back to normal in a moment.\n");
    Sleep(1500);
    clear_console();
}

/* Covers every physical input g_button_map (and, as of the PS-button-as-
   toggle feature, the toggle combo too) can be driven from -- trigger
   clicks and, on DirectInput or either raw-HID Sony backend, the PS
   button and touchpad click -- not just what fits in XINPUT_GAMEPAD's
   own wButtons bitmask. Used by both run_button_map_flow() (press M) and
   capture_button_from_controller() above, and by the main loop's own
   toggle-combo check, which builds this exact same combined mask from
   its own already-polled gp/g_ps_pressed/g_touchpad_pressed instead of
   calling this function again (that would re-poll the device a second
   time this same frame). */
static DWORD read_any_physical_input(backend_t backend, int userIndex) {
    XINPUT_GAMEPAD gp;
    int ps_pressed = 0, touchpad_pressed = 0, guide_pressed = 0;
    if (backend == BACKEND_XINPUT) {
        XINPUT_STATE st;
        int used_ex = (g_gamebar_guide_disabled && pXInputGetStateEx); /* see the main loop's identical check */
        DWORD xr = used_ex ? pXInputGetStateEx((DWORD)userIndex, &st) : pXInputGetState((DWORD)userIndex, &st);
        if (xr != ERROR_SUCCESS) return 0;
        gp = st.Gamepad;
        guide_pressed = used_ex && (st.Gamepad.wButtons & XINPUT_GAMEPAD_GUIDE) != 0;
    } else if (backend == BACKEND_DS4_RAWHID) {
        if (!poll_ds4_raw_hid(&gp)) return 0;
        ps_pressed = g_ps_pressed;
        touchpad_pressed = g_touchpad_pressed;
    } else if (backend == BACKEND_DUALSENSE_RAWHID) {
        if (!poll_dualsense_raw_hid(&gp)) return 0;
        ps_pressed = g_ps_pressed;
        touchpad_pressed = g_touchpad_pressed;
    } else if (backend == BACKEND_DS4_WIRED_RAWHID) {
        if (!poll_ds4_wired_raw_hid(&gp)) return 0;
        ps_pressed = g_ps_pressed;
        touchpad_pressed = g_touchpad_pressed;
    } else if (backend == BACKEND_DUALSENSE_WIRED_RAWHID) {
        if (!poll_dualsense_wired_raw_hid(&gp)) return 0;
        ps_pressed = g_ps_pressed;
        touchpad_pressed = g_touchpad_pressed;
    } else {
        DIJOYSTATE2 js;
        if (!poll_dinput(&js)) return 0;
        dinput_to_xinput_gamepad(&js, &gp);
        ps_pressed = (js.rgbButtons[DINPUT_BTN_IDX_PS] & 0x80) != 0;
        touchpad_pressed = (js.rgbButtons[DINPUT_BTN_IDX_TOUCHPAD] & 0x80) != 0;
    }
    DWORD mask = gp.wButtons;
    if (gp.bLeftTrigger > TRIGGER_CLICK_THRESHOLD) mask |= PHYS_TRIGGER_L;
    if (gp.bRightTrigger > TRIGGER_CLICK_THRESHOLD) mask |= PHYS_TRIGGER_R;
    if (ps_pressed) mask |= PHYS_PS_BUTTON;
    if (touchpad_pressed) mask |= PHYS_TOUCHPAD;
    if (guide_pressed) mask |= PHYS_GUIDE_BUTTON;
    return mask;
}

/* Same read_any_physical_input() source as capture_button_from_controller()
   above, but validated single-button-only (see is_single_mapped_button()
   below) -- kept as a separate function since the toggle combo is allowed
   to be two buttons held together (that's the whole point of a combo,
   see NAMED_PHYS-based parse_button_line() above) while a button-map (M)
   remap target must be exactly one recognizable physical input, never an
   ambiguous multi-button press. */
/* Only a mask that maps to EXACTLY one g_button_map slot is a valid single
   press -- rejects both "nothing pressed" (0) and an accidental multi-button
   press (e.g. a resting analog trigger just past TRIGGER_CLICK_THRESHOLD
   plus a face button), which would otherwise be a phys value found in no
   slot at all. Both capture functions below only ever return 0 or a value
   that passes this, so callers can trust vjoy_slot_for_phys() on whatever
   they get back -- see the OOB-write bug this fixed in run_button_map_flow,
   where an unrecognized combined mask indexed g_button_map at -1. */
static int is_single_mapped_button(DWORD mask) {
    return mask != 0 && vjoy_slot_for_phys(mask) != 0;
}

static DWORD capture_any_single_button(backend_t backend, int userIndex) {
    /* Wait for release before arming -- bounded, not infinite: a
       DirectInput device whose trigger axes read as permanently "pressed"
       (no Rx/Ry to read, see dinput_to_xinput_gamepad()) must not hang this
       forever with no way out short of Ctrl+C. After ~5s, just proceed with
       whatever's held; the "hold steady" check below still requires a
       clean, unchanging single-button read to actually accept anything. */
    for (int waited_ms = 0; waited_ms < 5000 && read_any_physical_input(backend, userIndex) != 0; waited_ms += 20)
        Sleep(20);
    for (;;) {
        DWORD now_pressed = read_any_physical_input(backend, userIndex);
        if (is_single_mapped_button(now_pressed)) {
            Sleep(150);
            if (read_any_physical_input(backend, userIndex) == now_pressed) return now_pressed;
        }
        Sleep(20);
    }
}

static DWORD capture_any_button_with_timeout(backend_t backend, int userIndex, DWORD timeout_ms) {
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (;;) {
        DWORD now_pressed = read_any_physical_input(backend, userIndex);
        if (is_single_mapped_button(now_pressed)) {
            Sleep(150);
            if (read_any_physical_input(backend, userIndex) == now_pressed) return now_pressed;
        }
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart >= timeout_ms) return 0;
        Sleep(20);
    }
}

/* Triggered by pressing M while the live dashboard is showing -- fixes the
   "if a button isn't working, a way to remap it, or the whole thing" ask:
   pick one button at a time (whichever "isn't working" or you just want
   changed), see what it currently drives, retarget it, repeat, or just
   wait a few seconds to stop. Swaps rather than overwrites, so nothing
   ends up silently duplicated onto two vJoy buttons at once. */
static void run_button_map_flow(backend_t backend, int userIndex) {
    for (;;) {
        clear_console();
        set_color(CLR_WHITE_BR);
        printf("=== BUTTON MAPPING ===\n\n");
        set_color(CLR_NORMAL);
        printf("Current vJoy button assignments:\n\n");
        for (int i = 0; i < NUM_VJOY_BUTTONS; i++) {
            printf("  vJoy %-2d <- %s\n", i + 1, label_for_phys(g_button_map[i], backend));
        }
        printf("\nPress the controller button/trigger you want to fix or change, and\n");
        printf("keep holding it steady for a moment (or wait ~4 seconds untouched to\n");
        printf("finish and save)...\n");

        DWORD chosen = capture_any_button_with_timeout(backend, userIndex, 4000);
        if (chosen == 0) break; /* nothing pressed in time -- done */

        printf("\nGot it: %s. Now press the button you want %s to act like from now\n",
               label_for_phys(chosen, backend), label_for_phys(chosen, backend));
        printf("on (hold it steady for a moment)...\n");

        DWORD target = capture_any_single_button(backend, userIndex);
        if (target == chosen) {
            printf("Same button -- nothing changed.\n");
        } else {
            int chosen_slot = -1, target_slot = -1;
            for (int i = 0; i < NUM_VJOY_BUTTONS; i++) {
                if (g_button_map[i] == chosen) chosen_slot = i;
                if (g_button_map[i] == target) target_slot = i;
            }
            /* Both captures already guarantee a recognized single button
               (see is_single_mapped_button()), so this should never trip --
               kept anyway rather than trusting that invariant silently,
               since indexing g_button_map at -1 is real memory corruption,
               not just a bad remap. */
            if (chosen_slot < 0 || target_slot < 0) {
                printf("Unrecognized button -- nothing changed.\n");
                Sleep(1200);
                continue;
            }
            g_button_map[chosen_slot] = target; /* swap, don't duplicate -- both ends stay assigned */
            g_button_map[target_slot] = chosen;
            set_color(CLR_GREEN_BR);
            printf("Set: %s now acts like %s (vJoy %d), and %s now acts like %s (vJoy %d).\n",
                   label_for_phys(chosen, backend), label_for_phys(target, backend), target_slot + 1,
                   label_for_phys(target, backend), label_for_phys(chosen, backend), chosen_slot + 1);
            set_color(CLR_NORMAL);
        }
        Sleep(1200);
    }

    save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    printf("\nSaved -- back to normal in a moment.\n");
    Sleep(1200);
    clear_console();
}

/* Standard Windows file-browse dialog -- more friend-friendly than typing
   a path, and doesn't require this to be a GUI app to use. */
static int run_set_game_flow(void) {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetConsoleWindow();
    ofn.lpstrFilter = "Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = "Pick your game's .exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return 0; /* cancelled */

    strncpy(g_game_path, path, sizeof(g_game_path) - 1);
    g_game_path[sizeof(g_game_path) - 1] = '\0';
    save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
    printf("\nSaved -- press G again any time to launch it.\n");
    Sleep(1500);
    return 1;
}

static void launch_game(void) {
    char dir[MAX_PATH];
    strncpy(dir, g_game_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = '\0';

    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(g_game_path, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        printf("\nLaunched %s\n", g_game_path);
    } else {
        printf("\nCouldn't launch %s (error %lu).\n", g_game_path, GetLastError());
    }
    Sleep(1200);
}

/* Non-blocking: drains any keys typed into the console window (not a
   global hook -- this one only fires while the console itself has focus,
   unlike the toggle hotkey/button) and returns the last one pressed, or 0
   if none. Checked once per dashboard redraw, not the hot input path. */
static char check_console_keypress(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD rec;
    DWORD n;
    char result = 0;
    while (PeekConsoleInputA(hIn, &rec, 1, &n) && n > 0) {
        ReadConsoleInputA(hIn, &rec, 1, &n);
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            result = (char)toupper((unsigned char)rec.Event.KeyEvent.uChar.AsciiChar);
        }
    }
    return result;
}


#define HID_USAGE_X 0x30u
#define HID_USAGE_Y 0x31u
#define HID_USAGE_Z 0x32u
#define HID_USAGE_RX 0x33u
#define HID_USAGE_RY 0x34u
#define HID_USAGE_RZ 0x35u

#define AXIS_MIN 1L
#define AXIS_MAX 32768L

typedef BOOL(__stdcall *vJoyEnabled_t)(void);
typedef BOOL(__stdcall *AcquireVJD_t)(UINT);
typedef void(__stdcall *RelinquishVJD_t)(UINT);
typedef BOOL(__stdcall *SetAxis_t)(LONG, UINT, UINT);
typedef BOOL(__stdcall *SetBtn_t)(BOOL, UINT, UCHAR);

static vJoyEnabled_t pvJoyEnabled;
static AcquireVJD_t pAcquireVJD;
static RelinquishVJD_t pRelinquishVJD;
static SetAxis_t pSetAxis;
static SetBtn_t pSetBtn;

/* Single, lock-guarded place vJoy gets relinquished from -- used by
   console_ctrl_handler(), fatal_exit(), and the main loop's own
   disconnect cleanup, instead of each duplicating (and racing on) the
   same check-then-clear sequence. See g_output_lock's own comment
   (declared near g_vjoy_acquired, above vigem_teardown()) for why this
   locking exists at all. */
static void relinquish_vjoy_if_acquired(void) {
    EnterCriticalSection(&g_output_lock);
    if (g_vjoy_acquired && pRelinquishVJD) {
        pRelinquishVJD(g_vjoy_rid);
        g_vjoy_acquired = 0;
    }
    LeaveCriticalSection(&g_output_lock);
}

static const char *VJOY_DLL_PATHS[] = {
    "C:\\Program Files\\vJoy\\x64\\vJoyInterface.dll",
    "C:\\Program Files (x86)\\vJoy\\x64\\vJoyInterface.dll",
};

/* The driver-only install (create_root_device against vjoy.inf) only ever
   puts the KERNEL driver in place -- it never installs vJoyInterface.dll,
   the user-mode SDK DLL SetAxis/SetBtn/etc actually come from. That DLL
   only exists in Program Files on a machine that's had vJoy's full,
   original installer run on it at some point. A genuinely fresh machine
   (confirmed via a real Windows Sandbox run) has neither, so we also ship
   our own copy next to the exe and try that first. */
static HMODULE load_vjoy(void) {
    HMODULE h = NULL;
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    char bundled_paths[2][MAX_PATH];
    snprintf(bundled_paths[0], sizeof(bundled_paths[0]), "%sdrivers\\vjoy\\vJoyInterface.dll", exe_dir);
    snprintf(bundled_paths[1], sizeof(bundled_paths[1]), "%svJoyInterface.dll", exe_dir);
    for (size_t i = 0; i < 2 && !h; i++) {
        h = LoadLibraryA(bundled_paths[i]);
    }
    for (size_t i = 0; i < sizeof(VJOY_DLL_PATHS) / sizeof(VJOY_DLL_PATHS[0]) && !h; i++) {
        h = LoadLibraryA(VJOY_DLL_PATHS[i]);
    }
    if (!h) {
        fprintf(stderr, "Could not load vJoyInterface.dll from any known path.\n");
        return NULL;
    }
    pvJoyEnabled = (vJoyEnabled_t)GetProcAddress(h, "vJoyEnabled");
    pAcquireVJD = (AcquireVJD_t)GetProcAddress(h, "AcquireVJD");
    pRelinquishVJD = (RelinquishVJD_t)GetProcAddress(h, "RelinquishVJD");
    pSetAxis = (SetAxis_t)GetProcAddress(h, "SetAxis");
    pSetBtn = (SetBtn_t)GetProcAddress(h, "SetBtn");
    if (!pvJoyEnabled || !pAcquireVJD || !pRelinquishVJD || !pSetAxis || !pSetBtn) {
        fprintf(stderr, "vJoyInterface.dll loaded but missing an expected export.\n");
        return NULL;
    }
    return h;
}

/* XInput thumbstick values are already int16 (-32768..32767) straight from
   the controller -- just re-center onto vJoy's 1..32768 range. */
static long i16_to_axis(SHORT v) {
    if (v < -32767) v = -32767;
    double frac = (v + 32767) / 65534.0;
    return AXIS_MIN + (long)(frac * (AXIS_MAX - AXIS_MIN) + 0.5);
}

/* Triggers are 0..255 -- spread across the full axis range. */
static long u8_to_axis(BYTE v) {
    double frac = v / 255.0;
    return AXIS_MIN + (long)(frac * (AXIS_MAX - AXIS_MIN) + 0.5);
}

/* Runs on its own OS-spawned thread the instant Ctrl+C is pressed, the
   console window is closed, or the process is otherwise asked to end --
   guarantees the real controller never gets left invisible to every other
   game/app just because this tool was closed rather than toggled back to
   Normal mode first. Windows allows a handler like this a few seconds to
   finish before forcing termination, plenty for one HidHideCLI call. */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    relinquish_vjoy_if_acquired();
    run_hidhide("--cloak-off");
    restore_gamebar_guide_capture(); /* undo whatever this session changed, same as the cloak-off right above */
    vigem_teardown(); /* don't leave an orphaned virtual controller behind on an abrupt close */
    /* vJoy's device is session-scoped by design (2026-09-07) -- created
       fresh on every launch, fully removed here on every exit, so nothing
       from this tool is ever visible in Device Manager while it isn't
       running. Relinquish above happens first so the device isn't yanked
       out from under a still-open handle. */
    destroy_vjoy_root_devices();
    printf("\x1b[?1049l"); /* leave the alternate screen buffer -- see console_init_once() -- so the
                               terminal's normal prompt/history comes back instead of staying blank/stuck */
    fflush(stdout);
    return FALSE; /* let Windows continue its normal shutdown afterward */
}

/* Prints an error and waits for a keypress before exiting -- without this,
   double-clicking this exe (rather than running it from an already-open
   terminal) means Windows closes the whole console window the instant the
   process exits, so any early failure message is gone before it can be
   read at all. Confirmed this actually happened ("ran two UAC then closed
   idk why"), which is exactly the failure mode this exists to prevent. */
static void fatal_exit(const char *message) {
    /* Same cleanup as console_ctrl_handler()'s clean-exit path, since
       vJoy's device is session-scoped now -- an error partway through
       startup (say, vJoy created and acquired but ViGEm failed to load
       right after) must not leave that half-finished state behind just
       because this exits through a different path than Ctrl+C. Every
       call here is already self-guarded/idempotent (checks its own
       "was anything actually done" state), so it's safe to call
       unconditionally even this early, before vJoy/ViGEm/HidHide are
       necessarily set up at all. */
    relinquish_vjoy_if_acquired();
    run_hidhide("--cloak-off");
    restore_gamebar_guide_capture();
    vigem_teardown();
    destroy_vjoy_root_devices();

    printf("\x1b[?1049l"); /* harmless no-op if the alternate screen buffer (see console_init_once) was never entered */
    fflush(stdout);
    fprintf(stderr, "\n%s\n\nPress Enter to close this window...", message);
    fflush(stderr);
    getchar();
    exit(1);
}

/* Everything below, through check_and_install_drivers(), used to be the
   separate driver_check.exe -- folded in here so this is genuinely one
   exe a friend can be handed, not two they have to know to run in order.
   See driver_check.c's own (now-historical) comment for the reasoning
   behind installing via pnputil against the bundled signed driver
   packages instead of each vendor's own installer. */

/* Returns 1 if the named service exists (installed), 0 if not, -1 on error
   opening the Service Control Manager itself. */
static int service_exists(const char *service_name) {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return -1;
    SC_HANDLE svc = OpenServiceA(scm, service_name, SERVICE_QUERY_STATUS);
    int exists = (svc != NULL);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

/* Plain registry read, no elevation needed for that (confirmed: reading
   this same key works fine from a non-admin session) -- lets
   check_and_install_drivers() below skip the elevated rename once it's
   already been done, instead of popping a UAC prompt on every launch. */
static int oem_name_already_set(void) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\"
                       "VID_1234&PID_BEAD",
                       0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return 0;
    }
    char value[128] = {0};
    DWORD size = sizeof(value), type = 0;
    LONG r = RegQueryValueExA(key, "OEMName", NULL, &type, (BYTE *)value, &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_SZ && !strcmp(value, "WARDOGS HOTAS Controller");
}

/* Creates a root-enumerated (non-Plug-and-Play, "software") device from a
   hardware ID and binds the given driver to it -- the same documented
   SetupAPI sequence Device Manager and the WDK's devcon.exe use internally
   (CreateDeviceInfo -> set the hardware ID -> DIF_REGISTERDEVICE -> update
   the driver binding), done directly instead of depending on devcon.exe
   (not bundled) or a vendor's own helper tool (nefconw.exe, ViGEmBus's
   own -- turned out not to even be reachable the way its install folder
   suggested). Needed because `pnputil /add-driver ... /install` only
   binds to a device that's ALREADY present in some form -- confirmed
   empirically: it silently created nothing for ViGEmBus (which has no
   pre-existing device on a truly fresh machine), while it appeared to
   work for vJoy here only because a "ghost" device node was still present
   in the tree from an earlier uninstall that removed the service but not
   the device record -- not something a friend's never-had-it-before
   machine could rely on either. Must run elevated. */
static int create_root_device(const char *hardware_id, const char *inf_path) {
    /* This function used to fail completely silently on any error --
       confirmed a real, concrete cost of that 2026-09-07: HIDHide/
       ViGEmBus both failed to install (root cause: the exe was run from
       a copy that didn't have its drivers\ folder bundled next to it, so
       inf_path pointed at a file that didn't exist) and driver_install_
       log.txt showed absolutely nothing explaining why -- just a bare
       "service STILL MISSING" from the caller with zero detail on which
       of the several steps here actually failed or with what Win32 error.
       Every failure path now logs exactly that. */
    if (GetFileAttributesA(inf_path) == INVALID_FILE_ATTRIBUTES) {
        log_line("create_root_device(%s): inf_path does not exist: %s", hardware_id, inf_path);
        return 0;
    }

    HDEVINFO devInfo = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (devInfo == INVALID_HANDLE_VALUE) {
        log_line("create_root_device(%s): SetupDiCreateDeviceInfoList failed (error %lu)", hardware_id,
                  (unsigned long)GetLastError());
        return 0;
    }

    SP_DEVINFO_DATA devInfoData = {0};
    devInfoData.cbSize = sizeof(devInfoData);
    int ok = SetupDiCreateDeviceInfoA(devInfo, "System", &GUID_DEVCLASS_SYSTEM, NULL, NULL, DICD_GENERATE_ID,
                                       &devInfoData);
    if (!ok) log_line("create_root_device(%s): SetupDiCreateDeviceInfoA failed (error %lu)", hardware_id,
                        (unsigned long)GetLastError());
    if (ok) {
        /* Hardware ID list must be double-null-terminated. */
        char hwidList[256] = {0};
        strncpy(hwidList, hardware_id, sizeof(hwidList) - 2);
        ok = SetupDiSetDeviceRegistryPropertyA(devInfo, &devInfoData, SPDRP_HARDWAREID, (const BYTE *)hwidList,
                                                (DWORD)(strlen(hwidList) + 2));
        if (!ok) log_line("create_root_device(%s): SetupDiSetDeviceRegistryPropertyA failed (error %lu)",
                            hardware_id, (unsigned long)GetLastError());
    }
    if (ok) {
        ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devInfo, &devInfoData);
        if (!ok) log_line("create_root_device(%s): SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed (error %lu)",
                            hardware_id, (unsigned long)GetLastError());
    }
    if (ok) {
        BOOL rebootRequired = FALSE;
        ok = UpdateDriverForPlugAndPlayDevicesA(NULL, hardware_id, inf_path, INSTALLFLAG_FORCE, &rebootRequired);
        if (!ok) log_line("create_root_device(%s): UpdateDriverForPlugAndPlayDevicesA(%s) failed (error %lu)",
                            hardware_id, inf_path, (unsigned long)GetLastError());
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return ok;
}

/* Real, serious incident this fixes (2026-09-07): repeated create_root_
   device() calls across a long day of install/uninstall/reinstall testing
   left TWO separate root-enumerated "vJoy Device" instances present at
   once, both fully functional, neither ever cleaned up by anything --
   confirmed by the user actually seeing duplicate devices in Device
   Manager. Combined with the user's explicit, direct requirement
   afterward ("anything hotas fix adds it will remove when it is closed,
   not idle, not keep secret") to make vJoy's device session-scoped
   instead of persistent: this is the mirror-image counterpart to
   create_root_device(), using SetupAPI's own DIF_REMOVE (the same
   class-installer action Device Manager's "Uninstall device" button
   issues for a local device) rather than shelling out to
   `pnputil /remove-device` -- confirmed necessary after a manual
   pnputil removal on this exact machine left one child device orphaned
   in an "Unknown" state instead of being cleanly removed with its
   parent.

   Loops rather than doing one enumeration pass: SetupDiCallClassInstaller
   changes the device list out from under any remaining index-based
   enumeration, so each match is handled against a freshly-fetched list
   instead of trusting stale indices. Called both to clean up on a normal
   exit AND, before ever creating a new device, to self-heal any leftover
   from a previous session that didn't exit cleanly (crash, force-kill,
   power loss) -- that self-heal-on-launch step is what guarantees nothing
   can silently accumulate across sessions even in the worst case, since a
   single exit-time removal alone can never be 100% guaranteed to run.
   Must run elevated -- true for this whole process now via the embedded
   manifest requiring administrator, see hotas_relay.manifest. Returns how
   many device instances were actually removed. */
static int destroy_vjoy_root_devices(void) {
    int removed = 0;
    for (;;) {
        /* Enumerating the whole device tree (no Enumerator filter), not
           just the "ROOT" branch -- confirmed necessary the hard way: a
           manual `pnputil /remove-device` on the root "vJoy Device" node
           earlier this same session left one of its own child devices
           orphaned, enumerated under a completely different branch
           ("VJOYRAWPDO", vjoy's own raw-PDO bus enumerator, not "ROOT" at
           all). Restricting to "ROOT" here would silently miss exactly
           that kind of leftover child -- the one failure mode this whole
           feature exists to prevent. */
        HDEVINFO devInfo = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES);
        if (devInfo == INVALID_HANDLE_VALUE) break;

        int found_one = 0;
        SP_DEVINFO_DATA devInfoData = {.cbSize = sizeof(SP_DEVINFO_DATA)};
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); i++) {
            char hwid[256] = {0};
            SetupDiGetDeviceRegistryPropertyA(devInfo, &devInfoData, SPDRP_HARDWAREID, NULL, (BYTE *)hwid,
                                                sizeof(hwid) - 1, NULL); /* result ignored -- absent is fine, checked below */
            for (char *p = hwid; *p; p++) *p = (char)tolower((unsigned char)*p);

            char instanceId[256] = {0};
            SetupDiGetDeviceInstanceIdA(devInfo, &devInfoData, instanceId, sizeof(instanceId), NULL);
            for (char *p = instanceId; *p; p++) *p = (char)tolower((unsigned char)*p);

            /* Matches either the root bus device itself (by hardware ID)
               or one of its raw-PDO children (by instance ID pattern,
               since those don't carry vJoy's hardware ID at all) --
               leaves every other device on the machine, of any kind,
               completely untouched. */
            if (!strstr(hwid, "vid_1234&pid_bead") && !strstr(instanceId, "vjoyrawpdo")) continue;

            if (SetupDiCallClassInstaller(DIF_REMOVE, devInfo, &devInfoData)) {
                removed++;
                log_line("destroy_vjoy_root_devices: removed one vJoy device instance (%s)", instanceId);
            } else {
                log_line("destroy_vjoy_root_devices: DIF_REMOVE failed on %s (error %lu)", instanceId,
                          (unsigned long)GetLastError());
            }
            found_one = 1;
            break; /* the list is stale now -- re-fetch fresh rather than trust the remaining indices */
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        if (!found_one) break; /* nothing left matching -- done */
    }
    return removed;
}

/* vJoy's own driver install does NOT configure which axes/buttons/POVs any
   of its 16 possible virtual devices actually expose -- that's normally a
   separate, manual step through the vJoyConf GUI (not bundled -- it ships
   with the full installer we deliberately don't use). Skipping it isn't
   an option: AcquireVJD on an unconfigured device slot just fails
   outright, confirmed directly -- a fresh driver install with no device
   configured behaves identically to vJoy not being installed at all, as
   far as this relay is concerned. So this bakes in the exact same raw HID
   report descriptor bytes already confirmed working all through this
   project (X/Y/Z/RX/RY/RZ at full 16-bit range, NUM_VJOY_BUTTONS buttons,
   0 POVs) and writes them straight into the registry vJoy itself reads
   them from (HKLM\SYSTEM\CurrentControlSet\Services\vjoy\Parameters\Device0<N>),
   rather than depending on a GUI tool or a personal backup a friend's
   fresh machine would never have. Must run elevated.

   Button usage-max/report-count (originally 0x10 = 16 both places) were
   bumped to 0x12 = 18 to add the PS button and touchpad click, with the
   trailing constant-padding field's report size shrunk from 0x70 (112
   bits) to 0x6e (110 bits) by that same 2 bits so the total report stays
   the same 64-byte size it always was -- NUM_VJOY_BUTTONS above must
   match this descriptor's button count, or vJoy will configure a device
   with a different number of buttons than this relay actually drives. */
static const BYTE VJOY_HID_REPORT_DESCRIPTOR[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xa1, 0x01, 0x05, 0x01, 0x85, 0x02, 0x09, 0x01, 0x15, 0x00,
    0x26, 0xff, 0x7f, 0x75, 0x20, 0x95, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81,
    0x02, 0x09, 0x32, 0x81, 0x02, 0x09, 0x33, 0x81, 0x02, 0x09, 0x34, 0x81, 0x02, 0x09, 0x35, 0x81,
    0x02, 0x81, 0x01, 0x81, 0x01, 0xc0, 0x75, 0x20, 0x95, 0x04, 0x81, 0x01, 0x05, 0x09, 0x15, 0x00,
    0x25, 0x01, 0x55, 0x00, 0x65, 0x00, 0x19, 0x01, 0x29, 0x12, 0x75, 0x01, 0x95, 0x12, 0x81, 0x02,
    0x75, 0x6e, 0x95, 0x01, 0x81, 0x01, 0xc0,
};

/* Byte-for-byte against VJOY_HID_REPORT_DESCRIPTOR, not just "is some
   descriptor present" -- the button count grew from 16 to
   NUM_VJOY_BUTTONS (18) without the array's total byte length changing
   (only 3 individual bytes differ: button usage-max/report-count and the
   padding field's size), so a presence-only check would never notice an
   old, already-configured device needed re-configuring for the new
   button count. Confirmed necessary: this exact machine's own vJoy
   device was already configured earlier this session with the old
   16-button descriptor. */
static int vjoy_device_already_configured(UINT device_id) {
    char key_path[128];
    snprintf(key_path, sizeof(key_path), "SYSTEM\\CurrentControlSet\\Services\\vjoy\\Parameters\\Device%02u",
              device_id);
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return 0;
    BYTE buf[sizeof(VJOY_HID_REPORT_DESCRIPTOR)];
    DWORD size = sizeof(buf), type = 0;
    LONG r = RegQueryValueExA(key, "HidReportDesctiptor", NULL, &type, buf, &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_BINARY && size == sizeof(VJOY_HID_REPORT_DESCRIPTOR) &&
           memcmp(buf, VJOY_HID_REPORT_DESCRIPTOR, sizeof(VJOY_HID_REPORT_DESCRIPTOR)) == 0;
}

/* Elevated: writes the descriptor, then restarts vJoy's device so the
   already-loaded driver actually picks up the change (registry writes to
   a running driver's parameters aren't read live otherwise -- confirmed:
   without this restart, the device stays unusable until something else
   happens to cycle it). */
static int configure_vjoy_device(UINT device_id) {
    char key_path[128];
    snprintf(key_path, sizeof(key_path), "SYSTEM\\CurrentControlSet\\Services\\vjoy\\Parameters\\Device%02u",
              device_id);
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, key_path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return 0;
    LONG r1 = RegSetValueExA(key, "HidReportDesctiptor", 0, REG_BINARY, VJOY_HID_REPORT_DESCRIPTOR,
                              sizeof(VJOY_HID_REPORT_DESCRIPTOR));
    DWORD descSize = sizeof(VJOY_HID_REPORT_DESCRIPTOR);
    LONG r2 = RegSetValueExA(key, "HidReportDesctiptorSize", 0, REG_DWORD, (const BYTE *)&descSize, sizeof(descSize));
    RegCloseKey(key);
    if (r1 != ERROR_SUCCESS || r2 != ERROR_SUCCESS) return 0;

    /* Simplest reliable restart available without extra dependencies:
       stop then start the service. vJoy's own service (unlike HidHide's)
       accepts this fine. */
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceA(scm, "vjoy", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
        if (svc) {
            SERVICE_STATUS status;
            ControlService(svc, SERVICE_CONTROL_STOP, &status);
            for (int i = 0; i < 50; i++) {
                if (!QueryServiceStatus(svc, &status) || status.dwCurrentState == SERVICE_STOPPED) break;
                Sleep(100);
            }
            StartServiceA(svc, 0, NULL);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    return 1;
}

/* Checked once at startup, before anything else -- if run_uninstall() left
   REBOOT_PENDING_FILE behind (some service still showed present right
   after uninstalling), and a reboot genuinely hasn't happened yet (i.e.
   at least one of the three still shows present), stop here with a clear
   explanation instead of letting the caller run into the confusing
   endless "Could not acquire vJoy device #2" retry loop that's otherwise
   the only visible symptom -- confirmed on real hardware this is exactly
   what happens if this isn't caught early. If the services are now
   actually gone (the reboot happened), silently deletes the marker and
   lets the normal flow continue -- reminds every launch until it's truly
   resolved, without nagging once it is. */
static void check_pending_reboot(void) {
    char marker_path[MAX_PATH + 32];
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    snprintf(marker_path, sizeof(marker_path), "%s%s", exe_dir, REBOOT_PENDING_FILE);

    WIN32_FILE_ATTRIBUTE_DATA marker_info;
    if (!GetFileAttributesExA(marker_path, GetFileExInfoStandard, &marker_info)) return; /* no pending reboot on record */

    /* If Windows has booted since this marker was written, a reboot DID
       happen -- whether some service still shows present now is then a
       separate, unrelated problem (a failed pnputil /delete-driver, or a
       driver reinstalled by hand outside this tool) that check_and_install_
       drivers()'s own self-healing logic should deal with, not something
       this hard stop should keep blocking on forever. Comparing the
       marker's own write time against the machine's actual boot time (now
       minus uptime) instead of only re-checking the services is what makes
       this self-clearing rather than a permanent brick if any one driver's
       removal never fully resolves even after a real reboot happened. */
    ULARGE_INTEGER now_ticks;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    now_ticks.LowPart = now_ft.dwLowDateTime;
    now_ticks.HighPart = now_ft.dwHighDateTime;
    ULONGLONG boot_time_100ns = now_ticks.QuadPart - (GetTickCount64() * 10000ULL);

    ULARGE_INTEGER marker_write;
    marker_write.LowPart = marker_info.ftLastWriteTime.dwLowDateTime;
    marker_write.HighPart = marker_info.ftLastWriteTime.dwHighDateTime;

    if (marker_write.QuadPart < boot_time_100ns) {
        DeleteFileA(marker_path); /* the system has rebooted since this was written */
        return;
    }

    int still_pending = service_exists("vjoy") == 1 || service_exists("HidHide") == 1 ||
                          service_exists("ViGEmBus") == 1;
    if (!still_pending) {
        DeleteFileA(marker_path); /* reboot happened since the uninstall -- fully resolved, stop reminding */
        return;
    }

    /* Same real OS-level prompt run_uninstall() offers right after
       uninstalling -- launching this tool again before actually rebooting
       is exactly the moment a friend most needs the option right in front
       of them instead of just being told to go do it themselves. Choosing
       No here falls through to fatal_exit() below, same explanation as
       before. Only offered at all if a re-scan confirms no leftover
       HidHide filter-driver registry references -- see clean_hidhide_
       filters()'s comment for exactly why that matters before ever
       inviting a reboot. If the scan itself fails (-1) that's treated as
       "not verified", same as "not clean" -- never assume safe. */
    if (count_hidhide_filter_refs() == 0) {
        offer_reboot_now(
            "A reboot is still needed to finish removing vJoy/HIDHide/ViGEmBus from your last uninstall.");
    }

    fatal_exit("A reboot is still needed to finish removing vJoy/HIDHide/ViGEmBus from your last "
                "uninstall.\n\nRestart Windows, then run this again -- it'll reinstall/reconfigure "
                "everything fresh automatically once the reboot clears the pending removal.\n\n"
                "(Already rebooted and still seeing this? Delete reboot_pending.txt next to this exe "
                "and run it again.)");
}

/* Checks vJoy/HIDHide/ViGEmBus and installs whichever are missing from the
   bundled drivers\ folder next to this exe (resolved relative to the exe's
   own directory, not the current working directory, so this works
   regardless of how/from-where it's launched). Safe to call every run --
   already-installed drivers are just reported and skipped. */
static void check_and_install_drivers(void) {
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    /* Start each run with a clean log rather than growing forever --
       installs are a rare, one-off-ish event, only the latest run's
       detail matters for diagnosing it. */
    char log_path[MAX_PATH + 32];
    get_install_log_path(log_path, sizeof(log_path));
    FILE *fresh = fopen(log_path, "w");
    if (fresh) fclose(fresh);
    log_line("=== Driver check/install run ===");

    struct {
        const char *display;
        const char *service;
        const char *inf_relpath;
        const char *hardware_id;
    } drivers[] = {
        {"vJoy", "vjoy", "drivers\\vjoy\\vjoy.inf", "root\\VID_1234&PID_BEAD&REV_0219"},
        {"HIDHide", "HidHide", "drivers\\hidhide\\HidHide.inf", "root\\HidHide"},
        {"ViGEmBus", "ViGEmBus", "drivers\\vigembus\\ViGEmBus.inf", "Nefarius\\ViGEmBus\\Gen1"},
    };

    /* Figure out everything that needs doing FIRST (all plain reads, no
       elevation needed for any of these checks), then run it all through
       ONE elevated script at the end -- confirmed necessary on real
       hardware: the original version called run_elevated() once per
       missing driver plus once for the OEMName rename plus once for vJoy
       device config, popping a separate UAC prompt for each (up to 5 on
       a genuinely fresh machine) -- the same friction problem the
       uninstaller had, fixed the same way here. */
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    int driver_missing[3] = {0, 0, 0};
    int any_missing = 0;
    printf("Checking for required drivers...\n\n");
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        int r = service_exists(drivers[i].service);
        if (r < 0) {
            printf("  %-10s could not check (failed to open Service Control Manager)\n", drivers[i].display);
            log_line("%s: could not check SCM", drivers[i].display);
        } else if (r) {
            printf("  %-10s already installed\n", drivers[i].display);
            log_line("%s: already installed", drivers[i].display);
        } else {
            driver_missing[i] = 1;
            any_missing = 1;
            printf("  %-10s NOT installed -- will install its signed driver package.\n", drivers[i].display);
            log_line("%s: NOT installed, will create device %s", drivers[i].display, drivers[i].hardware_id);
        }
    }

    int need_oem_rename = service_exists("vjoy") == 1 && !oem_name_already_set();
    int need_vjoy_config = service_exists("vjoy") == 1 && !vjoy_device_already_configured(2);
    /* vjoy/HidHide/ViGEmBus not existing yet also means their device
       won't be configured/renamed yet either -- if vJoy is among the
       ones about to be installed below, these two need to happen too,
       even though the checks just above (run before installing anything)
       couldn't have seen that. */
    if (driver_missing[0]) { need_oem_rename = 1; need_vjoy_config = 1; }

    if (!any_missing && !need_oem_rename && !need_vjoy_config) {
        printf("\n");
        return; /* nothing to elevate for at all -- no UAC prompt needed */
    }

    /* HARD requirement here too, same as run_uninstall() -- a driver
       install is exactly the other half of the kind of system change
       that caused the 2026-09-07 incident (see begin_system_restore_
       point()'s comment). Only reached when an actual install is about
       to happen (the early return above already handles "nothing to do"),
       so this doesn't add a restore point to every ordinary launch --
       only to the ones that actually change something. */
    printf("\nCreating a Windows System Restore point before installing anything...\n");
    long long restore_seq = begin_system_restore_point("Before hotas_relay driver install (vJoy/HIDHide/ViGEmBus)");
    if (restore_seq < 0) {
        printf("Could not create a System Restore point -- STOPPING, nothing was installed.\n");
        printf("Opening Windows' System Protection settings now -- turn protection ON for\n");
        printf("drive C:, then run this program again.\n");
        log_line("driver install: ABORTED -- could not create a required System Restore point");
        offer_to_enable_system_restore();
        fatal_exit("Could not create a required System Restore point -- see above.");
    }
    printf("Restore point created.\n");

    printf("\nWindows will ask you to approve this once -- that's the normal UAC\n");
    printf("prompt, for setting up the device(s).\n");

    char script[8192];
    int len = 0;
    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        if (!driver_missing[i]) continue;
        char inf_path[MAX_PATH * 2];
        snprintf(inf_path, sizeof(inf_path), "%s%s", exe_dir, drivers[i].inf_relpath);
        /* Self-invoke --create-root-device rather than call
           create_root_device() in-process: its SetupAPI calls need to run
           AS the elevated user, not be spawned as a separate elevated
           child of an unelevated caller. Also NOT plain
           pnputil /add-driver /install -- confirmed that only binds to a
           device already present in some form, and silently creates
           nothing new otherwise (that's what caused ViGEmBus, and
           possibly this exact scenario for anyone with none of these ever
           installed before, to go through the motions and still not
           actually end up installed). */
        len += snprintf(script + len, sizeof(script) - len,
                          "\"%s\" --create-root-device \"%s\" \"%s\" >> \"%s\" 2>&1\r\n", exe_path,
                          drivers[i].hardware_id, inf_path, log_path);
    }
    if (need_oem_rename) {
        /* vJoy's virtual joystick(s) otherwise show up in Windows' Game
           Controllers panel (joy.cpl) as the generic "vJoy Device" --
           rename it to something that means something to a friend setting
           this up. OEMName is a plain, unrelated registry value Windows'
           legacy joystick subsystem reads purely for display (confirmed
           against how a real Xbox 360 controller's own OEMName is set, at
           VID_045E&PID_028E, the same way) -- doesn't touch or invalidate
           vJoy's signed driver files. */
        len += snprintf(script + len, sizeof(script) - len,
                          "reg.exe add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\"
                          "PrivateProperties\\Joystick\\OEM\\VID_1234&PID_BEAD\" /v OEMName /t REG_SZ /d "
                          "\"WARDOGS HOTAS Controller\" /f >> \"%s\" 2>&1\r\n",
                          log_path);
    }
    if (need_vjoy_config) {
        /* vJoy device #2 (the one this relay always acquires -- see
           AcquireVJD calls further down) needs its own axis/button/POV
           layout configured separately from the driver itself --
           confirmed directly: without this, AcquireVJD on a freshly-
           installed vJoy fails outright, no different from vJoy not
           being installed at all. */
        len += snprintf(script + len, sizeof(script) - len, "\"%s\" --configure-vjoy 2 >> \"%s\" 2>&1\r\n", exe_path,
                          log_path);
    }

    int elevated_ok = run_elevated_script(script);
    end_system_restore_point(restore_seq); /* close the bracket either way -- an unclosed BEGIN is an
                                                incomplete restore point, not a usable one */
    if (!elevated_ok) {
        printf("Could not launch the setup steps (UAC declined, or not found).\n");
        log_line("driver install: could not launch combined elevated script (UAC declined, or not found)");
        printf("\n");
        return;
    }

    for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        if (!driver_missing[i]) continue;
        int r2 = service_exists(drivers[i].service);
        printf("  %-10s %s\n", drivers[i].display,
               r2 ? "Installed successfully." : "Still not detected -- install may not have completed.");
        log_line("%s: after install attempt, service %s", drivers[i].display,
                  r2 ? "now exists (success)" : "STILL MISSING (see log above)");
    }
    if (any_missing) {
        printf("\n(Full details for the above saved to %s next to this exe.)\n", INSTALL_LOG_FILE);
    }
    if (need_oem_rename) printf("\nGave the virtual controller a clearer name in Windows.\n");
    if (need_vjoy_config) {
        printf("Set up the virtual controller's buttons/axes.\n");
        log_line("vjoy: device #2 configure attempt finished");
    }
    printf("\n");
}

/* Non-elevated stdout capture for any command -- same pipe pattern as
   run_hidhide_capture() above, just not hardcoded to HidHideCLI. Used
   below for `pnputil /enum-drivers`, which is a read-only query and
   needs no elevation at all -- confirmed: only pnputil's mutating verbs
   (/add-driver, /delete-driver, etc.) need UAC. */
static size_t run_capture_plain(const char *program, const char *args, char *out, size_t out_size) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "%s %s", program, args);

    HANDLE readPipe, writePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return 0;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { .cb = sizeof(si), .dwFlags = STARTF_USESTDHANDLES, .hStdOutput = writePipe,
                         .hStdError = writePipe };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return 0;
    }

    size_t total = 0;
    DWORD n;
    while (total < out_size - 1 && ReadFile(readPipe, out + total, (DWORD)(out_size - 1 - total), &n, NULL) &&
           n > 0) {
        total += n;
    }
    out[total] = '\0';

    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    return total;
}

/* `pnputil /delete-driver` needs the driver STORE's own "oemNN.inf" name
   for a package, not the original .inf filename we published it from --
   `/enum-drivers` lists both together, one block per published package:
     Published Name:     oemNN.inf
     Original Name:       vjoy.inf
     ...
   Runs it ONCE (no elevation needed, see run_capture_plain() above) and
   finds all three of this relay's driver packages in one pass -- each
   out param is set to an empty string if that driver's package wasn't
   found (already removed, or never actually installed via pnputil). */
static void find_all_oem_names(char *vjoy_oem, size_t vjoy_len, char *hidhide_oem, size_t hidhide_len,
                                 char *vigembus_oem, size_t vigembus_len) {
    vjoy_oem[0] = hidhide_oem[0] = vigembus_oem[0] = '\0';
    static char buf[65536];
    if (run_capture_plain("pnputil", "/enum-drivers", buf, sizeof(buf)) == 0) return;

    char *pos = buf;
    for (;;) {
        char *pub = strstr(pos, "Published Name");
        if (!pub) return;
        char *next_pub = strstr(pub + 1, "Published Name");
        char *block_end = next_pub ? next_pub : buf + strlen(buf);
        char saved = *block_end;
        *block_end = '\0';

        char *orig = strstr(pub, "Original Name");
        if (orig) {
            char *ocolon = strchr(orig, ':');
            if (ocolon) {
                ocolon++;
                while (*ocolon == ' ' || *ocolon == '\t') ocolon++;
                char orig_value[64] = {0};
                size_t on = 0;
                while (ocolon[on] && ocolon[on] != '\r' && ocolon[on] != '\n' && on < sizeof(orig_value) - 1) {
                    orig_value[on] = (char)tolower((unsigned char)ocolon[on]);
                    on++;
                }
                char *target = NULL;
                size_t target_len = 0;
                if (!strcmp(orig_value, "vjoy.inf")) { target = vjoy_oem; target_len = vjoy_len; }
                else if (!strcmp(orig_value, "hidhide.inf")) { target = hidhide_oem; target_len = hidhide_len; }
                else if (!strcmp(orig_value, "vigembus.inf")) { target = vigembus_oem; target_len = vigembus_len; }
                if (target) {
                    char *colon = strchr(pub, ':');
                    if (colon) {
                        colon++;
                        while (*colon == ' ' || *colon == '\t') colon++;
                        size_t n = 0;
                        while (colon[n] && colon[n] != '\r' && colon[n] != '\n' && n < target_len - 1) {
                            target[n] = colon[n];
                            n++;
                        }
                        target[n] = '\0';
                    }
                }
            }
        }
        *block_end = saved;
        pos = block_end;
    }
}

/* Runs a whole pre-built multi-line script elevated (ONE UAC prompt) via
   a tiny generated batch file, blocking until it finishes. Each line in
   script_lines is expected to already end with its own output redirect
   (`>> "driver_install_log.txt path" 2>&1`), appending so a whole run's
   worth of steps land in one readable file in order -- without this, an
   elevated child's console output is effectively invisible (it flashes
   by in its own window, or vanishes entirely if this exe was launched by
   double-click and the whole window closes the instant the process
   exits).

   Goes through a .bat file rather than `cmd.exe /c "<command>"` directly:
   cmd's /c parsing has a well-known quirk where, unless the command line
   contains EXACTLY 2 quote characters, it strips the first and last quote
   it finds (not a matched pair) instead of leaving quoted spans alone --
   fine with one quoted path, silently mangled the moment a second one
   is needed (e.g. this exe's own path AND a driver's .inf path in the
   same command). A batch file's own lines aren't parsed with that quirk
   at all, so this works regardless of how many quoted arguments are
   involved.

   Batching every step of a whole install/uninstall run into ONE call to
   this (rather than one call per step) is itself a real, confirmed-
   necessary fix, not just tidiness: an earlier per-step version of both
   the installer and the uninstaller prompted for UAC once per command --
   up to a dozen separate prompts in one run -- exactly the kind of
   friction this whole project has otherwise gone out of its way to avoid.

   Returns 1 if it ran (regardless of its own exit code -- callers
   re-check service/registry state afterward to know for sure), 0 if it
   couldn't even be launched (e.g. UAC declined). */
static int run_elevated_script(const char *script_lines) {
    char bat_path[MAX_PATH + 32];
    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    snprintf(bat_path, sizeof(bat_path), "%s_elevated_step.bat", exe_dir);

    FILE *bat = fopen(bat_path, "w");
    if (!bat) return 0;
    fprintf(bat, "@echo off\r\n%s", script_lines);
    fclose(bat);

    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";
    sei.lpFile = bat_path;
    sei.nShow = SW_HIDE;
    int launched = ShellExecuteExA(&sei);
    if (launched && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        CloseHandle(sei.hProcess);
        DeleteFileA(bat_path); /* only safe to delete once we know the elevated process actually finished with it */
    }
    /* If launched but sei.hProcess came back NULL (a real, if rare,
       ShellExecuteExA/UAC quirk -- runas doesn't always hand back a
       waitable handle), do NOT delete the .bat here: the elevated process
       may still be starting up asynchronously and reading it. Leaving it
       behind is harmless (no secrets in it, and the next run overwrites it
       before its next use) -- deleting out from under a process that
       hasn't run yet is the actual risk. */
    return launched ? 1 : 0;
}

/* A normal user process doesn't have SeShutdownPrivilege active by default
   even on an account that holds it -- same "enable before use" dance as
   SE_DEBUG_NAME and friends. Required before ExitWindowsEx(EWX_REBOOT, ...)
   below will succeed. */
static int enable_shutdown_privilege(void) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return 0;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    int ok = LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

/* Real Windows-level restart prompt, not just a console line -- but
   deliberately NOT shutdown.exe's own /t <seconds> countdown-that-fires-
   unless-cancelled behavior either: the user explicitly wants this to ask
   and then wait for an actual answer, with nothing happening on its own if
   ignored. A plain MessageBoxA is exactly that -- it blocks right here
   until Yes/No is clicked, and "No" leaves the machine completely alone.
   The "or schedules one" half of that ask is really just the existing
   reboot_pending.txt reminder (see check_pending_reboot()) -- asking again
   next launch instead of demanding an immediate answer now, without
   needing any actual OS-level Task Scheduler entry for it. */
static void offer_reboot_now(const char *context_msg) {
    char msg[512];
    snprintf(msg, sizeof(msg),
              "%s\n\nRestart Windows now?\n\nChoose No and you'll be reminded again next time you run "
              "this tool -- nothing restarts on its own either way.",
              context_msg);
    int choice = MessageBoxA(NULL, msg, "Reboot needed", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
    if (choice != IDYES) return;

    if (!enable_shutdown_privilege()) {
        MessageBoxA(NULL, "Couldn't get permission to restart Windows from here -- please restart manually.",
                     "Reboot needed", MB_OK | MB_ICONWARNING);
        return;
    }
    log_line("reboot: user chose 'Restart now' from the OS-level prompt");
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_SOFTWARE | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED);
    /* If ExitWindowsEx itself failed (rare -- privilege enabled but still
       denied by policy, etc.), fall through and let the caller's normal
       flow continue rather than pretending a reboot is already underway. */
}

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* THE ACTUAL FIX for a real, serious incident (2026-09-07): a previous
   version of run_uninstall() removed HidHide's service and driver package
   but never released the UpperFilters/LowerFilters registry entries its
   driver adds to real device setup classes under
   HKLM\SYSTEM\CurrentControlSet\Control\Class -- including the generic
   HID class that keyboards and mice route through, not just game
   controllers. With HidHide's own service/driver gone but that class
   registration still pointing at it, Windows tried to load a filter
   driver that no longer existed on every single boot, which blocked ALL
   keyboard/mouse input system-wide -- no input at the login screen, in
   Safe Mode, or even in WinRE. Fixing it required booting from external
   installer media, loading the offline SYSTEM hive, and manually deleting
   the orphaned filter values. This function does exactly that surgery
   in code, and does it SURGICALLY: it only ever removes the literal
   string "HidHide" from these two REG_MULTI_SZ values, on whichever
   classes actually have it -- every other filter driver already present
   on a real machine (Steam's own "steamxbox", or any other remap tool a
   user has installed) is copied through completely untouched. Never
   touches anything outside these two value names under this one registry
   path. Self-invoked elevated via --clean-hidhide-filters, same pattern
   as --configure-vjoy/--create-root-device above. */
static int clean_hidhide_filters(void) {
    HKEY classesKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Class", 0, KEY_READ, &classesKey) !=
        ERROR_SUCCESS) {
        log_line("clean_hidhide_filters: couldn't open Control\\Class at all");
        return -1;
    }

    int cleaned = 0;
    char subkeyName[256];
    for (DWORD i = 0;; i++) {
        DWORD nameLen = sizeof(subkeyName);
        if (RegEnumKeyExA(classesKey, i, subkeyName, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

        HKEY subKey;
        if (RegOpenKeyExA(classesKey, subkeyName, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &subKey) != ERROR_SUCCESS)
            continue;

        static const char *filterNames[2] = {"UpperFilters", "LowerFilters"};
        for (int f = 0; f < 2; f++) {
            BYTE buf[2048];
            DWORD bufLen = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExA(subKey, filterNames[f], NULL, &type, buf, &bufLen) != ERROR_SUCCESS ||
                type != REG_MULTI_SZ)
                continue;

            char out[2048];
            DWORD outLen = 0;
            int found = 0;
            for (char *p = (char *)buf; (DWORD)(p - (char *)buf) < bufLen && *p; p += strlen(p) + 1) {
                if (streq_ci(p, "HidHide")) {
                    found = 1;
                    continue; /* the ONLY string this ever drops -- everything else is copied through as-is */
                }
                size_t n = strlen(p) + 1;
                memcpy(out + outLen, p, n);
                outLen += (DWORD)n;
            }
            if (!found) continue;

            if (outLen == 0) {
                RegDeleteValueA(subKey, filterNames[f]);
                log_line("clean_hidhide_filters: %s on %s was ONLY HidHide -- value removed", filterNames[f],
                          subkeyName);
            } else {
                out[outLen++] = '\0'; /* REG_MULTI_SZ's required final empty-string terminator */
                RegSetValueExA(subKey, filterNames[f], 0, REG_MULTI_SZ, (const BYTE *)out, outLen);
                log_line("clean_hidhide_filters: removed HidHide from %s on %s, other entries preserved",
                          filterNames[f], subkeyName);
            }
            cleaned++;
        }
        RegCloseKey(subKey);
    }
    RegCloseKey(classesKey);
    log_line("clean_hidhide_filters: done, %d filter value(s) had HidHide removed", cleaned);
    return cleaned;
}

/* Read-only, needs no elevation (HKLM\...\Control\Class is world-readable)
   -- used to PROVE clean_hidhide_filters() actually worked before
   run_uninstall() ever tells the user it's safe to reboot, instead of
   just hoping the elevated step succeeded. Same walk as above, just
   counts instead of modifying. -1 if the scan itself couldn't run at all
   (treated as "not verified", never as "verified clean"). */
static int count_hidhide_filter_refs(void) {
    HKEY classesKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Class", 0, KEY_READ, &classesKey) !=
        ERROR_SUCCESS)
        return -1;

    int total = 0;
    char subkeyName[256];
    for (DWORD i = 0;; i++) {
        DWORD nameLen = sizeof(subkeyName);
        if (RegEnumKeyExA(classesKey, i, subkeyName, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

        HKEY subKey;
        if (RegOpenKeyExA(classesKey, subkeyName, 0, KEY_QUERY_VALUE, &subKey) != ERROR_SUCCESS) continue;

        static const char *filterNames[2] = {"UpperFilters", "LowerFilters"};
        for (int f = 0; f < 2; f++) {
            BYTE buf[2048];
            DWORD bufLen = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExA(subKey, filterNames[f], NULL, &type, buf, &bufLen) != ERROR_SUCCESS ||
                type != REG_MULTI_SZ)
                continue;
            for (char *p = (char *)buf; (DWORD)(p - (char *)buf) < bufLen && *p; p += strlen(p) + 1) {
                if (streq_ci(p, "HidHide")) total++;
            }
        }
        RegCloseKey(subKey);
    }
    RegCloseKey(classesKey);
    return total;
}

/* Windows' own System Restore -- not anything custom, the actual proper
   rollback mechanism for a driver-level operation going wrong in some way
   this code doesn't already anticipate. Real, serious motivation: the
   2026-09-07 incident (see PROGRESS.md's "CRITICAL INCIDENT" section)
   that bricked keyboard/mouse system-wide required booting from external
   installer media and manually editing an offline registry hive to fix --
   ONLY because no System Restore point existed at all at the time. A
   restore point rolls back every driver AND the whole registry with a
   few clicks through Windows' own UI. This is defense in depth on top of
   the actual code fix for that incident (clean_hidhide_filters()), not a
   replacement for it -- it's what would catch whatever this tool gets
   wrong NEXT that hasn't been found yet. At the user's explicit
   direction, this is a HARD requirement for run_uninstall(), not a
   best-effort nicety: if a restore point can't be created, the uninstall
   does not proceed at all.

   Needs no elevation dance of its own -- this whole process is already
   elevated (see hotas_relay.manifest). Brackets the actual risky work
   with SRSetRestorePointW's BEGIN_SYSTEM_CHANGE/END_SYSTEM_CHANGE event
   pair (the documented way to use this API -- an unclosed BEGIN is
   treated as an incomplete/abandoned restore point by Windows), rather
   than one bare call. */
#define HOTAS_RESTOREPOINT_BEGIN 100  /* BEGIN_SYSTEM_CHANGE */
#define HOTAS_RESTOREPOINT_END 101    /* END_SYSTEM_CHANGE */
#define HOTAS_RESTOREPOINT_TYPE_DRIVER 10 /* DEVICE_DRIVER_INSTALL -- closest documented fit for what this touches */

typedef struct {
    DWORD dwEventType;
    DWORD dwRestorePtType;
    long long llSequenceNumber;
    WCHAR szDescription[256];
} HotasRestorePointInfoW;

typedef struct {
    DWORD nStatus;
    long long llSequenceNumber;
} HotasStateMgrStatus;

typedef BOOL(WINAPI *SRSetRestorePointW_t)(HotasRestorePointInfoW *, HotasStateMgrStatus *);

/* SRSetRestorePointW has no timeout parameter of its own, and is a real,
   synchronous, potentially slow system call -- confirmed on real hardware
   2026-09-07 that it can simply hang with zero feedback (no dialog, no
   error, just an unresponsive-looking wait) rather than fail fast, the
   exact opposite of what a "prevent problems" safety feature should ever
   do to someone. Run on its own thread so the caller can enforce a real
   timeout via WaitForSingleObject instead of blocking forever -- if it
   times out, this thread is simply abandoned (not forcibly killed
   mid-syscall, which risks corrupting whatever System Restore's own
   internal state was doing) and the caller treats it as a clean failure. */
typedef struct {
    SRSetRestorePointW_t pSet;
    HotasRestorePointInfoW info;
    HotasStateMgrStatus status;
    BOOL ok;
} RestorePointCallArgs;

static DWORD WINAPI restore_point_call_thread(LPVOID arg) {
    RestorePointCallArgs *a = (RestorePointCallArgs *)arg;
    a->ok = a->pSet(&a->info, &a->status);
    return 0;
}

/* Generous, not aggressive: the FIRST restore point ever created right
   after System Restore is freshly enabled can genuinely take a while
   (Windows setting up shadow-copy storage association on the volume for
   the first time) -- a short timeout here would fail fast but wrongly,
   treating "still legitimately working" the same as "actually hung".
   Bounded either way, which is the actual point -- never hangs forever
   again like the very first version of this code just did on real
   hardware. */
#define RESTOREPOINT_TIMEOUT_MS 90000

/* Returns the restore point's sequence number (needed to close it via
   end_system_restore_point() below), or -1 if it could not be created for
   any reason (System Restore disabled/blocked by policy, srclient.dll
   missing, the API itself failing or timing out). -1, NOT 0 -- confirmed
   on real hardware 2026-09-07 that a genuinely successful restore point
   on this machine legitimately came back with sequence number 0 (Windows'
   own Event Viewer independently confirmed "Successfully created restore
   point" for the exact same call this code was busy treating as a
   failure). Treating 0 as a failure sentinel was a real bug: it made this
   function report failure on an actual success, which meant end_system_
   restore_point() never got called, which meant the restore point never
   got properly closed/committed, which is why it never showed up in
   Get-ComputerRestorePoint despite Windows saying it worked. Never blocks
   longer than RESTOREPOINT_TIMEOUT_MS total. */
static long long begin_system_restore_point(const char *description) {
    /* Best-effort: make sure System Restore is actually turned on for
       C:\ first -- SRSetRestorePointW silently accomplishes nothing
       useful if it's disabled machine-wide. No simple native Win32 API
       for this specific toggle (it's WMI/COM-only under the hood), so
       this one step shells out to the same PowerShell cmdlet a person
       would run by hand; the actual restore point creation below still
       uses the real Win32 API directly, not PowerShell. Already bounded
       (15s) since this one goes through CreateProcess, not the unbounded
       in-process call below. */
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    char enableCmd[] = "powershell.exe -NoProfile -Command \"Enable-ComputerRestore -Drive 'C:\\'\"";
    if (CreateProcessA(NULL, enableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    HMODULE srclient = LoadLibraryA("srclient.dll");
    if (!srclient) {
        log_line("restore point: couldn't load srclient.dll");
        return -1;
    }
    SRSetRestorePointW_t pSet = (SRSetRestorePointW_t)GetProcAddress(srclient, "SRSetRestorePointW");
    if (!pSet) {
        log_line("restore point: SRSetRestorePointW not found in srclient.dll");
        FreeLibrary(srclient);
        return -1;
    }

    /* Heap-allocated, NOT a stack local: if the wait below times out, this
       function returns while the abandoned thread is still running (see
       the "abandoned, not killed" comment above), and it keeps writing
       into `*args` whenever SRSetRestorePointW eventually returns --
       seconds or minutes later. A stack-local `args` would by then belong
       to some completely different function's frame (real, confirmed-
       possible corruption, caught in the 2026-09-08 pre-ship audit: a
       late write from this exact zombie thread could land in, say,
       check_and_install_drivers()'s own script buffer). Freed on the
       normal-completion path below; deliberately leaked on the abandon
       path so a late write lands in unused heap memory instead of a live
       frame -- same accepted-risk shape as the FreeLibrary() call already
       made right next to it. */
    RestorePointCallArgs *args = calloc(1, sizeof(*args));
    if (!args) {
        log_line("restore point: out of memory allocating call args");
        FreeLibrary(srclient);
        return -1;
    }
    args->pSet = pSet;
    args->info.dwEventType = HOTAS_RESTOREPOINT_BEGIN;
    args->info.dwRestorePtType = HOTAS_RESTOREPOINT_TYPE_DRIVER;
    MultiByteToWideChar(CP_ACP, 0, description, -1, args->info.szDescription,
                          sizeof(args->info.szDescription) / sizeof(WCHAR));

    HANDLE thread = CreateThread(NULL, 0, restore_point_call_thread, args, 0, NULL);
    if (!thread) {
        log_line("restore point: couldn't even start the worker thread (error %lu)", (unsigned long)GetLastError());
        free(args);
        FreeLibrary(srclient);
        return -1;
    }
    DWORD waitResult = WaitForSingleObject(thread, RESTOREPOINT_TIMEOUT_MS);
    CloseHandle(thread);

    if (waitResult != WAIT_OBJECT_0) {
        log_line("restore point: SRSetRestorePointW did not return within %d seconds -- giving up "
                  "(this call is now abandoned, not killed, to avoid corrupting whatever it was doing)",
                  RESTOREPOINT_TIMEOUT_MS / 1000);
        FreeLibrary(srclient); /* the abandoned thread may still be inside this DLL's code -- freeing here is a
                                    known, accepted small risk, preferred over hanging the whole program forever */
        return -1; /* args intentionally NOT freed here -- see the allocation comment above */
    }

    FreeLibrary(srclient);
    /* Success is ok==TRUE and status.nStatus==0 (ERROR_SUCCESS) -- full
       stop, NOT also requiring a non-zero sequence number (see this
       function's own comment for the real incident that came from
       requiring that). Whatever sequence number came back, even 0, is
       the real one and gets returned so end_system_restore_point() can
       use it to actually close/commit the point. */
    long long result = -1;
    if (args->ok && args->status.nStatus == 0) {
        log_line("restore point: created, sequence %lld", args->status.llSequenceNumber);
        result = args->status.llSequenceNumber;
    } else {
        /* status.nStatus can legitimately be 0 here too -- the API returning
           FALSE (args->ok == 0) without ever populating `status` at all is a
           real, observed case, not just a theoretical one, so GetLastError()
           right after the call (captured inside the thread's own return path
           would be a race with other API calls on the main thread by the time
           we get here -- logging what we actually have instead of guessing). */
        log_line("restore point: failed to create (ok=%d status=%lu)", args->ok, (unsigned long)args->status.nStatus);
    }
    free(args); /* thread already confirmed finished (WAIT_OBJECT_0 above) -- safe to free now */
    return result;
}

/* Opens Windows' own System Protection settings dialog directly -- the
   exact place a person would turn System Restore on for C:\ by hand --
   instead of just telling them where to go look for it. Used when
   begin_system_restore_point() fails, since that's usually exactly
   because System Restore is off. Best-effort: if this doesn't launch for
   some reason, the printed instructions the caller already shows are
   still there as a fallback. */
static void offer_to_enable_system_restore(void) {
    ShellExecuteA(NULL, "open", "SystemPropertiesProtection.exe", NULL, NULL, SW_SHOW);
}

/* Closes out a restore point opened by begin_system_restore_point() --
   see that function's comment for why this matters (an unclosed BEGIN is
   an incomplete restore point, not a usable one). No-op if sequence_number
   is 0 (begin never actually succeeded, nothing to close). */
static void end_system_restore_point(long long sequence_number) {
    if (sequence_number < 0) return; /* begin_system_restore_point() never actually succeeded -- 0 is a
                                          legitimate real sequence number now, only negative means "no point" */
    HMODULE srclient = LoadLibraryA("srclient.dll");
    if (!srclient) return;
    SRSetRestorePointW_t pSet = (SRSetRestorePointW_t)GetProcAddress(srclient, "SRSetRestorePointW");
    if (pSet) {
        /* Same timeout-guarded pattern as begin_system_restore_point() --
           this is the same underlying blocking API call, no reason to
           trust it can't hang here too just because it's the "closing"
           half. Result isn't acted on either way (the restore point
           already has real content from the BEGIN call regardless of
           whether END completes cleanly), so this is best-effort: log
           and move on, never block the caller waiting on it. */
        /* Heap-allocated for the same reason as begin_system_restore_point()'s
           own args -- see that function's comment. This call site is the
           one actually hit by every driver install/uninstall run right
           after run_elevated_script() returns and the caller keeps right
           on running (printing results, checking backups, etc.), so a
           stack-local here really would get reused by the very next
           function call if this thread gets abandoned. */
        RestorePointCallArgs *args = calloc(1, sizeof(*args));
        if (args) {
            args->pSet = pSet;
            args->info.dwEventType = HOTAS_RESTOREPOINT_END;
            args->info.dwRestorePtType = HOTAS_RESTOREPOINT_TYPE_DRIVER;
            args->info.llSequenceNumber = sequence_number;

            HANDLE thread = CreateThread(NULL, 0, restore_point_call_thread, args, 0, NULL);
            if (thread) {
                DWORD waitResult = WaitForSingleObject(thread, RESTOREPOINT_TIMEOUT_MS);
                CloseHandle(thread);
                log_line(waitResult == WAIT_OBJECT_0 ? "restore point: closed, sequence %lld"
                                                      : "restore point: END call did not return in time for sequence %lld "
                                                        "(abandoned, not fatal -- the point itself is already usable)",
                          sequence_number);
                if (waitResult == WAIT_OBJECT_0) free(args); /* leaked on the abandon path, on purpose -- see above */
            } else {
                free(args); /* thread never started, nothing can still be using this */
            }
        }
    }
    FreeLibrary(srclient);
}

/* Full, clean removal of everything check_and_install_drivers() sets up --
   the driver packages (via pnputil, reversing the install), their
   services, and the vJoy OEMName rename. Explicitly does NOT touch this
   exe itself, ruthless_controller_relay.ini, or the dist\ folder -- an uninstaller that
   can't finish because it just deleted itself mid-run is worse than one
   that leaves a few of its own files behind for the user to delete by
   hand afterward.

   Why this exists at all: some games' kernel-level anti-cheat (confirmed
   2026-09-07: Battlefield 6's "Javelin" refuses to even launch with
   ViGEmBus-based tools present, and has community reports of blocking
   HidHide too) can flag the mere PRESENCE of these drivers as installed,
   regardless of whether they're actively doing anything. A friend who
   wants to play an anti-cheat-protected game after using this for WARDOGS
   needs a real way to fully remove everything first, not just stop using
   it -- an idle-but-installed driver is still visible to a driver
   enumeration scan.

   Backs up vJoy's OEMName + device #2 config to .reg files (same
   pre_uninstall_backup\ convention used elsewhere in this project) as
   part of the SAME elevated script -- real safety without a second UAC
   prompt for it. Every driver's stop/delete/uninstall runs in that one
   script too; a failure on one (HidHide's "sc stop" refusing outright
   with error 1052 is its own known anti-tamper trait, not a bug) doesn't
   stop or corrupt the others, since these are independent, isolated
   commands, not a single all-or-nothing transaction. */
static int run_uninstall(void) {
    printf("=== Uninstall vJoy, HIDHide, and ViGEmBus ===\n\n");
    printf("This removes the driver packages and services this relay installed:\n");
    printf("  - vJoy (virtual joystick)\n");
    printf("  - HIDHide (hides the real controller from games)\n");
    printf("  - ViGEmBus (virtual Xbox/DS4 gamepad emulation)\n\n");
    printf("It does NOT delete this exe, ruthless_controller_relay.ini, or this folder -- only\n");
    printf("the system-wide drivers/services. Windows will ask you to approve\n");
    printf("this ONE time (a single UAC prompt covers everything below).\n\n");
    printf("Safety net: vJoy's current name/config is backed up to a .reg file\n");
    printf("first (see below), and if you change your mind afterward, just run\n");
    printf("RuthlessControllerRelay.exe normally again -- it automatically detects anything\n");
    printf("missing and reinstalls/reconfigures it from scratch, the same way it\n");
    printf("does on a friend's brand new PC. Nothing here is truly one-way.\n\n");
    printf("Each driver below is removed independently -- if one fails or is\n");
    printf("only partially removed, it does not affect the others or leave\n");
    printf("Windows itself in a broken state; these are ordinary, isolated\n");
    printf("third-party driver removals, the same operation Device Manager's\n");
    printf("own \"Uninstall device\" button performs.\n\n");
    printf("A reboot afterward is recommended to fully clear everything, even if\n");
    printf("this reports success.\n\n");
    printf("Continue? [y/N]: ");
    fflush(stdout);

    char answer[16] = {0};
    if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
        printf("Cancelled -- nothing was changed.\n");
        return 0;
    }

    /* HARD requirement, not best-effort -- explicit direction after the
       2026-09-07 incident: this uninstall does not proceed at all unless
       a real Windows System Restore point was actually created first.
       See begin_system_restore_point()'s comment for why this exists. */
    printf("\nCreating a Windows System Restore point before doing anything else...\n");
    long long restore_seq = begin_system_restore_point("Before hotas_relay uninstall (vJoy/HIDHide/ViGEmBus)");
    if (restore_seq < 0) {
        printf("\nCould not create a System Restore point -- STOPPING, nothing was changed.\n");
        printf("This is a hard requirement, not a skippable warning. Possible causes: System\n");
        printf("Restore is disabled by Group Policy, or disk space for it is exhausted.\n");
        printf("Opening Windows' System Protection settings now -- turn protection ON for\n");
        printf("drive C:, then run this uninstall again.\n");
        log_line("uninstall: ABORTED -- could not create a required System Restore point");
        offer_to_enable_system_restore();
        return 0;
    }
    printf("Restore point created. If anything about this goes wrong in a way this tool\n");
    printf("doesn't already handle, you can roll back through Windows' own System Restore\n");
    printf("in addition to everything else below.\n\n");

    /* Graceful first step, before anything is stopped/deleted: ask HidHide
       to stop enforcing entirely while its driver is still fully running
       and able to respond properly. Cheap, safe, already-proven API (the
       live toggle already calls this while the relay runs unelevated) --
       real insurance alongside clean_hidhide_filters() below, not a
       replacement for it. */
    run_hidhide("--cloak-off");

    char vjoy_oem[64], hidhide_oem[64], vigembus_oem[64];
    find_all_oem_names(vjoy_oem, sizeof(vjoy_oem), hidhide_oem, sizeof(hidhide_oem), vigembus_oem,
                         sizeof(vigembus_oem));

    char exe_dir[MAX_PATH];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    char backup_dir[MAX_PATH];
    snprintf(backup_dir, sizeof(backup_dir), "%spre_uninstall_backup", exe_dir);
    CreateDirectoryA(backup_dir, NULL); /* ignore failure -- already existing is fine */

    /* Timestamped, not fixed, filenames: a fixed vjoy_oemname.reg/
       vjoy_device02.reg would get silently overwritten (reg.exe's own /y)
       by a second uninstall run, destroying the one and only backup of
       whatever config was in place before the FIRST uninstall -- exactly
       the kind of silent data loss "roll back if any issues are found"
       was asking to avoid. */
    SYSTEMTIME st;
    GetLocalTime(&st);
    char oemname_backup[MAX_PATH], device02_backup[MAX_PATH];
    snprintf(oemname_backup, sizeof(oemname_backup), "%s\\vjoy_oemname_%04u%02u%02u_%02u%02u%02u.reg", backup_dir,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    snprintf(device02_backup, sizeof(device02_backup), "%s\\vjoy_device02_%04u%02u%02u_%02u%02u%02u.reg", backup_dir,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char log_path[MAX_PATH + 32];
    get_install_log_path(log_path, sizeof(log_path));
    FILE *fresh = fopen(log_path, "w"); /* fresh log for this run, once, up front -- not per-step */
    if (fresh) fclose(fresh);

    char script[8192]; /* generous headroom -- long install paths (exe_dir/log_path/backup_dir, each up to
                           MAX_PATH) get repeated across many lines, easy to exceed a tighter buffer */
    /* Built as a sequence of small, independently-obviously-correct
       len += snprintf(...) calls, one logical step per call, instead of
       one giant snprintf with a long shared positional-argument list --
       that shape is exactly what produced a real bug while adding the
       --clean-hidhide-filters line below (a %s/argument count mismatch,
       caught before it ever ran, but this shape makes that whole class of
       mistake much harder to make again, and much easier to see is
       correct at a glance for any line added later). */
    int len = 0;
    len += snprintf(script + len, sizeof(script) - len,
        "reg.exe export \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\"
        "OEM\\VID_1234&PID_BEAD\" \"%s\" /y >> \"%s\" 2>&1\r\n", oemname_backup, log_path);
    len += snprintf(script + len, sizeof(script) - len,
        "reg.exe export \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\vjoy\\Parameters\\Device02\" "
        "\"%s\" /y >> \"%s\" 2>&1\r\n", device02_backup, log_path);
    len += snprintf(script + len, sizeof(script) - len, "sc stop vjoy >> \"%s\" 2>&1\r\n", log_path);
    len += snprintf(script + len, sizeof(script) - len, "sc delete vjoy >> \"%s\" 2>&1\r\n", log_path);
    len += snprintf(script + len, sizeof(script) - len, "sc stop HidHide >> \"%s\" 2>&1\r\n", log_path);
    len += snprintf(script + len, sizeof(script) - len, "sc delete HidHide >> \"%s\" 2>&1\r\n", log_path);
    /* THE fix for the real keyboard/mouse-killing incident 2026-09-07 --
       see clean_hidhide_filters()'s own comment. Must run regardless of
       whether the two sc commands above fully succeeded (HidHide's own
       "sc stop" failing with 1052 is a known, expected anti-tamper trait,
       not a reason to skip this). */
    len += snprintf(script + len, sizeof(script) - len,
        "\"%s\" --clean-hidhide-filters >> \"%s\" 2>&1\r\n", exe_path, log_path);
    len += snprintf(script + len, sizeof(script) - len, "sc stop ViGEmBus >> \"%s\" 2>&1\r\n", log_path);
    len += snprintf(script + len, sizeof(script) - len, "sc delete ViGEmBus >> \"%s\" 2>&1\r\n", log_path);

    if (vjoy_oem[0])
        len += snprintf(script + len, sizeof(script) - len,
                          "pnputil /delete-driver %s /uninstall /force >> \"%s\" 2>&1\r\n", vjoy_oem, log_path);
    if (hidhide_oem[0])
        len += snprintf(script + len, sizeof(script) - len,
                          "pnputil /delete-driver %s /uninstall /force >> \"%s\" 2>&1\r\n", hidhide_oem, log_path);
    if (vigembus_oem[0])
        len += snprintf(script + len, sizeof(script) - len,
                          "pnputil /delete-driver %s /uninstall /force >> \"%s\" 2>&1\r\n", vigembus_oem, log_path);

    snprintf(script + len, sizeof(script) - len,
              "reg.exe delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\"
              "Joystick\\OEM\\VID_1234&PID_BEAD\" /f >> \"%s\" 2>&1\r\n",
              log_path);

    printf("\nApprove the UAC prompt to continue...\n");
    log_line("=== Uninstall run ===");
    int elevated_ok = run_elevated_script(script);
    end_system_restore_point(restore_seq); /* close the bracket either way -- an unclosed BEGIN is an
                                                incomplete restore point, not a usable one */
    if (!elevated_ok) {
        printf("UAC prompt declined (or the script couldn't be launched) -- nothing was changed.\n");
        return 0;
    }

    /* Report what actually landed on disk instead of assuming reg.exe
       succeeded just because the elevated script ran -- a denied/failed
       export leaves no file behind, and that's exactly the case where a
       friend most needs to be told "there is no safety net here", not a
       cheerful "backed up" message that isn't true. */
    int oemname_ok = GetFileAttributesA(oemname_backup) != INVALID_FILE_ATTRIBUTES;
    int device02_ok = GetFileAttributesA(device02_backup) != INVALID_FILE_ATTRIBUTES;
    if (oemname_ok && device02_ok) {
        printf("\nBacked up vJoy's current name/config to %s\\ (double-click a .reg\n", backup_dir);
        printf("file there any time to restore it).\n\n");
    } else {
        printf("\nWarning: couldn't confirm vJoy's config backup was written to %s\\\n", backup_dir);
        printf("(vjoy_oemname %s, vjoy_device02 %s). Removal continued anyway -- see\n",
                oemname_ok ? "ok" : "MISSING", device02_ok ? "ok" : "MISSING");
        printf("%s for details.\n\n", INSTALL_LOG_FILE);
    }

    struct { const char *display; const char *service; } results[] = {
        {"vJoy", "vjoy"}, {"HIDHide", "HidHide"}, {"ViGEmBus", "ViGEmBus"},
    };
    int any_still_present = 0;
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        int still_present = service_exists(results[i].service) == 1;
        if (still_present) any_still_present = 1;
        printf("  %-8s %s\n", results[i].display,
               still_present ? "still shows a service (may need a reboot to fully clear -- this is normal for "
                                "some drivers, not an error)"
                             : "removed.");
        log_line("uninstall: %s -- service %s after uninstall attempt", results[i].display,
                  still_present ? "STILL PRESENT" : "gone");
    }

    if (any_still_present) {
        char marker_path[MAX_PATH + 32];
        char exe_dir[MAX_PATH];
        get_exe_dir(exe_dir, sizeof(exe_dir));
        snprintf(marker_path, sizeof(marker_path), "%s%s", exe_dir, REBOOT_PENDING_FILE);
        FILE *marker = fopen(marker_path, "w");
        if (marker) {
            fprintf(marker, "A reboot is needed to finish removing vJoy/HIDHide/ViGEmBus.\n");
            fclose(marker);
        }
    }

    /* PROVE the actual incident this session fixed can't happen again,
       instead of just hoping the elevated --clean-hidhide-filters step
       above worked. This is the exact check that would have caught the
       real problem before ever suggesting a reboot: an orphaned HidHide
       entry in UpperFilters/LowerFilters on the wrong device class can
       take out keyboard/mouse input system-wide, including in Safe Mode
       and WinRE, and a reboot is precisely the moment that damage would
       have surfaced. This scan is read-only and needs no elevation, so
       it's a genuine independent check, not just re-trusting the same
       elevated step that already ran. */
    int leftover_filters = count_hidhide_filter_refs();
    int hid_filters_clean = leftover_filters == 0;
    if (leftover_filters < 0) {
        printf("\nWarning: couldn't verify the HidHide filter-driver registry cleanup ran\n");
        printf("(the scan itself failed). Do NOT reboot yet -- run RuthlessControllerRelay.exe\n");
        printf("--check-hid-filters to check manually, or ask before proceeding.\n");
        log_line("uninstall: could not verify HidHide filter cleanup (scan failed)");
    } else if (!hid_filters_clean) {
        printf("\nWarning: %d leftover HidHide filter-driver registry reference(s) still\n", leftover_filters);
        printf("found after cleanup. Do NOT reboot yet -- this is exactly the condition\n");
        printf("that can break keyboard/mouse input system-wide on restart. Run\n");
        printf("RuthlessControllerRelay.exe --clean-hidhide-filters (elevated) again, or ask for help\n");
        printf("before rebooting.\n");
        log_line("uninstall: VERIFICATION FAILED -- %d HidHide filter reference(s) still present", leftover_filters);
    } else {
        printf("\nVerified: no leftover HidHide filter-driver registry references --\n");
        printf("safe to reboot.\n");
        log_line("uninstall: verified clean, 0 HidHide filter references remain");
    }

    printf("\nDone. Full details logged to %s.\n", INSTALL_LOG_FILE);
    printf("Changed your mind? Just run RuthlessControllerRelay.exe normally -- it puts\n");
    printf("everything back automatically.\n");

    if (any_still_present && hid_filters_clean) {
        offer_reboot_now("Removing vJoy/HIDHide/ViGEmBus needs a reboot to fully finish -- some drivers "
                           "still show as present until Windows restarts. The HidHide filter-driver registry "
                           "cleanup that a past version of this tool was missing has been verified clean, "
                           "so this reboot is safe.");
    }
    return 1;
}

/* Same lesson as begin_system_restore_point() (see that function's own
   comment for the first time this exact mistake was made and fixed
   today): destroy_vjoy_root_devices()/create_root_device()/configure_
   vjoy_device() are all real, synchronous, potentially slow SetupAPI/
   service-control calls with no timeout of their own -- confirmed on
   real hardware the same day this session-scoping feature shipped that
   this sequence can simply hang with zero feedback (likely a wedged
   device/service state left over from an earlier force-killed session --
   several were force-killed during testing this same day). Running the
   whole three-step sequence on one worker thread with one bounded wait
   is what makes this fail LOUD instead of silently, matching the exact
   fix already applied to the restore-point code above. */
typedef struct {
    const char *hardware_id;
    const char *inf_path;
    UINT vjoy_rid;
    int stale_removed;
    int create_ok;
    int configure_ok;
} VjoySessionSetupArgs;

static DWORD WINAPI vjoy_session_setup_thread(LPVOID arg) {
    VjoySessionSetupArgs *a = (VjoySessionSetupArgs *)arg;
    a->stale_removed = destroy_vjoy_root_devices();
    a->create_ok = create_root_device(a->hardware_id, a->inf_path);
    if (a->create_ok) a->configure_ok = configure_vjoy_device(a->vjoy_rid);
    return 0;
}

#define VJOY_SESSION_SETUP_TIMEOUT_MS 90000

int main(int argc, char **argv) {
    /* First thing, unconditionally: cheap, side-effect-free, and several
       functions callable from many places below (vigem_teardown(),
       relinquish_vjoy_if_acquired(), fatal_exit()) use this lock
       unconditionally -- initializing it here instead of further down
       main() removes any chance of an early-return CLI path reaching one
       of them before it exists. See g_output_lock's own comment. */
    InitializeCriticalSection(&g_output_lock);

    /* Standalone escape hatch, independent of anything else below: if the
       controller was ever left hidden (this tool crashed, got killed from
       Task Manager, or the PC lost power before it could clean up), this
       unhides it and exits immediately -- no vJoy/XInput/controller needed
       at all, so it always works as a last resort. */
    if (argc > 1 && !strcmp(argv[1], "--unhide")) {
        printf("Setting Normal mode (controller visible to every app/game again)...\n");
        int ok = run_hidhide("--cloak-off") == 0;
        printf(ok ? "Done.\n" : "Something went wrong running HidHideCLI -- is HIDHide installed?\n");
        return ok ? 0 : 1;
    }

    /* Fully removes vJoy/HIDHide/ViGEmBus -- see run_uninstall()'s own
       comment for why this needs to be a real, complete removal and not
       just "stop using it". Deliberately a command-line flag, not a
       dashboard keypress -- this is destructive and rare enough that it
       should never fire from an accidental key press mid-session. */
    if (argc > 1 && !strcmp(argv[1], "--uninstall")) {
        return run_uninstall() ? 0 : 1;
    }

    /* Self-invoked elevated (via run_elevated_script() below, never by a
       user directly) to actually run create_root_device() -- that function's
       SetupAPI calls need to happen in the elevated process itself, not
       be launched as a separate elevated child of it, so check_and_
       install_drivers() re-runs this same exe with these two arguments
       rather than calling create_root_device() directly in-process. */
    if (argc > 3 && !strcmp(argv[1], "--create-root-device")) {
        return create_root_device(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc > 2 && !strcmp(argv[1], "--configure-vjoy")) {
        return configure_vjoy_device((UINT)atoi(argv[2])) ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--clean-hidhide-filters")) {
        int cleaned = clean_hidhide_filters();
        return cleaned >= 0 ? 0 : 1;
    }
    /* Standalone, non-destructive, safe to run any time (even outside an
       uninstall) -- lets anyone confirm right now whether their machine
       currently has any orphaned HidHide filter reference sitting around,
       without needing to run a full uninstall to find out. Prints exactly
       what a friend or this user could otherwise only see by manually
       digging through regedit. */
    if (argc > 1 && !strcmp(argv[1], "--check-hid-filters")) {
        int n = count_hidhide_filter_refs();
        if (n < 0) { printf("Couldn't scan the registry.\n"); return 1; }
        if (n == 0) { printf("Clean -- no leftover HidHide filter references found.\n"); return 0; }
        printf("Found %d leftover HidHide filter reference(s) -- run RuthlessControllerRelay.exe --uninstall (or "
                "--clean-hidhide-filters, elevated) to remove them.\n", n);
        return 1;
    }
    /* Diagnostic-only, standalone -- see capture_dualsense_raw()'s own
       comment. Not part of the normal relay flow at all. */
    if (argc > 1 && !strcmp(argv[1], "--capture-dualsense-raw")) {
        return capture_dualsense_raw() ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--capture-gamesir-raw")) {
        return capture_gamesir_raw() ? 0 : 1;
    }
    /* Diagnostic-only, standalone -- see test_dualsense_keepalive()'s own
       comment. Tests the battery-idle-timeout keep-alive question, not
       part of the normal relay flow. */
    if (argc > 1 && !strcmp(argv[1], "--test-dualsense-keepalive")) {
        return test_dualsense_keepalive() ? 0 : 1;
    }
    /* Manual escape hatch for exactly the scenario that prompted it: a
       previous session's vJoy device left behind because it was force-
       killed (TerminateProcess skips console_ctrl_handler entirely) or
       because a diagnostic-only mode like --capture-dualsense-raw never
       goes through the normal create/destroy sequence at all. The normal
       self-heal-before-create still runs on the next real launch
       regardless -- this just lets it be cleaned up on demand instead of
       waiting for that. */
    if (argc > 1 && !strcmp(argv[1], "--cleanup-vjoy-device")) {
        int n = destroy_vjoy_root_devices();
        if (n > 0) printf("Removed %d leftover vJoy device instance(s).\n", n);
        else printf("Nothing to clean up.\n");
        return 0;
    }
    /* Deliberately undocumented, dev-only: exists purely to verify the
       crash handler above actually works end-to-end (writes a real crash
       entry to the log, then the process actually terminates) instead of
       just trusting that it compiles. Not something a friend would ever
       need or find by accident. */
    if (argc > 1 && !strcmp(argv[1], "--test-crash")) {
        install_crash_handler();
        printf("Deliberately crashing now to test the crash handler...\n");
        fflush(stdout);
        Sleep(200); /* let the printf actually reach the console before the crash */
        volatile int *p = NULL;
        *p = 1; /* real access violation, not a controlled exit() */
        return 0; /* unreachable */
    }
    /* Deliberately undocumented, dev-only: real, runnable regression
       coverage for the hotkey=/button=none feature (2026-09-08) --
       exercises parse_hotkey_line()/parse_button_line() directly against
       edge cases (case, whitespace, garbage, the "none" sentinel) and
       load_config()'s "both disabled -> restore default hotkey" fallback
       against a real file on disk, instead of relying on code review
       alone. Saves/restores whatever real ruthless_controller_relay.ini already sits
       next to the exe (or removes the test one if none existed) so this
       never leaves the actual config touched. No admin/driver/vJoy
       action happens on this path at all. */
    if (argc > 1 && !strcmp(argv[1], "--test-config")) {
        int failed = 0;
        UINT mods, vk;
        DWORD mask;
        DWORD hold_ms;

#define CHECK(cond, desc) do { \
    if (cond) printf("  PASS: %s\n", desc); \
    else { printf("  FAIL: %s\n", desc); failed++; } \
} while (0)

        printf("== parse_hotkey_line ==\n");
        CHECK(parse_hotkey_line("ctrl+alt+h", &mods, &vk) == 1 && vk == 'H' && (mods & MOD_CONTROL) && (mods & MOD_ALT),
              "valid combo parses");
        CHECK(parse_hotkey_line("none", &mods, &vk) == 2 && vk == 0 && mods == 0, "\"none\" disables");
        CHECK(parse_hotkey_line("  NoNe  ", &mods, &vk) == 2 && vk == 0, "\"none\" is case/whitespace-insensitive");
        CHECK(parse_hotkey_line("ctrl+alt", &mods, &vk) == 0, "no real key -> unparseable");
        CHECK(parse_hotkey_line("gibberish", &mods, &vk) == 0, "garbage -> unparseable");
        CHECK(parse_hotkey_line("CTRL+SHIFT+F5", &mods, &vk) == 1 && (mods & MOD_SHIFT), "uppercase combo parses");

        printf("== parse_button_line ==\n");
        CHECK(parse_button_line("back+start", &mask, &hold_ms) == 1 &&
              mask == (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START) && hold_ms == 0, "valid combo parses");
        CHECK(parse_button_line("back+start:1000", &mask, &hold_ms) == 1 && hold_ms == 1000, "hold duration parses");
        CHECK(parse_button_line("none", &mask, &hold_ms) == 2 && mask == 0, "\"none\" disables");
        CHECK(parse_button_line(" None ", &mask, &hold_ms) == 2 && mask == 0, "\"none\" is case/whitespace-insensitive");
        CHECK(parse_button_line("not_a_button", &mask, &hold_ms) == 0, "garbage -> unparseable");
        /* New with the PS-button-as-toggle feature: the toggle combo now
           shares NAMED_PHYS with the button-remap system instead of its
           own separate, narrower table, so "ps"/"touchpad" (and lt/rt)
           need to parse here too, not just in the buttonmap= path. */
        CHECK(parse_button_line("ps", &mask, &hold_ms) == 1 && mask == PHYS_PS_BUTTON,
              "\"ps\" parses to PHYS_PS_BUTTON");
        CHECK(parse_button_line("touchpad", &mask, &hold_ms) == 1 && mask == PHYS_TOUCHPAD,
              "\"touchpad\" parses to PHYS_TOUCHPAD");
        CHECK(parse_button_line("ps+touchpad", &mask, &hold_ms) == 1 &&
              mask == (PHYS_PS_BUTTON | PHYS_TOUCHPAD), "\"ps+touchpad\" combo parses");
        CHECK(parse_button_line("guide", &mask, &hold_ms) == 1 && mask == PHYS_GUIDE_BUTTON,
              "\"guide\" parses to PHYS_GUIDE_BUTTON");

        printf("== format_hotkey / format_button_combo ==\n");
        char buf[48];
        format_hotkey(buf, sizeof(buf), 0, 0);
        CHECK(!strcmp(buf, "None"), "format_hotkey shows None for vk=0");
        format_button_combo(buf, sizeof(buf), PHYS_PS_BUTTON, 0);
        CHECK(!strcmp(buf, "Ps"), "format_button_combo shows Ps for PHYS_PS_BUTTON");
        format_button_combo(buf, sizeof(buf), PHYS_GUIDE_BUTTON, 0);
        CHECK(!strcmp(buf, "Guide"), "format_button_combo shows Guide for PHYS_GUIDE_BUTTON");
        format_button_combo(buf, sizeof(buf), 0, 0);
        CHECK(!strcmp(buf, "None"), "format_button_combo shows None for mask=0");

        printf("== load_config \"both disabled\" fallback (real file I/O) ==\n");
        char cfgpath[MAX_PATH + 32];
        get_config_path(cfgpath, sizeof(cfgpath));
        char *saved = NULL;
        long savedLen = 0;
        FILE *orig = fopen(cfgpath, "rb");
        if (orig) {
            fseek(orig, 0, SEEK_END);
            savedLen = ftell(orig);
            fseek(orig, 0, SEEK_SET);
            saved = malloc((size_t)savedLen);
            if (saved) fread(saved, 1, (size_t)savedLen, orig);
            fclose(orig);
        }

        FILE *test = fopen(cfgpath, "w");
        if (test) {
            fprintf(test, "hotkey=none\nbutton=none\n");
            fclose(test);
        }
        char gamepath[MAX_PATH] = {0};
        load_config(&mods, &vk, &mask, &hold_ms, gamepath, sizeof(gamepath));
        CHECK(vk == 'H' && mods == (MOD_CONTROL | MOD_ALT) && mask == 0,
              "both none -> hotkey restored to Ctrl+Alt+H, button combo stays disabled");

        test = fopen(cfgpath, "w");
        if (test) {
            fprintf(test, "hotkey=none\nbutton=a+b\n");
            fclose(test);
        }
        load_config(&mods, &vk, &mask, &hold_ms, gamepath, sizeof(gamepath));
        CHECK(vk == 0 && mask == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B),
              "hotkey=none with a real button combo -> only the hotkey stays disabled");

        test = fopen(cfgpath, "w");
        if (test) {
            fprintf(test, "hotkey=ctrl+shift+f5\nbutton=none\n");
            fclose(test);
        }
        load_config(&mods, &vk, &mask, &hold_ms, gamepath, sizeof(gamepath));
        CHECK(vk == VK_F5 && mask == 0, "button=none with a real hotkey -> only the button combo stays disabled");

        if (saved) {
            FILE *restore = fopen(cfgpath, "wb");
            if (restore) {
                fwrite(saved, 1, (size_t)savedLen, restore);
                fclose(restore);
            }
            free(saved);
        } else {
            remove(cfgpath); /* no real config existed before this test -- don't leave one behind */
        }

#undef CHECK
        printf("\n%s (%d check%s failed)\n", failed ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", failed,
                failed == 1 ? "" : "s");
        return failed ? 1 : 0;
    }

    /* Any argument that reaches here without matching one of the --flag
       checks above falls through to being parsed as a vJoy device ID --
       confirmed via code audit 2026-09-07 that a typo'd flag (e.g.
       --uninstal) or an unrecognized one silently becomes atoi("--uninstal")
       == 0, which then fails AcquireVJD in a confusing retry loop, exactly
       the kind of unexplained failure check_pending_reboot() exists to
       prevent elsewhere. Reject anything that looks like a flag instead of
       guessing what the user meant. */
    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "Unrecognized option: %s\n\n", argv[1]);
        fprintf(stderr, "Usage: RuthlessControllerRelay.exe [vjoy_device_id]\n"
                         "       RuthlessControllerRelay.exe --uninstall\n"
                         "       RuthlessControllerRelay.exe --unhide\n"
                         "       RuthlessControllerRelay.exe --check-hid-filters\n"
                         "       RuthlessControllerRelay.exe --cleanup-vjoy-device\n");
        return 1;
    }

    /* As early as possible in the real relay startup -- before anything
       else that could plausibly crash gets a chance to. Diagnostic-only
       modes (--capture-*, --test-*, etc.) already returned above and
       don't need this; this is specifically for the actual gaming
       session, where a friend hitting a genuine crash is the whole
       reason this exists. */
    install_crash_handler();

    UINT rid = 2;
    if (argc > 1) rid = (UINT)atoi(argv[1]);
    /* vJoy only ever supports device IDs 1-16. Without this check, a typo
       (e.g. running "RuthlessControllerRelay.exe 0") or a non-numeric argument
       (atoi() silently returns 0 for that) becomes an illegal device ID
       that can never be acquired -- the relay would otherwise loop
       forever printing "Could not acquire vJoy device #0... Retrying"
       with no hint that 0 itself is the actual problem. Caught in the
       2026-09-08 pre-ship audit. */
    if (rid < 1 || rid > 16) {
        fprintf(stderr, "Invalid vJoy device ID: %u (must be 1-16). Run with no arguments to use the default "
                          "(device #2).\n", rid);
        return 1;
    }
    g_vjoy_rid = rid;

    /* Modest priority bump so the scheduler doesn't delay the poll loop
       behind normal-priority background work. Not TIME_CRITICAL/realtime --
       this thread never blocks once running (see cpu_relax() below), so
       going any higher risks starving the game itself for no real gain. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    check_pending_reboot();
    check_and_install_drivers();

    /* vJoy's device is session-scoped now, not persistent -- created
       fresh here on every launch, fully removed on every exit (clean or
       fatal, see fatal_exit()/console_ctrl_handler()). See destroy_vjoy_
       root_devices()'s comment for the full reasoning and the real
       incident that prompted this. This whole process already runs
       elevated (hotas_relay.manifest requires administrator), so this
       runs directly in-process now -- no self-invoke-elevated-child dance
       needed for this specific step, unlike check_and_install_drivers()'s
       one-time driver/service setup above, which still uses that pattern
       for the (rare, first-run-only) case of actually installing a
       missing driver package.
       Self-heals FIRST: clears out any leftover device instance from a
       previous session that didn't exit cleanly (crash, force-kill, power
       loss) before creating a new one -- the actual guarantee that nothing
       can silently accumulate across sessions, since a single exit-time
       removal alone can never be 100% relied on. */
    printf("Setting up vJoy for this session...\n");
    fflush(stdout); /* real progress feedback -- without this, a slow-but-not-hung pass here looks
                        identical to a genuine hang, exactly what caused real confusion on 2026-09-07 */

    char vjoy_exe_dir[MAX_PATH];
    get_exe_dir(vjoy_exe_dir, sizeof(vjoy_exe_dir));
    char vjoy_inf_path[MAX_PATH * 2];
    snprintf(vjoy_inf_path, sizeof(vjoy_inf_path), "%sdrivers\\vjoy\\vjoy.inf", vjoy_exe_dir);

    VjoySessionSetupArgs setupArgs = {0};
    setupArgs.hardware_id = "root\\VID_1234&PID_BEAD&REV_0219";
    setupArgs.inf_path = vjoy_inf_path;
    setupArgs.vjoy_rid = rid;
    HANDLE setupThread = CreateThread(NULL, 0, vjoy_session_setup_thread, &setupArgs, 0, NULL);
    if (!setupThread) fatal_exit("Could not start vJoy session setup (CreateThread failed).");
    DWORD setupWait = WaitForSingleObject(setupThread, VJOY_SESSION_SETUP_TIMEOUT_MS);
    CloseHandle(setupThread);

    if (setupWait != WAIT_OBJECT_0) {
        log_line("startup: vJoy session setup (destroy/create/configure) did not complete within %d "
                  "seconds -- abandoned, not killed", VJOY_SESSION_SETUP_TIMEOUT_MS / 1000);
        fatal_exit("Setting up vJoy for this session is taking far too long and appears stuck.\n\n"
                    "This can happen if an earlier session was force-closed while it was mid-operation, "
                    "leaving a device or service in a wedged state. Try rebooting Windows, then run this "
                    "again.");
    }
    if (setupArgs.stale_removed > 0)
        log_line("startup: cleaned up %d leftover vJoy device instance(s) from a previous session",
                  setupArgs.stale_removed);
    if (!setupArgs.create_ok)
        fatal_exit("Could not create the vJoy virtual device for this session. Check driver_install_log.txt.");
    if (!setupArgs.configure_ok)
        fatal_exit("Could not configure the vJoy virtual device for this session. Check driver_install_log.txt.");

    if (!load_vjoy()) fatal_exit("Could not load vJoyInterface.dll -- is vJoy actually installed?");
    if (!load_xinput()) fatal_exit("Could not load xinput1_4.dll (should ship with Windows 8+).");
    if (!load_dinput()) {
        fprintf(stderr, "Note: couldn't load DirectInput -- PlayStation/other non-Xbox controllers won't be found. "
                         "Xbox-style controllers (XInput) still work normally.\n");
    }
    if (!load_vigem()) {
        fprintf(stderr, "Note: couldn't load ViGEmClient.dll -- the virtual gamepad-emulation output isn't "
                         "available this run. vJoy/HOTAS relaying still works normally.\n");
    }

    /* g_hidden_mode starts at -1 (unknown/unset) and stays that way until
       either a controller connects (see the mutual-exclusivity comment in
       the main loop -- mode -1 defaults both outputs to live) or the user
       presses the toggle -- it is NOT initialized from HidHide's current
       cloak state anymore, since cloak state and mode are no longer the
       same thing (see do_toggle()'s comment). */

    load_config(&g_hotkey_mods, &g_hotkey_vk, &g_toggle_button_mask, &g_toggle_hold_ms, g_game_path,
                sizeof(g_game_path));
    char hotkey_str[32], button_str[48];
    format_hotkey(hotkey_str, sizeof(hotkey_str), g_hotkey_mods, g_hotkey_vk);
    format_button_combo(button_str, sizeof(button_str), g_toggle_button_mask, g_toggle_hold_ms);
    printf("Toggle mode with: %s, or controller %s. Press R any time to change either live.\n", hotkey_str,
           button_str);
    printf("Run this program with --unhide any time to force the controller visible again (e.g. after a "
            "crash left it hidden).\n\n");

    CreateThread(NULL, 0, toggle_thread, NULL, 0, &g_toggle_thread_id);

    if (!pvJoyEnabled()) {
        fatal_exit("vJoy driver is not enabled/running. Check the vJoy Configuration app.");
    }

    /* Outer lifecycle loop: wait for a controller (first time, or again
       after one drops mid-session) -> acquire vJoy live, right when it's
       actually needed -> relay until it disconnects -> relinquish and go
       back to waiting. Nothing here requires the controller to already be
       on before this program starts, and nothing requires restarting the
       program if it's turned off and back on later. */
    backend_t backend = BACKEND_XINPUT;
    int was_ever_connected = 0;
    for (;;) {
        int userIndex = -1;
        int waiting_screen_shown = 0; /* render once per wait, not every 250ms poll -- a same-content
                                          full-screen clear+redraw 4x/sec reads as flashing/flicker,
                                          confirmed visibly worse over Windows Sandbox's display */
        while (userIndex < 0 && backend == BACKEND_XINPUT) {
            for (DWORD i = 0; i < 4 && userIndex < 0; i++) {
                XINPUT_STATE st;
                if (pXInputGetState(i, &st) == ERROR_SUCCESS) userIndex = (int)i;
            }
            if (userIndex < 0) {
                if (setup_ds4_raw_hid()) {
                    backend = BACKEND_DS4_RAWHID;
                    break;
                }
                if (setup_dualsense_raw_hid()) {
                    backend = BACKEND_DUALSENSE_RAWHID;
                    break;
                }
                {
                    int wiredPs = setup_ps_wired_raw_hid();
                    if (wiredPs == 1) {
                        backend = BACKEND_DS4_WIRED_RAWHID;
                        break;
                    }
                    if (wiredPs == 2) {
                        backend = BACKEND_DUALSENSE_WIRED_RAWHID;
                        break;
                    }
                }
                if (setup_dinput_device()) {
                    WORD vid = (WORD)(g_dinput_vid_pid & 0xFFFFu);
                    WORD pid = (WORD)((g_dinput_vid_pid >> 16) & 0xFFFFu);
                    if (vid == 0x054C && is_ds4_product_id(pid) && is_ds4_present_over_bt()) {
                        /* A real DS4, over BLUETOOTH specifically, ended up
                           here instead of via setup_ds4_raw_hid() above --
                           a confirmed, real, already-established
                           limitation: DirectInput cannot correctly parse
                           this controller's large Bluetooth HID report
                           (frozen, all-zero input forever -- see
                           setup_ds4_raw_hid()'s own comment). Reject this
                           connection outright rather than ever locking
                           onto a backend already known to be broken for
                           this specific controller/transport combination
                           -- setup_ds4_raw_hid() above gets a fresh, full
                           attempt on the very next pass through this loop
                           instead. The is_ds4_present_over_bt() check is
                           what keeps this from also rejecting a WIRED DS4
                           -- that's DirectInput's correct, confirmed-
                           working path (a wired DS4's compact <=64-byte
                           report reads fine there), not the broken one. */
                        if (g_di_device) {
                            IDirectInputDevice8_Unacquire(g_di_device);
                            IDirectInputDevice8_Release(g_di_device);
                            g_di_device = NULL;
                        }
                    } else {
                        backend = BACKEND_DINPUT;
                        break;
                    }
                }
                if (!waiting_screen_shown) {
                    render_waiting_screen(rid, was_ever_connected);
                    waiting_screen_shown = 1;
                }
                Sleep(250);
            }
        }

        if (!pAcquireVJD(rid)) {
            fprintf(stderr, "Could not acquire vJoy device #%u. Already owned, or doesn't exist? Retrying...\n", rid);
            Sleep(1000);
            backend = BACKEND_XINPUT; /* re-try XInput first on the next pass */
            continue;
        }
        g_vjoy_acquired = 1; /* so fatal_exit()/console_ctrl_handler() know to relinquish before tearing the
                                 device down, if this process ends while still connected */
        was_ever_connected = 1;
        g_backend = backend; /* so label_for_phys()/render_dashboard() show the right names */

        /* Smart first-run default, only ever decided once (see
           g_toggle_button_is_fresh_default's own comment): a genuinely
           fresh install's first-ever connected controller decides the
           default toggle -- the PS button for a PlayStation controller,
           the Guide button for an Xbox one (confirmed working on real
           hardware, incl. via the official Wireless Adapter dongle,
           2026-09-09) -- instead of Back+Start. Back+Start is only ever
           the actual default now for a HOTAS-only device with neither. */
        if (g_toggle_button_is_fresh_default) {
            g_toggle_button_is_fresh_default = 0; /* decide this at most once, ever */
            int is_ps_controller =
                backend == BACKEND_DS4_RAWHID || backend == BACKEND_DUALSENSE_RAWHID ||
                backend == BACKEND_DS4_WIRED_RAWHID || backend == BACKEND_DUALSENSE_WIRED_RAWHID;
            if (!is_ps_controller && backend == BACKEND_DINPUT) {
                is_ps_controller = (WORD)(g_dinput_vid_pid & 0xFFFFu) == 0x054C;
            }
            if (is_ps_controller) {
                g_toggle_button_mask = PHYS_PS_BUTTON;
                save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
            } else if (backend == BACKEND_XINPUT && pXInputGetStateEx) {
                g_toggle_button_mask = PHYS_GUIDE_BUTTON;
                save_config(g_hotkey_mods, g_hotkey_vk, g_toggle_button_mask, g_toggle_hold_ms, g_game_path);
            }
        }

        /* Guide-button capability itself (as opposed to which combo is
           CONFIGURED as the toggle above) is enabled any time an Xbox
           controller connects and the undocumented API is available --
           same unconditional treatment as g_ps_pressed for PlayStation
           controllers, now that this is confirmed working on real
           hardware rather than a hedge against an unverified idea. Real,
           visible side effect worth knowing (documented in the ini and
           README): this flips a Windows setting (Game Bar's guide-button
           capture) for the session, restored to exactly what it was on
           every exit path -- see disable_gamebar_guide_capture(). */
        if (backend == BACKEND_XINPUT && pXInputGetStateEx) {
            disable_gamebar_guide_capture();
        }

        /* REAL BUG found and fixed 2026-09-08, on real hardware: registering
           this AFTER vigem_setup() (or retrying it later from the live
           loop, which was the first attempt at this fix) can find and hide
           ViGEmBus's own freshly-created virtual pad instead of the real
           controller -- confirmed via log, an Xbox 360-type virtual target
           and a real Xbox-compatible controller both show as VID_045E
           PID_028E-shaped, present, "gamingDevice" entries to HidHide, and
           find_real_controller_hidhide_path() has no way to tell them
           apart once both exist. Retried HERE, blocking, BEFORE
           vigem_setup() creates that virtual target at all -- so there is
           nothing else present for this to confuse the real controller
           with.
           A SEPARATE real bug (the actual root cause of what first looked
           like a slow-enumeration timing issue) turned out to live inside
           find_real_controller_hidhide_path() itself -- see its own
           comment. Once that parsing bug was fixed, a fresh first connect
           succeeds almost immediately. A quick controller off/on cycle
           (not a fresh first connect) is a genuinely different case,
           though, confirmed on real hardware to still fail all 8 tries at
           200ms apart (1.6s total) -- Windows' device tree apparently
           needs longer to re-settle after a very recent removal than it
           does for an already-stable fresh arrival. Widened to 15
           attempts at 300ms (4.5s total) to cover that case too. Kept
           bounded (not infinite) so a controller HidHide genuinely never
           lists still lets the relay proceed rather than hang;
           g_hidhide_registered stays 0 in that case and the real
           controller simply isn't hidden this session (visible but
           working, the same tradeoff every earlier "skip registration"
           path already made) rather than risking hiding the wrong thing
           -- and register_with_hidhide() itself now unhides any stale
           leftovers even on this failure path, so a timeout here can't
           leave some unrelated device wrongly hidden either (see its own
           comment). */
        for (int attempt = 0; attempt < 15 && !g_hidhide_registered; attempt++) {
            g_hidhide_registered = register_with_hidhide();
            if (!g_hidhide_registered && attempt < 14) Sleep(300);
        }
        /* Default the emulated output to match the real input: a
           PlayStation-style controller detected -> emulate DS4 (so the
           game shows the correct Cross/Circle/Square/Triangle prompts for
           what's actually in hand); an Xbox-style one -> emulate X360.
           Overridable live -- press V while the dashboard is showing. */
        g_vigem_emulate_ds4 = (backend == BACKEND_DINPUT || backend == BACKEND_DS4_RAWHID ||
                                 backend == BACKEND_DUALSENSE_RAWHID || backend == BACKEND_DS4_WIRED_RAWHID ||
                                 backend == BACKEND_DUALSENSE_WIRED_RAWHID);
        vigem_setup(backend, userIndex);
        /* Immediate first read/color instead of waiting up to ~1s for the
           periodic poll below -- a friend shouldn't see "unknown"/no color
           for a whole second right after connecting. */
        if (g_ps_output_handle != INVALID_HANDLE_VALUE) poll_dualsense_battery();
        else if (backend == BACKEND_XINPUT) poll_xbox_battery(userIndex);
        update_ps_mode_led();
        clear_console();

        DWORD count = 0;
        int combo_was = 0, combo_fired = 0;
        LARGE_INTEGER combo_start = {0};
        LARGE_INTEGER freq, lastPrint, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&lastPrint);
        XINPUT_GAMEPAD lastGamepad;
        memset(&lastGamepad, 0xFF, sizeof(lastGamepad)); /* guarantee the first read counts as "changed" */
        int lastPsSent = -1, lastTouchpadSent = -1; /* PS/touchpad live outside `gp`, so the change-check below needs them too */
        LONG lastModeSent = -2; /* forces the first tick to count as "changed" regardless of g_hidden_mode's actual
                                    starting value (-1/0/1); also outside `gp`, same reasoning as PS/touchpad above --
                                    without this, toggling mode while the stick/a button is held doesn't mute/unmute
                                    the output until the NEXT input change, since the memcmp below never saw a diff */
        int batteryPollTicks = 0; /* battery doesn't change fast enough to need checking every 15Hz dashboard
                                       tick -- counts ticks so the actual poll only fires once every ~1s (see
                                       below), keeping this cheap regardless of how often the dashboard redraws */

        int connected = 1;
        while (connected) {
            XINPUT_GAMEPAD gp;
            g_guide_pressed = 0; /* only the BACKEND_XINPUT branch below ever sets this to something else --
                                     reset unconditionally every iteration so a stale press from a previous
                                     Xbox connection can't linger once a different controller takes over */
            if (backend == BACKEND_XINPUT) {
                XINPUT_STATE st;
                /* Only reach for the undocumented Ex variant once the Game
                   Bar registry toggle is actually confirmed off THIS
                   session (see disable_gamebar_guide_capture()) -- calling
                   it before that wouldn't usefully set the Guide bit
                   anyway (per real-world confirmation from another
                   project's maintainer), so there's no reason to prefer
                   the undocumented call over the always-proven plain one
                   until it can actually tell us something extra. Same
                   XINPUT_STATE shape either way, so gp = st.Gamepad below
                   is identical regardless of which one filled it in. */
                int used_ex = (g_gamebar_guide_disabled && pXInputGetStateEx);
                DWORD xr = used_ex ? pXInputGetStateEx((DWORD)userIndex, &st)
                                    : pXInputGetState((DWORD)userIndex, &st);
                if (xr != ERROR_SUCCESS) {
                    connected = 0; /* dropped -- relinquish below and go back to waiting */
                    break;
                }
                gp = st.Gamepad;
                g_guide_pressed = used_ex && (st.Gamepad.wButtons & XINPUT_GAMEPAD_GUIDE) != 0;
                g_ps_pressed = 0;      /* no such button in XInput's bitmask */
                g_touchpad_pressed = 0;
            } else if (backend == BACKEND_DS4_RAWHID) {
                if (!poll_ds4_raw_hid(&gp)) {
                    connected = 0; /* dropped, or the device stopped sending report 0x11 -- go back to waiting */
                    break;
                }
            } else if (backend == BACKEND_DUALSENSE_RAWHID) {
                if (!poll_dualsense_raw_hid(&gp)) {
                    log_line("dualsense rawhid: poll failed, treating as disconnect (error %lu)",
                              (unsigned long)GetLastError()); /* temporary diagnostic 2026-09-08 --
                                                                    tracking down a real BT stability issue */
                    connected = 0; /* dropped (see poll_dualsense_raw_hid()'s known battery-idle-timeout
                                        limitation) -- go back to waiting, same as any other disconnect */
                    break;
                }
            } else if (backend == BACKEND_DS4_WIRED_RAWHID) {
                if (!poll_ds4_wired_raw_hid(&gp)) {
                    connected = 0; /* dropped -- go back to waiting, same as any other disconnect */
                    break;
                }
            } else if (backend == BACKEND_DUALSENSE_WIRED_RAWHID) {
                if (!poll_dualsense_wired_raw_hid(&gp)) {
                    connected = 0; /* dropped -- go back to waiting, same as any other disconnect */
                    break;
                }
            } else {
                DIJOYSTATE2 js;
                if (!poll_dinput(&js)) {
                    connected = 0;
                    break;
                }
                /* Logged independently of whether gp (the translated
                   struct) changes below -- if trigger data genuinely isn't
                   landing on any field dinput_to_xinput_gamepad() reads
                   (the bug being diagnosed), gp would never change from a
                   trigger pull alone, and gating this log on that same
                   condition would hide the exact data needed to find it. */
                static DIJOYSTATE2 last_logged_raw;
                static int have_logged_raw = 0;
                static ULONGLONG last_log_tick = 0;
                /* Rate-limited to at most 5/sec -- a real controller changes
                   SOME axis on nearly every poll (analog stick noise alone),
                   so gating purely on memcmp() against the latency-tuned,
                   Sleep-free main loop meant log_line()'s fopen/write/fclose
                   was firing ~100-250 times/sec, growing this file at
                   tens of KB/sec and adding real per-iteration disk I/O to
                   the one loop in this program deliberately built to avoid
                   exactly that (see cpu_relax()'s comment). Still logs every
                   genuinely distinct state, just not faster than a human
                   needs to read raw diagnostic data anyway. */
                ULONGLONG now_tick = GetTickCount64();
                if ((!have_logged_raw || memcmp(&js, &last_logged_raw, sizeof(js)) != 0) &&
                    now_tick - last_log_tick >= 200) {
                    /* Indices go past what any current mapping uses (0-19,
                       not just 0-13) on purpose -- headroom to read a
                       controller's extra buttons (GameSir Bluetooth-mode
                       paddles, etc.) straight off real hardware instead of
                       guessing, same as how the PS4's trigger axes and the
                       PS/touchpad indices below were each pinned down. */
                    char btnbuf[24];
                    for (int bi = 0; bi < 20; bi++) btnbuf[bi] = (js.rgbButtons[bi] & 0x80) ? '1' : '0';
                    btnbuf[20] = '\0';
                    log_line("dinput raw: X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld S0=%ld S1=%ld POV0=%lu btns0-19=%s",
                              js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz, js.rglSlider[0], js.rglSlider[1],
                              (unsigned long)js.rgdwPOV[0], btnbuf);
                    last_log_tick = now_tick;
                    last_logged_raw = js;
                    have_logged_raw = 1;
                }
                g_last_dinput_raw = js;
                g_have_dinput_raw = 1;
                g_ps_pressed = (js.rgbButtons[DINPUT_BTN_IDX_PS] & 0x80) != 0;
                g_touchpad_pressed = (js.rgbButtons[DINPUT_BTN_IDX_TOUCHPAD] & 0x80) != 0;
                dinput_to_xinput_gamepad(&js, &gp);
            }

            /* PS/touchpad live outside `gp` (see g_ps_pressed comment above),
               so a press that changes ONLY one of those two would otherwise
               go undetected here and never reach vJoy. g_hidden_mode is
               checked the same way, for the same reason -- see lastModeSent's
               comment above. */
            LONG modeNow = g_hidden_mode;
            int mode_changed = modeNow != lastModeSent; /* captured before lastModeSent is overwritten below --
                                                             used to send exactly one neutral frame to whichever
                                                             output just got muted, then skip re-sending identical
                                                             neutral data on every subsequent unchanged frame (see
                                                             the vjoy_muted/vigem_update guards further down) */
            if (memcmp(&gp, &lastGamepad, sizeof(gp)) != 0 || g_ps_pressed != lastPsSent ||
                g_touchpad_pressed != lastTouchpadSent || mode_changed) {
                lastGamepad = gp;
                lastPsSent = g_ps_pressed;
                lastTouchpadSent = g_touchpad_pressed;
                lastModeSent = modeNow;
                count++;

                /* Mutual exclusivity is the whole point ("goes both ways" --
                   HOTAS-only mode vs controller-only mode): vJoy mutes in
                   Normal mode (g_hidden_mode==0) the same way vigem_update()
                   below mutes the virtual gamepad in HOTAS mode
                   (g_hidden_mode==1). Mode -1 (unknown, before the user has
                   toggled at all) defaults to live here too, matching
                   vigem_update()'s own default -- both outputs live until a
                   mode is actively chosen, not neither. */
                int vjoy_muted = g_hidden_mode == 0;
                /* vJoy specifically (not ViGEmBus, not the dashboard --
                   both stay correct off the same gp values) reads Y
                   backwards vs. XInput/ViGEmBus -- negating it here,
                   confirmed correct live by the user. X, by contrast, was
                   ALSO reported backwards, but negating X was tried and
                   directly confirmed to NOT fix it -- so whatever's wrong
                   with X isn't a simple sign flip, and X is left as raw gp
                   here rather than "fixed" with a negation proven not to
                   work. Needs a real diagnosis (possibly a stale per-device
                   Windows joystick calibration on this specific vJoy
                   device/axis, outside anything this code touches --
                   worth checking via joy.cpl's own calibration screen)
                   before touching this again; don't just try flipping the
                   sign a third time. i16_to_axis() itself is a plain
                   unbiased rescale, and gp is shared and already correct,
                   so the Y negation belongs right here, applied only to
                   vJoy's own feed. Clamped the same way i16_to_axis()
                   itself already clamps its input. */
                /* Skip re-sending vJoy output entirely once it's muted and
                   nothing about the mode just changed -- confirmed via code
                   audit 2026-09-07 that every stick twitch was previously
                   pushing 24 identical all-zero DeviceIoControl calls to a
                   device nobody is reading in Normal mode (the common case).
                   Still sends exactly one neutral frame the moment muting
                   begins (mode_changed), so vJoy visibly zeroes out instead
                   of freezing at its last live value. */
                /* Locked for the whole write block, not just vigem_update()'s
                   own body: pSetAxis()/pSetBtn() below write to the vJoy
                   device via `rid`, and relinquish_vjoy_if_acquired() (called
                   from console_ctrl_handler() on its own thread, any time)
                   can call pRelinquishVJD(rid) concurrently with these very
                   calls otherwise -- the exact class of race the 2026-09-08
                   pre-ship audit flagged. CRITICAL_SECTION is reentrant for
                   the thread holding it, so vigem_update()'s own internal
                   Enter/Leave below doesn't deadlock against this outer one. */
                EnterCriticalSection(&g_output_lock);
                if (!vjoy_muted || mode_changed) {
                    SHORT nly = gp.sThumbLY == -32768 ? 32767 : (SHORT)(-gp.sThumbLY);
                    SHORT nry = gp.sThumbRY == -32768 ? 32767 : (SHORT)(-gp.sThumbRY);
                    pSetAxis(vjoy_muted ? i16_to_axis(0) : i16_to_axis(gp.sThumbLX), rid, HID_USAGE_X);
                    pSetAxis(vjoy_muted ? i16_to_axis(0) : i16_to_axis(nly), rid, HID_USAGE_Y);
                    pSetAxis(vjoy_muted ? u8_to_axis(0) : u8_to_axis(gp.bLeftTrigger), rid, HID_USAGE_Z);
                    pSetAxis(vjoy_muted ? u8_to_axis(0) : u8_to_axis(gp.bRightTrigger), rid, HID_USAGE_RZ);
                    pSetAxis(vjoy_muted ? i16_to_axis(0) : i16_to_axis(gp.sThumbRX), rid, HID_USAGE_RX);
                    pSetAxis(vjoy_muted ? i16_to_axis(0) : i16_to_axis(nry), rid, HID_USAGE_RY);

                    /* Whenever the PS button, touchpad, or Guide button is
                       (part of) the configured toggle combo, a press of it
                       is consumed for that purpose and deliberately never
                       ALSO forwarded as a normal vJoy button -- otherwise
                       pressing it to switch modes would simultaneously
                       fire whatever vJoy button it happens to be mapped to
                       (PS is button 17 by default), a confusing double-
                       duty press. Scoped to just these three: A/B/X/Y/etc.
                       staying part of both the toggle AND their own vJoy
                       button (e.g. the Back+Start default) is long-
                       standing, unchanged behavior -- only PS/touchpad/
                       Guide get this treatment, since they're the ones
                       explicitly meant to double as a game-invisible
                       toggle (see the button= ini comment in
                       save_config()). */
                    int ps_is_toggle = (g_toggle_button_mask & PHYS_PS_BUTTON) != 0;
                    int touchpad_is_toggle = (g_toggle_button_mask & PHYS_TOUCHPAD) != 0;
                    int guide_is_toggle = (g_toggle_button_mask & PHYS_GUIDE_BUTTON) != 0;

                    /* Every vJoy button 1-NUM_VJOY_BUTTONS is driven by
                       whichever physical input g_button_map says --
                       data-driven instead of a fixed A=1/B=2/... sequence so
                       run_button_map_flow() (press M) can remap any of them
                       live without touching this loop. */
                    for (int b = 0; b < NUM_VJOY_BUTTONS; b++) {
                        DWORD phys = g_button_map[b];
                        int pressed = vjoy_muted ? 0
                                      : phys == PHYS_TRIGGER_L   ? gp.bLeftTrigger > TRIGGER_CLICK_THRESHOLD
                                      : phys == PHYS_TRIGGER_R ? gp.bRightTrigger > TRIGGER_CLICK_THRESHOLD
                                      : phys == PHYS_PS_BUTTON  ? (ps_is_toggle ? 0 : g_ps_pressed)
                                      : phys == PHYS_TOUCHPAD   ? (touchpad_is_toggle ? 0 : g_touchpad_pressed)
                                      : phys == PHYS_GUIDE_BUTTON ? (guide_is_toggle ? 0 : g_guide_pressed)
                                                                : (gp.wButtons & phys) != 0;
                        pSetBtn(pressed, rid, (UCHAR)(b + 1));
                    }
                }

                /* The other half of the same mutual-exclusivity: same
                   source data, muted or live depending on g_hidden_mode,
                   fed to the (always-present) virtual gamepad instead. Same
                   skip-when-muted-and-unchanged optimization as vJoy above. */
                if (g_hidden_mode != 1 || mode_changed) vigem_update(&gp);
                LeaveCriticalSection(&g_output_lock);

                /* The configured controller button(s) also trigger the same
                   hide/unhide toggle as the keyboard hotkey -- fires once
                   per press (or once per completed hold, for a combo with a
                   hold requirement), not repeatedly while held. Checked
                   against the same combined wButtons+trigger+PS+touchpad+
                   Guide mask read_any_physical_input() builds (recomputed
                   here from this frame's already-polled gp/g_ps_pressed/
                   g_touchpad_pressed/g_guide_pressed, not by calling that
                   function again -- it does its own device poll, which
                   this frame already did once above) -- not just
                   gp.wButtons alone, now that the toggle combo can include
                   the PS button, touchpad, or (opt-in only) Xbox's Guide
                   button (see NAMED_PHYS/parse_button_line above). Runs
                   regardless of HOTAS/Normal mode -- the toggle has to
                   work in both. */
                DWORD live_phys_mask = gp.wButtons;
                if (gp.bLeftTrigger > TRIGGER_CLICK_THRESHOLD) live_phys_mask |= PHYS_TRIGGER_L;
                if (gp.bRightTrigger > TRIGGER_CLICK_THRESHOLD) live_phys_mask |= PHYS_TRIGGER_R;
                if (g_ps_pressed) live_phys_mask |= PHYS_PS_BUTTON;
                if (g_touchpad_pressed) live_phys_mask |= PHYS_TOUCHPAD;
                if (g_guide_pressed) live_phys_mask |= PHYS_GUIDE_BUTTON;
                int combo_now =
                    g_toggle_button_mask != 0 && (live_phys_mask & g_toggle_button_mask) == g_toggle_button_mask;
                if (combo_now && !combo_was) {
                    QueryPerformanceCounter(&combo_start); /* start of this hold */
                    combo_fired = 0;
                }
                if (combo_now && !combo_fired) {
                    if (g_toggle_hold_ms == 0) {
                        do_toggle();
                        combo_fired = 1;
                    } else {
                        LARGE_INTEGER heldNow;
                        QueryPerformanceCounter(&heldNow);
                        double held_ms = (double)(heldNow.QuadPart - combo_start.QuadPart) * 1000.0 / freq.QuadPart;
                        if (held_ms >= g_toggle_hold_ms) {
                            do_toggle();
                            combo_fired = 1;
                        }
                    }
                }
                combo_was = combo_now;
            }

            QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart - lastPrint.QuadPart) / freq.QuadPart;
            if (elapsed >= 0.066) { /* ~15Hz redraw -- fast enough to look live, cheap enough to not matter.
                                        Keypress handling lives in here too now, not every spin-loop iteration --
                                        confirmed 2026-09-07 (code audit) that PeekConsoleInputA was firing at the
                                        loop's full, uncapped iteration rate (an ALPC round-trip to conhost on
                                        every single pass of a deliberately Sleep-free loop) despite this exact
                                        block's own comment already claiming it only ran here. 15Hz is still far
                                        faster than any human keypress, so nothing is lost. */
                /* ~once/sec (15Hz tick rate / 15) -- battery status changes
                   far too slowly to need checking every dashboard redraw,
                   and HidD_GetInputReport/XInputGetBatteryInformation,
                   while both individually cheap, have no reason to run
                   any faster than this. */
                if (++batteryPollTicks >= 15) {
                    batteryPollTicks = 0;
                    if (g_ps_output_handle != INVALID_HANDLE_VALUE) poll_dualsense_battery();
                    else if (backend == BACKEND_XINPUT) poll_xbox_battery(userIndex);
                    else g_battery_src = BATTERY_SRC_NONE; /* DS4 real, or nothing applicable */
                    update_ps_mode_led(); /* catches a low-battery threshold crossing since the last poll --
                                              everything else that can change the color already calls this
                                              itself right when it happens (do_toggle(), the V key above) */
                }

                render_dashboard(&gp, count / elapsed, rid);
                count = 0;
                lastPrint = now;

                char key = check_console_keypress();
                if (key == 'R') {
                    run_remap_flow(backend, userIndex);
                    QueryPerformanceCounter(&lastPrint);
                } else if (key == 'G') {
                    if (g_game_path[0] == '\0') run_set_game_flow();
                    else launch_game();
                    QueryPerformanceCounter(&lastPrint);
                } else if (key == 'M') {
                    run_button_map_flow(backend, userIndex);
                    QueryPerformanceCounter(&lastPrint);
                } else if (key == 'V') {
                    /* Live switch between the emulated virtual controller
                       looking like an Xbox 360 pad or a DualShock 4 -- the
                       default already auto-matches whatever's actually
                       connected, this is the manual override for a specific
                       game that wants the other one. ViGEmBus targets can't
                       change type in place, so this tears down and recreates
                       it -- same cost as the initial connection setup, still
                       fast/clean, not a live-swap of anything fragile. */
                    vigem_teardown();
                    g_vigem_emulate_ds4 = !g_vigem_emulate_ds4;
                    vigem_setup(backend, userIndex);
                    update_ps_mode_led(); /* the real controller's identity (native PS vs standing in for
                                               Xbox) just changed -- reflect that on its own lightbar too */
                    clear_console();
                    QueryPerformanceCounter(&lastPrint);
                }
            }

            cpu_relax(); /* spin straight back to the next poll -- no Sleep */
        }

        if (backend == BACKEND_DINPUT && g_di_device) IDirectInputDevice8_Unacquire(g_di_device);
        /* Reset regardless of backend: the wired case (owns_handle=1) gets
           explicitly closed here since it's a separate handle from
           g_di_device; the Bluetooth cases (owns_handle=0) just get their
           now-stale copy of the handle value cleared -- the actual
           CloseHandle for those happens via g_ds4_raw_handle/
           g_dualsense_raw_handle's own cleanup right below, never both. */
        if (g_ps_output_owns_handle && g_ps_output_handle != INVALID_HANDLE_VALUE) CloseHandle(g_ps_output_handle);
        g_ps_output_handle = INVALID_HANDLE_VALUE;
        g_ps_output_is_dualsense = 0;
        g_ps_output_is_bt = 0;
        g_ps_output_owns_handle = 0;
        g_ps_output_shares_input_handle = 0;
        g_ps_led_r = g_ps_led_g = g_ps_led_b = 0; /* don't let a freshly (re)connected controller inherit a
                                                       stale color/rumble target from a previous session */
        g_ps_last_motor_left = g_ps_last_motor_right = 0;
        g_battery_src = BATTERY_SRC_NONE; /* stale battery % from a just-disconnected controller shouldn't
                                               linger on the waiting screen or a freshly reconnected one */
        g_hidhide_registered = 0; /* next connect (even the same physical controller again) gets its own real
                                      attempt instead of skipping registration because this looked "already done" */
        if ((backend == BACKEND_DS4_RAWHID || backend == BACKEND_DS4_WIRED_RAWHID) &&
             g_ds4_raw_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_ds4_raw_handle); /* otherwise held open for the rest of the process if something
                                                else (XInput, DirectInput) takes over on the next connect --
                                                also closes g_ps_output_handle for the wired case, since
                                                setup_ps_wired_raw_hid() points it at this same handle
                                                (g_ps_output_owns_handle is 0 there, so the block above
                                                won't also try to close it) */
            g_ds4_raw_handle = INVALID_HANDLE_VALUE;
        }
        if ((backend == BACKEND_DUALSENSE_RAWHID || backend == BACKEND_DUALSENSE_WIRED_RAWHID) &&
             g_dualsense_raw_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_dualsense_raw_handle); /* same reasoning as g_ds4_raw_handle above */
            g_dualsense_raw_handle = INVALID_HANDLE_VALUE;
        }
        vigem_teardown();
        relinquish_vjoy_if_acquired();
        restore_gamebar_guide_capture(); /* self-guarded/idempotent, safe to call even if this disconnect
                                              wasn't an Xbox controller or nothing was ever touched */
        backend = BACKEND_XINPUT; /* re-try XInput first on the next pass */
        /* Loop back to the top: render_waiting_screen will now show the
           "reconnect" wording since was_ever_connected is set. */
    }

    return 0; /* unreachable -- the loop above only exits the process via Ctrl+C */
}
