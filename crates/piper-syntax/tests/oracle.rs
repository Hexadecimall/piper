//! Differential test: piper's tokenizer against CPython's `tokenize` module
//! over CPython's own standard library. Skips when no `python3` is on PATH.

use piper_syntax::token::{Tok, tokenize};
use std::path::{Path, PathBuf};
use std::process::Command;

fn python() -> Option<&'static str> {
    for p in ["python3.14", "python3"] {
        if Command::new(p).arg("--version").output().is_ok_and(|o| o.status.success()) {
            return Some(p);
        }
    }
    None
}

fn stdlib_dir(py: &str) -> PathBuf {
    let out = Command::new(py)
        .args(["-c", "import sysconfig; print(sysconfig.get_paths()['stdlib'])"])
        .output()
        .unwrap();
    PathBuf::from(String::from_utf8(out.stdout).unwrap().trim())
}

fn cpython_tokens(py: &str, file: &Path) -> String {
    let script = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/oracle/tokens.py");
    let out = Command::new(py).arg(script).arg(file).output().unwrap();
    String::from_utf8(out.stdout).unwrap()
}

fn render(tok: &Tok) -> (&'static str, String) {
    match tok {
        Tok::Name(s) => ("NAME", s.clone()),
        Tok::Number(s) => ("NUMBER", s.clone()),
        Tok::String(s) => ("STRING", s.clone()),
        Tok::FStringStart(s) => ("FSTRING_START", s.clone()),
        Tok::FStringMiddle(s) => ("FSTRING_MIDDLE", s.clone()),
        Tok::FStringEnd(s) => ("FSTRING_END", s.clone()),
        Tok::TStringStart(s) => ("TSTRING_START", s.clone()),
        Tok::TStringMiddle(s) => ("TSTRING_MIDDLE", s.clone()),
        Tok::TStringEnd(s) => ("TSTRING_END", s.clone()),
        Tok::Op(s) => ("OP", s.clone()),
        Tok::Comment(s) => ("COMMENT", s.clone()),
        Tok::Newline => ("NEWLINE", String::new()),
        Tok::Nl => ("NL", String::new()),
        Tok::Indent => ("INDENT", String::new()),
        Tok::Dedent => ("DEDENT", String::new()),
        Tok::EndMarker => ("ENDMARKER", String::new()),
    }
}

/// Reduce both streams to the comparable core: type + text for tokens whose
/// text is meaningful, plus positions for all tokens.
fn piper_tokens(src: &str) -> String {
    let mut out = String::new();
    match tokenize(src) {
        Err(e) => out.push_str(&format!("ERROR {e}\n")),
        Ok(toks) => {
            for t in toks {
                let (kind, text) = render(&t.tok);
                let text = match kind {
                    "NEWLINE" | "NL" | "INDENT" | "DEDENT" | "ENDMARKER" => String::new(),
                    _ => text,
                };
                out.push_str(&format!("{},{}-{},{} {} {}\n", t.start.0, t.start.1, t.end.0, t.end.1, kind, py_repr(&text)));
            }
        }
    }
    out
}

/// Python `repr()` for str, enough for token text.
fn py_repr(s: &str) -> String {
    let use_double = s.contains('\'') && !s.contains('"');
    let q = if use_double { '"' } else { '\'' };
    let mut r = String::new();
    r.push(q);
    for c in s.chars() {
        match c {
            '\\' => r.push_str("\\\\"),
            '\n' => r.push_str("\\n"),
            '\r' => r.push_str("\\r"),
            '\t' => r.push_str("\\t"),
            c if c == q => { r.push('\\'); r.push(c); }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => r.push_str(&format!("\\x{:02x}", c as u32)),
            c => r.push(c),
        }
    }
    r.push(q);
    r
}

/// Normalize CPython's lines: blank text for the whitespace-only token kinds.
fn normalize_cpython(s: &str) -> String {
    let mut out = String::new();
    for line in s.lines() {
        let mut parts = line.splitn(3, ' ');
        let pos = parts.next().unwrap_or("");
        let kind = parts.next().unwrap_or("");
        let text = parts.next().unwrap_or("");
        let text = match kind {
            "NEWLINE" | "NL" | "INDENT" | "DEDENT" | "ENDMARKER" => "''",
            _ => text,
        };
        out.push_str(&format!("{pos} {kind} {text}\n"));
    }
    out
}

fn first_diff(a: &str, b: &str) -> Option<(usize, String, String)> {
    for (i, (x, y)) in a.lines().zip(b.lines()).enumerate() {
        if x != y { return Some((i + 1, x.into(), y.into())); }
    }
    let (na, nb) = (a.lines().count(), b.lines().count());
    if na != nb { return Some((na.min(nb) + 1, format!("<{na} lines>"), format!("<{nb} lines>"))); }
    None
}

fn check_file(py: &str, path: &Path) -> Result<(), String> {
    let src = match std::fs::read_to_string(path) { Ok(s) => s, Err(_) => return Ok(()) };
    let expected = normalize_cpython(&cpython_tokens(py, path));
    let actual = piper_tokens(&src);
    match first_diff(&expected, &actual) {
        None => Ok(()),
        Some((n, e, a)) => Err(format!("{}: token {n}\n  cpython: {e}\n  piper:   {a}", path.display())),
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
fn stdlib_tokenizes_identically_to_cpython() {
    let Some(py) = python() else { eprintln!("skipped: no python3"); return; };
    let limit: usize = std::env::var("PIPER_ORACLE_FILES").ok().and_then(|s| s.parse().ok()).unwrap_or(400);
    let mut files = Vec::new();
    collect(&stdlib_dir(py), &mut files, limit);
    assert!(!files.is_empty());
    let failures: Vec<String> = files.iter().filter_map(|f| check_file(py, f).err()).collect();
    if !failures.is_empty() {
        panic!("{} of {} files differ:\n{}", failures.len(), files.len(), failures.iter().take(15).cloned().collect::<Vec<_>>().join("\n"));
    }
}

#[test]
fn grammar_test_files_tokenize_identically_to_cpython() {
    let Some(py) = python() else { return; };
    let test = stdlib_dir(py).join("test");
    let mut failures = Vec::new();
    for name in ["test_grammar.py", "test_tokenize.py", "test_fstring.py", "test_tstring.py", "test_string_literals.py", "test_syntax.py", "test_unicode_identifiers.py"] {
        let p = test.join(name);
        if p.exists() { if let Err(e) = check_file(py, &p) { failures.push(e); } }
    }
    assert!(failures.is_empty(), "{}", failures.join("\n"));
}
