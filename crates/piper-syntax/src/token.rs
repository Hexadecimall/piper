//! Python 3.14 tokenizer. Token stream shape follows CPython's `tokenize`
//! module exactly (including f-string START/MIDDLE/END splitting), so CPython
//! can be used as an oracle.

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Tok {
    Name(String),
    Number(String),
    /// Complete literal including prefix and quotes, e.g. `rb'x'`.
    String(String),
    FStringStart(String),
    FStringMiddle(String),
    FStringEnd(String),
    TStringStart(String),
    TStringMiddle(String),
    TStringEnd(String),
    Op(String),
    Comment(String),
    Newline,
    Nl,
    Indent,
    Dedent,
    EndMarker,
}

/// `(line, col)`: line is 1-based, col is a 0-based character offset.
pub type Pos = (u32, u32);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Token {
    pub tok: Tok,
    pub start: Pos,
    pub end: Pos,
    /// UTF-8 byte offsets of `start.1` / `end.1` within their lines; the
    /// AST's `col_offset` convention.
    pub start_byte: u32,
    pub end_byte: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TokenizeError {
    pub msg: String,
    pub line: u32,
    pub col: u32,
}

impl fmt::Display for TokenizeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}: {}", self.line, self.col, self.msg)
    }
}
impl std::error::Error for TokenizeError {}

pub fn tokenize(src: &str) -> Result<Vec<Token>, TokenizeError> {
    let (toks, err) = tokenize_lenient(src);
    match err { Some(e) => Err(e), None => Ok(toks) }
}

/// Tokenize as far as possible. On error, returns the tokens produced
/// before it plus the error, so a parser can report whichever problem
/// comes first in the source.
pub fn tokenize_lenient(src: &str) -> (Vec<Token>, Option<TokenizeError>) {
    let mut lx = Lexer::new(src);
    match lx.run_inner() {
        Ok(()) => (lx.out, None),
        Err(e) => {
            let p = (lx.line, 0);
            lx.out.push(Token { tok: Tok::EndMarker, start: p, end: p, start_byte: 0, end_byte: 0 });
            (lx.out, Some(e))
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind { F, T }

/// One open f-/t-string. `{` inside it pushes onto `parens` like a bracket.
struct FMode {
    kind: Kind,
    quote: &'static str,
    raw: bool,
    /// Bracket depth when the string opened. Depth returns here when an
    /// expression part closes.
    base_depth: usize,
    /// Nesting of `{` belonging to this string (1 while inside `{expr}`).
    braces: usize,
    /// Bracket depths at which a format spec is active, innermost last.
    /// Nested fields inside a spec may open their own spec.
    specs: Vec<usize>,
}

impl FMode {
    fn in_spec(&self, depth: usize) -> bool { self.specs.last() == Some(&depth) }
}

struct Lexer<'a> {
    src: &'a [char],
    i: usize,
    line: u32,
    col: u32,
    bcol: u32,
    out: Vec<Token>,
    indents: Vec<u32>,
    parens: Vec<(char, Pos)>,
    fmodes: Vec<FMode>,
    at_line_start: bool,
    chars: Vec<char>,
    /// Char index where each line starts (line 1 at index 0).
    line_starts: Vec<usize>,
}

const OPS3: &[&str] = &["**=", "//=", ">>=", "<<=", "..."];
const OPS2: &[&str] = &[
    "**", "//", ">>", "<<", "<=", ">=", "==", "!=", "->", ":=", "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=", "@=",
];
const OPS1: &str = "+-*/%@&|^~<>()[]{},:;=.!";

impl<'a> Lexer<'a> {
    fn new(src: &'a str) -> Self {
        let chars: Vec<char> = src.chars().collect();
        let mut lx = Lexer {
            src: &[],
            i: 0,
            line: 1,
            col: 0,
            bcol: 0,
            out: Vec::new(),
            indents: vec![0],
            parens: Vec::new(),
            fmodes: Vec::new(),
            at_line_start: true,
            line_starts: {
                let mut v = vec![0];
                for (i, &c) in chars.iter().enumerate() { if c == '\n' { v.push(i + 1); } }
                v
            },
            chars,
        };
        // Self-referential workaround: keep chars owned, take a raw view.
        lx.src = unsafe { std::slice::from_raw_parts(lx.chars.as_ptr(), lx.chars.len()) };
        lx
    }

    fn pos(&self) -> Pos { (self.line, self.col) }
    fn peek(&self) -> Option<char> { self.src.get(self.i).copied() }
    fn peek_at(&self, n: usize) -> Option<char> { self.src.get(self.i + n).copied() }
    fn starts_with(&self, s: &str) -> bool {
        let mut j = self.i;
        for c in s.chars() {
            if self.src.get(j) != Some(&c) { return false; }
            j += 1;
        }
        true
    }
    fn bump(&mut self) -> Option<char> {
        let c = self.peek()?;
        self.i += 1;
        if c == '\n' { self.line += 1; self.col = 0; self.bcol = 0; } else { self.col += 1; self.bcol += c.len_utf8() as u32; }
        Some(c)
    }
    fn err<T>(&self, msg: impl Into<String>) -> Result<T, TokenizeError> {
        Err(TokenizeError { msg: msg.into(), line: self.line, col: self.col })
    }
    fn push(&mut self, tok: Tok, start: Pos, end: Pos) {
        let start_byte = self.byte_col(start);
        let end_byte = self.byte_col(end);
        self.out.push(Token { tok, start, end, start_byte, end_byte });
    }
    /// Byte column for a (line, char col) position, measured from the line start.
    fn byte_col(&self, p: Pos) -> u32 {
        if p == (self.line, self.col) { return self.bcol; }
        let ls = self.line_starts[(p.0 as usize - 1).min(self.line_starts.len() - 1)];
        self.src[ls..].iter().take(p.1 as usize).map(|c| c.len_utf8() as u32).sum()
    }
    fn text(&self, from: usize) -> String { self.src[from..self.i].iter().collect() }

    fn in_fstring_literal(&self) -> bool {
        // Inside f-string text (not inside its `{expr}`) when the innermost
        // fmode has no open braces, or is in a format spec at the current depth.
        match self.fmodes.last() {
            None => false,
            Some(m) => m.braces == 0 || m.in_spec(self.parens.len()),
        }
    }

    fn run_inner(&mut self) -> Result<(), TokenizeError> {
        loop {
            if self.in_fstring_literal() {
                self.lex_fstring_middle()?;
                continue;
            }
            if self.at_line_start {
                self.at_line_start = false;
                if self.parens.is_empty() && self.handle_indentation()? { continue; }
            }
            match self.peek() {
                None => break,
                Some(c) => self.lex_one(c)?,
            }
        }
        if let Some(m) = self.fmodes.last() {
            let _ = m;
            return self.err("unterminated f-string literal");
        }
        if let Some(&(c, pos)) = self.parens.last() {
            return Err(TokenizeError { msg: format!("'{c}' was never closed"), line: pos.0, col: pos.1 });
        }
        // A file not ending in a newline still ends its last logical line.
        let needs_newline = matches!(
            self.out.last(),
            Some(Token { tok, .. }) if !matches!(tok, Tok::Newline | Tok::Nl | Tok::Dedent | Tok::Indent)
        );
        if needs_newline {
            let p = self.pos();
            self.push(Tok::Newline, p, (p.0, p.1 + 1));
        }
        while self.indents.len() > 1 {
            self.indents.pop();
            let p = (self.line, 0);
            self.push(Tok::Dedent, p, p);
        }
        let p = (self.line, 0);
        self.push(Tok::EndMarker, p, p);
        Ok(())
    }

    /// Handles leading whitespace of a physical line. Returns true if the
    /// line was consumed entirely (blank / comment-only).
    fn handle_indentation(&mut self) -> Result<bool, TokenizeError> {
        let mut width: u32 = 0;
        let save_i = self.i;
        let save_col = self.col;
        while let Some(c) = self.peek() {
            match c {
                ' ' => width += 1,
                '\t' => width = (width / 8 + 1) * 8,
                '\x0c' => width = 0,
                _ => break,
            }
            self.bump();
        }
        match self.peek() {
            None => {
                self.i = save_i; self.col = save_col;
                return Ok(false);
            }
            Some('\n') | Some('\r') => {
                let s = self.pos();
                let from = self.i;
                self.eat_newline();
                self.push(Tok::Nl, s, (s.0, s.1 + (self.i - from) as u32));
                self.at_line_start = true;
                return Ok(true);
            }
            Some('#') => {
                let s = self.pos();
                let from = self.i;
                while let Some(c) = self.peek() { if c == '\n' || c == '\r' { break; } self.bump(); }
                let t = self.text(from);
                self.push(Tok::Comment(t), s, self.pos());
                let s2 = self.pos();
                let from = self.i;
                if self.peek().is_some() { self.eat_newline(); }
                self.push(Tok::Nl, s2, (s2.0, s2.1 + (self.i - from).max(1) as u32));
                self.at_line_start = true;
                return Ok(true);
            }
            Some('\\') if self.peek_at(1) == Some('\n') => {}
            _ => {}
        }
        let cur = *self.indents.last().unwrap();
        if width > cur {
            self.indents.push(width);
            self.push(Tok::Indent, (self.line, 0), self.pos());
        } else if width < cur {
            while width < *self.indents.last().unwrap() {
                self.indents.pop();
                let p = self.pos();
                self.push(Tok::Dedent, p, p);
            }
            if width != *self.indents.last().unwrap() {
                return self.err("unindent does not match any outer indentation level");
            }
        }
        Ok(false)
    }

    fn eat_newline(&mut self) {
        if self.peek() == Some('\r') { self.i += 1; if self.peek() != Some('\n') { self.line += 1; self.col = 0; self.bcol = 0; return; } }
        self.bump();
    }

    fn lex_one(&mut self, c: char) -> Result<(), TokenizeError> {
        match c {
            ' ' | '\t' | '\x0c' => { self.bump(); Ok(()) }
            '\n' | '\r' => {
                let s = self.pos();
                let from = self.i;
                self.eat_newline();
                let tok = if self.parens.is_empty() { Tok::Newline } else { Tok::Nl };
                self.push(tok, s, (s.0, s.1 + (self.i - from) as u32));
                self.at_line_start = true;
                Ok(())
            }
            '\\' => {
                if self.peek_at(1) == Some('\n') || self.peek_at(1) == Some('\r') {
                    self.bump();
                    self.eat_newline();
                    Ok(())
                } else if self.peek_at(1).is_none() {
                    self.err("unexpected EOF while parsing")
                } else {
                    self.err("unexpected character after line continuation character")
                }
            }
            '#' => {
                let s = self.pos();
                let from = self.i;
                while let Some(c) = self.peek() { if c == '\n' || c == '\r' { break; } self.bump(); }
                let t = self.text(from);
                self.push(Tok::Comment(t), s, self.pos());
                Ok(())
            }
            '0'..='9' => self.lex_number(),
            '.' if self.peek_at(1).is_some_and(|d| d.is_ascii_digit()) => self.lex_number(),
            '\'' | '"' => self.lex_string(self.i),
            c if is_id_start(c) => self.lex_name_or_prefixed_string(),
            _ => self.lex_op(),
        }
    }

    fn lex_name_or_prefixed_string(&mut self) -> Result<(), TokenizeError> {
        let start = self.pos();
        let start_b = self.bcol;
        let from = self.i;
        while let Some(c) = self.peek() { if is_id_continue(c) { self.bump(); } else { break; } }
        // String prefix?
        if matches!(self.peek(), Some('\'') | Some('"')) {
            let prefix: String = self.text(from).to_ascii_lowercase();
            if is_string_prefix(&prefix) {
                self.i = from; self.col = start.1; self.line = start.0; self.bcol = start_b;
                return self.lex_string(from);
            }
        }
        let t = self.text(from);
        self.push(Tok::Name(t), start, self.pos());
        Ok(())
    }

    fn lex_number(&mut self) -> Result<(), TokenizeError> {
        let start = self.pos();
        let from = self.i;
        let digits = |lx: &mut Self, pred: fn(char) -> bool| {
            while let Some(c) = lx.peek() {
                if pred(c) || (c == '_' && lx.peek_at(1).is_some_and(pred)) { lx.bump(); } else { break; }
            }
        };
        if self.peek() == Some('0') && matches!(self.peek_at(1), Some('x'|'X'|'o'|'O'|'b'|'B')) {
            let kind = self.peek_at(1).unwrap().to_ascii_lowercase();
            self.bump(); self.bump();
            if self.peek() == Some('_') { self.bump(); }
            match kind {
                'x' => digits(self, |c| c.is_ascii_hexdigit()),
                'o' => digits(self, |c| ('0'..='7').contains(&c)),
                _ => digits(self, |c| c == '0' || c == '1'),
            }
        } else {
            digits(self, |c| c.is_ascii_digit());
            let mut is_float = false;
            if self.peek() == Some('.') {
                is_float = true;
                self.bump();
                digits(self, |c| c.is_ascii_digit());
            }
            if matches!(self.peek(), Some('e' | 'E')) {
                let mut n = 1;
                if matches!(self.peek_at(1), Some('+' | '-')) { n = 2; }
                if self.peek_at(n).is_some_and(|c| c.is_ascii_digit()) {
                    is_float = true;
                    for _ in 0..n { self.bump(); }
                    digits(self, |c| c.is_ascii_digit());
                }
            }
            let _ = is_float;
            if matches!(self.peek(), Some('j' | 'J')) { self.bump(); }
        }
        let t = self.text(from);
        self.push(Tok::Number(t), start, self.pos());
        Ok(())
    }

    fn lex_op(&mut self) -> Result<(), TokenizeError> {
        let start = self.pos();
        // At the top level of a replacement field `:` always begins the
        // format spec, even when followed by `=`.
        if self.peek() == Some(':') && self.starts_fstring_spec() {
            return self.start_fstring_spec();
        }
        for op in OPS3 {
            if self.starts_with(op) {
                for _ in 0..3 { self.bump(); }
                self.push(Tok::Op(op.to_string()), start, self.pos());
                return Ok(());
            }
        }
        for op in OPS2 {
            if self.starts_with(op) {
                for _ in 0..2 { self.bump(); }
                self.push(Tok::Op(op.to_string()), start, self.pos());
                return Ok(());
            }
        }
        let c = self.peek().unwrap();
        if OPS1.contains(c) {
            match c {
                '(' | '[' | '{' => { let p = self.pos(); self.parens.push((c, p)); }
                ')' | ']' | '}' => {
                    // A `}` may close an f-string replacement field.
                    if c == '}' && self.closes_fstring_field() {
                        return self.close_fstring_field();
                    }
                    let want = match c { ')' => '(', ']' => '[', _ => '{' };
                    match self.parens.pop() {
                        Some((o, _)) if o == want => {}
                        Some((o, _)) => return self.err(format!("closing parenthesis '{c}' does not match opening parenthesis '{o}'")),
                        None => return self.err(format!("unmatched '{c}'")),
                    }
                }
                ':' if self.starts_fstring_spec() => return self.start_fstring_spec(),
                _ => {}
            }
            self.bump();
            self.push(Tok::Op(c.to_string()), start, self.pos());
            return Ok(());
        }
        self.err(format!("invalid character '{c}' (U+{:04X})", c as u32))
    }

    // ---- f-strings -------------------------------------------------------

    fn lex_string(&mut self, from: usize) -> Result<(), TokenizeError> {
        let start = self.pos();
        // Prefix
        let mut prefix = String::new();
        while let Some(c) = self.peek() { if c == '\'' || c == '"' { break; } prefix.push(c.to_ascii_lowercase()); self.bump(); }
        let q = self.peek().unwrap();
        let triple = self.peek_at(1) == Some(q) && self.peek_at(2) == Some(q);
        let quote: &'static str = match (q, triple) {
            ('\'', false) => "'", ('"', false) => "\"", ('\'', true) => "'''", _ => "\"\"\"",
        };
        for _ in 0..quote.len() { self.bump(); }
        let kind = if prefix.contains('f') { Some(Kind::F) } else if prefix.contains('t') { Some(Kind::T) } else { None };
        let raw = prefix.contains('r');
        if let Some(kind) = kind {
            let t = self.text(from);
            let tok = match kind { Kind::F => Tok::FStringStart(t), Kind::T => Tok::TStringStart(t) };
            self.push(tok, start, self.pos());
            self.fmodes.push(FMode { kind, quote, raw, base_depth: self.parens.len(), braces: 0, specs: Vec::new() });
            return Ok(());
        }
        // Plain string body
        loop {
            match self.peek() {
                None => return Err(TokenizeError { msg: "unterminated string literal".into(), line: start.0, col: start.1 }),
                Some('\\') => { self.bump(); if self.peek().is_some() { self.bump(); } }
                Some('\n') | Some('\r') if !triple => {
                    return Err(TokenizeError { msg: "unterminated string literal".into(), line: start.0, col: start.1 });
                }
                Some(_) if self.starts_with(quote) => { for _ in 0..quote.len() { self.bump(); } break; }
                Some(_) => { self.bump(); }
            }
        }
        let t = self.text(from);
        self.push(Tok::String(t), start, self.pos());
        Ok(())
    }

    fn fmode_middle_tok(kind: Kind, s: String) -> Tok {
        match kind { Kind::F => Tok::FStringMiddle(s), Kind::T => Tok::TStringMiddle(s) }
    }

    /// Lex literal text of the innermost f-string until `{`, `}` (in a spec),
    /// or the closing quote.
    fn lex_fstring_middle(&mut self) -> Result<(), TokenizeError> {
        let (kind, quote, raw, in_spec) = {
            let m = self.fmodes.last().unwrap();
            (m.kind, m.quote, m.raw, m.in_spec(self.parens.len()))
        };
        let triple = quote.len() == 3;
        let start = self.pos();
        let mut buf = String::new();
        // Middle tokens end just before `{`/`}`/quote; the emitted `end` is
        // the position after the last literal char consumed (CPython reports
        // the range of source consumed, including the skipped half of `{{`).
        loop {
            match self.peek() {
                None => return Err(TokenizeError { msg: "unterminated f-string literal".into(), line: start.0, col: start.1 }),
                Some('\n') | Some('\r') if !triple => {
                    return Err(TokenizeError { msg: "unterminated f-string literal".into(), line: start.0, col: start.1 });
                }
                Some('\\') => {
                    // Escapes are kept verbatim in the middle token; `\{` is not
                    // special. In raw mode only a following quote or backslash
                    // is consumed with it (it still cannot terminate the string).
                    buf.push('\\'); self.bump();
                    if !raw && self.peek() == Some('N') && self.peek_at(1) == Some('{') {
                        // `\N{NAME}`: the braces belong to the escape. CPython
                        // flushes the middle right after it, and stops early
                        // at a stray `{`.
                        buf.push('N'); self.bump();
                        buf.push('{'); self.bump();
                        while let Some(c) = self.peek() {
                            if c == '{' { break; }
                            buf.push(c); self.bump();
                            if c == '}' { break; }
                        }
                        self.push(Self::fmode_middle_tok(kind, std::mem::take(&mut buf)), start, self.pos());
                        return Ok(());
                    }
                    if let Some(c) = self.peek() {
                        let take = if raw { c == '\\' || c == '\'' || c == '"' } else { c != '{' && c != '}' };
                        if take { buf.push(c); self.bump(); }
                    }
                }
                Some('{') => {
                    if !in_spec && self.peek_at(1) == Some('{') {
                        buf.push('{'); self.bump();
                        let end = self.pos();
                        self.bump();
                        // CPython flushes the middle at a doubled brace.
                        self.push(Self::fmode_middle_tok(kind, std::mem::take(&mut buf)), start, end);
                        return Ok(());
                    }
                    if !buf.is_empty() {
                        self.push(Self::fmode_middle_tok(kind, std::mem::take(&mut buf)), start, self.pos());
                    }
                    let s = self.pos();
                    self.bump();
                    self.parens.push(('{', s));
                    let m = self.fmodes.last_mut().unwrap();
                    m.braces += 1;
                    self.push(Tok::Op("{".into()), s, self.pos());
                    return Ok(());
                }
                Some('}') => {
                    if in_spec {
                        // End of format spec: emit any middle, then the `}`
                        // closes the field.
                        self.push(Self::fmode_middle_tok(kind, std::mem::take(&mut buf)), start, self.pos());
                        self.fmodes.last_mut().unwrap().specs.pop();
                        return self.close_fstring_field();
                    }
                    if self.peek_at(1) == Some('}') {
                        buf.push('}'); self.bump();
                        let end = self.pos();
                        self.bump();
                        self.push(Self::fmode_middle_tok(kind, std::mem::take(&mut buf)), start, end);
                        return Ok(());
                    }
                    return self.err("f-string: single '}' is not allowed");
                }
                Some(_) if self.starts_with(quote) => {
                    if !buf.is_empty() {
                        self.push(Self::fmode_middle_tok(kind, std::mem::take(&mut buf)), start, self.pos());
                    }
                    let s = self.pos();
                    for _ in 0..quote.len() { self.bump(); }
                    let m = self.fmodes.pop().unwrap();
                    let tok = match m.kind { Kind::F => Tok::FStringEnd(quote.into()), Kind::T => Tok::TStringEnd(quote.into()) };
                    self.push(tok, s, self.pos());
                    return Ok(());
                }
                Some(c) => { buf.push(c); self.bump(); }
            }
        }
    }

    /// True when the `}` at the cursor closes the innermost f-string field.
    fn closes_fstring_field(&self) -> bool {
        match self.fmodes.last() {
            Some(m) if m.braces > 0 => matches!(self.parens.last(), Some(('{', _))) && self.parens.len() == m.base_depth + m.braces,
            _ => false,
        }
    }

    fn close_fstring_field(&mut self) -> Result<(), TokenizeError> {
        let s = self.pos();
        self.bump();
        self.parens.pop();
        self.fmodes.last_mut().unwrap().braces -= 1;
        // If the enclosing field has a spec active at this depth, spec text
        // resumes and CPython emits a (possibly empty) middle before its `}`.
        self.push(Tok::Op("}".into()), s, self.pos());
        Ok(())
    }

    /// `:` at the top level of a replacement field starts a format spec.
    fn starts_fstring_spec(&self) -> bool {
        match self.fmodes.last() {
            Some(m) if m.braces > 0 && !m.in_spec(self.parens.len()) => self.parens.len() == m.base_depth + m.braces,
            _ => false,
        }
    }

    fn start_fstring_spec(&mut self) -> Result<(), TokenizeError> {
        let s = self.pos();
        self.bump();
        self.push(Tok::Op(":".into()), s, self.pos());
        let depth = self.parens.len();
        self.fmodes.last_mut().unwrap().specs.push(depth);
        Ok(())
    }
}

fn is_string_prefix(p: &str) -> bool {
    matches!(p, "r" | "u" | "b" | "br" | "rb" | "f" | "fr" | "rf" | "t" | "tr" | "rt")
}

use crate::unicode_id::{is_id_continue, is_id_start};
