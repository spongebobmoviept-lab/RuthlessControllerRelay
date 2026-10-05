/* RelayControl -- sends one command to a running Ruthless Controller Relay
   over its local control pipe and prints the one-line reply.

   For Stream Deck / macro-pad buttons and scripts. The relay only listens
   when its ini has control_pipe=on (off by default). Runs as a normal user:
   no admin prompt, even though the relay itself runs as administrator.

   Usage:   RelayControl.exe <command>
   e.g.     RelayControl.exe mode toggle
            RelayControl.exe status
   Commands: status | mode hotas|normal|toggle | pad x360|ds4|toggle |
             freelook on|off|toggle | input | quit
   Exit code: 0 = the relay answered "ok ...", 1 = it answered "err ...",
              2 = relay not running / control_pipe not on / no answer,
              3 = bad usage.

   Built as a windowed (no console) program so a Stream Deck button doesn't
   flash a black window. The reply still prints when it's run from a
   terminal (it borrows the terminal's console) or when its output is piped
   or redirected. */
#include <windows.h>
#include <string.h>

#define CONTROL_PIPE_NAME "\\\\.\\pipe\\RuthlessControllerRelay"
#define REPLY_TIMEOUT_MS 2000

static void out(const char *s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
        /* Started from a terminal with no redirection: write to that terminal. */
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
        h = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD wrote;
    WriteFile(h, s, (DWORD)strlen(s), &wrote, NULL);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show) {
    (void)inst; (void)prev; (void)cmdLine; (void)show;
    char cmd[128] = {0};
    for (int i = 1; i < __argc; i++) {
        size_t have = strlen(cmd), add = strlen(__argv[i]);
        if (have + add + 3 > sizeof(cmd)) { out("RelayControl: command too long\n"); return 3; }
        if (i > 1) cmd[have++] = ' ';
        memcpy(cmd + have, __argv[i], add + 1);
    }
    if (!cmd[0]) {
        out("Usage: RelayControl.exe <command>   e.g. RelayControl.exe mode toggle\n"
            "Commands: status | mode hotas|normal|toggle | pad x360|ds4|toggle | freelook on|off|toggle | input | quit\n");
        return 3;
    }
    strcat(cmd, "\n");

    /* The relay serves one client at a time; wait a moment if it's busy. */
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 10 && pipe == INVALID_HANDLE_VALUE; tries++) {
        pipe = CreateFileA(CONTROL_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_PIPE_BUSY) break;
            WaitNamedPipeA(CONTROL_PIPE_NAME, 300);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        out("err relay not running, or control_pipe=on isn't set in its ini\n");
        return 2;
    }

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD n = 0;
    BOOL ok = WriteFile(pipe, cmd, (DWORD)strlen(cmd), NULL, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WaitForSingleObject(ov.hEvent, REPLY_TIMEOUT_MS) == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &ov, &n, FALSE);
    if (!ok) { CancelIo(pipe); CloseHandle(ov.hEvent); CloseHandle(pipe); out("err no answer from the relay\n"); return 2; }

    /* Read exactly one reply line. */
    char reply[512];
    DWORD total = 0;
    DWORD until = GetTickCount() + REPLY_TIMEOUT_MS;
    while (total < sizeof(reply) - 1 && (total == 0 || reply[total - 1] != '\n')) {
        DWORD now = GetTickCount();
        if ((LONG)(until - now) <= 0) break;
        ResetEvent(ov.hEvent);
        n = 0;
        ok = ReadFile(pipe, reply + total, (DWORD)(sizeof(reply) - 1 - total), NULL, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, until - now) != WAIT_OBJECT_0) { CancelIo(pipe); break; }
            ok = TRUE;
        }
        if (!ok || !GetOverlappedResult(pipe, &ov, &n, FALSE) || n == 0) break;
        total += n;
    }
    CloseHandle(ov.hEvent);
    CloseHandle(pipe);
    reply[total] = 0;
    if (total == 0) { out("err no answer from the relay\n"); return 2; }
    if (reply[total - 1] != '\n' && total < sizeof(reply) - 1) { reply[total++] = '\n'; reply[total] = 0; }
    out(reply);
    return strncmp(reply, "ok", 2) == 0 ? 0 : 1;
}
