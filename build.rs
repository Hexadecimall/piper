fn main() {
    println!("cargo:rerun-if-env-changed=PIPER_RELEASE_BASE_URL");
    println!("cargo:rerun-if-env-changed=PIPER_DISABLE_SELF_UPDATE");
    if let Ok(url) = std::env::var("PIPER_RELEASE_BASE_URL") {
        if url.starts_with("https://") || url.starts_with("http://") {
            println!("cargo:rustc-env=PIPER_RELEASE_BASE_URL={url}");
        }
    }
    if std::env::var("PIPER_DISABLE_SELF_UPDATE").as_deref() == Ok("1") {
        println!("cargo:rustc-env=PIPER_DISABLE_SELF_UPDATE=1");
    }
    let target = std::env::var("TARGET").unwrap_or_default();
    if target.contains("apple") {
        println!("cargo:rustc-link-arg=-Wl,-export_dynamic");
    } else if target.contains("windows") {
        println!("cargo:rustc-link-arg=-Wl,--export-all-symbols");
    } else {
        println!("cargo:rustc-link-arg=-Wl,--export-dynamic");
    }
}
