//! The app: the GUI page, fetched from the Pico (page.rs), and its link to it: over USB, the GUI lines as
//! SysEx on its MIDI port through CoreMIDI (an iOS browser can open neither serial nor MIDI); otherwise
//! over Wi-Fi, its WebSocket (wifi.rs). The cable wins whenever it's there. shim.js gives the page a socket.
//!
//! The page is the device's code, so it gets that socket and nothing else: the four commands below
//! (capabilities/default.json), no other Tauri API, and no way out of the app's own origin (run()).

#[cfg(target_vendor = "apple")]
mod midi;
mod page;
mod wifi;

use serde_json::{json, Value};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::ipc::Channel;
use tauri::webview::NewWindowResponse;
use tauri::{AppHandle, Manager, State, WebviewUrl, WebviewWindowBuilder};

const SHIM: &str = include_str!("shim.js");
static APP: std::sync::OnceLock<AppHandle> = std::sync::OnceLock::new();

fn window() -> Option<tauri::WebviewWindow> {
    APP.get()?.get_webview_window("main")
}

/// A line on the loader's screen (loader/index.html); the page has no picohStatus and ignores it.
fn status(text: &str) {
    if let Some(w) = window() {
        let _ = w.eval(format!("window.picohStatus?.({})", json!(text)));
    }
}

fn fetched(how: &str, result: page::Fetched) {
    debug(&format!("page over {how}: {result:?}"));
    match result {
        page::Fetched::New => {
            if let Some(w) = window() {
                let _ = w.eval("location.reload()");
            }
        }
        page::Fetched::Failed(why) => status(&format!("The Pico didn't send its panel ({why}).")),
        page::Fetched::Same => {}
    }
}

/// The page's link: its channel, the Pico's MIDI port while it's plugged in and talking, and Wi-Fi
/// while there's no cable.
#[derive(Default)]
struct Link {
    page: Option<Channel<Value>>,
    pico: Option<(u32, u32)>, // CoreMIDI source and destination
    wifi: Option<wifi::Wifi>,
    wifi_open: bool,
    wifi_run: u32, // which Wi-Fi start the events heard are from
}

impl Link {
    fn to_page(&self, message: Value) {
        if let Some(page) = &self.page {
            let _ = page.send(message);
        }
    }
}

type Shared = Arc<Mutex<Link>>;

#[tauri::command]
fn link_open(channel: Channel<Value>, link: State<Shared>) {
    let mut l = link.lock().unwrap();
    l.page = Some(channel);
    // Already connected (the page reconnecting): say so, and ask for every value again.
    if let Some((_, dst)) = l.pico {
        l.to_page(json!({ "open": true, "via": "USB" }));
        #[cfg(target_vendor = "apple")]
        midi::send(dst, "hello");
        let _ = dst;
    } else if l.wifi_open {
        l.to_page(json!({ "open": true, "via": "Wi-Fi" }));
        if let Some(w) = &l.wifi {
            w.send("hello");
        }
    }
}

#[tauri::command]
fn link_send(line: String, link: State<Shared>) {
    if cfg!(debug_assertions) && std::env::var("PICOH_LOG_LINES").is_ok() {
        eprintln!("picoh: > {line}");
    }
    let l = link.lock().unwrap();
    if let Some((_, dst)) = l.pico {
        #[cfg(target_vendor = "apple")]
        midi::send(dst, &line);
        let _ = dst;
    } else if let (true, Some(w)) = (l.wifi_open, &l.wifi) {
        w.send(&line);
    }
}

/// Development: on the app's console, and (a phone has none to see) in picoh.log in the app's temp folder.
pub(crate) fn debug(msg: &str) {
    if cfg!(debug_assertions) {
        eprintln!("picoh: {msg}");
        use std::io::Write;
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(std::env::temp_dir().join("picoh.log")) {
            let _ = writeln!(f, "{msg}");
        }
    }
}

/// The page's errors and progress (shim.js).
#[tauri::command]
fn debug_log(msg: String) {
    debug(&format!("page: {msg}"));
}

#[tauri::command]
fn link_close(link: State<Shared>) {
    link.lock().unwrap().page = None;
}

/// Opens the link when the Pico is plugged in (and the page is there), pings it (the Pico drops a silent
/// SysEx GUI after 5 s), and closes it when it's unplugged.
#[cfg(target_vendor = "apple")]
fn watch(link: Shared) {
    let mut last_ping = Instant::now();
    let mut board = None; // the Pico the app is on, until it goes away
    let mut checks_left = 0; // tries left at its page
    loop {
        std::thread::sleep(Duration::from_millis(250));
        let on = link.lock().unwrap().pico.or(board);
        let found = midi::find(on);
        if found != board {
            board = found;
            checks_left = 3;
        }
        // A Pico just plugged in: its page first, before the link opens.
        if let Some((src, dst)) = found {
            if checks_left > 0 && link.lock().unwrap().pico.is_none() {
                stop_wifi(&mut link.lock().unwrap());
                page::wait_for_install();
                status("Loading the panel from the Pico…");
                let mut shown = 0;
                let result = page::fetch_usb(
                    src,
                    dst,
                    |got, size| {
                        if got * 20 / size != shown {
                            shown = got * 20 / size;
                            status(&format!("Loading the panel from the Pico… {}%", shown * 5));
                        }
                    },
                    || midi::find(Some((src, dst))) == Some((src, dst)),
                );
                checks_left = if matches!(result, page::Fetched::Failed(_)) { checks_left - 1 } else { 0 };
                fetched("USB", result);
                continue;
            }
        }
        let mut l = link.lock().unwrap();
        match (l.pico, found) {
            (None, Some((src, dst))) if l.page.is_some() => {
                let to_page = link.clone();
                midi::listen(
                    src,
                    Arc::new(move |line| {
                        if cfg!(debug_assertions) && std::env::var("PICOH_LOG_LINES").is_ok() {
                            eprintln!("picoh: < {line}");
                        }
                        if let Some(page) = &to_page.lock().unwrap().page {
                            let _ = page.send(json!({ "line": line }));
                        }
                    }),
                );
                l.pico = Some((src, dst));
                if cfg!(debug_assertions) {
                    eprintln!("picoh: Pico on USB, link open");
                }
                l.to_page(json!({ "open": true, "via": "USB" }));
                midi::send(dst, "hello");
                last_ping = Instant::now();
            }
            (Some((src, _)), found) if found.map(|f| f.0) != Some(src) || l.page.is_none() => {
                midi::stop(src);
                l.pico = None;
                if cfg!(debug_assertions) {
                    eprintln!("picoh: link closed");
                }
                if let Some(page) = &l.page {
                    let _ = page.send(json!({ "open": false }));
                }
            }
            (Some((_, dst)), _) if last_ping.elapsed() > Duration::from_secs(2) => {
                midi::send(dst, "ping");
                last_ping = Instant::now();
            }
            _ => {}
        }
        // Wi-Fi while the cable isn't there, page or not: it may bring the page.
        let want_wifi = found.is_none();
        if want_wifi && l.wifi.is_none() {
            debug("wifi: no cable, trying Wi-Fi");
            l.wifi = Some(start_wifi(link.clone(), l.wifi_run));
        } else if !want_wifi {
            stop_wifi(&mut l);
        }
    }
}

fn start_wifi(link: Shared, run: u32) -> wifi::Wifi {
    wifi::Wifi::start(move |event| {
        let mut l = link.lock().unwrap();
        // The cable took over, or this Wi-Fi was stopped: only a page it made current still counts.
        if l.pico.is_some() || l.wifi_run != run {
            if matches!(event, wifi::Event::Page(page::Fetched::New)) {
                fetched("Wi-Fi", page::Fetched::New);
            }
            return;
        }
        match event {
            wifi::Event::Page(result) => fetched("Wi-Fi", result),
            wifi::Event::Open => {
                l.wifi_open = true;
                if cfg!(debug_assertions) {
                    eprintln!("picoh: Pico on Wi-Fi, link open");
                }
                l.to_page(json!({ "open": true, "via": "Wi-Fi" }));
            }
            wifi::Event::Line(line) => l.to_page(json!({ "line": line })),
            wifi::Event::Closed => {
                l.wifi_open = false;
                l.to_page(json!({ "open": false }));
            }
        }
    })
}

#[cfg(target_vendor = "apple")]
fn stop_wifi(l: &mut Link) {
    if l.wifi.take().is_some() {
        l.wifi_run += 1;
        if std::mem::take(&mut l.wifi_open) {
            l.to_page(json!({ "open": false }));
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let mut context = tauri::generate_context!();
    page::use_kept_page(&mut context);
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![link_open, link_send, link_close, debug_log])
        .setup(|app| {
            page::set_root(app.path().app_local_data_dir()?.join("page"));
            let _ = APP.set(app.handle().clone());
            let link: Shared = Arc::new(Mutex::new(Link::default()));
            app.manage(link.clone());
            #[cfg(target_vendor = "apple")]
            std::thread::spawn(move || watch(link));
            // The app's own origin only: no navigating elsewhere, no new windows, no downloads.
            let mut window = WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
                .initialization_script(SHIM)
                .on_navigation(own_origin)
                .on_new_window(|_, _| NewWindowResponse::Deny)
                .on_download(|_, _| false);
            #[cfg(desktop)]
            {
                window = window.title("PICOH-101").inner_size(1280.0, 860.0).min_inner_size(900.0, 560.0);
            }
            window.build()?;
            Ok(())
        })
        .run(context)
        .expect("error while running the app");
}

/// The origin Tauri serves the app's files from: tauri://localhost, or http://tauri.localhost on Windows
/// and Android.
fn own_origin(url: &tauri::Url) -> bool {
    let (scheme, host) =
        if cfg!(any(windows, target_os = "android")) { ("http", "tauri.localhost") } else { ("tauri", "localhost") };
    url.scheme() == scheme
        && url.host_str() == Some(host)
        && url.port().is_none()
        && url.username().is_empty()
        && url.password().is_none()
}

#[cfg(test)]
mod tests {
    #[test]
    fn only_the_apps_own_origin() {
        let ok = |u: &str| super::own_origin(&tauri::Url::parse(u).unwrap());
        let own = if cfg!(any(windows, target_os = "android")) { "http://tauri.localhost" } else { "tauri://localhost" };
        assert!(ok(&format!("{own}/index.html")) && ok(&format!("{own}/dist/app.js?x#y")));
        for bad in ["tauri://localhost:8080/", "tauri://user@localhost/", "tauri://localhost.evil/", "http://tauri.localhost:1/", "https://tauri.localhost/", "http://localhost/", "file:///etc/passwd", "https://example.com/"] {
            assert!(!ok(bad), "{bad}");
        }
    }
}
