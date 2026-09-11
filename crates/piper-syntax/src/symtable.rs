//! Scope analysis: which names are local, global, free, or cells in every
//! module, class, function, lambda, and comprehension. Follows CPython's
//! `symtable` rules. Child scopes are numbered in source order so a later
//! pass walking the same AST can pair each node with its scope.

use crate::ast::*;
use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScopeKind { Module, Function, Class, Lambda, Comprehension }

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Resolution {
    /// Bound in this scope (fast local, or class-namespace entry).
    Local,
    /// Bound here and captured by a nested scope.
    Cell,
    /// Bound in an enclosing function scope.
    Free,
    /// Declared `global`.
    GlobalExplicit,
    /// Not bound anywhere enclosing: module globals then builtins.
    GlobalImplicit,
}

#[derive(Debug, Default, Clone)]
struct SymFlags {
    bound: bool,
    used: bool,
    param: bool,
    global: bool,
    nonlocal: bool,
    annotated: bool,
}

#[derive(Debug, Clone)]
pub struct Scope {
    pub kind: ScopeKind,
    pub name: String,
    pub parent: Option<usize>,
    /// Child scopes in source order.
    pub children: Vec<usize>,
    /// Parameter names in slot order: positional, keyword-only, *args, **kwargs.
    pub params: Vec<String>,
    /// Locals captured by inner scopes (excluding params, which are still cells).
    pub cellvars: Vec<String>,
    /// Names captured from enclosing function scopes.
    pub freevars: Vec<String>,
    /// Every local in first-seen order (params first).
    pub varnames: Vec<String>,
    pub is_generator: bool,
    pub is_coroutine: bool,
    /// A class body whose methods use `super()` or `__class__`.
    pub needs_class_cell: bool,
    pub has_return_value: bool,
    symbols: HashMap<String, SymFlags>,
    resolved: HashMap<String, Resolution>,
    /// Names first seen order, for deterministic output.
    order: Vec<String>,
}

impl Scope {
    fn new(kind: ScopeKind, name: &str, parent: Option<usize>) -> Self {
        Scope { kind, name: name.into(), parent, children: Vec::new(), params: Vec::new(), cellvars: Vec::new(), freevars: Vec::new(), varnames: Vec::new(), is_generator: false, is_coroutine: false, needs_class_cell: false, has_return_value: false, symbols: HashMap::new(), resolved: HashMap::new(), order: Vec::new() }
    }
    fn flags(&mut self, name: &str) -> &mut SymFlags {
        if !self.symbols.contains_key(name) { self.order.push(name.to_string()); }
        self.symbols.entry(name.to_string()).or_default()
    }
    pub fn resolution(&self, name: &str) -> Resolution {
        self.resolved.get(name).copied().unwrap_or(Resolution::GlobalImplicit)
    }
    pub fn is_local(&self, name: &str) -> bool { matches!(self.resolution(name), Resolution::Local | Resolution::Cell) }
    pub fn is_cell(&self, name: &str) -> bool { self.resolution(name) == Resolution::Cell }
    pub fn is_free(&self, name: &str) -> bool { self.resolution(name) == Resolution::Free }
    pub fn is_global_explicit(&self, name: &str) -> bool { self.resolution(name) == Resolution::GlobalExplicit }
    pub fn is_global_implicit(&self, name: &str) -> bool { self.resolution(name) == Resolution::GlobalImplicit }
    pub fn is_global(&self, name: &String) -> bool { matches!(self.resolution(name), Resolution::GlobalExplicit | Resolution::GlobalImplicit) }
    /// True for function-like scopes whose locals live in slots.
    pub fn is_function_like(&self) -> bool { matches!(self.kind, ScopeKind::Function | ScopeKind::Lambda | ScopeKind::Comprehension) }
}

#[derive(Debug, Clone)]
pub struct SymbolTable {
    pub scopes: Vec<Scope>,
}

impl SymbolTable {
    pub fn module(&self) -> &Scope { &self.scopes[0] }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SymtableError {
    pub msg: String,
    pub span: Span,
}

type SResult<T> = Result<T, SymtableError>;

pub fn analyze(m: &Mod) -> SResult<SymbolTable> {
    let mut b = Builder { scopes: vec![Scope::new(ScopeKind::Module, "<module>", None)], cur: 0 };
    match m {
        Mod::Module { body, .. } | Mod::Interactive { body } => b.stmts(body)?,
        Mod::Expression { body } => b.expr(body)?,
        Mod::FunctionType { .. } => {}
    }
    let mut t = SymbolTable { scopes: b.scopes };
    resolve(&mut t, 0)?;
    Ok(t)
}

struct Builder {
    scopes: Vec<Scope>,
    cur: usize,
}

impl Builder {
    fn scope(&mut self) -> &mut Scope { &mut self.scopes[self.cur] }

    fn bind(&mut self, name: &str, span: Span) -> SResult<()> {
        let s = self.scope();
        let f = s.flags(name);
        if f.global && !f.bound { /* allowed: `global x; x = 1` */ }
        f.bound = true;
        let _ = span;
        Ok(())
    }
    fn use_name(&mut self, name: &str) { self.scope().flags(name).used = true; }
    fn param(&mut self, name: &str, span: Span) -> SResult<()> {
        let s = self.scope();
        if s.params.iter().any(|p| p == name) { return Err(SymtableError { msg: format!("duplicate argument '{name}' in function definition"), span }); }
        s.params.push(name.into());
        let f = s.flags(name);
        f.param = true;
        f.bound = true;
        Ok(())
    }

    fn push(&mut self, kind: ScopeKind, name: &str) -> usize {
        let id = self.scopes.len();
        self.scopes.push(Scope::new(kind, name, Some(self.cur)));
        self.scopes[self.cur].children.push(id);
        self.cur = id;
        id
    }
    fn pop(&mut self) { self.cur = self.scopes[self.cur].parent.unwrap(); }

    fn stmts(&mut self, ss: &[Stmt]) -> SResult<()> { for s in ss { self.stmt(s)?; } Ok(()) }

    fn stmt(&mut self, s: &Stmt) -> SResult<()> {
        match &s.kind {
            StmtKind::FunctionDef { name, args, body, decorator_list, returns, type_params, .. }
            | StmtKind::AsyncFunctionDef { name, args, body, decorator_list, returns, type_params, .. } => {
                let is_async = matches!(s.kind, StmtKind::AsyncFunctionDef { .. });
                self.bind(name, s.span)?;
                for d in decorator_list { self.expr(d)?; }
                self.arg_defaults(args)?;
                self.type_params(type_params)?;
                self.push(ScopeKind::Function, name);
                self.scope().is_coroutine = is_async;
                self.arguments(args)?;
                if let Some(r) = returns { self.annotation(r)?; }
                self.stmts(body)?;
                self.pop();
            }
            StmtKind::ClassDef { name, bases, keywords, body, decorator_list, type_params } => {
                self.bind(name, s.span)?;
                for d in decorator_list { self.expr(d)?; }
                for b in bases { self.expr(b)?; }
                for k in keywords { self.expr(&k.value)?; }
                self.type_params(type_params)?;
                self.push(ScopeKind::Class, name);
                self.stmts(body)?;
                self.pop();
            }
            StmtKind::Return { value } => { if let Some(v) = value { self.scope().has_return_value = true; self.expr(v)?; } }
            StmtKind::Delete { targets } => { for t in targets { self.target(t)?; } }
            StmtKind::Assign { targets, value, .. } => { self.expr(value)?; for t in targets { self.target(t)?; } }
            StmtKind::TypeAlias { name, type_params, value } => {
                self.target(name)?;
                self.push(ScopeKind::Function, "<typealias>");
                for parameter in type_params {
                    match &parameter.kind {
                        TypeParamKind::TypeVar { name, bound, default_value } => { self.param(name, parameter.span)?; if let Some(v) = bound { self.expr(v)?; } if let Some(v) = default_value { self.expr(v)?; } }
                        TypeParamKind::ParamSpec { name, default_value } | TypeParamKind::TypeVarTuple { name, default_value } => { self.param(name, parameter.span)?; if let Some(v) = default_value { self.expr(v)?; } }
                    }
                }
                self.expr(value)?;
                self.pop();
            }
            StmtKind::AugAssign { target, value, .. } => { self.expr(value)?; self.target(target)?; if let ExprKind::Name { id, .. } = &target.kind { self.use_name(id); } }
            StmtKind::AnnAssign { target, annotation, value, simple } => {
                if let Some(v) = value { self.expr(v)?; }
                if *simple { if let ExprKind::Name { id, .. } = &target.kind { self.scope().flags(id).annotated = true; if value.is_some() { self.bind(id, s.span)?; } else if self.scope().kind != ScopeKind::Module { self.bind(id, s.span)?; } } }
                else { self.target_use_only(target)?; }
                self.annotation(annotation)?;
            }
            StmtKind::For { target, iter, body, orelse, .. } | StmtKind::AsyncFor { target, iter, body, orelse, .. } => {
                self.expr(iter)?; self.target(target)?; self.stmts(body)?; self.stmts(orelse)?;
            }
            StmtKind::While { test, body, orelse } => { self.expr(test)?; self.stmts(body)?; self.stmts(orelse)?; }
            StmtKind::If { test, body, orelse } => { self.expr(test)?; self.stmts(body)?; self.stmts(orelse)?; }
            StmtKind::With { items, body, .. } | StmtKind::AsyncWith { items, body, .. } => {
                for it in items { self.expr(&it.context_expr)?; if let Some(v) = &it.optional_vars { self.target(v)?; } }
                self.stmts(body)?;
            }
            StmtKind::Match { subject, cases } => {
                self.expr(subject)?;
                for c in cases { self.pattern(&c.pattern)?; if let Some(g) = &c.guard { self.expr(g)?; } self.stmts(&c.body)?; }
            }
            StmtKind::Raise { exc, cause } => { if let Some(e) = exc { self.expr(e)?; } if let Some(c) = cause { self.expr(c)?; } }
            StmtKind::Try { body, handlers, orelse, finalbody } | StmtKind::TryStar { body, handlers, orelse, finalbody } => {
                self.stmts(body)?;
                for h in handlers { if let Some(t) = &h.type_ { self.expr(t)?; } if let Some(n) = &h.name { self.bind(n, h.span)?; } self.stmts(&h.body)?; }
                self.stmts(orelse)?; self.stmts(finalbody)?;
            }
            StmtKind::Assert { test, msg } => { self.expr(test)?; if let Some(m) = msg { self.expr(m)?; } }
            StmtKind::Import { names } => { for a in names { let n = a.asname.clone().unwrap_or_else(|| a.name.split('.').next().unwrap().to_string()); self.bind(&n, a.span)?; } }
            StmtKind::ImportFrom { names, .. } => { for a in names { if a.name == "*" { continue; } let n = a.asname.clone().unwrap_or_else(|| a.name.clone()); self.bind(&n, a.span)?; } }
            StmtKind::Global { names } => {
                for n in names {
                    let f = self.scope().flags(n);
                    if f.nonlocal { return Err(SymtableError { msg: format!("name '{n}' is nonlocal and global"), span: s.span }); }
                    if f.bound || f.used || f.param { return Err(SymtableError { msg: format!("name '{n}' is {} prior to global declaration", if f.param { "parameter and declared global" } else if f.bound { "assigned to before global declaration" } else { "used prior to global declaration" }), span: s.span }); }
                    f.global = true;
                }
            }
            StmtKind::Nonlocal { names } => {
                if self.scope().kind == ScopeKind::Module { return Err(SymtableError { msg: "nonlocal declaration not allowed at module level".into(), span: s.span }); }
                for n in names {
                    let f = self.scope().flags(n);
                    if f.global { return Err(SymtableError { msg: format!("name '{n}' is nonlocal and global"), span: s.span }); }
                    if f.bound || f.used || f.param { return Err(SymtableError { msg: format!("name '{n}' is {} prior to nonlocal declaration", if f.param { "parameter and nonlocal" } else if f.bound { "assigned to before nonlocal declaration" } else { "used prior to nonlocal declaration" }), span: s.span }); }
                    f.nonlocal = true;
                }
            }
            StmtKind::Expr { value } => self.expr(value)?,
            StmtKind::Pass | StmtKind::Break | StmtKind::Continue => {}
        }
        Ok(())
    }

    fn annotation(&mut self, e: &Expr) -> SResult<()> {
        // Deferred annotations (3.14): names are only used, never bound.
        self.expr(e)
    }

    fn type_params(&mut self, tps: &[TypeParam]) -> SResult<()> {
        for tp in tps {
            match &tp.kind {
                TypeParamKind::TypeVar { name, bound, default_value } => { self.bind(name, tp.span)?; if let Some(b) = bound { self.expr(b)?; } if let Some(d) = default_value { self.expr(d)?; } }
                TypeParamKind::ParamSpec { name, default_value } | TypeParamKind::TypeVarTuple { name, default_value } => { self.bind(name, tp.span)?; if let Some(d) = default_value { self.expr(d)?; } }
            }
        }
        Ok(())
    }

    fn arg_defaults(&mut self, a: &Arguments) -> SResult<()> {
        for d in &a.defaults { self.expr(d)?; }
        for d in a.kw_defaults.iter().flatten() { self.expr(d)?; }
        for arg in a.posonlyargs.iter().chain(&a.args).chain(&a.kwonlyargs) { if let Some(ann) = &arg.annotation { self.annotation(ann)?; } }
        if let Some(v) = &a.vararg { if let Some(ann) = &v.annotation { self.annotation(ann)?; } }
        if let Some(k) = &a.kwarg { if let Some(ann) = &k.annotation { self.annotation(ann)?; } }
        Ok(())
    }

    fn arguments(&mut self, a: &Arguments) -> SResult<()> {
        for arg in a.posonlyargs.iter().chain(&a.args) { self.param(&arg.arg, arg.span)?; }
        for arg in &a.kwonlyargs { self.param(&arg.arg, arg.span)?; }
        if let Some(v) = &a.vararg { self.param(&v.arg, v.span)?; }
        if let Some(k) = &a.kwarg { self.param(&k.arg, k.span)?; }
        Ok(())
    }

    /// Assignment target: binds names, evaluates subexpressions.
    fn target(&mut self, e: &Expr) -> SResult<()> {
        match &e.kind {
            ExprKind::Name { id, .. } => self.bind(id, e.span),
            ExprKind::Tuple { elts, .. } | ExprKind::List { elts, .. } => { for x in elts { self.target(x)?; } Ok(()) }
            ExprKind::Starred { value, .. } => self.target(value),
            ExprKind::Attribute { value, .. } => self.expr(value),
            ExprKind::Subscript { value, slice, .. } => { self.expr(value)?; self.expr(slice) }
            _ => self.expr(e),
        }
    }
    fn target_use_only(&mut self, e: &Expr) -> SResult<()> {
        match &e.kind {
            ExprKind::Attribute { value, .. } => self.expr(value),
            ExprKind::Subscript { value, slice, .. } => { self.expr(value)?; self.expr(slice) }
            _ => self.expr(e),
        }
    }

    fn pattern(&mut self, p: &Pattern) -> SResult<()> {
        match &p.kind {
            PatternKind::MatchValue { value } => self.expr(value),
            PatternKind::MatchSingleton { .. } => Ok(()),
            PatternKind::MatchSequence { patterns } | PatternKind::MatchOr { patterns } => { for q in patterns { self.pattern(q)?; } Ok(()) }
            PatternKind::MatchMapping { keys, patterns, rest } => { for k in keys { self.expr(k)?; } for q in patterns { self.pattern(q)?; } if let Some(r) = rest { self.bind(r, p.span)?; } Ok(()) }
            PatternKind::MatchClass { cls, patterns, kwd_patterns, .. } => { self.expr(cls)?; for q in patterns { self.pattern(q)?; } for q in kwd_patterns { self.pattern(q)?; } Ok(()) }
            PatternKind::MatchStar { name } => { if let Some(n) = name { self.bind(n, p.span)?; } Ok(()) }
            PatternKind::MatchAs { pattern, name } => { if let Some(q) = pattern { self.pattern(q)?; } if let Some(n) = name { self.bind(n, p.span)?; } Ok(()) }
        }
    }

    fn comprehension(&mut self, name: &str, elt: &Expr, value: Option<&Expr>, generators: &[Comprehension]) -> SResult<()> {
        // The outermost iterable is evaluated in the enclosing scope.
        self.expr(&generators[0].iter)?;
        self.push(ScopeKind::Comprehension, name);
        self.param(".0", elt.span)?;
        if name == "<genexpr>" { self.scope().is_generator = true; }
        for (i, g) in generators.iter().enumerate() {
            if i > 0 { self.expr(&g.iter)?; }
            self.target(&g.target)?;
            for c in &g.ifs { self.expr(c)?; }
            if g.is_async { self.scope().is_coroutine = true; }
        }
        self.expr(elt)?;
        if let Some(v) = value { self.expr(v)?; }
        self.pop();
        Ok(())
    }

    fn expr(&mut self, e: &Expr) -> SResult<()> {
        match &e.kind {
            ExprKind::BoolOp { values, .. } => { for v in values { self.expr(v)?; } }
            ExprKind::NamedExpr { target, value } => {
                self.expr(value)?;
                // Binds in the nearest enclosing non-comprehension scope.
                if let ExprKind::Name { id, .. } = &target.kind {
                    let mut sc = self.cur;
                    while self.scopes[sc].kind == ScopeKind::Comprehension { sc = self.scopes[sc].parent.unwrap(); }
                    if self.scopes[sc].kind == ScopeKind::Class { return Err(SymtableError { msg: "assignment expression within a comprehension cannot be used in a class body".into(), span: e.span }); }
                    if sc != self.cur {
                        // Every comprehension between here and `sc` sees it as nonlocal.
                        let target_is_module = self.scopes[sc].kind == ScopeKind::Module;
                        let mut c = self.cur;
                        while c != sc { let f = self.scopes[c].flags(id); if target_is_module { f.global = true; } else { f.nonlocal = true; } c = self.scopes[c].parent.unwrap(); }
                    }
                    self.scopes[sc].flags(id).bound = true;
                }
            }
            ExprKind::BinOp { left, right, .. } => { self.expr(left)?; self.expr(right)?; }
            ExprKind::UnaryOp { operand, .. } => self.expr(operand)?,
            ExprKind::Lambda { args, body } => {
                self.arg_defaults(args)?;
                self.push(ScopeKind::Lambda, "<lambda>");
                self.arguments(args)?;
                self.expr(body)?;
                self.pop();
            }
            ExprKind::IfExp { test, body, orelse } => { self.expr(test)?; self.expr(body)?; self.expr(orelse)?; }
            ExprKind::Dict { keys, values } => { for k in keys.iter().flatten() { self.expr(k)?; } for v in values { self.expr(v)?; } }
            ExprKind::Set { elts } | ExprKind::List { elts, .. } | ExprKind::Tuple { elts, .. } => { for x in elts { self.expr(x)?; } }
            ExprKind::ListComp { elt, generators } => self.comprehension("<listcomp>", elt, None, generators)?,
            ExprKind::SetComp { elt, generators } => self.comprehension("<setcomp>", elt, None, generators)?,
            ExprKind::DictComp { key, value, generators } => self.comprehension("<dictcomp>", key, Some(value), generators)?,
            ExprKind::GeneratorExp { elt, generators } => self.comprehension("<genexpr>", elt, None, generators)?,
            ExprKind::Await { value } => { self.scope().is_coroutine = self.scope().is_coroutine || self.scope().kind == ScopeKind::Function; self.expr(value)?; }
            ExprKind::Yield { value } => { self.mark_generator(e.span)?; if let Some(v) = value { self.expr(v)?; } }
            ExprKind::YieldFrom { value } => { self.mark_generator(e.span)?; self.expr(value)?; }
            ExprKind::Compare { left, comparators, .. } => { self.expr(left)?; for c in comparators { self.expr(c)?; } }
            ExprKind::Call { func, args, keywords } => {
                if let ExprKind::Name { id, .. } = &func.kind { if id == "super" && args.is_empty() { self.use_name("__class__"); self.scope().flags("__class__").used = true; } }
                self.expr(func)?; for a in args { self.expr(a)?; } for k in keywords { self.expr(&k.value)?; }
            }
            ExprKind::FormattedValue { value, format_spec, .. } | ExprKind::Interpolation { value, format_spec, .. } => { self.expr(value)?; if let Some(s) = format_spec { self.expr(s)?; } }
            ExprKind::JoinedStr { values } | ExprKind::TemplateStr { values } => { for v in values { self.expr(v)?; } }
            ExprKind::Constant { .. } => {}
            ExprKind::Attribute { value, .. } => self.expr(value)?,
            ExprKind::Subscript { value, slice, .. } => { self.expr(value)?; self.expr(slice)?; }
            ExprKind::Starred { value, .. } => self.expr(value)?,
            ExprKind::Name { id, ctx } => { match ctx { ExprContext::Load => self.use_name(id), _ => self.bind(id, e.span)? } }
            ExprKind::Slice { lower, upper, step } => { for x in [lower, upper, step].into_iter().flatten() { self.expr(x)?; } }
        }
        Ok(())
    }

    fn mark_generator(&mut self, span: Span) -> SResult<()> {
        match self.scope().kind {
            ScopeKind::Function | ScopeKind::Lambda => { self.scope().is_generator = true; Ok(()) }
            ScopeKind::Comprehension => Err(SymtableError { msg: "'yield' inside list comprehension".into(), span }),
            _ => Err(SymtableError { msg: "'yield' outside function".into(), span }),
        }
    }
}

/// Second pass: decide each name's resolution and propagate frees/cells.
fn resolve(t: &mut SymbolTable, id: usize) -> SResult<()> {
    let kind = t.scopes[id].kind;
    let names: Vec<String> = t.scopes[id].order.clone();
    for name in &names {
        let f = t.scopes[id].symbols[name].clone();
        let r = if kind == ScopeKind::Module { Resolution::GlobalImplicit }
        else if f.global { Resolution::GlobalExplicit }
        else if f.nonlocal {
            if !bind_free(t, id, name) { return Err(SymtableError { msg: format!("no binding for nonlocal '{name}' found"), span: Span::default() }); }
            Resolution::Free
        }
        else if f.bound || f.param { Resolution::Local }
        else if f.used {
            if bind_free(t, id, name) { Resolution::Free } else { Resolution::GlobalImplicit }
        } else { Resolution::GlobalImplicit };
        t.scopes[id].resolved.insert(name.clone(), r);
        if r == Resolution::Free && !t.scopes[id].freevars.contains(name) { t.scopes[id].freevars.push(name.clone()); }
    }
    // varnames: params first, then other locals in first-seen order
    let mut varnames = t.scopes[id].params.clone();
    for name in &names { if t.scopes[id].is_local(name) && !varnames.contains(name) { varnames.push(name.clone()); } }
    t.scopes[id].varnames = varnames;
    let children = t.scopes[id].children.clone();
    for c in children { resolve(t, c)?; }
    // `__class__` cell: a class whose method scopes reference __class__ freely.
    if kind == ScopeKind::Class && t.scopes[id].needs_class_cell && !t.scopes[id].cellvars.contains(&"__class__".to_string()) {
        t.scopes[id].cellvars.push("__class__".into());
        t.scopes[id].resolved.insert("__class__".into(), Resolution::Cell);
    }
    Ok(())
}

/// Try to bind `name` as free in scope `id` by finding a binding in an
/// enclosing function scope. Marks the binding scope's symbol as a cell and
/// every intermediate function scope as free. Class scopes are skipped,
/// except that `__class__` binds to the nearest class.
fn bind_free(t: &mut SymbolTable, id: usize, name: &str) -> bool {
    let mut chain = Vec::new();
    let mut cur = t.scopes[id].parent;
    let mut found = None;
    while let Some(p) = cur {
        let s = &t.scopes[p];
        match s.kind {
            ScopeKind::Module => break,
            ScopeKind::Class => {
                if name == "__class__" { found = Some(p); break; }
                chain.push(p);
            }
            _ => {
                let f = s.symbols.get(name).cloned().unwrap_or_default();
                if f.global { return false; }
                if f.bound || f.param || f.nonlocal { found = Some(p); break; }
                chain.push(p);
            }
        }
        cur = s.parent;
    }
    let Some(binder) = found else { return false };
    if name == "__class__" && t.scopes[binder].kind == ScopeKind::Class {
        t.scopes[binder].needs_class_cell = true;
    } else {
        let bf = t.scopes[binder].symbols.get(name).cloned().unwrap_or_default();
        if bf.nonlocal {
            // pass-through: binder is itself free
            if t.scopes[binder].resolved.get(name).is_none() { if !bind_free(t, binder, name) { return false; } t.scopes[binder].resolved.insert(name.into(), Resolution::Free); if !t.scopes[binder].freevars.contains(&name.to_string()) { t.scopes[binder].freevars.push(name.into()); } }
        } else {
            t.scopes[binder].resolved.insert(name.into(), Resolution::Cell);
            if !t.scopes[binder].cellvars.contains(&name.to_string()) { t.scopes[binder].cellvars.push(name.into()); }
        }
    }
    for p in chain {
        if t.scopes[p].kind == ScopeKind::Class {
            if !t.scopes[p].freevars.contains(&name.to_string()) { t.scopes[p].freevars.push(name.into()); }
            continue;
        }
        t.scopes[p].resolved.insert(name.into(), Resolution::Free);
        if !t.scopes[p].freevars.contains(&name.to_string()) { t.scopes[p].freevars.push(name.into()); }
    }
    true
}

/// Names that are free in `id` or any child, useful for building closures.
pub fn all_free_names(t: &SymbolTable, id: usize) -> HashSet<String> {
    let mut out: HashSet<String> = t.scopes[id].freevars.iter().cloned().collect();
    for &c in &t.scopes[id].children { out.extend(all_free_names(t, c)); }
    out
}
