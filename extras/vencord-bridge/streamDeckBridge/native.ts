/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Optional add-on for Ruthless Controller Relay's Stream Deck guide; builds into Vencord. See docs/STREAM_DECK.md.
 *
 * StreamDeckBridge -- lets a Stream Deck or a script read and flip Discord's Krisp (noise filter) in the
 * background, without touching any window. Runs in Discord's main process: listens on a local named pipe,
 * hands each command to the plugin in the Discord window and writes back its one-line answer.
 *
 * Protocol: connect to \\.\pipe\DiscordBridge, write one line, read one line, close.
 *   status                    -> ok incall=0|1 krisp=0|1 mute=0|1 deaf=0|1
 *   krisp on | off | toggle   -> same line, after the change
 * Named pipes are local only; nothing on the network can reach it.
 */

import { IpcMainInvokeEvent, WebContents } from "electron";
import { createServer, Server } from "net";

const PIPE = "\\\\.\\pipe\\DiscordBridge";
let server: Server | null = null;
let discordWindow: WebContents | null = null;

async function ask(cmd: string): Promise<string> {
    const w = discordWindow;
    if (!w || w.isDestroyed()) return "err discord not ready";
    const call = w.executeJavaScript("Vencord.Plugins.plugins.StreamDeckBridge.handle(" + JSON.stringify(cmd) + ")");
    const timeout = new Promise<string>(resolve => setTimeout(() => resolve("err discord busy"), 2000));
    try { return String(await Promise.race([call, timeout])); } catch (e) { return "err " + String(e); }
}

// Called by the plugin in the Discord window when it starts: remember that window and open the pipe.
export function start(e: IpcMainInvokeEvent) {
    discordWindow = e.sender;
    if (server) return;
    server = createServer(sock => {
        let buf = "";
        sock.setEncoding("utf8");
        sock.setTimeout(3000, () => sock.destroy());
        sock.on("error", () => { });
        sock.on("data", async (chunk: string) => {
            buf += chunk;
            const nl = buf.indexOf("\n");
            if (nl < 0) { if (buf.length > 200) sock.destroy(); return; }
            const cmd = buf.slice(0, nl).trim().toLowerCase();
            buf = "";
            const reply = cmd.length > 60 ? "err bad command" : await ask(cmd);
            sock.end(reply + "\n");
        });
    });
    server.on("error", () => { server = null; });   // another copy has the pipe, or it went away
    server.listen(PIPE);
}

export function stop() {
    server?.close();
    server = null;
    discordWindow = null;
}
