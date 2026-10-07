// The knob's controls over WiFi on Windows (input.rs is the Mac's): the HID report USB would have
// carried -- the whole keyboard / mouse / media state each time -- turned here into the SendInput
// events that state change means. Keys go as scan codes, the way a keyboard sends them, so they
// mean the same key whatever the layout and games read them too. Windows asks no permission; it
// only keeps input away from windows running as administrator (UIPI) unless this app is too.

use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, INPUT_MOUSE, KEYBDINPUT, KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP,
    KEYEVENTF_SCANCODE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP,
    MOUSEEVENTF_MOVE, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_WHEEL, MOUSEINPUT, MOUSE_EVENT_FLAGS,
    VK_MEDIA_NEXT_TRACK, VK_MEDIA_PLAY_PAUSE, VK_MEDIA_PREV_TRACK, VK_MEDIA_STOP, VK_NUMLOCK, VK_PAUSE,
    VK_VOLUME_DOWN, VK_VOLUME_MUTE, VK_VOLUME_UP,
};

pub const TAG_HID: u8 = 0xC9;
const KEYBOARD: u8 = 1;
const MOUSE: u8 = 2;
const CONSUMER: u8 = 3;

// A key as Windows takes it: a set-1 scan code (EXT: the E0-prefixed ones), or a virtual key for
// the few whose scan codes SendInput can't say (Pause's is a sequence; Num Lock's is Pause's).
#[derive(Clone, Copy)]
enum Key {
    Scan(u16),
    Vk(u16),
}

const EXT: u16 = 0xE000;

// HID keyboard usage -> the key; None = no such key here.
fn key(usage: u8) -> Option<Key> {
    const LETTERS: [u16; 26] = [
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, // a-m
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, // n-z
    ];
    const KP1_9: [u16; 9] = [0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49];
    const F13_24: [u16; 12] = [0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76];
    Some(Key::Scan(match usage {
        0x04..=0x1D => LETTERS[(usage - 0x04) as usize],
        0x1E..=0x26 => (usage - 0x1E + 0x02) as u16, // 1-9
        0x27 => 0x0B,                                // 0
        0x28 => 0x1C,                                // enter
        0x29 => 0x01,                                // escape
        0x2A => 0x0E,                                // backspace
        0x2B => 0x0F,                                // tab
        0x2C => 0x39,                                // space
        0x2D => 0x0C,                                // -
        0x2E => 0x0D,                                // =
        0x2F => 0x1A,                                // [
        0x30 => 0x1B,                                // ]
        0x31 | 0x32 => 0x2B,                         // backslash
        0x33 => 0x27,                                // ;
        0x34 => 0x28,                                // '
        0x35 => 0x29,                                // `
        0x36 => 0x33,                                // ,
        0x37 => 0x34,                                // .
        0x38 => 0x35,                                // /
        0x39 => 0x3A,                                // caps lock
        0x3A..=0x43 => (usage - 0x3A + 0x3B) as u16, // F1-F10
        0x44 => 0x57,                                // F11
        0x45 => 0x58,                                // F12
        0x46 => EXT | 0x37,                          // print screen
        0x47 => 0x46,                                // scroll lock
        0x48 => return Some(Key::Vk(VK_PAUSE)),
        0x49 => EXT | 0x52, // insert
        0x4A => EXT | 0x47, // home
        0x4B => EXT | 0x49, // page up
        0x4C => EXT | 0x53, // delete
        0x4D => EXT | 0x4F, // end
        0x4E => EXT | 0x51, // page down
        0x4F => EXT | 0x4D, // right
        0x50 => EXT | 0x4B, // left
        0x51 => EXT | 0x50, // down
        0x52 => EXT | 0x48, // up
        0x53 => return Some(Key::Vk(VK_NUMLOCK)),
        0x54 => EXT | 0x35, // keypad /
        0x55 => 0x37,       // keypad *
        0x56 => 0x4A,       // keypad -
        0x57 => 0x4E,       // keypad +
        0x58 => EXT | 0x1C, // keypad enter
        0x59..=0x61 => KP1_9[(usage - 0x59) as usize],
        0x62 => 0x52,       // keypad 0
        0x63 => 0x53,       // keypad .
        0x64 => 0x56,       // ISO section
        0x65 => EXT | 0x5D, // application (menu)
        0x67 => 0x59,       // keypad =
        0x68..=0x73 => F13_24[(usage - 0x68) as usize],
        _ => return None,
    }))
}

// The HID modifier bits (left ctrl, shift, alt, gui, then the right ones) as keys.
const MOD_KEYS: [u16; 8] = [0x1D, 0x2A, 0x38, EXT | 0x5B, EXT | 0x1D, 0x36, EXT | 0x38, EXT | 0x5C];

// Consumer usage (media keys) -> virtual key. Brightness, fast forward and rewind have none.
fn media_key(usage: u16) -> Option<u16> {
    Some(match usage {
        0xE9 => VK_VOLUME_UP,
        0xEA => VK_VOLUME_DOWN,
        0xE2 => VK_VOLUME_MUTE,
        0xCD => VK_MEDIA_PLAY_PAUSE,
        0xB5 => VK_MEDIA_NEXT_TRACK,
        0xB6 => VK_MEDIA_PREV_TRACK,
        0xB7 => VK_MEDIA_STOP,
        _ => return None,
    })
}

fn send(inputs: &[INPUT]) {
    if !inputs.is_empty() {
        // SAFETY: a slice of initialised INPUTs and its length; SendInput only reads them.
        unsafe { SendInput(inputs.len() as u32, inputs.as_ptr(), std::mem::size_of::<INPUT>() as i32) };
    }
}

fn key_input(k: Key, down: bool) -> INPUT {
    let up = if down { 0 } else { KEYEVENTF_KEYUP };
    let ki = match k {
        Key::Scan(s) => KEYBDINPUT {
            wVk: 0,
            wScan: s & 0xFF,
            dwFlags: KEYEVENTF_SCANCODE | up | if s & EXT != 0 { KEYEVENTF_EXTENDEDKEY } else { 0 },
            time: 0,
            dwExtraInfo: 0,
        },
        Key::Vk(vk) => KEYBDINPUT { wVk: vk, wScan: 0, dwFlags: up, time: 0, dwExtraInfo: 0 },
    };
    INPUT { r#type: INPUT_KEYBOARD, Anonymous: INPUT_0 { ki } }
}

fn mouse_input(flags: MOUSE_EVENT_FLAGS, dx: i32, dy: i32, data: i32) -> INPUT {
    let mi = MOUSEINPUT { dx, dy, mouseData: data as u32, dwFlags: flags, time: 0, dwExtraInfo: 0 };
    INPUT { r#type: INPUT_MOUSE, Anonymous: INPUT_0 { mi } }
}

pub struct Input {
    mods: u8,
    keys: [u8; 6],
    buttons: u8,
    media: u16,
}

impl Input {
    pub fn new() -> Self {
        Input { mods: 0, keys: [0; 6], buttons: 0, media: 0 }
    }

    pub fn holding(&self) -> bool {
        self.mods != 0 || self.keys.iter().any(|&k| k != 0) || self.buttons != 0 || self.media != 0
    }

    pub fn report(&mut self, r: &[u8]) {
        match r[1] {
            KEYBOARD => self.keyboard(r[2], r[3..9].try_into().unwrap()),
            MOUSE => self.mouse(r[2], r[3] as i8, r[4] as i8, r[5] as i8),
            CONSUMER => self.consumer(u16::from_le_bytes([r[2], r[3]])),
            _ => {}
        }
    }

    // The link went, or went quiet with something held: nothing stays down on this PC.
    pub fn release_all(&mut self) {
        if self.holding() {
            self.keyboard(0, [0; 6]);
            self.mouse(0, 0, 0, 0);
            self.consumer(0);
        }
    }

    fn keyboard(&mut self, mods: u8, keys: [u8; 6]) {
        // Keys let go first, then the modifiers, then the new keys: a chord's order, the way a
        // keyboard sends it. One SendInput, so nothing typed meanwhile lands in the middle.
        let mut out = Vec::new();
        for &k in self.keys.iter().filter(|&&k| k != 0 && !keys.contains(&k)) {
            out.extend(key(k).map(|k| key_input(k, false)));
        }
        for bit in 0..8 {
            let (was, now) = (self.mods >> bit & 1, mods >> bit & 1);
            if was != now {
                out.push(key_input(Key::Scan(MOD_KEYS[bit]), now == 1));
            }
        }
        for &k in keys.iter().filter(|&&k| k != 0 && !self.keys.contains(&k)) {
            out.extend(key(k).map(|k| key_input(k, true)));
        }
        send(&out);
        self.mods = mods;
        self.keys = keys;
    }

    fn mouse(&mut self, buttons: u8, dx: i8, dy: i8, wheel: i8) {
        const BUTTONS: [(u8, MOUSE_EVENT_FLAGS, MOUSE_EVENT_FLAGS); 3] = [
            (1, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP),
            (2, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP),
            (4, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP),
        ];
        let mut out = Vec::new();
        for (bit, down, up) in BUTTONS {
            if (self.buttons ^ buttons) & bit != 0 {
                out.push(mouse_input(if buttons & bit != 0 { down } else { up }, 0, 0, 0));
            }
        }
        self.buttons = buttons;
        if dx != 0 || dy != 0 {
            // Relative, like a mouse: pointer speed and acceleration apply, and a 3D app reading
            // raw input sees the movement too.
            out.push(mouse_input(MOUSEEVENTF_MOVE, dx as i32, dy as i32, 0));
        }
        if wheel != 0 {
            // One detent is 120 (WHEEL_DELTA); up is positive here as in HID.
            out.push(mouse_input(MOUSEEVENTF_WHEEL, 0, 0, wheel as i32 * 120));
        }
        send(&out);
    }

    fn consumer(&mut self, usage: u16) {
        if usage == self.media {
            return;
        }
        let mut out = Vec::new();
        if let Some(vk) = media_key(self.media) {
            out.push(key_input(Key::Vk(vk), false));
        }
        if let Some(vk) = media_key(usage) {
            out.push(key_input(Key::Vk(vk), true));
        }
        send(&out);
        self.media = usage;
    }
}

// No permission to ask for on Windows.
pub fn trusted(_prompt: bool) -> bool {
    true
}
