use std::process::Command;

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
