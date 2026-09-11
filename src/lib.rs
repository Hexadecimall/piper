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
    tree: Mod,
    is_package: bool,
}

fn nested_imports(stmts: &[Stmt], out: &mut Vec<(Option<String>, Vec<String>, u32)>) {
    for stmt in stmts {
        match &stmt.kind {
            StmtKind::Import { names } => for alias in names { out.push((Some(alias.name.clone()), Vec::new(), 0)); },
            StmtKind::ImportFrom { module, names, level } => out.push((module.clone(), names.iter().map(|a| a.name.clone()).collect(), *level)),
            StmtKind::FunctionDef { body, .. } | StmtKind::AsyncFunctionDef { body, .. } | StmtKind::ClassDef { body, .. }
            | StmtKind::With { body, .. } | StmtKind::AsyncWith { body, .. } => nested_imports(body, out),
            StmtKind::If { test, body, orelse } if main_guard(test) => nested_imports(orelse, out),
            StmtKind::For { body, orelse, .. } | StmtKind::AsyncFor { body, orelse, .. }
            | StmtKind::While { body, orelse, .. } | StmtKind::If { body, orelse, .. } => {
                nested_imports(body, out); nested_imports(orelse, out);
            }
            StmtKind::Try { body, handlers, orelse, finalbody } | StmtKind::TryStar { body, handlers, orelse, finalbody } => {
                nested_imports(body, out);
                for handler in handlers { nested_imports(&handler.body, out); }
                nested_imports(orelse, out); nested_imports(finalbody, out);
            }
            StmtKind::Match { cases, .. } => for case in cases { nested_imports(&case.body, out); },
            _ => {}
        }
    }
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
    if let Mod::Module { body, .. } = entry_tree { nested_imports(body, &mut imports); }
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
        let (source, is_package) = if let Some((path, is_package)) = module_source(root, &name) {
            (std::fs::read_to_string(&path).map_err(|e| format!("can't open module '{name}': {e}"))?, is_package)
        } else if let Some((source, is_package)) = piper_bundle::stdlib_source(&name) {
            (source.to_string(), is_package)
        } else { continue };
        let logical_file = if is_package { format!("{}/__init__.py", name.replace('.', "/")) } else { format!("{}.py", name.replace('.', "/")) };
        let tree = piper_syntax::parse_module(&source, &logical_file).map_err(|e| format!("  File \"{logical_file}\", line {}\nSyntaxError: {}", e.lineno, e.msg))?;
        let mut imports = Vec::new();
        if let Mod::Module { body, .. } = &tree { nested_imports(body, &mut imports); }
        for (module, names, level) in imports {
            if let Some(base) = absolute_import(&name, is_package, module.as_deref(), level) {
                push_module_and_parents(&mut pending, &base);
                for item in names { if item != "*" { push_module_and_parents(&mut pending, &format!("{base}.{item}")); } }
            }
        }
        found.insert(name.clone(), SourceModule { name, tree, is_package });
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
        let logical_file = if module.is_package { format!("{}/__init__.py", module.name.replace('.', "/")) } else { format!("{}.py", module.name.replace('.', "/")) };
        let symbol = init_symbol(&module.name);
        let mut module_cg = lower_parsed(&module.tree, &logical_file, &module.name, lower::ModuleConfig {
            init_symbol: &symbol, emit_main: false, emit_extension: false, is_package: module.is_package, static_modules: &[],
        }).map_err(|e| format!("  File \"{logical_file}\", line {}\nSyntaxError: {}", e.lineno, e.msg))?;
        let module_obj = module_cg.emit_object(Some(&target.triple), opts.opt).map_err(|e| format!("codegen failed for {}: {e}", module.name))?;
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
