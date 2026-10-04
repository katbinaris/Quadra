// The Rust side is only a pipe: it finds the knob's vendor HID interface, forwards every
// 64-byte input report to the UI as a `hid-report` event, and writes the UI's 64-byte reports.
// Or the same reports over WiFi, once paired (`net-report`, below). The protocol itself lives
// in the UI (src/proto.ts), so the same UI runs over WebHID too.

use aes_gcm::aead::{AeadInPlace, KeyInit};
use aes_gcm::{Aes256Gcm, Nonce, Tag};
use hidapi::{HidApi, HidDevice};
use hmac::{Hmac, Mac};
use serde::Serialize;
use sha2::Sha256;
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpStream};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, State};

#[cfg(target_os = "macos")]
mod input;

const VENDOR_USAGE_PAGE: u16 = 0xFF00;
const VENDOR_USAGE: u16 = 0x01;
const PRODUCT: &str = "Quadra";
const REPORT_SIZE: usize = 64;

// `generation` changes on every open and close, so a reader thread left over from an earlier
// connection stops (and never clears the new one's device).
#[derive(Default)]
struct Link {
    device: Arc<Mutex<Option<HidDevice>>>,
    generation: Arc<AtomicU64>,
}

#[derive(Serialize, Clone)]
struct DeviceInfo {
    path: String,
    product: String,
    serial: String,
}

fn api() -> Result<HidApi, String> {
    HidApi::new().map_err(|e| e.to_string())
}

#[tauri::command]
fn hid_list() -> Result<Vec<DeviceInfo>, String> {
    // QUADRA_NO_USB=1: no knob on USB, so the app takes WiFi with the cable in (the cable powers it).
    if std::env::var_os("QUADRA_NO_USB").is_some() {
        return Ok(Vec::new());
    }
    let api = api()?;
    Ok(api
        .device_list()
        .filter(|d| {
            d.usage_page() == VENDOR_USAGE_PAGE
                && d.usage() == VENDOR_USAGE
                && d.product_string().map_or(false, |p| p.starts_with(PRODUCT))
        })
        .map(|d| DeviceInfo {
            path: d.path().to_string_lossy().into_owned(),
            product: d.product_string().unwrap_or_default().to_string(),
            serial: d.serial_number().unwrap_or_default().to_string(),
        })
        .collect())
}

#[tauri::command]
fn hid_open(path: String, link: State<Link>, app: AppHandle) -> Result<(), String> {
    let api = api()?;
    let cpath = std::ffi::CString::new(path).map_err(|e| e.to_string())?;
    let dev = api.open_path(&cpath).map_err(|e| e.to_string())?;
    let gen = link.generation.fetch_add(1, Ordering::SeqCst) + 1;
    *link.device.lock().unwrap() = Some(dev);

    // Reader: short timeouts so a write never waits long for the lock.
    let device = link.device.clone();
    let generation = link.generation.clone();
    thread::spawn(move || {
        let mut buf = [0u8; REPORT_SIZE];
        while generation.load(Ordering::SeqCst) == gen {
            let res = {
                let guard = device.lock().unwrap();
                match guard.as_ref() {
                    Some(d) => d.read_timeout(&mut buf, 5),
                    None => break,
                }
            };
            match res {
                Ok(0) => thread::sleep(Duration::from_millis(1)),
                Ok(n) => {
                    let _ = app.emit("hid-report", buf[..n].to_vec());
                }
                Err(e) => {
                    // Unplugged (or reset): drop this connection unless a newer one replaced it.
                    let mut guard = device.lock().unwrap();
                    if generation.load(Ordering::SeqCst) == gen {
                        *guard = None;
                        let _ = app.emit("hid-closed", e.to_string());
                    }
                    break;
                }
            }
        }
    });
    Ok(())
}

#[tauri::command]
fn hid_write(data: Vec<u8>, link: State<Link>) -> Result<(), String> {
    // hidapi wants the report ID first: 0 for an interface without report IDs.
    let mut out = [0u8; REPORT_SIZE + 1];
    let n = data.len().min(REPORT_SIZE);
    out[1..1 + n].copy_from_slice(&data[..n]);
    let guard = link.device.lock().unwrap();
    let dev = guard.as_ref().ok_or("not connected")?;
    dev.write(&out).map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
fn hid_close(link: State<Link>) {
    link.generation.fetch_add(1, Ordering::SeqCst);
    *link.device.lock().unwrap() = None;
}

// --- WiFi (NanoDepsidf/src/net_link.h) ---
// TCP to the knob, a handshake that proves both ends hold the pairing key (got over USB), then
// each report as AES-256-GCM: 64 bytes + a 16-byte tag, nonce 'C' (from here) or 'K' (from the
// knob) | 64-bit counter, little-endian | 0 0 0. A writer thread keeps the reports in order
// without the UI ever waiting on the network.

const WIRE: usize = REPORT_SIZE + 16;
const NET_IO: Duration = Duration::from_secs(3);
// The app polls the knob every second (device.ts), so it always hears back: this long without a
// word, it's gone.
const NET_SILENCE: Duration = Duration::from_secs(10);
// The reader wakes this often to look at the time (a read that times out loses nothing: frames
// are put together in its own buffer).
const NET_TICK: Duration = Duration::from_millis(250);
// A key or button held (the controls over WiFi, input.rs) and the knob this quiet: let go, so a
// lost link can't leave Cmd down on this Mac.
const INPUT_HOLD: Duration = Duration::from_millis(1500);

#[derive(Default)]
struct Net {
    tx: Mutex<Option<mpsc::Sender<[u8; REPORT_SIZE]>>>,
    stream: Mutex<Option<TcpStream>>,
    generation: Arc<AtomicU64>,
}

impl Net {
    fn close(&self) {
        self.generation.fetch_add(1, Ordering::SeqCst);
        *self.tx.lock().unwrap() = None; // the writer thread ends
        if let Some(s) = self.stream.lock().unwrap().take() {
            let _ = s.shutdown(Shutdown::Both); // ...and the reader's read fails
        }
    }
}

fn mac(key: &[u8], parts: &[&[u8]]) -> Hmac<Sha256> {
    let mut m = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC takes any key length");
    for p in parts {
        m.update(p);
    }
    m
}

fn nonce(dir: u8, seq: u64) -> [u8; 12] {
    let mut n = [0u8; 12];
    n[0] = dir;
    n[1..9].copy_from_slice(&seq.to_le_bytes());
    n
}

// `host`'s IPv4 addresses. Not ToSocketAddrs: asked for any family, macOS waits ~5 s for the
// IPv6 answer a knob's .local name never gets (it has no IPv6 address); asked for IPv4, it's ms.
fn resolve_v4(host: &str, port: u16) -> Result<Vec<SocketAddr>, String> {
    if let Ok(ip) = host.parse::<Ipv4Addr>() {
        return Ok(vec![SocketAddr::from((ip, port))]);
    }
    let name = std::ffi::CString::new(host).map_err(|e| e.to_string())?;
    // SAFETY: a zeroed addrinfo is a valid "no hints" value; getaddrinfo's list is only read
    // before freeaddrinfo, which gets exactly what getaddrinfo returned.
    unsafe {
        let mut hints: libc::addrinfo = std::mem::zeroed();
        hints.ai_family = libc::AF_INET;
        hints.ai_socktype = libc::SOCK_STREAM;
        let mut list: *mut libc::addrinfo = std::ptr::null_mut();
        if libc::getaddrinfo(name.as_ptr(), std::ptr::null(), &hints, &mut list) != 0 {
            return Err(format!("{host}: not found on the network"));
        }
        let mut out = Vec::new();
        let mut p = list;
        while !p.is_null() {
            let ai = &*p;
            if ai.ai_family == libc::AF_INET && !ai.ai_addr.is_null() {
                let sin = &*(ai.ai_addr as *const libc::sockaddr_in);
                out.push(SocketAddr::from((Ipv4Addr::from(u32::from_be(sin.sin_addr.s_addr)), port)));
            }
            p = ai.ai_next;
        }
        libc::freeaddrinfo(list);
        Ok(out)
    }
}

// The first host that answers: the knob's mDNS name, then its last known address.
fn connect(hosts: &[String], port: u16) -> Result<TcpStream, String> {
    let mut last = String::from("no address to try");
    for h in hosts {
        match resolve_v4(h, port) {
            Ok(addrs) => {
                for a in addrs {
                    match TcpStream::connect_timeout(&a, NET_IO) {
                        Ok(s) => return Ok(s),
                        Err(e) => last = format!("{h}: {e}"),
                    }
                }
            }
            Err(e) => last = e,
        }
    }
    Err(last)
}

fn handshake(s: &mut TcpStream, key: &[u8]) -> Result<Aes256Gcm, String> {
    let io = |e: std::io::Error| e.to_string();
    let mut hello = [0u8; 20];
    hello[..4].copy_from_slice(b"QDR1");
    getrandom::fill(&mut hello[4..]).map_err(|e| e.to_string())?;
    s.write_all(&hello).map_err(io)?;
    let mut reply = [0u8; 36];
    s.read_exact(&mut reply)
        .map_err(|_| "the knob turned this app away: pair again over USB".to_string())?;
    if &reply[..4] != b"QDR1" {
        return Err("not a Quadra".into());
    }
    let k = mac(key, &[b"quadra session", &hello[4..], &reply[4..20]]).finalize().into_bytes();
    mac(&k, &[b"knob"])
        .verify_truncated_left(&reply[20..36])
        .map_err(|_| "the knob holds another key: pair again over USB".to_string())?;
    let proof = mac(&k, &[b"client"]).finalize().into_bytes();
    s.write_all(&proof[..16]).map_err(io)?;
    Ok(Aes256Gcm::new_from_slice(&k).expect("32-byte key"))
}

#[tauri::command]
async fn net_open(hosts: Vec<String>, port: u16, key: Vec<u8>, net: State<'_, Net>, app: AppHandle) -> Result<(), String> {
    if key.len() != 32 {
        return Err("bad pairing key".into());
    }
    net.close();
    let (stream, cipher) = tauri::async_runtime::spawn_blocking(move || {
        let mut s = connect(&hosts, port)?;
        let _ = s.set_nodelay(true);
        s.set_read_timeout(Some(NET_IO)).map_err(|e| e.to_string())?;
        s.set_write_timeout(Some(NET_IO)).map_err(|e| e.to_string())?;
        let c = handshake(&mut s, &key)?;
        s.set_read_timeout(Some(NET_TICK)).map_err(|e| e.to_string())?;
        Ok::<_, String>((s, c))
    })
    .await
    .map_err(|e| e.to_string())??;

    let gen = net.generation.fetch_add(1, Ordering::SeqCst) + 1;
    let mut rd = stream.try_clone().map_err(|e| e.to_string())?;
    let mut wr = stream.try_clone().map_err(|e| e.to_string())?;
    let (tx, rx) = mpsc::channel::<[u8; REPORT_SIZE]>();
    *net.stream.lock().unwrap() = Some(stream);
    *net.tx.lock().unwrap() = Some(tx);

    let wcipher = cipher.clone();
    thread::spawn(move || {
        let mut seq = 0u64;
        for r in rx {
            let mut frame = [0u8; WIRE];
            frame[..REPORT_SIZE].copy_from_slice(&r);
            let Ok(tag) = wcipher.encrypt_in_place_detached(Nonce::from_slice(&nonce(b'C', seq)), b"", &mut frame[..REPORT_SIZE]) else { break };
            seq += 1;
            frame[REPORT_SIZE..].copy_from_slice(&tag);
            if wr.write_all(&frame).is_err() {
                let _ = wr.shutdown(Shutdown::Both); // the reader reports it
                break;
            }
        }
    });

    let generation = net.generation.clone();
    thread::spawn(move || {
        #[cfg(target_os = "macos")]
        let mut input = input::Input::new();
        let mut seq = 0u64;
        let mut buf: Vec<u8> = Vec::with_capacity(WIRE * 16);
        let mut chunk = [0u8; 2048];
        let mut heard = Instant::now();
        let why = 'read: loop {
            match rd.read(&mut chunk) {
                Ok(0) => break "closed by the knob".to_string(),
                Ok(n) => {
                    buf.extend_from_slice(&chunk[..n]);
                    heard = Instant::now();
                }
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
                Err(e) => break e.to_string(),
            }
            let mut at = 0;
            while buf.len() - at >= WIRE {
                let mut frame = [0u8; WIRE];
                frame.copy_from_slice(&buf[at..at + WIRE]);
                at += WIRE;
                let (body, tag) = frame.split_at_mut(REPORT_SIZE);
                if cipher.decrypt_in_place_detached(Nonce::from_slice(&nonce(b'K', seq)), b"", body, Tag::from_slice(tag)).is_err() {
                    break 'read "a frame from the knob didn't verify".into();
                }
                seq += 1;
                if generation.load(Ordering::SeqCst) != gen {
                    #[cfg(target_os = "macos")]
                    input.release_all();
                    return;
                }
                #[cfg(target_os = "macos")]
                if body[0] == input::TAG_HID {
                    input.report(body);
                    continue;
                }
                let _ = app.emit("net-report", body.to_vec());
            }
            buf.drain(..at);
            let quiet = heard.elapsed();
            #[cfg(target_os = "macos")]
            if quiet > INPUT_HOLD && input.holding() {
                input.release_all();
            }
            if quiet > NET_SILENCE {
                break "no word from the knob".into();
            }
        };
        #[cfg(target_os = "macos")]
        input.release_all();
        let _ = rd.shutdown(Shutdown::Both);
        if generation.load(Ordering::SeqCst) == gen {
            let _ = app.emit("net-closed", why);
        }
    });
    Ok(())
}

#[tauri::command]
fn net_write(data: Vec<u8>, net: State<Net>) -> Result<(), String> {
    let mut r = [0u8; REPORT_SIZE];
    let n = data.len().min(REPORT_SIZE);
    r[..n].copy_from_slice(&data[..n]);
    let guard = net.tx.lock().unwrap();
    guard.as_ref().ok_or("not connected")?.send(r).map_err(|_| "not connected".to_string())
}

#[tauri::command]
fn net_close(net: State<Net>) {
    net.close();
}

// The controls over WiFi need Accessibility permission (input.rs). `prompt`: macOS asks, and
// lists the app in System Settings.
#[tauri::command]
fn input_trusted(prompt: bool) -> bool {
    #[cfg(target_os = "macos")]
    return input::trusted(prompt);
    #[cfg(not(target_os = "macos"))]
    {
        let _ = prompt;
        false
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(Link::default())
        .manage(Net::default())
        .invoke_handler(tauri::generate_handler![hid_list, hid_open, hid_write, hid_close, net_open, net_write, net_close, input_trusted])
        .run(tauri::generate_context!())
        .expect("error while running the Quadra companion");
}
