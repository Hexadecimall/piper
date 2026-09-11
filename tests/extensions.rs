//! A native shared library enters through the public module ABI and is found
//! through the runtime search path without help from an external interpreter.

use std::path::Path;
use std::process::Command;

#[test]
fn loads_native_extensions() {
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no linker library"); return; }
    let clang = Command::new("clang").arg("--version").output();
    if !clang.is_ok_and(|o| o.status.success()) { eprintln!("skipped: no C compiler for test fixture"); return; }
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let work = std::env::temp_dir().join(format!("piper-extension-{}", std::process::id()));
    std::fs::create_dir_all(&work).unwrap();
    for name in ["minimal", "multiphase"] {
        let extension = work.join(format!("{name}.so"));
        let mut cc = Command::new("clang");
        cc.args(["-shared", "-fPIC", "-I"]).arg(root.join("crates/piper-rt/include"));
        if cfg!(target_os = "macos") { cc.args(["-undefined", "dynamic_lookup"]); }
        let status = cc.arg(root.join(format!("tests/extensions/{name}.c"))).arg("-o").arg(&extension).status().unwrap();
        assert!(status.success(), "{name} extension fixture did not compile");
    }
    let source = work.join("use_extension.py");
    std::fs::write(&source, "import minimal\nimport multiphase\nprint(minimal.answer())\nprint(multiphase.answer)\n").unwrap();
    let exe = work.join("use_extension");
    piper::compile_file(&source, &exe, &piper::CompileOptions::default()).unwrap();
    let output = Command::new(&exe).env("PIPERPATH", &work).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(output.stdout, b"42\n84\n");
    let _ = std::fs::remove_dir_all(work);
}
