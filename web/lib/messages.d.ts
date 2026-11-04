// The plugin's webview bridge, from char-clap-template (libs/webview/js/messages.js): there in the
// plugin's build only, at page/lib/messages.js.
export function sendMessage(value: unknown): void;
export function onMessage(handler: (value: unknown) => void): void;
