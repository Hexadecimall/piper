use piper_llvm::Jit;
use piper_syntax::ast::{Mod, StmtKind};
use std::io::{self, BufRead, Write};
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    Help,
    Quit,
    Clear,
    Load(String),
}

pub fn command(line: &str) -> Result<Option<Command>, String> {
    let trimmed = line.trim();
    if !trimmed.starts_with(':') { return Ok(None); }
    let mut parts = trimmed[1..].splitn(2, char::is_whitespace);
    let name = parts.next().unwrap_or("");
    let argument = parts.next().unwrap_or("").trim();
    match name {
        "help" | "h" | "?" if argument.is_empty() => Ok(Some(Command::Help)),
        "quit" | "q" | "exit" if argument.is_empty() => Ok(Some(Command::Quit)),
        "clear" if argument.is_empty() => Ok(Some(Command::Clear)),
        "load" if !argument.is_empty() => Ok(Some(Command::Load(argument.to_string()))),
        "load" => Err(":load needs a Python file".into()),
        _ => Err(format!("unknown Piper command: :{name}")),
    }
}

pub fn needs_more(source: &str) -> bool {
    let trimmed = source.trim_end();
    if trimmed.ends_with(':') || trimmed.ends_with('\\') { return true; }
    let mut stack = Vec::new();
    let mut quote = None;
    let mut escaped = false;
    for character in source.chars() {
        if let Some(mark) = quote {
            if escaped { escaped = false; continue; }
            if character == '\\' { escaped = true; continue; }
            if character == mark { quote = None; }
            continue;
        }
        match character {
            '\'' | '"' => quote = Some(character),
            '(' | '[' | '{' => stack.push(character),
            ')' => { if stack.last() == Some(&'(') { stack.pop(); } },
            ']' => { if stack.last() == Some(&'[') { stack.pop(); } },
            '}' => { if stack.last() == Some(&'{') { stack.pop(); } },
            _ => {}
        }
    }
    quote.is_some() || !stack.is_empty()
}

fn display_source(source: &str) -> String {
    let Ok(module) = piper_syntax::parse_module(source, "<stdin>") else { return source.to_string() };
    let Mod::Module { body, .. } = module else { return source.to_string() };
    if body.len() == 1 && matches!(body[0].kind, StmtKind::Expr { .. }) {
        format!("__piper_value = ({source})\nif __piper_value is not None:\n    print(repr(__piper_value))\n")
    } else {
        source.to_string()
    }
}

pub struct Session {
    modules: Vec<Jit>,
    sequence: usize,
}

impl Session {
    pub fn new() -> Self {
        unsafe { piper_rt::ffi::piper_initialize(); }
        Self { modules: Vec::new(), sequence: 0 }
    }

    pub fn execute(&mut self, source: &str) -> Result<i32, String> {
        self.sequence += 1;
        let filename = format!("<piper:{}>", self.sequence);
        let compiled = display_source(source);
        let cg = crate::lower_source_named(&compiled, &filename, "__main__").map_err(|error| format!("SyntaxError: {error}"))?;
        let jit = cg.into_jit()?;
        let main: extern "C" fn(i32, *mut *mut i8) -> i32 = unsafe { std::mem::transmute(jit.lookup("main")?) };
        let status = main(0, std::ptr::null_mut());
        self.modules.push(jit);
        Ok(status)
    }

    pub fn load(&mut self, path: &Path) -> Result<i32, String> {
        let source = std::fs::read_to_string(path).map_err(|error| format!("cannot load '{}': {error}", path.display()))?;
        self.execute(&source)
    }
}

impl Default for Session {
    fn default() -> Self { Self::new() }
}

pub fn run() -> i32 {
    let stdin = io::stdin();
    let mut input = stdin.lock();
    let mut output = io::stdout();
    let color = std::env::var_os("NO_COLOR").is_none();
    let primary = if color { "\x1b[38;5;81m>>>\x1b[0m " } else { ">>> " };
    let continuation = if color { "\x1b[38;5;244m...\x1b[0m " } else { "... " };
    let mut session = Session::new();
    println!("Piper {} — Python 3.14.7", env!("CARGO_PKG_VERSION"));
    loop {
        let mut source = String::new();
        print!("{primary}");
        let _ = output.flush();
        if input.read_line(&mut source).unwrap_or(0) == 0 { break; }
        if source.trim().is_empty() { continue; }
        match command(&source) {
            Ok(Some(Command::Help)) => { println!(":help  show commands\n:load PATH  compile and run a file\n:clear  clear the terminal\n:quit  leave Piper"); continue; }
            Ok(Some(Command::Quit)) => break,
            Ok(Some(Command::Clear)) => { print!("\x1b[2J\x1b[H"); let _ = output.flush(); continue; }
            Ok(Some(Command::Load(path))) => { if let Err(error) = session.load(Path::new(&path)) { eprintln!("{error}"); } continue; }
            Ok(None) => {}
            Err(error) => { eprintln!("{error}"); continue; }
        }
        let suite = source.trim_end().ends_with(':');
        while suite || needs_more(&source) {
            print!("{continuation}");
            let _ = output.flush();
            let mut line = String::new();
            if input.read_line(&mut line).unwrap_or(0) == 0 { break; }
            source.push_str(&line);
            if line.trim().is_empty() { break; }
            if !suite && !needs_more(&source) { break; }
        }
        if let Err(error) = session.execute(&source) { eprintln!("{error}"); }
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn terminal_commands_are_unambiguous() {
        assert_eq!(command("x = 1"), Ok(None));
        assert_eq!(command(":help"), Ok(Some(Command::Help)));
        assert_eq!(command(":q"), Ok(Some(Command::Quit)));
        assert_eq!(command(":load demo.py"), Ok(Some(Command::Load("demo.py".into()))));
        assert!(command(":load").is_err());
        assert!(command(":wat").is_err());
    }

    #[test]
    fn continuation_tracks_blocks_and_delimiters() {
        assert!(needs_more("if True:"));
        assert!(needs_more("values = [1,"));
        assert!(!needs_more("values = [1, 2]"));
        assert!(!needs_more("print('ok')"));
    }

    #[test]
    fn jit_session_preserves_main_globals() {
        let mut session = Session::new();
        assert_eq!(session.execute("answer = 40").unwrap(), 0);
        assert_eq!(session.execute("answer += 2").unwrap(), 0);
        unsafe {
            let module = piper_rt::ffi::PyImport_AddModule(c"__main__".as_ptr());
            let globals = piper_rt::ffi::PyModule_GetDict(module);
            let answer = piper_rt::ffi::PyDict_GetItemString(globals, c"answer".as_ptr());
            assert_eq!(piper_rt::ffi::PyLong_AsLongLong(answer), 42);
        }
    }
}
