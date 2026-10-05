// HOME's lamp import, the parts that need the computer (src/pages/LampImport.tsx does the rest and
// sends the lamps to the knob over USB): the token extractor's list of devices, where each lamp
// answers now, which protocol it speaks, and its MIoT spec. A port of quadra.py's `home import`.
// The extractor itself (Xiaomi's login) runs in Terminal: it asks for a password or a QR scan.

use aes::cipher::{block_padding::Pkcs7, BlockDecryptMut, BlockEncryptMut, KeyIvInit};
use md5::{Digest, Md5};
use serde::Serialize;
use std::collections::HashMap;
use std::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

type Enc = cbc::Encryptor<aes::Aes128>;
type Dec = cbc::Decryptor<aes::Aes128>;

const MIIO_PORT: u16 = 54321;
const MIOT_SPEC: &str = "https://miot-spec.org/miot-spec-v2";
const HELLO: [u8; 32] = {
    let mut h = [0xffu8; 32];
    h[0] = 0x21;
    h[1] = 0x31;
    h[2] = 0x00;
    h[3] = 0x20;
    h
};

fn quadra_dir() -> Result<PathBuf, String> {
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".quadra")).ok_or_else(|| "no home folder".into())
}

#[derive(Serialize)]
pub struct XDevice {
    name: String,
    model: String,
    did: String,
    ip: String,
    token: String, // hex; goes to the knob only, never shown
}

#[derive(Serialize)]
pub struct XFile {
    path: String,
    modified_ms: u64,
    devices: Vec<XDevice>,
}

// The token extractor's output (~/.quadra/xiaomi-devices.json): every device of every home, or
// None when there's no file yet.
#[tauri::command]
pub fn home_devices() -> Result<Option<XFile>, String> {
    let path = quadra_dir()?.join("xiaomi-devices.json");
    let Ok(text) = std::fs::read_to_string(&path) else { return Ok(None) };
    let v: serde_json::Value = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", path.display()))?;
    let s = |d: &serde_json::Value, k: &str| match &d[k] {
        serde_json::Value::String(x) => x.clone(),
        serde_json::Value::Number(n) => n.to_string(),
        _ => String::new(),
    };
    let mut devices = Vec::new();
    for server in v.as_array().into_iter().flatten() {
        for home in server["homes"].as_array().into_iter().flatten() {
            for d in home["devices"].as_array().into_iter().flatten() {
                devices.push(XDevice { name: s(d, "name"), model: s(d, "model"), did: s(d, "did"), ip: s(d, "localip"), token: s(d, "token") });
            }
        }
    }
    let modified_ms = std::fs::metadata(&path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_millis() as u64);
    Ok(Some(XFile { path: path.display().to_string(), modified_ms, devices }))
}

// Where each miIO device answers now ({did: ip}): a hello to every address of the subnets the
// lamps were last seen on (a router hands out new addresses; the extractor's are the cloud's).
#[tauri::command]
pub async fn home_find(ips: Vec<String>) -> Result<HashMap<String, String>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let sock = UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
        sock.set_read_timeout(Some(Duration::from_millis(50))).map_err(|e| e.to_string())?;
        let mut nets: Vec<[u8; 3]> = ips
            .iter()
            .filter_map(|ip| ip.parse::<Ipv4Addr>().ok())
            .map(|ip| {
                let o = ip.octets();
                [o[0], o[1], o[2]]
            })
            .collect();
        nets.sort();
        nets.dedup();
        for n in &nets {
            for i in 1..=254u8 {
                let _ = sock.send_to(&HELLO, SocketAddrV4::new(Ipv4Addr::new(n[0], n[1], n[2], i), MIIO_PORT));
            }
        }
        let mut seen = HashMap::new();
        let end = Instant::now() + Duration::from_secs(2);
        let mut buf = [0u8; 1024];
        while Instant::now() < end {
            if let Ok((n, from)) = sock.recv_from(&mut buf) {
                if n == 32 {
                    let did = u32::from_be_bytes([buf[8], buf[9], buf[10], buf[11]]);
                    seen.insert(did.to_string(), from.ip().to_string());
                }
            }
        }
        Ok(seen)
    })
    .await
    .map_err(|e| e.to_string())?
}

// Just enough miIO to ask a lamp something (the knob's home.c does the rest).
struct Miio {
    sock: UdpSocket,
    addr: SocketAddrV4,
    token: [u8; 16],
    key: [u8; 16],
    iv: [u8; 16],
    did: u32,
    stamp: u32,
    t0: Instant,
}

impl Miio {
    fn hello(ip: &str, token: [u8; 16]) -> Option<Miio> {
        let addr = SocketAddrV4::new(ip.parse().ok()?, MIIO_PORT);
        let sock = UdpSocket::bind("0.0.0.0:0").ok()?;
        sock.set_read_timeout(Some(Duration::from_secs(1))).ok()?;
        sock.send_to(&HELLO, addr).ok()?;
        let mut buf = [0u8; 1024];
        let (n, _) = sock.recv_from(&mut buf).ok()?;
        if n < 32 {
            return None;
        }
        let key: [u8; 16] = Md5::digest(token).into();
        let iv: [u8; 16] = Md5::new().chain_update(key).chain_update(token).finalize().into();
        Some(Miio {
            sock,
            addr,
            token,
            key,
            iv,
            did: u32::from_be_bytes([buf[8], buf[9], buf[10], buf[11]]),
            stamp: u32::from_be_bytes([buf[12], buf[13], buf[14], buf[15]]),
            t0: Instant::now(),
        })
    }

    fn call(&self, method: &str, params: serde_json::Value, id: u32) -> Option<serde_json::Value> {
        let body = serde_json::json!({ "id": id, "method": method, "params": params }).to_string();
        let enc = Enc::new(&self.key.into(), &self.iv.into()).encrypt_padded_vec_mut::<Pkcs7>(body.as_bytes());
        let mut pkt = Vec::with_capacity(32 + enc.len());
        pkt.extend_from_slice(&0x2131u16.to_be_bytes());
        pkt.extend_from_slice(&((32 + enc.len()) as u16).to_be_bytes());
        pkt.extend_from_slice(&0u32.to_be_bytes());
        pkt.extend_from_slice(&self.did.to_be_bytes());
        pkt.extend_from_slice(&(self.stamp + self.t0.elapsed().as_secs() as u32 + 1).to_be_bytes());
        let sum = Md5::new().chain_update(&pkt[..16]).chain_update(self.token).chain_update(&enc).finalize();
        pkt.extend_from_slice(&sum);
        pkt.extend_from_slice(&enc);
        self.sock.send_to(&pkt, self.addr).ok()?;
        let mut buf = [0u8; 4096];
        let (n, _) = self.sock.recv_from(&mut buf).ok()?;
        if n <= 32 {
            return None;
        }
        let dec = Dec::new(&self.key.into(), &self.iv.into()).decrypt_padded_vec_mut::<Pkcs7>(&buf[32..n]).ok()?;
        let end = dec.iter().rposition(|&b| b != 0).map_or(0, |i| i + 1);
        serde_json::from_slice(&dec[..end]).ok()
    }
}

fn token_bytes(hex: &str) -> Option<[u8; 16]> {
    if hex.len() != 32 {
        return None;
    }
    let mut t = [0u8; 16];
    for (i, b) in t.iter_mut().enumerate() {
        *b = u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(t)
}

// Which protocol a lamp answers: 0 MIoT, 1 the older one (home.h HOME_PROTO_*), or None (no
// answer: offline). `siid` / `piid`: one of its MIoT properties, to ask for.
#[tauri::command]
pub async fn home_probe(ip: String, token: String, did: String, siid: u32, piid: u32) -> Result<Option<u8>, String> {
    let token = token_bytes(&token).ok_or("bad token")?;
    tauri::async_runtime::spawn_blocking(move || {
        let Some(m) = Miio::hello(&ip, token) else { return Ok(None) };
        if siid > 0 && m.call("get_properties", serde_json::json!([{ "did": did, "siid": siid, "piid": piid }]), 101).is_some() {
            return Ok(Some(0));
        }
        if m.call("get_prop", serde_json::json!(["power"]), 102).is_some() {
            return Ok(Some(1));
        }
        Ok(None)
    })
    .await
    .map_err(|e| e.to_string())?
}

// A model's MIoT spec (miot-spec.org), cached in ~/.quadra/miot-spec/ like quadra.py's. None: no
// spec for that model. Fetched with curl, which every Mac has.
#[tauri::command]
pub async fn miot_spec(model: String) -> Result<Option<String>, String> {
    if !model.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-') {
        return Err("bad model name".into());
    }
    let dir = quadra_dir()?.join("miot-spec");
    let path = dir.join(format!("{model}.json"));
    if let Ok(text) = std::fs::read_to_string(&path) {
        return Ok(Some(text));
    }
    tauri::async_runtime::spawn_blocking(move || {
        let get = |url: &str| -> Result<serde_json::Value, String> {
            let out = std::process::Command::new("curl").args(["-sfL", "--max-time", "30", url]).output().map_err(|e| e.to_string())?;
            if !out.status.success() {
                return Err("miot-spec.org didn't answer (no internet?)".into());
            }
            serde_json::from_slice(&out.stdout).map_err(|e| e.to_string())
        };
        let all = get(&format!("{MIOT_SPEC}/instances?status=all"))?;
        let best = all["instances"]
            .as_array()
            .into_iter()
            .flatten()
            .filter(|i| i["model"] == model.as_str())
            .max_by_key(|i| (i["status"] == "released", i["version"].as_i64().unwrap_or(0)));
        let Some(best) = best else { return Ok(None) };
        let ty = best["type"].as_str().ok_or("no type")?;
        let spec = get(&format!("{MIOT_SPEC}/instance?type={ty}"))?;
        let text = spec.to_string();
        let _ = std::fs::create_dir_all(&dir);
        let _ = std::fs::write(&path, &text);
        Ok(Some(text))
    })
    .await
    .map_err(|e| e.to_string())?
}

// The token extractor (installed in ~/.quadra/token-extractor, its own .venv) in a Terminal
// window: Xiaomi's login asks for a password or a QR scan there. It writes
// ~/.quadra/xiaomi-devices.json; the app looks again when the window says it's done.
#[tauri::command]
pub fn home_run_extractor() -> Result<(), String> {
    let q = quadra_dir()?;
    let ex = q.join("token-extractor");
    let py = ex.join(".venv/bin/python");
    if !py.exists() || !ex.join("token_extractor.py").exists() {
        return Err("The token extractor isn't installed in ~/.quadra/token-extractor (see the README's HOME section)".into());
    }
    let out = q.join("xiaomi-devices.json");
    let script = std::env::temp_dir().join("quadra-xiaomi-login.command");
    let text = format!(
        "#!/bin/zsh\n# Quadra: your Xiaomi account's devices and their keys, for the knob's HOME mode.\ncd {ex:?} || exit 1\n\
         {py:?} token_extractor.py -o {out:?} && chmod 600 {out:?} && echo && echo 'Done: go back to Quadra and press Look again.'\n"
    );
    std::fs::write(&script, text).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o700)).map_err(|e| e.to_string())?;
    }
    std::process::Command::new("open").arg(&script).status().map_err(|e| e.to_string())?;
    Ok(())
}

// A synth profile (or anything else the app exports) into ~/Downloads, under a name not taken yet.
#[tauri::command]
pub fn save_download(name: String, text: String) -> Result<String, String> {
    let name: String = name.chars().filter(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_')).collect();
    let dir = std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Downloads")).ok_or("no home folder")?;
    let (stem, ext) = name.rsplit_once('.').unwrap_or((&name, "txt"));
    let mut path = dir.join(format!("{stem}.{ext}"));
    let mut n = 2;
    while path.exists() {
        path = dir.join(format!("{stem} {n}.{ext}"));
        n += 1;
    }
    std::fs::write(&path, text).map_err(|e| e.to_string())?;
    Ok(path.display().to_string())
}
