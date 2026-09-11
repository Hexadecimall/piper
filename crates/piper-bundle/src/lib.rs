//! Link inputs embedded in the compiler.
//!
//! Every archive and CRT object the linker needs, for every target, is built
//! into the executable. Compiling for a target unpacks that target's files into
//! a cache directory the first time they are needed and hands the linker a
//! sysroot pointing at them, so no cross toolchain has to be installed.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

/// One embedded file: which target it belongs to, and where it sits inside
/// that target's sysroot.
pub struct Asset {
    pub target: &'static str,
    pub path: &'static str,
    pub bytes: &'static [u8],
}

include!(concat!(env!("OUT_DIR"), "/assets.rs"));

struct StdlibAsset {
    name: &'static str,
    start: usize,
    len: usize,
    package: bool,
}

static STDLIB_BYTES: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/stdlib.bin"));
include!(concat!(env!("OUT_DIR"), "/stdlib.rs"));

/// Targets with embedded link inputs, in the order they were bundled.
pub fn targets() -> Vec<&'static str> {
    let mut v: Vec<&'static str> = Vec::new();
    for a in ASSETS { if !v.contains(&a.target) { v.push(a.target); } }
    v
}

/// Whether anything is embedded for `target`.
pub fn has(target: &str) -> bool { ASSETS.iter().any(|a| a.target == target) }

/// Total embedded size for `target`, in bytes.
pub fn size_of(target: &str) -> u64 {
    ASSETS.iter().filter(|a| a.target == target).map(|a| a.bytes.len() as u64).sum()
}

/// Python source modules carried by every Piper build.
pub fn stdlib_source(name: &str) -> Option<(&'static str, bool)> {
    // These modules are initialized by the runtime. Compiling a source module
    // with the same name would replace its target-specific implementation.
    if matches!(name, "_abc" | "_types" | "_weakref" | "atexit" | "builtins" | "errno" | "math" | "string.templatelib" | "sys" | "time" | "typing") {
        return None;
    }
    let compatibility = match name {
        "_random" => Some((include_str!("../stdlib/_random.py"), false)),
        "_ast" => Some((include_str!("../stdlib/_ast.py"), false)),
        "_codecs" => Some((include_str!("../stdlib/_codecs.py"), false)),
        "_collections" => Some((include_str!("../stdlib/_collections.py"), false)),
        "_contextvars" => Some((include_str!("../stdlib/_contextvars.py"), false)),
        "_imp" => Some((include_str!("../stdlib/_imp.py"), false)),
        "_hashlib" => Some((include_str!("../stdlib/_hashlib.py"), false)),
        "_opcode" => Some((include_str!("../stdlib/_opcode.py"), false)),
        "_csv" => Some((include_str!("../stdlib/_csv.py"), false)),
        "_sre" => Some((include_str!("../stdlib/_sre.py"), false)),
        "_struct" => Some((include_str!("../stdlib/_struct.py"), false)),
        "_string" => Some((include_str!("../stdlib/_string.py"), false)),
        "_thread" => Some((include_str!("../stdlib/_thread.py"), false)),
        "_tokenize" => Some((include_str!("../stdlib/_tokenize.py"), false)),
        "abc" => Some((include_str!("../stdlib/abc.py"), false)),
        "bisect" => Some((include_str!("../stdlib/bisect.py"), false)),
        "binascii" => Some((include_str!("../stdlib/binascii.py"), false)),
        "colorsys" => Some((include_str!("../stdlib/colorsys.py"), false)),
        "collections.abc" => Some((include_str!("../stdlib/collections_abc.py"), false)),
        "copyreg" => Some((include_str!("../stdlib/copyreg.py"), false)),
        "dataclasses" => Some((include_str!("../stdlib/dataclasses.py"), false)),
        "decimal" => Some((include_str!("../stdlib/decimal.py"), false)),
        "enum" => Some((include_str!("../stdlib/enum.py"), false)),
        "heapq" => Some((include_str!("../stdlib/heapq.py"), false)),
        "hashlib" => Some((include_str!("../stdlib/hashlib.py"), false)),
        "itertools" => Some((include_str!("../stdlib/itertools.py"), false)),
        "importlib" => Some((include_str!("../stdlib/importlib_init.py"), true)),
        "importlib.machinery" => Some((include_str!("../stdlib/importlib_machinery.py"), false)),
        "io" => Some((include_str!("../stdlib/io.py"), false)),
        "keyword" => Some((include_str!("../stdlib/keyword.py"), false)),
        "operator" => Some((include_str!("../stdlib/operator.py"), false)),
        "re._compiler" => Some((include_str!("../stdlib/re_compiler.py"), false)),
        "stat" => Some((include_str!("../stdlib/stat.py"), false)),
        "types" => Some((include_str!("../stdlib/types.py"), false)),
        _ => None,
    };
    if compatibility.is_some() { return compatibility; }
    let asset = STDLIB.binary_search_by_key(&name, |asset| asset.name).ok().map(|index| &STDLIB[index])?;
    let bytes = &STDLIB_BYTES[asset.start..asset.start + asset.len];
    Some((std::str::from_utf8(bytes).expect("bundled standard library is UTF-8"), asset.package))
}

/// Number of source modules available in the complete bundled library and
/// Piper's compatibility layer.
pub fn stdlib_module_count() -> usize {
    let compatibility = ["_ast", "_codecs", "_collections", "_contextvars", "_csv", "_hashlib", "_imp", "_opcode", "_random", "_sre", "_string", "_struct", "_thread", "_tokenize", "abc", "binascii", "bisect", "collections.abc", "colorsys", "copyreg", "dataclasses", "decimal", "enum", "hashlib", "heapq", "importlib", "importlib.machinery", "io", "itertools", "keyword", "operator", "re._compiler", "stat", "types"];
    STDLIB.len() + compatibility.iter().filter(|name| STDLIB.binary_search_by(|asset| asset.name.cmp(name)).is_err()).count()
}

/// Unpack `target`'s link inputs and return the sysroot holding them, or
/// `None` when nothing is embedded for it.
pub fn sysroot(target: &str) -> Option<PathBuf> {
    let assets: Vec<&Asset> = ASSETS.iter().filter(|a| a.target == target).collect();
    if assets.is_empty() { return None; }
    let root = cache_dir()?.join("sysroots").join(target);
    for a in assets {
        if let Err(e) = ensure(&root.join(a.path), a.bytes) {
            eprintln!("piper: cannot unpack the bundled sysroot for {target}: {e}");
            return None;
        }
    }
    Some(root)
}

/// The runtime archive for `target`, unpacked from the embedded copy.
pub fn runtime_archive(target: &str) -> Option<PathBuf> {
    let name = "lib/libpiper_rt.a";
    let a = ASSETS.iter().find(|a| a.target == target && a.path == name)?;
    let p = cache_dir()?.join("sysroots").join(target).join(name);
    ensure(&p, a.bytes).ok()?;
    Some(p)
}

/// Write `bytes` to `path` unless a file of exactly that size is already
/// there. The write goes to a neighbouring temporary first, so a reader never
/// sees a half-written archive and two compilers can race safely.
fn ensure(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Ok(m) = std::fs::metadata(path) {
        if m.is_file() && m.len() == bytes.len() as u64 && std::fs::read(path).is_ok_and(|current| current == bytes) { return Ok(()); }
    }
    let dir = path.parent().expect("asset paths have a parent");
    std::fs::create_dir_all(dir)?;
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let tmp = dir.join(format!(".{}.{}.{}", path.file_name().unwrap().to_string_lossy(), std::process::id(), sequence));
    {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    match std::fs::rename(&tmp, path) {
        Ok(()) => Ok(()),
        Err(e) => { let _ = std::fs::remove_file(&tmp); Err(e) }
    }
}

/// Where unpacked files live. Versioned, so an upgraded compiler never reads
/// an older build's archives.
pub fn cache_dir() -> Option<PathBuf> {
    let base = if let Ok(v) = std::env::var("PIPER_CACHE_DIR") { PathBuf::from(v) }
        else if let Ok(v) = std::env::var("XDG_CACHE_HOME") { PathBuf::from(v).join("piper") }
        else { PathBuf::from(std::env::var("HOME").ok()?).join(".cache/piper") };
    Some(base.join(env!("CARGO_PKG_VERSION")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn complete_python_library_is_indexed() {
        assert!(stdlib_module_count() > 1_800);
        assert!(stdlib_source("os").is_some());
        assert!(stdlib_source("email.message").is_some());
        assert!(stdlib_source("http.server").is_some());
        assert!(stdlib_source("unittest.mock").is_some());
        assert!(stdlib_source("xml.etree.ElementTree").is_some());
    }
}
