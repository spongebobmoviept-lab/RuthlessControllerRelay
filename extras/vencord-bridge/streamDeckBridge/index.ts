/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Optional add-on for Ruthless Controller Relay's Stream Deck guide; builds into Vencord. See docs/STREAM_DECK.md.
 *
 * StreamDeckBridge -- A Stream Deck or a script reads and flips Discord's Krisp (noise filter) through this plugin,
 * in the background: no window is shown, focused or clicked. See native.ts for the local pipe it listens on.
 */

import { findByPropsLazy } from "@webpack";
import { FluxDispatcher, MediaEngineStore, SelectedChannelStore } from "@webpack/common";
import definePlugin, { PluginNative } from "@utils/types";

const Native = VencordNative.pluginHelpers.StreamDeckBridge as PluginNative<typeof import("./native")>;

// Discord's own audio settings actions (the same ones its Krisp switch uses)
const AudioActions = findByPropsLazy("setNoiseCancellation", "setNoiseSuppression");

function status() {
    const inCall = SelectedChannelStore.getVoiceChannelId() != null;
    const b = (v: boolean) => (v ? "1" : "0");
    return "ok incall=" + b(inCall) + " krisp=" + b(MediaEngineStore.getNoiseCancellation())
        + " mute=" + b(MediaEngineStore.isSelfMute()) + " deaf=" + b(MediaEngineStore.isSelfDeaf());
}

function setKrisp(on: boolean) {
    if (MediaEngineStore.getNoiseCancellation() === on) return;
    const location = { section: "Stream Deck" };
    try {
        AudioActions.setNoiseCancellation(on, location);
    } catch {
        FluxDispatcher.dispatch({ type: "AUDIO_SET_NOISE_CANCELLATION", enabled: on, location } as any);
    }
}

export default definePlugin({
    name: "StreamDeckBridge",
    description: "Lets a Stream Deck read and switch Krisp in the background (local pipe, no windows).",
    authors: [{ name: "Ruthless Controller Relay", id: 0n }],
    enabledByDefault: true,

    handle(cmd: string): string {
        try {
            if (cmd === "status") return status();
            if (cmd === "krisp on" || cmd === "krisp off" || cmd === "krisp toggle") {
                setKrisp(cmd === "krisp toggle" ? !MediaEngineStore.getNoiseCancellation() : cmd === "krisp on");
                return status();
            }
            return "err unknown command";
        } catch (e) {
            return "err " + String(e);
        }
    },

    start() { Native.start(); },
    stop() { Native.stop(); },
});
