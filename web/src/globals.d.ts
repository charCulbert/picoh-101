// What the hosts around the page add to window.
import type { LineSocket } from "./live.js";

declare global {
    interface Window {
        // The app (app/src/shim.js): a socket to the Pico over the USB cable.
        __picohApp?: { openSocket(): LineSocket };
        // The plugin's page (synth/resources/page/index.html).
        __picohPlugin?: boolean;
    }
}
