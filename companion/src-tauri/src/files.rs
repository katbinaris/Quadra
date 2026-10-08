// Export and import (src/files.ts): the text of a file the user picked in a Save or Open dialog
// (tauri-plugin-dialog), so the app needs no file-system scope of its own.

// Backups are tens of KB; anything this big isn't one.
const MAX_READ: u64 = 4 * 1024 * 1024;

#[tauri::command]
pub fn write_text(path: String, text: String) -> Result<(), String> {
    std::fs::write(&path, text).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn read_text(path: String) -> Result<String, String> {
    let len = std::fs::metadata(&path).map_err(|e| e.to_string())?.len();
    if len > MAX_READ {
        return Err("That file is too big to be a Quadra file".into());
    }
    std::fs::read_to_string(&path).map_err(|e| e.to_string())
}
