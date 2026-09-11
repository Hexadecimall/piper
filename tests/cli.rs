use std::io::Write;
use std::process::{Command, Stdio};

fn piper(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_piper")).args(args).env("NO_COLOR", "1").output().unwrap()
}

#[test]
fn global_commands_have_stable_terminal_syntax() {
    let help = piper(&["help"]);
    assert!(help.status.success());
    assert!(String::from_utf8_lossy(&help.stdout).contains("piper compile"));
    assert!(String::from_utf8_lossy(&help.stdout).contains("piper eval"));

    let version = piper(&["--version"]);
    assert!(version.status.success());
    assert!(String::from_utf8_lossy(&version.stdout).starts_with("piper "));

    let library = piper(&["stdlib"]);
    assert!(library.status.success());
    assert!(String::from_utf8_lossy(&library.stdout).contains("Python 3.14.7"));
}

#[test]
fn eval_compiles_and_displays_an_expression() {
    let output = piper(&["eval", "6 * 7"]);
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(String::from_utf8_lossy(&output.stdout), "42\n");
}

#[test]
fn compile_accepts_long_and_joined_options() {
    let missing = piper(&["compile", "--target=x86_64-unknown-linux-musl", "--output=out", "--opt-level", "z"]);
    assert_eq!(missing.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&missing.stderr).contains("compile needs an input file"));
}

#[test]
fn update_requires_a_configured_release_channel() {
    let output = Command::new(env!("CARGO_BIN_EXE_piper")).arg("--update").env_remove("PIPER_RELEASE_BASE_URL").env("NO_COLOR", "1").output().unwrap();
    assert_eq!(output.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&output.stderr).contains("no update channel"));
}

#[test]
fn repl_output_records_successful_input() {
    let directory = std::env::temp_dir().join(format!("piper-repl-output-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let recording = directory.join("session.py");
    let mut child = Command::new(env!("CARGO_BIN_EXE_piper"))
        .args(["repl", "-o", recording.to_str().unwrap()])
        .env("NO_COLOR", "1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    child.stdin.as_mut().unwrap().write_all(b"answer = 42\n:quit\n").unwrap();
    let output = child.wait_with_output().unwrap();
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
    assert_eq!(std::fs::read_to_string(&recording).unwrap(), "answer = 42\n\n");
    let _ = std::fs::remove_dir_all(directory);
}
