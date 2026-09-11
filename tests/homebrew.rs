#[cfg(unix)]
#[test]
fn formula_generator_uses_release_checksums() {
    use std::fs;
    use std::process::Command;

    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let work = std::env::temp_dir().join(format!("piper-homebrew-test-{}", std::process::id()));
    let checksums = work.join("checksums.txt");
    let formula = work.join("piper.rb");
    fs::create_dir_all(&work).unwrap();
    fs::write(
        &checksums,
        concat!(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  piper-aarch64-macos-static-release\n",
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  piper-x86_64-macos-static-release\n",
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc  piper-x86_64-linux-static-release\n",
        ),
    )
    .unwrap();

    let status = Command::new("sh")
        .arg(root.join("packaging/homebrew/generate-formula.sh"))
        .args([
            "0.1.0",
            "https://example.invalid/piper",
            "https://example.invalid/releases/v0.1.0",
        ])
        .arg(&checksums)
        .arg(&formula)
        .status()
        .unwrap();
    assert!(status.success());

    let generated = fs::read_to_string(&formula).unwrap();
    assert!(generated.contains("version \"0.1.0\""));
    assert!(generated.contains("piper-aarch64-macos-static-release"));
    assert!(generated.contains(&"a".repeat(64)));
    assert!(generated.contains(&"b".repeat(64)));
    assert!(generated.contains(&"c".repeat(64)));
    assert!(!generated.contains("@BASE_URL@"));

    fs::remove_file(checksums).unwrap();
    fs::remove_file(formula).unwrap();
    fs::remove_dir(work).unwrap();
}
