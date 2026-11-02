/*
 * JoystickDoctor -- small standalone helper that ships beside the relay. Shows
 * every joystick Windows knows about, and clears out old leftover records of
 * ones that aren't plugged in anymore.
 *
 * Why it exists: Issue #11, a relay latched onto the wrong DirectInput device
 * (dashboard said "generic DirectInput", all axes centered, Updates/sec 0.0)
 * and nobody had a simple way to see what Windows was offering it.
 *
 * What it will and won't touch, because this project once bricked a whole PC's
 * keyboard and mouse by leaving a class filter behind: it removes single device
 * nodes that pass node_is_game_controller() below, and nothing else. It never
 * touches UpperFilters/LowerFilters, services, drivers, driver packages, or the
 * USB node a controller hangs off. Every removal re-checks the node right before
 * the call, so a stale list or a hand-typed --remove-id can't get past the check.
 *
 * Build:
 *   zig cc -target x86_64-windows-gnu -O2 -Wall -Wextra -s -o JoystickDoctor.exe joystick_doctor.c ^
 *       -ldinput8 -ldxguid -lole32 -lsetupapi -lcfgmgr32 -ladvapi32 -lshell32 -luser32
 *
 * (-s for a smaller exe with no .pdb beside it to ship by accident. No admin
 * manifest on purpose: listing must work without admin rights. Only an actual
 * removal asks, see relaunch_elevated().)
 *
 * Run: JoystickDoctor.exe [--list | --dry-run | --selftest]
 */
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <dinput.h>

#define VJOY_VID 0x1234u
#define VJOY_PID 0xBEADu
#define DATA_PROBE_MS 1500
#define MAX_CONNECTED 32
/* ponytail: fixed list, anything past this shows up on the next run after these
   are cleared. Grow it if a real PC ever has more than this many at once. */
#define MAX_LEFTOVERS 256
#define LOG_FILE_NAME L"joystick_doctor_log.txt"
#ifndef RELAY_MUTEX /* a test can point at another name, so it can't get in a real relay's way */
#define RELAY_MUTEX L"Global\\RuthlessControllerRelay.Running"
#endif

typedef struct {
    GUID guid;
    wchar_t name[MAX_PATH];
    unsigned vid, pid;
    wchar_t id[MAX_DEVICE_ID_LEN + 1]; /* device instance ID, empty if DirectInput wouldn't give a path */
} Connected;

typedef struct {
    wchar_t id[MAX_DEVICE_ID_LEN + 1];
    wchar_t desc[256];
    int still_loaded; /* only ever a vJoy root device, see collect_leftovers() */
} Leftover;

static Connected g_connected[MAX_CONNECTED];
static int g_connected_count = 0;
static Leftover g_leftovers[MAX_LEFTOVERS];
static int g_leftover_count = 0;
static int g_leftover_overflow = 0;
static UINT g_old_console_cp = 0;

/* Everything printed goes out as UTF-8, so a pad with a non-English name
   survives both the console and a redirected bug-report file. Four buffers
   because one printf can name more than one string. */
static const char *u8(const wchar_t *s) {
    static char bufs[4][1024];
    static int next = 0;
    char *b = bufs[next++ & 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, b, sizeof(bufs[0]), NULL, NULL)) b[0] = '\0';
    return b;
}

static void restore_console_cp(void) {
    SetConsoleOutputCP(g_old_console_cp);
}

/* ---------- the classifier: pure strings, so --selftest can hit it directly ---------- */

static int has_nocase(const wchar_t *hay, const wchar_t *needle) {
    size_t n = wcslen(needle);
    for (; *hay; hay++)
        if (_wcsnicmp(hay, needle, n) == 0) return 1;
    return 0;
}

/* Hardware and compatible IDs come from Windows as a "multi-string": several
   strings back to back, ended by an empty one. Checks every entry. */
static int multi_has(const wchar_t *multi, const wchar_t *needle) {
    if (!multi) return 0;
    for (; *multi; multi += wcslen(multi) + 1)
        if (has_nocase(multi, needle)) return 1;
    return 0;
}

static int node_is_vjoy_root(const wchar_t *hw) {
    return multi_has(hw, L"ROOT\\VID_1234&PID_BEAD");
}

/* The one gate every removal goes through. The keyboard/mouse refusal comes
   first and beats everything below it on purpose: a node that says both
   "game" and "keyboard" (combo receivers do this) is a keyboard. Losing a
   joystick record costs a replug, losing a keyboard can cost the whole PC. */
static int node_is_game_controller(const wchar_t *hw, const wchar_t *compat, const wchar_t *cls) {
    if (!cls) cls = L"";
    if (multi_has(hw, L"HID_DEVICE_SYSTEM_KEYBOARD") || multi_has(compat, L"HID_DEVICE_SYSTEM_KEYBOARD") ||
        multi_has(hw, L"HID_DEVICE_SYSTEM_MOUSE") || multi_has(compat, L"HID_DEVICE_SYSTEM_MOUSE") ||
        _wcsicmp(cls, L"Keyboard") == 0 || _wcsicmp(cls, L"Mouse") == 0)
        return 0;
    /* USB parents stay out of it: the record a game sees is the HID one under them. Only XInput's own node class counts. */
    if (hw && _wcsnicmp(hw, L"USB\\", 4) == 0 && _wcsicmp(cls, L"XnaComposite") != 0) return 0;
    return multi_has(hw, L"HID_DEVICE_SYSTEM_GAME") || multi_has(compat, L"HID_DEVICE_SYSTEM_GAME") ||
           multi_has(hw, L"&IG_") || _wcsicmp(cls, L"XnaComposite") == 0 || node_is_vjoy_root(hw);
}

/* ---------- reading Windows' device records ---------- */

typedef struct {
    wchar_t hw[2048], compat[2048], cls[64];
} NodeIds;

static int read_prop(HDEVINFO set, SP_DEVINFO_DATA *d, DWORD prop, wchar_t *buf, DWORD chars) {
    memset(buf, 0, chars * sizeof(wchar_t));
    /* Two characters held back so a multi-string is always properly ended. */
    if (SetupDiGetDeviceRegistryPropertyW(set, d, prop, NULL, (BYTE *)buf, (chars - 2) * sizeof(wchar_t), NULL))
        return 1;
    return GetLastError() == ERROR_INVALID_DATA; /* Windows' way of saying the node just doesn't have this one */
}

/* 0 on any read that failed for a real reason (a list too long for the buffer,
   say). Callers treat that as "not a game controller": a list we couldn't read
   is a list that might have said "keyboard" in it. */
static int read_node_ids(HDEVINFO set, SP_DEVINFO_DATA *d, NodeIds *n) {
    return read_prop(set, d, SPDRP_HARDWAREID, n->hw, 2048) && read_prop(set, d, SPDRP_COMPATIBLEIDS, n->compat, 2048) &&
           read_prop(set, d, SPDRP_CLASS, n->cls, 64);
}

/* Same check as the relay's instance_id_is_vigem_child(): walk up the device
   tree looking for a parent run by the ViGEmBus driver. That's the only reliable
   way to tell the relay's own virtual pad apart, since a virtual DS4 or Xbox pad
   is built to look exactly like the real thing everywhere else. */
static int instance_id_is_vigem_child(const wchar_t *id) {
    DEVINST dev;
    if (!id[0]) return 0; /* an empty ID means the root of the whole device tree to CM_Locate_DevNode */
    if (CM_Locate_DevNodeW(&dev, (DEVINSTID_W)id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 0;
    for (int depth = 0; depth < 8; depth++) {
        wchar_t svc[64];
        ULONG len = sizeof(svc);
        if (CM_Get_DevNode_Registry_PropertyW(dev, CM_DRP_SERVICE, NULL, svc, &len, 0) == CR_SUCCESS &&
            _wcsicmp(svc, L"ViGEmBus") == 0)
            return 1;
        DEVINST parent;
        if (CM_Get_Parent(&parent, dev, 0) != CR_SUCCESS) return 0;
        dev = parent;
    }
    return 0;
}

/* Same conversion as the relay's interface_path_is_vigem_child(), from the
   device path DirectInput hands back to the instance ID everything else wants:
   "\\?\hid#vid_045e&pid_028e&ig_01#3&966d8b0&0&0000#{interface-guid}"
   -> "hid\vid_045e&pid_028e&ig_01\3&966d8b0&0&0000". */
static int path_to_instance_id(const wchar_t *path, wchar_t *id, size_t id_chars) {
    id[0] = L'\0';
    if (!wcsncmp(path, L"\\\\?\\", 4) || !wcsncmp(path, L"\\\\.\\", 4)) path += 4;
    size_t n = wcslen(path);
    const wchar_t *guid = wcsrchr(path, L'#');
    if (guid && guid[1] == L'{') n = (size_t)(guid - path);
    if (n == 0 || n >= id_chars) return 0;
    for (size_t i = 0; i < n; i++) id[i] = path[i] == L'#' ? L'\\' : path[i];
    id[n] = L'\0';
    return 1;
}

/* NOT the relay's another_relay_running(), which uses CreateMutexA: that creates
   the mutex when nobody holds it, and the relay would then refuse to start for
   as long as this tool sat open. Open-only instead. The relay runs as admin, so
   its mutex normally refuses to open for a tool without admin rights: "access
   denied" means it exists too. Only "not found" counts as not running, so any
   surprise lands on the side that removes nothing. */
static int relay_running(void) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, RELAY_MUTEX);
    if (m) {
        CloseHandle(m);
        return 1;
    }
    return GetLastError() != ERROR_FILE_NOT_FOUND;
}

/* ---------- 1. Connected now ---------- */

static BOOL CALLBACK enum_connected_cb(const DIDEVICEINSTANCEW *inst, VOID *ctx) {
    (void)ctx;
    if (g_connected_count == MAX_CONNECTED) return DIENUM_STOP;
    Connected *c = &g_connected[g_connected_count++];
    c->guid = inst->guidInstance;
    wcsncpy(c->name, inst->tszInstanceName, MAX_PATH - 1);
    c->vid = inst->guidProduct.Data1 & 0xFFFFu;
    c->pid = (inst->guidProduct.Data1 >> 16) & 0xFFFFu;
    return DIENUM_CONTINUE;
}

static void device_path(LPDIRECTINPUT8W di, const GUID *guid, wchar_t *path) {
    LPDIRECTINPUTDEVICE8W dev = NULL;
    path[0] = L'\0';
    if (FAILED(IDirectInput8_CreateDevice(di, guid, &dev, NULL))) return;
    DIPROPGUIDANDPATH gp = {.diph = {sizeof(gp), sizeof(gp.diph), 0, DIPH_DEVICE}};
    if (SUCCEEDED(IDirectInputDevice8_GetProperty(dev, DIPROP_GUIDANDPATH, &gp.diph))) wcscpy(path, gp.wszPath);
    IDirectInputDevice8_Release(dev);
}

/* The relay's di_candidate_is_live() with a longer wait: this one is a person
   looking at a list, not a startup path, so it can afford to give a pad time.
   A buffered read only reports changes, so a pad sitting perfectly still can
   read as silent, which is why the text says to touch it. */
static int device_sends_data(LPDIRECTINPUT8W di, const GUID *guid, HWND hwnd) {
    LPDIRECTINPUTDEVICE8W dev = NULL;
    if (FAILED(IDirectInput8_CreateDevice(di, guid, &dev, NULL))) return 0;
    int live = 0;
    if (SUCCEEDED(IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2))) {
        IDirectInputDevice8_SetCooperativeLevel(dev, hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        DIPROPDWORD bufSize = {.diph = {sizeof(bufSize), sizeof(bufSize.diph), 0, DIPH_DEVICE}, .dwData = 32};
        IDirectInputDevice8_SetProperty(dev, DIPROP_BUFFERSIZE, &bufSize.diph);
        if (SUCCEEDED(IDirectInputDevice8_Acquire(dev))) {
            for (DWORD waited = 0; waited < DATA_PROBE_MS && !live; waited += 20) {
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

/* vJoy goes first: it's recognized by its numbers alone, wherever it hangs. */
static const char *device_kind(unsigned vid, unsigned pid, const wchar_t *path, const wchar_t *id) {
    if (vid == VJOY_VID && pid == VJOY_PID) return "vJoy";
    if (instance_id_is_vigem_child(id)) return "the relay's own virtual pad";
    if (has_nocase(path, L"ig_")) return "Xbox-style";
    return "joystick or PlayStation-style";
}

static void print_connected(void) {
    printf("Connected now, in the order games see them. Move a stick to help this along:\n");
    fflush(stdout);
    LPDIRECTINPUT8W di = NULL;
    if (FAILED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&di, NULL))) {
        printf("  Couldn't ask Windows for the list.\n");
        return;
    }
    IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, enum_connected_cb, NULL, DIEDFL_ATTACHEDONLY);
    if (g_connected_count == 0) printf("  Nothing.\n");

    /* DirectInput wants a real top-level window, and output sent to a file from
       something with no console has none to offer. */
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);

    for (int i = 0; i < g_connected_count; i++) {
        Connected *c = &g_connected[i];
        wchar_t path[MAX_PATH];
        device_path(di, &c->guid, path);
        path_to_instance_id(path, c->id, MAX_DEVICE_ID_LEN + 1);
        printf("%3d. %s\n     %04X:%04X, %s, ", i + 1, u8(c->name), c->vid, c->pid, device_kind(c->vid, c->pid, path, c->id));
        fflush(stdout);
        printf("%s\n", device_sends_data(di, &c->guid, hwnd) ? "sending data" : "silent (touch it to test)");
        fflush(stdout);
    }
    IDirectInput8_Release(di);
}

/* ---------- 2. Leftovers ---------- */

/* Whole device tree, no DIGCF_PRESENT, so unplugged records show up too. A
   record only counts as absent on CM_Locate_DevNode's own "no such device"
   answer, not on any failure at all. A vJoy root device that's still loaded
   counts as well, but only with no relay running: the relay makes vJoy fresh
   each session and removes it on exit, so one sitting there with no relay is
   what a crash leaves behind. */
static void collect_leftovers(void) {
    g_leftover_count = 0;
    g_leftover_overflow = 0;
    int relay = relay_running();
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        printf("  Couldn't read Windows' device list (error %lu).\n", (unsigned long)GetLastError());
        return;
    }
    SP_DEVINFO_DATA d = {.cbSize = sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        NodeIds n;
        wchar_t id[MAX_DEVICE_ID_LEN + 1];
        DEVINST dn;
        if (!read_node_ids(set, &d, &n) || !node_is_game_controller(n.hw, n.compat, n.cls)) continue;
        if (!SetupDiGetDeviceInstanceIdW(set, &d, id, MAX_DEVICE_ID_LEN + 1, NULL)) continue;
        CONFIGRET cr = CM_Locate_DevNodeW(&dn, id, CM_LOCATE_DEVNODE_NORMAL);
        int stray_vjoy = cr == CR_SUCCESS && !relay && node_is_vjoy_root(n.hw);
        if (cr != CR_NO_SUCH_DEVNODE && !stray_vjoy) continue;
        if (g_leftover_count == MAX_LEFTOVERS) {
            g_leftover_overflow = 1;
            break;
        }
        Leftover *l = &g_leftovers[g_leftover_count++];
        wcscpy(l->id, id);
        l->still_loaded = stray_vjoy;
        l->desc[0] = L'\0';
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)l->desc, sizeof(l->desc), NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE *)l->desc, sizeof(l->desc), NULL))
            wcscpy(l->desc, L"(no description)");
    }
    SetupDiDestroyDeviceInfoList(set);
}

static void print_leftovers(void) {
    printf("\nLeftovers, old records of devices that aren't plugged in:\n");
    collect_leftovers();
    if (g_leftover_count == 0) printf("  None.\n");
    for (int i = 0; i < g_leftover_count; i++) {
        printf("  %s%s\n    %s\n", u8(g_leftovers[i].desc),
               g_leftovers[i].still_loaded ? " (still loaded, but no relay is running)" : "", u8(g_leftovers[i].id));
    }
    if (g_leftover_overflow) printf("  There are more than these. Run this again after removing them.\n");
}

/* ---------- removing ---------- */

static void append_log(const wchar_t *id) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = n ? wcsrchr(path, L'\\') : NULL;
    FILE *f = NULL;
    if (slash) {
        wcscpy(slash + 1, LOG_FILE_NAME);
        f = _wfopen(path, L"a");
    }
    if (!f) {
        printf("  (couldn't write to %s)\n", u8(LOG_FILE_NAME));
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u removed %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, u8(id));
    fclose(f);
}

/* Opens one record and says why it must be left alone, or NULL if it's fair
   game. Fresh list every time, same reason destroy_vjoy_root_devices() re-fetches:
   each removal changes the device list out from under anything still holding it.
   *set always comes back for the caller to close, if it isn't invalid. */
static const char *open_and_check(const wchar_t *id, HDEVINFO *set, SP_DEVINFO_DATA *d) {
    NodeIds n;
    *set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (*set == INVALID_HANDLE_VALUE || !id[0] || !SetupDiOpenDeviceInfoW(*set, id, NULL, 0, d)) return "Windows has no record of it";
    if (!read_node_ids(*set, d, &n)) return "couldn't read what it is";
    if (!node_is_game_controller(n.hw, n.compat, n.cls)) return "it isn't a game controller";
    if (instance_id_is_vigem_child(id)) return "it's the relay's own virtual pad";
    return NULL;
}

/* The same action Device Manager's "Uninstall device" takes, on this one node
   only. See destroy_vjoy_root_devices() for why not pnputil. Returns 1 if it
   went (or in a dry run, would go). */
static int remove_node(const wchar_t *id, int dry) {
    HDEVINFO set;
    SP_DEVINFO_DATA d = {.cbSize = sizeof(d)};
    const char *why = open_and_check(id, &set, &d);
    int ok = 0;
    if (why) {
        printf("  Left alone: %s (%s)\n", u8(id), why);
    } else if (dry) {
        printf("  Would remove: %s\n", u8(id));
        ok = 1;
    } else if (SetupDiCallClassInstaller(DIF_REMOVE, set, &d)) {
        SP_DEVINSTALL_PARAMS_W p = {.cbSize = sizeof(p)};
        int restart = SetupDiGetDeviceInstallParamsW(set, &d, &p) && (p.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART));
        printf("  Removed: %s%s\n", u8(id), restart ? " (finishes after a restart)" : "");
        append_log(id);
        ok = 1;
    } else {
        DWORD err = GetLastError();
        printf("  Couldn't remove: %s (Windows error 0x%lX)\n", u8(id), (unsigned long)err);
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    fflush(stdout);
    return ok;
}

static int is_elevated(void) {
    HANDLE tok;
    TOKEN_ELEVATION e;
    DWORD n;
    int up = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n)) up = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return up;
}

/* id NULL means every leftover. The elevated copy gets its own console window
   (Windows won't share one across the admin line), so the results print there
   and this copy just waits for it to close. */
static int relaunch_elevated(const wchar_t *id) {
    wchar_t exe[MAX_PATH * 2], params[MAX_DEVICE_ID_LEN + 32];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 1;
    if (id) _snwprintf(params, MAX_DEVICE_ID_LEN + 32, L"--remove-id \"%ls\"", id);
    else wcscpy(params, L"--remove-leftovers");

    printf("Asking Windows for admin rights. The results show in a new window.\n");
    fflush(stdout);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); /* ShellExecuteEx asks for this */
    SHELLEXECUTEINFOW sei = {.cbSize = sizeof(sei), .fMask = SEE_MASK_NOCLOSEPROCESS, .lpVerb = L"runas",
                             .lpFile = exe, .lpParameters = params, .nShow = SW_SHOWNORMAL};
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) printf("You said no to the admin prompt. Nothing was removed.\n");
        else printf("Couldn't get admin rights (Windows error %lu). Nothing was removed.\n", (unsigned long)err);
        return 1;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    printf(code == 0 ? "Done.\n" : "Done, but not everything was removed. The other window said why.\n");
    return code != 0;
}

/* id NULL means every leftover, freshly re-read here rather than trusted from
   whoever launched this: something may have been plugged back in since. */
static int do_removal(const wchar_t *id, int dry) {
    if (!dry && !is_elevated()) return relaunch_elevated(id);
    if (relay_running()) {
        printf("The relay is running. Close it first. Nothing was removed.\n");
        return 1;
    }
    printf("%s\n", dry ? "Dry run, nothing will be changed:" : "Removing:");
    int failed = 0;
    if (id) {
        failed = !remove_node(id, dry);
    } else {
        collect_leftovers();
        if (g_leftover_count == 0) printf("  Nothing to remove.\n");
        for (int i = 0; i < g_leftover_count; i++) failed += !remove_node(g_leftovers[i].id, dry);
    }
    if (dry) printf("A real run asks for admin rights first, then does the same.\n");
    return failed != 0;
}

/* ---------- 3. The prompt ---------- */

static void read_answer(char *line, int size) {
    fflush(stdout);
    if (!fgets(line, size, stdin)) line[0] = '\0'; /* no input at all is a no */
    line[strcspn(line, "\r\n")] = '\0';
}

static int is_yes(const char *s) {
    return _stricmp(s, "y") == 0 || _stricmp(s, "yes") == 0;
}

/* One question, one answer, anything but a clear yes or a good number is a no.
   Run it again to do more. */
static int prompt_and_remove(void) {
    char line[64];
    if (relay_running()) {
        printf("\nThe relay is running. Close it first if you want to remove anything.\n");
        return 0;
    }
    if (g_leftover_count == 0 && g_connected_count == 0) {
        printf("\nNothing to remove.\n");
        return 0;
    }
    printf("\n");
    if (g_connected_count) printf("To remove one device from the Connected list instead, type its number.\n");
    if (g_leftover_count == 1)
        printf("Remove this 1 leftover? Windows makes a record again if that device is ever plugged back in. [y/N] ");
    else if (g_leftover_count)
        printf("Remove these %d leftovers? Windows makes a record again if that device is ever plugged back in. [y/N] ", g_leftover_count);
    else
        printf("No leftovers to remove. Type a number, or press Enter to finish: ");
    read_answer(line, sizeof(line));

    if (is_yes(line) && g_leftover_count) return do_removal(NULL, 0);

    char *end;
    long n = strtol(line, &end, 10);
    if (end == line || *end) {
        printf("Nothing removed.\n");
        return 0;
    }
    if (n < 1 || n > g_connected_count) {
        printf("There's no number %ld in the Connected list. Nothing removed.\n", n);
        return 0;
    }
    Connected *c = &g_connected[n - 1];
    HDEVINFO set;
    SP_DEVINFO_DATA d = {.cbSize = sizeof(d)};
    const char *why = open_and_check(c->id, &set, &d);
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    if (why) {
        printf("Won't remove %s: %s.\n", u8(c->name), why);
        return 0;
    }
    printf("Remove the record for %s? It comes back when it's plugged back in or on restart. [y/N] ", u8(c->name));
    read_answer(line, sizeof(line));
    if (!is_yes(line)) {
        printf("Nothing removed.\n");
        return 0;
    }
    return do_removal(c->id, 0);
}

/* Only when this console is ours alone (double-clicked, or the admin relaunch),
   since it would otherwise vanish before anyone could read it. A shell sharing
   the console, or output going to a file, means no pause. */
static void pause_if_own_console(void) {
    DWORD procs[2], mode;
    if (GetConsoleProcessList(procs, 2) != 1) return;
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) || !GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) return;
    printf("\nPress Enter to close.");
    fflush(stdout);
    getchar();
}

/* ---------- --selftest ---------- */

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, what)                                 \
    do {                                                  \
        int passed_ = (cond);                             \
        g_checks++;                                       \
        if (!passed_) g_fails++;                          \
        printf("%s %s\n", passed_ ? "ok  " : "FAIL", what); \
    } while (0)

/* Multi-strings: the literal's own final NUL is what ends the list. */
#define KEYBOARD_COMPAT L"HID_DEVICE_SYSTEM_KEYBOARD\0HID_DEVICE_UP:0001_U:0006\0HID_DEVICE\0"
#define MOUSE_COMPAT L"HID_DEVICE_SYSTEM_MOUSE\0HID_DEVICE_UP:0001_U:0002\0HID_DEVICE\0"
#define GAME_COMPAT L"HID_DEVICE_SYSTEM_GAME\0HID_DEVICE_UP:0001_U:0005\0HID_DEVICE\0"

static int selftest(void) {
    CHECK(!node_is_game_controller(L"HID\\VID_046D&PID_C52B&MI_00\0", KEYBOARD_COMPAT, L""), "keyboard compat ID rejected");
    CHECK(!node_is_game_controller(L"HID\\VID_046D&PID_C52B&MI_01\0", MOUSE_COMPAT, L""), "mouse compat ID rejected");
    CHECK(!node_is_game_controller(L"HID_DEVICE_SYSTEM_KEYBOARD\0", NULL, NULL), "keyboard hardware ID rejected");
    CHECK(!node_is_game_controller(L"HID_DEVICE_SYSTEM_MOUSE\0", NULL, NULL), "mouse hardware ID rejected");
    CHECK(!node_is_game_controller(NULL, L"HID_DEVICE_SYSTEM_GAME\0HID_DEVICE_SYSTEM_KEYBOARD\0", L"HIDClass"), "game + keyboard rejected (keyboard listed second)");
    CHECK(!node_is_game_controller(NULL, L"HID_DEVICE_SYSTEM_KEYBOARD\0HID_DEVICE_SYSTEM_GAME\0", L"HIDClass"), "game + keyboard rejected (keyboard listed first)");
    CHECK(!node_is_game_controller(NULL, L"HID_DEVICE_SYSTEM_GAME\0HID_DEVICE_SYSTEM_MOUSE\0", L"HIDClass"), "game + mouse rejected");
    CHECK(!node_is_game_controller(L"HID_DEVICE_SYSTEM_GAME\0", KEYBOARD_COMPAT, L"HIDClass"), "game in hardware IDs, keyboard in compat IDs rejected");
    CHECK(!node_is_game_controller(L"HID\\VID_045E&PID_02FF&IG_00\0", GAME_COMPAT, L"Keyboard"), "class Keyboard rejected despite game IDs");
    CHECK(!node_is_game_controller(L"HID\\VID_045E&PID_02FF&IG_00\0", GAME_COMPAT, L"mouse"), "class Mouse rejected despite game IDs, any case");
    CHECK(!node_is_game_controller(L"root\\VID_1234&PID_BEAD\0", MOUSE_COMPAT, L""), "vJoy ID with mouse compat ID rejected");

    CHECK(node_is_game_controller(L"HID\\VID_054C&PID_0CE6\0", GAME_COMPAT, L"HIDClass"), "HID_DEVICE_SYSTEM_GAME accepted");
    CHECK(node_is_game_controller(L"HID_DEVICE_SYSTEM_GAME\0", NULL, NULL), "HID_DEVICE_SYSTEM_GAME in hardware IDs accepted");
    CHECK(node_is_game_controller(L"HID\\VID_045E&PID_02FF&IG_00\0", NULL, NULL), "HID\\VID_045E&PID_02FF&IG_00 accepted");
    CHECK(node_is_game_controller(L"hid\\vid_045e&pid_02ff&ig_00\0", L"", L""), "lowercase ig_ accepted");
    CHECK(node_is_game_controller(L"USB\\VID_045E&PID_028E&REV_0114\0USB\\VID_045E&PID_028E\0", NULL, L"XnaComposite"), "class XnaComposite accepted");
    CHECK(node_is_game_controller(L"root\\VID_1234&PID_BEAD&REV_0219\0", NULL, L"HIDClass"), "vJoy root device accepted");

    CHECK(!node_is_game_controller(L"USB\\ROOT_HUB30\0", L"USB\\ROOT_HUB\0", L"USB"), "USB root hub rejected");
    CHECK(!node_is_game_controller(L"USB\\VID_05E3&PID_0610&REV_9226\0USB\\VID_05E3&PID_0610\0", L"USB\\USB20_HUB\0USB\\Class_09\0", L"USB"), "USB hub rejected");
    CHECK(!node_is_game_controller(L"USB\\VID_054C&PID_0CE6&REV_0100\0", L"USB\\COMPOSITE\0", L"USB"), "the USB parent of a controller rejected");
    CHECK(!node_is_game_controller(L"USB\\VID_045E&PID_028E&IG_00\0", NULL, L"HIDClass"), "a USB record with &IG_ rejected");
    CHECK(!node_is_game_controller(L"HDAUDIO\\FUNC_01&VEN_10EC&DEV_0887\0", L"HDAUDIO\\FUNC_01\0", L"MEDIA"), "audio device rejected");
    CHECK(!node_is_game_controller(L"USB\\VID_1234&PID_BEAD\0", NULL, L""), "same numbers as vJoy, not its root device, rejected");
    CHECK(!node_is_game_controller(L"HID\\VID_045E&PID_02FF\0", L"&IG_00\0", L""), "&IG_ only counts in hardware IDs");
    CHECK(!node_is_game_controller(NULL, NULL, NULL), "empty rejected (nothing at all)");
    CHECK(!node_is_game_controller(L"", L"", L""), "empty rejected (empty strings)");

    wchar_t id[MAX_DEVICE_ID_LEN + 1];
    path_to_instance_id(L"\\\\?\\hid#vid_045e&pid_028e&ig_01#3&966d8b0&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}", id, MAX_DEVICE_ID_LEN + 1);
    CHECK(wcscmp(id, L"hid\\vid_045e&pid_028e&ig_01\\3&966d8b0&0&0000") == 0, "device path turned into an instance ID");
    CHECK(!path_to_instance_id(L"", id, MAX_DEVICE_ID_LEN + 1) && !id[0], "empty device path gives no instance ID");
    CHECK(!strcmp(device_kind(VJOY_VID, VJOY_PID, L"", L""), "vJoy"), "vJoy named as vJoy");
    CHECK(!strcmp(device_kind(0x045E, 0x02FF, L"\\\\?\\HID#VID_045E&PID_02FF&IG_00#x", L""), "Xbox-style"), "ig_ in the path named Xbox-style");
    CHECK(!strcmp(device_kind(0x054C, 0x0CE6, L"\\\\?\\HID#VID_054C&PID_0CE6#x", L""), "joystick or PlayStation-style"), "no ig_ named joystick or PlayStation-style");

    printf("%d of %d checks passed\n", g_checks - g_fails, g_checks);
    return g_fails != 0;
}

int main(void) {
    int argc = 0, list = 0, dry = 0, all = 0, test = 0, bad = 0;
    const wchar_t *id = NULL;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--list")) list = 1;
        else if (!wcscmp(argv[i], L"--dry-run")) dry = 1;
        else if (!wcscmp(argv[i], L"--selftest")) test = 1;
        else if (!wcscmp(argv[i], L"--remove-leftovers")) all = 1;
        else if (!wcscmp(argv[i], L"--remove-id") && i + 1 < argc && wcslen(argv[i + 1]) <= MAX_DEVICE_ID_LEN) id = argv[++i];
        else bad = 1;
    }
    if (bad) {
        printf("JoystickDoctor [--list | --dry-run | --selftest]\n"
               "  --list      print what's there and exit, good for a bug report\n"
               "  --dry-run   print what removal would do, and do nothing\n"
               "  --selftest  check the game-controller test and exit\n");
        return 2;
    }
    if (test) return selftest();

    g_old_console_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    atexit(restore_console_cp);

    int rc = 0;
    if (id || all) {
        rc = do_removal(id, dry);
    } else {
        print_connected();
        print_leftovers();
        printf("\nThe relay is %s.\n", relay_running() ? "running" : "not running");
        if (list) return 0;
        if (dry) {
            printf("\n");
            return do_removal(NULL, 1);
        }
        rc = prompt_and_remove();
    }
    if (!dry) pause_if_own_console();
    return rc;
}
