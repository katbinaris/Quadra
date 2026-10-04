// The knob's controls over WiFi (NanoDepsidf/src/ext_proto.h, EXT_TAG_HID): the HID report USB
// would have carried -- the whole keyboard / mouse / media state each time -- turned here into
// the macOS events that state change means. Posting them needs Accessibility permission
// (`trusted`). Runs in the WiFi reader thread (lib.rs), so a key costs no trip through the UI.

use objc2_app_kit::{NSEvent, NSEventModifierFlags, NSEventType};
use objc2_core_foundation::CGPoint;
use objc2_core_graphics::{CGEvent, CGEventField, CGEventFlags, CGEventTapLocation, CGEventType, CGMouseButton, CGScrollEventUnit};
use std::ffi::c_void;

pub const TAG_HID: u8 = 0xC9;
const KEYBOARD: u8 = 1;
const MOUSE: u8 = 2;
const CONSUMER: u8 = 3;

// HID keyboard usage -> macOS virtual key code (Carbon kVK_*); 0xFF = none.
fn key_code(usage: u8) -> u16 {
    const LETTERS: [u16; 26] = [
        0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E, // a-m
        0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06, // n-z
    ];
    const DIGITS: [u16; 10] = [0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D]; // 1-9, 0
    const F1_12: [u16; 12] = [0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F];
    const KP1_9: [u16; 9] = [0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C];
    const F13_20: [u16; 8] = [0x69, 0x6B, 0x71, 0x6A, 0x40, 0x4F, 0x50, 0x5A];
    match usage {
        0x04..=0x1D => LETTERS[(usage - 0x04) as usize],
        0x1E..=0x27 => DIGITS[(usage - 0x1E) as usize],
        0x28 => 0x24, // return
        0x29 => 0x35, // escape
        0x2A => 0x33, // backspace
        0x2B => 0x30, // tab
        0x2C => 0x31, // space
        0x2D => 0x1B, // -
        0x2E => 0x18, // =
        0x2F => 0x21, // [
        0x30 => 0x1E, // ]
        0x31 | 0x32 => 0x2A, // backslash
        0x33 => 0x29, // ;
        0x34 => 0x27, // '
        0x35 => 0x32, // `
        0x36 => 0x2B, // ,
        0x37 => 0x2F, // .
        0x38 => 0x2C, // /
        0x39 => 0x39, // caps lock
        0x3A..=0x45 => F1_12[(usage - 0x3A) as usize],
        0x46 => 0x69, // print screen: F13
        0x47 => 0x6B, // scroll lock: F14
        0x48 => 0x71, // pause: F15
        0x49 => 0x72, // insert: help
        0x4A => 0x73, // home
        0x4B => 0x74, // page up
        0x4C => 0x75, // forward delete
        0x4D => 0x77, // end
        0x4E => 0x79, // page down
        0x4F => 0x7C, // right
        0x50 => 0x7B, // left
        0x51 => 0x7D, // down
        0x52 => 0x7E, // up
        0x53 => 0x47, // num lock: keypad clear
        0x54 => 0x4B, // keypad /
        0x55 => 0x43, // keypad *
        0x56 => 0x4E, // keypad -
        0x57 => 0x45, // keypad +
        0x58 => 0x4C, // keypad enter
        0x59..=0x61 => KP1_9[(usage - 0x59) as usize],
        0x62 => 0x52, // keypad 0
        0x63 => 0x41, // keypad .
        0x64 => 0x0A, // ISO section
        0x67 => 0x51, // keypad =
        0x68..=0x6F => F13_20[(usage - 0x68) as usize],
        _ => 0xFF,
    }
}

// The HID modifier bits (left ctrl, shift, alt, gui, then the right ones) as keys and flags.
const MOD_KEYS: [u16; 8] = [0x3B, 0x38, 0x3A, 0x37, 0x3E, 0x3C, 0x3D, 0x36];

fn flags(mods: u8) -> CGEventFlags {
    let m = mods | mods >> 4;
    let mut f = CGEventFlags::empty();
    if m & 1 != 0 {
        f |= CGEventFlags::MaskControl;
    }
    if m & 2 != 0 {
        f |= CGEventFlags::MaskShift;
    }
    if m & 4 != 0 {
        f |= CGEventFlags::MaskAlternate;
    }
    if m & 8 != 0 {
        f |= CGEventFlags::MaskCommand;
    }
    f
}

// Consumer usage (media keys) -> NX_KEYTYPE_* (IOKit's hidsystem/ev_keymap.h).
fn media_key(usage: u16) -> Option<i64> {
    Some(match usage {
        0xE9 => 0,   // volume up
        0xEA => 1,   // volume down
        0x6F => 2,   // brightness up
        0x70 => 3,   // brightness down
        0xE2 => 7,   // mute
        0xCD => 16,  // play / pause
        0xB5 => 17,  // next
        0xB6 => 18,  // previous
        0xB3 => 19,  // fast forward
        0xB4 => 20,  // rewind
        _ => return None,
    })
}

fn post(e: Option<objc2_core_foundation::CFRetained<CGEvent>>, mods: u8) {
    if let Some(e) = e {
        CGEvent::set_flags(Some(&e), flags(mods));
        CGEvent::post(CGEventTapLocation::HIDEventTap, Some(&e));
    }
}

fn cursor() -> CGPoint {
    let e = CGEvent::new(None);
    CGEvent::location(e.as_deref())
}

// System Settings > Trackpad / Mouse "natural scrolling": a USB wheel is turned round by the
// system, an event posted here isn't -- so it's done here, to match the knob on a cable.
fn natural_scrolling() -> bool {
    std::process::Command::new("defaults")
        .args(["read", "-g", "com.apple.swipescrolldirection"])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim() != "0")
        .unwrap_or(true) // the macOS default
}

pub struct Input {
    mods: u8,
    keys: [u8; 6],
    buttons: u8,
    media: u16,
    wheel_sign: i32,
}

impl Input {
    pub fn new() -> Self {
        Input { mods: 0, keys: [0; 6], buttons: 0, media: 0, wheel_sign: if natural_scrolling() { -1 } else { 1 } }
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

    // The link went, or went quiet with something held: nothing stays down on this Mac.
    pub fn release_all(&mut self) {
        if self.holding() {
            self.keyboard(0, [0; 6]);
            self.mouse(0, 0, 0, 0);
            self.consumer(0);
        }
    }

    fn keyboard(&mut self, mods: u8, keys: [u8; 6]) {
        // Keys let go first (under the modifiers they were pressed with), then the modifiers,
        // then the new keys: a chord's order, the way a keyboard sends it.
        for &k in self.keys.iter().filter(|&&k| k != 0 && !keys.contains(&k)) {
            if key_code(k) != 0xFF {
                post(CGEvent::new_keyboard_event(None, key_code(k), false), self.mods);
            }
        }
        for bit in 0..8 {
            let (was, now) = (self.mods >> bit & 1, mods >> bit & 1);
            if was != now {
                let m = self.mods ^ (1 << bit);
                post(CGEvent::new_keyboard_event(None, MOD_KEYS[bit], now == 1), m);
                self.mods = m;
            }
        }
        for &k in keys.iter().filter(|&&k| k != 0 && !self.keys.contains(&k)) {
            if key_code(k) != 0xFF {
                post(CGEvent::new_keyboard_event(None, key_code(k), true), mods);
            }
        }
        self.keys = keys;
    }

    fn mouse(&mut self, buttons: u8, dx: i8, dy: i8, wheel: i8) {
        let mut at = cursor();
        const BUTTONS: [(u8, CGMouseButton, CGEventType, CGEventType); 3] = [
            (1, CGMouseButton::Left, CGEventType::LeftMouseDown, CGEventType::LeftMouseUp),
            (2, CGMouseButton::Right, CGEventType::RightMouseDown, CGEventType::RightMouseUp),
            (4, CGMouseButton::Center, CGEventType::OtherMouseDown, CGEventType::OtherMouseUp),
        ];
        for (bit, button, down, up) in BUTTONS {
            if (self.buttons ^ buttons) & bit != 0 {
                post(CGEvent::new_mouse_event(None, if buttons & bit != 0 { down } else { up }, at, button), self.mods);
            }
        }
        self.buttons = buttons;
        if dx != 0 || dy != 0 {
            at.x += dx as f64;
            at.y += dy as f64;
            let (kind, button) = if buttons & 1 != 0 {
                (CGEventType::LeftMouseDragged, CGMouseButton::Left)
            } else if buttons & 2 != 0 {
                (CGEventType::RightMouseDragged, CGMouseButton::Right)
            } else if buttons & 4 != 0 {
                (CGEventType::OtherMouseDragged, CGMouseButton::Center)
            } else {
                (CGEventType::MouseMoved, CGMouseButton::Left)
            };
            if let Some(e) = CGEvent::new_mouse_event(None, kind, at, button) {
                // The deltas too: a 3D app orbiting with the pointer hidden reads only those.
                CGEvent::set_integer_value_field(Some(&e), CGEventField::MouseEventDeltaX, dx as i64);
                CGEvent::set_integer_value_field(Some(&e), CGEventField::MouseEventDeltaY, dy as i64);
                post(Some(e), self.mods);
            }
        }
        if wheel != 0 {
            post(CGEvent::new_scroll_wheel_event2(None, CGScrollEventUnit::Line, 1, wheel as i32 * self.wheel_sign, 0, 0), self.mods);
        }
    }

    fn consumer(&mut self, usage: u16) {
        if usage == self.media {
            return;
        }
        if let Some(k) = media_key(self.media) {
            self.media_event(k, false);
        }
        if let Some(k) = media_key(usage) {
            self.media_event(k, true);
        }
        self.media = usage;
    }

    // A media key is an NSSystemDefined event, subtype 8 (NX_SUBTYPE_AUX_CONTROL_BUTTONS):
    // data1 = key type << 16 | 0xA (down) or 0xB (up) << 8.
    fn media_event(&self, key: i64, down: bool) {
        let state: i64 = if down { 0xA } else { 0xB };
        let e = NSEvent::otherEventWithType_location_modifierFlags_timestamp_windowNumber_context_subtype_data1_data2(
            NSEventType::SystemDefined,
            CGPoint { x: 0.0, y: 0.0 },
            NSEventModifierFlags::from_bits_retain((state as usize) << 8 | flags(self.mods).bits() as usize),
            0.0,
            0,
            None,
            8,
            (key << 16 | state << 8) as isize,
            -1,
        );
        if let Some(cg) = e.and_then(|e| e.CGEvent()) {
            CGEvent::post(CGEventTapLocation::HIDEventTap, Some(&cg));
        }
    }
}

#[link(name = "ApplicationServices", kind = "framework")]
extern "C" {
    fn AXIsProcessTrustedWithOptions(options: *const c_void) -> bool;
    static kAXTrustedCheckOptionPrompt: *const c_void;
}

// Accessibility permission (System Settings > Privacy & Security > Accessibility): posting key
// and mouse events needs it. `prompt`: macOS asks, and lists this app there.
pub fn trusted(prompt: bool) -> bool {
    use core_foundation::base::TCFType;
    use core_foundation::boolean::CFBoolean;
    use core_foundation::dictionary::CFDictionary;
    use core_foundation::string::CFString;
    // SAFETY: kAXTrustedCheckOptionPrompt is a constant CFString from ApplicationServices; the
    // dictionary lives across the call.
    unsafe {
        let key = CFString::wrap_under_get_rule(kAXTrustedCheckOptionPrompt as _);
        let opts = CFDictionary::from_CFType_pairs(&[(key, if prompt { CFBoolean::true_value() } else { CFBoolean::false_value() })]);
        AXIsProcessTrustedWithOptions(opts.as_concrete_TypeRef() as *const c_void)
    }
}
