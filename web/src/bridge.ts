// The plugin's link: the GUI lines as strings over the webview bridge (clap.webview). It never drops on
// its own, so LiveView doesn't wait for the Pico's once-a-second load to know it's there.
import { sendMessage, onMessage } from "../lib/messages.js";
import type { LineSocket, OpenSocket } from "./live.js";

let current: LineSocket | null = null;
onMessage((message) => { if (typeof message === "string") current?.onmessage?.({ data: message }); });

export function bridgeSocket(): OpenSocket {
    return () => {
        const socket: LineSocket = {
            readyState: 1, steady: true, transport: "", onopen: null, onmessage: null, onclose: null, onerror: null,
            send: (line) => sendMessage(line),
            close: () => { socket.readyState = 3; if (current === socket) current = null; socket.onclose?.(); },
        };
        current = socket;
        setTimeout(() => { socket.onopen?.(); sendMessage("hello"); });
        return socket;
    };
}
