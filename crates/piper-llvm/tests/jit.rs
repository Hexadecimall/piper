//! End-to-end: build IR with the bindings, JIT it in-process, call it, and
//! emit a native object for the host into memory.

use piper_llvm::{Codegen, OptLevel};

#[test]
fn jit_compiles_and_runs_a_function_returning_42() {
    let mut cg = Codegen::new("t");
    cg.emit_const_i32_function("answer", 42);
    let jit = cg.into_jit().unwrap();
    let f: extern "C" fn() -> i32 = unsafe { std::mem::transmute(jit.lookup("answer").unwrap()) };
    assert_eq!(f(), 42);
}

#[test]
fn emits_a_native_object_file_for_the_host() {
    let mut cg = Codegen::new("t");
    cg.emit_const_i32_function("answer", 7);
    let obj = cg.emit_object(None, OptLevel::O2).unwrap();
    assert!(obj.len() > 64);
    let magic = &obj[..4];
    let is_macho = magic == [0xcf, 0xfa, 0xed, 0xfe] || magic == [0xfe, 0xed, 0xfa, 0xcf];
    let is_elf = magic == [0x7f, b'E', b'L', b'F'];
    let is_coff = obj.len() > 2 && (obj[..2] == [0x64, 0x86] || obj[..2] == [0x64, 0xaa]);
    assert!(is_macho || is_elf || is_coff, "unknown object magic {magic:02x?}");
}

#[test]
fn cross_targets_are_available() {
    let mut cg = Codegen::new("t");
    cg.emit_const_i32_function("answer", 1);
    let obj = cg.emit_object(Some("x86_64-unknown-linux-gnu"), OptLevel::O0).unwrap();
    assert_eq!(&obj[..4], &[0x7f, b'E', b'L', b'F']);
}

#[test]
fn optimizer_runs_at_every_level() {
    for lvl in [OptLevel::O0, OptLevel::O1, OptLevel::O2, OptLevel::O3, OptLevel::Os, OptLevel::Oz] {
        let mut cg = Codegen::new("t");
        cg.emit_const_i32_function("answer", 3);
        cg.emit_object(None, lvl).unwrap();
    }
}
