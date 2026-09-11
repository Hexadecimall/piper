use piper_llvm::Jit;
use piper_syntax::ast::{Mod, StmtKind};
use piper_syntax::token::Tok;
use std::fs::File;
use std::io::{self, BufRead, IsTerminal, Write};
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

const RESET: &str = "\x1b[0m";
const KEYWORD: &str = "\x1b[38;5;213m";
const STRING: &str = "\x1b[38;5;114m";
const NUMBER: &str = "\x1b[38;5;81m";
const OPERATOR: &str = "\x1b[38;5;220m";
const COMMENT: &str = "\x1b[3;38;5;244m";

fn is_keyword(name: &str) -> bool {
    matches!(name, "False" | "None" | "True" | "and" | "as" | "assert" | "async" | "await" |
        "break" | "case" | "class" | "continue" | "def" | "del" | "elif" | "else" | "except" |
        "finally" | "for" | "from" | "global" | "if" | "import" | "in" | "is" | "lambda" |
        "match" | "nonlocal" | "not" | "or" | "pass" | "raise" | "return" | "try" | "type" |
        "while" | "with" | "yield")
}

fn token_color(token: &Tok) -> Option<&'static str> {
    match token {
        Tok::Name(name) if is_keyword(name) => Some(KEYWORD),
        Tok::Number(_) => Some(NUMBER),
        Tok::String(_) | Tok::FStringStart(_) | Tok::FStringMiddle(_) | Tok::FStringEnd(_) |
            Tok::TStringStart(_) | Tok::TStringMiddle(_) | Tok::TStringEnd(_) => Some(STRING),
        Tok::Op(_) => Some(OPERATOR),
        Tok::Comment(_) => Some(COMMENT),
        _ => None,
    }
}

pub fn highlight(source: &str) -> String {
    let (tokens, _) = piper_syntax::token::tokenize_lenient(source);
    let mut starts = vec![0usize];
    for (offset, byte) in source.bytes().enumerate() {
        if byte == b'\n' { starts.push(offset + 1); }
    }
    let mut spans = Vec::new();
    for token in tokens {
        let Some(color) = token_color(&token.tok) else { continue };
        let line = token.start.0.saturating_sub(1) as usize;
        let end_line = token.end.0.saturating_sub(1) as usize;
        let Some(&line_start) = starts.get(line) else { continue };
        let Some(&end_start) = starts.get(end_line) else { continue };
        let start = line_start + token.start_byte as usize;
        let end = end_start + token.end_byte as usize;
        if start < end && end <= source.len() { spans.push((start, end, color)); }
    }
    spans.sort_by_key(|span| span.0);
    let mut rendered = String::with_capacity(source.len() + spans.len() * 16);
    let mut cursor = 0;
    for (start, end, color) in spans {
        if start < cursor { continue; }
        rendered.push_str(&source[cursor..start]);
        rendered.push_str(color);
        rendered.push_str(&source[start..end]);
        rendered.push_str(RESET);
        cursor = end;
    }
    rendered.push_str(&source[cursor..]);
    rendered
}

fn read_line(input: &mut impl BufRead, output: &mut impl Write, prompt: &str, color: bool) -> io::Result<Option<String>> {
    write!(output, "{prompt}")?;
    output.flush()?;
    let mut line = String::new();
    if input.read_line(&mut line)? == 0 { return Ok(None); }
    if color {
        let visible = line.strip_suffix('\n').unwrap_or(&line);
        write!(output, "\x1b[1A\r\x1b[2K{prompt}{}\n", highlight(visible))?;
        output.flush()?;
    }
    Ok(Some(line))
}

fn record(file: &mut Option<File>, source: &str) -> io::Result<()> {
    let Some(file) = file else { return Ok(()); };
    file.write_all(source.as_bytes())?;
    if !source.ends_with('\n') { file.write_all(b"\n")?; }
    file.write_all(b"\n")
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

pub fn run(record_path: Option<&Path>) -> i32 {
    let stdin = io::stdin();
    let mut input = stdin.lock();
    let mut output = io::stdout();
    let color = output.is_terminal() && std::env::var_os("NO_COLOR").is_none();
    let primary = if color { "\x1b[38;5;81m>>>\x1b[0m " } else { ">>> " };
    let continuation = if color { "\x1b[38;5;244m...\x1b[0m " } else { "... " };
    let mut session = Session::new();
    let mut recording = match record_path {
        Some(path) => match File::create(path) {
            Ok(file) => Some(file),
            Err(error) => { eprintln!("cannot create '{}': {error}", path.display()); return 1; }
        },
        None => None,
    };
    println!("Piper {} — Python 3.14.7", env!("CARGO_PKG_VERSION"));
    loop {
        let Some(mut source) = read_line(&mut input, &mut output, primary, color).unwrap_or(None) else { break; };
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
            let Some(line) = read_line(&mut input, &mut output, continuation, color).unwrap_or(None) else { break; };
            source.push_str(&line);
            if line.trim().is_empty() { break; }
            if !suite && !needs_more(&source) { break; }
        }
        match session.execute(&source) {
            Ok(_) => if let Err(error) = record(&mut recording, &source) { eprintln!("cannot record REPL input: {error}"); return 1; },
            Err(error) => eprintln!("{error}"),
        }
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
    fn syntax_highlighting_distinguishes_python_tokens() {
        let rendered = highlight("def answer(x): return x + 42 # result");
        assert!(rendered.contains(&format!("{KEYWORD}def{RESET}")));
        assert!(rendered.contains(&format!("{NUMBER}42{RESET}")));
        assert!(rendered.contains(&format!("{COMMENT}# result{RESET}")));
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
