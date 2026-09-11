//! Recursive-descent parser for Python 3.14, structured after CPython's PEG
//! grammar (`Grammar/python.gram`). Produces the AST in `crate::ast`.

use crate::ast::*;
use crate::token::{Tok, Token, TokenizeError, tokenize_lenient};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseError {
    pub msg: String,
    pub lineno: u32,
    pub col_offset: u32,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}: {}", self.lineno, self.col_offset, self.msg)
    }
}
impl std::error::Error for ParseError {}

impl From<TokenizeError> for ParseError {
    fn from(e: TokenizeError) -> Self { ParseError { msg: e.msg, lineno: e.line, col_offset: e.col } }
}

type PResult<T> = Result<T, ParseError>;

/// Parse a whole file into a `Module`.
pub fn parse_module(src: &str, _filename: &str) -> PResult<Mod> {
    let (toks, tok_err) = tokenize_lenient(src);
    let comments: Vec<(u32, u32, u32)> = toks.iter()
        .filter(|t| matches!(t.tok, Tok::Comment(_)))
        .map(|t| (t.start.0, t.start_byte, t.end_byte))
        .collect();
    let toks: Vec<Token> = toks.into_iter().filter(|t| !matches!(t.tok, Tok::Comment(_) | Tok::Nl)).collect();
    let mut p = Parser::new(src, toks);
    p.comments = comments;
    match p.file() {
        Ok(body) => match tok_err {
            Some(e) => Err(e.into()),
            None => Ok(Mod::Module { body, type_ignores: Vec::new() }),
        },
        Err(e) => {
            // An error at the synthetic end marker is the tokenizer's error.
            if let Some(te) = tok_err {
                if p.i >= p.toks.len() - 1 { return Err(te.into()); }
            }
            Err(e)
        }
    }
}

struct Parser<'a> {
    src: &'a str,
    /// Byte offset of the start of each line (line 1 at index 0).
    line_starts: Vec<usize>,
    toks: Vec<Token>,
    i: usize,
    /// `(line, start_byte, end_byte)` of every comment, for debug f-strings.
    comments: Vec<(u32, u32, u32)>,
}

const KEYWORDS: &[&str] = &[
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
    "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
];

fn is_keyword(s: &str) -> bool { KEYWORDS.contains(&s) }

impl<'a> Parser<'a> {
    fn new(src: &'a str, toks: Vec<Token>) -> Self {
        let mut line_starts = vec![0];
        for (i, b) in src.bytes().enumerate() { if b == b'\n' { line_starts.push(i + 1); } }
        Parser { src, line_starts, toks, i: 0, comments: Vec::new() }
    }

    // ---- token access ----------------------------------------------------

    fn tok(&self) -> &Tok { &self.toks[self.i.min(self.toks.len() - 1)].tok }
    fn tok_at(&self, n: usize) -> &Tok { &self.toks[(self.i + n).min(self.toks.len() - 1)].tok }
    fn cur(&self) -> &Token { &self.toks[self.i.min(self.toks.len() - 1)] }
    /// Last consumed token that carries a real position (skips the
    /// layout tokens NEWLINE / INDENT / DEDENT, which end nothing).
    fn prev(&self) -> &Token {
        let mut j = self.i;
        while j > 0 {
            j -= 1;
            if !matches!(self.toks[j].tok, Tok::Newline | Tok::Indent | Tok::Dedent | Tok::EndMarker) { return &self.toks[j]; }
        }
        &self.toks[0]
    }
    fn advance(&mut self) -> &Token { let t = &self.toks[self.i.min(self.toks.len() - 1)]; if self.i < self.toks.len() { self.i += 1; } t }

    fn start(&self) -> (u32, u32) { let t = self.cur(); (t.start.0, t.start_byte) }
    fn span_from(&self, start: (u32, u32)) -> Span {
        let p = self.prev();
        Span::new(start.0, start.1, p.end.0, p.end_byte)
    }
    fn tok_span(t: &Token) -> Span { Span::new(t.start.0, t.start_byte, t.end.0, t.end_byte) }

    /// Span of an f-string literal piece. The tokenizer ends a middle token
    /// before the second half of a doubled brace; the AST covers it.
    fn middle_span(&self, t: &Token, text: &str) -> Span {
        let mut sp = Self::tok_span(t);
        if let Some(last) = text.chars().last() {
            if last == '{' || last == '}' {
                let off = self.line_starts[sp.end_lineno as usize - 1] + sp.end_col_offset as usize;
                if self.src.as_bytes().get(off) == Some(&(last as u8)) { sp.end_col_offset += 1; }
            }
        }
        sp
    }

    fn is_op(&self, s: &str) -> bool { matches!(self.tok(), Tok::Op(o) if o == s) }
    fn is_op_at(&self, n: usize, s: &str) -> bool { matches!(self.tok_at(n), Tok::Op(o) if o == s) }
    fn is_kw(&self, s: &str) -> bool { matches!(self.tok(), Tok::Name(n) if n == s) }
    fn is_kw_at(&self, n: usize, s: &str) -> bool { matches!(self.tok_at(n), Tok::Name(x) if x == s) }
    fn eat_op(&mut self, s: &str) -> bool { if self.is_op(s) { self.advance(); true } else { false } }
    fn eat_kw(&mut self, s: &str) -> bool { if self.is_kw(s) { self.advance(); true } else { false } }

    fn error<T>(&self, msg: impl Into<String>) -> PResult<T> {
        let t = self.cur();
        Err(ParseError { msg: msg.into(), lineno: t.start.0, col_offset: t.start_byte })
    }
    fn error_at<T>(&self, span: Span, msg: impl Into<String>) -> PResult<T> {
        Err(ParseError { msg: msg.into(), lineno: span.lineno, col_offset: span.col_offset })
    }
    fn expect_op(&mut self, s: &str) -> PResult<()> {
        if self.eat_op(s) { Ok(()) } else { self.error(format!("expected '{s}'")) }
    }
    fn expect_kw(&mut self, s: &str) -> PResult<()> {
        if self.eat_kw(s) { Ok(()) } else { self.error(format!("expected '{s}'")) }
    }
    fn expect_name(&mut self) -> PResult<String> {
        match self.tok().clone() {
            Tok::Name(n) if !is_keyword(&n) => { self.advance(); Ok(n) }
            _ => self.error("invalid syntax"),
        }
    }
    fn expect_newline(&mut self) -> PResult<()> {
        if matches!(self.tok(), Tok::Newline) { self.advance(); Ok(()) } else { self.error("invalid syntax") }
    }
    fn at_name(&self) -> bool { matches!(self.tok(), Tok::Name(n) if !is_keyword(n)) }

    /// Source text between two byte positions.
    fn text(&self, from: (u32, u32), to: (u32, u32)) -> &'a str {
        let a = self.line_starts[from.0 as usize - 1] + from.1 as usize;
        let b = self.line_starts[to.0 as usize - 1] + to.1 as usize;
        &self.src[a..b]
    }

    /// Source text between two positions with comments removed.
    fn text_without_comments(&self, from: (u32, u32), to: (u32, u32)) -> String {
        let a = self.line_starts[from.0 as usize - 1] + from.1 as usize;
        let b = self.line_starts[to.0 as usize - 1] + to.1 as usize;
        let mut out = String::new();
        let mut pos = a;
        for &(line, cs, ce) in &self.comments {
            let ls = self.line_starts[line as usize - 1];
            let (cs, ce) = (ls + cs as usize, ls + ce as usize);
            if cs < a || ce > b { continue; }
            out.push_str(&self.src[pos..cs]);
            pos = ce;
        }
        out.push_str(&self.src[pos..b]);
        out
    }

    // ---- file / blocks ---------------------------------------------------

    fn file(&mut self) -> PResult<Vec<Stmt>> {
        let mut body = Vec::new();
        while !matches!(self.tok(), Tok::EndMarker) {
            if matches!(self.tok(), Tok::Newline) { self.advance(); continue; }
            body.extend(self.statement()?);
        }
        Ok(body)
    }

    fn block(&mut self) -> PResult<Vec<Stmt>> {
        if matches!(self.tok(), Tok::Newline) {
            self.advance();
            if !matches!(self.tok(), Tok::Indent) { return self.error("expected an indented block"); }
            self.advance();
            let mut body = Vec::new();
            while !matches!(self.tok(), Tok::Dedent | Tok::EndMarker) {
                body.extend(self.statement()?);
            }
            if matches!(self.tok(), Tok::Dedent) { self.advance(); }
            Ok(body)
        } else {
            self.simple_stmts()
        }
    }

    fn statement(&mut self) -> PResult<Vec<Stmt>> {
        if let Some(s) = self.compound_stmt()? { return Ok(vec![s]); }
        self.simple_stmts()
    }

    fn simple_stmts(&mut self) -> PResult<Vec<Stmt>> {
        let mut out = vec![self.simple_stmt()?];
        while self.eat_op(";") {
            if matches!(self.tok(), Tok::Newline | Tok::EndMarker) { break; }
            out.push(self.simple_stmt()?);
        }
        if matches!(self.tok(), Tok::EndMarker) { return Ok(out); }
        self.expect_newline()?;
        Ok(out)
    }

    // ---- simple statements -----------------------------------------------

    fn simple_stmt(&mut self) -> PResult<Stmt> {
        let start = self.start();
        let kind = match self.tok().clone() {
            Tok::Name(n) => match n.as_str() {
                "pass" => { self.advance(); StmtKind::Pass }
                "break" => { self.advance(); StmtKind::Break }
                "continue" => { self.advance(); StmtKind::Continue }
                "return" => {
                    self.advance();
                    let value = if self.at_stmt_end() { None } else { Some(Box::new(self.star_expressions()?)) };
                    StmtKind::Return { value }
                }
                "raise" => {
                    self.advance();
                    if self.at_stmt_end() { StmtKind::Raise { exc: None, cause: None } } else {
                        let exc = Box::new(self.expression()?);
                        let cause = if self.eat_kw("from") { Some(Box::new(self.expression()?)) } else { None };
                        StmtKind::Raise { exc: Some(exc), cause }
                    }
                }
                "global" | "nonlocal" => {
                    self.advance();
                    let mut names = vec![self.expect_name()?];
                    while self.eat_op(",") { names.push(self.expect_name()?); }
                    if n == "global" { StmtKind::Global { names } } else { StmtKind::Nonlocal { names } }
                }
                "del" => {
                    self.advance();
                    let mut targets = Vec::new();
                    loop {
                        let mut t = self.del_target()?;
                        set_ctx(&mut t, ExprContext::Del);
                        targets.push(t);
                        if !self.eat_op(",") || self.at_stmt_end() { break; }
                    }
                    StmtKind::Delete { targets }
                }
                "assert" => {
                    self.advance();
                    let test = Box::new(self.expression()?);
                    let msg = if self.eat_op(",") { Some(Box::new(self.expression()?)) } else { None };
                    StmtKind::Assert { test, msg }
                }
                "import" => {
                    self.advance();
                    let mut names = Vec::new();
                    loop {
                        let s = self.start();
                        let name = self.dotted_name()?;
                        let asname = if self.eat_kw("as") { Some(self.expect_name()?) } else { None };
                        names.push(Alias { name, asname, span: self.span_from(s) });
                        if !self.eat_op(",") { break; }
                    }
                    StmtKind::Import { names }
                }
                "from" => {
                    self.advance();
                    let mut level = 0u32;
                    loop {
                        if self.eat_op(".") { level += 1; } else if self.eat_op("...") { level += 3; } else { break; }
                    }
                    let module = if self.is_kw("import") { None } else { Some(self.dotted_name()?) };
                    if module.is_none() && level == 0 { return self.error("invalid syntax"); }
                    self.expect_kw("import")?;
                    let mut names = Vec::new();
                    if self.is_op("*") {
                        let t = self.advance().clone();
                        names.push(Alias { name: "*".into(), asname: None, span: Self::tok_span(&t) });
                    } else {
                        let paren = self.eat_op("(");
                        loop {
                            let s = self.start();
                            let name = self.expect_name()?;
                            let asname = if self.eat_kw("as") { Some(self.expect_name()?) } else { None };
                            names.push(Alias { name, asname, span: self.span_from(s) });
                            if !self.eat_op(",") { break; }
                            if paren && self.is_op(")") { break; }
                        }
                        if paren { self.expect_op(")")?; }
                    }
                    StmtKind::ImportFrom { module, names, level }
                }
                "type" if self.at_name_at(1) && (self.is_op_at(2, "=") || self.is_op_at(2, "[")) => {
                    self.advance();
                    let ns = self.start();
                    let id = self.expect_name()?;
                    let name = Box::new(Expr { kind: ExprKind::Name { id, ctx: ExprContext::Store }, span: self.span_from(ns) });
                    let type_params = self.type_params_opt()?;
                    self.expect_op("=")?;
                    let value = Box::new(self.expression()?);
                    StmtKind::TypeAlias { name, type_params, value }
                }
                _ => return self.assignment_or_expr(start),
            },
            _ => return self.assignment_or_expr(start),
        };
        Ok(Stmt { kind, span: self.span_from(start) })
    }

    fn at_name_at(&self, n: usize) -> bool { matches!(self.tok_at(n), Tok::Name(x) if !is_keyword(x)) }

    fn at_stmt_end(&self) -> bool { matches!(self.tok(), Tok::Newline | Tok::EndMarker) || self.is_op(";") }

    fn dotted_name(&mut self) -> PResult<String> {
        let mut s = self.expect_name()?;
        while self.is_op(".") {
            self.advance();
            s.push('.');
            s.push_str(&self.expect_name()?);
        }
        Ok(s)
    }

    /// Assignment (plain, augmented, annotated) or an expression statement.
    fn assignment_or_expr(&mut self, start: (u32, u32)) -> PResult<Stmt> {
        let parenthesized = self.is_op("(");
        let first = if self.is_kw("yield") { self.yield_expr()? } else { self.star_expressions()? };
        // Annotated assignment
        if self.is_op(":") {
            self.advance();
            let mut target = first;
            let simple = matches!(target.kind, ExprKind::Name { .. }) && !parenthesized;
            match target.kind {
                ExprKind::Name { .. } | ExprKind::Attribute { .. } | ExprKind::Subscript { .. } => {}
                ExprKind::Tuple { .. } => return self.error_at(target.span, "only single target (not tuple) can be annotated"),
                ExprKind::List { .. } => return self.error_at(target.span, "only single target (not list) can be annotated"),
                _ => return self.error_at(target.span, "illegal target for annotation"),
            }
            set_ctx(&mut target, ExprContext::Store);
            let annotation = Box::new(self.expression()?);
            let value = if self.eat_op("=") {
                Some(Box::new(if self.is_kw("yield") { self.yield_expr()? } else { self.star_expressions()? }))
            } else { None };
            return Ok(Stmt { kind: StmtKind::AnnAssign { target: Box::new(target), annotation, value, simple }, span: self.span_from(start) });
        }
        // Augmented assignment
        if let Tok::Op(o) = self.tok().clone() {
            if let Some(op) = augassign_op(&o) {
                let mut target = first;
                match target.kind {
                    ExprKind::Name { .. } | ExprKind::Attribute { .. } | ExprKind::Subscript { .. } => {}
                    _ => return self.error_at(target.span, format!("'{}' is an illegal expression for augmented assignment", describe(&target))),
                }
                set_ctx(&mut target, ExprContext::Store);
                self.advance();
                let value = Box::new(if self.is_kw("yield") { self.yield_expr()? } else { self.star_expressions()? });
                return Ok(Stmt { kind: StmtKind::AugAssign { target: Box::new(target), op, value }, span: self.span_from(start) });
            }
        }
        // Plain assignment chain
        if self.is_op("=") {
            let mut targets = vec![first];
            let mut value;
            loop {
                self.advance();
                value = if self.is_kw("yield") { self.yield_expr()? } else { self.star_expressions()? };
                if self.is_op("=") { targets.push(value); } else { break; }
            }
            for t in &mut targets { self.check_target(t)?; set_ctx(t, ExprContext::Store); }
            return Ok(Stmt { kind: StmtKind::Assign { targets, value: Box::new(value), type_comment: None }, span: self.span_from(start) });
        }
        Ok(Stmt { kind: StmtKind::Expr { value: Box::new(first) }, span: self.span_from(start) })
    }

    fn check_target(&self, e: &Expr) -> PResult<()> {
        match &e.kind {
            ExprKind::Name { id, .. } => {
                if id == "__debug__" { return self.error_at(e.span, "cannot assign to __debug__"); }
                Ok(())
            }
            ExprKind::Attribute { .. } | ExprKind::Subscript { .. } => Ok(()),
            ExprKind::Starred { value, .. } => self.check_target(value),
            ExprKind::List { elts, .. } | ExprKind::Tuple { elts, .. } => { for x in elts { self.check_target(x)?; } Ok(()) }
            _ => self.error_at(e.span, format!("cannot assign to {} here. Maybe you meant '==' instead of '='?", describe(e))),
        }
    }

    fn del_target(&mut self) -> PResult<Expr> {
        let e = self.bitwise_or()?;
        match &e.kind {
            ExprKind::Name { .. } | ExprKind::Attribute { .. } | ExprKind::Subscript { .. } | ExprKind::List { .. } | ExprKind::Tuple { .. } => Ok(e),
            _ => self.error_at(e.span, format!("cannot delete {}", describe(&e))),
        }
    }

    // ---- compound statements ---------------------------------------------

    fn compound_stmt(&mut self) -> PResult<Option<Stmt>> {
        let start = self.start();
        let Tok::Name(n) = self.tok().clone() else { return self.decorated_or_none(start) };
        let kind = match n.as_str() {
            "if" => self.if_stmt()?,
            "while" => {
                self.advance();
                let test = Box::new(self.named_expression()?);
                self.expect_op(":")?;
                let body = self.block()?;
                let orelse = if self.eat_kw("else") { self.expect_op(":")?; self.block()? } else { Vec::new() };
                StmtKind::While { test, body, orelse }
            }
            "for" => { self.advance(); self.for_rest(false)? }
            "with" => { self.advance(); self.with_rest(false)? }
            "try" => self.try_stmt()?,
            "def" => self.funcdef(Vec::new(), false, start)?,
            "class" => self.classdef(Vec::new(), start)?,
            "async" => {
                self.advance();
                match self.tok().clone() {
                    Tok::Name(x) if x == "def" => self.funcdef(Vec::new(), true, start)?,
                    Tok::Name(x) if x == "for" => { self.advance(); self.for_rest(true)? }
                    Tok::Name(x) if x == "with" => { self.advance(); self.with_rest(true)? }
                    _ => return self.error("invalid syntax"),
                }
            }
            "match" if self.looks_like_match() => self.match_stmt()?,
            _ => return self.decorated_or_none(start),
        };
        Ok(Some(Stmt { kind, span: self.span_from(start) }))
    }

    fn decorated_or_none(&mut self, start: (u32, u32)) -> PResult<Option<Stmt>> {
        if !self.is_op("@") { return Ok(None); }
        let mut decorators = Vec::new();
        while self.eat_op("@") {
            decorators.push(self.named_expression()?);
            self.expect_newline()?;
        }
        let dstart = self.start();
        let kind = match self.tok().clone() {
            Tok::Name(x) if x == "def" => self.funcdef(decorators, false, dstart)?,
            Tok::Name(x) if x == "class" => self.classdef(decorators, dstart)?,
            Tok::Name(x) if x == "async" && self.is_kw_at(1, "def") => { self.advance(); self.funcdef(decorators, true, dstart)? }
            _ => return self.error("invalid syntax"),
        };
        let _ = start;
        Ok(Some(Stmt { kind, span: self.span_from(dstart) }))
    }

    fn if_stmt(&mut self) -> PResult<StmtKind> {
        // Consumes 'if' or 'elif'.
        self.advance();
        let test = Box::new(self.named_expression()?);
        self.expect_op(":")?;
        let body = self.block()?;
        let orelse = if self.is_kw("elif") {
            let s = self.start();
            let kind = self.if_stmt()?;
            vec![Stmt { kind, span: self.span_from(s) }]
        } else if self.eat_kw("else") {
            self.expect_op(":")?;
            self.block()?
        } else { Vec::new() };
        Ok(StmtKind::If { test, body, orelse })
    }

    fn for_rest(&mut self, is_async: bool) -> PResult<StmtKind> {
        let mut target = self.star_targets()?;
        set_ctx(&mut target, ExprContext::Store);
        self.expect_kw("in")?;
        let iter = Box::new(self.star_expressions()?);
        self.expect_op(":")?;
        let body = self.block()?;
        let orelse = if self.eat_kw("else") { self.expect_op(":")?; self.block()? } else { Vec::new() };
        let target = Box::new(target);
        Ok(if is_async { StmtKind::AsyncFor { target, iter, body, orelse, type_comment: None } } else { StmtKind::For { target, iter, body, orelse, type_comment: None } })
    }

    fn with_rest(&mut self, is_async: bool) -> PResult<StmtKind> {
        let mut items = Vec::new();
        let mut done = false;
        if self.is_op("(") {
            // Try the parenthesized form: '(' with_item (',' with_item)* [','] ')' ':'
            let save = self.i;
            self.advance();
            let mut ok = true;
            let mut tmp = Vec::new();
            loop {
                match self.with_item() {
                    Ok(it) => tmp.push(it),
                    Err(_) => { ok = false; break; }
                }
                if !self.eat_op(",") { break; }
                if self.is_op(")") { break; }
            }
            if ok && self.eat_op(")") && self.is_op(":") { items = tmp; done = true; } else { self.i = save; }
        }
        if !done {
            loop {
                items.push(self.with_item()?);
                if !self.eat_op(",") { break; }
            }
        }
        self.expect_op(":")?;
        let body = self.block()?;
        Ok(if is_async { StmtKind::AsyncWith { items, body, type_comment: None } } else { StmtKind::With { items, body, type_comment: None } })
    }

    fn with_item(&mut self) -> PResult<WithItem> {
        let context_expr = self.expression()?;
        let optional_vars = if self.eat_kw("as") {
            let mut t = self.star_target()?;
            set_ctx(&mut t, ExprContext::Store);
            Some(t)
        } else { None };
        Ok(WithItem { context_expr, optional_vars })
    }

    fn try_stmt(&mut self) -> PResult<StmtKind> {
        self.advance();
        self.expect_op(":")?;
        let body = self.block()?;
        let mut handlers = Vec::new();
        let mut star = false;
        while self.is_kw("except") {
            let hs = self.start();
            self.advance();
            if self.eat_op("*") { star = true; }
            let (type_, name) = if self.is_op(":") { (None, None) } else {
                // 3.14: `except A, B:` without parentheses is a tuple.
                let es = self.start();
                let first = self.expression()?;
                let type_ = if self.is_op(",") {
                    let mut elts = vec![first];
                    while self.eat_op(",") { if self.is_op(":") { break; } elts.push(self.expression()?); }
                    Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Load }, span: self.span_from(es) }
                } else { first };
                let name = if self.eat_kw("as") { Some(self.expect_name()?) } else { None };
                (Some(Box::new(type_)), name)
            };
            self.expect_op(":")?;
            let hbody = self.block()?;
            handlers.push(ExceptHandler { type_, name, body: hbody, span: self.span_from(hs) });
        }
        let orelse = if self.eat_kw("else") { self.expect_op(":")?; self.block()? } else { Vec::new() };
        let finalbody = if self.eat_kw("finally") { self.expect_op(":")?; self.block()? } else { Vec::new() };
        if handlers.is_empty() && finalbody.is_empty() { return self.error("expected 'except' or 'finally' block"); }
        Ok(if star { StmtKind::TryStar { body, handlers, orelse, finalbody } } else { StmtKind::Try { body, handlers, orelse, finalbody } })
    }

    fn funcdef(&mut self, decorator_list: Vec<Expr>, is_async: bool, _start: (u32, u32)) -> PResult<StmtKind> {
        self.expect_kw("def")?;
        let name = self.expect_name()?;
        let type_params = self.type_params_opt()?;
        self.expect_op("(")?;
        let args = Box::new(self.parameters(false)?);
        self.expect_op(")")?;
        let returns = if self.eat_op("->") { Some(Box::new(self.star_expression_or_expr()?)) } else { None };
        self.expect_op(":")?;
        let body = self.block()?;
        Ok(if is_async {
            StmtKind::AsyncFunctionDef { name, args, body, decorator_list, returns, type_comment: None, type_params }
        } else {
            StmtKind::FunctionDef { name, args, body, decorator_list, returns, type_comment: None, type_params }
        })
    }

    /// `-> *Ts` style return annotations allow a starred expression.
    fn star_expression_or_expr(&mut self) -> PResult<Expr> {
        if self.is_op("*") {
            let s = self.start();
            self.advance();
            let value = Box::new(self.bitwise_or()?);
            return Ok(Expr { kind: ExprKind::Starred { value, ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        self.expression()
    }

    fn classdef(&mut self, decorator_list: Vec<Expr>, _start: (u32, u32)) -> PResult<StmtKind> {
        self.expect_kw("class")?;
        let name = self.expect_name()?;
        let type_params = self.type_params_opt()?;
        let (bases, keywords) = if self.eat_op("(") {
            let (a, k) = self.call_arguments()?;
            self.expect_op(")")?;
            (a, k)
        } else { (Vec::new(), Vec::new()) };
        self.expect_op(":")?;
        let body = self.block()?;
        Ok(StmtKind::ClassDef { name, bases, keywords, body, decorator_list, type_params })
    }

    fn type_params_opt(&mut self) -> PResult<Vec<TypeParam>> {
        if !self.eat_op("[") { return Ok(Vec::new()); }
        let mut out = Vec::new();
        loop {
            if self.is_op("]") { break; }
            let s = self.start();
            let kind = if self.eat_op("*") {
                let name = self.expect_name()?;
                let default_value = if self.eat_op("=") { Some(Box::new(self.star_expression_or_expr()?)) } else { None };
                TypeParamKind::TypeVarTuple { name, default_value }
            } else if self.eat_op("**") {
                let name = self.expect_name()?;
                let default_value = if self.eat_op("=") { Some(Box::new(self.expression()?)) } else { None };
                TypeParamKind::ParamSpec { name, default_value }
            } else {
                let name = self.expect_name()?;
                let bound = if self.eat_op(":") { Some(Box::new(self.expression()?)) } else { None };
                let default_value = if self.eat_op("=") { Some(Box::new(self.expression()?)) } else { None };
                TypeParamKind::TypeVar { name, bound, default_value }
            };
            out.push(TypeParam { kind, span: self.span_from(s) });
            if !self.eat_op(",") { break; }
        }
        self.expect_op("]")?;
        Ok(out)
    }

    /// Function or lambda parameter list, up to (not including) `)` / `:`.
    fn parameters(&mut self, lambda: bool) -> PResult<Arguments> {
        let mut a = Arguments::default();
        let mut seen_slash = false;
        let mut seen_star = false;
        let mut seen_default = false;
        let end = if lambda { ":" } else { ")" };
        loop {
            if self.is_op(end) { break; }
            if self.eat_op("/") {
                if seen_slash || seen_star || a.args.is_empty() { return self.error("invalid syntax"); }
                seen_slash = true;
                a.posonlyargs = std::mem::take(&mut a.args);
            } else if self.eat_op("**") {
                a.kwarg = Some(Box::new(self.param(lambda, false)?));
                self.eat_op(",");
                if !self.is_op(end) { return self.error("arguments cannot follow var-keyword argument"); }
                break;
            } else if self.eat_op("*") {
                if seen_star { return self.error("* argument may appear only once"); }
                seen_star = true;
                if self.is_op(",") {
                    if self.is_op_at(1, end) { return self.error("named arguments must follow bare *"); }
                } else {
                    a.vararg = Some(Box::new(self.param(lambda, true)?));
                }
            } else {
                let arg = self.param(lambda, false)?;
                let default = if self.eat_op("=") { Some(self.expression()?) } else { None };
                if seen_star {
                    a.kwonlyargs.push(arg);
                    a.kw_defaults.push(default);
                } else {
                    match default {
                        Some(d) => { seen_default = true; a.defaults.push(d); }
                        None if seen_default => return self.error("parameter without a default follows parameter with a default"),
                        None => {}
                    }
                    a.args.push(arg);
                }
            }
            if !self.eat_op(",") { break; }
        }
        Ok(a)
    }

    fn param(&mut self, lambda: bool, star_annotation: bool) -> PResult<Arg> {
        let s = self.start();
        let arg = self.expect_name()?;
        let annotation = if !lambda && self.eat_op(":") {
            Some(Box::new(if star_annotation { self.star_expression_or_expr()? } else { self.expression()? }))
        } else { None };
        Ok(Arg { arg, annotation, type_comment: None, span: self.span_from(s) })
    }

    // ---- match -------------------------------------------------------------

    /// `match` is a soft keyword: it starts a match statement only when a
    /// subject follows and the line ends with `:`.
    fn looks_like_match(&mut self) -> bool {
        let save = self.i;
        self.advance();
        let ok = match self.tok() {
            Tok::Newline | Tok::EndMarker => false,
            Tok::Op(o) if matches!(o.as_str(), "=" | "." | "," | ")" | "]" | "}" | ":" | ";") || augassign_op(o).is_some() => false,
            _ => {
                let r = self.subject_expr().is_ok() && self.is_op(":") && matches!(self.tok_at(1), Tok::Newline) && matches!(self.tok_at(2), Tok::Indent) && self.is_kw_at(3, "case");
                r
            }
        };
        self.i = save;
        ok
    }

    fn subject_expr(&mut self) -> PResult<Expr> {
        let s = self.start();
        let first = self.star_named_expression()?;
        if self.is_op(",") {
            let mut elts = vec![first];
            while self.eat_op(",") {
                if self.is_op(":") { break; }
                elts.push(self.star_named_expression()?);
            }
            return Ok(Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        if matches!(first.kind, ExprKind::Starred { .. }) { return self.error("invalid syntax"); }
        Ok(first)
    }

    fn match_stmt(&mut self) -> PResult<StmtKind> {
        self.advance();
        let subject = Box::new(self.subject_expr()?);
        self.expect_op(":")?;
        self.expect_newline()?;
        if !matches!(self.tok(), Tok::Indent) { return self.error("expected an indented block"); }
        self.advance();
        let mut cases = Vec::new();
        while self.is_kw("case") {
            self.advance();
            let pattern = self.patterns()?;
            let guard = if self.eat_kw("if") { Some(Box::new(self.named_expression()?)) } else { None };
            self.expect_op(":")?;
            let body = self.block()?;
            cases.push(MatchCase { pattern, guard, body });
        }
        if cases.is_empty() { return self.error("invalid syntax"); }
        if matches!(self.tok(), Tok::Dedent) { self.advance(); }
        Ok(StmtKind::Match { subject, cases })
    }

    /// Top-level case pattern: an open sequence (`a, *b`) or a single pattern.
    fn patterns(&mut self) -> PResult<Pattern> {
        let s = self.start();
        let first = self.maybe_star_pattern()?;
        if self.is_op(",") {
            let mut patterns = vec![first];
            while self.eat_op(",") {
                if self.is_op(":") || self.is_kw("if") { break; }
                patterns.push(self.maybe_star_pattern()?);
            }
            return Ok(Pattern { kind: PatternKind::MatchSequence { patterns }, span: self.span_from(s) });
        }
        if matches!(first.kind, PatternKind::MatchStar { .. }) { return self.error_at(first.span, "invalid syntax"); }
        Ok(first)
    }

    fn maybe_star_pattern(&mut self) -> PResult<Pattern> {
        if self.is_op("*") {
            let s = self.start();
            self.advance();
            let name = self.expect_name()?;
            let name = if name == "_" { None } else { Some(name) };
            return Ok(Pattern { kind: PatternKind::MatchStar { name }, span: self.span_from(s) });
        }
        self.pattern()
    }

    fn pattern(&mut self) -> PResult<Pattern> {
        let s = self.start();
        let or = self.or_pattern()?;
        if self.eat_kw("as") {
            let name = self.expect_name()?;
            if name == "_" { return self.error("cannot use '_' as a target"); }
            return Ok(Pattern { kind: PatternKind::MatchAs { pattern: Some(Box::new(or)), name: Some(name) }, span: self.span_from(s) });
        }
        Ok(or)
    }

    fn or_pattern(&mut self) -> PResult<Pattern> {
        let s = self.start();
        let first = self.closed_pattern()?;
        if !self.is_op("|") { return Ok(first); }
        let mut patterns = vec![first];
        while self.eat_op("|") { patterns.push(self.closed_pattern()?); }
        Ok(Pattern { kind: PatternKind::MatchOr { patterns }, span: self.span_from(s) })
    }

    fn closed_pattern(&mut self) -> PResult<Pattern> {
        let s = self.start();
        match self.tok().clone() {
            Tok::Name(n) if n == "None" || n == "True" || n == "False" => {
                self.advance();
                let value = match n.as_str() { "None" => Constant::None, "True" => Constant::Bool(true), _ => Constant::Bool(false) };
                Ok(Pattern { kind: PatternKind::MatchSingleton { value }, span: self.span_from(s) })
            }
            Tok::Name(n) if !is_keyword(&n) => {
                // capture, wildcard, value (dotted), or class pattern
                self.advance();
                if !self.is_op(".") && !self.is_op("(") {
                    let name = if n == "_" { None } else { Some(n) };
                    return Ok(Pattern { kind: PatternKind::MatchAs { pattern: None, name }, span: self.span_from(s) });
                }
                let mut e = Expr { kind: ExprKind::Name { id: n, ctx: ExprContext::Load }, span: self.span_from(s) };
                while self.eat_op(".") {
                    let attr = self.expect_name()?;
                    e = Expr { kind: ExprKind::Attribute { value: Box::new(e), attr, ctx: ExprContext::Load }, span: self.span_from(s) };
                }
                if self.eat_op("(") { return self.class_pattern_rest(e, s); }
                Ok(Pattern { kind: PatternKind::MatchValue { value: Box::new(e) }, span: self.span_from(s) })
            }
            Tok::Number(_) | Tok::String(_) | Tok::FStringStart(_) | Tok::TStringStart(_) => {
                let value = self.literal_expr()?;
                Ok(Pattern { kind: PatternKind::MatchValue { value: Box::new(value) }, span: self.span_from(s) })
            }
            Tok::Op(o) if o == "-" => {
                let value = self.literal_expr()?;
                Ok(Pattern { kind: PatternKind::MatchValue { value: Box::new(value) }, span: self.span_from(s) })
            }
            Tok::Op(o) if o == "(" => {
                self.advance();
                if self.eat_op(")") {
                    return Ok(Pattern { kind: PatternKind::MatchSequence { patterns: Vec::new() }, span: self.span_from(s) });
                }
                let first = self.maybe_star_pattern()?;
                if self.is_op(",") {
                    let mut patterns = vec![first];
                    while self.eat_op(",") {
                        if self.is_op(")") { break; }
                        patterns.push(self.maybe_star_pattern()?);
                    }
                    self.expect_op(")")?;
                    return Ok(Pattern { kind: PatternKind::MatchSequence { patterns }, span: self.span_from(s) });
                }
                self.expect_op(")")?;
                if matches!(first.kind, PatternKind::MatchStar { .. }) {
                    return Ok(Pattern { kind: PatternKind::MatchSequence { patterns: vec![first] }, span: self.span_from(s) });
                }
                Ok(first)
            }
            Tok::Op(o) if o == "[" => {
                self.advance();
                let mut patterns = Vec::new();
                while !self.is_op("]") {
                    patterns.push(self.maybe_star_pattern()?);
                    if !self.eat_op(",") { break; }
                }
                self.expect_op("]")?;
                Ok(Pattern { kind: PatternKind::MatchSequence { patterns }, span: self.span_from(s) })
            }
            Tok::Op(o) if o == "{" => {
                self.advance();
                let mut keys = Vec::new();
                let mut patterns = Vec::new();
                let mut rest = None;
                while !self.is_op("}") {
                    if self.eat_op("**") {
                        rest = Some(self.expect_name()?);
                        self.eat_op(",");
                        break;
                    }
                    let key = match self.tok().clone() {
                        Tok::Name(n) if n == "None" || n == "True" || n == "False" => {
                            let ks = self.start();
                            self.advance();
                            let value = match n.as_str() { "None" => Constant::None, "True" => Constant::Bool(true), _ => Constant::Bool(false) };
                            Expr { kind: ExprKind::Constant { value, kind: None }, span: self.span_from(ks) }
                        }
                        Tok::Name(n) if !is_keyword(&n) => {
                            let ks = self.start();
                            self.advance();
                            let mut e = Expr { kind: ExprKind::Name { id: n, ctx: ExprContext::Load }, span: self.span_from(ks) };
                            if !self.is_op(".") { return self.error("invalid syntax"); }
                            while self.eat_op(".") {
                                let attr = self.expect_name()?;
                                e = Expr { kind: ExprKind::Attribute { value: Box::new(e), attr, ctx: ExprContext::Load }, span: self.span_from(ks) };
                            }
                            e
                        }
                        _ => self.literal_expr()?,
                    };
                    self.expect_op(":")?;
                    keys.push(key);
                    patterns.push(self.pattern()?);
                    if !self.eat_op(",") { break; }
                }
                self.expect_op("}")?;
                Ok(Pattern { kind: PatternKind::MatchMapping { keys, patterns, rest }, span: self.span_from(s) })
            }
            _ => self.error("invalid syntax"),
        }
    }

    fn class_pattern_rest(&mut self, cls: Expr, s: (u32, u32)) -> PResult<Pattern> {
        let mut patterns = Vec::new();
        let mut kwd_attrs = Vec::new();
        let mut kwd_patterns = Vec::new();
        while !self.is_op(")") {
            if self.at_name() && self.is_op_at(1, "=") {
                kwd_attrs.push(self.expect_name()?);
                self.advance();
                kwd_patterns.push(self.pattern()?);
            } else {
                if !kwd_attrs.is_empty() { return self.error("positional patterns follow keyword patterns"); }
                patterns.push(self.pattern()?);
            }
            if !self.eat_op(",") { break; }
        }
        self.expect_op(")")?;
        Ok(Pattern { kind: PatternKind::MatchClass { cls: Box::new(cls), patterns, kwd_attrs, kwd_patterns }, span: self.span_from(s) })
    }

    /// Literal usable in a pattern: signed number, complex `a+bj`, strings.
    fn literal_expr(&mut self) -> PResult<Expr> {
        let s = self.start();
        match self.tok().clone() {
            Tok::String(_) | Tok::FStringStart(_) | Tok::TStringStart(_) => {
                let e = self.strings()?;
                if matches!(e.kind, ExprKind::JoinedStr { .. } | ExprKind::TemplateStr { .. }) {
                    return self.error_at(e.span, "patterns may only match literals and attribute lookups");
                }
                Ok(e)
            }
            _ => {
                let neg = self.eat_op("-");
                let Tok::Number(_) = self.tok() else { return self.error("invalid syntax") };
                let num = self.number()?;
                let mut e = if neg {
                    Expr { kind: ExprKind::UnaryOp { op: UnaryOp::USub, operand: Box::new(num) }, span: self.span_from(s) }
                } else { num };
                if self.is_op("+") || self.is_op("-") {
                    let op = if self.eat_op("+") { Operator::Add } else { self.advance(); Operator::Sub };
                    let Tok::Number(_) = self.tok() else { return self.error("invalid syntax") };
                    let imag = self.number()?;
                    if !matches!(imag.kind, ExprKind::Constant { value: Constant::Complex(_), .. }) {
                        return self.error_at(imag.span, "imaginary number required in complex literal");
                    }
                    e = Expr { kind: ExprKind::BinOp { left: Box::new(e), op, right: Box::new(imag) }, span: self.span_from(s) };
                }
                Ok(e)
            }
        }
    }

    // ---- targets -----------------------------------------------------------

    /// `star_targets`: comma-separated targets, stopping before `in` / `=`.
    fn star_targets(&mut self) -> PResult<Expr> {
        let s = self.start();
        let first = self.star_target()?;
        if !self.is_op(",") { return Ok(first); }
        let mut elts = vec![first];
        while self.eat_op(",") {
            if self.is_kw("in") || self.is_op("=") || self.is_op(")") || self.is_op("]") || self.is_op(":") { break; }
            elts.push(self.star_target()?);
        }
        Ok(Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Store }, span: self.span_from(s) })
    }

    fn star_target(&mut self) -> PResult<Expr> {
        let s = self.start();
        if self.eat_op("*") {
            let value = Box::new(self.star_target()?);
            return Ok(Expr { kind: ExprKind::Starred { value, ctx: ExprContext::Store }, span: self.span_from(s) });
        }
        self.target_with_star_atom()
    }

    fn target_with_star_atom(&mut self) -> PResult<Expr> {
        let s = self.start();
        if self.is_op("(") || self.is_op("[") {
            let close = if self.is_op("(") { ")" } else { "]" };
            self.advance();
            let mut elts = Vec::new();
            let mut trailing_comma = false;
            while !self.is_op(close) {
                elts.push(self.star_target()?);
                trailing_comma = false;
                if !self.eat_op(",") { break; }
                trailing_comma = true;
            }
            self.expect_op(close)?;
            let inner_span = self.span_from(s);
            let mut e = if close == "]" {
                Expr { kind: ExprKind::List { elts, ctx: ExprContext::Store }, span: inner_span }
            } else if elts.len() == 1 && !trailing_comma {
                let single = elts.pop().unwrap();
                if matches!(single.kind, ExprKind::Starred { .. }) { return self.error_at(single.span, "invalid syntax"); }
                single
            } else {
                Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Store }, span: inner_span }
            };
            e = self.target_trailers(e, s)?;
            return Ok(e);
        }
        let atom = self.atom()?;
        self.target_trailers(atom, s)
    }

    /// `.attr`, `[sub]`, `(call)` trailers after a target atom; the result
    /// must end with an attribute or subscript unless it is a bare name.
    fn target_trailers(&mut self, mut e: Expr, s: (u32, u32)) -> PResult<Expr> {
        loop {
            if self.eat_op(".") {
                let attr = self.expect_name()?;
                e = Expr { kind: ExprKind::Attribute { value: Box::new(e), attr, ctx: ExprContext::Store }, span: self.span_from(s) };
            } else if self.is_op("[") {
                self.advance();
                let slice = Box::new(self.slices()?);
                self.expect_op("]")?;
                e = Expr { kind: ExprKind::Subscript { value: Box::new(e), slice, ctx: ExprContext::Store }, span: self.span_from(s) };
            } else if self.is_op("(") {
                self.advance();
                let (args, keywords) = self.call_arguments()?;
                self.expect_op(")")?;
                e = Expr { kind: ExprKind::Call { func: Box::new(e), args, keywords }, span: self.span_from(s) };
            } else { break; }
        }
        match e.kind {
            ExprKind::Name { .. } | ExprKind::Attribute { .. } | ExprKind::Subscript { .. } | ExprKind::Tuple { .. } | ExprKind::List { .. } | ExprKind::Starred { .. } => Ok(e),
            _ => self.error_at(e.span, format!("cannot assign to {}", describe(&e))),
        }
    }

    // ---- expressions ---------------------------------------------------------

    fn yield_expr(&mut self) -> PResult<Expr> {
        let s = self.start();
        self.expect_kw("yield")?;
        if self.eat_kw("from") {
            let value = Box::new(self.expression()?);
            return Ok(Expr { kind: ExprKind::YieldFrom { value }, span: self.span_from(s) });
        }
        let value = if self.at_expr_end() { None } else { Some(Box::new(self.star_expressions()?)) };
        Ok(Expr { kind: ExprKind::Yield { value }, span: self.span_from(s) })
    }

    fn at_expr_end(&self) -> bool {
        matches!(self.tok(), Tok::Newline | Tok::EndMarker) || matches!(self.tok(), Tok::Op(o) if matches!(o.as_str(), ")" | "]" | "}" | ";" | "=" | ":" | ","))
    }

    /// `star_expressions`: tuple if a comma is present.
    fn star_expressions(&mut self) -> PResult<Expr> {
        let s = self.start();
        let first = self.star_expression()?;
        if !self.is_op(",") { return Ok(first); }
        let mut elts = vec![first];
        while self.eat_op(",") {
            if self.at_expr_end() { break; }
            elts.push(self.star_expression()?);
        }
        Ok(Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Load }, span: self.span_from(s) })
    }

    fn star_expression(&mut self) -> PResult<Expr> {
        if self.is_op("*") {
            let s = self.start();
            self.advance();
            let value = Box::new(self.bitwise_or()?);
            return Ok(Expr { kind: ExprKind::Starred { value, ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        self.expression()
    }

    fn star_named_expression(&mut self) -> PResult<Expr> {
        if self.is_op("*") {
            let s = self.start();
            self.advance();
            let value = Box::new(self.bitwise_or()?);
            return Ok(Expr { kind: ExprKind::Starred { value, ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        self.named_expression()
    }

    fn named_expression(&mut self) -> PResult<Expr> {
        if self.at_name() && self.is_op_at(1, ":=") {
            let s = self.start();
            let id = self.expect_name()?;
            let target = Box::new(Expr { kind: ExprKind::Name { id, ctx: ExprContext::Store }, span: self.span_from(s) });
            self.advance();
            let value = Box::new(self.expression()?);
            return Ok(Expr { kind: ExprKind::NamedExpr { target, value }, span: self.span_from(s) });
        }
        let e = self.expression()?;
        if self.is_op(":=") { return self.error_at(e.span, format!("cannot use assignment expressions with {}", describe(&e))); }
        Ok(e)
    }

    fn expression(&mut self) -> PResult<Expr> {
        if self.is_kw("lambda") { return self.lambdef(); }
        let s = self.start();
        let body = self.disjunction()?;
        if self.is_kw("if") {
            self.advance();
            let test = Box::new(self.disjunction()?);
            self.expect_kw("else")?;
            let orelse = Box::new(self.expression()?);
            return Ok(Expr { kind: ExprKind::IfExp { test, body: Box::new(body), orelse }, span: self.span_from(s) });
        }
        Ok(body)
    }

    fn lambdef(&mut self) -> PResult<Expr> {
        let s = self.start();
        self.expect_kw("lambda")?;
        let args = Box::new(self.parameters(true)?);
        self.expect_op(":")?;
        let body = Box::new(self.expression()?);
        Ok(Expr { kind: ExprKind::Lambda { args, body }, span: self.span_from(s) })
    }

    fn disjunction(&mut self) -> PResult<Expr> {
        let s = self.start();
        let first = self.conjunction()?;
        if !self.is_kw("or") { return Ok(first); }
        let mut values = vec![first];
        while self.eat_kw("or") { values.push(self.conjunction()?); }
        Ok(Expr { kind: ExprKind::BoolOp { op: BoolOp::Or, values }, span: self.span_from(s) })
    }

    fn conjunction(&mut self) -> PResult<Expr> {
        let s = self.start();
        let first = self.inversion()?;
        if !self.is_kw("and") { return Ok(first); }
        let mut values = vec![first];
        while self.eat_kw("and") { values.push(self.inversion()?); }
        Ok(Expr { kind: ExprKind::BoolOp { op: BoolOp::And, values }, span: self.span_from(s) })
    }

    fn inversion(&mut self) -> PResult<Expr> {
        if self.is_kw("not") {
            let s = self.start();
            self.advance();
            let operand = Box::new(self.inversion()?);
            return Ok(Expr { kind: ExprKind::UnaryOp { op: UnaryOp::Not, operand }, span: self.span_from(s) });
        }
        self.comparison()
    }

    fn comparison(&mut self) -> PResult<Expr> {
        let s = self.start();
        let left = self.bitwise_or()?;
        let mut ops = Vec::new();
        let mut comparators = Vec::new();
        loop {
            let op = match self.tok() {
                Tok::Op(o) => match o.as_str() {
                    "==" => CmpOp::Eq, "!=" => CmpOp::NotEq, "<" => CmpOp::Lt, "<=" => CmpOp::LtE, ">" => CmpOp::Gt, ">=" => CmpOp::GtE,
                    _ => break,
                },
                Tok::Name(n) => match n.as_str() {
                    "in" => CmpOp::In,
                    "is" => if self.is_kw_at(1, "not") { self.advance(); CmpOp::IsNot } else { CmpOp::Is },
                    "not" => if self.is_kw_at(1, "in") { self.advance(); CmpOp::NotIn } else { break },
                    _ => break,
                },
                _ => break,
            };
            self.advance();
            ops.push(op);
            comparators.push(self.bitwise_or()?);
        }
        if ops.is_empty() { return Ok(left); }
        Ok(Expr { kind: ExprKind::Compare { left: Box::new(left), ops, comparators }, span: self.span_from(s) })
    }

    fn binary_level(&mut self, ops: &[(&str, Operator)], next: fn(&mut Self) -> PResult<Expr>) -> PResult<Expr> {
        let s = self.start();
        let mut left = next(self)?;
        loop {
            let Some(&(_, op)) = ops.iter().find(|(t, _)| self.is_op(t)) else { break };
            self.advance();
            let right = Box::new(next(self)?);
            left = Expr { kind: ExprKind::BinOp { left: Box::new(left), op, right }, span: self.span_from(s) };
        }
        Ok(left)
    }

    fn bitwise_or(&mut self) -> PResult<Expr> { self.binary_level(&[("|", Operator::BitOr)], Self::bitwise_xor) }
    fn bitwise_xor(&mut self) -> PResult<Expr> { self.binary_level(&[("^", Operator::BitXor)], Self::bitwise_and) }
    fn bitwise_and(&mut self) -> PResult<Expr> { self.binary_level(&[("&", Operator::BitAnd)], Self::shift_expr) }
    fn shift_expr(&mut self) -> PResult<Expr> { self.binary_level(&[("<<", Operator::LShift), (">>", Operator::RShift)], Self::sum) }
    fn sum(&mut self) -> PResult<Expr> { self.binary_level(&[("+", Operator::Add), ("-", Operator::Sub)], Self::term) }
    fn term(&mut self) -> PResult<Expr> {
        self.binary_level(&[("*", Operator::Mult), ("/", Operator::Div), ("//", Operator::FloorDiv), ("%", Operator::Mod), ("@", Operator::MatMult)], Self::factor)
    }

    fn factor(&mut self) -> PResult<Expr> {
        let s = self.start();
        let op = match self.tok() {
            Tok::Op(o) => match o.as_str() { "+" => Some(UnaryOp::UAdd), "-" => Some(UnaryOp::USub), "~" => Some(UnaryOp::Invert), _ => None },
            _ => None,
        };
        if let Some(op) = op {
            self.advance();
            let operand = Box::new(self.factor()?);
            return Ok(Expr { kind: ExprKind::UnaryOp { op, operand }, span: self.span_from(s) });
        }
        self.power()
    }

    fn power(&mut self) -> PResult<Expr> {
        let s = self.start();
        let base = self.await_primary()?;
        if self.eat_op("**") {
            let right = Box::new(self.factor()?);
            return Ok(Expr { kind: ExprKind::BinOp { left: Box::new(base), op: Operator::Pow, right }, span: self.span_from(s) });
        }
        Ok(base)
    }

    fn await_primary(&mut self) -> PResult<Expr> {
        if self.is_kw("await") {
            let s = self.start();
            self.advance();
            let value = Box::new(self.primary()?);
            return Ok(Expr { kind: ExprKind::Await { value }, span: self.span_from(s) });
        }
        self.primary()
    }

    fn primary(&mut self) -> PResult<Expr> {
        let s = self.start();
        let mut e = self.atom()?;
        loop {
            if self.eat_op(".") {
                let attr = self.expect_name()?;
                e = Expr { kind: ExprKind::Attribute { value: Box::new(e), attr, ctx: ExprContext::Load }, span: self.span_from(s) };
            } else if self.is_op("(") {
                self.advance();
                let (args, keywords) = self.call_arguments()?;
                self.expect_op(")")?;
                e = Expr { kind: ExprKind::Call { func: Box::new(e), args, keywords }, span: self.span_from(s) };
            } else if self.is_op("[") {
                self.advance();
                let slice = Box::new(self.slices()?);
                self.expect_op("]")?;
                e = Expr { kind: ExprKind::Subscript { value: Box::new(e), slice, ctx: ExprContext::Load }, span: self.span_from(s) };
            } else { break; }
        }
        Ok(e)
    }

    fn slices(&mut self) -> PResult<Expr> {
        let s = self.start();
        let first = self.slice()?;
        if !self.is_op(",") {
            if matches!(first.kind, ExprKind::Starred { .. }) {
                return Ok(Expr { kind: ExprKind::Tuple { elts: vec![first], ctx: ExprContext::Load }, span: self.span_from(s) });
            }
            return Ok(first);
        }
        let mut elts = vec![first];
        while self.eat_op(",") {
            if self.is_op("]") { break; }
            elts.push(self.slice()?);
        }
        Ok(Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Load }, span: self.span_from(s) })
    }

    fn slice(&mut self) -> PResult<Expr> {
        let s = self.start();
        if self.is_op("*") { return self.star_named_expression(); }
        let lower = if self.is_op(":") { None } else {
            let e = self.named_expression()?;
            if !self.is_op(":") { return Ok(e); }
            Some(Box::new(e))
        };
        self.expect_op(":")?;
        let upper = if self.is_op(":") || self.is_op("]") || self.is_op(",") { None } else { Some(Box::new(self.expression()?)) };
        let step = if self.eat_op(":") {
            if self.is_op("]") || self.is_op(",") { None } else { Some(Box::new(self.expression()?)) }
        } else { None };
        Ok(Expr { kind: ExprKind::Slice { lower, upper, step }, span: self.span_from(s) })
    }

    /// Call arguments up to (not including) `)`.
    fn call_arguments(&mut self) -> PResult<(Vec<Expr>, Vec<Keyword>)> {
        let mut args = Vec::new();
        let mut keywords: Vec<Keyword> = Vec::new();
        let open = self.prev().clone();
        while !self.is_op(")") {
            let s = self.start();
            if self.eat_op("**") {
                let value = self.expression()?;
                keywords.push(Keyword { arg: None, value, span: self.span_from(s) });
            } else if self.eat_op("*") {
                let value = Box::new(self.expression()?);
                args.push(Expr { kind: ExprKind::Starred { value, ctx: ExprContext::Load }, span: self.span_from(s) });
            } else if self.at_name() && self.is_op_at(1, "=") {
                let name = self.expect_name()?;
                self.advance();
                let value = self.expression()?;
                keywords.push(Keyword { arg: Some(name), value, span: self.span_from(s) });
            } else {
                let e = self.named_expression()?;
                if self.is_kw("for") || (self.is_kw("async") && self.is_kw_at(1, "for")) {
                    // Generator expression as the sole argument: spans the parens.
                    let generators = self.for_if_clauses()?;
                    let gs = (open.start.0, open.start_byte);
                    if !self.is_op(")") { return self.error("Generator expression must be parenthesized"); }
                    let end = self.cur().clone();
                    let span = Span::new(gs.0, gs.1, end.end.0, end.end_byte);
                    args.push(Expr { kind: ExprKind::GeneratorExp { elt: Box::new(e), generators }, span });
                    break;
                }
                if let Some(k) = keywords.iter().find(|k| k.arg.is_some()) {
                    let _ = k;
                    return self.error_at(e.span, "positional argument follows keyword argument");
                }
                if keywords.iter().any(|k| k.arg.is_none()) {
                    return self.error_at(e.span, "positional argument follows keyword argument unpacking");
                }
                args.push(e);
            }
            if !self.eat_op(",") { break; }
        }
        Ok((args, keywords))
    }

    fn for_if_clauses(&mut self) -> PResult<Vec<Comprehension>> {
        let mut gens = Vec::new();
        loop {
            let is_async = if self.is_kw("async") && self.is_kw_at(1, "for") { self.advance(); true } else { false };
            if !self.eat_kw("for") { break; }
            let mut target = self.star_targets()?;
            set_ctx(&mut target, ExprContext::Store);
            self.expect_kw("in")?;
            let iter = self.disjunction()?;
            let mut ifs = Vec::new();
            while self.eat_kw("if") { ifs.push(self.disjunction()?); }
            gens.push(Comprehension { target, iter, ifs, is_async });
        }
        if gens.is_empty() { return self.error("invalid syntax"); }
        Ok(gens)
    }

    fn atom(&mut self) -> PResult<Expr> {
        let s = self.start();
        match self.tok().clone() {
            Tok::Name(n) => match n.as_str() {
                "True" => { self.advance(); Ok(Expr { kind: ExprKind::Constant { value: Constant::Bool(true), kind: None }, span: self.span_from(s) }) }
                "False" => { self.advance(); Ok(Expr { kind: ExprKind::Constant { value: Constant::Bool(false), kind: None }, span: self.span_from(s) }) }
                "None" => { self.advance(); Ok(Expr { kind: ExprKind::Constant { value: Constant::None, kind: None }, span: self.span_from(s) }) }
                _ if is_keyword(&n) => self.error("invalid syntax"),
                _ => { self.advance(); Ok(Expr { kind: ExprKind::Name { id: n, ctx: ExprContext::Load }, span: self.span_from(s) }) }
            },
            Tok::Number(_) => self.number(),
            Tok::String(_) | Tok::FStringStart(_) | Tok::TStringStart(_) => self.strings(),
            Tok::Op(o) => match o.as_str() {
                "..." => { self.advance(); Ok(Expr { kind: ExprKind::Constant { value: Constant::Ellipsis, kind: None }, span: self.span_from(s) }) }
                "(" => self.paren_atom(),
                "[" => self.list_atom(),
                "{" => self.dict_or_set_atom(),
                _ => self.error("invalid syntax"),
            },
            Tok::Indent => self.error("unexpected indent"),
            _ => self.error("invalid syntax"),
        }
    }

    fn paren_atom(&mut self) -> PResult<Expr> {
        let s = self.start();
        self.advance();
        if self.eat_op(")") {
            return Ok(Expr { kind: ExprKind::Tuple { elts: Vec::new(), ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        if self.is_kw("yield") {
            let e = self.yield_expr()?;
            self.expect_op(")")?;
            return Ok(e);
        }
        let first = self.star_named_expression()?;
        if self.is_kw("for") || (self.is_kw("async") && self.is_kw_at(1, "for")) {
            let generators = self.for_if_clauses()?;
            self.expect_op(")")?;
            return Ok(Expr { kind: ExprKind::GeneratorExp { elt: Box::new(first), generators }, span: self.span_from(s) });
        }
        if self.is_op(",") {
            let mut elts = vec![first];
            while self.eat_op(",") {
                if self.is_op(")") { break; }
                elts.push(self.star_named_expression()?);
            }
            self.expect_op(")")?;
            return Ok(Expr { kind: ExprKind::Tuple { elts, ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        self.expect_op(")")?;
        if matches!(first.kind, ExprKind::Starred { .. }) { return self.error_at(first.span, "cannot use starred expression here"); }
        Ok(first)
    }

    fn list_atom(&mut self) -> PResult<Expr> {
        let s = self.start();
        self.advance();
        if self.eat_op("]") {
            return Ok(Expr { kind: ExprKind::List { elts: Vec::new(), ctx: ExprContext::Load }, span: self.span_from(s) });
        }
        let first = self.star_named_expression()?;
        if self.is_kw("for") || (self.is_kw("async") && self.is_kw_at(1, "for")) {
            let generators = self.for_if_clauses()?;
            self.expect_op("]")?;
            return Ok(Expr { kind: ExprKind::ListComp { elt: Box::new(first), generators }, span: self.span_from(s) });
        }
        let mut elts = vec![first];
        while self.eat_op(",") {
            if self.is_op("]") { break; }
            elts.push(self.star_named_expression()?);
        }
        self.expect_op("]")?;
        Ok(Expr { kind: ExprKind::List { elts, ctx: ExprContext::Load }, span: self.span_from(s) })
    }

    fn dict_or_set_atom(&mut self) -> PResult<Expr> {
        let s = self.start();
        self.advance();
        if self.eat_op("}") {
            return Ok(Expr { kind: ExprKind::Dict { keys: Vec::new(), values: Vec::new() }, span: self.span_from(s) });
        }
        // First item decides dict vs set.
        if self.eat_op("**") {
            let v = self.bitwise_or()?;
            return self.dict_rest(s, None, v);
        }
        let first = self.star_named_expression()?;
        if self.is_op(":") && !matches!(first.kind, ExprKind::Starred { .. }) {
            self.advance();
            let v = self.expression()?;
            return self.dict_rest(s, Some(first), v);
        }
        // Set
        if self.is_kw("for") || (self.is_kw("async") && self.is_kw_at(1, "for")) {
            let generators = self.for_if_clauses()?;
            self.expect_op("}")?;
            return Ok(Expr { kind: ExprKind::SetComp { elt: Box::new(first), generators }, span: self.span_from(s) });
        }
        let mut elts = vec![first];
        while self.eat_op(",") {
            if self.is_op("}") { break; }
            elts.push(self.star_named_expression()?);
        }
        self.expect_op("}")?;
        Ok(Expr { kind: ExprKind::Set { elts }, span: self.span_from(s) })
    }

    fn dict_rest(&mut self, s: (u32, u32), first_key: Option<Expr>, first_value: Expr) -> PResult<Expr> {
        if first_key.is_some() && (self.is_kw("for") || (self.is_kw("async") && self.is_kw_at(1, "for"))) {
            let generators = self.for_if_clauses()?;
            self.expect_op("}")?;
            return Ok(Expr { kind: ExprKind::DictComp { key: Box::new(first_key.unwrap()), value: Box::new(first_value), generators }, span: self.span_from(s) });
        }
        let mut keys = vec![first_key];
        let mut values = vec![first_value];
        while self.eat_op(",") {
            if self.is_op("}") { break; }
            if self.eat_op("**") {
                keys.push(None);
                values.push(self.bitwise_or()?);
            } else {
                let k = self.expression()?;
                self.expect_op(":")?;
                keys.push(Some(k));
                values.push(self.expression()?);
            }
        }
        self.expect_op("}")?;
        Ok(Expr { kind: ExprKind::Dict { keys, values }, span: self.span_from(s) })
    }

    // ---- literals --------------------------------------------------------------

    fn number(&mut self) -> PResult<Expr> {
        let s = self.start();
        let Tok::Number(text) = self.advance().tok.clone() else { unreachable!() };
        let value = parse_number(&text).ok_or_else(|| ParseError { msg: "invalid number".into(), lineno: s.0, col_offset: s.1 })?;
        Ok(Expr { kind: ExprKind::Constant { value, kind: None }, span: self.span_from(s) })
    }

    /// One or more adjacent string / f-string / t-string literals.
    fn strings(&mut self) -> PResult<Expr> {
        let s = self.start();
        let mut parts: Vec<StrPart> = Vec::new();
        let mut is_bytes: Option<bool> = None;
        let mut kind_u = false;
        let mut has_f = false;
        let mut has_t = false;
        let mut has_plain = false;
        let mut first = true;
        loop {
            match self.tok().clone() {
                Tok::String(text) => {
                    let t = self.advance().clone();
                    let (prefix, body) = split_string_literal(&text);
                    let raw = prefix.contains('r');
                    let bytes = prefix.contains('b');
                    has_plain = true;
                    if first && prefix.contains('u') { kind_u = true; }
                    match is_bytes {
                        None => is_bytes = Some(bytes),
                        Some(b) if b != bytes => return self.error_at(Self::tok_span(&t), "cannot mix bytes and nonbytes literals"),
                        _ => {}
                    }
                    let span = Self::tok_span(&t);
                    if bytes {
                        let v = decode_bytes(body, raw).map_err(|m| ParseError { msg: m, lineno: span.lineno, col_offset: span.col_offset })?;
                        parts.push(StrPart::Bytes(v));
                    } else {
                        let v = decode_str(body, raw).map_err(|m| ParseError { msg: m, lineno: span.lineno, col_offset: span.col_offset })?;
                        parts.push(StrPart::Lit(v, span));
                    }
                }
                Tok::FStringStart(_) | Tok::TStringStart(_) => {
                    let is_t = matches!(self.tok(), Tok::TStringStart(_));
                    if is_t { has_t = true; } else { has_f = true; }
                    if is_bytes == Some(true) { return self.error("cannot mix bytes and nonbytes literals"); }
                    is_bytes = Some(false);
                    let vals = self.fstring(is_t)?;
                    parts.extend(vals);
                }
                _ => break,
            }
            first = false;
        }
        let span = self.span_from(s);
        if has_t && has_f { return self.error_at(span, "cannot mix t-string literals with string or bytes literals"); }
        if has_t && has_plain { return self.error_at(span, "cannot mix t-string literals with string or bytes literals"); }
        if is_bytes == Some(true) {
            let mut v = Vec::new();
            for p in parts { if let StrPart::Bytes(b) = p { v.extend(b); } }
            return Ok(Expr { kind: ExprKind::Constant { value: Constant::Bytes(v), kind: None }, span });
        }
        if !has_f && !has_t {
            let mut v = String::new();
            for p in parts { if let StrPart::Lit(s, _) = p { v.push_str(&s); } }
            return Ok(Expr { kind: ExprKind::Constant { value: Constant::Str(v), kind: if kind_u { Some("u".into()) } else { None } }, span });
        }
        // Joined: merge adjacent literal pieces.
        let mut values: Vec<Expr> = Vec::new();
        let mut lit: Option<(String, Span)> = None;
        for p in parts {
            match p {
                StrPart::Lit(s, sp) => {
                    if s.is_empty() { continue; }
                    match &mut lit { Some((acc, span)) => { acc.push_str(&s); *span = span.to(sp); } None => lit = Some((s, sp)) }
                }
                StrPart::Expr(e) => {
                    if let Some((s, sp)) = lit.take() { values.push(Expr { kind: ExprKind::Constant { value: Constant::Str(s), kind: None }, span: sp }); }
                    values.push(e);
                }
                StrPart::Bytes(_) => unreachable!(),
            }
        }
        if let Some((s, sp)) = lit.take() { values.push(Expr { kind: ExprKind::Constant { value: Constant::Str(s), kind: None }, span: sp }); }
        let kind = if has_t { ExprKind::TemplateStr { values } } else { ExprKind::JoinedStr { values } };
        Ok(Expr { kind, span })
    }

    /// Parse one f-/t-string from START to END into literal and expression parts.
    fn fstring(&mut self, is_t: bool) -> PResult<Vec<StrPart>> {
        let start_tok = self.advance().clone();
        let raw = match &start_tok.tok { Tok::FStringStart(p) | Tok::TStringStart(p) => p.to_ascii_lowercase().contains('r'), _ => false };
        let mut parts = Vec::new();
        loop {
            match self.tok().clone() {
                Tok::FStringMiddle(text) | Tok::TStringMiddle(text) => {
                    let t = self.advance().clone();
                    let span = self.middle_span(&t, &text);
                    let v = decode_str(&text, raw).map_err(|m| ParseError { msg: m, lineno: span.lineno, col_offset: span.col_offset })?;
                    parts.push(StrPart::Lit(v, span));
                }
                Tok::FStringEnd(_) | Tok::TStringEnd(_) => { self.advance(); break; }
                Tok::Op(o) if o == "{" => {
                    let e = self.replacement_field(is_t, raw, &mut parts)?;
                    parts.push(StrPart::Expr(e));
                }
                _ => return self.error("f-string: expecting '}'"),
            }
        }
        Ok(parts)
    }

    /// `{ expr [=] [!conv] [:spec] }`. Debug `=` pushes a literal part first.
    fn replacement_field(&mut self, is_t: bool, raw: bool, parts: &mut Vec<StrPart>) -> PResult<Expr> {
        let open = self.advance().clone();
        let s = (open.start.0, open.start_byte);
        let expr_start = self.start();
        if self.is_op("}") || self.is_op("!") || self.is_op(":") || self.is_op("=") {
            return self.error("f-string: valid expression required before '}'");
        }
        let value = if self.is_kw("yield") { self.yield_expr()? } else { self.star_expressions()? };
        let expr_end = { let p = self.prev(); (p.end.0, p.end_byte) };
        let mut debug = None;
        if self.eat_op("=") {
            // Text from just after `{` through `=` and any whitespace after it.
            let after_open = (open.end.0, open.end_byte);
            let next = self.cur().clone();
            let text = self.text_without_comments(after_open, (next.start.0, next.start_byte));
            let span = Span::new(after_open.0, after_open.1, next.start.0, next.start_byte);
            debug = Some((text, span));
        }
        let mut conversion = -1;
        if self.eat_op("!") {
            let Tok::Name(c) = self.tok().clone() else { return self.error("f-string: missing conversion character") };
            let t = self.advance().clone();
            conversion = match c.as_str() {
                "s" => 115, "r" => 114, "a" => 97,
                _ => return self.error_at(Self::tok_span(&t), format!("f-string: invalid conversion character '{c}': expected 's', 'r', or 'a'")),
            };
        }
        let mut format_spec = None;
        if self.is_op(":") {
            let colon = self.advance().clone();
            let mut spec_parts: Vec<StrPart> = Vec::new();
            loop {
                match self.tok().clone() {
                    Tok::FStringMiddle(text) | Tok::TStringMiddle(text) => {
                        let t = self.advance().clone();
                        let span = self.middle_span(&t, &text);
                        let v = decode_str(&text, raw).map_err(|m| ParseError { msg: m, lineno: span.lineno, col_offset: span.col_offset })?;
                        spec_parts.push(StrPart::Lit(v, span));
                    }
                    Tok::Op(o) if o == "{" => {
                        // A t-string's format spec is evaluated as an ordinary
                        // joined string; nested fields are formatted values,
                        // not nested Interpolation objects.
                        let e = self.replacement_field(false, raw, &mut spec_parts)?;
                        spec_parts.push(StrPart::Expr(e));
                    }
                    Tok::Op(o) if o == "}" => break,
                    _ => return self.error("f-string: expecting '}'"),
                }
            }
            let close = self.cur().clone();
            let mut values = Vec::new();
            let mut lit: Option<(String, Span)> = None;
            for p in spec_parts {
                match p {
                    StrPart::Lit(s, sp) => {
                        if s.is_empty() { continue; }
                        match &mut lit { Some((acc, span)) => { acc.push_str(&s); *span = span.to(sp); } None => lit = Some((s, sp)) }
                    }
                    StrPart::Expr(e) => {
                        if let Some((s, sp)) = lit.take() { values.push(Expr { kind: ExprKind::Constant { value: Constant::Str(s), kind: None }, span: sp }); }
                        values.push(e);
                    }
                    StrPart::Bytes(_) => unreachable!(),
                }
            }
            if let Some((s, sp)) = lit.take() { values.push(Expr { kind: ExprKind::Constant { value: Constant::Str(s), kind: None }, span: sp }); }
            let span = Span::new(colon.start.0, colon.start_byte, close.start.0, close.start_byte);
            format_spec = Some(Box::new(Expr { kind: ExprKind::JoinedStr { values }, span }));
        }
        if !self.is_op("}") { return self.error("f-string: expecting '}'"); }
        self.advance();
        let span = self.span_from(s);
        if let Some((text, dspan)) = debug {
            parts.push(StrPart::Lit(text, dspan));
            if conversion == -1 && format_spec.is_none() { conversion = 114; }
        }
        let kind = if is_t {
            let str = self.text(expr_start, expr_end).to_string();
            ExprKind::Interpolation { value: Box::new(value), str, conversion, format_spec }
        } else {
            ExprKind::FormattedValue { value: Box::new(value), conversion, format_spec }
        };
        Ok(Expr { kind, span })
    }
}

enum StrPart {
    Lit(String, Span),
    Bytes(Vec<u8>),
    Expr(Expr),
}

fn augassign_op(o: &str) -> Option<Operator> {
    Some(match o {
        "+=" => Operator::Add, "-=" => Operator::Sub, "*=" => Operator::Mult, "@=" => Operator::MatMult, "/=" => Operator::Div,
        "%=" => Operator::Mod, "&=" => Operator::BitAnd, "|=" => Operator::BitOr, "^=" => Operator::BitXor,
        "<<=" => Operator::LShift, ">>=" => Operator::RShift, "**=" => Operator::Pow, "//=" => Operator::FloorDiv,
        _ => return None,
    })
}

/// Recursively set the expression context of an assignment target.
pub fn set_ctx(e: &mut Expr, ctx: ExprContext) {
    match &mut e.kind {
        ExprKind::Name { ctx: c, .. } | ExprKind::Attribute { ctx: c, .. } | ExprKind::Subscript { ctx: c, .. } => *c = ctx,
        ExprKind::Starred { value, ctx: c } => { *c = ctx; set_ctx(value, ctx); }
        ExprKind::List { elts, ctx: c } | ExprKind::Tuple { elts, ctx: c } => { *c = ctx; for x in elts { set_ctx(x, ctx); } }
        _ => {}
    }
}

/// Human description of an expression kind, for error messages.
fn describe(e: &Expr) -> &'static str {
    match &e.kind {
        ExprKind::Attribute { .. } => "attribute",
        ExprKind::Subscript { .. } => "subscript",
        ExprKind::Starred { .. } => "starred",
        ExprKind::Name { .. } => "name",
        ExprKind::List { .. } => "list",
        ExprKind::Tuple { .. } => "tuple",
        ExprKind::Lambda { .. } => "lambda",
        ExprKind::Call { .. } => "function call",
        ExprKind::BoolOp { .. } | ExprKind::BinOp { .. } | ExprKind::UnaryOp { .. } => "expression",
        ExprKind::GeneratorExp { .. } => "generator expression",
        ExprKind::Yield { .. } | ExprKind::YieldFrom { .. } => "yield expression",
        ExprKind::Await { .. } => "await expression",
        ExprKind::ListComp { .. } => "list comprehension",
        ExprKind::SetComp { .. } => "set comprehension",
        ExprKind::DictComp { .. } => "dict comprehension",
        ExprKind::Dict { .. } => "dict literal",
        ExprKind::Set { .. } => "set display",
        ExprKind::JoinedStr { .. } | ExprKind::FormattedValue { .. } => "f-string expression",
        ExprKind::TemplateStr { .. } | ExprKind::Interpolation { .. } => "t-string expression",
        ExprKind::Constant { value, .. } => match value {
            Constant::None => "None", Constant::Bool(true) => "True", Constant::Bool(false) => "False",
            Constant::Ellipsis => "ellipsis", _ => "literal",
        },
        ExprKind::Compare { .. } => "comparison",
        ExprKind::IfExp { .. } => "conditional expression",
        ExprKind::NamedExpr { .. } => "named expression",
        ExprKind::Slice { .. } => "slice",
    }
}

// ---- number literals ----------------------------------------------------------

fn parse_number(text: &str) -> Option<Constant> {
    let clean: String = text.chars().filter(|&c| c != '_').collect();
    let lower = clean.to_ascii_lowercase();
    if let Some(rest) = lower.strip_suffix('j') {
        return rest.parse::<f64>().ok().map(Constant::Complex);
    }
    if lower.starts_with("0x") { return Some(Constant::Int(radix_to_decimal(&lower[2..], 16))); }
    if lower.starts_with("0o") { return Some(Constant::Int(radix_to_decimal(&lower[2..], 8))); }
    if lower.starts_with("0b") { return Some(Constant::Int(radix_to_decimal(&lower[2..], 2))); }
    if lower.contains('.') || lower.contains('e') {
        return lower.parse::<f64>().ok().map(Constant::Float);
    }
    // Decimal integer: strip leading zeros (only all-zero literals may have them).
    let trimmed = lower.trim_start_matches('0');
    Some(Constant::Int(if trimmed.is_empty() { "0".into() } else { trimmed.into() }))
}

/// Arbitrary-precision base conversion to a decimal string.
fn radix_to_decimal(digits: &str, radix: u32) -> String {
    // Little-endian limbs, base 1e9.
    let mut limbs: Vec<u64> = vec![0];
    for c in digits.chars() {
        let d = c.to_digit(radix).unwrap_or(0) as u64;
        let mut carry = d;
        for limb in limbs.iter_mut() {
            let v = *limb * radix as u64 + carry;
            *limb = v % 1_000_000_000;
            carry = v / 1_000_000_000;
        }
        while carry > 0 { limbs.push(carry % 1_000_000_000); carry /= 1_000_000_000; }
    }
    let mut s = limbs.last().unwrap().to_string();
    for limb in limbs.iter().rev().skip(1) { s.push_str(&format!("{limb:09}")); }
    s
}

// ---- string literals ------------------------------------------------------------

/// Split `prefix'body'` into (lowercase prefix, body without quotes).
fn split_string_literal(text: &str) -> (String, &str) {
    let q = text.find(['\'', '"']).unwrap();
    let prefix = text[..q].to_ascii_lowercase();
    let rest = &text[q..];
    let quote_len = if rest.len() >= 6 && (rest.starts_with("'''") || rest.starts_with("\"\"\"")) { 3 } else { 1 };
    (prefix, &rest[quote_len..rest.len() - quote_len])
}

pub fn decode_str(body: &str, raw: bool) -> Result<String, String> {
    if raw { return Ok(body.to_string()); }
    let mut out = String::with_capacity(body.len());
    let mut it = body.chars().peekable();
    while let Some(c) = it.next() {
        if c != '\\' { out.push(c); continue; }
        let Some(e) = it.next() else { out.push('\\'); break; };
        match e {
            '\n' => {}
            '\\' => out.push('\\'),
            '\'' => out.push('\''),
            '"' => out.push('"'),
            'a' => out.push('\x07'),
            'b' => out.push('\x08'),
            'f' => out.push('\x0c'),
            'n' => out.push('\n'),
            'r' => out.push('\r'),
            't' => out.push('\t'),
            'v' => out.push('\x0b'),
            '0'..='7' => {
                let mut v = e.to_digit(8).unwrap();
                for _ in 0..2 {
                    match it.peek() { Some(d) if d.is_digit(8) => { v = v * 8 + d.to_digit(8).unwrap(); it.next(); } _ => break }
                }
                out.push(char::from_u32(v).ok_or("invalid octal escape")?);
            }
            'x' => {
                let v = take_hex(&mut it, 2).ok_or("(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\xXX escape")?;
                out.push(char::from_u32(v).unwrap());
            }
            'u' => {
                let v = take_hex(&mut it, 4).ok_or("(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\uXXXX escape")?;
                out.push(char::from_u32(v).ok_or("(unicode error) illegal Unicode character")?);
            }
            'U' => {
                let v = take_hex(&mut it, 8).ok_or("(unicode error) 'unicodeescape' codec can't decode bytes: truncated \\UXXXXXXXX escape")?;
                out.push(char::from_u32(v).ok_or("(unicode error) illegal Unicode character")?);
            }
            'N' => {
                if it.next() != Some('{') { return Err("(unicode error) 'unicodeescape' codec can't decode bytes: malformed \\N character escape".into()); }
                let mut name = String::new();
                loop {
                    match it.next() { Some('}') => break, Some(c) => name.push(c), None => return Err("(unicode error) malformed \\N character escape".into()) }
                }
                match crate::unicode_names::lookup(&name) {
                    Some(c) => out.push(c),
                    None => return Err(format!("(unicode error) 'unicodeescape' codec can't decode bytes: unknown Unicode character name")),
                }
            }
            other => { out.push('\\'); out.push(other); }
        }
    }
    Ok(out)
}

fn take_hex(it: &mut std::iter::Peekable<std::str::Chars>, n: usize) -> Option<u32> {
    let mut v = 0u32;
    for _ in 0..n {
        let c = it.next()?;
        v = v * 16 + c.to_digit(16)?;
    }
    Some(v)
}

pub fn decode_bytes(body: &str, raw: bool) -> Result<Vec<u8>, String> {
    if body.chars().any(|c| !c.is_ascii()) { return Err("bytes can only contain ASCII literal characters".into()); }
    if raw { return Ok(body.as_bytes().to_vec()); }
    let mut out = Vec::with_capacity(body.len());
    let b = body.as_bytes();
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        i += 1;
        if c != b'\\' { out.push(c); continue; }
        if i >= b.len() { out.push(b'\\'); break; }
        let e = b[i];
        i += 1;
        match e {
            b'\n' => {}
            b'\\' => out.push(b'\\'),
            b'\'' => out.push(b'\''),
            b'"' => out.push(b'"'),
            b'a' => out.push(7),
            b'b' => out.push(8),
            b'f' => out.push(12),
            b'n' => out.push(b'\n'),
            b'r' => out.push(b'\r'),
            b't' => out.push(b'\t'),
            b'v' => out.push(11),
            b'0'..=b'7' => {
                let mut v = (e - b'0') as u32;
                for _ in 0..2 {
                    if i < b.len() && (b'0'..=b'7').contains(&b[i]) { v = v * 8 + (b[i] - b'0') as u32; i += 1; } else { break; }
                }
                if v > 255 { return Err("invalid octal escape sequence".into()); }
                out.push(v as u8);
            }
            b'x' => {
                if i + 2 > b.len() { return Err("(value error) invalid \\x escape".into()); }
                let v = u8::from_str_radix(std::str::from_utf8(&b[i..i + 2]).unwrap(), 16).map_err(|_| "(value error) invalid \\x escape")?;
                i += 2;
                out.push(v);
            }
            other => { out.push(b'\\'); out.push(other); }
        }
    }
    Ok(out)
}
