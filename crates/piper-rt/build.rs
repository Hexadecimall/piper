//! Compiles the C runtime into a static archive for the host and for every
//! cross target a sysroot can be found for.
//!
//! Archives land in `$OUT_DIR/runtime/<target>/libpiper_rt.a`. The compiler
//! links them in-process, so nothing but this build step needs a C compiler.
//!
//! Cross targets are taken from `PIPER_CROSS_TARGETS` (comma separated) or a
//! default list. A target with no usable sysroot is skipped with a warning
//! rather than failing the build.

use std::path::{Path, PathBuf};
use std::process::Command;

struct Spec {
    /// Directory name and the identity piper looks up at compile time.
    dir: String,
    /// LLVM triple passed to clang.
    triple: String,
    os: &'static str,
    arch: &'static str,
    libc: &'static str,
}

impl Spec {
    /// Directory name mingw installs its target tree under.
    fn gnu_triple(&self) -> String { format!("{}-w64-mingw32", self.arch) }
}

fn main() {
    println!("cargo:rerun-if-changed=c");
    let sources = sources();
    for s in &sources { println!("cargo:rerun-if-changed={}", s.display()); }
    println!("cargo:rerun-if-changed=include/piper/object.h");
    println!("cargo:rerun-if-env-changed=PIPER_CROSS_TARGETS");
    println!("cargo:rerun-if-env-changed=PIPER_SYSROOT");
    println!("cargo:rerun-if-env-changed=PIPER_SYSROOT_PATH");
    for target in ["X86_64_LINUX_MUSL", "AARCH64_LINUX_MUSL", "X86_64_LINUX_GNU", "AARCH64_LINUX_GNU", "X86_64_WINDOWS_GNU", "AARCH64_WINDOWS_GNU", "X86_64_MACOS", "AARCH64_MACOS"] {
        println!("cargo:rerun-if-env-changed=PIPER_SYSROOT_{target}");
    }

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let runtime_dir = out.join("runtime");
    std::fs::create_dir_all(&runtime_dir).unwrap();

    // The host archive is also linked into the piper binary's test harness.
    let host = host_spec();
    let host_archive = build_archive(&host, &sources, &runtime_dir, None)
        .unwrap_or_else(|e| panic!("failed to build the host runtime: {e}"));
    println!("cargo:rustc-link-search=native={}", host_archive.parent().unwrap().display());
    // ORC resolves generated code against this process, so every runtime
    // object must survive the host link even when Rust does not call it.
    println!("cargo:rustc-link-lib=static:+whole-archive=piper_rt");
    println!("cargo:archive={}", host_archive.display());
    println!("cargo:runtime_dir={}", runtime_dir.display());

    for spec in cross_specs(&host) {
        match Sysroot::find(&spec) {
            None => println!("cargo:warning=piper: no sysroot for {}, skipping its runtime (set PIPER_SYSROOT_{})", spec.dir, spec.dir.to_uppercase().replace('-', "_")),
            Some(sr) => match build_archive(&spec, &sources, &runtime_dir, Some(&sr)) {
                Ok(p) => println!("cargo:warning=piper: built runtime for {} at {}", spec.dir, p.display()),
                Err(e) => println!("cargo:warning=piper: runtime for {} failed to build: {e}", spec.dir),
            },
        }
    }
}

fn sources() -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir("c").unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "c")).collect();
    v.sort();
    v
}

fn host_spec() -> Spec {
    let target = std::env::var("TARGET").unwrap();
    let arch = if target.starts_with("aarch64") { "aarch64" } else { "x86_64" };
    let (os, libc) = if target.contains("apple") { ("macos", "") }
        else if target.contains("windows") { ("windows", if target.contains("msvc") { "-msvc" } else { "-gnu" }) }
        else { ("linux", if target.contains("musl") { "-musl" } else { "-gnu" }) };
    Spec { dir: format!("{arch}-{os}{libc}"), triple: canonical_triple(arch, os, libc), os: leak(os), arch: leak(arch), libc: leak(libc) }
}

fn leak(s: &str) -> &'static str { Box::leak(s.to_string().into_boxed_str()) }

fn canonical_triple(arch: &str, os: &str, libc: &str) -> String {
    match os {
        "macos" => format!("{arch}-apple-macosx11.0.0"),
        "windows" => format!("{arch}-pc-windows{}", if libc == "-msvc" { "-msvc" } else { "-gnu" }),
        _ => format!("{arch}-unknown-linux{}", if libc == "-musl" { "-musl" } else { "-gnu" }),
    }
}

fn cross_specs(host: &Spec) -> Vec<Spec> {
    let requested = std::env::var("PIPER_CROSS_TARGETS").unwrap_or_default();
    let names: Vec<String> = if requested.trim().is_empty() {
        let mut v = vec!["x86_64-linux-musl".to_string(), "aarch64-linux-musl".to_string(),
                         "x86_64-linux-gnu".to_string(), "aarch64-linux-gnu".to_string(),
                         "x86_64-windows-gnu".to_string(), "aarch64-windows-gnu".to_string()];
        // The macOS SDK carries both architectures, so the other one is free.
        if host.os == "macos" { v.push(if host.arch == "aarch64" { "x86_64-macos".into() } else { "aarch64-macos".into() }); }
        v
    } else { requested.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect() };
    names.iter().filter_map(|n| parse_spec(n)).filter(|s| s.dir != host.dir).collect()
}

fn parse_spec(name: &str) -> Option<Spec> {
    let n = name.to_ascii_lowercase();
    let arch = if n.starts_with("aarch64") || n.starts_with("arm64") { "aarch64" } else if n.starts_with("x86_64") || n.starts_with("amd64") { "x86_64" } else { return None };
    let (os, libc) = if n.contains("macos") || n.contains("apple") || n.contains("darwin") { ("macos", "") }
        else if n.contains("windows") || n.contains("mingw") { ("windows", if n.contains("msvc") { "-msvc" } else { "-gnu" }) }
        else if n.contains("linux") { ("linux", if n.contains("gnu") { "-gnu" } else { "-musl" }) }
        else { return None };
    Some(Spec { dir: format!("{arch}-{os}{libc}"), triple: canonical_triple(arch, os, libc), os: leak(os), arch: leak(arch), libc: leak(libc) })
}

/// A target's C headers and libraries.
struct Sysroot { path: PathBuf }

impl Sysroot {
    fn find(spec: &Spec) -> Option<Sysroot> {
        let var = format!("PIPER_SYSROOT_{}", spec.dir.to_uppercase().replace('-', "_"));
        for key in [var.as_str(), "PIPER_SYSROOT"] {
            if let Ok(v) = std::env::var(key) { let p = PathBuf::from(v); if p.exists() { return Some(Sysroot { path: p }); } }
        }
        if let Some(p) = search_path_sysroot(spec) { return Some(Sysroot { path: p }); }
        if spec.os == "macos" { return macos_sdk().map(|p| Sysroot { path: p }); }
        // mingw has no sysroot in the GNU sense: the driver reports its
        // installation prefix, while the headers and import libraries sit in
        // <prefix>/<triple>. Find that tree from where crt2.o lives.
        if spec.os == "windows" {
            for name in cross_compilers(spec) {
                if let Some(p) = query_path(&name, "-print-file-name=crt2.o") {
                    // <root>/lib/crt2.o
                    if let Some(root) = p.parent().and_then(|d| d.parent()) {
                        if let Some(sr) = accept(root.to_path_buf(), spec) { return Some(sr); }
                    }
                }
            }
        }
        // Ask an installed cross compiler for its own sysroot.
        for name in cross_compilers(spec) {
            if let Some(p) = query_path(&name, "-print-sysroot") {
                if let Some(sr) = accept(p, spec) { return Some(sr); }
            }
        }
        None
    }

    fn include_dirs(&self) -> Vec<PathBuf> {
        let mut v: Vec<PathBuf> = ["usr/include", "include"].iter().map(|r| self.path.join(r)).filter(|p| p.exists()).collect();
        // Debian keeps the architecture-dependent headers in a multiarch
        // subdirectory, which the generic ones include from.
        for arch in ["x86_64", "aarch64"] {
            let d = self.path.join(format!("usr/include/{arch}-linux-gnu"));
            if d.exists() { v.push(d); }
        }
        // A mingw tree nests its headers under the target triple.
        for d in triple_dirs(&self.path) {
            let i = d.join("include");
            if i.exists() { v.push(i); }
        }
        v
    }
}

/// Look for `spec`'s sysroot under `PIPER_SYSROOT_PATH`, a colon separated
/// list of directories, each holding sysroots named either the way piper names
/// targets or as `<libc>-<arch>` with the Debian spelling of the architecture.
fn search_path_sysroot(spec: &Spec) -> Option<PathBuf> {
    let raw = std::env::var("PIPER_SYSROOT_PATH").ok()?;
    let debian = if spec.arch == "aarch64" { "arm64" } else { "amd64" };
    let libc = match (spec.os, spec.libc) {
        ("linux", "-musl") => "musl",
        ("linux", _) => "glibc",
        ("windows", _) => "windows",
        _ => "macos",
    };
    let names = [spec.dir.clone(), spec.triple.clone(), format!("{libc}-{debian}"), format!("{libc}-{}", spec.arch)];
    for entry in raw.split(':').map(str::trim).filter(|s| !s.is_empty()) {
        for name in &names {
            let p = PathBuf::from(entry).join(name);
            let sr = Sysroot { path: p };
            if !sr.include_dirs().is_empty() { return Some(sr.path); }
        }
    }
    None
}

/// Subdirectories named after a target triple, the way a mingw tree nests its
/// headers and import libraries.
fn triple_dirs(root: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(root) else { return out };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.contains("-w64-mingw32") && e.path().join("include/windows.h").exists() { out.push(e.path()); }
    }
    out.sort();
    out
}

/// Take `path` as a sysroot if it carries headers, descending into the
/// per-target subdirectory a mingw prefix keeps them in.
fn accept(path: PathBuf, spec: &Spec) -> Option<Sysroot> {
    let sr = Sysroot { path };
    if !sr.include_dirs().is_empty() { return Some(sr); }
    let nested = Sysroot { path: sr.path.join(spec.gnu_triple()) };
    if !nested.include_dirs().is_empty() { return Some(nested); }
    None
}

/// Ask a compiler driver for a path it would use itself. The answer can be
/// relative and full of `..` components, so it is canonicalized; a driver that
/// cannot find the file echoes the bare name, which fails to canonicalize.
fn query_path(compiler: &str, flag: &str) -> Option<PathBuf> {
    let out = Command::new(compiler).arg(flag).output().ok()?;
    if !out.status.success() { return None; }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if s.is_empty() { return None; }
    PathBuf::from(s).canonicalize().ok()
}

fn cross_compilers(spec: &Spec) -> Vec<String> {
    match (spec.os, spec.libc) {
        ("linux", "-musl") => vec![format!("{}-linux-musl-gcc", spec.arch), format!("{}-unknown-linux-musl-gcc", spec.arch)],
        ("linux", _) => vec![format!("{}-linux-gnu-gcc", spec.arch), format!("{}-unknown-linux-gnu-gcc", spec.arch)],
        ("windows", _) => vec![format!("{}-w64-mingw32-gcc", spec.arch), format!("{}-w64-mingw32-clang", spec.arch)],
        _ => Vec::new(),
    }
}

fn macos_sdk() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("SDKROOT") { let p = PathBuf::from(p); if p.join("usr/include/stdio.h").exists() { return Some(p); } }
    let out = Command::new("xcrun").args(["--show-sdk-path"]).output().ok()?;
    if !out.status.success() { return None; }
    let p = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim());
    if p.join("usr/include/stdio.h").exists() { Some(p) } else { None }
}

/// Compile every source for `spec` and archive the objects.
fn build_archive(spec: &Spec, sources: &[PathBuf], runtime_dir: &Path, sysroot: Option<&Sysroot>) -> Result<PathBuf, String> {
    let dir = runtime_dir.join(&spec.dir);
    let objdir = dir.join("obj");
    std::fs::create_dir_all(&objdir).map_err(|e| e.to_string())?;
    let clang = clang_path();
    let mut objects = Vec::new();
    for src in sources {
        let obj = objdir.join(format!("{}.o", src.file_stem().unwrap().to_string_lossy()));
        let mut cmd = Command::new(&clang);
        cmd.args(["-c", "-O2", "-std=c11", "-fvisibility=default", "-fno-strict-aliasing"])
            .args(["-Wall", "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-command-line-argument"])
            .arg("-target").arg(&spec.triple)
            .arg("-Iinclude")
            .arg(src)
            .arg("-o").arg(&obj);
        // POSIX declarations (fdopen and friends) sit behind a feature macro.
        if spec.os != "macos" { cmd.arg("-D_GNU_SOURCE"); }
        // Static PIE is the default Linux link mode, so the runtime must be PIE.
        if spec.os == "linux" { cmd.arg("-fPIE"); }
        if let Some(sr) = sysroot {
            if spec.os == "macos" { cmd.arg("-isysroot").arg(&sr.path); }
            else {
                cmd.arg("--sysroot").arg(&sr.path);
                cmd.arg("-nostdlibinc");
                for inc in sr.include_dirs() { cmd.arg("-isystem").arg(inc); }
            }
        }
        let out = cmd.output().map_err(|e| format!("cannot run {}: {e}", clang.display()))?;
        if !out.status.success() {
            return Err(format!("{} failed:\n{}", src.display(), String::from_utf8_lossy(&out.stderr).lines().take(12).collect::<Vec<_>>().join("\n")));
        }
        objects.push(obj);
    }
    let archive = dir.join("libpiper_rt.a");
    let _ = std::fs::remove_file(&archive);
    let ar = ar_path();
    let mut cmd = Command::new(&ar);
    cmd.arg("crs").arg(&archive).args(&objects);
    let out = cmd.output().map_err(|e| format!("cannot run {}: {e}", ar.display()))?;
    if !out.status.success() { return Err(format!("archiving failed:\n{}", String::from_utf8_lossy(&out.stderr))); }
    Ok(archive)
}

fn clang_path() -> PathBuf {
    if let Ok(p) = std::env::var("PIPER_CLANG") { return PathBuf::from(p); }
    // Prefer the clang from the LLVM install piper links against.
    if let Ok(prefix) = std::env::var("PIPER_LLVM_PREFIX") {
        let p = PathBuf::from(prefix).join("bin/clang");
        if p.exists() { return p; }
    }
    PathBuf::from("clang")
}

/// llvm-ar builds archives for any target; the Rust toolchain ships one.
fn ar_path() -> PathBuf {
    if let Ok(p) = std::env::var("PIPER_AR") { return PathBuf::from(p); }
    if let Ok(prefix) = std::env::var("PIPER_LLVM_PREFIX") {
        let p = PathBuf::from(prefix).join("bin/llvm-ar");
        if p.exists() { return p; }
    }
    if let Ok(out) = Command::new("rustc").args(["--print", "sysroot"]).output() {
        if out.status.success() {
            let root = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim());
            if let Ok(rd) = std::fs::read_dir(root.join("lib/rustlib")) {
                for e in rd.flatten() {
                    let p = e.path().join("bin/llvm-ar");
                    if p.exists() { return p; }
                }
            }
        }
    }
    PathBuf::from("ar")
}
