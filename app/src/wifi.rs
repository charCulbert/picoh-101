//! The Pico over Wi-Fi: its WebSocket (firmware/wifi.cpp), the GUI lines both ways. A thread connects,
//! to the Pico's own network first and then to it on the network it joined (picoh-101.local), and
//! connects again whenever the link drops, until the Wifi is dropped.

use std::net::{TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;
use std::time::Duration;
use tungstenite::{Message, WebSocket};

const HOSTS: &[&str] = &["192.168.4.1", "picoh-101.local"];

pub enum Event {
    Open,
    Line(String),
    Closed,
}

pub struct Wifi {
    to_pico: Sender<String>,
    stop: Arc<AtomicBool>,
}

impl Wifi {
    pub fn start(on_event: impl Fn(Event) + Send + 'static) -> Wifi {
        let (to_pico, lines) = mpsc::channel();
        let stop = Arc::new(AtomicBool::new(false));
        let running = stop.clone();
        std::thread::spawn(move || {
            while !running.load(Ordering::Relaxed) {
                for host in HOSTS {
                    if let Some(socket) = connect(host) {
                        on_event(Event::Open);
                        talk(socket, &lines, &on_event, &running);
                        on_event(Event::Closed);
                        break;
                    }
                }
                std::thread::sleep(Duration::from_secs(1));
            }
        });
        Wifi { to_pico, stop }
    }

    pub fn send(&self, line: &str) {
        let _ = self.to_pico.send(line.to_owned());
    }
}

impl Drop for Wifi {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

fn connect(host: &str) -> Option<WebSocket<TcpStream>> {
    let fail = |what: &str, e: &dyn std::fmt::Debug| crate::debug(&format!("wifi: {host}: {what}: {e:?}"));
    let address = match (host, 80).to_socket_addrs() {
        Ok(mut found) => found.find(|a| a.is_ipv4())?,
        Err(e) => { fail("no address", &e); return None; }
    };
    let stream = match TcpStream::connect_timeout(&address, Duration::from_secs(2)) {
        Ok(stream) => stream,
        Err(e) => { fail("connect", &e); return None; }
    };
    stream.set_read_timeout(Some(Duration::from_secs(2))).ok()?;
    let (socket, _) = match tungstenite::client(format!("ws://{host}/ws"), stream) {
        Ok(done) => done,
        Err(e) => { fail("handshake", &e); return None; }
    };
    crate::debug(&format!("wifi: {host}: open"));
    // Short reads from here on, so the lines going out don't wait on the lines coming in.
    socket.get_ref().set_read_timeout(Some(Duration::from_millis(20))).ok()?;
    socket.get_ref().set_nodelay(true).ok()?;
    Some(socket)
}

// Until the link fails, or the Pico says nothing for 5 s (it sends its load every second).
fn talk(mut socket: WebSocket<TcpStream>, lines: &Receiver<String>, on_event: &impl Fn(Event), stop: &AtomicBool) {
    while lines.try_recv().is_ok() {} // what the page sent while there was no link is stale
    let mut quiet = Duration::ZERO;
    while !stop.load(Ordering::Relaxed) && quiet < Duration::from_secs(5) {
        while let Ok(line) = lines.try_recv() {
            if socket.send(Message::text(line)).is_err() {
                return;
            }
        }
        match socket.read() {
            Ok(Message::Text(text)) => {
                quiet = Duration::ZERO;
                on_event(Event::Line(text.to_string()));
            }
            Ok(Message::Close(_)) => return,
            Ok(_) => quiet = Duration::ZERO,
            Err(tungstenite::Error::Io(e))
                if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) =>
            {
                quiet += Duration::from_millis(20);
            }
            Err(_) => return,
        }
    }
    let _ = socket.close(None);
}
