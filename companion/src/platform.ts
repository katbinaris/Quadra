// Which computer the app (or the web page) runs on, for the words it shows: keys, "this Mac" or
// "this PC". Only Windows changes anything; everywhere else the Mac's words stay.

export const isWindows = typeof navigator !== "undefined" && /Windows/.test(navigator.userAgent);

// "this Mac" / "this PC", for copy.
export const thisComputer = isWindows ? "this PC" : "this Mac";

// The service that sends the knob the time, the music cover and agent requests.
export const serviceName = isWindows ? "the Quadra service" : "the Mac service";

// Modifier names, in the order the Mac shows them. Profiles are written with Mac shortcuts, and
// with BINDINGS = PC the knob sends Ctrl where a profile says Cmd (usb_task.c host_modifier): so
// on Windows Cmd reads as the Ctrl it becomes.
export const MOD_WORDS = isWindows
  ? { ctrl: ["Ctrl", "Ctrl"], alt: ["Alt", "Alt"], shift: ["Shift", "Shift"], gui: ["Ctrl", "Cmd in a Mac profile: sent as Ctrl on a PC"] }
  : { ctrl: ["⌃", "Control"], alt: ["⌥", "Option"], shift: ["⇧", "Shift"], gui: ["⌘", "Command"] };
