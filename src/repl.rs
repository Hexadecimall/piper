use piper_llvm::Jit;
use piper_syntax::ast::{Mod, StmtKind};
use piper_syntax::token::Tok;
use std::fs::{File, OpenOptions};
use std::collections::BTreeSet;
use std::io::{self, BufRead, IsTerminal, Read, Write};
use std::path::{Path, PathBuf};

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
const BUILTIN: &str = "\x1b[38;5;117m";

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
        Tok::Name(name) if COMPLETIONS.contains(&name.as_str()) => Some(BUILTIN),
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

fn read_plain_line(input: &mut impl BufRead, output: &mut impl Write, prompt: &str, color: bool) -> io::Result<Option<String>> {
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

const COMPLETIONS: &[&str] = &[
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "case", "class",
    "continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "match", "nonlocal", "not", "or", "pass", "raise", "return",
    "try", "type", "while", "with", "yield", "abs", "all", "any", "ascii", "bin", "bool",
    "breakpoint", "bytearray", "bytes", "callable", "chr", "classmethod", "compile", "complex", "delattr",
    "dict", "dir", "divmod", "enumerate", "eval", "exec", "filter", "float", "format", "frozenset",
    "getattr", "globals", "hasattr", "hash", "help", "hex", "id", "input", "int", "isinstance",
    "issubclass", "iter", "len", "list", "locals", "map", "max", "memoryview", "min", "next",
    "object", "oct", "open", "ord", "pow", "print", "property", "range", "repr", "reversed", "round",
    "set", "setattr", "slice", "sorted", "staticmethod", "str", "sum", "super", "tuple", "type", "vars", "zip",
];

struct RawTerminal {
    state: [u8; 256],
    active: bool,
}

impl RawTerminal {
    fn begin() -> Self {
        let mut terminal = Self { state: [0; 256], active: false };
        terminal.active = unsafe { piper_rt::ffi::piper_terminal_raw_begin(0, terminal.state.as_mut_ptr().cast(), terminal.state.len()) == 0 };
        terminal
    }
}

impl Drop for RawTerminal {
    fn drop(&mut self) {
        if self.active { unsafe { piper_rt::ffi::piper_terminal_raw_end(0, self.state.as_mut_ptr().cast()); } }
    }
}

fn previous_boundary(value: &str, cursor: usize) -> usize {
    value[..cursor].char_indices().next_back().map(|(index, _)| index).unwrap_or(0)
}

fn next_boundary(value: &str, cursor: usize) -> usize {
    value[cursor..].char_indices().nth(1).map(|(index, _)| cursor + index).unwrap_or(value.len())
}

fn previous_word(value: &str, mut cursor: usize) -> usize {
    while cursor > 0 {
        let previous = previous_boundary(value, cursor);
        if !value[previous..cursor].chars().all(char::is_whitespace) { break; }
        cursor = previous;
    }
    while cursor > 0 {
        let previous = previous_boundary(value, cursor);
        if !value[previous..cursor].chars().all(|character| character.is_alphanumeric() || character == '_') { break; }
        cursor = previous;
    }
    cursor
}

fn next_word(value: &str, mut cursor: usize) -> usize {
    while cursor < value.len() && value[cursor..].chars().next().is_some_and(|character| character.is_alphanumeric() || character == '_') {
        cursor = next_boundary(value, cursor);
    }
    while cursor < value.len() && value[cursor..].chars().next().is_some_and(char::is_whitespace) { cursor = next_boundary(value, cursor); }
    cursor
}

fn redraw(output: &mut impl Write, prompt: &str, value: &str, cursor: usize, color: bool) -> io::Result<()> {
    write!(output, "\r\x1b[2K{prompt}{}", if color { highlight(value) } else { value.to_string() })?;
    let right = value[cursor..].chars().count();
    if right > 0 { write!(output, "\x1b[{right}D")?; }
    output.flush()
}

fn complete(value: &mut String, cursor: &mut usize, learned: &BTreeSet<String>) {
    let start = value[..*cursor].char_indices().rev().find(|(_, character)| !character.is_alphanumeric() && *character != '_')
        .map(|(index, character)| index + character.len_utf8()).unwrap_or(0);
    let prefix = &value[start..*cursor];
    if prefix.is_empty() { return; }
    let candidate = COMPLETIONS.iter().copied().chain(learned.iter().map(String::as_str))
        .filter(|candidate| candidate.starts_with(prefix) && *candidate != prefix).min();
    if let Some(candidate) = candidate {
        value.replace_range(start..*cursor, candidate);
        *cursor = start + candidate.len();
    }
}

fn read_interactive_line(input: &mut impl Read, output: &mut impl Write, prompt: &str, color: bool,
                         history: &[String], learned: &BTreeSet<String>, initial: &str) -> io::Result<Option<String>> {
    let terminal = RawTerminal::begin();
    if !terminal.active { return Ok(None); }
    let mut value = initial.to_string();
    let mut cursor = value.len();
    let mut history_index = history.len();
    redraw(output, prompt, &value, cursor, color)?;
    loop {
        let mut byte = [0u8; 1];
        if input.read(&mut byte)? == 0 { write!(output, "\r\n")?; return Ok(None); }
        match byte[0] {
            b'\r' | b'\n' => { write!(output, "\r\n")?; output.flush()?; return Ok(Some(value + "\n")); }
            1 => cursor = 0,
            5 => cursor = value.len(),
            12 => { write!(output, "\x1b[2J\x1b[H")?; }
            3 => { write!(output, "^C\r\n")?; output.flush()?; return Ok(Some("\n".into())); }
            4 if value.is_empty() => { write!(output, "\r\n")?; output.flush()?; return Ok(None); }
            4 => { if cursor < value.len() { let end = next_boundary(&value, cursor); value.replace_range(cursor..end, ""); } }
            8 | 127 => if cursor > 0 { let start = previous_boundary(&value, cursor); value.replace_range(start..cursor, ""); cursor = start; },
            18 => if let Some(entry) = history.iter().rev().find(|entry| entry.contains(&value)) { value = entry.clone(); cursor = value.len(); },
            23 => if cursor > 0 { let start = previous_word(&value, cursor); value.replace_range(start..cursor, ""); cursor = start; },
            b'\t' => complete(&mut value, &mut cursor, learned),
            27 => {
                let mut first = [0u8; 1];
                if input.read_exact(&mut first).is_ok() && first[0] == b'[' {
                    let mut sequence = Vec::new();
                    while sequence.len() < 8 {
                        let mut next = [0u8; 1]; input.read_exact(&mut next)?; sequence.push(next[0]);
                        if next[0].is_ascii_alphabetic() || next[0] == b'~' { break; }
                    }
                    match sequence.as_slice() {
                        b"A" => if !history.is_empty() && history_index > 0 { history_index -= 1; value = history[history_index].clone(); cursor = value.len(); },
                        b"B" => if history_index + 1 < history.len() { history_index += 1; value = history[history_index].clone(); cursor = value.len(); } else { history_index = history.len(); value.clear(); cursor = 0; },
                        b"C" => if cursor < value.len() { cursor = next_boundary(&value, cursor); },
                        b"D" => if cursor > 0 { cursor = previous_boundary(&value, cursor); },
                        b"H" | b"1~" => cursor = 0,
                        b"F" | b"4~" => cursor = value.len(),
                        b"3~" => if cursor < value.len() { let end = next_boundary(&value, cursor); value.replace_range(cursor..end, ""); },
                        b"1;5D" | b"5D" => cursor = previous_word(&value, cursor),
                        b"1;5C" | b"5C" => cursor = next_word(&value, cursor),
                        _ => {}
                    }
                } else if first[0] == b'b' { cursor = previous_word(&value, cursor); }
                else if first[0] == b'f' { cursor = next_word(&value, cursor); }
            }
            first if first >= 32 => {
                let width = if first < 0x80 { 1 } else if first & 0xe0 == 0xc0 { 2 } else if first & 0xf0 == 0xe0 { 3 } else { 4 };
                let mut bytes = vec![first];
                for _ in 1..width { let mut next = [0u8; 1]; input.read_exact(&mut next)?; bytes.push(next[0]); }
                if let Ok(text) = std::str::from_utf8(&bytes) {
                    let closing = matches!(text, ")" | "]" | "}" | "\"" | "'") && value[cursor..].starts_with(text);
                    if closing { cursor += text.len(); }
                    else {
                        value.insert_str(cursor, text); cursor += text.len();
                        let pair = match text { "(" => Some(")"), "[" => Some("]"), "{" => Some("}"), "\"" => Some("\""), "'" => Some("'"), _ => None };
                        if let Some(pair) = pair { value.insert_str(cursor, pair); }
                    }
                }
            }
            _ => {}
        }
        redraw(output, prompt, &value, cursor, color)?;
    }
}

fn record(file: &mut Option<File>, source: &str) -> io::Result<()> {
    let Some(file) = file else { return Ok(()); };
    file.write_all(source.as_bytes())?;
    if !source.ends_with('\n') { file.write_all(b"\n")?; }
    file.write_all(b"\n")
}

fn learn_names(source: &str, learned: &mut BTreeSet<String>) {
    let (tokens, _) = piper_syntax::token::tokenize_lenient(source);
    for token in tokens {
        if let Tok::Name(name) = token.tok { if !is_keyword(&name) { learned.insert(name); } }
    }
}

fn history_path() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("PIPER_HISTORY") { return Some(PathBuf::from(path)); }
    #[cfg(target_os = "windows")]
    if let Some(path) = std::env::var_os("LOCALAPPDATA") { return Some(PathBuf::from(path).join("Piper/history")); }
    if let Some(path) = std::env::var_os("XDG_STATE_HOME") { return Some(PathBuf::from(path).join("piper/history")); }
    std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/state/piper/history"))
}

fn load_history() -> Vec<String> {
    let Some(path) = history_path() else { return Vec::new() };
    let Ok(contents) = std::fs::read_to_string(path) else { return Vec::new() };
    contents.lines().filter(|line| !line.trim().is_empty()).map(str::to_string).collect()
}

fn save_history(entry: &str) {
    let Some(path) = history_path() else { return };
    if let Some(parent) = path.parent() { if std::fs::create_dir_all(parent).is_err() { return; } }
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) { let _ = writeln!(file, "{entry}"); }
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
    let interactive = stdin.is_terminal() && io::stdout().is_terminal();
    let mut input = stdin.lock();
    let mut output = io::stdout();
    let color = output.is_terminal() && std::env::var_os("NO_COLOR").is_none();
    let primary = if color { "\x1b[38;5;81m>>>\x1b[0m " } else { ">>> " };
    let continuation = if color { "\x1b[38;5;244m...\x1b[0m " } else { "... " };
    let mut session = Session::new();
    let mut history = load_history();
    let mut learned = BTreeSet::new();
    for entry in &history { learn_names(entry, &mut learned); }
    let mut recording = match record_path {
        Some(path) => match File::create(path) {
            Ok(file) => Some(file),
            Err(error) => { eprintln!("cannot create '{}': {error}", path.display()); return 1; }
        },
        None => None,
    };
    println!("Piper {} — Python 3.14.7", env!("CARGO_PKG_VERSION"));
    loop {
        let line = if interactive { read_interactive_line(&mut input, &mut output, primary, color, &history, &learned, "") }
            else { read_plain_line(&mut input, &mut output, primary, color) };
        let Some(mut source) = line.unwrap_or(None) else { break; };
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
            let indentation: String = source.lines().last().unwrap_or("").chars().take_while(|character| character.is_whitespace()).collect();
            let indent = if source.trim_end().ends_with(':') { format!("{indentation}    ") } else { indentation };
            let line = if interactive { read_interactive_line(&mut input, &mut output, continuation, color, &history, &learned, &indent) }
                else { read_plain_line(&mut input, &mut output, continuation, color) };
            let Some(line) = line.unwrap_or(None) else { break; };
            source.push_str(&line);
            if line.trim().is_empty() { break; }
            if !suite && !needs_more(&source) { break; }
        }
        match session.execute(&source) {
            Ok(_) => {
                let entry = source.trim_end().to_string();
                if !entry.is_empty() && !entry.contains('\n') { save_history(&entry); history.push(entry); }
                learn_names(&source, &mut learned);
                if let Err(error) = record(&mut recording, &source) { eprintln!("cannot record REPL input: {error}"); return 1; }
            }
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
        assert!(rendered.contains(&format!("{BUILTIN}print{RESET}")) || highlight("print(42)").contains(&format!("{BUILTIN}print{RESET}")));
        assert!(rendered.contains(&format!("{NUMBER}42{RESET}")));
        assert!(rendered.contains(&format!("{COMMENT}# result{RESET}")));
    }

    #[test]
    fn completion_uses_builtins_and_learned_names() {
        let learned = BTreeSet::from(["player_choice".to_string()]);
        let mut builtin = "pri".to_string();
        let mut cursor = builtin.len();
        complete(&mut builtin, &mut cursor, &learned);
        assert_eq!(builtin, "print");
        let mut local = "player_c".to_string();
        cursor = local.len();
        complete(&mut local, &mut cursor, &learned);
        assert_eq!(local, "player_choice");
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
