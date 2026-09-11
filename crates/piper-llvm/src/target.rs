//! Target descriptions: how to name, link, and find libraries for every
//! platform piper can compile to.

use crate::link::Flavor;
use std::path::PathBuf;
use std::sync::OnceLock;

/// Supplies a sysroot for a target from link inputs carried inside the
/// compiler. Registered by the driver, which owns the embedded copies.
type Provider = fn(&str) -> Option<PathBuf>;
static BUNDLED: OnceLock<Provider> = OnceLock::new();

/// Register the source of embedded link inputs. The first call wins.
pub fn set_bundled_sysroot_provider(f: Provider) { let _ = BUNDLED.set(f); }


#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Os { MacOs, Linux, Windows }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arch { X86_64, Aarch64 }

impl Arch {
    /// Name used by Apple's linker and SDK.
    pub fn apple_name(self) -> &'static str { match self { Arch::X86_64 => "x86_64", Arch::Aarch64 => "arm64" } }
    pub fn llvm_name(self) -> &'static str { match self { Arch::X86_64 => "x86_64", Arch::Aarch64 => "aarch64" } }
}

/// Which C library a Linux target links against.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Libc { Musl, Gnu, Msvc, MinGw, Apple }

#[derive(Debug, Clone)]
pub struct Target {
    /// Canonical LLVM triple.
    pub triple: String,
    pub os: Os,
    pub arch: Arch,
    pub libc: Libc,
}

impl Target {
    /// Parse a triple, accepting the common spellings.
    pub fn parse(triple: &str) -> Result<Target, String> {
        let t = triple.to_ascii_lowercase();
        let arch = if t.starts_with("x86_64") || t.starts_with("amd64") { Arch::X86_64 }
            else if t.starts_with("aarch64") || t.starts_with("arm64") { Arch::Aarch64 }
            else { return Err(format!("unsupported architecture in target '{triple}' (piper supports x86_64 and aarch64)")); };
        let (os, libc) = if t.contains("apple") || t.contains("darwin") || t.contains("macos") { (Os::MacOs, Libc::Apple) }
            else if t.contains("windows") || t.contains("mingw") {
                (Os::Windows, if t.contains("msvc") { Libc::Msvc } else { Libc::MinGw })
            }
            else if t.contains("linux") {
                (Os::Linux, if t.contains("musl") { Libc::Musl } else { Libc::Gnu })
            }
            else { return Err(format!("unsupported operating system in target '{triple}' (piper supports macos, linux and windows)")); };
        // Normalize to the triples LLVM and the sysroots agree on.
        let canonical = match (os, arch, libc) {
            (Os::MacOs, a, _) => format!("{}-apple-macosx11.0.0", a.llvm_name()),
            (Os::Linux, a, Libc::Musl) => format!("{}-unknown-linux-musl", a.llvm_name()),
            (Os::Linux, a, _) => format!("{}-unknown-linux-gnu", a.llvm_name()),
            (Os::Windows, a, Libc::Msvc) => format!("{}-pc-windows-msvc", a.llvm_name()),
            (Os::Windows, a, _) => format!("{}-pc-windows-gnu", a.llvm_name()),
        };
        Ok(Target { triple: canonical, os, arch, libc })
    }

    /// The host target.
    pub fn host() -> Target { Target::parse(&crate::host_triple()).expect("unsupported host") }

    /// Short directory-safe name, used for runtime archives and caches.
    pub fn dir_name(&self) -> String {
        let os = match self.os { Os::MacOs => "macos", Os::Linux => "linux", Os::Windows => "windows" };
        let libc = match self.libc { Libc::Musl => "-musl", Libc::Gnu => "-gnu", Libc::Msvc => "-msvc", Libc::MinGw => "-gnu", Libc::Apple => "" };
        format!("{}-{os}{libc}", self.arch.llvm_name())
    }

    pub fn flavor(&self) -> Flavor {
        match (self.os, self.libc) {
            (Os::MacOs, _) => Flavor::Darwin,
            (Os::Linux, _) => Flavor::Gnu,
            (Os::Windows, Libc::Msvc) => Flavor::Coff,
            (Os::Windows, _) => Flavor::MinGw,
        }
    }

    /// Executable file name for a program stem.
    pub fn exe_name(&self, stem: &str) -> String {
        if self.os == Os::Windows { format!("{stem}.exe") } else { stem.to_string() }
    }

    /// Static archive name for the runtime.
    pub fn runtime_archive_name(&self) -> &'static str {
        if self.os == Os::Windows && self.libc == Libc::Msvc { "piper_rt.lib" } else { "libpiper_rt.a" }
    }

    /// Whether a fully static link (no dynamic libc) is possible.
    pub fn supports_static_libc(&self) -> bool {
        match self.os { Os::MacOs => false, Os::Linux => true, Os::Windows => true }
    }

    /// Extra system libraries the runtime needs.
    pub fn system_libs(&self) -> Vec<&'static str> {
        match self.os {
            Os::MacOs => vec!["System"],
            // musl puts the math functions in libc; only glibc splits them out.
            Os::Linux => if self.libc == Libc::Musl { Vec::new() } else { vec!["m", "dl"] },
            Os::Windows => vec!["kernel32", "msvcrt", "ucrt"],
        }
    }

    pub fn is_host(&self) -> bool { self.triple == Target::host().triple }
}

/// Where a target's sysroot (C headers and libraries) can be found.
#[derive(Debug, Clone)]
pub struct Sysroot {
    pub path: PathBuf,
    /// Directories holding CRT objects and libc.
    pub lib_dirs: Vec<PathBuf>,
}

impl Sysroot {
    /// Locate the sysroot for `target`: an explicit path, an environment
    /// variable, a bundled sysroot next to the executable, or by asking an
    /// installed cross compiler where its own sysroot lives.
    pub fn find(target: &Target, explicit: Option<&std::path::Path>) -> Option<Sysroot> {
        if let Some(p) = explicit { return Some(Sysroot::at(p.to_path_buf())); }
        let var = format!("PIPER_SYSROOT_{}", target.dir_name().to_ascii_uppercase().replace('-', "_"));
        for key in [var.as_str(), "PIPER_SYSROOT"] {
            if let Ok(v) = std::env::var(key) { let p = PathBuf::from(v); if p.exists() { return Some(Sysroot::at(p)); } }
        }
        if std::env::var("PIPER_NO_BUNDLED_SYSROOT").is_err() {
            if let Some(f) = BUNDLED.get() {
                // The embedded copy is already complete; nothing outside it is needed.
                if let Some(p) = f(&target.dir_name()) { return Some(Sysroot::at(p)); }
            }
        }
        // A search path is a hint for finding sysroots, not an override: the
        // embedded copy above is the one this build was verified against.
        if let Some(p) = search_path_sysroot(target) { return Some(Sysroot::at(p)); }
        if target.os == Os::MacOs { return crate::link::macos_sdk().map(Sysroot::at); }
        if let Some(dir) = bundled_dir() {
            let p = dir.join("sysroots").join(target.dir_name());
            if p.exists() { return Some(Sysroot::at(p)); }
        }
        if target.os == Os::Windows && target.libc == Libc::MinGw {
            if let Some(sr) = mingw_sysroot(target) { return Some(sr); }
        }
        // Ask a cross compiler on PATH. Its name follows the GNU convention.
        for name in cross_compiler_names(target) {
            if let Some(p) = query_path(&name, "-print-sysroot") {
                return Some(Sysroot::at(p).prefer(pie_libc_dirs(target)));
            }
        }
        None
    }

    fn at(path: PathBuf) -> Sysroot {
        let mut lib_dirs = Vec::new();
        for rel in ["lib", "usr/lib", "lib64", "usr/lib64", "usr/lib/x86_64-linux-gnu", "usr/lib/aarch64-linux-gnu"] {
            let d = path.join(rel);
            if d.exists() { lib_dirs.push(d); }
        }
        // A mingw tree keeps its headers and import libraries one level down,
        // under the target triple.
        for d in triple_dirs(&path) {
            let l = d.join("lib");
            if l.exists() { lib_dirs.push(l); }
        }
        // A sysroot carrying its own gcc support library keeps it in a
        // versioned directory, which nothing else would look inside.
        for d in gcc_dirs(&path) { lib_dirs.push(d); }
        if lib_dirs.is_empty() { lib_dirs.push(path.clone()); }
        Sysroot { path, lib_dirs }
    }

    /// Prepend directories searched ahead of the sysroot's own.
    fn prefer(mut self, dirs: Vec<PathBuf>) -> Sysroot {
        let mut all = dirs;
        all.append(&mut self.lib_dirs);
        self.lib_dirs = all;
        self
    }

    /// Find a file (a CRT object or an archive) in the sysroot's library dirs.
    pub fn find_lib(&self, name: &str) -> Option<PathBuf> {
        for d in &self.lib_dirs {
            let p = d.join(name);
            if p.exists() { return Some(p); }
        }
        None
    }

    /// Include directories for compiling the runtime against this sysroot.
    pub fn include_dirs(&self) -> Vec<PathBuf> {
        let mut v = Vec::new();
        for rel in ["usr/include", "include"] {
            let d = self.path.join(rel);
            if d.exists() { v.push(d); }
        }
        for d in triple_dirs(&self.path) {
            let i = d.join("include");
            if i.exists() { v.push(i); }
        }
        v
    }
}

/// Versioned directories holding libgcc, under `lib/gcc` or `usr/lib/gcc`.
fn gcc_dirs(root: &std::path::Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for rel in ["usr/lib/gcc", "lib/gcc"] {
        let Ok(triples) = std::fs::read_dir(root.join(rel)) else { continue };
        for t in triples.flatten() {
            let Ok(versions) = std::fs::read_dir(t.path()) else { continue };
            for v in versions.flatten() {
                if v.path().join("libgcc.a").exists() { out.push(v.path()); }
            }
        }
    }
    out.sort();
    out
}

/// Subdirectories named after a target triple, the way a mingw tree nests its
/// headers and import libraries.
fn triple_dirs(root: &std::path::Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(root) else { return out };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.contains("-w64-mingw32") && e.path().join("include/windows.h").exists() { out.push(e.path()); }
    }
    out.sort();
    out
}

/// Look for `target`'s sysroot in the directories named by
/// `PIPER_SYSROOT_PATH`, a colon separated list. Each entry may itself be a
/// sysroot or a directory of them, named either the way piper names targets or
/// as `<libc>-<arch>` with the Debian spelling of the architecture.
fn search_path_sysroot(target: &Target) -> Option<PathBuf> {
    let raw = std::env::var("PIPER_SYSROOT_PATH").ok()?;
    for entry in raw.split(':').map(str::trim).filter(|s| !s.is_empty()) {
        let base = PathBuf::from(entry);
        for name in sysroot_names(target) {
            let p = base.join(&name);
            if p.join("usr/include").exists() || p.join("include").exists() || !triple_dirs(&p).is_empty() { return Some(p); }
        }
        if base.join("usr/include").exists() && base.file_name().is_some_and(|n| sysroot_names(target).iter().any(|s| n == s.as_str())) {
            return Some(base);
        }
    }
    None
}

/// Directory names a sysroot for `target` is plausibly stored under.
fn sysroot_names(target: &Target) -> Vec<String> {
    let arch = target.arch.llvm_name();
    let debian = match target.arch { Arch::X86_64 => "amd64", Arch::Aarch64 => "arm64" };
    let mut v = vec![target.dir_name(), target.triple.clone()];
    let libc = match (target.os, target.libc) {
        (Os::Linux, Libc::Musl) => "musl",
        (Os::Linux, _) => "glibc",
        (Os::Windows, _) => "windows",
        (Os::MacOs, _) => "macos",
    };
    v.push(format!("{libc}-{debian}"));
    v.push(format!("{libc}-{arch}"));
    v
}

/// mingw has no sysroot in the GNU sense: the driver reports its installation
/// prefix, while the headers and import libraries live in `<prefix>/<triple>`.
/// Locate that tree from the driver's own crt2.o, and add the gcc-internal
/// directory that holds crtbegin.o, crtend.o and libgcc.a.
fn mingw_sysroot(target: &Target) -> Option<Sysroot> {
    let mut root = None;
    let mut gcc_dir = None;
    for name in cross_compiler_names(target) {
        // <root>/lib/crt2.o
        if root.is_none() { root = query_path(&name, "-print-file-name=crt2.o").and_then(|p| p.parent()?.parent().map(PathBuf::from)); }
        if gcc_dir.is_none() { gcc_dir = query_path(&name, "-print-libgcc-file-name").and_then(|p| p.parent().map(PathBuf::from)); }
        if root.is_some() && gcc_dir.is_some() { break; }
    }
    let root = root?;
    if !root.join("include/windows.h").exists() { return None; }
    let mut sr = Sysroot::at(root);
    if let Some(d) = gcc_dir { if !sr.lib_dirs.contains(&d) { sr.lib_dirs.push(d); } }
    Some(sr)
}

/// Ask a compiler driver for a path it would use itself. The answer can be
/// relative and full of `..` components, so it is canonicalized; a driver that
/// cannot find the file echoes the bare name, which fails to canonicalize.
fn query_path(compiler: &str, flag: &str) -> Option<PathBuf> {
    let out = std::process::Command::new(compiler).arg(flag).output().ok()?;
    if !out.status.success() { return None; }
    let s = String::from_utf8_lossy(&out.stdout).trim().to_string();
    if s.is_empty() { return None; }
    PathBuf::from(s).canonicalize().ok()
}

/// Directories holding a position-independent libc for `target`, searched
/// ahead of the sysroot. Static PIE needs a libc built with -fPIE, which a
/// stock cross toolchain often does not ship; the Rust toolchain does.
fn pie_libc_dirs(target: &Target) -> Vec<PathBuf> {
    if target.libc != Libc::Musl { return Vec::new(); }
    if let Ok(v) = std::env::var("PIPER_MUSL_LIBC") { let p = PathBuf::from(v); if p.join("rcrt1.o").exists() { return vec![p]; } }
    let Ok(out) = std::process::Command::new("rustc").args(["--print", "sysroot"]).output() else { return Vec::new() };
    if !out.status.success() { return Vec::new(); }
    let root = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim());
    let d = root.join(format!("lib/rustlib/{}-unknown-linux-musl/lib/self-contained", target.arch.llvm_name()));
    if d.join("rcrt1.o").exists() { vec![d] } else { Vec::new() }
}

/// Names a cross compiler for `target` is conventionally installed under.
pub fn cross_compiler_names(target: &Target) -> Vec<String> {
    let arch = target.arch.llvm_name();
    match (target.os, target.libc) {
        (Os::Linux, Libc::Musl) => vec![format!("{arch}-linux-musl-gcc"), format!("{arch}-unknown-linux-musl-gcc")],
        (Os::Linux, _) => vec![format!("{arch}-linux-gnu-gcc"), format!("{arch}-unknown-linux-gnu-gcc")],
        (Os::Windows, Libc::MinGw) => vec![format!("{arch}-w64-mingw32-gcc"), format!("{arch}-w64-mingw32-clang")],
        _ => Vec::new(),
    }
}

/// Directory holding piper's bundled data (runtime archives, sysroots),
/// resolved relative to the running executable.
pub fn bundled_dir() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let dir = exe.parent()?;
    for rel in ["../lib/piper", "lib/piper", "."] {
        let p = dir.join(rel);
        if p.join("runtime").exists() || p.join("sysroots").exists() { return Some(p); }
    }
    None
}
