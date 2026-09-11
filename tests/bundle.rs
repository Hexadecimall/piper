//! The link inputs carried inside the compiler must be complete on their own:
//! a target that is bundled has to link without consulting anything installed
//! on the machine.

use piper_llvm::target::{Sysroot, Target};
use std::path::Path;

/// Every bundled target names a sysroot, and it is the unpacked bundle rather
/// than a toolchain found on this machine.
#[test]
fn bundled_targets_resolve_to_the_unpacked_bundle() {
    piper::init();
    let cache = piper_bundle::cache_dir().expect("a cache directory");
    let mut checked = 0;
    for name in piper_bundle::targets() {
        let triple = triple_for(name);
        let target = Target::parse(&triple).unwrap_or_else(|e| panic!("{name}: {e}"));
        let sr = Sysroot::find(&target, None).unwrap_or_else(|| panic!("{name} is bundled but has no sysroot"));
        assert!(sr.path.starts_with(&cache), "{name} resolved to {} instead of the bundle", sr.path.display());
        checked += 1;
    }
    assert!(checked > 0, "nothing is bundled, so this build depends on installed toolchains");
}

/// Each bundled target links a program end to end with no toolchain consulted.
#[test]
fn bundled_targets_link_without_a_toolchain() {
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    piper::init();
    let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/programs/hello.py");
    let work = std::env::temp_dir().join(format!("piper-bundle-{}", std::process::id()));
    std::fs::create_dir_all(&work).unwrap();

    let mut failures = Vec::new();
    let mut linked = 0;
    for name in piper_bundle::targets() {
        let triple = triple_for(name);
        let target = Target::parse(&triple).unwrap();
        // Linking also needs the runtime for that target, built alongside.
        if piper::runtime_archive_for(&target, None).is_err() { eprintln!("skipped {name}: no runtime archive"); continue }
        let out = work.join(target.exe_name(name));
        let opts = piper::CompileOptions { triple: Some(target.triple.clone()), ..Default::default() };
        match piper::compile_file(&src, &out, &opts) {
            Ok(()) => { linked += 1; assert!(out.exists(), "{name}: linked but produced no file"); }
            Err(e) => failures.push(format!("{name}: {e}")),
        }
    }
    let _ = std::fs::remove_dir_all(&work);
    if !failures.is_empty() { panic!("{} bundled targets failed to link:\n\n{}", failures.len(), failures.join("\n\n")); }
    eprintln!("linked {linked} bundled targets");
}

/// Bundle directory names are short; the target parser wants a triple.
fn triple_for(dir: &str) -> String {
    let arch = if dir.starts_with("aarch64") { "aarch64" } else { "x86_64" };
    if dir.contains("macos") { format!("{arch}-apple-macos") }
    else if dir.contains("windows") { format!("{arch}-pc-windows-gnu") }
    else if dir.contains("musl") { format!("{arch}-unknown-linux-musl") }
    else { format!("{arch}-unknown-linux-gnu") }
}
