// The Synths page's monitor: what the knob sends on USB MIDI, read from its port here (CoreMIDI,
// which lets every program open a port). Only in MIDI mode does the knob have one, so while the
// page asks for it a thread looks for it every second; each message goes to the UI as a
// `midi-msg` event, and `midi-port` says when the port comes or goes.

use midir::{Ignore, MidiInput, MidiInputConnection};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, State};

const PORT_NAME: &str = "Quadra";

#[derive(Default)]
pub struct Midi {
    generation: Arc<AtomicU64>,
}

#[derive(serde::Serialize, Clone)]
struct Msg {
    us: u64, // CoreMIDI's timestamp
    bytes: Vec<u8>,
}

fn find_port(input: &MidiInput) -> Option<(midir::MidiInputPort, String)> {
    input.ports().into_iter().find_map(|p| {
        let name = input.port_name(&p).ok()?;
        name.contains(PORT_NAME).then_some((p, name))
    })
}

#[tauri::command]
pub fn midi_watch(on: bool, midi: State<Midi>, app: AppHandle) {
    let gen = midi.generation.fetch_add(1, Ordering::SeqCst) + 1;
    if !on {
        return;
    }
    let generation = midi.generation.clone();
    thread::spawn(move || {
        let mut conn: Option<(MidiInputConnection<()>, String)> = None;
        while generation.load(Ordering::SeqCst) == gen {
            let Ok(probe) = MidiInput::new("Quadra companion probe") else { break };
            let present = find_port(&probe).map(|(_, n)| n);
            match (&conn, present) {
                (Some((_, name)), Some(now)) if *name == now => {}
                (_, Some(_)) => {
                    conn = None;
                    if let Ok(mut input) = MidiInput::new("Quadra companion") {
                        input.ignore(Ignore::ActiveSense);
                        if let Some((port, name)) = find_port(&input) {
                            let a = app.clone();
                            if let Ok(c) = input.connect(&port, "quadra-monitor", move |us, bytes, _| {
                                let _ = a.emit("midi-msg", Msg { us, bytes: bytes.to_vec() });
                            }, ()) {
                                let _ = app.emit("midi-port", name.clone());
                                conn = Some((c, name));
                            }
                        }
                    }
                }
                (Some(_), None) => {
                    conn = None;
                    let _ = app.emit("midi-port", String::new());
                }
                (None, None) => {}
            }
            thread::sleep(Duration::from_secs(1));
        }
        drop(conn);
    });
}

// The port's name now ("" = none), without waiting for the watcher.
#[tauri::command]
pub fn midi_port() -> String {
    MidiInput::new("Quadra companion probe").ok().and_then(|i| find_port(&i)).map(|(_, n)| n).unwrap_or_default()
}
