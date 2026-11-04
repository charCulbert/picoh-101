//! The app: the GUI page (web/) and its link to the Pico: over USB, the GUI lines as SysEx on its MIDI
//! port through CoreMIDI (an iOS browser can open neither serial nor MIDI); otherwise over Wi-Fi, its
//! WebSocket (wifi.rs). The cable wins whenever it's there. shim.js gives the page a socket.

#[cfg(target_vendor = "apple")]
mod midi;
mod wifi;

use serde_json::{json, Value};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::ipc::Channel;
use tauri::{Manager, State, WebviewUrl, WebviewWindowBuilder};

const SHIM: &str = include_str!("shim.js");
// Changes with every change to the page (build.rs), so the app recompiles and embeds the new one.
const _PAGE: &str = env!("PICOH_PAGE_STAMP");

/// The page's link: its channel, the Pico's MIDI port while it's plugged in and talking, and Wi-Fi
/// while there's no cable.
#[derive(Default)]
struct Link {
    page: Option<Channel<Value>>,
    pico: Option<(u32, u32)>, // CoreMIDI source and destination
    wifi: Option<wifi::Wifi>,
    wifi_open: bool,
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
    loop {
        std::thread::sleep(Duration::from_millis(250));
        let found = midi::find();
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
        // Wi-Fi while the page is there and the cable isn't.
        let want_wifi = l.page.is_some() && l.pico.is_none();
        if want_wifi && l.wifi.is_none() {
            debug("wifi: no cable, trying Wi-Fi");
            l.wifi = Some(start_wifi(link.clone()));
        } else if !want_wifi && l.wifi.is_some() {
            l.wifi = None;
            l.wifi_open = false;
        }
    }
}

fn start_wifi(link: Shared) -> wifi::Wifi {
    wifi::Wifi::start(move |event| {
        let mut l = link.lock().unwrap();
        if l.pico.is_some() {
            return; // the cable took over
        }
        match event {
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![link_open, link_send, link_close, debug_log])
        .setup(|app| {
            let link: Shared = Arc::new(Mutex::new(Link::default()));
            app.manage(link.clone());
            #[cfg(target_vendor = "apple")]
            std::thread::spawn(move || watch(link));
            let mut window = WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
                .initialization_script(SHIM);
            #[cfg(desktop)]
            {
                window = window.title("PICOH-101").inner_size(1280.0, 860.0).min_inner_size(900.0, 560.0);
            }
            window.build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running the app");
}
