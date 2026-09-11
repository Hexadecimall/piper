//! Package directories can become native shared libraries and can be imported
//! by another Piper-compiled program.

use std::path::Path;
use std::process::Command;

#[test]
#[ignore = "package-library output is parked behind its API"]
fn compiles_and_imports_package_library() {
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no linker library"); return; }
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let work = std::env::temp_dir().join(format!("piper-library-{}", std::process::id()));
    std::fs::create_dir_all(&work).unwrap();
    let extension = work.join(if cfg!(target_os = "macos") { "sample.dylib" } else if cfg!(target_os = "windows") { "sample.dll" } else { "sample.so" });
    let options = piper::CompileOptions { library: true, static_libc: false, module_name: Some("sample".into()), ..Default::default() };
    piper::compile_file(&root.join("tests/packages/sample"), &extension, &options).unwrap();

    let source = work.join("use_package.py");
    std::fs::write(&source, "import sample\nprint(sample.answer())\n").unwrap();
    let executable = work.join("use_package");
    piper::compile_file(&source, &executable, &piper::CompileOptions::default()).unwrap();
    let output = Command::new(&executable).env("PIPERPATH", &work).output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(output.stdout, b"126\n");
    let _ = std::fs::remove_dir_all(work);
}
