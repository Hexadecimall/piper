fn main() {
    println!("cargo:rerun-if-env-changed=PIPER_RELEASE_BASE_URL");
    if let Ok(url) = std::env::var("PIPER_RELEASE_BASE_URL") {
        if url.starts_with("https://") || url.starts_with("http://") {
            println!("cargo:rustc-env=PIPER_RELEASE_BASE_URL={url}");
        }
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
