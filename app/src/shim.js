// Runs in the app's page before its own scripts. Gives the page (web/src/app.ts) a socket to the Pico
// over the app's IPC: lib.rs carries its lines over the USB cable or Wi-Fi. It opens when the Pico is
// reachable and closes when it isn't; live.ts then keeps reopening it, as it does a WebSocket.
(() => {
    if (window.__picohApp) return;
    const core = window.__TAURI__.core;
    document.documentElement.dataset.app = ""; // page.css: no theme or USB buttons in the app
    // Development: the page's errors reach the app's console (lib.rs debug_log).
    const report = (msg) => core.invoke("debug_log", { msg: String(msg) }).catch(() => {});
    // Capture, so a script or stylesheet that fails to load is reported too (its error doesn't bubble).
    addEventListener("error", (e) => report(e.message ? `error: ${e.message} at ${e.filename}:${e.lineno}`
        : `failed to load ${e.target?.src ?? e.target?.href ?? e.target?.tagName}`), true);
    addEventListener("unhandledrejection", (e) => report(`rejection: ${e.reason?.message ?? e.reason}`));
    addEventListener("DOMContentLoaded", () => report(`loaded ${location.href}, ${document.styleSheets.length} stylesheets`));
    window.__picohApp = {
        openSocket() {
            const socket = { readyState: 0, onopen: null, onmessage: null, onclose: null, onerror: null, transport: "" };
            const channel = new core.Channel();
            channel.onmessage = (m) => {
                if (socket.readyState === 3) return;
                if (m.open === true && m.via) socket.transport = m.via; // USB, or Wi-Fi
                if (m.open === true && socket.readyState === 0) { socket.readyState = 1; socket.onopen?.(); }
                else if (m.open === false) socket.close();
                else if (typeof m.line === "string" && socket.readyState === 1) socket.onmessage?.({ data: m.line });
            };
            socket.send = (line) => { if (socket.readyState === 1) core.invoke("link_send", { line: String(line) }); };
            socket.close = () => {
                if (socket.readyState === 3) return;
                socket.readyState = 3;
                channel.onmessage = () => {};
                core.invoke("link_close");
                socket.onclose?.();
            };
            core.invoke("link_open", { channel });
            return socket;
        },
    };
})();
