//! AST to LLVM IR. Every Python value is a `ptr` (PyObject*) and every
//! operation is a call into the runtime; control flow is real LLVM control
//! flow. Errors are signalled by null returns and routed to the innermost
//! handler block.
//!
//! Reference discipline: an expression yields an owned reference. Statements
//! consume what they evaluate. Locals own one reference each. Temporaries
//! are released on the normal path only; an exception unwinding through a
//! partially evaluated expression leaks them (a known v1 limitation).

use crate::codegen::Codegen;
use crate::sys::*;
use piper_syntax::ast::*;
use piper_syntax::symtable::{Resolution, Scope, ScopeKind, SymbolTable};
use std::collections::HashMap;
use std::ffi::{CString, c_uint};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LowerError {
    pub msg: String,
    pub span: Span,
}
type LResult<T> = Result<T, LowerError>;

fn err<T>(msg: impl Into<String>, span: Span) -> LResult<T> { Err(LowerError { msg: msg.into(), span }) }

/// Configuration for an executable entry module or a statically linked import.
pub struct ModuleConfig<'a> {
    pub init_symbol: &'a str,
    pub emit_main: bool,
    pub emit_extension: bool,
    pub is_package: bool,
    pub static_modules: &'a [(String, String)],
    pub specialize_core: bool,
}

/// Lower a parsed module into `cg`. Emits `piper_module_init` and `main`.
pub fn lower_module(cg: &mut Codegen, m: &Mod, st: &SymbolTable, filename: &str, modname: &str) -> LResult<()> {
    lower_module_config(cg, m, st, filename, modname, ModuleConfig { init_symbol: "piper_module_init", emit_main: true, emit_extension: false, is_package: false, static_modules: &[], specialize_core: false })
}

pub fn lower_module_config(cg: &mut Codegen, m: &Mod, st: &SymbolTable, filename: &str, modname: &str, config: ModuleConfig<'_>) -> LResult<()> {
    let Mod::Module { body, .. } = m else { return err("only modules can be compiled", Span::default()) };
    let mut l = Lower::new(cg, st, filename, modname, config);
    l.emit_module(body)?;
    Ok(())
}

/// A pending constant: created once at module init into a global slot.
enum ConstInit { Str(String), Int(String), Float(f64), Complex(f64, f64), Bytes(Vec<u8>), Bool(bool), None, Ellipsis, Tuple(Vec<ValueRef>), Intern(String) }

struct Loop { break_bb: BasicBlockRef, continue_bb: BasicBlockRef, depth: usize }

/// What to do when an error occurs: jump to a handler block.
#[derive(Clone, Copy)]
enum Handler {
    /// Jump here; the block will call PyErr_GetRaisedException itself.
    Block(BasicBlockRef),
    /// Function-level exit (decref locals, pop frame, return null).
    FunctionExit,
}

/// Per-function lowering state.
struct Func {
    llfn: ValueRef,
    scope: usize,
    /// name -> alloca holding the local's PyObject* (null when unbound)
    locals: HashMap<String, ValueRef>,
    /// name -> alloca holding the cell object
    cells: HashMap<String, ValueRef>,
    /// The `func` argument (PiperFunctionObject*); only valid when `has_funcobj`.
    funcobj: ValueRef,
    has_funcobj: bool,
    /// Namespace dict for class bodies; only valid when `has_ns`.
    ns: ValueRef,
    has_ns: bool,
    /// Slot holding the current line number.
    line_slot: ValueRef,
    /// Extra allocas to release at function exit (iterators, with-exits).
    temp_slots: Vec<ValueRef>,
    handlers: Vec<Handler>,
    loops: Vec<Loop>,
    /// Finally/with bodies active around the current point, innermost last.
    cleanups: Vec<Cleanup>,
    error_exit: BasicBlockRef,
    return_slot: ValueRef,
    return_bb: BasicBlockRef,
    /// Number of child scopes consumed so far (to pair AST nodes with scopes).
    next_child: usize,
    #[allow(dead_code)]
    funcname: String,
    super_pushed: bool,
    /// Handler stack height when each cleanup was pushed.
    cleanup_handler_heights: Vec<usize>,
    /// Generator resume functions keep pointer slots in the generator frame.
    generator: Option<GeneratorState>,
}

#[derive(Clone, Copy)]
struct GeneratorState {
    object: ValueRef,
    sent: ValueRef,
    dispatch: ValueRef,
    next_state: u64,
    next_slot: i64,
}

#[derive(Clone)]
enum Cleanup {
    Finally(Vec<Stmt>),
    With { exit_slot: ValueRef },
    AsyncWith { exit_slot: ValueRef },
    Except,
}

pub struct Lower<'a> {
    cg: &'a mut Codegen,
    st: &'a SymbolTable,
    filename: String,
    modname: String,
    ptr_t: TypeRef,
    i32_t: TypeRef,
    i64_t: TypeRef,
    i8_t: TypeRef,
    void_t: TypeRef,
    f64_t: TypeRef,
    fns: HashMap<&'static str, (ValueRef, TypeRef)>,
    consts: Vec<(ValueRef, ConstInit)>,
    const_cache: HashMap<String, ValueRef>,
    globals_var: ValueRef,
    builtins_var: ValueRef,
    fstack: Vec<Func>,
    fn_counter: usize,
    file_str: ValueRef,
    init_symbol: String,
    emit_main: bool,
    emit_extension: bool,
    is_package: bool,
    static_modules: Vec<(String, String)>,
    specialize_core: bool,
}

/// Runtime function signatures: (return, params). `P` = ptr, `I` = i32, `L` = i64, `V` = void, `D` = double.
const RT: &[(&str, &str, &str)] = &[
    ("piper_initialize", "V", ""), ("piper_main", "I", "IPP"), ("piper_main_core", "I", "IPP"), ("piper_builtins", "P", ""), ("PyImport_AddModule", "P", "P"),
    ("piper_builtin_print", "P", "PLP"), ("piper_builtin_input", "P", "PL"),
    ("PyModule_GetDict", "P", "P"), ("PyDict_SetItemString", "I", "PPP"), ("piper_str_const", "P", "PL"), ("piper_int_const", "P", "P"),
    ("piper_float_const", "P", "D"), ("piper_complex_const", "P", "DD"), ("piper_bytes_const", "P", "PL"), ("piper_bool", "P", "I"),
    ("piper_none", "P", ""), ("piper_ellipsis", "P", ""), ("piper_tuple_const", "P", "PL"), ("piper_intern", "P", "P"),
    ("Py_IncRef", "V", "P"), ("Py_DecRef", "V", "P"), ("piper_load_global", "P", "PPP"), ("piper_load_name", "P", "PPPP"),
    ("piper_store_global", "I", "PPP"), ("piper_delete_global", "I", "PP"), ("piper_unbound_local", "P", "P"),
    ("piper_binop", "P", "IPP"), ("piper_inplace_binop", "P", "IPP"), ("PyNumber_Negative", "P", "P"), ("PyNumber_Positive", "P", "P"),
    ("PyNumber_Invert", "P", "P"), ("PyObject_IsTrue", "I", "P"), ("piper_compare", "P", "IPP"), ("piper_is", "P", "PPI"), ("piper_contains", "P", "PPI"),
    ("piper_call", "P", "PPLP"), ("piper_call_ex", "P", "PPP"), ("piper_load_method", "P", "PPP"), ("PyObject_GetAttr", "P", "PP"),
    ("PyObject_SetAttr", "I", "PPP"), ("PyObject_GetItem", "P", "PP"), ("PyObject_SetItem", "I", "PPP"), ("PyObject_DelItem", "I", "PP"),
    ("piper_build_slice", "P", "PPP"), ("PyList_New", "P", "L"), ("PyList_Append", "I", "PP"), ("piper_list_extend", "P", "PP"),
    ("piper_list_to_tuple", "P", "P"), ("PyTuple_New", "P", "L"), ("PyTuple_SetItem", "I", "PLP"), ("PyDict_New", "P", ""), ("PyDict_SetItem", "I", "PPP"),
    ("piper_dict_update", "I", "PP"), ("piper_dict_merge_call", "I", "PPP"), ("PySet_New", "P", "P"), ("piper_set_add", "I", "PP"), ("piper_set_update", "I", "PP"),
    ("PyObject_GetIter", "P", "P"), ("PyIter_Next", "P", "P"), ("PyErr_Occurred", "P", ""), ("piper_unpack_sequence", "I", "PLP"), ("piper_unpack_ex", "I", "PLLP"),
    ("piper_format_value", "P", "PIP"), ("piper_build_string", "P", "PL"), ("piper_function_new", "P", "PPPPPIIIIPPP"), ("piper_cell_new", "P", "P"),
    ("piper_cell_get", "P", "PPI"), ("piper_cell_set", "V", "PP"), ("piper_closure_cell", "P", "PL"), ("piper_bind_args", "I", "PPLPP"),
    ("piper_frame_push", "V", "PPP"), ("piper_frame_pop", "V", ""), ("piper_frame_set_line", "V", "I"), ("piper_traceback_add", "V", "PPI"),
    ("PyErr_GetRaisedException", "P", ""), ("PyErr_SetRaisedException", "V", "P"), ("piper_exception_matches", "I", "PP"), ("piper_exc_info_push", "V", "P"),
    ("piper_exc_info_pop", "P", ""), ("piper_raise", "P", "PP"), ("piper_reraise", "P", ""), ("piper_assert_failed", "I", "P"),
    ("piper_with_enter", "I", "PPP"), ("piper_with_exit", "I", "PP"), ("piper_make_class", "P", "PPPPP"), ("piper_import_name", "P", "PPPI"),
    ("piper_import_from", "P", "PP"), ("piper_import_star", "I", "PP"), ("piper_super_push", "V", "PP"), ("piper_super_pop", "V", ""),
    ("piper_function_globals", "P", "P"), ("piper_function_builtins", "P", "P"), ("piper_setup_annotations", "I", "P"),
    ("piper_match_class", "P", "PPLP"), ("piper_match_keys", "P", "PP"), ("piper_match_sequence_check", "I", "P"), ("piper_match_mapping_check", "I", "P"),
    ("PyObject_Length", "L", "P"), ("PyObject_RichCompareBool", "I", "PPI"), ("PyList_GetItem", "P", "PL"), ("PyList_GetSlice", "P", "PLL"), ("PyList_Size", "L", "P"),
    ("PyTuple_GetItem", "P", "PL"),
    ("PySequence_List", "P", "P"), ("PyDict_DelItem", "I", "PP"), ("PyDict_Copy", "P", "P"), ("PyErr_Clear", "V", ""),
    ("piper_interpolation_new", "P", "PPIP"), ("piper_template_new", "P", "PL"),
    ("piper_generator_new", "P", "PPLI"), ("piper_generator_function", "P", "P"), ("piper_generator_slot", "P", "PL"),
    ("piper_generator_state", "I", "P"), ("piper_generator_set_state", "V", "PI"), ("piper_generator_finish", "P", "PP"),
    ("piper_generator_abort", "V", "P"),
    ("piper_yield_from_next", "P", "PPI"),
    ("piper_await_iter", "P", "P"),
    ("piper_async_iter", "P", "P"), ("piper_async_next", "P", "P"), ("piper_async_iteration_done", "I", ""),
    ("piper_async_with_enter", "P", "PP"), ("piper_async_with_exit", "P", "PP"),
    ("piper_exception_group_match", "I", "PPPP"),
    ("piper_exception_group_merge", "P", "PP"),
    ("piper_type_alias_new", "P", "PPP"),
    ("piper_type_param_new", "P", "PIPP"),
    ("piper_set_static_importer", "V", "P"),
    ("piper_static_import_module", "P", "PP"),
    ("PyUnicode_EqualToUTF8", "I", "PP"),
];

impl<'a> Lower<'a> {
    fn new(cg: &'a mut Codegen, st: &'a SymbolTable, filename: &str, modname: &str, config: ModuleConfig<'_>) -> Self {
        unsafe {
            let ctx = cg.ctx;
            let ptr_t = LLVMPointerTypeInContext(ctx, 0);
            let module = cg.module;
            let globals_var = LLVMAddGlobal(module, ptr_t, c"piper.globals".as_ptr());
            LLVMSetInitializer(globals_var, LLVMConstNull(ptr_t));
            LLVMSetLinkage(globals_var, LINKAGE_INTERNAL);
            let builtins_var = LLVMAddGlobal(module, ptr_t, c"piper.builtins".as_ptr());
            LLVMSetInitializer(builtins_var, LLVMConstNull(ptr_t));
            LLVMSetLinkage(builtins_var, LINKAGE_INTERNAL);
            let mut l = Lower {
                cg, st, filename: filename.into(), modname: modname.into(), ptr_t,
                i32_t: LLVMInt32TypeInContext(ctx), i64_t: LLVMInt64TypeInContext(ctx), i8_t: LLVMInt8TypeInContext(ctx),
                void_t: LLVMVoidTypeInContext(ctx), f64_t: LLVMDoubleTypeInContext(ctx),
                fns: HashMap::new(), consts: Vec::new(), const_cache: HashMap::new(), globals_var, builtins_var, fstack: Vec::new(), fn_counter: 0,
                file_str: std::ptr::null_mut(), init_symbol: config.init_symbol.into(), emit_main: config.emit_main, emit_extension: config.emit_extension,
                is_package: config.is_package, static_modules: config.static_modules.to_vec(), specialize_core: config.specialize_core,
            };
            l.file_str = l.cstring_global(filename);
            l
        }
    }

    // ---- LLVM helpers ------------------------------------------------------------

    fn b(&self) -> BuilderRef { self.cg.builder }
    fn f(&self) -> &Func { self.fstack.last().unwrap() }
    fn fm(&mut self) -> &mut Func { self.fstack.last_mut().unwrap() }
    fn scope(&self) -> &Scope { &self.st.scopes[self.f().scope] }

    fn ty(&self, c: u8) -> TypeRef {
        match c { b'P' => self.ptr_t, b'I' => self.i32_t, b'L' => self.i64_t, b'V' => self.void_t, b'D' => self.f64_t, _ => self.ptr_t }
    }

    fn rt(&mut self, name: &'static str) -> (ValueRef, TypeRef) {
        if let Some(v) = self.fns.get(name) { return *v; }
        let (ret, params) = RT.iter().find(|(n, _, _)| *n == name).map(|(_, r, p)| (*r, *p)).unwrap_or_else(|| panic!("unknown runtime function {name}"));
        unsafe {
            let mut ps: Vec<TypeRef> = params.bytes().map(|c| self.ty(c)).collect();
            let fty = LLVMFunctionType(self.ty(ret.as_bytes()[0]), ps.as_mut_ptr(), ps.len() as c_uint, 0);
            let cname = CString::new(name).unwrap();
            let f = LLVMAddFunction(self.cg.module, cname.as_ptr(), fty);
            self.fns.insert(name, (f, fty));
            (f, fty)
        }
    }

    fn call(&mut self, name: &'static str, args: &[ValueRef]) -> ValueRef {
        let (f, fty) = self.rt(name);
        let mut a: Vec<ValueRef> = args.to_vec();
        let returns_void = RT.iter().find(|(n, _, _)| *n == name).map(|(_, r, _)| *r == "V").unwrap_or(false);
        unsafe { LLVMBuildCall2(self.b(), fty, f, a.as_mut_ptr(), a.len() as c_uint, if returns_void { c"".as_ptr() } else { c"r".as_ptr() }) }
    }

    fn i32c(&self, v: i64) -> ValueRef { unsafe { LLVMConstInt(self.i32_t, v as u64, 1) } }
    fn i64c(&self, v: i64) -> ValueRef { unsafe { LLVMConstInt(self.i64_t, v as u64, 1) } }
    fn null(&self) -> ValueRef { unsafe { LLVMConstNull(self.ptr_t) } }
    /// NUL-terminated string as a private global constant (position independent).
    fn cstring_global(&mut self, s: &str) -> ValueRef {
        if let Some(v) = self.const_cache.get(&format!("cstr:{s}")) { return *v; }
        unsafe {
            let bytes = s.as_bytes();
            let arr_t = LLVMArrayType2(self.i8_t, bytes.len() as u64 + 1);
            let g = LLVMAddGlobal(self.cg.module, arr_t, c".str".as_ptr());
            let init = LLVMConstStringInContext2(self.cg.ctx, bytes.as_ptr() as *const _, bytes.len(), 0);
            LLVMSetInitializer(g, init);
            LLVMSetGlobalConstant(g, 1);
            LLVMSetLinkage(g, LINKAGE_PRIVATE);
            self.const_cache.insert(format!("cstr:{s}"), g);
            g
        }
    }
    fn bb(&mut self, name: &str) -> BasicBlockRef {
        let c = CString::new(name).unwrap();
        unsafe { LLVMAppendBasicBlockInContext(self.cg.ctx, self.f().llfn, c.as_ptr()) }
    }
    fn position(&mut self, bb: BasicBlockRef) { unsafe { LLVMPositionBuilderAtEnd(self.b(), bb) } }
    fn current_bb(&self) -> BasicBlockRef { unsafe { LLVMGetInsertBlock(self.b()) } }
    fn terminated(&self) -> bool { unsafe { !LLVMGetBasicBlockTerminator(self.current_bb()).is_null() } }
    fn br(&mut self, bb: BasicBlockRef) { if !self.terminated() { unsafe { LLVMBuildBr(self.b(), bb); } } }
    fn cond_br(&mut self, c: ValueRef, t: BasicBlockRef, e: BasicBlockRef) { unsafe { LLVMBuildCondBr(self.b(), c, t, e); } }
    fn alloca(&mut self, name: &str) -> ValueRef {
        if let Some(mut generator) = self.f().generator {
            let index = generator.next_slot;
            generator.next_slot += 1;
            self.fm().generator = Some(generator);
            unsafe {
                let cur = self.current_bb();
                let entry = LLVMGetEntryBasicBlock(self.f().llfn);
                let term = LLVMGetBasicBlockTerminator(entry);
                if term.is_null() { LLVMPositionBuilderAtEnd(self.b(), entry); } else { LLVMPositionBuilderBefore(self.b(), term); }
                let object = generator.object;
                let index = self.i64c(index);
                let slot = self.call("piper_generator_slot", &[object, index]);
                if cur == entry { LLVMPositionBuilderBefore(self.b(), term); } else { LLVMPositionBuilderAtEnd(self.b(), cur); }
                return slot;
            }
        }
        // All allocas go in the entry block so they dominate everything.
        unsafe {
            let cur = self.current_bb();
            let entry = LLVMGetEntryBasicBlock(self.f().llfn);
            let first = LLVMGetFirstInstruction(entry);
            if first.is_null() { LLVMPositionBuilderAtEnd(self.b(), entry); } else { LLVMPositionBuilderBefore(self.b(), first); }
            let c = CString::new(name).unwrap();
            let a = LLVMBuildAlloca(self.b(), self.ptr_t, c.as_ptr());
            LLVMBuildStore(self.b(), LLVMConstNull(self.ptr_t), a);
            LLVMPositionBuilderAtEnd(self.b(), cur);
            a
        }
    }
    fn load(&mut self, slot: ValueRef) -> ValueRef { unsafe { LLVMBuildLoad2(self.b(), self.ptr_t, slot, c"v".as_ptr()) } }
    fn take(&mut self, slot: ValueRef) -> ValueRef {
        let value = self.load(slot);
        self.store(slot, self.null());
        value
    }

    fn generator_slot_at(&mut self, index: i64) -> ValueRef {
        let generator = self.f().generator.expect("generator slot outside generator");
        unsafe {
            let cur = self.current_bb();
            let entry = LLVMGetEntryBasicBlock(self.f().llfn);
            let term = LLVMGetBasicBlockTerminator(entry);
            LLVMPositionBuilderBefore(self.b(), term);
            let index = self.i64c(index);
            let slot = self.call("piper_generator_slot", &[generator.object, index]);
            if cur == entry { LLVMPositionBuilderBefore(self.b(), term); } else { LLVMPositionBuilderAtEnd(self.b(), cur); }
            slot
        }
    }
    fn store(&mut self, slot: ValueRef, v: ValueRef) { unsafe { LLVMBuildStore(self.b(), v, slot); } }
    fn incref(&mut self, v: ValueRef) { self.call("Py_IncRef", &[v]); }
    fn decref(&mut self, v: ValueRef) { self.call("Py_DecRef", &[v]); }
    fn is_null(&mut self, v: ValueRef) -> ValueRef { unsafe { LLVMBuildIsNull(self.b(), v, c"isnull".as_ptr()) } }
    fn is_neg(&mut self, v: ValueRef) -> ValueRef { unsafe { LLVMBuildICmp(self.b(), INT_SLT, v, self.i32c(0), c"isneg".as_ptr()) } }

    fn error_target(&self) -> BasicBlockRef {
        match self.f().handlers.last().copied().unwrap_or(Handler::FunctionExit) {
            Handler::Block(bb) => bb,
            Handler::FunctionExit => self.f().error_exit,
        }
    }
    /// Branch to the error target if `v` is null; continue otherwise.
    fn check_null(&mut self, v: ValueRef) {
        let ok = self.bb("ok");
        let bad = self.error_target();
        let c = self.is_null(v);
        self.cond_br(c, bad, ok);
        self.position(ok);
    }
    fn check_neg(&mut self, v: ValueRef) {
        let ok = self.bb("ok");
        let bad = self.error_target();
        let c = self.is_neg(v);
        self.cond_br(c, bad, ok);
        self.position(ok);
    }
    /// An array of ptr on the stack holding `vals`; returns the base pointer.
    fn ptr_array(&mut self, vals: &[ValueRef]) -> ValueRef {
        unsafe {
            let n = vals.len().max(1);
            let arr_t = LLVMArrayType2(self.ptr_t, n as u64);
            let cur = self.current_bb();
            let entry = LLVMGetEntryBasicBlock(self.f().llfn);
            let first = LLVMGetFirstInstruction(entry);
            if first.is_null() { LLVMPositionBuilderAtEnd(self.b(), entry); } else { LLVMPositionBuilderBefore(self.b(), first); }
            let arr = LLVMBuildAlloca(self.b(), arr_t, c"args".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), cur);
            for (i, v) in vals.iter().enumerate() {
                let mut idx = [self.i64c(0), self.i64c(i as i64)];
                let p = LLVMBuildGEP2(self.b(), arr_t, arr, idx.as_mut_ptr(), 2, c"slot".as_ptr());
                LLVMBuildStore(self.b(), *v, p);
            }
            arr
        }
    }
    fn set_line(&mut self, span: Span) {
        let l = self.i32c(span.lineno as i64);
        let slot = self.f().line_slot;
        unsafe { LLVMBuildStore(self.b(), l, slot); }
        self.call("piper_frame_set_line", &[l]);
    }

    // ---- constants -----------------------------------------------------------------

    fn const_slot(&mut self, key: String, init: ConstInit) -> ValueRef {
        if let Some(v) = self.const_cache.get(&key) { return *v; }
        unsafe {
            let name = CString::new(format!("piper.const.{}", self.consts.len())).unwrap();
            let g = LLVMAddGlobal(self.cg.module, self.ptr_t, name.as_ptr());
            LLVMSetInitializer(g, LLVMConstNull(self.ptr_t));
            LLVMSetLinkage(g, LINKAGE_INTERNAL);
            self.consts.push((g, init));
            self.const_cache.insert(key, g);
            g
        }
    }
    /// Owned reference to a constant.
    fn constant(&mut self, c: &Constant) -> ValueRef {
        let (key, init) = match c {
            Constant::None => ("None".to_string(), ConstInit::None),
            Constant::Bool(b) => (format!("bool:{b}"), ConstInit::Bool(*b)),
            Constant::Int(s) => (format!("int:{s}"), ConstInit::Int(s.clone())),
            Constant::Float(f) => (format!("float:{:?}", f.to_bits()), ConstInit::Float(*f)),
            Constant::Complex(im) => (format!("complex:{:?}", im.to_bits()), ConstInit::Complex(0.0, *im)),
            Constant::Str(s) => (format!("str:{s}"), ConstInit::Str(s.clone())),
            Constant::Bytes(b) => (format!("bytes:{b:?}"), ConstInit::Bytes(b.clone())),
            Constant::Ellipsis => ("Ellipsis".to_string(), ConstInit::Ellipsis),
        };
        let g = self.const_slot(key, init);
        let v = self.load(g);
        self.incref(v);
        v
    }
    /// Borrowed interned string for names.
    fn name_const(&mut self, s: &str) -> ValueRef {
        let g = self.const_slot(format!("intern:{s}"), ConstInit::Intern(s.to_string()));
        self.load(g)
    }
    /// Borrowed constant tuple of interned strings.
    fn names_tuple(&mut self, names: &[String]) -> ValueRef {
        let key = format!("names:{}", names.join("\0"));
        if let Some(v) = self.const_cache.get(&key) { let g = *v; return self.load(g); }
        let items: Vec<ValueRef> = names.iter().map(|n| self.const_slot(format!("intern:{n}"), ConstInit::Intern(n.clone()))).collect();
        let g = self.const_slot(key, ConstInit::Tuple(items));
        self.load(g)
    }

    fn emit_const_inits(&mut self) {
        // Runs at the start of module init.
        let consts = std::mem::take(&mut self.consts);
        for (g, init) in &consts {
            let v = match init {
                ConstInit::Str(s) => { let p = self.cstring_global(s); let n = self.i64c(s.len() as i64); self.call("piper_str_const", &[p, n]) }
                ConstInit::Intern(s) => { let p = self.cstring_global(s); self.call("piper_intern", &[p]) }
                ConstInit::Int(s) => { let p = self.cstring_global(s); self.call("piper_int_const", &[p]) }
                ConstInit::Float(f) => { let d = unsafe { LLVMConstReal(self.f64_t, *f) }; self.call("piper_float_const", &[d]) }
                ConstInit::Complex(re, im) => { let a = unsafe { LLVMConstReal(self.f64_t, *re) }; let b = unsafe { LLVMConstReal(self.f64_t, *im) }; self.call("piper_complex_const", &[a, b]) }
                ConstInit::Bytes(b) => {
                    let s: String = b.iter().map(|&c| c as char).collect();
                    // bytes may contain NUL: emit as a constant array
                    let arr = unsafe { LLVMConstStringInContext2(self.cg.ctx, s.as_bytes().as_ptr() as *const _, 0, 1) };
                    let _ = arr;
                    let data = self.bytes_global(b);
                    let n = self.i64c(b.len() as i64);
                    self.call("piper_bytes_const", &[data, n])
                }
                ConstInit::Bool(b) => { let i = self.i32c(*b as i64); self.call("piper_bool", &[i]) }
                ConstInit::None => self.call("piper_none", &[]),
                ConstInit::Ellipsis => self.call("piper_ellipsis", &[]),
                ConstInit::Tuple(items) => {
                    let vals: Vec<ValueRef> = items.iter().map(|g| self.load(*g)).collect();
                    let arr = self.ptr_array(&vals);
                    let n = self.i64c(vals.len() as i64);
                    self.call("piper_tuple_const", &[arr, n])
                }
            };
            self.store(*g, v);
        }
        self.consts = consts;
    }

    fn bytes_global(&mut self, b: &[u8]) -> ValueRef {
        unsafe {
            let arr_t = LLVMArrayType2(self.i8_t, b.len().max(1) as u64);
            let g = LLVMAddGlobal(self.cg.module, arr_t, c"piper.bytes".as_ptr());
            let empty = [0u8];
            let bytes = if b.is_empty() { &empty[..] } else { b };
            let init = LLVMConstStringInContext2(self.cg.ctx, bytes.as_ptr() as *const _, bytes.len(), 1);
            LLVMSetInitializer(g, init);
            LLVMSetGlobalConstant(g, 1);
            LLVMSetLinkage(g, LINKAGE_PRIVATE);
            g
        }
    }

    // ---- module ----------------------------------------------------------------------

    fn emit_static_importer(&mut self) -> Option<ValueRef> {
        if self.static_modules.is_empty() { return None; }
        unsafe {
            let mut params = [self.ptr_t];
            let ty = LLVMFunctionType(self.ptr_t, params.as_mut_ptr(), 1, 0);
            let importer = LLVMAddFunction(self.cg.module, c"piper.static_import".as_ptr(), ty);
            LLVMSetLinkage(importer, LINKAGE_INTERNAL);
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, importer, c"entry".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let requested = LLVMGetParam(importer, 0);
            let modules = self.static_modules.clone();
            let mut declarations = HashMap::new();
            for (index, (name, _)) in modules.iter().enumerate() {
                let matched = LLVMAppendBasicBlockInContext(self.cg.ctx, importer, c"matched".as_ptr());
                let next_name = CString::new(format!("next.{index}")).unwrap();
                let next = LLVMAppendBasicBlockInContext(self.cg.ctx, importer, next_name.as_ptr());
                let name_ptr = self.cstring_global(name);
                let equal = self.call("PyUnicode_EqualToUTF8", &[requested, name_ptr]);
                let is_equal = LLVMBuildICmp(self.b(), INT_NE, equal, self.i32c(0), c"equal".as_ptr());
                LLVMBuildCondBr(self.b(), is_equal, matched, next);

                LLVMPositionBuilderAtEnd(self.b(), matched);
                let mut result = self.null();
                let parts: Vec<&str> = name.split('.').collect();
                for end in 1..=parts.len() {
                    let prefix = parts[..end].join(".");
                    let Some((_, prefix_symbol)) = modules.iter().find(|(module, _)| module == &prefix) else { continue };
                    let init_ty = LLVMFunctionType(self.ptr_t, std::ptr::null_mut(), 0, 0);
                    let init = *declarations.entry(prefix_symbol.clone()).or_insert_with(|| {
                        let symbol = CString::new(prefix_symbol.as_str()).unwrap();
                        LLVMAddFunction(self.cg.module, symbol.as_ptr(), init_ty)
                    });
                    let fullname = self.cstring_global(&prefix);
                    let loaded = self.call("piper_static_import_module", &[init, fullname]);
                    if end == parts.len() { result = loaded; } else { self.decref(loaded); }
                }
                LLVMBuildRet(self.b(), result);
                LLVMPositionBuilderAtEnd(self.b(), next);
            }
            LLVMBuildRet(self.b(), self.null());
            Some(importer)
        }
    }

    fn emit_module(&mut self, body: &[Stmt]) -> LResult<()> {
        unsafe {
            let static_importer = self.emit_static_importer();
            let init_ty = LLVMFunctionType(self.ptr_t, std::ptr::null_mut(), 0, 0);
            let init_symbol = CString::new(self.init_symbol.clone()).unwrap();
            let init = LLVMAddFunction(self.cg.module, init_symbol.as_ptr(), init_ty);
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, init, c"entry".as_ptr());
            let consts_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, init, c"consts".as_ptr());
            let body_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, init, c"body".as_ptr());
            let error_exit = LLVMAppendBasicBlockInContext(self.cg.ctx, init, c"error".as_ptr());
            let return_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, init, c"ret".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let line_slot = LLVMBuildAlloca(self.b(), self.i32_t, c"line".as_ptr());
            LLVMBuildStore(self.b(), self.i32c(0), line_slot);
            let return_slot = LLVMBuildAlloca(self.b(), self.ptr_t, c"retval".as_ptr());
            LLVMBuildStore(self.b(), self.null(), return_slot);
            self.fstack.push(Func {
                llfn: init, scope: 0, locals: HashMap::new(), cells: HashMap::new(), funcobj: self.null(), has_funcobj: false, ns: self.null(), has_ns: false, line_slot,
                temp_slots: Vec::new(), handlers: Vec::new(), loops: Vec::new(), cleanups: Vec::new(), error_exit, return_slot, return_bb,
                next_child: 0, funcname: "<module>".into(), super_pushed: false, cleanup_handler_heights: Vec::new(), generator: None,
            });
            // module setup
            if let Some(importer) = static_importer {
                self.call("piper_set_static_importer", &[importer]);
            }
            let modname = self.cstring_global(&self.modname.clone());
            let module = self.call("PyImport_AddModule", &[modname]);
            let g = self.call("PyModule_GetDict", &[module]);
            self.store(self.globals_var, g);
            let bi = self.call("piper_builtins", &[]);
            self.store(self.builtins_var, bi);
            let fname = self.file_str;
            let flen = self.i64c(self.filename.len() as i64);
            let fobj = self.call("piper_str_const", &[fname, flen]);
            let key = self.cstring_global("__file__");
            self.call("PyDict_SetItemString", &[g, key, fobj]);
            self.decref(fobj);
            let bkey = self.cstring_global("__builtins__");
            let bmod = self.cstring_global("builtins");
            let bmodobj = self.call("PyImport_AddModule", &[bmod]);
            self.call("PyDict_SetItemString", &[g, bkey, bmodobj]);
            let package = if self.is_package { self.modname.clone() } else { self.modname.rsplit_once('.').map(|(p, _)| p).unwrap_or("").to_string() };
            let package_key = self.cstring_global("__package__");
            let package_ptr = self.cstring_global(&package);
            let package_obj = self.call("piper_str_const", &[package_ptr, self.i64c(package.len() as i64)]);
            self.call("PyDict_SetItemString", &[g, package_key, package_obj]);
            self.decref(package_obj);
            if self.is_package {
                let path_key = self.cstring_global("__path__");
                let path = self.call("PyList_New", &[self.i64c(0)]);
                self.call("PyDict_SetItemString", &[g, path_key, path]);
                self.decref(path);
            }
            LLVMBuildBr(self.b(), consts_bb);
            LLVMPositionBuilderAtEnd(self.b(), body_bb);
            let fs = self.file_str;
            let mn = self.cstring_global("<module>");
            self.call("piper_frame_push", &[fs, mn, g]);
            self.stmts(body)?;
            if !self.terminated() {
                let none = if self.emit_main { let value = self.call("piper_none", &[]); self.incref(value); value }
                    else { self.incref(module); module };
                let rs = self.f().return_slot;
                self.store(rs, none);
                LLVMBuildBr(self.b(), return_bb);
            }
            // constants must be emitted after the body so all are known
            LLVMPositionBuilderAtEnd(self.b(), consts_bb);
            self.emit_const_inits();
            LLVMBuildBr(self.b(), body_bb);
            // error exit
            LLVMPositionBuilderAtEnd(self.b(), error_exit);
            let line = LLVMBuildLoad2(self.b(), self.i32_t, line_slot, c"line".as_ptr());
            let fs = self.file_str;
            let mn = self.cstring_global("<module>");
            self.call("piper_traceback_add", &[fs, mn, line]);
            self.call("piper_frame_pop", &[]);
            LLVMBuildRet(self.b(), self.null());
            // return
            LLVMPositionBuilderAtEnd(self.b(), return_bb);
            self.call("piper_frame_pop", &[]);
            let rv = LLVMBuildLoad2(self.b(), self.ptr_t, return_slot, c"rv".as_ptr());
            LLVMBuildRet(self.b(), rv);
            self.fstack.pop();

            if self.emit_extension {
                // Native library entry point. A library owns its piper runtime,
                // initializes it on first import, executes this module once and
                // returns the registered module object.
                let leaf = self.modname.rsplit('.').next().unwrap_or(&self.modname).trim_start_matches("lib").to_string();
                let entry_name = CString::new(format!("PyInit_{}", sanitize(&leaf))).unwrap();
                let entryf = LLVMAddFunction(self.cg.module, entry_name.as_ptr(), init_ty);
                let entry_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, entryf, c"entry".as_ptr());
                let entry_ok = LLVMAppendBasicBlockInContext(self.cg.ctx, entryf, c"ok".as_ptr());
                let entry_err = LLVMAppendBasicBlockInContext(self.cg.ctx, entryf, c"error".as_ptr());
                LLVMPositionBuilderAtEnd(self.b(), entry_bb);
                self.call("piper_initialize", &[]);
                let result = LLVMBuildCall2(self.b(), init_ty, init, std::ptr::null_mut(), 0, c"init".as_ptr());
                let failed = LLVMBuildIsNull(self.b(), result, c"failed".as_ptr());
                LLVMBuildCondBr(self.b(), failed, entry_err, entry_ok);
                LLVMPositionBuilderAtEnd(self.b(), entry_err);
                LLVMBuildRet(self.b(), self.null());
                LLVMPositionBuilderAtEnd(self.b(), entry_ok);
                self.decref(result);
                let modname = self.cstring_global(&self.modname.clone());
                let module = self.call("PyImport_AddModule", &[modname]);
                self.incref(module);
                LLVMBuildRet(self.b(), module);
            }

            if self.emit_main {
                let mut main_params = [self.i32_t, self.ptr_t];
                let main_ty = LLVMFunctionType(self.i32_t, main_params.as_mut_ptr(), 2, 0);
                let mainf = LLVMAddFunction(self.cg.module, c"main".as_ptr(), main_ty);
                let mbb = LLVMAppendBasicBlockInContext(self.cg.ctx, mainf, c"entry".as_ptr());
                LLVMPositionBuilderAtEnd(self.b(), mbb);
                let argc = LLVMGetParam(mainf, 0);
                let argv = LLVMGetParam(mainf, 1);
                let entry = if self.specialize_core { "piper_main_core" } else { "piper_main" };
                let r = self.call(entry, &[argc, argv, init]);
                LLVMBuildRet(self.b(), r);
            }
        }
        Ok(())
    }

    // ---- statements ------------------------------------------------------------------

    fn stmts(&mut self, ss: &[Stmt]) -> LResult<()> {
        for s in ss {
            if self.terminated() { break; }
            self.stmt(s)?;
        }
        Ok(())
    }

    fn stmt(&mut self, s: &Stmt) -> LResult<()> {
        self.set_line(s.span);
        match &s.kind {
            StmtKind::Expr { value } => { let v = self.expr(value)?; self.decref(v); }
            StmtKind::Pass => {}
            StmtKind::Assign { targets, value, .. } => {
                let v = self.expr(value)?;
                for (i, t) in targets.iter().enumerate() {
                    if i + 1 < targets.len() { self.incref(v); }
                    self.assign(t, v)?;
                }
            }
            StmtKind::AugAssign { target, op, value } => self.augassign(target, *op, value)?,
            StmtKind::AnnAssign { target, annotation: _, value, simple } => {
                if let Some(v) = value { let val = self.expr(v)?; self.assign(target, val)?; }
                if *simple && matches!(self.scope().kind, ScopeKind::Module | ScopeKind::Class) {
                    if let ExprKind::Name { id, .. } = &target.kind {
                        let ns = self.namespace_dict();
                        let r = self.call("piper_setup_annotations", &[ns]);
                        self.check_neg(r);
                        let key = self.name_const("__annotations__");
                        let annotations = self.call("PyObject_GetItem", &[ns, key]);
                        self.check_null(annotations);
                        let name = self.name_const(id);
                        let none = self.call("piper_none", &[]);
                        let r = self.call("PyObject_SetItem", &[annotations, name, none]);
                        self.decref(annotations);
                        self.check_neg(r);
                    }
                }
                if value.is_none() && !matches!(&target.kind, ExprKind::Name { .. }) {
                    // evaluate the target's subexpressions for side effects
                    match &target.kind {
                        ExprKind::Attribute { value: obj, .. } => { let o = self.expr(obj)?; self.decref(o); }
                        ExprKind::Subscript { value: obj, slice, .. } => { let o = self.expr(obj)?; self.decref(o); let sl = self.expr(slice)?; self.decref(sl); }
                        _ => {}
                    }
                }
            }
            StmtKind::Return { value } => {
                let v = match value { Some(e) => self.expr(e)?, None => { let n = self.call("piper_none", &[]); self.incref(n); n } };
                let rs = self.f().return_slot;
                self.store(rs, v);
                self.run_cleanups_for_exit(0)?;
                let rb = self.f().return_bb;
                self.br(rb);
            }
            StmtKind::If { test, body, orelse } => {
                let then_bb = self.bb("then");
                let else_bb = self.bb("else");
                let end_bb = self.bb("endif");
                self.cond_jump(test, then_bb, else_bb)?;
                self.position(then_bb);
                self.stmts(body)?;
                self.br(end_bb);
                self.position(else_bb);
                self.stmts(orelse)?;
                self.br(end_bb);
                self.position(end_bb);
            }
            StmtKind::While { test, body, orelse } => {
                let cond_bb = self.bb("while.cond");
                let body_bb = self.bb("while.body");
                let else_bb = self.bb("while.else");
                let end_bb = self.bb("while.end");
                self.br(cond_bb);
                self.position(cond_bb);
                self.cond_jump(test, body_bb, else_bb)?;
                self.position(body_bb);
                let depth = self.f().cleanups.len();
                self.fm().loops.push(Loop { break_bb: end_bb, continue_bb: cond_bb, depth });
                self.stmts(body)?;
                self.fm().loops.pop();
                self.br(cond_bb);
                self.position(else_bb);
                self.stmts(orelse)?;
                self.br(end_bb);
                self.position(end_bb);
            }
            StmtKind::For { target, iter, body, orelse, .. } => self.for_loop(target, iter, body, orelse)?,
            StmtKind::Break => {
                let Some(lp) = self.f().loops.last().copied_loop() else { return err("'break' outside loop", s.span) };
                self.run_cleanups_for_exit(lp.depth)?;
                self.br(lp.break_bb);
            }
            StmtKind::Continue => {
                let Some(lp) = self.f().loops.last().copied_loop() else { return err("'continue' not properly in loop", s.span) };
                self.run_cleanups_for_exit(lp.depth)?;
                self.br(lp.continue_bb);
            }
            StmtKind::FunctionDef { name, args, body, decorator_list, type_params, .. } | StmtKind::AsyncFunctionDef { name, args, body, decorator_list, type_params, .. } => {
                let decos: Vec<ValueRef> = decorator_list.iter().map(|d| self.expr(d)).collect::<LResult<_>>()?;
                let f = self.make_function(name, args, body, type_params, s.span)?;
                let f = self.apply_decorators(f, &decos)?;
                self.store_name(name, f)?;
            }
            StmtKind::ClassDef { name, bases, keywords, body, decorator_list, type_params } => {
                let decos: Vec<ValueRef> = decorator_list.iter().map(|d| self.expr(d)).collect::<LResult<_>>()?;
                let cls = self.make_class(name, bases, keywords, body, type_params, s.span)?;
                let cls = self.apply_decorators(cls, &decos)?;
                self.store_name(name, cls)?;
            }
            StmtKind::Delete { targets } => { for t in targets { self.delete(t)?; } }
            StmtKind::Global { .. } | StmtKind::Nonlocal { .. } => {}
            StmtKind::Import { names } => {
                for a in names {
                    let nm = self.name_const(&a.name);
                    let g = self.globals_ptr();
                    let zero = self.i32c(0);
                    let fromlist = self.null();
                    let m = self.call("piper_import_name", &[g, nm, fromlist, zero]);
                    self.check_null(m);
                    if let Some(asname) = &a.asname {
                        // import a.b.c as x binds the leaf: walk attributes
                        let mut cur = m;
                        for part in a.name.split('.').skip(1) {
                            let pn = self.name_const(part);
                            let next = self.call("PyObject_GetAttr", &[cur, pn]);
                            self.decref(cur);
                            self.check_null(next);
                            cur = next;
                        }
                        self.store_name(asname, cur)?;
                    } else {
                        let top = a.name.split('.').next().unwrap();
                        self.store_name(top, m)?;
                    }
                }
            }
            StmtKind::ImportFrom { module, names, level } => {
                let mod_name = self.name_const(module.as_deref().unwrap_or(""));
                let fl: Vec<String> = names.iter().map(|a| a.name.clone()).collect();
                let fromlist = self.names_tuple(&fl);
                let g = self.globals_ptr();
                let lv = self.i32c(*level as i64);
                let m = self.call("piper_import_name", &[g, mod_name, fromlist, lv]);
                self.check_null(m);
                if names.len() == 1 && names[0].name == "*" {
                    let ns = self.namespace_dict();
                    let r = self.call("piper_import_star", &[m, ns]);
                    self.decref(m);
                    self.check_neg(r);
                } else {
                    for a in names {
                        let nm = self.name_const(&a.name);
                        let v = self.call("piper_import_from", &[m, nm]);
                        self.check_null(v);
                        self.store_name(a.asname.as_deref().unwrap_or(&a.name), v)?;
                    }
                    self.decref(m);
                }
            }
            StmtKind::Raise { exc, cause } => {
                match exc {
                    None => { self.call("piper_reraise", &[]); }
                    Some(e) => {
                        let ev = self.expr(e)?;
                        let cv = match cause { Some(c) => self.expr(c)?, None => self.null() };
                        self.call("piper_raise", &[ev, cv]);
                        self.decref(ev);
                        if cause.is_some() { self.decref(cv); }
                    }
                }
                let t = self.error_target();
                self.br(t);
            }
            StmtKind::Assert { test, msg } => {
                let ok_bb = self.bb("assert.ok");
                let fail_bb = self.bb("assert.fail");
                self.cond_jump(test, ok_bb, fail_bb)?;
                self.position(fail_bb);
                let m = match msg { Some(m) => self.expr(m)?, None => self.null() };
                self.call("piper_assert_failed", &[m]);
                if msg.is_some() { self.decref(m); }
                let t = self.error_target();
                self.br(t);
                self.position(ok_bb);
            }
            StmtKind::Try { body, handlers, orelse, finalbody } => self.try_stmt(body, handlers, orelse, finalbody, false, s.span)?,
            StmtKind::TryStar { body, handlers, orelse, finalbody } => self.try_stmt(body, handlers, orelse, finalbody, true, s.span)?,
            StmtKind::With { items, body, .. } => self.with_stmt(items, body)?,
            StmtKind::AsyncFor { target, iter, body, orelse, .. } => self.async_for_loop(target, iter, body, orelse)?,
            StmtKind::AsyncWith { items, body, .. } => self.async_with_stmt(items, body)?,
            StmtKind::Match { subject, cases } => self.match_stmt(subject, cases, s.span)?,
            StmtKind::TypeAlias { name, type_params, value } => {
                let ExprKind::Name { id, .. } = &name.kind else { return err("invalid type alias name", name.span) };
                let body = Stmt { kind: StmtKind::Return { value: Some(value.clone()) }, span: value.span };
                let mut arguments = Arguments::default();
                for parameter in type_params {
                    let parameter_name = match &parameter.kind {
                        TypeParamKind::TypeVar { name, .. } | TypeParamKind::ParamSpec { name, .. } | TypeParamKind::TypeVarTuple { name, .. } => name,
                    };
                    arguments.args.push(Arg { arg: parameter_name.clone(), annotation: None, type_comment: None, span: parameter.span });
                }
                let thunk = self.make_function(&format!("{id}.__value__"), &arguments, std::slice::from_ref(&body), &[], s.span)?;
                let alias_name = self.name_const(id);
                let params = self.type_params_tuple(type_params)?;
                let alias = self.call("piper_type_alias_new", &[alias_name, thunk, params]);
                self.decref(thunk); self.decref(params);
                self.check_null(alias);
                self.store_name(id, alias)?;
            }
        }
        Ok(())
    }

    /// Evaluate `test` for truth and branch.
    fn cond_jump(&mut self, test: &Expr, t: BasicBlockRef, e: BasicBlockRef) -> LResult<()> {
        // Short-circuit forms without materializing booleans.
        match &test.kind {
            ExprKind::UnaryOp { op: UnaryOp::Not, operand } => return self.cond_jump(operand, e, t),
            ExprKind::BoolOp { op, values } => {
                let n = values.len();
                for (i, v) in values.iter().enumerate() {
                    if i + 1 == n { return self.cond_jump(v, t, e); }
                    let next = self.bb("boolop.next");
                    match op { BoolOp::And => self.cond_jump(v, next, e)?, BoolOp::Or => self.cond_jump(v, t, next)? }
                    self.position(next);
                }
                return Ok(());
            }
            ExprKind::Constant { value: Constant::Bool(b), .. } => { self.br(if *b { t } else { e }); return Ok(()); }
            _ => {}
        }
        let v = self.expr(test)?;
        let r = self.call("PyObject_IsTrue", &[v]);
        self.decref(v);
        self.check_neg(r);
        let c = unsafe { LLVMBuildICmp(self.b(), INT_NE, r, self.i32c(0), c"truth".as_ptr()) };
        self.cond_br(c, t, e);
        Ok(())
    }

    fn for_loop(&mut self, target: &Expr, iter: &Expr, body: &[Stmt], orelse: &[Stmt]) -> LResult<()> {
        let it_obj = self.expr(iter)?;
        let it = self.call("PyObject_GetIter", &[it_obj]);
        self.decref(it_obj);
        self.check_null(it);
        let slot = self.alloca("for.iter");
        self.store(slot, it);
        self.fm().temp_slots.push(slot);
        let next_bb = self.bb("for.next");
        let body_bb = self.bb("for.body");
        let exhausted_bb = self.bb("for.exhausted");
        let else_bb = self.bb("for.else");
        let end_bb = self.bb("for.end");
        let break_bb = self.bb("for.break");
        self.br(next_bb);
        self.position(next_bb);
        let itv = self.load(slot);
        let item = self.call("PyIter_Next", &[itv]);
        let c = self.is_null(item);
        self.cond_br(c, exhausted_bb, body_bb);
        self.position(exhausted_bb);
        let e = self.call("PyErr_Occurred", &[]);
        let has_err = unsafe { LLVMBuildIsNotNull(self.b(), e, c"haserr".as_ptr()) };
        let err_bb = self.error_target();
        // release the iterator before either path
        let itv2 = self.load(slot);
        self.store(slot, self.null());
        self.decref(itv2);
        self.cond_br(has_err, err_bb, else_bb);
        self.position(body_bb);
        self.assign(target, item)?;
        let depth = self.f().cleanups.len();
        self.fm().loops.push(Loop { break_bb, continue_bb: next_bb, depth });
        self.stmts(body)?;
        self.fm().loops.pop();
        self.br(next_bb);
        self.position(break_bb);
        let itv3 = self.load(slot);
        self.store(slot, self.null());
        self.decref(itv3);
        self.br(end_bb);
        self.position(else_bb);
        self.stmts(orelse)?;
        self.br(end_bb);
        self.position(end_bb);
        Ok(())
    }

    fn async_for_loop(&mut self, target: &Expr, iter: &Expr, body: &[Stmt], orelse: &[Stmt]) -> LResult<()> {
        if self.f().generator.is_none() { return err("'async for' outside async function", iter.span); }
        let iterable = self.expr(iter)?;
        let iterator = self.call("piper_async_iter", &[iterable]);
        self.decref(iterable); self.check_null(iterator);
        let slot = self.alloca("asyncfor.iterator"); self.store(slot, iterator); self.fm().temp_slots.push(slot);
        let next_bb = self.bb("asyncfor.next"); let body_bb = self.bb("asyncfor.body");
        let stop_bb = self.bb("asyncfor.stop"); let else_bb = self.bb("asyncfor.else");
        let break_bb = self.bb("asyncfor.break"); let end_bb = self.bb("asyncfor.end");
        self.br(next_bb);
        self.position(next_bb);
        self.fm().handlers.push(Handler::Block(stop_bb));
        let current = self.load(slot);
        let awaitable = self.call("piper_async_next", &[current]);
        self.check_null(awaitable);
        let await_iterator = self.call("piper_await_iter", &[awaitable]);
        self.decref(awaitable); self.check_null(await_iterator);
        let item = self.yield_from_iterator(await_iterator)?;
        self.fm().handlers.pop();
        self.br(body_bb);
        self.position(body_bb);
        self.assign(target, item)?;
        let depth = self.f().cleanups.len(); self.fm().loops.push(Loop { break_bb, continue_bb: next_bb, depth });
        self.stmts(body)?; self.fm().loops.pop(); self.br(next_bb);
        self.position(stop_bb);
        let done = self.call("piper_async_iteration_done", &[]);
        let is_done = unsafe { LLVMBuildICmp(self.b(), INT_NE, done, self.i32c(0), c"asyncdone".as_ptr()) };
        let outer_error = self.error_target();
        self.cond_br(is_done, else_bb, outer_error);
        self.position(break_bb);
        let current = self.load(slot); self.store(slot, self.null()); self.decref(current); self.br(end_bb);
        self.position(else_bb);
        let current = self.load(slot); self.store(slot, self.null()); self.decref(current);
        self.stmts(orelse)?; self.br(end_bb);
        self.position(end_bb);
        Ok(())
    }

    // ---- names -----------------------------------------------------------------------

    fn globals_ptr(&mut self) -> ValueRef {
        let f = self.f();
        if f.has_funcobj { let fo = f.funcobj; return self.call("piper_function_globals", &[fo]); }
        self.load(self.globals_var)
    }
    fn builtins_ptr(&mut self) -> ValueRef { self.load(self.builtins_var) }
    /// Dict receiving names at module level (globals) or class level (ns).
    fn namespace_dict(&mut self) -> ValueRef {
        if self.f().has_ns { return self.f().ns; }
        self.globals_ptr()
    }

    /// Owned reference to a name's current value.
    fn load_name(&mut self, name: &str, span: Span) -> LResult<ValueRef> {
        let scope = self.scope();
        let res = scope.resolution(name);
        let kind = scope.kind;
        match kind {
            ScopeKind::Module => {
                let n = self.name_const(name);
                let g = self.globals_ptr();
                let b = self.builtins_ptr();
                let v = self.call("piper_load_global", &[g, b, n]);
                self.check_null(v);
                Ok(v)
            }
            ScopeKind::Class => {
                match res {
                    Resolution::Free => self.load_cell(name, true, span),
                    Resolution::GlobalExplicit => {
                        let n = self.name_const(name);
                        let g = self.globals_ptr();
                        let b = self.builtins_ptr();
                        let v = self.call("piper_load_global", &[g, b, n]);
                        self.check_null(v);
                        Ok(v)
                    }
                    _ => {
                        let n = self.name_const(name);
                        let ns = self.f().ns;
                        let g = self.globals_ptr();
                        let b = self.builtins_ptr();
                        let v = self.call("piper_load_name", &[ns, g, b, n]);
                        self.check_null(v);
                        Ok(v)
                    }
                }
            }
            _ => match res {
                Resolution::Local => {
                    let slot = self.f().locals.get(name).copied().unwrap_or_else(|| { std::ptr::null_mut() });
                    let slot = if slot.is_null() { self.local_slot(name) } else { slot };
                    let v = self.load(slot);
                    let ok = self.bb("local.ok");
                    let unbound = self.bb("local.unbound");
                    let c = self.is_null(v);
                    self.cond_br(c, unbound, ok);
                    self.position(unbound);
                    let n = self.name_const(name);
                    self.call("piper_unbound_local", &[n]);
                    let t = self.error_target();
                    self.br(t);
                    self.position(ok);
                    self.incref(v);
                    Ok(v)
                }
                Resolution::Cell => self.load_cell(name, false, span),
                Resolution::Free => self.load_cell(name, true, span),
                Resolution::GlobalExplicit | Resolution::GlobalImplicit => {
                    let n = self.name_const(name);
                    let g = self.globals_ptr();
                    let b = self.builtins_ptr();
                    let v = self.call("piper_load_global", &[g, b, n]);
                    self.check_null(v);
                    Ok(v)
                }
            },
        }
    }

    fn load_cell(&mut self, name: &str, is_free: bool, span: Span) -> LResult<ValueRef> {
        let Some(cell_slot) = self.f().cells.get(name).copied() else { return err(format!("internal: no cell for '{name}'"), span) };
        let cell = self.load(cell_slot);
        let n = self.name_const(name);
        let fr = self.i32c(is_free as i64);
        let v = self.call("piper_cell_get", &[cell, n, fr]);
        self.check_null(v);
        Ok(v)
    }

    fn local_slot(&mut self, name: &str) -> ValueRef {
        if let Some(s) = self.f().locals.get(name) { return *s; }
        let s = self.alloca(&format!("local.{name}"));
        self.fm().locals.insert(name.to_string(), s);
        s
    }

    /// Store an owned reference into a name.
    fn store_name(&mut self, name: &str, v: ValueRef) -> LResult<()> {
        let scope = self.scope();
        let res = scope.resolution(name);
        let kind = scope.kind;
        match kind {
            ScopeKind::Module => {
                let n = self.name_const(name);
                let g = self.globals_ptr();
                let r = self.call("piper_store_global", &[g, n, v]);
                self.decref(v);
                self.check_neg(r);
            }
            ScopeKind::Class => {
                if res == Resolution::GlobalExplicit {
                    let n = self.name_const(name);
                    let g = self.globals_ptr();
                    let r = self.call("piper_store_global", &[g, n, v]);
                    self.decref(v);
                    self.check_neg(r);
                } else if res == Resolution::Free || res == Resolution::Cell {
                    let cell_slot = self.f().cells[name];
                    let cell = self.load(cell_slot);
                    self.call("piper_cell_set", &[cell, v]);
                    self.decref(v);
                } else {
                    let n = self.name_const(name);
                    let ns = self.f().ns;
                    let r = self.call("PyObject_SetItem", &[ns, n, v]);
                    self.decref(v);
                    self.check_neg(r);
                }
            }
            _ => match res {
                Resolution::Local => {
                    let slot = self.local_slot(name);
                    let old = self.load(slot);
                    self.store(slot, v);
                    self.call("Py_DecRef", &[old]);
                }
                Resolution::Cell | Resolution::Free => {
                    let cell_slot = self.f().cells[name];
                    let cell = self.load(cell_slot);
                    self.call("piper_cell_set", &[cell, v]);
                    self.decref(v);
                }
                _ => {
                    let n = self.name_const(name);
                    let g = self.globals_ptr();
                    let r = self.call("piper_store_global", &[g, n, v]);
                    self.decref(v);
                    self.check_neg(r);
                }
            },
        }
        Ok(())
    }

    fn delete_name(&mut self, name: &str, span: Span) -> LResult<()> {
        let scope = self.scope();
        let res = scope.resolution(name);
        let kind = scope.kind;
        match (kind, res) {
            (ScopeKind::Module, _) | (_, Resolution::GlobalExplicit) | (_, Resolution::GlobalImplicit) => {
                let n = self.name_const(name);
                let g = self.globals_ptr();
                let r = self.call("piper_delete_global", &[g, n]);
                self.check_neg(r);
            }
            (ScopeKind::Class, _) if res != Resolution::Free && res != Resolution::Cell => {
                let n = self.name_const(name);
                let ns = self.f().ns;
                let r = self.call("PyObject_DelItem", &[ns, n]);
                self.check_neg(r);
            }
            (_, Resolution::Local) => {
                let slot = self.local_slot(name);
                let v = self.load(slot);
                let ok = self.bb("del.ok");
                let unbound = self.bb("del.unbound");
                let c = self.is_null(v);
                self.cond_br(c, unbound, ok);
                self.position(unbound);
                let n = self.name_const(name);
                self.call("piper_unbound_local", &[n]);
                let t = self.error_target();
                self.br(t);
                self.position(ok);
                self.store(slot, self.null());
                self.decref(v);
            }
            _ => {
                let cell_slot = self.f().cells[name];
                let cell = self.load(cell_slot);
                let _ = span;
                let nul = self.null();
                self.call("piper_cell_set", &[cell, nul]);
            }
        }
        Ok(())
    }

    // ---- assignment ------------------------------------------------------------------

    /// Assign an owned reference to a target (consumes `v`).
    fn assign(&mut self, target: &Expr, v: ValueRef) -> LResult<()> {
        match &target.kind {
            ExprKind::Name { id, .. } => self.store_name(id, v),
            ExprKind::Attribute { value, attr, .. } => {
                let obj = self.expr(value)?;
                let n = self.name_const(attr);
                let r = self.call("PyObject_SetAttr", &[obj, n, v]);
                self.decref(obj); self.decref(v);
                self.check_neg(r);
                Ok(())
            }
            ExprKind::Subscript { value, slice, .. } => {
                let obj = self.expr(value)?;
                let key = self.expr(slice)?;
                let r = self.call("PyObject_SetItem", &[obj, key, v]);
                self.decref(obj); self.decref(key); self.decref(v);
                self.check_neg(r);
                Ok(())
            }
            ExprKind::Tuple { elts, .. } | ExprKind::List { elts, .. } => {
                let star = elts.iter().position(|e| matches!(e.kind, ExprKind::Starred { .. }));
                let n = elts.len();
                let out = self.ptr_array(&vec![self.null(); n]);
                let arr_t = unsafe { LLVMArrayType2(self.ptr_t, n.max(1) as u64) };
                let r = match star {
                    None => { let nn = self.i64c(n as i64); self.call("piper_unpack_sequence", &[v, nn, out]) }
                    Some(si) => { let before = self.i64c(si as i64); let after = self.i64c((n - si - 1) as i64); self.call("piper_unpack_ex", &[v, before, after, out]) }
                };
                self.decref(v);
                self.check_neg(r);
                for (i, e) in elts.iter().enumerate() {
                    let item = unsafe {
                        let mut idx = [self.i64c(0), self.i64c(i as i64)];
                        let p = LLVMBuildGEP2(self.b(), arr_t, out, idx.as_mut_ptr(), 2, c"slot".as_ptr());
                        LLVMBuildLoad2(self.b(), self.ptr_t, p, c"item".as_ptr())
                    };
                    match &e.kind { ExprKind::Starred { value, .. } => self.assign(value, item)?, _ => self.assign(e, item)? }
                }
                Ok(())
            }
            ExprKind::Starred { .. } => err("starred assignment target must be in a list or tuple", target.span),
            _ => err("cannot assign to expression", target.span),
        }
    }

    fn augassign(&mut self, target: &Expr, op: Operator, value: &Expr) -> LResult<()> {
        let opc = self.i32c(binop_code(op) as i64);
        match &target.kind {
            ExprKind::Name { id, .. } => {
                let cur = self.load_name(id, target.span)?;
                let rhs = self.expr(value)?;
                let r = self.call("piper_inplace_binop", &[opc, cur, rhs]);
                self.decref(cur); self.decref(rhs);
                self.check_null(r);
                self.store_name(id, r)
            }
            ExprKind::Attribute { value: obj, attr, .. } => {
                let o = self.expr(obj)?;
                let n = self.name_const(attr);
                let cur = self.call("PyObject_GetAttr", &[o, n]);
                self.check_null(cur);
                let rhs = self.expr(value)?;
                let r = self.call("piper_inplace_binop", &[opc, cur, rhs]);
                self.decref(cur); self.decref(rhs);
                self.check_null(r);
                let rc = self.call("PyObject_SetAttr", &[o, n, r]);
                self.decref(o); self.decref(r);
                self.check_neg(rc);
                Ok(())
            }
            ExprKind::Subscript { value: obj, slice, .. } => {
                let o = self.expr(obj)?;
                let k = self.expr(slice)?;
                let cur = self.call("PyObject_GetItem", &[o, k]);
                self.check_null(cur);
                let rhs = self.expr(value)?;
                let r = self.call("piper_inplace_binop", &[opc, cur, rhs]);
                self.decref(cur); self.decref(rhs);
                self.check_null(r);
                let rc = self.call("PyObject_SetItem", &[o, k, r]);
                self.decref(o); self.decref(k); self.decref(r);
                self.check_neg(rc);
                Ok(())
            }
            _ => err("illegal expression for augmented assignment", target.span),
        }
    }

    fn delete(&mut self, t: &Expr) -> LResult<()> {
        match &t.kind {
            ExprKind::Name { id, .. } => self.delete_name(id, t.span),
            ExprKind::Attribute { value, attr, .. } => {
                let o = self.expr(value)?;
                let n = self.name_const(attr);
                let nul = self.null();
                let r = self.call("PyObject_SetAttr", &[o, n, nul]);
                self.decref(o);
                self.check_neg(r);
                Ok(())
            }
            ExprKind::Subscript { value, slice, .. } => {
                let o = self.expr(value)?;
                let k = self.expr(slice)?;
                let r = self.call("PyObject_DelItem", &[o, k]);
                self.decref(o); self.decref(k);
                self.check_neg(r);
                Ok(())
            }
            ExprKind::Tuple { elts, .. } | ExprKind::List { elts, .. } => { for e in elts { self.delete(e)?; } Ok(()) }
            _ => err("cannot delete expression", t.span),
        }
    }

    // ---- expressions -----------------------------------------------------------------

    fn expr(&mut self, e: &Expr) -> LResult<ValueRef> {
        match &e.kind {
            ExprKind::Constant { value, .. } => Ok(self.constant(value)),
            ExprKind::Name { id, .. } => self.load_name(id, e.span),
            ExprKind::BinOp { left, op, right } => {
                let mut l = self.expr(left)?;
                let spill = if self.f().generator.is_some() {
                    let slot = self.alloca("binop.left");
                    self.store(slot, l);
                    Some(slot)
                } else { None };
                let r = self.expr(right)?;
                if let Some(slot) = spill { l = self.load(slot); self.store(slot, self.null()); }
                let opc = self.i32c(binop_code(*op) as i64);
                let v = self.call("piper_binop", &[opc, l, r]);
                self.decref(l); self.decref(r);
                self.check_null(v);
                Ok(v)
            }
            ExprKind::UnaryOp { op, operand } => {
                match op {
                    UnaryOp::Not => {
                        let t = self.bb("not.true");
                        let f = self.bb("not.false");
                        let end = self.bb("not.end");
                        self.cond_jump(operand, f, t)?;
                        self.position(t);
                        let tv = self.call("piper_none", &[]); let _ = tv;
                        let one = self.i32c(1);
                        let tv = self.call("piper_bool", &[one]);
                        let tb = self.current_bb();
                        self.br(end);
                        self.position(f);
                        let zero = self.i32c(0);
                        let fv = self.call("piper_bool", &[zero]);
                        let fb = self.current_bb();
                        self.br(end);
                        self.position(end);
                        let phi = unsafe { LLVMBuildPhi(self.b(), self.ptr_t, c"not".as_ptr()) };
                        let mut vals = [tv, fv];
                        let mut bbs = [tb, fb];
                        unsafe { LLVMAddIncoming(phi, vals.as_mut_ptr(), bbs.as_mut_ptr(), 2); }
                        self.incref(phi);
                        Ok(phi)
                    }
                    _ => {
                        let o = self.expr(operand)?;
                        let name = match op { UnaryOp::USub => "PyNumber_Negative", UnaryOp::UAdd => "PyNumber_Positive", UnaryOp::Invert => "PyNumber_Invert", UnaryOp::Not => unreachable!() };
                        let v = self.call(name, &[o]);
                        self.decref(o);
                        self.check_null(v);
                        Ok(v)
                    }
                }
            }
            ExprKind::BoolOp { op, values } => {
                // Result is the last evaluated operand.
                let end = self.bb("boolop.end");
                let result = self.alloca("boolop.result");
                let n = values.len();
                for (i, v) in values.iter().enumerate() {
                    let val = self.expr(v)?;
                    self.store(result, val);
                    if i + 1 == n { break; }
                    let t = self.call("PyObject_IsTrue", &[val]);
                    self.check_neg(t);
                    let c = unsafe { LLVMBuildICmp(self.b(), INT_NE, t, self.i32c(0), c"truth".as_ptr()) };
                    let next = self.bb("boolop.next");
                    match op { BoolOp::And => self.cond_br(c, next, end), BoolOp::Or => self.cond_br(c, end, next) }
                    self.position(next);
                    let prev = self.take(result);
                    self.decref(prev);
                }
                self.br(end);
                self.position(end);
                Ok(self.take(result))
            }
            ExprKind::Compare { left, ops, comparators } => self.compare(left, ops, comparators),
            ExprKind::Call { func, args, keywords } => self.call_expr(func, args, keywords, e.span),
            ExprKind::Attribute { value, attr, .. } => {
                let o = self.expr(value)?;
                let n = self.name_const(attr);
                let v = self.call("PyObject_GetAttr", &[o, n]);
                self.decref(o);
                self.check_null(v);
                Ok(v)
            }
            ExprKind::Subscript { value, slice, .. } => {
                let o = self.expr(value)?;
                let k = self.expr(slice)?;
                let v = self.call("PyObject_GetItem", &[o, k]);
                self.decref(o); self.decref(k);
                self.check_null(v);
                Ok(v)
            }
            ExprKind::Slice { lower, upper, step } => {
                let a = match lower { Some(x) => self.expr(x)?, None => self.null() };
                let b = match upper { Some(x) => self.expr(x)?, None => self.null() };
                let c = match step { Some(x) => self.expr(x)?, None => self.null() };
                let v = self.call("piper_build_slice", &[a, b, c]);
                if lower.is_some() { self.decref(a); }
                if upper.is_some() { self.decref(b); }
                if step.is_some() { self.decref(c); }
                self.check_null(v);
                Ok(v)
            }
            ExprKind::Tuple { elts, .. } => {
                if elts.iter().any(|x| matches!(x.kind, ExprKind::Starred { .. })) {
                    let l = self.list_display(elts)?;
                    let t = self.call("piper_list_to_tuple", &[l]);
                    self.decref(l);
                    self.check_null(t);
                    return Ok(t);
                }
                let n = self.i64c(elts.len() as i64);
                let t = self.call("PyTuple_New", &[n]);
                self.check_null(t);
                for (i, x) in elts.iter().enumerate() {
                    let v = self.expr(x)?;
                    let idx = self.i64c(i as i64);
                    self.call("PyTuple_SetItem", &[t, idx, v]);
                }
                Ok(t)
            }
            ExprKind::List { elts, .. } => self.list_display(elts),
            ExprKind::Set { elts } => {
                let nul = self.null();
                let s = self.call("PySet_New", &[nul]);
                self.check_null(s);
                for x in elts {
                    match &x.kind {
                        ExprKind::Starred { value, .. } => { let v = self.expr(value)?; let r = self.call("piper_set_update", &[s, v]); self.decref(v); self.check_neg(r); }
                        _ => { let v = self.expr(x)?; let r = self.call("piper_set_add", &[s, v]); self.decref(v); self.check_neg(r); }
                    }
                }
                Ok(s)
            }
            ExprKind::Dict { keys, values } => {
                let d = self.call("PyDict_New", &[]);
                self.check_null(d);
                for (k, v) in keys.iter().zip(values) {
                    match k {
                        Some(k) => {
                            let kv = self.expr(k)?;
                            let vv = self.expr(v)?;
                            let r = self.call("PyDict_SetItem", &[d, kv, vv]);
                            self.decref(kv); self.decref(vv);
                            self.check_neg(r);
                        }
                        None => {
                            let vv = self.expr(v)?;
                            let r = self.call("piper_dict_update", &[d, vv]);
                            self.decref(vv);
                            self.check_neg(r);
                        }
                    }
                }
                Ok(d)
            }
            ExprKind::IfExp { test, body, orelse } => {
                let t = self.bb("ifexp.then");
                let f = self.bb("ifexp.else");
                let end = self.bb("ifexp.end");
                let slot = self.alloca("ifexp");
                self.cond_jump(test, t, f)?;
                self.position(t);
                let tv = self.expr(body)?;
                self.store(slot, tv);
                self.br(end);
                self.position(f);
                let fv = self.expr(orelse)?;
                self.store(slot, fv);
                self.br(end);
                self.position(end);
                Ok(self.take(slot))
            }
            ExprKind::JoinedStr { values } => {
                let mut parts = Vec::new();
                for v in values { parts.push(self.expr(v)?); }
                let arr = self.ptr_array(&parts);
                let n = self.i64c(parts.len() as i64);
                let s = self.call("piper_build_string", &[arr, n]);
                for p in &parts { self.decref(*p); }
                self.check_null(s);
                Ok(s)
            }
            ExprKind::FormattedValue { value, conversion, format_spec } => {
                let v = self.expr(value)?;
                let spec = match format_spec { Some(s) => self.expr(s)?, None => self.null() };
                let conv = self.i32c(*conversion as i64);
                let r = self.call("piper_format_value", &[v, conv, spec]);
                self.decref(v);
                if format_spec.is_some() { self.decref(spec); }
                self.check_null(r);
                Ok(r)
            }
            ExprKind::Interpolation { value, str, conversion, format_spec } => {
                let value_obj = self.expr(value)?;
                let expression = self.name_const(str);
                let spec = match format_spec { Some(s) => self.expr(s)?, None => self.null() };
                let conv = self.i32c(*conversion as i64);
                let interpolation = self.call("piper_interpolation_new", &[value_obj, expression, conv, spec]);
                self.decref(value_obj);
                if format_spec.is_some() { self.decref(spec); }
                self.check_null(interpolation);
                Ok(interpolation)
            }
            ExprKind::TemplateStr { values } => {
                let mut parts = Vec::new();
                for value in values { parts.push(self.expr(value)?); }
                let array = self.ptr_array(&parts);
                let count = self.i64c(parts.len() as i64);
                let template = self.call("piper_template_new", &[array, count]);
                for part in parts { self.decref(part); }
                self.check_null(template);
                Ok(template)
            }
            ExprKind::Lambda { args, body } => {
                let body_stmt = Stmt { kind: StmtKind::Return { value: Some(body.clone()) }, span: body.span };
                self.make_function("<lambda>", args, std::slice::from_ref(&body_stmt), &[], e.span)
            }
            ExprKind::NamedExpr { target, value } => {
                let v = self.expr(value)?;
                self.incref(v);
                let ExprKind::Name { id, .. } = &target.kind else { return err("invalid walrus target", e.span) };
                self.store_name_in_binding_scope(id, v)?;
                Ok(v)
            }
            ExprKind::ListComp { elt, generators } => if generators.iter().any(|g| g.is_async) { self.async_comprehension(CompKind::List, elt, None, generators, e.span) } else { self.comprehension(CompKind::List, elt, None, generators, e.span) },
            ExprKind::SetComp { elt, generators } => if generators.iter().any(|g| g.is_async) { self.async_comprehension(CompKind::Set, elt, None, generators, e.span) } else { self.comprehension(CompKind::Set, elt, None, generators, e.span) },
            ExprKind::DictComp { key, value, generators } => if generators.iter().any(|g| g.is_async) { self.async_comprehension(CompKind::Dict, key, Some(value), generators, e.span) } else { self.comprehension(CompKind::Dict, key, Some(value), generators, e.span) },
            ExprKind::GeneratorExp { elt, generators } => self.generator_comprehension(elt, generators, e.span),
            ExprKind::Yield { value } => self.yield_expr(value.as_deref(), e.span),
            ExprKind::YieldFrom { value } => self.yield_from_expr(value, e.span),
            ExprKind::Await { value } => {
                let awaitable = self.expr(value)?;
                let iterator = self.call("piper_await_iter", &[awaitable]);
                self.decref(awaitable);
                self.check_null(iterator);
                self.yield_from_iterator(iterator)
            }
            ExprKind::Starred { .. } => err("can't use starred expression here", e.span),
        }
    }

    fn yield_expr(&mut self, value: Option<&Expr>, span: Span) -> LResult<ValueRef> {
        let yielded = match value {
            Some(value) => self.expr(value)?,
            None => { let none = self.call("piper_none", &[]); self.incref(none); none }
        };
        self.suspend_value(yielded, span)
    }

    fn suspend_value(&mut self, yielded: ValueRef, span: Span) -> LResult<ValueRef> {
        let Some(mut generator) = self.f().generator else { return err("yield outside generator", span) };
        let resume = self.bb("yield.resume");
        generator.next_state += 1;
        let state = generator.next_state;
        let dispatch = generator.dispatch;
        self.fm().generator = Some(generator);
        let object = generator.object;
        let state_value = self.i32c(state as i64);
        self.call("piper_generator_set_state", &[object, state_value]);
        self.call("piper_frame_pop", &[]);
        unsafe { LLVMBuildRet(self.b(), yielded); LLVMAddCase(dispatch, self.i32c(state as i64), resume); }
        self.position(resume);
        let sent = generator.sent;
        self.incref(sent);
        Ok(sent)
    }

    fn yield_from_expr(&mut self, value: &Expr, span: Span) -> LResult<ValueRef> {
        if self.f().generator.is_none() { return err("yield from outside generator", span); }
        let source = self.expr(value)?;
        let iterator = self.call("PyObject_GetIter", &[source]);
        self.decref(source);
        self.check_null(iterator);
        self.yield_from_iterator(iterator)
    }

    fn yield_from_iterator(&mut self, iterator: ValueRef) -> LResult<ValueRef> {
        let iterator_slot = self.alloca("yieldfrom.iterator");
        self.fm().temp_slots.push(iterator_slot);
        self.store(iterator_slot, iterator);
        let first = self.bb("yieldfrom.first");
        let resumed = self.bb("yieldfrom.resumed");
        let finish = self.bb("yieldfrom.finish");
        let result_slot = self.alloca("yieldfrom.result");
        self.br(first);

        self.position(first);
        let it = self.load(iterator_slot);
        let none = self.call("piper_none", &[]);
        let zero = self.i32c(0);
        let pair = self.call("piper_yield_from_next", &[it, none, zero]);
        self.check_null(pair);
        self.yield_from_pair(pair, resumed, finish, result_slot)?;

        self.position(resumed);
        let it = self.load(iterator_slot);
        let sent = self.f().generator.unwrap().sent;
        let one = self.i32c(1);
        let pair = self.call("piper_yield_from_next", &[it, sent, one]);
        self.check_null(pair);
        self.yield_from_pair(pair, resumed, finish, result_slot)?;

        self.position(finish);
        let iterator = self.load(iterator_slot);
        self.decref(iterator);
        self.store(iterator_slot, self.null());
        Ok(self.take(result_slot))
    }

    fn yield_from_pair(&mut self, pair: ValueRef, resumed: BasicBlockRef, finish: BasicBlockRef, result_slot: ValueRef) -> LResult<()> {
        let zero = self.i64c(0);
        let one = self.i64c(1);
        let flag = self.call("PyTuple_GetItem", &[pair, zero]);
        let value = self.call("PyTuple_GetItem", &[pair, one]);
        self.incref(value);
        let done = self.call("PyObject_IsTrue", &[flag]);
        self.decref(pair);
        self.check_neg(done);
        let complete = self.bb("yieldfrom.complete");
        let produce = self.bb("yieldfrom.produce");
        let condition = unsafe { LLVMBuildICmp(self.b(), INT_NE, done, self.i32c(0), c"done".as_ptr()) };
        self.cond_br(condition, complete, produce);
        self.position(complete);
        self.store(result_slot, value);
        self.br(finish);
        self.position(produce);
        let sent = self.suspend_value(value, Span::default())?;
        self.decref(sent);
        self.br(resumed);
        Ok(())
    }

    /// Walrus inside a comprehension binds in the enclosing scope: handled
    /// through the free/nonlocal machinery, so a plain store works.
    fn store_name_in_binding_scope(&mut self, id: &str, v: ValueRef) -> LResult<()> { self.store_name(id, v) }

    fn list_display(&mut self, elts: &[Expr]) -> LResult<ValueRef> {
        let zero = self.i64c(0);
        let l = self.call("PyList_New", &[zero]);
        self.check_null(l);
        for x in elts {
            match &x.kind {
                ExprKind::Starred { value, .. } => { let v = self.expr(value)?; let r = self.call("piper_list_extend", &[l, v]); self.decref(v); self.check_null(r); }
                _ => { let v = self.expr(x)?; let r = self.call("PyList_Append", &[l, v]); self.decref(v); self.check_neg(r); }
            }
        }
        Ok(l)
    }

    fn compare(&mut self, left: &Expr, ops: &[CmpOp], comparators: &[Expr]) -> LResult<ValueRef> {
        let mut cur = self.expr(left)?;
        if ops.len() == 1 {
            let r = self.expr(&comparators[0])?;
            let v = self.compare_one(ops[0], cur, r);
            self.decref(cur); self.decref(r);
            self.check_null(v);
            return Ok(v);
        }
        // chained: a < b < c  ==  (a < b) and (b < c), each operand evaluated once
        let result = self.alloca("cmp.result");
        let end = self.bb("cmp.end");
        let n = ops.len();
        for (i, (op, rhs_e)) in ops.iter().zip(comparators).enumerate() {
            let rhs = self.expr(rhs_e)?;
            let v = self.compare_one(*op, cur, rhs);
            self.decref(cur);
            self.check_null(v);
            self.store(result, v);
            if i + 1 == n { self.decref(rhs); break; }
            let t = self.call("PyObject_IsTrue", &[v]);
            self.check_neg(t);
            let c = unsafe { LLVMBuildICmp(self.b(), INT_NE, t, self.i32c(0), c"truth".as_ptr()) };
            let next = self.bb("cmp.next");
            let stop = self.bb("cmp.stop");
            self.cond_br(c, next, stop);
            self.position(stop);
            self.decref(rhs);
            self.br(end);
            self.position(next);
            let discarded = self.take(result);
            self.decref(discarded);
            cur = rhs;
        }
        self.br(end);
        self.position(end);
        Ok(self.take(result))
    }

    fn compare_one(&mut self, op: CmpOp, a: ValueRef, b: ValueRef) -> ValueRef {
        match op {
            CmpOp::Is => { let z = self.i32c(0); self.call("piper_is", &[a, b, z]) }
            CmpOp::IsNot => { let o = self.i32c(1); self.call("piper_is", &[a, b, o]) }
            CmpOp::In => { let z = self.i32c(0); self.call("piper_contains", &[a, b, z]) }
            CmpOp::NotIn => { let o = self.i32c(1); self.call("piper_contains", &[a, b, o]) }
            _ => {
                let code = match op { CmpOp::Lt => 0, CmpOp::LtE => 1, CmpOp::Eq => 2, CmpOp::NotEq => 3, CmpOp::Gt => 4, CmpOp::GtE => 5, _ => unreachable!() };
                let c = self.i32c(code);
                self.call("piper_compare", &[c, a, b])
            }
        }
    }

    fn call_expr(&mut self, func: &Expr, args: &[Expr], keywords: &[Keyword], span: Span) -> LResult<ValueRef> {
        let has_star = args.iter().any(|a| matches!(a.kind, ExprKind::Starred { .. })) || keywords.iter().any(|k| k.arg.is_none());
        if self.specialize_core && !has_star {
            if let ExprKind::Name { id, .. } = &func.kind {
                if self.scope().resolution(id) == Resolution::GlobalImplicit && (id == "print" || id == "input") {
                    let mut values = Vec::new();
                    for argument in args { values.push(self.expr(argument)?); }
                    for keyword in keywords { values.push(self.expr(&keyword.value)?); }
                    let array = self.ptr_array(&values);
                    let count = self.i64c(args.len() as i64);
                    let result = if id == "print" {
                        let names = if keywords.is_empty() { self.null() } else {
                            let names: Vec<String> = keywords.iter().map(|keyword| keyword.arg.clone().unwrap()).collect();
                            self.names_tuple(&names)
                        };
                        self.call("piper_builtin_print", &[array, count, names])
                    } else if keywords.is_empty() {
                        self.call("piper_builtin_input", &[array, count])
                    } else {
                        return err("input() takes no keyword arguments", span);
                    };
                    for value in values { self.decref(value); }
                    self.check_null(result);
                    return Ok(result);
                }
            }
        }
        // callable, with the method fast path
        let (callable, self_slot) = match &func.kind {
            ExprKind::Attribute { value, attr, .. } if !has_star => {
                let obj = self.expr(value)?;
                let n = self.name_const(attr);
                let self_slot = self.alloca("method.self");
                let m = self.call("piper_load_method", &[obj, n, self_slot]);
                // obj must stay alive while bound; keep it in a slot and release after the call
                let objslot = self.alloca("method.obj");
                self.store(objslot, obj);
                self.check_null(m);
                (m, Some((self_slot, objslot)))
            }
            _ => (self.expr(func)?, None),
        };
        if has_star {
            // build tuple and dict
            let l = self.list_display(args)?;
            let t = self.call("piper_list_to_tuple", &[l]);
            self.decref(l);
            self.check_null(t);
            let kw = if keywords.is_empty() { self.null() } else {
                let d = self.call("PyDict_New", &[]);
                self.check_null(d);
                for k in keywords {
                    let v = self.expr(&k.value)?;
                    let r = match &k.arg {
                        Some(name) => { let n = self.name_const(name); self.call("PyDict_SetItem", &[d, n, v]) }
                        None => self.call("piper_dict_merge_call", &[d, v, callable]),
                    };
                    self.decref(v);
                    self.check_neg(r);
                }
                d
            };
            let r = self.call("piper_call_ex", &[callable, t, kw]);
            self.decref(callable); self.decref(t);
            if !keywords.is_empty() { self.decref(kw); }
            self.check_null(r);
            return Ok(r);
        }
        let _ = span;
        let mut vals = Vec::new();
        for a in args { vals.push(self.expr(a)?); }
        for k in keywords { vals.push(self.expr(&k.value)?); }
        let kwnames = if keywords.is_empty() { self.null() } else { let names: Vec<String> = keywords.iter().map(|k| k.arg.clone().unwrap()).collect(); self.names_tuple(&names) };
        let r = match self_slot {
            Some((self_slot, objslot)) => {
                // Two paths: bound self present (prepend) or not.
                let sv = self.load(self_slot);
                let has_self = unsafe { LLVMBuildIsNotNull(self.b(), sv, c"hasself".as_ptr()) };
                let with_bb = self.bb("call.withself");
                let without_bb = self.bb("call.noself");
                let end = self.bb("call.end");
                let result = self.alloca("call.result");
                self.cond_br(has_self, with_bb, without_bb);
                self.position(with_bb);
                let mut all = vec![sv];
                all.extend(&vals);
                let arr = self.ptr_array(&all);
                let n = self.i64c(args.len() as i64 + 1);
                let r1 = self.call("piper_call", &[callable, arr, n, kwnames]);
                self.store(result, r1);
                self.br(end);
                self.position(without_bb);
                let arr2 = self.ptr_array(&vals);
                let n2 = self.i64c(args.len() as i64);
                let r2 = self.call("piper_call", &[callable, arr2, n2, kwnames]);
                self.store(result, r2);
                self.br(end);
                self.position(end);
                self.store(self_slot, self.null());
                let obj = self.take(objslot);
                self.decref(obj);
                self.take(result)
            }
            None => {
                let arr = self.ptr_array(&vals);
                let n = self.i64c(args.len() as i64);
                self.call("piper_call", &[callable, arr, n, kwnames])
            }
        };
        self.decref(callable);
        for v in &vals { self.decref(*v); }
        self.check_null(r);
        Ok(r)
    }

    fn apply_decorators(&mut self, mut v: ValueRef, decos: &[ValueRef]) -> LResult<ValueRef> {
        for d in decos.iter().rev() {
            let arr = self.ptr_array(&[v]);
            let one = self.i64c(1);
            let nul = self.null();
            let r = self.call("piper_call", &[*d, arr, one, nul]);
            self.decref(v); self.decref(*d);
            self.check_null(r);
            v = r;
        }
        Ok(v)
    }

    // ---- functions -------------------------------------------------------------------

    fn next_child_scope(&mut self) -> usize {
        let parent = self.f().scope;
        let idx = self.f().next_child;
        self.fm().next_child += 1;
        self.st.scopes[parent].children[idx]
    }

    /// Build the closure tuple for a child scope from the current scope's cells.
    fn closure_for(&mut self, child: usize) -> LResult<ValueRef> {
        let frees = self.st.scopes[child].freevars.clone();
        if frees.is_empty() { return Ok(self.null()); }
        let n = self.i64c(frees.len() as i64);
        let t = self.call("PyTuple_New", &[n]);
        self.check_null(t);
        for (i, name) in frees.iter().enumerate() {
            let Some(cell_slot) = self.f().cells.get(name).copied() else { return err(format!("internal: no cell '{name}' for closure"), Span::default()) };
            let cell = self.load(cell_slot);
            self.incref(cell);
            let idx = self.i64c(i as i64);
            self.call("PyTuple_SetItem", &[t, idx, cell]);
        }
        Ok(t)
    }

    /// Emit the native function for a scope and return the function object (owned).
    fn make_function(&mut self, name: &str, args: &Arguments, body: &[Stmt], type_params: &[TypeParam], _span: Span) -> LResult<ValueRef> {
        // defaults evaluated in the defining scope
        let defaults = if args.defaults.is_empty() { self.null() } else {
            let n = self.i64c(args.defaults.len() as i64);
            let t = self.call("PyTuple_New", &[n]);
            self.check_null(t);
            for (i, d) in args.defaults.iter().enumerate() { let v = self.expr(d)?; let idx = self.i64c(i as i64); self.call("PyTuple_SetItem", &[t, idx, v]); }
            t
        };
        let kwdefaults = if args.kw_defaults.iter().all(|d| d.is_none()) { self.null() } else {
            let d = self.call("PyDict_New", &[]);
            self.check_null(d);
            for (arg, dv) in args.kwonlyargs.iter().zip(&args.kw_defaults) {
                if let Some(dv) = dv { let v = self.expr(dv)?; let n = self.name_const(&arg.arg); let r = self.call("PyDict_SetItem", &[d, n, v]); self.decref(v); self.check_neg(r); }
            }
            d
        };
        // Default expressions can contain lambdas and comprehensions. Their
        // scopes precede the function's own scope in the symbol table.
        let child = self.next_child_scope();
        let scope = &self.st.scopes[child];
        let closure = self.closure_for(child)?;
        let qualname = self.qualname_for(name);
        let llfn = if scope.is_generator || scope.is_coroutine {
            let kind = if scope.is_generator && scope.is_coroutine { 2 } else if scope.is_coroutine { 1 } else { 0 };
            self.emit_generator_body(child, name, &qualname, args, body, kind)?
        } else {
            self.emit_function_body(child, name, &qualname, args, body)?
        };
        // function object
        let name_c = self.name_const(name);
        let qual_c = self.name_const(&qualname);
        let g = self.globals_ptr();
        let params = self.st.scopes[child].params.clone();
        let argnames = self.names_tuple(&params);
        let posonly = self.i32c(args.posonlyargs.len() as i64);
        let argcount = self.i32c((args.posonlyargs.len() + args.args.len()) as i64);
        let kwonly = self.i32c(args.kwonlyargs.len() as i64);
        let async_generator = scope.is_generator && scope.is_coroutine;
        let flags = self.i32c((args.vararg.is_some() as i64) | ((args.kwarg.is_some() as i64) << 1)
            | ((!async_generator && scope.is_generator) as i64) << 2 | ((!async_generator && scope.is_coroutine) as i64) << 3
            | (async_generator as i64) << 4);
        let f = self.call("piper_function_new", &[llfn, name_c, qual_c, g, argnames, posonly, argcount, kwonly, flags, defaults, kwdefaults, closure]);
        if !args.defaults.is_empty() { self.decref(defaults); }
        if !args.kw_defaults.iter().all(|d| d.is_none()) { self.decref(kwdefaults); }
        if !self.st.scopes[child].freevars.is_empty() { self.decref(closure); }
        self.check_null(f);
        if !type_params.is_empty() {
            let params = self.type_params_tuple(type_params)?;
            let attribute = self.name_const("__type_params__");
            let result = self.call("PyObject_SetAttr", &[f, attribute, params]);
            self.decref(params);
            self.check_neg(result);
        }
        Ok(f)
    }

    fn type_params_tuple(&mut self, type_params: &[TypeParam]) -> LResult<ValueRef> {
        let count = self.i64c(type_params.len() as i64);
        let params = self.call("PyTuple_New", &[count]);
        self.check_null(params);
        for (index, parameter) in type_params.iter().enumerate() {
            let (parameter_name, kind, bound_expr, default_expr) = match &parameter.kind {
                TypeParamKind::TypeVar { name, bound, default_value } => (name, 0, bound.as_deref(), default_value.as_deref()),
                TypeParamKind::ParamSpec { name, default_value } => (name, 1, None, default_value.as_deref()),
                TypeParamKind::TypeVarTuple { name, default_value } => (name, 2, None, default_value.as_deref()),
            };
            let name = self.name_const(parameter_name);
            let kind = self.i32c(kind);
            let bound = match bound_expr { Some(expression) => self.expr(expression)?, None => self.null() };
            let default_value = match default_expr { Some(expression) => self.expr(expression)?, None => self.null() };
            let object = self.call("piper_type_param_new", &[name, kind, bound, default_value]);
            if bound_expr.is_some() { self.decref(bound); }
            if default_expr.is_some() { self.decref(default_value); }
            self.check_null(object);
            let index = self.i64c(index as i64);
            self.call("PyTuple_SetItem", &[params, index, object]);
        }
        Ok(params)
    }

    fn qualname_for(&self, name: &str) -> String {
        // Walk enclosing scopes to build a qualified name.
        let mut parts = vec![name.to_string()];
        for f in self.fstack.iter().rev() {
            let s = &self.st.scopes[f.scope];
            match s.kind {
                ScopeKind::Module => break,
                ScopeKind::Class => parts.push(s.name.clone()),
                _ => { parts.push("<locals>".into()); parts.push(s.name.clone()); }
            }
        }
        parts.reverse();
        parts.join(".")
    }

    fn native_fn_type(&self) -> TypeRef {
        unsafe {
            let mut ps = [self.ptr_t, self.ptr_t, self.i64_t, self.ptr_t];
            LLVMFunctionType(self.ptr_t, ps.as_mut_ptr(), 4, 0)
        }
    }

    fn generator_resume_type(&self) -> TypeRef {
        unsafe {
            let mut ps = [self.ptr_t, self.ptr_t, self.i32_t];
            LLVMFunctionType(self.ptr_t, ps.as_mut_ptr(), 3, 0)
        }
    }

    /// Emit a constructor with the normal function ABI and a resumable body.
    fn emit_generator_body(&mut self, scope_id: usize, name: &str, qualname: &str, _args: &Arguments, body: &[Stmt], kind: i32) -> LResult<ValueRef> {
        self.fn_counter += 1;
        let serial = self.fn_counter;
        let resume_name = CString::new(format!("piper.gen.resume.{serial}.{}", sanitize(qualname))).unwrap();
        let resume_ty = self.generator_resume_type();
        let resume_fn = unsafe { LLVMAddFunction(self.cg.module, resume_name.as_ptr(), resume_ty) };
        unsafe { LLVMSetLinkage(resume_fn, LINKAGE_INTERNAL); }
        let saved_bb = self.current_bb();
        let params = self.st.scopes[scope_id].params.clone();
        let cellvars = self.st.scopes[scope_id].cellvars.clone();
        let freevars = self.st.scopes[scope_id].freevars.clone();
        let varnames = self.st.scopes[scope_id].varnames.clone();
        let bound_slots = params.len();
        unsafe {
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"entry".as_ptr());
            let dispatch_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"dispatch".as_ptr());
            let body_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"body".as_ptr());
            let closed_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"closed".as_ptr());
            let error_exit = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"error".as_ptr());
            let return_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"ret".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let line_slot = LLVMBuildAlloca(self.b(), self.i32_t, c"line".as_ptr());
            LLVMBuildStore(self.b(), self.i32c(0), line_slot);
            let return_slot = LLVMBuildAlloca(self.b(), self.ptr_t, c"retval".as_ptr());
            LLVMBuildStore(self.b(), self.null(), return_slot);
            let object = LLVMGetParam(resume_fn, 0);
            let sent = LLVMGetParam(resume_fn, 1);
            let op = LLVMGetParam(resume_fn, 2);
            let funcobj = self.call("piper_generator_function", &[object]);
            let closing = LLVMBuildICmp(self.b(), INT_NE, op, self.i32c(0), c"closing".as_ptr());
            LLVMBuildCondBr(self.b(), closing, closed_bb, dispatch_bb);
            LLVMPositionBuilderAtEnd(self.b(), dispatch_bb);
            let state = self.call("piper_generator_state", &[object]);
            let dispatch = LLVMBuildSwitch(self.b(), state, closed_bb, 8);
            LLVMAddCase(dispatch, self.i32c(0), body_bb);
            self.fstack.push(Func {
                llfn: resume_fn, scope: scope_id, locals: HashMap::new(), cells: HashMap::new(), funcobj, has_funcobj: true,
                ns: self.null(), has_ns: false, line_slot, temp_slots: Vec::new(), handlers: Vec::new(), loops: Vec::new(), cleanups: Vec::new(),
                error_exit, return_slot, return_bb, next_child: 0, funcname: name.to_string(), super_pushed: false,
                cleanup_handler_heights: Vec::new(), generator: Some(GeneratorState { object, sent, dispatch, next_state: 0, next_slot: bound_slots as i64 }),
            });
            for (i, parameter) in params.iter().enumerate() {
                let slot = self.generator_slot_at(i as i64);
                if !cellvars.contains(parameter) { self.fm().locals.insert(parameter.clone(), slot); }
            }
            LLVMPositionBuilderAtEnd(self.b(), body_bb);
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            let globals = self.call("piper_function_globals", &[funcobj]);
            self.call("piper_frame_push", &[fs, qn, globals]);
            for variable in &varnames { if !cellvars.contains(variable) && !params.contains(variable) { self.local_slot(variable); } }
            for cname in &cellvars {
                let slot = if let Some(i) = params.iter().position(|p| p == cname) {
                    self.generator_slot_at(i as i64)
                } else { self.alloca(&format!("cell.{cname}")) };
                let initial = self.load(slot);
                let cell = self.call("piper_cell_new", &[initial]);
                self.decref(initial);
                self.store(slot, cell);
                self.fm().cells.insert(cname.clone(), slot);
            }
            for (i, fname) in freevars.iter().enumerate() {
                let slot = self.alloca(&format!("free.{fname}"));
                let index = self.i64c(i as i64);
                let cell = self.call("piper_closure_cell", &[funcobj, index]);
                self.incref(cell);
                self.store(slot, cell);
                self.fm().cells.insert(fname.clone(), slot);
            }
            self.stmts(body)?;
            if !self.terminated() {
                let none = self.call("piper_none", &[]); self.incref(none); self.store(return_slot, none); self.br(return_bb);
            }
            self.position(error_exit);
            let line = LLVMBuildLoad2(self.b(), self.i32_t, line_slot, c"line".as_ptr());
            self.call("piper_traceback_add", &[fs, qn, line]);
            self.call("piper_generator_abort", &[object]);
            self.call("piper_frame_pop", &[]);
            LLVMBuildRet(self.b(), self.null());
            self.position(return_bb);
            self.call("piper_frame_pop", &[]);
            let value = LLVMBuildLoad2(self.b(), self.ptr_t, return_slot, c"rv".as_ptr());
            let finished = self.call("piper_generator_finish", &[object, value]);
            self.decref(value);
            LLVMBuildRet(self.b(), finished);
            self.position(closed_bb);
            let is_close = LLVMBuildICmp(self.b(), INT_NE, op, self.i32c(0), c"isclose".as_ptr());
            let close_ret = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"close.ret".as_ptr());
            let exhausted = LLVMAppendBasicBlockInContext(self.cg.ctx, resume_fn, c"exhausted".as_ptr());
            LLVMBuildCondBr(self.b(), is_close, close_ret, exhausted);
            LLVMPositionBuilderAtEnd(self.b(), close_ret);
            let none = self.call("piper_none", &[]); self.incref(none); LLVMBuildRet(self.b(), none);
            LLVMPositionBuilderAtEnd(self.b(), exhausted);
            let none = self.call("piper_none", &[]);
            let done = self.call("piper_generator_finish", &[object, none]);
            LLVMBuildRet(self.b(), done);
        }
        let slots = self.f().generator.unwrap().next_slot;
        self.fstack.pop();

        let ctor_name = CString::new(format!("piper.gen.new.{serial}.{}", sanitize(qualname))).unwrap();
        let ctor_ty = self.native_fn_type();
        let ctor = unsafe { LLVMAddFunction(self.cg.module, ctor_name.as_ptr(), ctor_ty) };
        unsafe {
            LLVMSetLinkage(ctor, LINKAGE_INTERNAL);
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, ctor, c"entry".as_ptr());
            let fail = LLVMAppendBasicBlockInContext(self.cg.ctx, ctor, c"fail".as_ptr());
            let ok = LLVMAppendBasicBlockInContext(self.cg.ctx, ctor, c"ok".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let arr_ty = LLVMArrayType2(self.ptr_t, bound_slots.max(1) as u64);
            let bound = LLVMBuildAlloca(self.b(), arr_ty, c"bound".as_ptr());
            let func = LLVMGetParam(ctor, 0);
            let bind = self.call("piper_bind_args", &[func, LLVMGetParam(ctor, 1), LLVMGetParam(ctor, 2), LLVMGetParam(ctor, 3), bound]);
            let bad = self.is_neg(bind);
            LLVMBuildCondBr(self.b(), bad, fail, ok);
            LLVMPositionBuilderAtEnd(self.b(), fail); LLVMBuildRet(self.b(), self.null());
            LLVMPositionBuilderAtEnd(self.b(), ok);
            let generator = self.call("piper_generator_new", &[func, resume_fn, self.i64c(slots), self.i32c(kind as i64)]);
            let made = self.bb_for(ctor, "made");
            let alloc_fail = self.bb_for(ctor, "alloc.fail");
            let null = self.is_null(generator); LLVMBuildCondBr(self.b(), null, alloc_fail, made);
            LLVMPositionBuilderAtEnd(self.b(), alloc_fail);
            for i in 0..bound_slots { let mut idx = [self.i64c(0), self.i64c(i as i64)]; let p = LLVMBuildGEP2(self.b(), arr_ty, bound, idx.as_mut_ptr(), 2, c"arg".as_ptr()); let v = LLVMBuildLoad2(self.b(), self.ptr_t, p, c"v".as_ptr()); self.decref(v); }
            LLVMBuildRet(self.b(), self.null());
            LLVMPositionBuilderAtEnd(self.b(), made);
            for i in 0..bound_slots { let mut idx = [self.i64c(0), self.i64c(i as i64)]; let p = LLVMBuildGEP2(self.b(), arr_ty, bound, idx.as_mut_ptr(), 2, c"arg".as_ptr()); let v = LLVMBuildLoad2(self.b(), self.ptr_t, p, c"v".as_ptr()); let slot = self.call("piper_generator_slot", &[generator, self.i64c(i as i64)]); LLVMBuildStore(self.b(), v, slot); }
            LLVMBuildRet(self.b(), generator);
        }
        self.position(saved_bb);
        Ok(ctor)
    }

    fn bb_for(&self, function: ValueRef, name: &str) -> BasicBlockRef {
        let name = CString::new(name).unwrap();
        unsafe { LLVMAppendBasicBlockInContext(self.cg.ctx, function, name.as_ptr()) }
    }

    /// Emit `ptr f(ptr func, ptr args, i64 nargs, ptr kwnames)` for a function scope.
    fn emit_function_body(&mut self, scope_id: usize, name: &str, qualname: &str, args: &Arguments, body: &[Stmt]) -> LResult<ValueRef> {
        self.fn_counter += 1;
        let llname = CString::new(format!("piper.fn.{}.{}", self.fn_counter, sanitize(qualname))).unwrap();
        let fty = self.native_fn_type();
        let llfn = unsafe { LLVMAddFunction(self.cg.module, llname.as_ptr(), fty) };
        unsafe { LLVMSetLinkage(llfn, LINKAGE_INTERNAL); }
        let saved_bb = self.current_bb();
        unsafe {
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"entry".as_ptr());
            let body_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"body".as_ptr());
            let error_exit = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"error".as_ptr());
            let return_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"ret".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let line_slot = LLVMBuildAlloca(self.b(), self.i32_t, c"line".as_ptr());
            LLVMBuildStore(self.b(), self.i32c(0), line_slot);
            let return_slot = LLVMBuildAlloca(self.b(), self.ptr_t, c"retval".as_ptr());
            LLVMBuildStore(self.b(), self.null(), return_slot);
            let funcobj = LLVMGetParam(llfn, 0);
            self.fstack.push(Func {
                llfn, scope: scope_id, locals: HashMap::new(), cells: HashMap::new(), funcobj, has_funcobj: true, ns: self.null(), has_ns: false, line_slot,
                temp_slots: Vec::new(), handlers: Vec::new(), loops: Vec::new(), cleanups: Vec::new(), error_exit, return_slot, return_bb,
                next_child: 0, funcname: name.to_string(), super_pushed: false, cleanup_handler_heights: Vec::new(), generator: None,
            });
            let scope = &self.st.scopes[scope_id];
            let params = scope.params.clone();
            let cellvars = scope.cellvars.clone();
            let freevars = scope.freevars.clone();
            let varnames = scope.varnames.clone();
            // bind arguments (before frame push: binding errors belong to the caller's line)
            let nslots = params.len();
            let slots = self.ptr_array(&vec![self.null(); nslots]);
            let arr_t = LLVMArrayType2(self.ptr_t, nslots.max(1) as u64);
            let a_args = LLVMGetParam(llfn, 1);
            let a_nargs = LLVMGetParam(llfn, 2);
            let a_kw = LLVMGetParam(llfn, 3);
            let bound = self.call("piper_bind_args", &[funcobj, a_args, a_nargs, a_kw, slots]);
            let bind_ok = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"bind.ok".as_ptr());
            let bind_fail = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"bind.fail".as_ptr());
            let c = self.is_neg(bound);
            self.cond_br(c, bind_fail, bind_ok);
            LLVMPositionBuilderAtEnd(self.b(), bind_fail);
            LLVMBuildRet(self.b(), self.null());
            LLVMPositionBuilderAtEnd(self.b(), bind_ok);
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            let g = self.call("piper_function_globals", &[funcobj]);
            self.call("piper_frame_push", &[fs, qn, g]);
            // locals and cells
            for v in &varnames { if !cellvars.contains(v) { self.local_slot(v); } }
            for cname in &cellvars {
                let slot = self.alloca(&format!("cell.{cname}"));
                let init = if params.contains(cname) { let i = params.iter().position(|p| p == cname).unwrap(); let mut idx = [self.i64c(0), self.i64c(i as i64)]; let p = LLVMBuildGEP2(self.b(), arr_t, slots, idx.as_mut_ptr(), 2, c"slot".as_ptr()); let v = LLVMBuildLoad2(self.b(), self.ptr_t, p, c"arg".as_ptr()); let cell = self.call("piper_cell_new", &[v]); self.decref(v); cell } else { let nul = self.null(); self.call("piper_cell_new", &[nul]) };
                self.store(slot, init);
                self.fm().cells.insert(cname.clone(), slot);
            }
            for (i, fname) in freevars.iter().enumerate() {
                let slot = self.alloca(&format!("free.{fname}"));
                let idx = self.i64c(i as i64);
                let cell = self.call("piper_closure_cell", &[funcobj, idx]);
                self.incref(cell);
                self.store(slot, cell);
                self.fm().cells.insert(fname.clone(), slot);
            }
            for (i, p) in params.iter().enumerate() {
                if cellvars.contains(p) { continue; }
                let mut idx = [self.i64c(0), self.i64c(i as i64)];
                let ptr = LLVMBuildGEP2(self.b(), arr_t, slots, idx.as_mut_ptr(), 2, c"slot".as_ptr());
                let v = LLVMBuildLoad2(self.b(), self.ptr_t, ptr, c"arg".as_ptr());
                let ls = self.local_slot(p);
                self.store(ls, v);
            }
            // zero-arg super support
            if freevars.contains(&"__class__".to_string()) && !params.is_empty() {
                let cs = self.f().cells["__class__"];
                let cell = self.load(cs);
                let first = if cellvars.contains(&params[0]) { let cs2 = self.f().cells[&params[0]]; let c2 = self.load(cs2); let n = self.name_const(&params[0]); let z = self.i32c(0); let v = self.call("piper_cell_get", &[c2, n, z]); self.decref(v); v } else { let ls = self.f().locals[&params[0]]; self.load(ls) };
                self.call("piper_super_push", &[cell, first]);
                self.fm().super_pushed = true;
            }
            LLVMBuildBr(self.b(), body_bb);
            LLVMPositionBuilderAtEnd(self.b(), body_bb);
            self.stmts(body)?;
            if !self.terminated() {
                let none = self.call("piper_none", &[]);
                self.incref(none);
                self.store(return_slot, none);
                LLVMBuildBr(self.b(), return_bb);
            }
            // error exit: traceback entry, cleanup, return null
            LLVMPositionBuilderAtEnd(self.b(), error_exit);
            let line = LLVMBuildLoad2(self.b(), self.i32_t, line_slot, c"line".as_ptr());
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            self.call("piper_traceback_add", &[fs, qn, line]);
            self.emit_function_cleanup();
            LLVMBuildRet(self.b(), self.null());
            LLVMPositionBuilderAtEnd(self.b(), return_bb);
            self.emit_function_cleanup();
            let rv = LLVMBuildLoad2(self.b(), self.ptr_t, return_slot, c"rv".as_ptr());
            LLVMBuildRet(self.b(), rv);
            let _ = args;
        }
        self.fstack.pop();
        self.position(saved_bb);
        Ok(llfn)
    }

    fn emit_function_cleanup(&mut self) {
        if self.f().super_pushed { self.call("piper_super_pop", &[]); }
        let locals: Vec<ValueRef> = self.f().locals.values().copied().collect();
        let cells: Vec<ValueRef> = self.f().cells.values().copied().collect();
        let temps = self.f().temp_slots.clone();
        for s in locals.into_iter().chain(cells).chain(temps) { let v = self.load(s); self.call("Py_DecRef", &[v]); }
        self.call("piper_frame_pop", &[]);
    }

    // ---- classes ---------------------------------------------------------------------

    fn make_class(&mut self, name: &str, bases: &[Expr], keywords: &[Keyword], body: &[Stmt], type_params: &[TypeParam], span: Span) -> LResult<ValueRef> {
        let child = self.next_child_scope();
        let closure = self.closure_for(child)?;
        let qualname = self.qualname_for(name);
        let llfn = self.emit_class_body(child, name, &qualname, body, span)?;
        let name_c = self.name_const(name);
        let qual_c = self.name_const(&qualname);
        let g = self.globals_ptr();
        let argnames = self.names_tuple(&[".ns".to_string()]);
        let zero = self.i32c(0);
        let one = self.i32c(1);
        let nul = self.null();
        let bodyfn = self.call("piper_function_new", &[llfn, name_c, qual_c, g, argnames, zero, one, zero, zero, nul, nul, closure]);
        if !self.st.scopes[child].freevars.is_empty() { self.decref(closure); }
        self.check_null(bodyfn);
        // bases tuple (starred allowed)
        let bl = self.list_display(bases)?;
        let bt = self.call("piper_list_to_tuple", &[bl]);
        self.decref(bl);
        self.check_null(bt);
        let kw = if keywords.is_empty() { self.null() } else {
            let d = self.call("PyDict_New", &[]);
            self.check_null(d);
            for k in keywords {
                let v = self.expr(&k.value)?;
                let r = match &k.arg { Some(n) => { let nc = self.name_const(n); self.call("PyDict_SetItem", &[d, nc, v]) } None => self.call("piper_dict_update", &[d, v]) };
                self.decref(v);
                self.check_neg(r);
            }
            d
        };
        let cls = self.call("piper_make_class", &[bodyfn, name_c, bt, kw, g]);
        self.decref(bodyfn); self.decref(bt);
        if !keywords.is_empty() { self.decref(kw); }
        self.check_null(cls);
        if !type_params.is_empty() {
            let params = self.type_params_tuple(type_params)?;
            let attribute = self.name_const("__type_params__");
            let result = self.call("PyObject_SetAttr", &[cls, attribute, params]);
            self.decref(params);
            self.check_neg(result);
        }
        Ok(cls)
    }

    fn emit_class_body(&mut self, scope_id: usize, name: &str, qualname: &str, body: &[Stmt], span: Span) -> LResult<ValueRef> {
        let _ = span;
        self.fn_counter += 1;
        let llname = CString::new(format!("piper.class.{}.{}", self.fn_counter, sanitize(qualname))).unwrap();
        let fty = self.native_fn_type();
        let llfn = unsafe { LLVMAddFunction(self.cg.module, llname.as_ptr(), fty) };
        unsafe { LLVMSetLinkage(llfn, LINKAGE_INTERNAL); }
        let saved_bb = self.current_bb();
        unsafe {
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"entry".as_ptr());
            let body_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"body".as_ptr());
            let error_exit = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"error".as_ptr());
            let return_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"ret".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let line_slot = LLVMBuildAlloca(self.b(), self.i32_t, c"line".as_ptr());
            LLVMBuildStore(self.b(), self.i32c(0), line_slot);
            let return_slot = LLVMBuildAlloca(self.b(), self.ptr_t, c"retval".as_ptr());
            LLVMBuildStore(self.b(), self.null(), return_slot);
            let funcobj = LLVMGetParam(llfn, 0);
            let a_args = LLVMGetParam(llfn, 1);
            // ns is args[0]
            let ns = LLVMBuildLoad2(self.b(), self.ptr_t, a_args, c"ns".as_ptr());
            self.fstack.push(Func {
                llfn, scope: scope_id, locals: HashMap::new(), cells: HashMap::new(), funcobj, has_funcobj: true, ns, has_ns: true, line_slot,
                temp_slots: Vec::new(), handlers: Vec::new(), loops: Vec::new(), cleanups: Vec::new(), error_exit, return_slot, return_bb,
                next_child: 0, funcname: name.to_string(), super_pushed: false, cleanup_handler_heights: Vec::new(), generator: None,
            });
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            let g = self.call("piper_function_globals", &[funcobj]);
            self.call("piper_frame_push", &[fs, qn, g]);
            let freevars = self.st.scopes[scope_id].freevars.clone();
            let cellvars = self.st.scopes[scope_id].cellvars.clone();
            for (i, fname) in freevars.iter().enumerate() {
                let slot = self.alloca(&format!("free.{fname}"));
                let idx = self.i64c(i as i64);
                let cell = self.call("piper_closure_cell", &[funcobj, idx]);
                self.incref(cell);
                self.store(slot, cell);
                self.fm().cells.insert(fname.clone(), slot);
            }
            for cname in &cellvars {
                let slot = self.alloca(&format!("cell.{cname}"));
                let nul = self.null();
                let cell = self.call("piper_cell_new", &[nul]);
                self.store(slot, cell);
                self.fm().cells.insert(cname.clone(), slot);
            }
            // __module__ and __qualname__
            let g = self.globals_ptr();
            let b = self.builtins_ptr();
            let nm = self.name_const("__name__");
            let modname = self.call("piper_load_global", &[g, b, nm]);
            self.check_null(modname);
            let k = self.name_const("__module__");
            let r = self.call("PyObject_SetItem", &[ns, k, modname]);
            self.decref(modname);
            self.check_neg(r);
            let qc = self.name_const(qualname);
            let k2 = self.name_const("__qualname__");
            let r = self.call("PyObject_SetItem", &[ns, k2, qc]);
            self.check_neg(r);
            LLVMBuildBr(self.b(), body_bb);
            LLVMPositionBuilderAtEnd(self.b(), body_bb);
            self.stmts(body)?;
            if !self.terminated() {
                // return the __class__ cell if any
                let rv = if let Some(cs) = self.f().cells.get("__class__").copied() {
                    let cell = self.load(cs);
                    let k = self.name_const("__classcell__");
                    let r = self.call("PyObject_SetItem", &[ns, k, cell]);
                    self.check_neg(r);
                    self.incref(cell);
                    cell
                } else { let n = self.call("piper_none", &[]); self.incref(n); n };
                self.store(return_slot, rv);
                LLVMBuildBr(self.b(), return_bb);
            }
            LLVMPositionBuilderAtEnd(self.b(), error_exit);
            let line = LLVMBuildLoad2(self.b(), self.i32_t, line_slot, c"line".as_ptr());
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            self.call("piper_traceback_add", &[fs, qn, line]);
            self.emit_function_cleanup();
            LLVMBuildRet(self.b(), self.null());
            LLVMPositionBuilderAtEnd(self.b(), return_bb);
            self.emit_function_cleanup();
            let rv = LLVMBuildLoad2(self.b(), self.ptr_t, return_slot, c"rv".as_ptr());
            LLVMBuildRet(self.b(), rv);
        }
        self.fstack.pop();
        self.position(saved_bb);
        Ok(llfn)
    }

    // ---- comprehensions --------------------------------------------------------------

    fn generator_comprehension(&mut self, elt: &Expr, generators: &[Comprehension], span: Span) -> LResult<ValueRef> {
        let is_async = generators.iter().any(|generator| generator.is_async);
        let outer = self.expr(&generators[0].iter)?;
        let iterator = if generators[0].is_async { self.call("piper_async_iter", &[outer]) } else { self.call("PyObject_GetIter", &[outer]) };
        self.decref(outer);
        self.check_null(iterator);
        let child = self.next_child_scope();
        let closure = self.closure_for(child)?;
        let name = "<genexpr>";
        let qualname = self.qualname_for(name);
        let mut nested = vec![Stmt { kind: StmtKind::Expr { value: Box::new(Expr { kind: ExprKind::Yield { value: Some(Box::new(elt.clone())) }, span: elt.span }) }, span: elt.span }];
        for (index, generator) in generators.iter().enumerate().rev() {
            for condition in generator.ifs.iter().rev() {
                nested = vec![Stmt { kind: StmtKind::If { test: Box::new(condition.clone()), body: nested, orelse: Vec::new() }, span: condition.span }];
            }
            let iter = if index == 0 { Expr { kind: ExprKind::Name { id: ".0".into(), ctx: ExprContext::Load }, span } } else { generator.iter.clone() };
            let kind = if generator.is_async {
                StmtKind::AsyncFor { target: Box::new(generator.target.clone()), iter: Box::new(iter), body: nested, orelse: Vec::new(), type_comment: None }
            } else {
                StmtKind::For { target: Box::new(generator.target.clone()), iter: Box::new(iter), body: nested, orelse: Vec::new(), type_comment: None }
            };
            nested = vec![Stmt { kind, span: generator.target.span }];
        }
        let arguments = Arguments { args: vec![Arg { arg: ".0".into(), annotation: None, type_comment: None, span }], ..Arguments::default() };
        let native = self.emit_generator_body(child, name, &qualname, &arguments, &nested, if is_async { 2 } else { 0 })?;
        let name_object = self.name_const(name);
        let qualname_object = self.name_const(&qualname);
        let globals = self.globals_ptr();
        let argnames = self.names_tuple(&[".0".to_string()]);
        let zero = self.i32c(0); let one = self.i32c(1); let generator_flag = self.i32c(if is_async { 16 } else { 4 }); let null = self.null();
        let function = self.call("piper_function_new", &[native, name_object, qualname_object, globals, argnames, zero, one, zero, generator_flag, null, null, closure]);
        if !self.st.scopes[child].freevars.is_empty() { self.decref(closure); }
        self.check_null(function);
        let argv = self.ptr_array(&[iterator]);
        let result = self.call("piper_call", &[function, argv, self.i64c(1), null]);
        self.decref(function); self.decref(iterator);
        self.check_null(result);
        Ok(result)
    }

    fn async_comprehension(&mut self, kind: CompKind, elt: &Expr, value: Option<&Expr>, generators: &[Comprehension], span: Span) -> LResult<ValueRef> {
        if self.f().generator.is_none() { return err("asynchronous comprehension outside async function", span); }
        let yielded = if kind == CompKind::Dict {
            Expr { kind: ExprKind::Tuple { elts: vec![elt.clone(), value.unwrap().clone()], ctx: ExprContext::Load }, span }
        } else { elt.clone() };
        let generator = self.generator_comprehension(&yielded, generators, span)?;
        let generator_slot = self.alloca("asynccomp.generator"); self.store(generator_slot, generator); self.fm().temp_slots.push(generator_slot);
        let accumulator = match kind {
            CompKind::List => { let zero = self.i64c(0); self.call("PyList_New", &[zero]) }
            CompKind::Set => { let null = self.null(); self.call("PySet_New", &[null]) }
            CompKind::Dict => self.call("PyDict_New", &[]),
        };
        self.check_null(accumulator);
        let accumulator_slot = self.alloca("asynccomp.accumulator"); self.store(accumulator_slot, accumulator); self.fm().temp_slots.push(accumulator_slot);
        let next = self.bb("asynccomp.next"); let body = self.bb("asynccomp.body"); let stop = self.bb("asynccomp.stop"); let end = self.bb("asynccomp.end");
        self.br(next);
        self.position(next);
        self.fm().handlers.push(Handler::Block(stop));
        let generator = self.load(generator_slot);
        let awaitable = self.call("piper_async_next", &[generator]); self.check_null(awaitable);
        let iterator = self.call("piper_await_iter", &[awaitable]); self.decref(awaitable); self.check_null(iterator);
        let item = self.yield_from_iterator(iterator)?;
        self.fm().handlers.pop(); self.br(body);
        self.position(body);
        let accumulator = self.load(accumulator_slot);
        match kind {
            CompKind::List => { let rc = self.call("PyList_Append", &[accumulator, item]); self.decref(item); self.check_neg(rc); }
            CompKind::Set => { let rc = self.call("piper_set_add", &[accumulator, item]); self.decref(item); self.check_neg(rc); }
            CompKind::Dict => {
                let zero = self.constant(&Constant::Int("0".into())); let one = self.constant(&Constant::Int("1".into()));
                let key = self.call("PyObject_GetItem", &[item, zero]); let value = self.call("PyObject_GetItem", &[item, one]);
                self.decref(zero); self.decref(one); self.check_null(key); self.check_null(value);
                let rc = self.call("PyDict_SetItem", &[accumulator, key, value]);
                self.decref(key); self.decref(value); self.decref(item); self.check_neg(rc);
            }
        }
        self.br(next);
        self.position(stop);
        let done = self.call("piper_async_iteration_done", &[]);
        let finished = unsafe { LLVMBuildICmp(self.b(), INT_NE, done, self.i32c(0), c"asyncdone".as_ptr()) };
        let outer = self.error_target(); self.cond_br(finished, end, outer);
        self.position(end);
        let generator = self.load(generator_slot); self.store(generator_slot, self.null()); self.decref(generator);
        let result = self.load(accumulator_slot); self.store(accumulator_slot, self.null());
        Ok(result)
    }

    fn comprehension(&mut self, kind: CompKind, elt: &Expr, value: Option<&Expr>, generators: &[Comprehension], span: Span) -> LResult<ValueRef> {
        // Outermost iterable evaluated here; the rest runs in a nested function.
        let outer_iter = self.expr(&generators[0].iter)?;
        let it = self.call("PyObject_GetIter", &[outer_iter]);
        self.decref(outer_iter);
        self.check_null(it);
        let child = self.next_child_scope();
        let closure = self.closure_for(child)?;
        let name = match kind { CompKind::List => "<listcomp>", CompKind::Set => "<setcomp>", CompKind::Dict => "<dictcomp>" };
        let qualname = self.qualname_for(name);
        let llfn = self.emit_comprehension_body(child, kind, name, &qualname, elt, value, generators, span)?;
        let name_c = self.name_const(name);
        let qual_c = self.name_const(&qualname);
        let g = self.globals_ptr();
        let argnames = self.names_tuple(&[".0".to_string()]);
        let zero = self.i32c(0);
        let one = self.i32c(1);
        let nul = self.null();
        let f = self.call("piper_function_new", &[llfn, name_c, qual_c, g, argnames, zero, one, zero, zero, nul, nul, closure]);
        if !self.st.scopes[child].freevars.is_empty() { self.decref(closure); }
        self.check_null(f);
        let arr = self.ptr_array(&[it]);
        let n1 = self.i64c(1);
        let r = self.call("piper_call", &[f, arr, n1, nul]);
        self.decref(f); self.decref(it);
        self.check_null(r);
        Ok(r)
    }

    #[allow(clippy::too_many_arguments)]
    fn emit_comprehension_body(&mut self, scope_id: usize, kind: CompKind, name: &str, qualname: &str, elt: &Expr, value: Option<&Expr>, generators: &[Comprehension], span: Span) -> LResult<ValueRef> {
        let _ = span;
        self.fn_counter += 1;
        let llname = CString::new(format!("piper.comp.{}.{}", self.fn_counter, sanitize(qualname))).unwrap();
        let fty = self.native_fn_type();
        let llfn = unsafe { LLVMAddFunction(self.cg.module, llname.as_ptr(), fty) };
        unsafe { LLVMSetLinkage(llfn, LINKAGE_INTERNAL); }
        let saved_bb = self.current_bb();
        unsafe {
            let entry = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"entry".as_ptr());
            let body_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"body".as_ptr());
            let error_exit = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"error".as_ptr());
            let return_bb = LLVMAppendBasicBlockInContext(self.cg.ctx, llfn, c"ret".as_ptr());
            LLVMPositionBuilderAtEnd(self.b(), entry);
            let line_slot = LLVMBuildAlloca(self.b(), self.i32_t, c"line".as_ptr());
            LLVMBuildStore(self.b(), self.i32c(0), line_slot);
            let return_slot = LLVMBuildAlloca(self.b(), self.ptr_t, c"retval".as_ptr());
            LLVMBuildStore(self.b(), self.null(), return_slot);
            let funcobj = LLVMGetParam(llfn, 0);
            let a_args = LLVMGetParam(llfn, 1);
            self.fstack.push(Func {
                llfn, scope: scope_id, locals: HashMap::new(), cells: HashMap::new(), funcobj, has_funcobj: true, ns: self.null(), has_ns: false, line_slot,
                temp_slots: Vec::new(), handlers: Vec::new(), loops: Vec::new(), cleanups: Vec::new(), error_exit, return_slot, return_bb,
                next_child: 0, funcname: name.to_string(), super_pushed: false, cleanup_handler_heights: Vec::new(), generator: None,
            });
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            let g = self.call("piper_function_globals", &[funcobj]);
            self.call("piper_frame_push", &[fs, qn, g]);
            let freevars = self.st.scopes[scope_id].freevars.clone();
            let cellvars = self.st.scopes[scope_id].cellvars.clone();
            let varnames = self.st.scopes[scope_id].varnames.clone();
            for v in &varnames { if !cellvars.contains(v) { self.local_slot(v); } }
            for (i, fname) in freevars.iter().enumerate() {
                let slot = self.alloca(&format!("free.{fname}"));
                let idx = self.i64c(i as i64);
                let cell = self.call("piper_closure_cell", &[funcobj, idx]);
                self.incref(cell);
                self.store(slot, cell);
                self.fm().cells.insert(fname.clone(), slot);
            }
            for cname in &cellvars {
                let slot = self.alloca(&format!("cell.{cname}"));
                let nul = self.null();
                let cell = self.call("piper_cell_new", &[nul]);
                self.store(slot, cell);
                self.fm().cells.insert(cname.clone(), slot);
            }
            // .0 holds the outermost iterator (owned by the caller; incref for our slot)
            let it0 = LLVMBuildLoad2(self.b(), self.ptr_t, a_args, c"it0".as_ptr());
            self.incref(it0);
            let dot0 = self.local_slot(".0");
            self.store(dot0, it0);
            LLVMBuildBr(self.b(), body_bb);
            LLVMPositionBuilderAtEnd(self.b(), body_bb);
            // accumulator
            let acc = match kind {
                CompKind::List => { let z = self.i64c(0); self.call("PyList_New", &[z]) }
                CompKind::Set => { let nul = self.null(); self.call("PySet_New", &[nul]) }
                CompKind::Dict => self.call("PyDict_New", &[]),
            };
            self.check_null(acc);
            let acc_slot = self.alloca("comp.acc");
            self.store(acc_slot, acc);
            self.fm().temp_slots.push(acc_slot);
            self.comp_loops(kind, elt, value, generators, 0, acc_slot)?;
            let result = self.load(acc_slot);
            self.store(acc_slot, self.null());
            self.store(return_slot, result);
            LLVMBuildBr(self.b(), return_bb);
            LLVMPositionBuilderAtEnd(self.b(), error_exit);
            let line = LLVMBuildLoad2(self.b(), self.i32_t, line_slot, c"line".as_ptr());
            let fs = self.file_str;
            let qn = self.cstring_global(qualname);
            self.call("piper_traceback_add", &[fs, qn, line]);
            self.emit_function_cleanup();
            LLVMBuildRet(self.b(), self.null());
            LLVMPositionBuilderAtEnd(self.b(), return_bb);
            self.emit_function_cleanup();
            let rv = LLVMBuildLoad2(self.b(), self.ptr_t, return_slot, c"rv".as_ptr());
            LLVMBuildRet(self.b(), rv);
        }
        self.fstack.pop();
        self.position(saved_bb);
        Ok(llfn)
    }

    fn comp_loops(&mut self, kind: CompKind, elt: &Expr, value: Option<&Expr>, generators: &[Comprehension], depth: usize, acc_slot: ValueRef) -> LResult<()> {
        if depth == generators.len() {
            let acc = self.load(acc_slot);
            match kind {
                CompKind::List => { let v = self.expr(elt)?; let r = self.call("PyList_Append", &[acc, v]); self.decref(v); self.check_neg(r); }
                CompKind::Set => { let v = self.expr(elt)?; let r = self.call("piper_set_add", &[acc, v]); self.decref(v); self.check_neg(r); }
                CompKind::Dict => { let k = self.expr(elt)?; let v = self.expr(value.unwrap())?; let r = self.call("PyDict_SetItem", &[acc, k, v]); self.decref(k); self.decref(v); self.check_neg(r); }
            }
            return Ok(());
        }
        let g = &generators[depth];
        if g.is_async { return err("async comprehensions are not supported yet", g.target.span); }
        // iterator: .0 for the first, evaluated for the rest
        let it = if depth == 0 { let s = self.f().locals[".0"]; let v = self.load(s); self.incref(v); v } else { let o = self.expr(&g.iter)?; let it = self.call("PyObject_GetIter", &[o]); self.decref(o); self.check_null(it); it };
        let slot = self.alloca("comp.iter");
        self.store(slot, it);
        self.fm().temp_slots.push(slot);
        let next_bb = self.bb("comp.next");
        let body_bb = self.bb("comp.body");
        let exhausted_bb = self.bb("comp.exhausted");
        let end_bb = self.bb("comp.end");
        self.br(next_bb);
        self.position(next_bb);
        let itv = self.load(slot);
        let item = self.call("PyIter_Next", &[itv]);
        let c = self.is_null(item);
        self.cond_br(c, exhausted_bb, body_bb);
        self.position(exhausted_bb);
        let e = self.call("PyErr_Occurred", &[]);
        let has_err = unsafe { LLVMBuildIsNotNull(self.b(), e, c"haserr".as_ptr()) };
        let err_bb = self.error_target();
        let itv2 = self.load(slot);
        self.store(slot, self.null());
        self.decref(itv2);
        self.cond_br(has_err, err_bb, end_bb);
        self.position(body_bb);
        self.assign(&g.target, item)?;
        for cond in &g.ifs {
            let then = self.bb("comp.if");
            self.cond_jump(cond, then, next_bb)?;
            self.position(then);
        }
        self.comp_loops(kind, elt, value, generators, depth + 1, acc_slot)?;
        self.br(next_bb);
        self.position(end_bb);
        Ok(())
    }

    // ---- try / with -------------------------------------------------------------------

    /// Run cleanups (finally bodies, with-exits) from the innermost down to `depth`.
    fn run_cleanups_for_exit(&mut self, depth: usize) -> LResult<()> {
        let cleanups: Vec<Cleanup> = self.f().cleanups.clone();
        let handlers_len = self.f().handlers.len();
        for (i, c) in cleanups.iter().enumerate().rev() {
            if i < depth { break; }
            // While running this cleanup, the handler installed for it must not be active.
            let saved_cleanups = self.fm().cleanups.split_off(i);
            let cut = handlers_len.min(self.handler_index_for_cleanup(i));
            let saved_handlers = self.fm().handlers.split_off(cut);
            match c {
                Cleanup::Finally(body) => { self.stmts(body)?; }
                Cleanup::With { exit_slot } => {
                    let exit = self.load(*exit_slot);
                    let nul = self.null();
                    let r = self.call("piper_with_exit", &[exit, nul]);
                    self.check_neg(r);
                }
                Cleanup::AsyncWith { exit_slot } => {
                    let exit = self.load(*exit_slot);
                    let nul = self.null();
                    let awaitable = self.call("piper_async_with_exit", &[exit, nul]);
                    self.check_null(awaitable);
                    let iterator = self.call("piper_await_iter", &[awaitable]);
                    self.decref(awaitable); self.check_null(iterator);
                    let result = self.yield_from_iterator(iterator)?;
                    self.decref(result);
                }
                Cleanup::Except => {
                    let handled = self.call("piper_exc_info_pop", &[]);
                    self.decref(handled);
                }
            }
            self.fm().handlers.extend(saved_handlers);
            self.fm().cleanups.extend(saved_cleanups);
        }
        Ok(())
    }
    /// Cleanups and handlers are pushed in lockstep; cleanup i owns handler index i (from the bottom of the try nesting).
    fn handler_index_for_cleanup(&self, i: usize) -> usize {
        // Each cleanup pushes exactly one handler; other handlers (except blocks) also push one.
        // Track by recording the handler stack height at cleanup push time.
        self.f().cleanup_handler_heights.get(i).copied().unwrap_or(0)
    }

    fn try_stmt(&mut self, body: &[Stmt], handlers: &[ExceptHandler], orelse: &[Stmt], finalbody: &[Stmt], star: bool, span: Span) -> LResult<()> {
        if !finalbody.is_empty() {
            // try/finally wrapping an inner try/except
            let handler_bb = self.bb("finally.handler");
            let normal_bb = self.bb("finally.normal");
            let end_bb = self.bb("finally.end");
            let height = self.f().handlers.len();
            self.fm().cleanup_handler_heights.push(height);
            self.fm().handlers.push(Handler::Block(handler_bb));
            self.fm().cleanups.push(Cleanup::Finally(finalbody.to_vec()));
            if handlers.is_empty() { self.stmts(body)?; } else if star { self.try_except_star(body, handlers, orelse)?; } else { self.try_except(body, handlers, orelse)?; }
            self.fm().cleanups.pop();
            self.fm().handlers.pop();
            self.fm().cleanup_handler_heights.pop();
            self.br(normal_bb);
            // error path: save exception, run finally, re-raise
            self.position(handler_bb);
            let exc = self.call("PyErr_GetRaisedException", &[]);
            let exc_slot = self.alloca("finally.exc");
            self.store(exc_slot, exc);
            self.call("piper_exc_info_push", &[exc]);
            self.stmts(finalbody)?;
            if !self.terminated() {
                let popped = self.call("piper_exc_info_pop", &[]);
                let e2 = self.load(exc_slot);
                let _ = popped;
                self.call("PyErr_SetRaisedException", &[e2]);
                let t = self.error_target();
                self.br(t);
            }
            self.position(normal_bb);
            self.stmts(finalbody)?;
            self.br(end_bb);
            self.position(end_bb);
            return Ok(());
        }
        let _ = span;
        if star { self.try_except_star(body, handlers, orelse) } else { self.try_except(body, handlers, orelse) }
    }

    fn try_except_star(&mut self, body: &[Stmt], handlers: &[ExceptHandler], orelse: &[Stmt]) -> LResult<()> {
        let dispatch = self.bb("exceptstar.dispatch");
        let else_bb = self.bb("exceptstar.else");
        let end_bb = self.bb("exceptstar.end");
        self.fm().handlers.push(Handler::Block(dispatch));
        self.stmts(body)?;
        self.fm().handlers.pop();
        self.br(else_bb);
        self.position(dispatch);
        let original = self.call("PyErr_GetRaisedException", &[]);
        let remaining = self.alloca("exceptstar.remaining");
        self.store(remaining, original);
        let handler_error = self.bb("exceptstar.handler_error");
        for handler in handlers {
            let Some(type_expr) = &handler.type_ else { return err("except* requires an exception type", handler.span) };
            let skip = self.bb("exceptstar.skip");
            let run = self.bb("exceptstar.run");
            let match_slot = self.alloca("exceptstar.match");
            let rest_slot = self.alloca("exceptstar.rest");
            let current = self.load(remaining);
            let has_current = unsafe { LLVMBuildIsNotNull(self.b(), current, c"hasremaining".as_ptr()) };
            let split = self.bb("exceptstar.split");
            self.cond_br(has_current, split, skip);
            self.position(split);
            let exception_type = self.expr(type_expr)?;
            let rc = self.call("piper_exception_group_match", &[current, exception_type, match_slot, rest_slot]);
            self.decref(exception_type);
            self.check_neg(rc);
            let rest = self.load(rest_slot);
            self.store(remaining, rest);
            self.decref(current);
            let matched = self.load(match_slot);
            let has_match = unsafe { LLVMBuildIsNotNull(self.b(), matched, c"hasmatch".as_ptr()) };
            self.cond_br(has_match, run, skip);
            self.position(run);
            self.call("piper_exc_info_push", &[matched]);
            self.fm().handlers.push(Handler::Block(handler_error));
            if let Some(name) = &handler.name { self.incref(matched); self.store_name(name, matched)?; }
            self.stmts(&handler.body)?;
            self.fm().handlers.pop();
            if !self.terminated() {
                if let Some(name) = &handler.name {
                    if matches!(self.scope().kind, ScopeKind::Module | ScopeKind::Class) || !self.scope().is_local(name) {
                        let none = self.call("piper_none", &[]); self.incref(none); self.store_name(name, none)?; self.delete_name(name, handler.span)?;
                    } else {
                        let slot = self.local_slot(name); let value = self.load(slot); self.store(slot, self.null()); self.decref(value);
                    }
                }
                let handled = self.call("piper_exc_info_pop", &[]); self.decref(handled); self.br(skip);
            }
            self.position(skip);
        }
        let rest = self.load(remaining);
        let has_rest = unsafe { LLVMBuildIsNotNull(self.b(), rest, c"hasrest".as_ptr()) };
        let reraise = self.bb("exceptstar.reraise");
        self.cond_br(has_rest, reraise, end_bb);
        self.position(reraise);
        self.call("PyErr_SetRaisedException", &[rest]);
        let outer = self.error_target(); self.br(outer);
        self.position(handler_error);
        let handled = self.call("piper_exc_info_pop", &[]); self.decref(handled);
        let raised = self.call("PyErr_GetRaisedException", &[]);
        let leftover = self.load(remaining);
        let merged = self.call("piper_exception_group_merge", &[raised, leftover]);
        self.decref(raised); self.decref(leftover);
        self.call("PyErr_SetRaisedException", &[merged]);
        let outer = self.error_target(); self.br(outer);
        self.position(else_bb); self.stmts(orelse)?; self.br(end_bb);
        self.position(end_bb);
        Ok(())
    }

    fn try_except(&mut self, body: &[Stmt], handlers: &[ExceptHandler], orelse: &[Stmt]) -> LResult<()> {
        let handler_bb = self.bb("except.dispatch");
        let else_bb = self.bb("try.else");
        let end_bb = self.bb("try.end");
        self.fm().handlers.push(Handler::Block(handler_bb));
        self.stmts(body)?;
        self.fm().handlers.pop();
        self.br(else_bb);
        // dispatch
        self.position(handler_bb);
        let exc = self.call("PyErr_GetRaisedException", &[]);
        let exc_slot = self.alloca("except.exc");
        self.store(exc_slot, exc);
        self.call("piper_exc_info_push", &[exc]);
        // While a handler body runs, errors go to a block that pops exc_info and re-routes.
        let reraise_bb = self.bb("except.reraise");
        let body_err_bb = self.bb("except.body_error");
        let mut next_test: BasicBlockRef = self.bb("except.test");
        self.br(next_test);
        for h in handlers {
            self.position(next_test);
            next_test = self.bb("except.test");
            let hbody = self.bb("except.body");
            match &h.type_ {
                Some(t) => {
                    let tv = self.expr(t)?;
                    let e = self.load(exc_slot);
                    let m = self.call("piper_exception_matches", &[e, tv]);
                    self.decref(tv);
                    // -1: error evaluating -> body_err (pops exc info)
                    let neg = self.is_neg(m);
                    let chk = self.bb("except.chk");
                    self.cond_br(neg, body_err_bb, chk);
                    self.position(chk);
                    let c = unsafe { LLVMBuildICmp(self.b(), INT_NE, m, self.i32c(0), c"matched".as_ptr()) };
                    self.cond_br(c, hbody, next_test);
                }
                None => { self.br(hbody); }
            }
            self.position(hbody);
            self.fm().handlers.push(Handler::Block(body_err_bb));
            let cleanup_height = self.f().handlers.len() - 1;
            self.fm().cleanup_handler_heights.push(cleanup_height);
            self.fm().cleanups.push(Cleanup::Except);
            if let Some(name) = &h.name {
                let e = self.load(exc_slot);
                self.incref(e);
                self.store_name(name, e)?;
            }
            self.stmts(&h.body)?;
            self.fm().handlers.pop();
            self.fm().cleanups.pop();
            self.fm().cleanup_handler_heights.pop();
            if !self.terminated() {
                if let Some(name) = &h.name {
                    // `del name` at handler end, ignoring unbound
                    let scope_kind = self.scope().kind;
                    if scope_kind == ScopeKind::Module || scope_kind == ScopeKind::Class || !self.scope().is_local(name) {
                        let nul = self.null();
                        let _ = nul;
                        let none = self.call("piper_none", &[]);
                        self.incref(none);
                        self.store_name(name, none)?;
                        self.delete_name(name, h.span)?;
                    } else {
                        let slot = self.local_slot(name);
                        let v = self.load(slot);
                        self.store(slot, self.null());
                        self.call("Py_DecRef", &[v]);
                    }
                }
                let popped = self.call("piper_exc_info_pop", &[]);
                self.call("Py_DecRef", &[popped]);
                self.br(end_bb);
            }
        }
        // no handler matched
        self.position(next_test);
        self.br(reraise_bb);
        self.position(reraise_bb);
        let popped = self.call("piper_exc_info_pop", &[]);
        let _ = popped;
        let e = self.load(exc_slot);
        self.call("PyErr_SetRaisedException", &[e]);
        let outer = self.error_target();
        self.br(outer);
        // error inside a handler body: drop the handled exception context
        self.position(body_err_bb);
        let popped = self.call("piper_exc_info_pop", &[]);
        self.call("Py_DecRef", &[popped]);
        let outer = self.error_target();
        self.br(outer);
        // else
        self.position(else_bb);
        self.stmts(orelse)?;
        self.br(end_bb);
        self.position(end_bb);
        Ok(())
    }

    fn with_stmt(&mut self, items: &[WithItem], body: &[Stmt]) -> LResult<()> {
        if items.is_empty() { return self.stmts(body); }
        let item = &items[0];
        let rest = &items[1..];
        let mgr = self.expr(&item.context_expr)?;
        let exit_slot = self.alloca("with.exit");
        let value_slot = self.alloca("with.value");
        let r = self.call("piper_with_enter", &[mgr, exit_slot, value_slot]);
        self.decref(mgr);
        self.check_neg(r);
        self.fm().temp_slots.push(exit_slot);
        let value = self.load(value_slot);
        match &item.optional_vars { Some(t) => self.assign(t, value)?, None => self.decref(value) }
        let handler_bb = self.bb("with.error");
        let normal_bb = self.bb("with.normal");
        let end_bb = self.bb("with.end");
        let height = self.f().handlers.len();
        self.fm().cleanup_handler_heights.push(height);
        self.fm().handlers.push(Handler::Block(handler_bb));
        self.fm().cleanups.push(Cleanup::With { exit_slot });
        if rest.is_empty() { self.stmts(body)?; } else { self.with_stmt(rest, body)?; }
        self.fm().cleanups.pop();
        self.fm().handlers.pop();
        self.fm().cleanup_handler_heights.pop();
        self.br(normal_bb);
        // error path
        self.position(handler_bb);
        let exc = self.call("PyErr_GetRaisedException", &[]);
        let exit = self.load(exit_slot);
        let r = self.call("piper_with_exit", &[exit, exc]);
        // r: -1 error (new exception set), 0 propagate, 1 suppress
        let neg = self.is_neg(r);
        let propagate_bb = self.bb("with.propagate");
        let suppress_bb = self.bb("with.suppress");
        let chk = self.bb("with.chk");
        let errt = self.error_target();
        self.cond_br(neg, chk, chk);
        self.position(chk);
        let neg2 = self.is_neg(r);
        let chk2 = self.bb("with.chk2");
        self.cond_br(neg2, errt, chk2);
        self.position(chk2);
        let zero = self.i32c(0);
        let is_zero = unsafe { LLVMBuildICmp(self.b(), INT_EQ, r, zero, c"propagate".as_ptr()) };
        self.cond_br(is_zero, propagate_bb, suppress_bb);
        self.position(propagate_bb);
        self.call("PyErr_SetRaisedException", &[exc]);
        let ex = self.load(exit_slot);
        self.store(exit_slot, self.null());
        self.decref(ex);
        self.br(errt);
        self.position(suppress_bb);
        self.decref(exc);
        let ex = self.load(exit_slot);
        self.store(exit_slot, self.null());
        self.decref(ex);
        self.br(end_bb);
        // normal path
        self.position(normal_bb);
        let exit = self.load(exit_slot);
        let nul = self.null();
        let r = self.call("piper_with_exit", &[exit, nul]);
        self.store(exit_slot, self.null());
        self.decref(exit);
        self.check_neg(r);
        self.br(end_bb);
        self.position(end_bb);
        Ok(())
    }

    fn async_with_stmt(&mut self, items: &[WithItem], body: &[Stmt]) -> LResult<()> {
        if items.is_empty() { return self.stmts(body); }
        if self.f().generator.is_none() { return err("'async with' outside async function", Span::default()); }
        let item = &items[0]; let rest = &items[1..];
        let manager = self.expr(&item.context_expr)?;
        let exit_slot = self.alloca("asyncwith.exit");
        let awaitable = self.call("piper_async_with_enter", &[manager, exit_slot]);
        self.decref(manager); self.check_null(awaitable);
        self.fm().temp_slots.push(exit_slot);
        let iterator = self.call("piper_await_iter", &[awaitable]); self.decref(awaitable); self.check_null(iterator);
        let entered = self.yield_from_iterator(iterator)?;
        match &item.optional_vars { Some(target) => self.assign(target, entered)?, None => self.decref(entered) }
        let handler = self.bb("asyncwith.error"); let normal = self.bb("asyncwith.normal"); let end = self.bb("asyncwith.end");
        let height = self.f().handlers.len(); self.fm().cleanup_handler_heights.push(height);
        self.fm().handlers.push(Handler::Block(handler)); self.fm().cleanups.push(Cleanup::AsyncWith { exit_slot });
        if rest.is_empty() { self.stmts(body)?; } else { self.async_with_stmt(rest, body)?; }
        self.fm().cleanups.pop(); self.fm().handlers.pop(); self.fm().cleanup_handler_heights.pop(); self.br(normal);

        self.position(handler);
        let exception = self.call("PyErr_GetRaisedException", &[]);
        let exception_slot = self.alloca("asyncwith.exception"); self.store(exception_slot, exception);
        let exit = self.load(exit_slot);
        let awaitable = self.call("piper_async_with_exit", &[exit, exception]); self.check_null(awaitable);
        let iterator = self.call("piper_await_iter", &[awaitable]); self.decref(awaitable); self.check_null(iterator);
        let suppress = self.yield_from_iterator(iterator)?;
        let truth = self.call("PyObject_IsTrue", &[suppress]); self.decref(suppress); self.check_neg(truth);
        let suppressed = self.bb("asyncwith.suppressed"); let propagate = self.bb("asyncwith.propagate");
        let condition = unsafe { LLVMBuildICmp(self.b(), INT_NE, truth, self.i32c(0), c"suppress".as_ptr()) };
        self.cond_br(condition, suppressed, propagate);
        self.position(suppressed);
        let exception = self.load(exception_slot); self.store(exception_slot, self.null()); self.decref(exception);
        let exit = self.load(exit_slot); self.store(exit_slot, self.null()); self.decref(exit); self.br(end);
        self.position(propagate);
        let exception = self.load(exception_slot); self.store(exception_slot, self.null());
        self.call("PyErr_SetRaisedException", &[exception]); let exit = self.load(exit_slot); self.store(exit_slot, self.null()); self.decref(exit);
        let outer = self.error_target(); self.br(outer);

        self.position(normal);
        let exit = self.load(exit_slot); let null = self.null();
        let awaitable = self.call("piper_async_with_exit", &[exit, null]); self.check_null(awaitable);
        let iterator = self.call("piper_await_iter", &[awaitable]); self.decref(awaitable); self.check_null(iterator);
        let result = self.yield_from_iterator(iterator)?; self.decref(result);
        let exit = self.load(exit_slot); self.store(exit_slot, self.null()); self.decref(exit); self.br(end);
        self.position(end);
        Ok(())
    }

    // ---- match ------------------------------------------------------------------------

    fn match_stmt(&mut self, subject: &Expr, cases: &[MatchCase], span: Span) -> LResult<()> {
        let subj = self.expr(subject)?;
        let subj_slot = self.alloca("match.subject");
        self.store(subj_slot, subj);
        self.fm().temp_slots.push(subj_slot);
        let end_bb = self.bb("match.end");
        for c in cases {
            let body_bb = self.bb("case.body");
            let next_bb = self.bb("case.next");
            let s = self.load(subj_slot);
            self.pattern(&c.pattern, s, body_bb, next_bb)?;
            // guard
            self.position(body_bb);
            if let Some(g) = &c.guard {
                let gb = self.bb("case.guarded");
                self.cond_jump(g, gb, next_bb)?;
                self.position(gb);
            }
            self.stmts(&c.body)?;
            self.br(end_bb);
            self.position(next_bb);
        }
        self.br(end_bb);
        self.position(end_bb);
        let s = self.load(subj_slot);
        self.store(subj_slot, self.null());
        self.decref(s);
        let _ = span;
        Ok(())
    }

    /// Emit a test of `subject` (borrowed) against `p`; jumps to `ok` on match
    /// (with captures bound) or `fail` otherwise.
    fn pattern(&mut self, p: &Pattern, subject: ValueRef, ok: BasicBlockRef, fail: BasicBlockRef) -> LResult<()> {
        match &p.kind {
            PatternKind::MatchValue { value } => {
                let v = self.expr(value)?;
                let two = self.i32c(2);
                let r = self.call("PyObject_RichCompareBool", &[subject, v, two]);
                self.decref(v);
                self.check_neg(r);
                let c = unsafe { LLVMBuildICmp(self.b(), INT_NE, r, self.i32c(0), c"eq".as_ptr()) };
                self.cond_br(c, ok, fail);
            }
            PatternKind::MatchSingleton { value } => {
                let v = self.constant(value);
                self.decref(v);
                let c = unsafe { LLVMBuildICmp(self.b(), INT_EQ, subject, v, c"is".as_ptr()) };
                self.cond_br(c, ok, fail);
            }
            PatternKind::MatchAs { pattern, name } => {
                match pattern {
                    None => {
                        if let Some(n) = name { self.incref(subject); self.store_name(n, subject)?; }
                        self.br(ok);
                    }
                    Some(inner) => {
                        let bind_bb = self.bb("as.bind");
                        self.pattern(inner, subject, bind_bb, fail)?;
                        self.position(bind_bb);
                        if let Some(n) = name { self.incref(subject); self.store_name(n, subject)?; }
                        self.br(ok);
                    }
                }
            }
            PatternKind::MatchOr { patterns } => {
                for (i, alt) in patterns.iter().enumerate() {
                    if i + 1 == patterns.len() { self.pattern(alt, subject, ok, fail)?; }
                    else { let next = self.bb("or.next"); self.pattern(alt, subject, ok, next)?; self.position(next); }
                }
            }
            PatternKind::MatchSequence { patterns } => {
                let r = self.call("piper_match_sequence_check", &[subject]);
                let is_seq = unsafe { LLVMBuildICmp(self.b(), INT_NE, r, self.i32c(0), c"isseq".as_ptr()) };
                let lenchk = self.bb("seq.len");
                self.cond_br(is_seq, lenchk, fail);
                self.position(lenchk);
                let l = self.call("PySequence_List", &[subject]);
                self.check_null(l);
                let lslot = self.alloca("seq.list");
                self.store(lslot, l);
                self.fm().temp_slots.push(lslot);
                let n = self.call("PyList_Size", &[l]);
                let star = patterns.iter().position(|q| matches!(q.kind, PatternKind::MatchStar { .. }));
                let fixed = patterns.len() - star.is_some() as usize;
                let want = self.i64c(fixed as i64);
                let c = unsafe { LLVMBuildICmp(self.b(), if star.is_some() { INT_SGE } else { INT_EQ }, n, want, c"lenok".as_ptr()) };
                let elems = self.bb("seq.elems");
                let cleanup_fail = self.bb("seq.fail");
                self.cond_br(c, elems, cleanup_fail);
                self.position(cleanup_fail);
                let lv = self.load(lslot); self.store(lslot, self.null()); self.decref(lv);
                self.br(fail);
                self.position(elems);
                for (i, q) in patterns.iter().enumerate() {
                    let lv = self.load(lslot);
                    match &q.kind {
                        PatternKind::MatchStar { name } => {
                            if let Some(nm) = name {
                                let start = self.i64c(i as i64);
                                let after = self.i64c((patterns.len() - i - 1) as i64);
                                let stop = unsafe { LLVMBuildSub(self.b(), n, after, c"stop".as_ptr()) };
                                let sub = self.call("PyList_GetSlice", &[lv, start, stop]);
                                self.check_null(sub);
                                self.store_name(nm, sub)?;
                            }
                        }
                        _ => {
                            let idx = if star.is_some_and(|s| i > s) { let back = self.i64c((patterns.len() - i) as i64); unsafe { LLVMBuildSub(self.b(), n, back, c"idx".as_ptr()) } } else { self.i64c(i as i64) };
                            let item = self.call("PyList_GetItem", &[lv, idx]);
                            let next = self.bb("seq.next");
                            self.pattern(q, item, next, cleanup_fail)?;
                            self.position(next);
                        }
                    }
                }
                let lv = self.load(lslot); self.store(lslot, self.null()); self.decref(lv);
                self.br(ok);
            }
            PatternKind::MatchMapping { keys, patterns, rest } => {
                let r = self.call("piper_match_mapping_check", &[subject]);
                let is_map = unsafe { LLVMBuildICmp(self.b(), INT_NE, r, self.i32c(0), c"ismap".as_ptr()) };
                let keychk = self.bb("map.keys");
                self.cond_br(is_map, keychk, fail);
                self.position(keychk);
                let kvals: Vec<ValueRef> = keys.iter().map(|k| self.expr(k)).collect::<LResult<_>>()?;
                let n = self.i64c(kvals.len() as i64);
                let kt = self.call("PyTuple_New", &[n]);
                for (i, kv) in kvals.iter().enumerate() { let idx = self.i64c(i as i64); self.call("PyTuple_SetItem", &[kt, idx, *kv]); }
                let values = self.call("piper_match_keys", &[subject, kt]);
                self.check_null(values);
                let none = self.call("piper_none", &[]);
                let missing = unsafe { LLVMBuildICmp(self.b(), INT_EQ, values, none, c"missing".as_ptr()) };
                let vals_bb = self.bb("map.vals");
                let fail2 = self.bb("map.fail");
                self.cond_br(missing, fail2, vals_bb);
                self.position(fail2);
                self.decref(kt);
                self.br(fail);
                self.position(vals_bb);
                let vslot = self.alloca("map.values");
                self.store(vslot, values);
                self.fm().temp_slots.push(vslot);
                for (i, q) in patterns.iter().enumerate() {
                    let vv = self.load(vslot);
                    let idx = self.i64c(i as i64);
                    let item = self.call("PyList_GetItem", &[vv, idx]); // tuple items via generic index: use PyObject_GetItem
                    let _ = item;
                    let iobj = { let k = unsafe { LLVMConstInt(self.i64_t, i as u64, 0) }; let _ = k; let ki = self.constant(&Constant::Int(i.to_string())); let it = self.call("PyObject_GetItem", &[vv, ki]); self.decref(ki); it };
                    self.check_null(iobj);
                    let next = self.bb("map.next");
                    let fail3 = self.bb("map.fail3");
                    self.pattern(q, iobj, next, fail3)?;
                    self.position(fail3);
                    self.decref(iobj);
                    let vv2 = self.load(vslot); self.store(vslot, self.null()); self.decref(vv2); self.decref(kt);
                    self.br(fail);
                    self.position(next);
                    self.decref(iobj);
                }
                if let Some(rn) = rest {
                    let d = self.call("PyDict_New", &[]);
                    let r = self.call("piper_dict_update", &[d, subject]);
                    self.check_neg(r);
                    for kv in &kvals { let r = self.call("PyDict_DelItem", &[d, *kv]); let _ = r; self.call("PyErr_Clear", &[]); }
                    self.store_name(rn, d)?;
                }
                let vv = self.load(vslot); self.store(vslot, self.null()); self.decref(vv); self.decref(kt);
                self.br(ok);
            }
            PatternKind::MatchClass { cls, patterns, kwd_attrs, kwd_patterns } => {
                let c = self.expr(cls)?;
                let npos = self.i64c(patterns.len() as i64);
                let kwnames = if kwd_attrs.is_empty() { self.null() } else { self.names_tuple(kwd_attrs) };
                let attrs = self.call("piper_match_class", &[subject, c, npos, kwnames]);
                self.decref(c);
                self.check_null(attrs);
                let none = self.call("piper_none", &[]);
                let nomatch = unsafe { LLVMBuildICmp(self.b(), INT_EQ, attrs, none, c"nomatch".as_ptr()) };
                let sub_bb = self.bb("class.sub");
                self.cond_br(nomatch, fail, sub_bb);
                self.position(sub_bb);
                let aslot = self.alloca("class.attrs");
                self.store(aslot, attrs);
                self.fm().temp_slots.push(aslot);
                let all: Vec<&Pattern> = patterns.iter().chain(kwd_patterns).collect();
                for (i, q) in all.iter().enumerate() {
                    let av = self.load(aslot);
                    let ki = self.constant(&Constant::Int(i.to_string()));
                    let item = self.call("PyObject_GetItem", &[av, ki]);
                    self.decref(ki);
                    self.check_null(item);
                    let next = self.bb("class.next");
                    let failc = self.bb("class.fail");
                    self.pattern(q, item, next, failc)?;
                    self.position(failc);
                    self.decref(item);
                    let av2 = self.load(aslot); self.store(aslot, self.null()); self.decref(av2);
                    self.br(fail);
                    self.position(next);
                    self.decref(item);
                }
                let av = self.load(aslot); self.store(aslot, self.null()); self.decref(av);
                self.br(ok);
            }
            PatternKind::MatchStar { .. } => return err("star pattern outside sequence", p.span),
        }
        Ok(())
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum CompKind { List, Set, Dict }

fn binop_code(op: Operator) -> i32 {
    match op {
        Operator::Add => 0, Operator::Sub => 1, Operator::Mult => 2, Operator::MatMult => 3, Operator::Div => 4, Operator::Mod => 5,
        Operator::Pow => 6, Operator::LShift => 7, Operator::RShift => 8, Operator::BitOr => 9, Operator::BitXor => 10, Operator::BitAnd => 11, Operator::FloorDiv => 12,
    }
}

fn sanitize(s: &str) -> String { s.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '.' { c } else { '_' }).collect() }

trait CopiedLoop { fn copied_loop(&self) -> Option<LoopCopy>; }
#[derive(Clone, Copy)]
struct LoopCopy { break_bb: BasicBlockRef, continue_bb: BasicBlockRef, depth: usize }
impl CopiedLoop for Option<&Loop> { fn copied_loop(&self) -> Option<LoopCopy> { self.map(|l| LoopCopy { break_bb: l.break_bb, continue_bb: l.continue_bb, depth: l.depth }) } }
