//! The piper driver: parse, analyze, lower, optimize, emit, link.

use piper_llvm::target::Target;
use piper_llvm::{Codegen, OptLevel, link, lower};
use piper_syntax::ast::{CmpOp, Constant, Expr, ExprKind, Mod, Stmt, StmtKind};
use piper_syntax::symtable;
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

pub mod repl;
pub mod update;

/// Make the embedded link inputs visible to sysroot discovery. Cheap and
/// idempotent, so every entry point can call it.
pub fn init() {
    piper_llvm::target::set_bundled_sysroot_provider(piper_bundle::sysroot);
}

#[derive(Debug, Clone)]
pub struct CompileOptions {
    pub opt: OptLevel,
    pub triple: Option<String>,
    pub emit_ir: bool,
    pub emit_object: bool,
    /// Explicit sysroot for the target's C library.
    pub sysroot: Option<PathBuf>,
    /// Explicit runtime archive, overriding the bundled one.
    pub runtime: Option<PathBuf>,
    /// Link the target's libc statically where possible.
    pub static_libc: bool,
    /// Produce a native shared library.
    pub library: bool,
    /// Module name exported by a shared library.
    pub module_name: Option<String>,
}

impl Default for CompileOptions {
    fn default() -> Self { CompileOptions { opt: OptLevel::O2, triple: None, emit_ir: false, emit_object: false, sysroot: None, runtime: None, static_libc: true, library: false, module_name: None } }
}

/// A compile failure with a location the CLI can render.
#[derive(Debug, Clone)]
pub struct CompileError {
    pub msg: String,
    pub lineno: u32,
    pub col: u32,
}

impl std::fmt::Display for CompileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "{}:{}: {}", self.lineno, self.col, self.msg) }
}

/// Directory holding the runtime archives built alongside this driver.
fn built_runtime_dir() -> Option<PathBuf> { option_env!("PIPER_RT_RUNTIME_DIR").map(PathBuf::from) }

/// Path of the host runtime archive built alongside this driver.
fn host_archive() -> Option<PathBuf> { option_env!("PIPER_RT_ARCHIVE").map(PathBuf::from) }

/// Locate the runtime archive for `target`: an explicit path, an environment
/// variable, the archives built with this compiler, a bundled install
/// directory, or the host archive when the target is the host.
pub fn runtime_archive_for(target: &Target, explicit: Option<&Path>) -> Result<PathBuf, String> {
    init();
    if let Some(p) = explicit {
        if !p.exists() { return Err(format!("runtime archive not found: {}", p.display())); }
        return Ok(p.to_path_buf());
    }
    let var = format!("PIPER_RT_{}", target.dir_name().to_uppercase().replace('-', "_"));
    if let Ok(v) = std::env::var(&var) { let p = PathBuf::from(v); if p.exists() { return Ok(p); } }
    if let Some(dir) = built_runtime_dir() {
        let p = dir.join(target.dir_name()).join(target.runtime_archive_name());
        if p.exists() { return Ok(p); }
    }
    if let Some(p) = piper_bundle::runtime_archive(&target.dir_name()) { return Ok(p); }
    if let Some(dir) = piper_llvm::target::bundled_dir() {
        let p = dir.join("runtime").join(target.dir_name()).join(target.runtime_archive_name());
        if p.exists() { return Ok(p); }
    }
    if target.is_host() { if let Some(p) = host_archive() { if p.exists() { return Ok(p); } } }
    Err(format!("no piper runtime for {}: build it by setting PIPER_CROSS_TARGETS={} when building piper, or point {var} at an existing archive", target.triple, target.dir_name()))
}

/// Lower `src` (from `filename`) into LLVM IR; returns the codegen holding the module.
pub fn lower_source(src: &str, filename: &str) -> Result<Codegen, CompileError> {
    lower_source_named(src, filename, "__main__")
}

/// Lower a source module using an explicit import name.
pub fn lower_source_named(src: &str, filename: &str, modname: &str) -> Result<Codegen, CompileError> {
    let m = piper_syntax::parse_module(src, filename).map_err(|e| CompileError { msg: e.msg, lineno: e.lineno, col: e.col_offset })?;
    let st = symtable::analyze(&m).map_err(|e| CompileError { msg: e.msg, lineno: e.span.lineno, col: e.span.col_offset })?;
    let mut cg = Codegen::new(modname);
    lower::lower_module(&mut cg, &m, &st, filename, modname).map_err(|e| CompileError { msg: e.msg, lineno: e.span.lineno, col: e.span.col_offset })?;
    Ok(cg)
}

struct SourceModule {
    name: String,
    source: String,
    tree: Mod,
    is_package: bool,
    bundled: bool,
}

fn called_names_expr(expr: &Expr, out: &mut BTreeSet<String>) {
    match &expr.kind {
        ExprKind::Call { func, args, keywords } => {
            if let ExprKind::Name { id, .. } = &func.kind { out.insert(id.clone()); }
            called_names_expr(func, out);
            for arg in args { called_names_expr(arg, out); }
            for keyword in keywords { called_names_expr(&keyword.value, out); }
        }
        ExprKind::BoolOp { values, .. } | ExprKind::JoinedStr { values } | ExprKind::TemplateStr { values }
        | ExprKind::List { elts: values, .. } | ExprKind::Tuple { elts: values, .. } | ExprKind::Set { elts: values } =>
            for value in values { called_names_expr(value, out); },
        ExprKind::NamedExpr { target, value } | ExprKind::BinOp { left: target, right: value, .. }
        | ExprKind::Subscript { value: target, slice: value, .. } => {
            called_names_expr(target, out); called_names_expr(value, out);
        }
        ExprKind::UnaryOp { operand, .. } | ExprKind::Await { value: operand } | ExprKind::YieldFrom { value: operand }
        | ExprKind::Attribute { value: operand, .. } | ExprKind::Starred { value: operand, .. } => called_names_expr(operand, out),
        ExprKind::Lambda { body, .. } => called_names_expr(body, out),
        ExprKind::IfExp { test, body, orelse } => {
            called_names_expr(test, out); called_names_expr(body, out); called_names_expr(orelse, out);
        }
        ExprKind::Dict { keys, values } => {
            for key in keys.iter().flatten() { called_names_expr(key, out); }
            for value in values { called_names_expr(value, out); }
        }
        ExprKind::ListComp { elt, generators } | ExprKind::SetComp { elt, generators }
        | ExprKind::GeneratorExp { elt, generators } => {
            called_names_expr(elt, out);
            for generator in generators {
                called_names_expr(&generator.iter, out);
                for condition in &generator.ifs { called_names_expr(condition, out); }
            }
        }
        ExprKind::DictComp { key, value, generators } => {
            called_names_expr(key, out); called_names_expr(value, out);
            for generator in generators {
                called_names_expr(&generator.iter, out);
                for condition in &generator.ifs { called_names_expr(condition, out); }
            }
        }
        ExprKind::Yield { value } => { if let Some(value) = value { called_names_expr(value, out); } }
        ExprKind::Compare { left, comparators, .. } => {
            called_names_expr(left, out); for value in comparators { called_names_expr(value, out); }
        }
        ExprKind::FormattedValue { value, format_spec, .. } | ExprKind::Interpolation { value, format_spec, .. } => {
            called_names_expr(value, out); if let Some(spec) = format_spec { called_names_expr(spec, out); }
        }
        ExprKind::Slice { lower, upper, step } => {
            for value in [lower, upper, step].into_iter().flatten() { called_names_expr(value, out); }
        }
        ExprKind::Constant { .. } | ExprKind::Name { .. } => {}
    }
}

fn reachable_stmt_data(stmts: &[Stmt], imports: &mut Vec<(Option<String>, Vec<String>, u32)>, calls: &mut BTreeSet<String>) {
    for stmt in stmts {
        match &stmt.kind {
            StmtKind::Import { names } => for alias in names { imports.push((Some(alias.name.clone()), Vec::new(), 0)); },
            StmtKind::ImportFrom { module, names, level } => imports.push((module.clone(), names.iter().map(|a| a.name.clone()).collect(), *level)),
            StmtKind::FunctionDef { decorator_list, args, .. } | StmtKind::AsyncFunctionDef { decorator_list, args, .. } => {
                for value in decorator_list.iter().chain(args.defaults.iter()).chain(args.kw_defaults.iter().flatten()) { called_names_expr(value, calls); }
            }
            StmtKind::ClassDef { bases, keywords, body, decorator_list, .. } => {
                for value in bases.iter().chain(decorator_list.iter()) { called_names_expr(value, calls); }
                for keyword in keywords { called_names_expr(&keyword.value, calls); }
                reachable_stmt_data(body, imports, calls);
            }
            StmtKind::Return { value } => { if let Some(value) = value { called_names_expr(value, calls); } }
            StmtKind::Delete { targets } => for value in targets { called_names_expr(value, calls); },
            StmtKind::Assign { targets, value, .. } => { for target in targets { called_names_expr(target, calls); } called_names_expr(value, calls); }
            StmtKind::TypeAlias { name, value, .. } => { called_names_expr(name, calls); called_names_expr(value, calls); }
            StmtKind::AugAssign { target, value, .. } => { called_names_expr(target, calls); called_names_expr(value, calls); }
            StmtKind::AnnAssign { target, annotation, value, .. } => {
                called_names_expr(target, calls); called_names_expr(annotation, calls); if let Some(value) = value { called_names_expr(value, calls); }
            }
            StmtKind::For { target, iter, body, orelse, .. } | StmtKind::AsyncFor { target, iter, body, orelse, .. } => {
                called_names_expr(target, calls); called_names_expr(iter, calls); reachable_stmt_data(body, imports, calls); reachable_stmt_data(orelse, imports, calls);
            }
            StmtKind::While { test, body, orelse } | StmtKind::If { test, body, orelse } => {
                called_names_expr(test, calls);
                if main_guard(test) { reachable_stmt_data(orelse, imports, calls); }
                else { reachable_stmt_data(body, imports, calls); reachable_stmt_data(orelse, imports, calls); }
            }
            StmtKind::With { items, body, .. } | StmtKind::AsyncWith { items, body, .. } => {
                for item in items { called_names_expr(&item.context_expr, calls); }
                reachable_stmt_data(body, imports, calls);
            }
            StmtKind::Match { subject, cases } => { called_names_expr(subject, calls); for case in cases { reachable_stmt_data(&case.body, imports, calls); } }
            StmtKind::Raise { exc, cause } => for value in [exc, cause].into_iter().flatten() { called_names_expr(value, calls); },
            StmtKind::Try { body, handlers, orelse, finalbody } | StmtKind::TryStar { body, handlers, orelse, finalbody } => {
                reachable_stmt_data(body, imports, calls); for handler in handlers { reachable_stmt_data(&handler.body, imports, calls); }
                reachable_stmt_data(orelse, imports, calls); reachable_stmt_data(finalbody, imports, calls);
            }
            StmtKind::Assert { test, msg } => { called_names_expr(test, calls); if let Some(msg) = msg { called_names_expr(msg, calls); } }
            StmtKind::Expr { value } => called_names_expr(value, calls),
            StmtKind::Global { .. } | StmtKind::Nonlocal { .. } | StmtKind::Pass | StmtKind::Break | StmtKind::Continue => {}
        }
    }
}

fn reachable_imports(stmts: &[Stmt]) -> Vec<(Option<String>, Vec<String>, u32)> {
    let mut functions = BTreeMap::new();
    for stmt in stmts {
        match &stmt.kind {
            StmtKind::FunctionDef { name, body, .. } | StmtKind::AsyncFunctionDef { name, body, .. } => { functions.insert(name.as_str(), body.as_slice()); }
            _ => {}
        }
    }
    let mut imports = Vec::new();
    let mut calls = BTreeSet::new();
    reachable_stmt_data(stmts, &mut imports, &mut calls);
    let mut scanned = BTreeSet::new();
    while let Some(name) = calls.iter().find(|name| !scanned.contains(*name)).cloned() {
        scanned.insert(name.clone());
        if let Some(body) = functions.get(name.as_str()) { reachable_stmt_data(body, &mut imports, &mut calls); }
    }
    imports
}

fn main_guard(expr: &Expr) -> bool {
    let ExprKind::Compare { left, ops, comparators } = &expr.kind else { return false };
    matches!((&left.kind, ops.as_slice(), comparators.as_slice()),
        (ExprKind::Name { id, .. }, [CmpOp::Eq], [Expr { kind: ExprKind::Constant { value: Constant::Str(value), .. }, .. }])
            if id == "__name__" && value == "__main__")
}

fn absolute_import(current: &str, is_package: bool, module: Option<&str>, level: u32) -> Option<String> {
    if level == 0 { return module.map(str::to_string); }
    let package = if is_package { current } else { current.rsplit_once('.').map(|(p, _)| p).unwrap_or("") };
    let mut parts: Vec<&str> = package.split('.').filter(|p| !p.is_empty()).collect();
    for _ in 1..level { parts.pop()?; }
    if let Some(module) = module { parts.extend(module.split('.').filter(|p| !p.is_empty())); }
    (!parts.is_empty()).then(|| parts.join("."))
}

fn module_source(root: &Path, name: &str) -> Option<(PathBuf, bool)> {
    if name.split('.').any(|part| part.is_empty() || !part.chars().all(|c| c == '_' || c.is_alphanumeric())) { return None; }
    let relative = name.split('.').fold(PathBuf::new(), |path, part| path.join(part));
    let file = root.join(&relative).with_extension("py");
    if file.is_file() { return Some((file, false)); }
    let package = root.join(relative).join("__init__.py");
    package.is_file().then_some((package, true))
}

fn push_module_and_parents(pending: &mut Vec<String>, name: &str) {
    let parts: Vec<&str> = name.split('.').collect();
    for end in 1..=parts.len() { pending.push(parts[..end].join(".")); }
}

fn discover_modules(root: &Path, entry_tree: &Mod, entry_name: &str, entry_is_package: bool) -> Result<Vec<SourceModule>, String> {
    let mut imports = Vec::new();
    if let Mod::Module { body, .. } = entry_tree { imports = reachable_imports(body); }
    let mut pending = Vec::new();
    for (module, names, level) in imports {
        if let Some(base) = absolute_import(entry_name, entry_is_package, module.as_deref(), level) {
            push_module_and_parents(&mut pending, &base);
            for name in names { if name != "*" { push_module_and_parents(&mut pending, &format!("{base}.{name}")); } }
        }
    }
    let mut seen = BTreeSet::new();
    let mut found = BTreeMap::new();
    while let Some(name) = pending.pop() {
        if !seen.insert(name.clone()) { continue; }
        let (source, is_package, bundled) = if let Some((path, is_package)) = module_source(root, &name) {
            (std::fs::read_to_string(&path).map_err(|e| format!("can't open module '{name}': {e}"))?, is_package, false)
        } else if let Some((source, is_package)) = piper_bundle::stdlib_source(&name) {
            (source.to_string(), is_package, true)
        } else { continue };
        let logical_file = if is_package { format!("{}/__init__.py", name.replace('.', "/")) } else { format!("{}.py", name.replace('.', "/")) };
        let tree = piper_syntax::parse_module(&source, &logical_file).map_err(|e| format!("  File \"{logical_file}\", line {}\nSyntaxError: {}", e.lineno, e.msg))?;
        let mut imports = Vec::new();
        if let Mod::Module { body, .. } = &tree { imports = reachable_imports(body); }
        for (module, names, level) in imports {
            if let Some(base) = absolute_import(&name, is_package, module.as_deref(), level) {
                push_module_and_parents(&mut pending, &base);
                for item in names { if item != "*" { push_module_and_parents(&mut pending, &format!("{base}.{item}")); } }
            }
        }
        found.insert(name.clone(), SourceModule { name, source, tree, is_package, bundled });
    }
    Ok(found.into_values().collect())
}

fn init_symbol(name: &str) -> String {
    format!("piper_static_init_{}", name.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect::<String>())
}

fn lower_parsed(tree: &Mod, filename: &str, modname: &str, config: lower::ModuleConfig<'_>) -> Result<Codegen, CompileError> {
    let st = symtable::analyze(tree).map_err(|e| CompileError { msg: e.msg, lineno: e.span.lineno, col: e.span.col_offset })?;
    let mut cg = Codegen::new(modname);
    lower::lower_module_config(&mut cg, tree, &st, filename, modname, config).map_err(|e| CompileError { msg: e.msg, lineno: e.span.lineno, col: e.span.col_offset })?;
    Ok(cg)
}

fn object_cache_dir() -> Option<PathBuf> {
    if std::env::var_os("PIPER_NO_CACHE").is_some() { return None; }
    if let Some(path) = std::env::var_os("PIPER_CACHE_DIR") { return Some(PathBuf::from(path).join("objects")); }
    #[cfg(target_os = "macos")]
    if let Some(home) = std::env::var_os("HOME") { return Some(PathBuf::from(home).join("Library/Caches/piper/objects")); }
    #[cfg(target_os = "windows")]
    if let Some(path) = std::env::var_os("LOCALAPPDATA") { return Some(PathBuf::from(path).join("Piper/cache/objects")); }
    if let Some(path) = std::env::var_os("XDG_CACHE_HOME") { return Some(PathBuf::from(path).join("piper/objects")); }
    std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".cache/piper/objects"))
}

fn cache_hash(parts: &[&[u8]]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for part in parts {
        for byte in *part { hash = (hash ^ *byte as u64).wrapping_mul(0x100000001b3); }
        hash = (hash ^ 0xff).wrapping_mul(0x100000001b3);
    }
    hash
}

fn opt_name(opt: OptLevel) -> &'static str {
    match opt { OptLevel::O0 => "O0", OptLevel::O1 => "O1", OptLevel::O2 => "O2", OptLevel::O3 => "O3", OptLevel::Os => "Os", OptLevel::Oz => "Oz" }
}

fn module_object(module: &SourceModule, target: &Target, opt: OptLevel) -> Result<Vec<u8>, String> {
    let logical_file = if module.is_package { format!("{}/__init__.py", module.name.replace('.', "/")) } else { format!("{}.py", module.name.replace('.', "/")) };
    let symbol = init_symbol(&module.name);
    let key = cache_hash(&[env!("CARGO_PKG_VERSION").as_bytes(), target.triple.as_bytes(), opt_name(opt).as_bytes(), module.name.as_bytes(), module.source.as_bytes()]);
    let cache = object_cache_dir().map(|directory| directory.join(format!("{key:016x}.o")));
    if let Some(path) = &cache {
        if let Ok(object) = std::fs::read(path) { return Ok(object); }
    }
    let mut codegen = lower_parsed(&module.tree, &logical_file, &module.name, lower::ModuleConfig {
        init_symbol: &symbol, emit_main: false, emit_extension: false, is_package: module.is_package, static_modules: &[],
    }).map_err(|e| format!("  File \"{logical_file}\", line {}\nSyntaxError: {}", e.lineno, e.msg))?;
    let object = codegen.emit_object(Some(&target.triple), opt).map_err(|e| format!("codegen failed for {}: {e}", module.name))?;
    if let Some(path) = cache {
        if let Some(parent) = path.parent() {
            if std::fs::create_dir_all(parent).is_ok() {
                let temporary = parent.join(format!(".{key:016x}-{}.tmp", std::process::id()));
                if std::fs::write(&temporary, &object).is_ok() {
                    let _ = std::fs::rename(&temporary, &path);
                }
                let _ = std::fs::remove_file(temporary);
            }
        }
    }
    Ok(object)
}

/// Compile a source file to a native executable at `output`.
pub fn compile_file(input: &Path, output: &Path, opts: &CompileOptions) -> Result<(), String> {
    let source_path = if input.is_dir() { input.join("__init__.py") } else { input.to_path_buf() };
    let src = std::fs::read_to_string(&source_path).map_err(|e| format!("can't open source '{}': {e}", source_path.display()))?;
    // CPython reports __file__ and tracebacks with an absolute path.
    let filename = std::fs::canonicalize(&source_path).unwrap_or_else(|_| source_path.clone()).to_string_lossy().into_owned();
    let inferred = input.file_name().and_then(|s| s.to_str()).unwrap_or("module").trim_end_matches(".py").trim_start_matches("lib");
    let modname = opts.module_name.as_deref().unwrap_or(if opts.library { inferred } else { "__main__" });
    let tree = piper_syntax::parse_module(&src, &filename).map_err(|e| CompileError { msg: e.msg, lineno: e.lineno, col: e.col_offset }).map_err(|e| {
        let line = src.lines().nth(e.lineno.saturating_sub(1) as usize).unwrap_or("");
        format!("  File \"{}\", line {}\n    {}\n    {}^\nSyntaxError: {}", filename, e.lineno, line.trim_end(), " ".repeat(e.col as usize), e.msg)
    })?;
    let search_root = source_path.parent().unwrap_or_else(|| Path::new("."));
    let modules = discover_modules(search_root, &tree, modname, input.is_dir())?;
    let registry: Vec<(String, String)> = modules.iter().map(|module| (module.name.clone(), init_symbol(&module.name))).collect();
    let mut cg = lower_parsed(&tree, &filename, modname, lower::ModuleConfig {
        init_symbol: "piper_module_init", emit_main: !opts.library, emit_extension: opts.library, is_package: input.is_dir(), static_modules: &registry,
    }).map_err(|e| {
        let line = src.lines().nth(e.lineno.saturating_sub(1) as usize).unwrap_or("");
        format!("  File \"{}\", line {}\n    {}\n    {}^\nSyntaxError: {}", filename, e.lineno, line.trim_end(), " ".repeat(e.col as usize), e.msg)
    })?;
    if opts.emit_ir {
        cg.verify().map_err(|e| format!("internal error: invalid IR:\n{e}"))?;
        std::fs::write(output, cg.ir()).map_err(|e| e.to_string())?;
        return Ok(());
    }
    let target = match &opts.triple { Some(t) => Target::parse(t)?, None => Target::host() };
    let obj = cg.emit_object(Some(&target.triple), opts.opt).map_err(|e| format!("codegen failed: {e}"))?;
    if opts.emit_object {
        std::fs::write(output, obj).map_err(|e| e.to_string())?;
        return Ok(());
    }
    let rt = runtime_archive_for(&target, opts.runtime.as_deref())?;
    let dir = std::env::temp_dir().join(format!("piper-build-{}-{}", std::process::id(), target.dir_name()));
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let obj_path = dir.join("program.o");
    std::fs::write(&obj_path, obj).map_err(|e| e.to_string())?;
    let mut inputs = vec![obj_path];
    for module in &modules {
        let module_opt = if module.bundled { OptLevel::O0 } else { opts.opt };
        let module_obj = module_object(module, &target, module_opt)?;
        let path = dir.join(format!("module-{}.o", module.name.replace('.', "-")));
        std::fs::write(&path, module_obj).map_err(|e| e.to_string())?;
        inputs.push(path);
    }
    inputs.push(rt);
    let req = link::LinkRequest {
        target: target.clone(),
        output: output.to_path_buf(),
        inputs,
        static_libc: opts.static_libc,
        sysroot: opts.sysroot.clone(),
        libs: target.system_libs().iter().map(|s| s.to_string()).collect(),
        shared: opts.library,
    };
    let r = link::link(&req);
    let _ = std::fs::remove_dir_all(&dir);
    r.map_err(|e| format!("link failed:\n{e}"))
}
