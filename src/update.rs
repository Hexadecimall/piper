use std::path::Path;
use std::process::Command;

pub struct UpdateOptions {
    pub check: bool,
    pub dynamic: bool,
    pub development: bool,
}

pub fn asset_name(dynamic: bool, development: bool) -> String {
    let arch = match std::env::consts::ARCH { "aarch64" => "aarch64", "x86_64" => "x86_64", other => other };
    let os = match std::env::consts::OS { "macos" => "macos", "windows" => "windows", "linux" => "linux", other => other };
    let linkage = if dynamic { "dynamic" } else { "static" };
    let stage = if development { "dev" } else { "release" };
    format!("piper-{arch}-{os}-{linkage}-{stage}{}", if os == "windows" { ".exe" } else { "" })
}

fn release_base() -> Result<String, String> {
    let value = std::env::var("PIPER_RELEASE_BASE_URL").ok()
        .or_else(|| option_env!("PIPER_RELEASE_BASE_URL").map(str::to_string))
        .ok_or("this Piper build has no update channel; set PIPER_RELEASE_BASE_URL to a release download directory")?;
    let value = value.trim_end_matches('/').to_string();
    if !value.starts_with("https://") && !value.starts_with("http://") {
        return Err("PIPER_RELEASE_BASE_URL must use HTTP or HTTPS".into());
    }
    Ok(value)
}

fn curl_bytes(url: &str) -> Result<Vec<u8>, String> {
    let output = Command::new("curl").args(["--fail", "--location", "--silent", "--show-error", url]).output()
        .map_err(|error| format!("cannot start curl: {error}"))?;
    if !output.status.success() { return Err(String::from_utf8_lossy(&output.stderr).trim().to_string()); }
    Ok(output.stdout)
}

fn expected_checksum(text: &str, asset: &str) -> Result<[u8; 32], String> {
    for line in text.lines() {
        let mut fields = line.split_whitespace();
        let Some(hash) = fields.next() else { continue };
        let Some(name) = fields.next() else { continue };
        if name.trim_start_matches('*') == asset {
            if hash.len() != 64 { break; }
            let mut bytes = [0u8; 32];
            for (index, byte) in bytes.iter_mut().enumerate() {
                *byte = u8::from_str_radix(&hash[index * 2..index * 2 + 2], 16).map_err(|_| "release checksum is invalid")?;
            }
            return Ok(bytes);
        }
    }
    Err(format!("checksums.txt has no entry for {asset}"))
}

pub fn run(options: &UpdateOptions) -> Result<String, String> {
    if option_env!("PIPER_DISABLE_SELF_UPDATE") == Some("1") {
        return Err("updates for this installation are managed by the package manager".into());
    }
    let base = release_base()?;
    let asset = asset_name(options.dynamic, options.development);
    let latest = String::from_utf8(curl_bytes(&format!("{base}/version.txt"))?).map_err(|_| "version.txt is not UTF-8")?;
    let latest = latest.trim();
    if latest.is_empty() || !latest.chars().all(|character| character.is_ascii_digit() || character == '.') {
        return Err("release version is invalid".into());
    }
    let current = env!("CARGO_PKG_VERSION");
    if options.check {
        return Ok(if latest == current { format!("Piper {current} is up to date") } else { format!("Piper {latest} is available for {asset}") });
    }
    if latest == current { return Ok(format!("Piper {current} is already up to date")); }
    let checksums = String::from_utf8(curl_bytes(&format!("{base}/checksums.txt"))?).map_err(|_| "checksums.txt is not UTF-8")?;
    let expected = expected_checksum(&checksums, &asset)?;

    let executable = std::env::current_exe().map_err(|error| format!("cannot locate the running Piper executable: {error}"))?;
    let parent = executable.parent().ok_or("the Piper executable has no parent directory")?;
    let temporary = parent.join(format!(".piper-update-{}", std::process::id()));
    let bytes = curl_bytes(&format!("{base}/{asset}"))?;
    if sha256(&bytes) != expected { return Err("downloaded Piper binary failed SHA-256 verification".into()); }
    use std::io::Write;
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(&temporary)
        .map_err(|error| format!("cannot create update beside Piper: {error}"))?;
    if let Err(error) = file.write_all(&bytes).and_then(|_| file.sync_all()) {
        let _ = std::fs::remove_file(&temporary);
        return Err(format!("cannot write update beside Piper: {error}"));
    }
    copy_permissions(&executable, &temporary)?;
    replace(&temporary, &executable)?;
    Ok(format!("updated Piper to {latest} ({asset})"))
}

#[cfg(unix)]
fn copy_permissions(current: &Path, replacement: &Path) -> Result<(), String> {
    use std::os::unix::fs::PermissionsExt;
    let mode = std::fs::metadata(current).map_err(|error| error.to_string())?.permissions().mode();
    std::fs::set_permissions(replacement, std::fs::Permissions::from_mode(mode)).map_err(|error| error.to_string())
}

#[cfg(windows)]
fn copy_permissions(_current: &Path, _replacement: &Path) -> Result<(), String> { Ok(()) }

#[cfg(not(any(unix, windows)))]
fn copy_permissions(_current: &Path, _replacement: &Path) -> Result<(), String> { Ok(()) }

fn replace(temporary: &Path, executable: &Path) -> Result<(), String> {
    match std::fs::rename(temporary, executable) {
        Ok(()) => Ok(()),
        Err(error) => {
            let _ = std::fs::remove_file(temporary);
            Err(format!("cannot replace '{}': {error}", executable.display()))
        }
    }
}

fn sha256(input: &[u8]) -> [u8; 32] {
    const INITIAL: [u32; 8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    const ROUND: [u32; 64] = [
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    ];
    let bit_len = (input.len() as u64) * 8;
    let mut data = input.to_vec();
    data.push(0x80);
    while data.len() % 64 != 56 { data.push(0); }
    data.extend_from_slice(&bit_len.to_be_bytes());
    let mut state = INITIAL;
    for block in data.chunks_exact(64) {
        let mut words = [0u32; 64];
        for i in 0..16 { words[i] = u32::from_be_bytes(block[i*4..i*4+4].try_into().unwrap()); }
        for i in 16..64 {
            let s0 = words[i-15].rotate_right(7) ^ words[i-15].rotate_right(18) ^ (words[i-15] >> 3);
            let s1 = words[i-2].rotate_right(17) ^ words[i-2].rotate_right(19) ^ (words[i-2] >> 10);
            words[i] = words[i-16].wrapping_add(s0).wrapping_add(words[i-7]).wrapping_add(s1);
        }
        let [mut a,mut b,mut c,mut d,mut e,mut f,mut g,mut h] = state;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choose = (e & f) ^ (!e & g);
            let t1 = h.wrapping_add(s1).wrapping_add(choose).wrapping_add(ROUND[i]).wrapping_add(words[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(majority);
            h=g; g=f; f=e; e=d.wrapping_add(t1); d=c; c=b; b=a; a=t1.wrapping_add(t2);
        }
        for (slot, value) in state.iter_mut().zip([a,b,c,d,e,f,g,h]) { *slot = slot.wrapping_add(value); }
    }
    let mut output = [0u8; 32];
    for (chunk, value) in output.chunks_exact_mut(4).zip(state) { chunk.copy_from_slice(&value.to_be_bytes()); }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_matches_the_standard_vector() {
        assert_eq!(sha256(b"abc"), [0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad]);
    }

    #[test]
    fn checksum_manifest_selects_an_exact_asset() {
        let hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        assert_eq!(expected_checksum(&format!("{hash}  piper-test\n"), "piper-test").unwrap(), sha256(b"abc"));
    }
}
