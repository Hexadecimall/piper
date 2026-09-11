//! Cross-compile one program for every target this build can satisfy and check
//! that the output really is that platform's executable format. Targets whose
//! runtime archive or sysroot is missing are skipped, so the test passes on a
//! machine with no cross toolchains installed.

use piper_llvm::target::{Os, Sysroot, Target};
use std::path::Path;

/// Targets worth trying. Each is skipped unless both a runtime archive and a
/// sysroot are available for it.
const TARGETS: &[&str] = &[
    "aarch64-apple-macosx11.0.0",
    "x86_64-apple-macosx11.0.0",
    "x86_64-unknown-linux-musl",
    "aarch64-unknown-linux-musl",
    "x86_64-unknown-linux-gnu",
    "aarch64-unknown-linux-gnu",
    "x86_64-pc-windows-gnu",
    "aarch64-pc-windows-gnu",
];

/// Check the leading bytes identify an executable of the target's format.
fn check_format(target: &Target, bytes: &[u8]) -> Result<(), String> {
    match target.os {
        // 64-bit Mach-O, little endian.
        Os::MacOs => {
            let want: &[u8] = &[0xcf, 0xfa, 0xed, 0xfe];
            if bytes.starts_with(want) { Ok(()) } else { Err(format!("not a 64-bit Mach-O: magic {:02x?}", head(bytes))) }
        }
        Os::Linux => {
            if bytes.starts_with(b"\x7fELF") { Ok(()) } else { Err(format!("not an ELF: magic {:02x?}", head(bytes))) }
        }
        // A PE starts with a DOS stub whose header points at the PE signature.
        Os::Windows => {
            if !bytes.starts_with(b"MZ") { return Err(format!("no DOS stub: magic {:02x?}", head(bytes))); }
            let at = u32::from_le_bytes(bytes.get(0x3c..0x40).ok_or("truncated DOS header")?.try_into().unwrap()) as usize;
            let sig = bytes.get(at..at + 4).ok_or_else(|| format!("PE signature offset {at:#x} is past the end"))?;
            if sig != b"PE\0\0" { return Err(format!("no PE signature at {at:#x}: {sig:02x?}")); }
            Ok(())
        }
    }
}

fn head(bytes: &[u8]) -> &[u8] { &bytes[..bytes.len().min(4)] }

#[test]
fn cross_targets_produce_native_executables() {
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/programs/hello.py");
    let work = std::env::temp_dir().join(format!("piper-cross-{}", std::process::id()));
    std::fs::create_dir_all(&work).unwrap();

    let mut tried = 0;
    let mut failures = Vec::new();
    for name in TARGETS {
        let target = match Target::parse(name) {
            Ok(t) => t,
            Err(e) => { failures.push(format!("{name}: {e}")); continue }
        };
        if let Err(e) = piper::runtime_archive_for(&target, None) { eprintln!("skipped {name}: {e}"); continue }
        if Sysroot::find(&target, None).is_none() { eprintln!("skipped {name}: no sysroot"); continue }

        let out = work.join(target.exe_name(&target.dir_name()));
        let opts = piper::CompileOptions { triple: Some(target.triple.clone()), ..Default::default() };
        tried += 1;
        match piper::compile_file(&src, &out, &opts) {
            Err(e) => failures.push(format!("{name}: {e}")),
            Ok(()) => match std::fs::read(&out) {
                Err(e) => failures.push(format!("{name}: cannot read the output: {e}")),
                Ok(bytes) => {
                    if let Err(e) = check_format(&target, &bytes) { failures.push(format!("{name}: {e}")); }
                }
            },
        }
    }
    let _ = std::fs::remove_dir_all(&work);

    if !failures.is_empty() { panic!("{} of {tried} targets failed:\n\n{}", failures.len(), failures.join("\n\n")); }
    if tried == 0 { eprintln!("skipped: no target has both a runtime and a sysroot"); return; }
    eprintln!("checked {tried} targets");
}
