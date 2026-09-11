use piper::{CompileOptions, compile_file};
use piper_llvm::OptLevel;
use std::path::PathBuf;
use std::io::IsTerminal;
use std::process::ExitCode;

const USAGE: &str = "usage:
  piper compile FILE.py|PACKAGE_DIR [-o OUT] [options]
  piper repl [-o FILE]
  piper eval CODE
  piper ast FILE.py
  piper targets
  piper stdlib
  piper --update [--check] [--dynamic] [--dev]
  piper help
  piper --dump-ast FILE.py
  piper --version

compile options:
  -o PATH            output file (default: the input's stem)
  -O0 -O1 -O2 -O3    optimization level (default -O2)
  -Os -Oz            optimize for size
  --target TRIPLE    cross compile, for example x86_64-unknown-linux-musl
  --sysroot PATH     C library root for the target, overriding the built-in one
  --runtime PATH     piper runtime archive for the target
  -dynamic, --dynamic  link the target's libc dynamically
  --emit-ir          write LLVM IR instead of an executable
  --emit-obj         write an object file instead of an executable

Every target listed by `piper targets` with a size under bundled carries its
own C library and startup files inside this executable, so cross compiling to
it needs nothing installed.";

fn main() -> ExitCode {
    piper::init();
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("help") | Some("--help") | Some("-h") => help(),
        Some("--version") | Some("-V") => { println!("piper {}", env!("CARGO_PKG_VERSION")); ExitCode::SUCCESS }
        Some("--dump-ast") | Some("ast") => match args.get(1) { Some(path) => dump_ast(path), None => usage() },
        Some("-c") | Some("eval") => evaluate(&args[1..]),
        Some("compile") => compile(&args[1..]),
        Some("repl") => repl(&args[1..]),
        Some("targets") => targets(),
        Some("stdlib") => stdlib(),
        Some("--update") | Some("update") => update(&args[1..]),
        None => ExitCode::from(piper::repl::run(None) as u8),
        _ => usage(),
    }
}

fn update(args: &[String]) -> ExitCode {
    let mut options = piper::update::UpdateOptions { check: false, dynamic: false, development: false };
    for argument in args {
        match argument.as_str() {
            "--check" => options.check = true,
            "--dynamic" => options.dynamic = true,
            "--static" => options.dynamic = false,
            "--dev" => options.development = true,
            "--release" => options.development = false,
            _ => return fail(&format!("unknown update option '{argument}'"), 2),
        }
    }
    match piper::update::run(&options) {
        Ok(message) => { println!("{message}"); ExitCode::SUCCESS }
        Err(error) => fail(&error, 1),
    }
}

fn usage() -> ExitCode { eprintln!("{USAGE}"); ExitCode::from(2) }
fn help() -> ExitCode { println!("{USAGE}"); ExitCode::SUCCESS }

fn fail(message: &str, code: u8) -> ExitCode {
    if std::io::stderr().is_terminal() && std::env::var_os("NO_COLOR").is_none() {
        eprintln!("\x1b[31mpiper:\x1b[0m {message}");
    } else {
        eprintln!("piper: {message}");
    }
    ExitCode::from(code)
}

fn repl(args: &[String]) -> ExitCode {
    let mut output = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-o" | "--output" => {
                i += 1;
                let Some(path) = args.get(i) else { return fail("repl output option needs a path", 2); };
                output = Some(PathBuf::from(path));
            }
            argument if argument.starts_with("--output=") => output = Some(PathBuf::from(&argument[9..])),
            "--help" | "-h" => { println!("usage: piper repl [-o FILE]"); return ExitCode::SUCCESS; }
            argument => return fail(&format!("unknown repl option '{argument}'"), 2),
        }
        i += 1;
    }
    ExitCode::from(piper::repl::run(output.as_deref()) as u8)
}

fn evaluate(args: &[String]) -> ExitCode {
    if args.len() != 1 { return fail("eval expects exactly one source argument", 2); }
    let mut session = piper::repl::Session::new();
    match session.execute(&args[0]) {
        Ok(status) => ExitCode::from(status.clamp(0, 255) as u8),
        Err(error) => fail(&error, 1),
    }
}

fn compile(args: &[String]) -> ExitCode {
    let mut input: Option<PathBuf> = None;
    let mut output: Option<PathBuf> = None;
    let mut opts = CompileOptions::default();
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--help" || a == "-h" { println!("{USAGE}"); return ExitCode::SUCCESS; }
        match a.as_str() {
            "-o" | "--output" => { i += 1; match args.get(i) { Some(o) => output = Some(PathBuf::from(o)), None => return fail("output option needs a path", 2) } }
            "-O0" => opts.opt = OptLevel::O0, "-O1" => opts.opt = OptLevel::O1, "-O2" => opts.opt = OptLevel::O2, "-O3" => opts.opt = OptLevel::O3,
            "-Os" => opts.opt = OptLevel::Os, "-Oz" => opts.opt = OptLevel::Oz,
            "-O" | "--opt-level" => {
                i += 1;
                opts.opt = match args.get(i).map(String::as_str) {
                    Some("0") => OptLevel::O0, Some("1") => OptLevel::O1, Some("2") => OptLevel::O2,
                    Some("3") => OptLevel::O3, Some("s") => OptLevel::Os, Some("z") => OptLevel::Oz,
                    _ => return fail("optimization level must be 0, 1, 2, 3, s, or z", 2),
                };
            }
            "--target" => { i += 1; match args.get(i) { Some(t) => opts.triple = Some(t.clone()), None => { eprintln!("piper: --target needs a triple"); return ExitCode::from(2); } } }
            "--sysroot" => { i += 1; match args.get(i) { Some(p) => opts.sysroot = Some(PathBuf::from(p)), None => { eprintln!("piper: --sysroot needs a path"); return ExitCode::from(2); } } }
            "--runtime" => { i += 1; match args.get(i) { Some(p) => opts.runtime = Some(PathBuf::from(p)), None => { eprintln!("piper: --runtime needs a path"); return ExitCode::from(2); } } }
            "-dynamic" | "--dynamic" => opts.static_libc = false,
            // Package-library output is preserved behind CompileOptions while
            // language and standard-library coverage takes priority.
            // "-lib" | "--lib" => { opts.library = true; opts.static_libc = false; }
            "--emit-ir" => opts.emit_ir = true,
            "--emit-obj" => opts.emit_object = true,
            _ if a.starts_with("--target=") => opts.triple = Some(a[9..].to_string()),
            _ if a.starts_with("--output=") => output = Some(PathBuf::from(&a[9..])),
            _ if a.starts_with('-') => return fail(&format!("unknown option '{a}'"), 2),
            _ => { if input.is_some() { return fail("only one input file is accepted", 2); } input = Some(PathBuf::from(a)); }
        }
        i += 1;
    }
    let Some(input) = input else { return fail("compile needs an input file", 2); };
    let output = output.unwrap_or_else(|| {
        let stem = input.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "a.out".into());
        if opts.emit_ir { return PathBuf::from(format!("{stem}.ll")); }
        if opts.emit_object { return PathBuf::from(format!("{stem}.o")); }
        if opts.library {
            let target = opts.triple.as_deref().and_then(|t| piper_llvm::target::Target::parse(t).ok()).unwrap_or_else(piper_llvm::target::Target::host);
            let ext = match target.os { piper_llvm::target::Os::MacOs => "dylib", piper_llvm::target::Os::Windows => "dll", piper_llvm::target::Os::Linux => "so" };
            return PathBuf::from(format!("lib{}.{}", stem.trim_start_matches("lib"), ext));
        }
        // Windows executables need their extension.
        let name = match &opts.triple {
            Some(t) => piper_llvm::target::Target::parse(t).map(|t| t.exe_name(&stem)).unwrap_or(stem),
            None => piper_llvm::target::Target::host().exe_name(&stem),
        };
        PathBuf::from(name)
    });
    match compile_file(&input, &output, &opts) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => fail(&e, 1)
    }
}

fn stdlib() -> ExitCode {
    println!("Python 3.14.7 standard library");
    println!("source modules: {}", piper_bundle::stdlib_module_count());
    println!("storage: embedded");
    ExitCode::SUCCESS
}

/// Report which targets this piper can produce executables for.
fn targets() -> ExitCode {
    use piper_llvm::target::Target;
    let host = Target::host();
    println!("host: {}", host.triple);
    println!();
    println!("{:<28} {:<10} {:<12} {}", "target", "runtime", "sysroot", "bundled");
    let names = ["aarch64-apple-macos", "x86_64-apple-macos", "x86_64-unknown-linux-musl", "aarch64-unknown-linux-musl",
                 "x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu", "x86_64-pc-windows-gnu", "aarch64-pc-windows-gnu"];
    for n in names {
        let Ok(t) = Target::parse(n) else { continue };
        let rt = match piper::runtime_archive_for(&t, None) { Ok(_) => "yes", Err(_) => "-" };
        let sr = match piper_llvm::target::Sysroot::find(&t, None) { Some(_) => "yes", None => "-" };
        let dir = t.dir_name();
        let bundled = if piper_bundle::has(&dir) { format!("{:.1} MB", piper_bundle::size_of(&dir) as f64 / 1_048_576.0) } else { "-".to_string() };
        println!("{:<28} {:<10} {:<12} {}", t.triple, rt, sr, bundled);
    }
    if !piper_llvm::link::AVAILABLE { println!("\nnote: this build has no lld, so it can only emit IR and objects"); }
    ExitCode::SUCCESS
}

fn dump_ast(path: &str) -> ExitCode {
    let src = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => { eprintln!("piper: can't open file '{path}': {e}"); return ExitCode::from(2); }
    };
    match piper_syntax::parse_module(&src, path) {
        Ok(m) => { println!("{}", piper_syntax::dump::dump(&m)); ExitCode::SUCCESS }
        Err(e) => {
            let line = src.lines().nth(e.lineno as usize - 1).unwrap_or("");
            eprintln!("  File \"{path}\", line {}\n    {}\n    {}^\nSyntaxError: {}", e.lineno, line.trim_end(), " ".repeat(e.col_offset as usize), e.msg);
            ExitCode::from(1)
        }
    }
}
