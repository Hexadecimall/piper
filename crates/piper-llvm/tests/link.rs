//! Link an emitted object into a real executable, in-process, and run it.

use piper_llvm::{Codegen, OptLevel, host_triple, link};

#[test]
fn links_an_object_into_a_runnable_executable() {
    if !link::AVAILABLE { eprintln!("skipped: no lld in this build"); return; }
    let mut cg = Codegen::new("t");
    cg.emit_const_i32_function("main", 42);
    let obj = cg.emit_object(None, OptLevel::O2).unwrap();
    let dir = std::env::temp_dir().join(format!("piper-link-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let obj_path = dir.join("t.o");
    let exe = dir.join("t");
    std::fs::write(&obj_path, obj).unwrap();
    link::link_executable(&host_triple(), &exe, &[obj_path], false, &[]).unwrap();
    let status = std::process::Command::new(&exe).status().unwrap();
    assert_eq!(status.code(), Some(42));
    let _ = std::fs::remove_dir_all(&dir);
}
