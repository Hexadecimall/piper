//! Unicode character names for `\\N{...}` escapes, generated from CPython 3.14's
//! `unicodedata`. Packed as `u32 code point, u8 length, ASCII name` records.
//! Names with algorithmic forms (CJK ideographs and similar) are computed
//! in `lookup` instead of stored.

use std::collections::HashMap;
use std::sync::OnceLock;

static DATA: &[u8] = include_bytes!("unicode_names.bin");

static ALGORITHMIC: &[&str] = &["CJK UNIFIED IDEOGRAPH-", "CJK COMPATIBILITY IDEOGRAPH-", "TANGUT IDEOGRAPH-", "KHITAN SMALL SCRIPT CHARACTER-", "NUSHU CHARACTER-", "EGYPTIAN HIEROGLYPH-"];

fn table() -> &'static HashMap<&'static str, char> {
    static T: OnceLock<HashMap<&'static str, char>> = OnceLock::new();
    T.get_or_init(|| {
        let mut m = HashMap::with_capacity(DATA.len() / 24);
        let mut i = 0;
        while i + 5 <= DATA.len() {
            let cp = u32::from_le_bytes([DATA[i], DATA[i + 1], DATA[i + 2], DATA[i + 3]]);
            let len = DATA[i + 4] as usize;
            i += 5;
            let name = std::str::from_utf8(&DATA[i..i + len]).unwrap();
            i += len;
            if let Some(c) = char::from_u32(cp) { m.insert(name, c); }
        }
        m
    })
}

/// Look up a character by its Unicode name (case-insensitive, like CPython).
pub fn lookup(name: &str) -> Option<char> {
    let upper = name.to_ascii_uppercase();
    if let Some(c) = table().get(upper.as_str()) { return Some(*c); }
    for prefix in ALGORITHMIC {
        if let Some(hex) = upper.strip_prefix(prefix) {
            if (4..=5).contains(&hex.len()) && hex.bytes().all(|b| b.is_ascii_hexdigit()) {
                return char::from_u32(u32::from_str_radix(hex, 16).ok()?);
            }
        }
    }
    None
}
