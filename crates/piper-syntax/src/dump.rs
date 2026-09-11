//! Renders an AST the way CPython's `ast.dump(tree, include_attributes=True)`
//! does, byte for byte. Used by the differential tests and by `piper --dump-ast`.

use crate::ast::*;
use crate::unicode_printable::is_printable;
use std::fmt::Write;

pub fn dump(m: &Mod) -> String {
    let mut w = W::default();
    w.module(m);
    w.out
}

#[derive(Default)]
struct W {
    out: String,
}

impl W {
    // ---- helpers -------------------------------------------------------

    fn node(&mut self, name: &str, f: impl FnOnce(&mut Fields)) {
        self.out.push_str(name);
        self.out.push('(');
        let mut fields = Fields { w: self, first: true };
        f(&mut fields);
        self.out.push(')');
    }

    fn span(&self, s: Span) -> String {
        format!("lineno={}, col_offset={}, end_lineno={}, end_col_offset={}", s.lineno, s.col_offset, s.end_lineno, s.end_col_offset)
    }

    // ---- nodes ---------------------------------------------------------

    fn module(&mut self, m: &Mod) {
        match m {
            Mod::Module { body, type_ignores } => self.node("Module", |f| {
                f.stmts("body", body);
                if !type_ignores.is_empty() {
                    f.raw_list("type_ignores", type_ignores.len(), |w, i| {
                        let t = &type_ignores[i];
                        w.node("TypeIgnore", |f| { f.raw("lineno", &t.lineno.to_string()); f.raw("tag", &py_str_repr(&t.tag)); });
                    });
                }
            }),
            Mod::Interactive { body } => self.node("Interactive", |f| f.stmts("body", body)),
            Mod::Expression { body } => self.node("Expression", |f| f.expr("body", body)),
            Mod::FunctionType { argtypes, returns } => self.node("FunctionType", |f| { f.exprs("argtypes", argtypes); f.expr("returns", returns); }),
        }
    }

    fn stmt(&mut self, s: &Stmt) {
        let sp = self.span(s.span);
        match &s.kind {
            StmtKind::FunctionDef { name, args, body, decorator_list, returns, type_comment, type_params }
            | StmtKind::AsyncFunctionDef { name, args, body, decorator_list, returns, type_comment, type_params } => {
                let n = if matches!(s.kind, StmtKind::FunctionDef { .. }) { "FunctionDef" } else { "AsyncFunctionDef" };
                self.node(n, |f| {
                    f.raw("name", &py_str_repr(name));
                    f.arguments("args", args);
                    f.stmts("body", body);
                    f.exprs("decorator_list", decorator_list);
                    f.opt_expr("returns", returns.as_deref());
                    f.opt_str("type_comment", type_comment.as_deref());
                    f.type_params("type_params", type_params);
                    f.raw_tail(&sp);
                })
            }
            StmtKind::ClassDef { name, bases, keywords, body, decorator_list, type_params } => self.node("ClassDef", |f| {
                f.raw("name", &py_str_repr(name));
                f.exprs("bases", bases);
                f.keywords("keywords", keywords);
                f.stmts("body", body);
                f.exprs("decorator_list", decorator_list);
                f.type_params("type_params", type_params);
                f.raw_tail(&sp);
            }),
            StmtKind::Return { value } => self.node("Return", |f| { f.opt_expr("value", value.as_deref()); f.raw_tail(&sp); }),
            StmtKind::Delete { targets } => self.node("Delete", |f| { f.exprs("targets", targets); f.raw_tail(&sp); }),
            StmtKind::Assign { targets, value, type_comment } => self.node("Assign", |f| {
                f.exprs("targets", targets);
                f.expr("value", value);
                f.opt_str("type_comment", type_comment.as_deref());
                f.raw_tail(&sp);
            }),
            StmtKind::TypeAlias { name, type_params, value } => self.node("TypeAlias", |f| {
                f.expr("name", name);
                f.type_params("type_params", type_params);
                f.expr("value", value);
                f.raw_tail(&sp);
            }),
            StmtKind::AugAssign { target, op, value } => self.node("AugAssign", |f| {
                f.expr("target", target);
                f.raw("op", &format!("{}()", operator_name(*op)));
                f.expr("value", value);
                f.raw_tail(&sp);
            }),
            StmtKind::AnnAssign { target, annotation, value, simple } => self.node("AnnAssign", |f| {
                f.expr("target", target);
                f.expr("annotation", annotation);
                f.opt_expr("value", value.as_deref());
                f.raw("simple", if *simple { "1" } else { "0" });
                f.raw_tail(&sp);
            }),
            StmtKind::For { target, iter, body, orelse, type_comment }
            | StmtKind::AsyncFor { target, iter, body, orelse, type_comment } => {
                let n = if matches!(s.kind, StmtKind::For { .. }) { "For" } else { "AsyncFor" };
                self.node(n, |f| {
                    f.expr("target", target);
                    f.expr("iter", iter);
                    f.stmts("body", body);
                    f.stmts("orelse", orelse);
                    f.opt_str("type_comment", type_comment.as_deref());
                    f.raw_tail(&sp);
                })
            }
            StmtKind::While { test, body, orelse } => self.node("While", |f| {
                f.expr("test", test);
                f.stmts("body", body);
                f.stmts("orelse", orelse);
                f.raw_tail(&sp);
            }),
            StmtKind::If { test, body, orelse } => self.node("If", |f| {
                f.expr("test", test);
                f.stmts("body", body);
                f.stmts("orelse", orelse);
                f.raw_tail(&sp);
            }),
            StmtKind::With { items, body, type_comment } | StmtKind::AsyncWith { items, body, type_comment } => {
                let n = if matches!(s.kind, StmtKind::With { .. }) { "With" } else { "AsyncWith" };
                self.node(n, |f| {
                    if !items.is_empty() {
                        f.raw_list("items", items.len(), |w, i| {
                            let it = &items[i];
                            w.node("withitem", |f| {
                                f.expr("context_expr", &it.context_expr);
                                f.opt_expr("optional_vars", it.optional_vars.as_ref());
                            });
                        });
                    }
                    f.stmts("body", body);
                    f.opt_str("type_comment", type_comment.as_deref());
                    f.raw_tail(&sp);
                })
            }
            StmtKind::Match { subject, cases } => self.node("Match", |f| {
                f.expr("subject", subject);
                if !cases.is_empty() {
                    f.raw_list("cases", cases.len(), |w, i| {
                        let c = &cases[i];
                        w.node("match_case", |f| {
                            f.pattern("pattern", &c.pattern);
                            f.opt_expr("guard", c.guard.as_deref());
                            f.stmts("body", &c.body);
                        });
                    });
                }
                f.raw_tail(&sp);
            }),
            StmtKind::Raise { exc, cause } => self.node("Raise", |f| {
                f.opt_expr("exc", exc.as_deref());
                f.opt_expr("cause", cause.as_deref());
                f.raw_tail(&sp);
            }),
            StmtKind::Try { body, handlers, orelse, finalbody } | StmtKind::TryStar { body, handlers, orelse, finalbody } => {
                let n = if matches!(s.kind, StmtKind::Try { .. }) { "Try" } else { "TryStar" };
                self.node(n, |f| {
                    f.stmts("body", body);
                    if !handlers.is_empty() {
                        f.raw_list("handlers", handlers.len(), |w, i| {
                            let h = &handlers[i];
                            let hsp = w.span(h.span);
                            w.node("ExceptHandler", |f| {
                                f.opt_expr("type", h.type_.as_deref());
                                f.opt_str("name", h.name.as_deref());
                                f.stmts("body", &h.body);
                                f.raw_tail(&hsp);
                            });
                        });
                    }
                    f.stmts("orelse", orelse);
                    f.stmts("finalbody", finalbody);
                    f.raw_tail(&sp);
                })
            }
            StmtKind::Assert { test, msg } => self.node("Assert", |f| {
                f.expr("test", test);
                f.opt_expr("msg", msg.as_deref());
                f.raw_tail(&sp);
            }),
            StmtKind::Import { names } => self.node("Import", |f| { f.aliases("names", names); f.raw_tail(&sp); }),
            StmtKind::ImportFrom { module, names, level } => self.node("ImportFrom", |f| {
                f.opt_str("module", module.as_deref());
                f.aliases("names", names);
                f.raw("level", &level.to_string());
                f.raw_tail(&sp);
            }),
            StmtKind::Global { names } => self.node("Global", |f| { f.str_list("names", names); f.raw_tail(&sp); }),
            StmtKind::Nonlocal { names } => self.node("Nonlocal", |f| { f.str_list("names", names); f.raw_tail(&sp); }),
            StmtKind::Expr { value } => self.node("Expr", |f| { f.expr("value", value); f.raw_tail(&sp); }),
            StmtKind::Pass => self.node("Pass", |f| f.raw_tail(&sp)),
            StmtKind::Break => self.node("Break", |f| f.raw_tail(&sp)),
            StmtKind::Continue => self.node("Continue", |f| f.raw_tail(&sp)),
        }
    }

    fn expr(&mut self, e: &Expr) {
        let sp = self.span(e.span);
        match &e.kind {
            ExprKind::BoolOp { op, values } => self.node("BoolOp", |f| {
                f.raw("op", match op { BoolOp::And => "And()", BoolOp::Or => "Or()" });
                f.exprs("values", values);
                f.raw_tail(&sp);
            }),
            ExprKind::NamedExpr { target, value } => self.node("NamedExpr", |f| { f.expr("target", target); f.expr("value", value); f.raw_tail(&sp); }),
            ExprKind::BinOp { left, op, right } => self.node("BinOp", |f| {
                f.expr("left", left);
                f.raw("op", &format!("{}()", operator_name(*op)));
                f.expr("right", right);
                f.raw_tail(&sp);
            }),
            ExprKind::UnaryOp { op, operand } => self.node("UnaryOp", |f| {
                f.raw("op", match op { UnaryOp::Invert => "Invert()", UnaryOp::Not => "Not()", UnaryOp::UAdd => "UAdd()", UnaryOp::USub => "USub()" });
                f.expr("operand", operand);
                f.raw_tail(&sp);
            }),
            ExprKind::Lambda { args, body } => self.node("Lambda", |f| { f.arguments("args", args); f.expr("body", body); f.raw_tail(&sp); }),
            ExprKind::IfExp { test, body, orelse } => self.node("IfExp", |f| { f.expr("test", test); f.expr("body", body); f.expr("orelse", orelse); f.raw_tail(&sp); }),
            ExprKind::Dict { keys, values } => self.node("Dict", |f| {
                if !keys.is_empty() {
                    f.raw_list("keys", keys.len(), |w, i| match &keys[i] { Some(k) => w.expr(k), None => w.out.push_str("None") });
                }
                f.exprs("values", values);
                f.raw_tail(&sp);
            }),
            ExprKind::Set { elts } => self.node("Set", |f| { f.exprs("elts", elts); f.raw_tail(&sp); }),
            ExprKind::ListComp { elt, generators } => self.node("ListComp", |f| { f.expr("elt", elt); f.generators(generators); f.raw_tail(&sp); }),
            ExprKind::SetComp { elt, generators } => self.node("SetComp", |f| { f.expr("elt", elt); f.generators(generators); f.raw_tail(&sp); }),
            ExprKind::DictComp { key, value, generators } => self.node("DictComp", |f| { f.expr("key", key); f.expr("value", value); f.generators(generators); f.raw_tail(&sp); }),
            ExprKind::GeneratorExp { elt, generators } => self.node("GeneratorExp", |f| { f.expr("elt", elt); f.generators(generators); f.raw_tail(&sp); }),
            ExprKind::Await { value } => self.node("Await", |f| { f.expr("value", value); f.raw_tail(&sp); }),
            ExprKind::Yield { value } => self.node("Yield", |f| { f.opt_expr("value", value.as_deref()); f.raw_tail(&sp); }),
            ExprKind::YieldFrom { value } => self.node("YieldFrom", |f| { f.expr("value", value); f.raw_tail(&sp); }),
            ExprKind::Compare { left, ops, comparators } => self.node("Compare", |f| {
                f.expr("left", left);
                if !ops.is_empty() {
                    f.raw_list("ops", ops.len(), |w, i| { w.out.push_str(cmpop_name(ops[i])); w.out.push_str("()"); });
                }
                f.exprs("comparators", comparators);
                f.raw_tail(&sp);
            }),
            ExprKind::Call { func, args, keywords } => self.node("Call", |f| {
                f.expr("func", func);
                f.exprs("args", args);
                f.keywords("keywords", keywords);
                f.raw_tail(&sp);
            }),
            ExprKind::FormattedValue { value, conversion, format_spec } => self.node("FormattedValue", |f| {
                f.expr("value", value);
                f.raw("conversion", &conversion.to_string());
                f.opt_expr("format_spec", format_spec.as_deref());
                f.raw_tail(&sp);
            }),
            ExprKind::Interpolation { value, str, conversion, format_spec } => self.node("Interpolation", |f| {
                f.expr("value", value);
                f.raw("str", &py_str_repr(str));
                f.raw("conversion", &conversion.to_string());
                f.opt_expr("format_spec", format_spec.as_deref());
                f.raw_tail(&sp);
            }),
            ExprKind::JoinedStr { values } => self.node("JoinedStr", |f| { f.exprs("values", values); f.raw_tail(&sp); }),
            ExprKind::TemplateStr { values } => self.node("TemplateStr", |f| { f.exprs("values", values); f.raw_tail(&sp); }),
            ExprKind::Constant { value, kind } => self.node("Constant", |f| {
                f.raw("value", &constant_repr(value));
                f.opt_str("kind", kind.as_deref());
                f.raw_tail(&sp);
            }),
            ExprKind::Attribute { value, attr, ctx } => self.node("Attribute", |f| {
                f.expr("value", value);
                f.raw("attr", &py_str_repr(attr));
                f.raw("ctx", ctx_name(*ctx));
                f.raw_tail(&sp);
            }),
            ExprKind::Subscript { value, slice, ctx } => self.node("Subscript", |f| {
                f.expr("value", value);
                f.expr("slice", slice);
                f.raw("ctx", ctx_name(*ctx));
                f.raw_tail(&sp);
            }),
            ExprKind::Starred { value, ctx } => self.node("Starred", |f| { f.expr("value", value); f.raw("ctx", ctx_name(*ctx)); f.raw_tail(&sp); }),
            ExprKind::Name { id, ctx } => self.node("Name", |f| { f.raw("id", &py_str_repr(id)); f.raw("ctx", ctx_name(*ctx)); f.raw_tail(&sp); }),
            ExprKind::List { elts, ctx } => self.node("List", |f| { f.exprs("elts", elts); f.raw("ctx", ctx_name(*ctx)); f.raw_tail(&sp); }),
            ExprKind::Tuple { elts, ctx } => self.node("Tuple", |f| { f.exprs("elts", elts); f.raw("ctx", ctx_name(*ctx)); f.raw_tail(&sp); }),
            ExprKind::Slice { lower, upper, step } => self.node("Slice", |f| {
                f.opt_expr("lower", lower.as_deref());
                f.opt_expr("upper", upper.as_deref());
                f.opt_expr("step", step.as_deref());
                f.raw_tail(&sp);
            }),
        }
    }

    fn pattern(&mut self, p: &Pattern) {
        let sp = self.span(p.span);
        match &p.kind {
            PatternKind::MatchValue { value } => self.node("MatchValue", |f| { f.expr("value", value); f.raw_tail(&sp); }),
            PatternKind::MatchSingleton { value } => self.node("MatchSingleton", |f| { f.raw("value", &constant_repr(value)); f.raw_tail(&sp); }),
            PatternKind::MatchSequence { patterns } => self.node("MatchSequence", |f| { f.patterns("patterns", patterns); f.raw_tail(&sp); }),
            PatternKind::MatchMapping { keys, patterns, rest } => self.node("MatchMapping", |f| {
                f.exprs("keys", keys);
                f.patterns("patterns", patterns);
                f.opt_str("rest", rest.as_deref());
                f.raw_tail(&sp);
            }),
            PatternKind::MatchClass { cls, patterns, kwd_attrs, kwd_patterns } => self.node("MatchClass", |f| {
                f.expr("cls", cls);
                f.patterns("patterns", patterns);
                f.str_list("kwd_attrs", kwd_attrs);
                f.patterns("kwd_patterns", kwd_patterns);
                f.raw_tail(&sp);
            }),
            PatternKind::MatchStar { name } => self.node("MatchStar", |f| { f.opt_str("name", name.as_deref()); f.raw_tail(&sp); }),
            PatternKind::MatchAs { pattern, name } => self.node("MatchAs", |f| {
                if let Some(p) = pattern { f.key("pattern"); f.w.pattern(p); }
                f.opt_str("name", name.as_deref());
                f.raw_tail(&sp);
            }),
            PatternKind::MatchOr { patterns } => self.node("MatchOr", |f| { f.patterns("patterns", patterns); f.raw_tail(&sp); }),
        }
    }

    fn arg(&mut self, a: &Arg) {
        let sp = self.span(a.span);
        self.node("arg", |f| {
            f.raw("arg", &py_str_repr(&a.arg));
            f.opt_expr("annotation", a.annotation.as_deref());
            f.opt_str("type_comment", a.type_comment.as_deref());
            f.raw_tail(&sp);
        });
    }
}

/// Field emitter for one node; handles comma placement.
struct Fields<'a> {
    w: &'a mut W,
    first: bool,
}

impl Fields<'_> {
    fn key(&mut self, name: &str) {
        if !self.first { self.w.out.push_str(", "); }
        self.first = false;
        self.w.out.push_str(name);
        self.w.out.push('=');
    }
    fn raw(&mut self, name: &str, value: &str) { self.key(name); self.w.out.push_str(value); }
    fn raw_tail(&mut self, span_text: &str) {
        if !self.first { self.w.out.push_str(", "); }
        self.first = false;
        self.w.out.push_str(span_text);
    }
    fn opt_str(&mut self, name: &str, v: Option<&str>) { if let Some(s) = v { self.raw(name, &py_str_repr(s)); } }
    fn expr(&mut self, name: &str, e: &Expr) { self.key(name); self.w.expr(e); }
    fn opt_expr(&mut self, name: &str, e: Option<&Expr>) { if let Some(e) = e { self.expr(name, e); } }
    fn raw_list(&mut self, name: &str, n: usize, mut item: impl FnMut(&mut W, usize)) {
        self.key(name);
        self.w.out.push('[');
        for i in 0..n {
            if i > 0 { self.w.out.push_str(", "); }
            item(self.w, i);
        }
        self.w.out.push(']');
    }
    fn exprs(&mut self, name: &str, es: &[Expr]) {
        if es.is_empty() { return; }
        self.raw_list(name, es.len(), |w, i| w.expr(&es[i]));
    }
    fn stmts(&mut self, name: &str, ss: &[Stmt]) {
        if ss.is_empty() { return; }
        self.raw_list(name, ss.len(), |w, i| w.stmt(&ss[i]));
    }
    fn patterns(&mut self, name: &str, ps: &[Pattern]) {
        if ps.is_empty() { return; }
        self.raw_list(name, ps.len(), |w, i| w.pattern(&ps[i]));
    }
    fn pattern(&mut self, name: &str, p: &Pattern) { self.key(name); self.w.pattern(p); }
    fn str_list(&mut self, name: &str, ss: &[String]) {
        if ss.is_empty() { return; }
        self.raw_list(name, ss.len(), |w, i| w.out.push_str(&py_str_repr(&ss[i])));
    }
    fn aliases(&mut self, name: &str, al: &[Alias]) {
        if al.is_empty() { return; }
        self.raw_list(name, al.len(), |w, i| {
            let a = &al[i];
            let sp = w.span(a.span);
            w.node("alias", |f| { f.raw("name", &py_str_repr(&a.name)); f.opt_str("asname", a.asname.as_deref()); f.raw_tail(&sp); });
        });
    }
    fn keywords(&mut self, name: &str, ks: &[Keyword]) {
        if ks.is_empty() { return; }
        self.raw_list(name, ks.len(), |w, i| {
            let k = &ks[i];
            let sp = w.span(k.span);
            w.node("keyword", |f| { f.opt_str("arg", k.arg.as_deref()); f.expr("value", &k.value); f.raw_tail(&sp); });
        });
    }
    fn type_params(&mut self, name: &str, tps: &[TypeParam]) {
        if tps.is_empty() { return; }
        self.raw_list(name, tps.len(), |w, i| {
            let t = &tps[i];
            let sp = w.span(t.span);
            match &t.kind {
                TypeParamKind::TypeVar { name, bound, default_value } => w.node("TypeVar", |f| {
                    f.raw("name", &py_str_repr(name));
                    f.opt_expr("bound", bound.as_deref());
                    f.opt_expr("default_value", default_value.as_deref());
                    f.raw_tail(&sp);
                }),
                TypeParamKind::ParamSpec { name, default_value } => w.node("ParamSpec", |f| {
                    f.raw("name", &py_str_repr(name));
                    f.opt_expr("default_value", default_value.as_deref());
                    f.raw_tail(&sp);
                }),
                TypeParamKind::TypeVarTuple { name, default_value } => w.node("TypeVarTuple", |f| {
                    f.raw("name", &py_str_repr(name));
                    f.opt_expr("default_value", default_value.as_deref());
                    f.raw_tail(&sp);
                }),
            }
        });
    }
    fn generators(&mut self, gens: &[Comprehension]) {
        if gens.is_empty() { return; }
        self.raw_list("generators", gens.len(), |w, i| {
            let g = &gens[i];
            w.node("comprehension", |f| {
                f.expr("target", &g.target);
                f.expr("iter", &g.iter);
                f.exprs("ifs", &g.ifs);
                f.raw("is_async", if g.is_async { "1" } else { "0" });
            });
        });
    }
    fn arguments(&mut self, name: &str, a: &Arguments) {
        self.key(name);
        self.w.node("arguments", |f| {
            if !a.posonlyargs.is_empty() { f.raw_list("posonlyargs", a.posonlyargs.len(), |w, i| w.arg(&a.posonlyargs[i])); }
            if !a.args.is_empty() { f.raw_list("args", a.args.len(), |w, i| w.arg(&a.args[i])); }
            if let Some(v) = &a.vararg { f.key("vararg"); f.w.arg(v); }
            if !a.kwonlyargs.is_empty() { f.raw_list("kwonlyargs", a.kwonlyargs.len(), |w, i| w.arg(&a.kwonlyargs[i])); }
            if !a.kw_defaults.is_empty() {
                f.raw_list("kw_defaults", a.kw_defaults.len(), |w, i| match &a.kw_defaults[i] { Some(e) => w.expr(e), None => w.out.push_str("None") });
            }
            if let Some(k) = &a.kwarg { f.key("kwarg"); f.w.arg(k); }
            f.exprs("defaults", &a.defaults);
        });
    }
}

fn ctx_name(c: ExprContext) -> &'static str {
    match c { ExprContext::Load => "Load()", ExprContext::Store => "Store()", ExprContext::Del => "Del()" }
}

pub fn operator_name(op: Operator) -> &'static str {
    match op {
        Operator::Add => "Add", Operator::Sub => "Sub", Operator::Mult => "Mult", Operator::MatMult => "MatMult",
        Operator::Div => "Div", Operator::Mod => "Mod", Operator::Pow => "Pow", Operator::LShift => "LShift",
        Operator::RShift => "RShift", Operator::BitOr => "BitOr", Operator::BitXor => "BitXor",
        Operator::BitAnd => "BitAnd", Operator::FloorDiv => "FloorDiv",
    }
}

fn cmpop_name(op: CmpOp) -> &'static str {
    match op {
        CmpOp::Eq => "Eq", CmpOp::NotEq => "NotEq", CmpOp::Lt => "Lt", CmpOp::LtE => "LtE", CmpOp::Gt => "Gt",
        CmpOp::GtE => "GtE", CmpOp::Is => "Is", CmpOp::IsNot => "IsNot", CmpOp::In => "In", CmpOp::NotIn => "NotIn",
    }
}

pub fn constant_repr(c: &Constant) -> String {
    match c {
        Constant::None => "None".into(),
        Constant::Bool(true) => "True".into(),
        Constant::Bool(false) => "False".into(),
        Constant::Int(s) => s.clone(),
        Constant::Float(f) => py_float_repr(*f),
        Constant::Complex(im) => format!("{}j", py_complex_part_repr(*im)),
        Constant::Str(s) => py_str_repr(s),
        Constant::Bytes(b) => py_bytes_repr(b),
        Constant::Ellipsis => "Ellipsis".into(),
    }
}

/// `repr()` of a Python `str`.
pub fn py_str_repr(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut r = String::with_capacity(s.len() + 2);
    r.push(q);
    for c in s.chars() {
        match c {
            '\\' => r.push_str("\\\\"),
            '\n' => r.push_str("\\n"),
            '\r' => r.push_str("\\r"),
            '\t' => r.push_str("\\t"),
            c if c == q => { r.push('\\'); r.push(c); }
            c if is_printable(c) => r.push(c),
            c => {
                let u = c as u32;
                if u < 0x100 { let _ = write!(r, "\\x{u:02x}"); }
                else if u < 0x10000 { let _ = write!(r, "\\u{u:04x}"); }
                else { let _ = write!(r, "\\U{u:08x}"); }
            }
        }
    }
    r.push(q);
    r
}

/// `repr()` of a Python `bytes`.
pub fn py_bytes_repr(b: &[u8]) -> String {
    let q = if b.contains(&b'\'') && !b.contains(&b'"') { '"' } else { '\'' };
    let mut r = String::with_capacity(b.len() + 3);
    r.push('b');
    r.push(q);
    for &c in b {
        match c {
            b'\\' => r.push_str("\\\\"),
            b'\n' => r.push_str("\\n"),
            b'\r' => r.push_str("\\r"),
            b'\t' => r.push_str("\\t"),
            c if c == q as u8 => { r.push('\\'); r.push(c as char); }
            0x20..=0x7e => r.push(c as char),
            c => { let _ = write!(r, "\\x{c:02x}"); }
        }
    }
    r.push(q);
    r
}

/// `repr()` of a Python `float`: shortest round-trip digits, fixed notation
/// for exponents in `[-4, 16)`, scientific otherwise, always with a `.0` or
/// exponent so it reads back as a float.
pub fn py_float_repr(f: f64) -> String {
    if f.is_nan() { return "nan".into(); }
    if f.is_infinite() { return if f > 0.0 { "inf".into() } else { "-inf".into() }; }
    let (neg, digits, exp) = shortest_digits(f);
    let mut out = String::new();
    if neg { out.push('-'); }
    out.push_str(&format_digits(&digits, exp, true));
    out
}

/// `repr()` of the imaginary part of a complex: like float repr, but
/// integral values print without `.0`.
fn py_complex_part_repr(f: f64) -> String {
    if f.is_nan() { return "nan".into(); }
    if f.is_infinite() { return if f > 0.0 { "inf".into() } else { "-inf".into() }; }
    let (neg, digits, exp) = shortest_digits(f);
    let mut out = String::new();
    if neg { out.push('-'); }
    out.push_str(&format_digits(&digits, exp, false));
    out
}

/// Shortest round-trip decimal digits and the decimal exponent such that
/// value = 0.d1d2d3... * 10^exp.
fn shortest_digits(f: f64) -> (bool, Vec<u8>, i32) {
    if f == 0.0 { return (f.is_sign_negative(), vec![0], 1); }
    let s = format!("{:e}", f.abs());
    let (mant, e) = s.split_once('e').unwrap();
    let e: i32 = e.parse().unwrap();
    let digits: Vec<u8> = mant.bytes().filter(|b| b.is_ascii_digit()).map(|b| b - b'0').collect();
    (f.is_sign_negative(), digits, e + 1)
}

fn format_digits(digits: &[u8], exp: i32, add_dot_zero: bool) -> String {
    let n = digits.len() as i32;
    let ds = |d: &[u8]| d.iter().map(|&x| (x + b'0') as char).collect::<String>();
    // Python: use scientific if exp10 < -4 or exp10 >= 16, where exp10 = exp - 1.
    let exp10 = exp - 1;
    if !(-4..16).contains(&exp10) {
        let mut m = String::new();
        m.push((digits[0] + b'0') as char);
        if n > 1 { m.push('.'); m.push_str(&ds(&digits[1..])); }
        return format!("{m}e{}{:02}", if exp10 < 0 { '-' } else { '+' }, exp10.abs());
    }
    let mut out = String::new();
    if exp <= 0 {
        out.push_str("0.");
        for _ in 0..(-exp) { out.push('0'); }
        out.push_str(&ds(digits));
    } else if exp >= n {
        out.push_str(&ds(digits));
        for _ in 0..(exp - n) { out.push('0'); }
        if add_dot_zero { out.push_str(".0"); }
    } else {
        out.push_str(&ds(&digits[..exp as usize]));
        out.push('.');
        out.push_str(&ds(&digits[exp as usize..]));
    }
    out
}
