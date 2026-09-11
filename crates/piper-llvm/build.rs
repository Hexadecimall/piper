//! Locates LLVM for linking.
//!
//! Development builds use the `libLLVM` shared library that ships inside the
//! Rust toolchain (found via `rustc --print sysroot`). Setting
//! `PIPER_LLVM_PREFIX` to an LLVM install prefix switches to the static
//! archives under `<prefix>/lib`, which also brings in lld for in-process
//! linking (the shared toolchain library has no lld).

use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-env-changed=PIPER_LLVM_PREFIX");
    println!("cargo:rerun-if-changed=cxx/lld_shim.cpp");
    println!("cargo::rustc-check-cfg=cfg(piper_has_lld)");
    if let Ok(prefix) = std::env::var("PIPER_LLVM_PREFIX") {
        let prefix = PathBuf::from(prefix);
        let lib = prefix.join("lib");
        let mut llvm_libs = Vec::new();
        let mut lld_libs = Vec::new();
        for entry in std::fs::read_dir(&lib).expect("PIPER_LLVM_PREFIX/lib") {
            let name = entry.unwrap().file_name().to_string_lossy().into_owned();
            if let Some(stem) = name.strip_prefix("lib").and_then(|s| s.strip_suffix(".a")) {
                if stem.starts_with("LLVM") { llvm_libs.push(stem.to_string()); }
                if stem.starts_with("lld") { lld_libs.push(stem.to_string()); }
            }
        }
        let has_lld = !lld_libs.is_empty();
        if has_lld {
            cc::Build::new()
                .cpp(true)
                .file("cxx/lld_shim.cpp")
                .include(prefix.join("include"))
                .std("c++17")
                .flag_if_supported("-fno-rtti")
                .flag_if_supported("-fno-exceptions")
                .warnings(false)
                .compile("piper_lld_shim");
            println!("cargo:rustc-cfg=piper_has_lld");
        }
        println!("cargo:rustc-link-search=native={}", lib.display());
        // Static archives: on Linux the group wrapper resolves circular references.
        if cfg!(target_os = "linux") { println!("cargo:rustc-link-arg=-Wl,--start-group"); }
        for l in &lld_libs { println!("cargo:rustc-link-lib=static={l}"); }
        for l in &llvm_libs { println!("cargo:rustc-link-lib=static={l}"); }
        if cfg!(target_os = "linux") { println!("cargo:rustc-link-arg=-Wl,--end-group"); }
        if cfg!(target_os = "macos") { println!("cargo:rustc-link-lib=c++"); }
        else if cfg!(target_os = "linux") { println!("cargo:rustc-link-lib=stdc++"); }
        return;
    }
    let rustc = std::env::var("RUSTC").unwrap_or_else(|_| "rustc".into());
    let out = Command::new(rustc).arg("--print").arg("sysroot").output().expect("rustc --print sysroot");
    let sysroot = PathBuf::from(String::from_utf8(out.stdout).unwrap().trim());
    let lib = sysroot.join("lib");
    let dylib = lib.join(if cfg!(target_os = "macos") { "libLLVM.dylib" } else if cfg!(windows) { "LLVM.dll" } else { "libLLVM.so" });
    if !dylib.exists() {
        panic!("no LLVM found: {} is missing and PIPER_LLVM_PREFIX is not set", dylib.display());
    }
    println!("cargo:rustc-link-search=native={}", lib.display());
    println!("cargo:rustc-link-lib=dylib=LLVM");
    if cfg!(any(target_os = "macos", target_os = "linux")) {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display());
    }
}
