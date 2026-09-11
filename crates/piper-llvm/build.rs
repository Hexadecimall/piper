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
    println!("cargo:rerun-if-env-changed=PIPER_LLD_PREFIX");
    println!("cargo:rerun-if-env-changed=PIPER_LLVM_LINK");
    println!("cargo:rerun-if-env-changed=PIPER_LLVM_SYSTEM_LIBS");
    println!("cargo:rerun-if-changed=cxx/lld_shim.cpp");
    println!("cargo::rustc-check-cfg=cfg(piper_has_lld)");
    if let Ok(prefix) = std::env::var("PIPER_LLVM_PREFIX") {
        let prefix = PathBuf::from(prefix);
        let llvm_lib = prefix.join("lib");
        let lld_prefix = std::env::var("PIPER_LLD_PREFIX").map(PathBuf::from).unwrap_or_else(|_| prefix.clone());
        let lld_lib = lld_prefix.join("lib");
        let llvm_libs = archives(&llvm_lib, "LLVM", "PIPER_LLVM_PREFIX/lib");
        let lld_libs = archives(&lld_lib, "lld", "PIPER_LLD_PREFIX/lib");
        let has_lld = !lld_libs.is_empty();
        if has_lld {
            cc::Build::new()
                .cpp(true)
                .file("cxx/lld_shim.cpp")
                .include(prefix.join("include"))
                .include(lld_prefix.join("include"))
                .std("c++17")
                .flag_if_supported("-fno-rtti")
                .flag_if_supported("-fno-exceptions")
                .warnings(false)
                .compile("piper_lld_shim");
            println!("cargo:rustc-cfg=piper_has_lld");
        }
        println!("cargo:rustc-link-search=native={}", llvm_lib.display());
        if lld_lib != llvm_lib { println!("cargo:rustc-link-search=native={}", lld_lib.display()); }
        // Static archives: on Linux the group wrapper resolves circular references.
        if cfg!(target_os = "linux") { println!("cargo:rustc-link-arg=-Wl,--start-group"); }
        for l in &lld_libs { println!("cargo:rustc-link-lib=static={l}"); }
        if std::env::var("PIPER_LLVM_LINK").as_deref() == Ok("dynamic") {
            println!("cargo:rustc-link-lib=dylib=LLVM");
        } else {
            for l in &llvm_libs { println!("cargo:rustc-link-lib=static={l}"); }
        }
        if let Ok(libraries) = std::env::var("PIPER_LLVM_SYSTEM_LIBS") {
            for argument in libraries.split_whitespace() {
                if let Some(library) = argument.strip_prefix("-l") {
                    println!("cargo:rustc-link-lib={library}");
                } else if let Some(directory) = argument.strip_prefix("-L") {
                    println!("cargo:rustc-link-search=native={directory}");
                } else if argument == "-pthread" {
                    println!("cargo:rustc-link-lib=pthread");
                } else {
                    let path = std::path::Path::new(argument);
                    let filename = path.file_name().and_then(|value| value.to_str()).unwrap_or("");
                    let (kind, suffix) = if filename.ends_with(".a") { ("static", ".a") }
                        else if filename.ends_with(".dylib") { ("dylib", ".dylib") }
                        else if filename.ends_with(".so") { ("dylib", ".so") }
                        else { panic!("unsupported PIPER_LLVM_SYSTEM_LIBS argument: {argument}") };
                    let library = filename.strip_prefix("lib").and_then(|value| value.strip_suffix(suffix))
                        .unwrap_or_else(|| panic!("invalid native library path: {argument}"));
                    let directory = path.parent().unwrap_or_else(|| panic!("native library has no parent: {argument}"));
                    println!("cargo:rustc-link-search=native={}", directory.display());
                    println!("cargo:rustc-link-lib={kind}={library}");
                }
            }
        }
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

fn archives(directory: &std::path::Path, prefix: &str, label: &str) -> Vec<String> {
    let mut libraries = Vec::new();
    for entry in std::fs::read_dir(directory).unwrap_or_else(|_| panic!("{label}")) {
        let name = entry.unwrap().file_name().to_string_lossy().into_owned();
        if let Some(stem) = name.strip_prefix("lib").and_then(|value| value.strip_suffix(".a")) {
            if stem.starts_with(prefix) { libraries.push(stem.to_string()); }
        }
    }
    libraries.sort();
    libraries
}
