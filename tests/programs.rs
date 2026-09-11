//! Compile every program under tests/programs, run it, and compare stdout
//! and exit status with CPython's. Skips when linking is unavailable.

use std::path::{Path, PathBuf};
use std::process::Command;

fn python() -> Option<&'static str> {
    for p in ["python3.14", "python3"] { if Command::new(p).arg("--version").output().is_ok_and(|o| o.status.success()) { return Some(p); } }
    None
}

/// Drop the caret/anchor lines CPython adds under a source line: piper does
/// not compute column anchors yet, so tracebacks are compared without them.
fn strip_anchors(s: &str) -> String {
    s.lines()
        .filter(|l| { let t = l.trim(); !t.is_empty() && t.chars().all(|c| c == '^' || c == '~') == false || t.is_empty() })
        .collect::<Vec<_>>()
        .join("\n")
}

fn run_case(py: &str, src: &Path, workdir: &Path) -> Result<(), String> {
    let exe = workdir.join(src.file_stem().unwrap());
    piper::compile_file(src, &exe, &piper::CompileOptions::default()).map_err(|e| format!("compile failed: {e}"))?;
    let expected = Command::new(py).arg(src).output().map_err(|e| e.to_string())?;
    let actual = Command::new(&exe).output().map_err(|e| e.to_string())?;
    let (es, ao) = (String::from_utf8_lossy(&expected.stdout), String::from_utf8_lossy(&actual.stdout));
    if es != ao || expected.status.code() != actual.status.code() {
        return Err(format!("output differs\n--- cpython (exit {:?}) ---\n{}\n--- piper (exit {:?}) ---\n{}\n--- piper stderr ---\n{}", expected.status.code(), es, actual.status.code(), ao, String::from_utf8_lossy(&actual.stderr)));
    }
    let (ee, ae) = (strip_anchors(&String::from_utf8_lossy(&expected.stderr)), strip_anchors(&String::from_utf8_lossy(&actual.stderr)));
    if ee != ae {
        return Err(format!("stderr differs\n--- cpython ---\n{ee}\n--- piper ---\n{ae}"));
    }
    Ok(())
}

#[test]
fn programs_match_cpython() {
    if !piper_llvm::link::AVAILABLE { eprintln!("skipped: no lld"); return; }
    let Some(py) = python() else { eprintln!("skipped: no python3"); return; };
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/programs");
    let mut files: Vec<PathBuf> = std::fs::read_dir(&dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "py")).collect();
    files.sort();
    if let Ok(only) = std::env::var("PIPER_PROGRAM") { files.retain(|f| f.file_name().unwrap().to_string_lossy().contains(&only)); }
    let work = std::env::temp_dir().join(format!("piper-programs-{}", std::process::id()));
    std::fs::create_dir_all(&work).unwrap();
    let mut failures = Vec::new();
    for f in &files {
        if let Err(e) = run_case(py, f, &work) { failures.push(format!("{}:\n{e}", f.display())); }
    }
    let _ = std::fs::remove_dir_all(&work);
    if !failures.is_empty() { panic!("{} of {} programs differ:\n\n{}", failures.len(), files.len(), failures.join("\n\n")); }
}
