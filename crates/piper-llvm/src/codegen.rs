//! Thin safe layer over the raw bindings: owns a context and module, runs
//! the optimizer, emits objects for any target, and JITs in-process.

use crate::sys::*;
use std::ffi::{CStr, CString};
use std::sync::Once;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OptLevel { O0, O1, O2, O3, Os, Oz }

impl OptLevel {
    /// Pass pipeline name for `LLVMRunPasses`. Size levels run the O2
    /// pipeline; the size preference is a per-function attribute.
    fn pipeline(self) -> &'static str {
        match self {
            OptLevel::O0 => "default<O0>", OptLevel::O1 => "default<O1>",
            OptLevel::O2 | OptLevel::Os | OptLevel::Oz => "default<O2>", OptLevel::O3 => "default<O3>",
        }
    }
    fn size_attribute(self) -> Option<&'static str> {
        match self { OptLevel::Os => Some("optsize"), OptLevel::Oz => Some("minsize"), _ => None }
    }
    fn codegen_level(self) -> u32 {
        match self {
            OptLevel::O0 => CODEGEN_LEVEL_NONE,
            OptLevel::O1 => CODEGEN_LEVEL_LESS,
            OptLevel::O2 | OptLevel::Os | OptLevel::Oz => CODEGEN_LEVEL_DEFAULT,
            OptLevel::O3 => CODEGEN_LEVEL_AGGRESSIVE,
        }
    }
}

fn init_targets() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeX86AsmParser();
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
    });
}

fn cstr(s: &str) -> CString { CString::new(s).expect("interior NUL") }

/// Take ownership of an LLVM-allocated message and free it.
unsafe fn take_message(p: *mut std::ffi::c_char) -> String {
    if p.is_null() { return String::new(); }
    let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
    unsafe { LLVMDisposeMessage(p) };
    s
}

unsafe fn take_error(e: ErrorRef) -> Result<(), String> {
    if e.is_null() { return Ok(()); }
    let msg = unsafe { LLVMGetErrorMessage(e) };
    let s = unsafe { CStr::from_ptr(msg) }.to_string_lossy().into_owned();
    unsafe { LLVMDisposeErrorMessage(msg) };
    Err(s)
}

/// Host target triple as LLVM reports it.
pub fn host_triple() -> String {
    unsafe { take_message(LLVMGetDefaultTargetTriple()) }
}

/// Owns an LLVM context, one module, and a builder.
pub struct Codegen {
    pub ctx: ContextRef,
    pub module: ModuleRef,
    pub builder: BuilderRef,
}

impl Codegen {
    pub fn new(name: &str) -> Self {
        init_targets();
        unsafe {
            let ctx = LLVMContextCreate();
            let module = LLVMModuleCreateWithNameInContext(cstr(name).as_ptr(), ctx);
            let builder = LLVMCreateBuilderInContext(ctx);
            Codegen { ctx, module, builder }
        }
    }

    /// `i32 name()` returning a constant. Used by the smoke tests.
    pub fn emit_const_i32_function(&mut self, name: &str, value: i32) {
        unsafe {
            let i32t = LLVMInt32TypeInContext(self.ctx);
            let fty = LLVMFunctionType(i32t, std::ptr::null_mut(), 0, 0);
            let f = LLVMAddFunction(self.module, cstr(name).as_ptr(), fty);
            let bb = LLVMAppendBasicBlockInContext(self.ctx, f, c"entry".as_ptr());
            LLVMPositionBuilderAtEnd(self.builder, bb);
            LLVMBuildRet(self.builder, LLVMConstInt(i32t, value as u64, 1));
        }
    }

    /// Add an enum attribute (e.g. `optsize`) to every defined function.
    fn add_function_attribute(&mut self, name: &str) {
        unsafe {
            let kind = LLVMGetEnumAttributeKindForName(name.as_ptr() as *const _, name.len());
            let attr = LLVMCreateEnumAttribute(self.ctx, kind, 0);
            let mut f = LLVMGetFirstFunction(self.module);
            while !f.is_null() {
                if LLVMIsDeclaration(f) == 0 { LLVMAddAttributeAtIndex(f, ATTRIBUTE_FUNCTION_INDEX, attr); }
                f = LLVMGetNextFunction(f);
            }
        }
    }

    /// Textual IR of the module.
    pub fn ir(&self) -> String {
        unsafe { take_message(LLVMPrintModuleToString(self.module)) }
    }

    pub fn verify(&self) -> Result<(), String> {
        unsafe {
            let mut msg = std::ptr::null_mut();
            if LLVMVerifyModule(self.module, VERIFIER_RETURN_STATUS, &mut msg) != 0 {
                return Err(take_message(msg));
            }
            take_message(msg);
            Ok(())
        }
    }

    fn target_machine(&self, triple: Option<&str>, level: OptLevel) -> Result<TargetMachineRef, String> {
        unsafe {
            let triple = triple.map(str::to_owned).unwrap_or_else(host_triple);
            let ctriple = cstr(&triple);
            let mut target = std::ptr::null_mut();
            let mut err = std::ptr::null_mut();
            if LLVMGetTargetFromTriple(ctriple.as_ptr(), &mut target, &mut err) != 0 {
                return Err(take_message(err));
            }
            let (cpu, features) = if triple == host_triple() {
                (take_message(LLVMGetHostCPUName()), take_message(LLVMGetHostCPUFeatures()))
            } else {
                (String::from("generic"), String::new())
            };
            let tm = LLVMCreateTargetMachine(target, ctriple.as_ptr(), cstr(&cpu).as_ptr(), cstr(&features).as_ptr(), level.codegen_level(), RELOC_PIC, CODE_MODEL_DEFAULT);
            if tm.is_null() { return Err(format!("cannot create target machine for {triple}")); }
            LLVMSetTarget(self.module, ctriple.as_ptr());
            let dl = LLVMCreateTargetDataLayout(tm);
            LLVMSetModuleDataLayout(self.module, dl);
            LLVMDisposeTargetData(dl);
            Ok(tm)
        }
    }

    /// Run the optimizer at `level`, then emit a native object for `triple`
    /// (host when `None`).
    pub fn emit_object(&mut self, triple: Option<&str>, level: OptLevel) -> Result<Vec<u8>, String> {
        self.verify()?;
        unsafe {
            if let Some(attr) = level.size_attribute() { self.add_function_attribute(attr); }
            let tm = self.target_machine(triple, level)?;
            let opts = LLVMCreatePassBuilderOptions();
            let r = take_error(LLVMRunPasses(self.module, cstr(level.pipeline()).as_ptr(), tm, opts));
            LLVMDisposePassBuilderOptions(opts);
            if let Err(e) = r { LLVMDisposeTargetMachine(tm); return Err(e); }
            let mut err = std::ptr::null_mut();
            let mut buf = std::ptr::null_mut();
            let failed = LLVMTargetMachineEmitToMemoryBuffer(tm, self.module, OBJECT_FILE, &mut err, &mut buf);
            LLVMDisposeTargetMachine(tm);
            if failed != 0 { return Err(take_message(err)); }
            let bytes = std::slice::from_raw_parts(LLVMGetBufferStart(buf) as *const u8, LLVMGetBufferSize(buf)).to_vec();
            LLVMDisposeMemoryBuffer(buf);
            Ok(bytes)
        }
    }

    /// Hand the module to an in-process ORC JIT. Symbols from the host
    /// process (the runtime, libc) resolve automatically.
    pub fn into_jit(self) -> Result<Jit, String> {
        self.verify()?;
        unsafe {
            let mut jit = std::ptr::null_mut();
            take_error(LLVMOrcCreateLLJIT(&mut jit, std::ptr::null_mut()))?;
            let jd = LLVMOrcLLJITGetMainJITDylib(jit);
            let mut generator = std::ptr::null_mut();
            take_error(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(&mut generator, LLVMOrcLLJITGetGlobalPrefix(jit), std::ptr::null(), std::ptr::null_mut()))?;
            LLVMOrcJITDylibAddGenerator(jd, generator);
            let tsc = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(self.ctx);
            let tsm = LLVMOrcCreateNewThreadSafeModule(self.module, tsc);
            LLVMOrcDisposeThreadSafeContext(tsc);
            // The JIT now owns the module and (via the TSC) the context.
            LLVMDisposeBuilder(self.builder);
            std::mem::forget(self);
            take_error(LLVMOrcLLJITAddLLVMIRModule(jit, jd, tsm))?;
            Ok(Jit { jit })
        }
    }
}

impl Drop for Codegen {
    fn drop(&mut self) {
        unsafe {
            LLVMDisposeBuilder(self.builder);
            LLVMDisposeModule(self.module);
            LLVMContextDispose(self.ctx);
        }
    }
}

pub struct Jit {
    jit: OrcLLJITRef,
}

impl Jit {
    /// Address of a JIT-compiled symbol.
    pub fn lookup(&self, name: &str) -> Result<usize, String> {
        unsafe {
            let mut addr: OrcExecutorAddress = 0;
            take_error(LLVMOrcLLJITLookup(self.jit, &mut addr, cstr(name).as_ptr()))?;
            Ok(addr as usize)
        }
    }
}

impl Drop for Jit {
    fn drop(&mut self) {
        unsafe { let _ = take_error(LLVMOrcDisposeLLJIT(self.jit)); }
    }
}
