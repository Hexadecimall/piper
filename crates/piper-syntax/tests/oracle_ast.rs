//! Differential test: piper's parser against CPython's `ast.dump` over the
//! standard library. Skips when no `python3` is on PATH.

use piper_syntax::dump::dump;
use piper_syntax::parse_module;
use std::path::{Path, PathBuf};
use std::process::Command;

fn python() -> Option<&'static str> {
    for p in ["python3.14", "python3"] {
        if Command::new(p).arg("--version").output().is_ok_and(|o| o.status.success()) { return Some(p); }
    }
    None
}

fn stdlib_dir(py: &str) -> PathBuf {
    let out = Command::new(py).args(["-c", "import sysconfig; print(sysconfig.get_paths()['stdlib'])"]).output().unwrap();
    PathBuf::from(String::from_utf8(out.stdout).unwrap().trim())
}

fn cpython_dump(py: &str, file: &Path) -> String {
    let script = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/oracle/astdump.py");
    let out = Command::new(py).arg(script).arg(file).output().unwrap();
    String::from_utf8(out.stdout).unwrap().trim_end().to_string()
}

fn piper_dump(src: &str, name: &str) -> String {
    match parse_module(src, name) {
        Ok(m) => dump(&m),
        Err(e) => format!("ERROR {}", e.lineno),
    }
}

/// Position of the first differing byte, with context from both sides.
fn first_diff(a: &str, b: &str) -> Option<String> {
    if a == b { return None; }
    let i = a.bytes().zip(b.bytes()).position(|(x, y)| x != y).unwrap_or(a.len().min(b.len()));
    let lo = i.saturating_sub(120);
    let cut = |s: &str, from: usize, to: usize| -> String {
        let from = (from..=s.len()).find(|&k| s.is_char_boundary(k)).unwrap_or(s.len());
        let to = (0..=to.min(s.len())).rev().find(|&k| s.is_char_boundary(k)).unwrap_or(0);
        s[from..to.max(from)].to_string()
    };
    Some(format!("  cpython: ...{}\n  piper:   ...{}", cut(a, lo, i + 80), cut(b, lo, i + 80)))
}

fn check_file(py: &str, path: &Path) -> Result<(), String> {
    let Ok(src) = std::fs::read_to_string(path) else { return Ok(()) };
    let expected = cpython_dump(py, path);
    let actual = piper_dump(&src, &path.to_string_lossy());
    match first_diff(&expected, &actual) {
        None => Ok(()),
        Some(d) => Err(format!("{}\n{d}", path.display())),
    }
}

fn collect(dir: &Path, out: &mut Vec<PathBuf>, limit: usize) {
    let mut entries: Vec<_> = std::fs::read_dir(dir).unwrap().flatten().map(|e| e.path()).collect();
    entries.sort();
    for p in entries {
        if out.len() >= limit { return; }
        if p.is_dir() {
            let name = p.file_name().unwrap().to_string_lossy();
            if name == "test" || name == "site-packages" || name.starts_with("__") { continue; }
            collect(&p, out, limit);
        } else if p.extension().is_some_and(|e| e == "py") {
            out.push(p);
        }
    }
}

#[test]
fn stdlib_parses_identically_to_cpython() {
    let Some(py) = python() else { eprintln!("skipped: no python3"); return; };
    let limit: usize = std::env::var("PIPER_ORACLE_FILES").ok().and_then(|s| s.parse().ok()).unwrap_or(400);
    let mut files = Vec::new();
    collect(&stdlib_dir(py), &mut files, limit);
    assert!(!files.is_empty());
    let failures: Vec<String> = files.iter().filter_map(|f| check_file(py, f).err()).collect();
    if !failures.is_empty() {
        panic!("{} of {} files differ:\n{}", failures.len(), files.len(), failures.iter().take(10).cloned().collect::<Vec<_>>().join("\n"));
    }
}

#[test]
fn grammar_test_files_parse_identically_to_cpython() {
    let Some(py) = python() else { return; };
    let test = stdlib_dir(py).join("test");
    let mut failures = Vec::new();
    for name in ["test_grammar.py", "test_fstring.py", "test_tstring.py", "test_string_literals.py", "test_patma.py", "test_type_params.py", "test_typing.py", "test_ast/test_ast.py", "test_unparse.py", "test_positional_only_arg.py", "test_named_expressions.py", "test_except_star.py"] {
        let p = test.join(name);
        if p.exists() { if let Err(e) = check_file(py, &p) { failures.push(e); } }
    }
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
