//! In-process linking through lld's library API. Available when the crate was
//! built against a static LLVM prefix that includes lld (`PIPER_LLVM_PREFIX`).

use crate::target::{Libc, Os, Sysroot, Target};
use std::path::{Path, PathBuf};

/// Whether this build can link executables.
pub const AVAILABLE: bool = cfg!(piper_has_lld);

#[cfg(piper_has_lld)]
unsafe extern "C" {
    fn piper_lld_link(flavor: *const std::ffi::c_char, args: *const *const std::ffi::c_char, nargs: usize, out_log: *mut *mut std::ffi::c_char) -> i32;
    fn piper_lld_free(p: *mut std::ffi::c_char);
}

/// Target flavor for lld.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Flavor { Darwin, Gnu, Coff, MinGw }

impl Flavor {
    /// The program name lld dispatches on.
    fn name(self) -> &'static str {
        match self { Flavor::Darwin => "ld64.lld", Flavor::Gnu => "ld.lld", Flavor::Coff => "lld-link", Flavor::MinGw => "ld.lld" }
    }
    pub fn for_triple(triple: &str) -> Flavor {
        Target::parse(triple).map(|t| t.flavor()).unwrap_or(Flavor::Gnu)
    }
}

/// Run lld with the given arguments (without the program name).
#[cfg(piper_has_lld)]
pub fn run(flavor: Flavor, args: &[String]) -> Result<(), String> {
    use std::ffi::{CStr, CString};
    let cargs: Vec<CString> = args.iter().map(|a| CString::new(a.as_str()).unwrap()).collect();
    let ptrs: Vec<*const std::ffi::c_char> = cargs.iter().map(|c| c.as_ptr()).collect();
    let flavor = CString::new(flavor.name()).unwrap();
    let mut log: *mut std::ffi::c_char = std::ptr::null_mut();
    let code = unsafe { piper_lld_link(flavor.as_ptr(), ptrs.as_ptr(), ptrs.len(), &mut log) };
    let text = if log.is_null() { String::new() } else { unsafe { let s = CStr::from_ptr(log).to_string_lossy().into_owned(); piper_lld_free(log); s } };
    if code == 0 { Ok(()) } else { Err(if text.is_empty() { format!("linker failed with code {code}") } else { text }) }
}

#[cfg(not(piper_has_lld))]
pub fn run(_flavor: Flavor, _args: &[String]) -> Result<(), String> {
    Err("linking is unavailable: this build has no lld (set PIPER_LLVM_PREFIX to a static LLVM install with lld and rebuild)".into())
}

/// The macOS SDK directory, needed to resolve libSystem.
pub fn macos_sdk() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("SDKROOT") { let p = PathBuf::from(p); if p.join("usr/lib/libSystem.tbd").exists() { return Some(p); } }
    let candidates = [
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
        "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
        "/Applications/Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
    ];
    for c in candidates { let p = Path::new(c); if p.join("usr/lib/libSystem.tbd").exists() { return Some(p.to_path_buf()); } }
    if let Ok(rd) = std::fs::read_dir("/Library/Developer/CommandLineTools/SDKs") {
        let mut sdks: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|p| p.join("usr/lib/libSystem.tbd").exists()).collect();
        sdks.sort();
        return sdks.pop();
    }
    None
}

/// How to link one executable.
#[derive(Debug, Clone)]
pub struct LinkRequest {
    pub target: Target,
    pub output: PathBuf,
    /// Objects and archives, in order.
    pub inputs: Vec<PathBuf>,
    /// Link libc statically when the target allows it.
    pub static_libc: bool,
    /// Explicit sysroot, overriding discovery.
    pub sysroot: Option<PathBuf>,
    /// Extra system libraries by short name.
    pub libs: Vec<String>,
    /// Produce a loadable shared library instead of a process executable.
    pub shared: bool,
    /// Keep the small public C API needed by loadable Python extensions.
    pub export_python_api: bool,
}

/// Link an executable for any supported target.
pub fn link(req: &LinkRequest) -> Result<(), String> {
    let t = &req.target;
    let sysroot = Sysroot::find(t, req.sysroot.as_deref());
    let out = req.output.to_string_lossy().into_owned();
    let mut args: Vec<String> = Vec::new();
    match t.os {
        Os::MacOs => {
            let sdk = sysroot.ok_or_else(|| "no macOS SDK found: install the Xcode command line tools or set SDKROOT".to_string())?;
            args.extend(["-arch".into(), t.arch.apple_name().into()]);
            args.extend(["-platform_version".into(), "macos".into(), "11.0".into(), "11.0".into()]);
            if req.shared { args.push("-dylib".into()); }
            args.push("-dead_strip".into());
            if !req.static_libc || req.shared { args.push("-export_dynamic".into()); }
            else if req.export_python_api {
                for symbol in ["_PyLong_FromLong", "_PyModule_AddIntConstant", "_PyModule_Create2", "_PyModuleDef_Init"] {
                    args.extend(["-exported_symbol".into(), symbol.into()]);
                }
            }
            args.extend(["-syslibroot".into(), sdk.path.to_string_lossy().into_owned()]);
            args.extend(["-o".into(), out]);
            for i in &req.inputs { args.push(i.to_string_lossy().into_owned()); }
            args.push("-lSystem".into());
            for l in &req.libs { if l != "System" { args.push(format!("-l{l}")); } }
        }
        Os::Linux => {
            let sr = sysroot.ok_or_else(|| missing_sysroot_message(t))?;
            let stat = !req.shared && req.static_libc && t.supports_static_libc();
            // A static PIE loads at any address, which plain -static cannot.
            let static_pie = stat && sr.find_lib("rcrt1.o").is_some();
            args.extend(["-o".into(), out]);
            if req.shared { args.push("-shared".into()); }
            args.push("--gc-sections".into());
            if stat { args.push("-static".into()); }
            if static_pie { args.push("-pie".into()); }
            if !stat {
                args.push("--export-dynamic".into());
                args.push("-dynamic-linker".into());
                args.push(dynamic_linker(t));
            }
            for d in &sr.lib_dirs { args.push(format!("-L{}", d.display())); }
            // C runtime startup files, in the order the ELF ABI expects.
            let crt1: &[&str] = if static_pie { &["rcrt1.o"] } else if stat { &["crt1.o"] } else { &["Scrt1.o", "crt1.o"] };
            if !req.shared {
                let start = crt1.iter().find_map(|n| sr.find_lib(n)).ok_or_else(|| format!("no {} in the sysroot at {}", crt1[0], sr.path.display()))?;
                args.push(start.to_string_lossy().into_owned());
                if let Some(p) = sr.find_lib("crti.o") { args.push(p.to_string_lossy().into_owned()); }
            }
            for i in &req.inputs { args.push(i.to_string_lossy().into_owned()); }
            // A group resolves the runtime's and libc's mutual references.
            args.push("--start-group".into());
            for l in &req.libs { args.push(format!("-l{l}")); }
            args.push("-lc".into());
            // glibc's static archives call into the compiler builtins.
            for l in ["gcc", "gcc_eh"] {
                if sr.find_lib(&format!("lib{l}.a")).is_some() { args.push(format!("-l{l}")); }
            }
            args.push("--end-group".into());
            if !req.shared { if let Some(p) = sr.find_lib("crtn.o") { args.push(p.to_string_lossy().into_owned()); } }
        }
        Os::Windows => {
            let sr = sysroot.ok_or_else(|| missing_sysroot_message(t))?;
            match t.libc {
                Libc::Msvc => {
                    args.push(format!("/out:{out}"));
                    args.push("/opt:ref".into());
                    if req.shared { args.push("/dll".into()); } else { args.push("/subsystem:console".into()); }
                    args.push(format!("/machine:{}", if t.arch.llvm_name() == "x86_64" { "x64" } else { "arm64" }));
                    for d in &sr.lib_dirs { args.push(format!("/libpath:{}", d.display())); }
                    for i in &req.inputs { args.push(i.to_string_lossy().into_owned()); }
                    for l in &req.libs { args.push(format!("{l}.lib")); }
                }
                _ => {
                    // mingw: GNU-style driver producing a PE.
                    args.extend(["-m".into(), if t.arch.llvm_name() == "x86_64" { "i386pep".into() } else { "arm64pe".into() }]);
                    args.extend(["-o".into(), out]);
                    args.push("--gc-sections".into());
                    if req.shared { args.push("--dll".into()); }
                    for d in &sr.lib_dirs { args.push(format!("-L{}", d.display())); }
                    let startup = if req.shared { "dllcrt2.o" } else { "crt2.o" };
                    if let Some(p) = sr.find_lib(startup) { args.push(p.to_string_lossy().into_owned()); }
                    if let Some(p) = sr.find_lib("crtbegin.o") { args.push(p.to_string_lossy().into_owned()); }
                    for i in &req.inputs { args.push(i.to_string_lossy().into_owned()); }
                    args.push("--start-group".into());
                    for l in &req.libs { args.push(format!("-l{l}")); }
                    // libgcc carries the compiler builtins (__chkstk_ms and
                    // the wide integer helpers) the runtime calls into.
                    for l in ["mingw32", "gcc", "mingwex", "msvcrt", "kernel32", "ucrt"] { args.push(format!("-l{l}")); }
                    args.push("--end-group".into());
                    if let Some(p) = sr.find_lib("crtend.o") { args.push(p.to_string_lossy().into_owned()); }
                }
            }
        }
    }
    run(t.flavor(), &args)
}

fn dynamic_linker(t: &Target) -> String {
    match (t.arch.llvm_name(), t.libc) {
        ("aarch64", Libc::Musl) => "/lib/ld-musl-aarch64.so.1".into(),
        ("aarch64", _) => "/lib/ld-linux-aarch64.so.1".into(),
        (_, Libc::Musl) => "/lib/ld-musl-x86_64.so.1".into(),
        _ => "/lib64/ld-linux-x86-64.so.2".into(),
    }
}

fn missing_sysroot_message(t: &Target) -> String {
    let var = format!("PIPER_SYSROOT_{}", t.dir_name().to_uppercase().replace('-', "_"));
    let hint = crate::target::cross_compiler_names(t).first().cloned().unwrap_or_default();
    let mut m = format!("no sysroot for {}: piper needs that target's C library to link.\n  set {var}=/path/to/sysroot", t.triple);
    if !hint.is_empty() { m.push_str(&format!("\n  or install a cross toolchain providing {hint}, which piper will query for its sysroot")); }
    m
}

/// Convenience wrapper used by the tests.
pub fn link_executable(triple: &str, output: &Path, inputs: &[PathBuf], static_libc: bool, libs: &[&str]) -> Result<(), String> {
    let target = Target::parse(triple)?;
    let libs = if libs.is_empty() { target.system_libs().iter().map(|s| s.to_string()).collect() } else { libs.iter().map(|s| s.to_string()).collect() };
    link(&LinkRequest { target, output: output.to_path_buf(), inputs: inputs.to_vec(), static_libc, sysroot: None, libs, shared: false, export_python_api: true })
}
