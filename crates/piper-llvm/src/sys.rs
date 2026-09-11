//! Raw bindings to the LLVM-C API (LLVM 23). Only what piper uses is
//! declared; everything here mirrors `llvm-c/*.h` exactly.

#![allow(non_camel_case_types, non_snake_case, dead_code)]

use std::ffi::{c_char, c_double, c_int, c_uint, c_ulonglong, c_void};

macro_rules! opaque {
    ($($name:ident),* $(,)?) => {$(
        #[repr(C)] pub struct $name { _private: [u8; 0] }
    )*};
}

opaque!(
    Context, Module, Type, Value, BasicBlock, Builder, MemoryBuffer, Target, TargetMachine, TargetData,
    PassBuilderOptions, Error, OrcLLJIT, OrcLLJITBuilder, OrcJITDylib, OrcThreadSafeContext, OrcThreadSafeModule,
    OrcDefinitionGenerator, OrcExecutionSession, OrcSymbolStringPoolEntry, OrcJITTargetMachineBuilder, Attribute,
);
pub type AttributeRef = *mut Attribute;
/// `LLVMAttributeFunctionIndex`
pub const ATTRIBUTE_FUNCTION_INDEX: c_uint = u32::MAX;

pub type ContextRef = *mut Context;
pub type ModuleRef = *mut Module;
pub type TypeRef = *mut Type;
pub type ValueRef = *mut Value;
pub type BasicBlockRef = *mut BasicBlock;
pub type BuilderRef = *mut Builder;
pub type MemoryBufferRef = *mut MemoryBuffer;
pub type TargetRef = *mut Target;
pub type TargetMachineRef = *mut TargetMachine;
pub type TargetDataRef = *mut TargetData;
pub type PassBuilderOptionsRef = *mut PassBuilderOptions;
pub type ErrorRef = *mut Error;
pub type OrcLLJITRef = *mut OrcLLJIT;
pub type OrcLLJITBuilderRef = *mut OrcLLJITBuilder;
pub type OrcJITDylibRef = *mut OrcJITDylib;
pub type OrcThreadSafeContextRef = *mut OrcThreadSafeContext;
pub type OrcThreadSafeModuleRef = *mut OrcThreadSafeModule;
pub type OrcDefinitionGeneratorRef = *mut OrcDefinitionGenerator;
pub type OrcJITTargetMachineBuilderRef = *mut OrcJITTargetMachineBuilder;
pub type OrcExecutorAddress = u64;
pub type Bool = c_int;

pub const CODEGEN_LEVEL_NONE: c_uint = 0;
pub const CODEGEN_LEVEL_LESS: c_uint = 1;
pub const CODEGEN_LEVEL_DEFAULT: c_uint = 2;
pub const CODEGEN_LEVEL_AGGRESSIVE: c_uint = 3;

pub const RELOC_DEFAULT: c_uint = 0;
pub const RELOC_STATIC: c_uint = 1;
pub const RELOC_PIC: c_uint = 2;

pub const CODE_MODEL_DEFAULT: c_uint = 0;

pub const ASSEMBLY_FILE: c_uint = 0;
pub const OBJECT_FILE: c_uint = 1;

pub const VERIFIER_ABORT_PROCESS: c_uint = 0;
pub const VERIFIER_PRINT_MESSAGE: c_uint = 1;
pub const VERIFIER_RETURN_STATUS: c_uint = 2;

/// `LLVMIntPredicate`
pub const INT_EQ: c_uint = 32;
pub const INT_NE: c_uint = 33;
pub const INT_UGT: c_uint = 34;
pub const INT_UGE: c_uint = 35;
pub const INT_ULT: c_uint = 36;
pub const INT_ULE: c_uint = 37;
pub const INT_SGT: c_uint = 38;
pub const INT_SGE: c_uint = 39;
pub const INT_SLT: c_uint = 40;
pub const INT_SLE: c_uint = 41;

/// `LLVMRealPredicate`
pub const REAL_OEQ: c_uint = 1;
pub const REAL_OGT: c_uint = 2;
pub const REAL_OGE: c_uint = 3;
pub const REAL_OLT: c_uint = 4;
pub const REAL_OLE: c_uint = 5;
pub const REAL_ONE: c_uint = 6;
pub const REAL_UNE: c_uint = 14;

/// `LLVMLinkage`
pub const LINKAGE_EXTERNAL: c_uint = 0;
pub const LINKAGE_INTERNAL: c_uint = 8;
pub const LINKAGE_PRIVATE: c_uint = 9;

unsafe extern "C" {
    // Core: context / module
    pub fn LLVMContextCreate() -> ContextRef;
    pub fn LLVMContextDispose(c: ContextRef);
    pub fn LLVMModuleCreateWithNameInContext(name: *const c_char, c: ContextRef) -> ModuleRef;
    pub fn LLVMDisposeModule(m: ModuleRef);
    pub fn LLVMSetTarget(m: ModuleRef, triple: *const c_char);
    pub fn LLVMSetModuleDataLayout(m: ModuleRef, dl: TargetDataRef);
    pub fn LLVMPrintModuleToString(m: ModuleRef) -> *mut c_char;
    pub fn LLVMDisposeMessage(msg: *mut c_char);
    pub fn LLVMVerifyModule(m: ModuleRef, action: c_uint, out_msg: *mut *mut c_char) -> Bool;

    // Types
    pub fn LLVMVoidTypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMInt1TypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMInt8TypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMInt16TypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMInt32TypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMInt64TypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMDoubleTypeInContext(c: ContextRef) -> TypeRef;
    pub fn LLVMPointerTypeInContext(c: ContextRef, addr_space: c_uint) -> TypeRef;
    pub fn LLVMFunctionType(ret: TypeRef, params: *mut TypeRef, count: c_uint, is_var_arg: Bool) -> TypeRef;
    pub fn LLVMStructTypeInContext(c: ContextRef, elems: *mut TypeRef, count: c_uint, packed: Bool) -> TypeRef;
    pub fn LLVMArrayType2(elem: TypeRef, count: u64) -> TypeRef;
    pub fn LLVMTypeOf(v: ValueRef) -> TypeRef;

    // Values / constants
    pub fn LLVMConstInt(t: TypeRef, n: c_ulonglong, sign_extend: Bool) -> ValueRef;
    pub fn LLVMConstReal(t: TypeRef, n: c_double) -> ValueRef;
    pub fn LLVMConstNull(t: TypeRef) -> ValueRef;
    pub fn LLVMConstStringInContext2(c: ContextRef, s: *const c_char, len: usize, dont_null_terminate: Bool) -> ValueRef;
    pub fn LLVMGetUndef(t: TypeRef) -> ValueRef;
    pub fn LLVMSetValueName2(v: ValueRef, name: *const c_char, len: usize);

    // Globals / functions
    pub fn LLVMAddFunction(m: ModuleRef, name: *const c_char, ty: TypeRef) -> ValueRef;
    pub fn LLVMGetNamedFunction(m: ModuleRef, name: *const c_char) -> ValueRef;
    pub fn LLVMGetParam(f: ValueRef, index: c_uint) -> ValueRef;
    pub fn LLVMSetLinkage(v: ValueRef, linkage: c_uint);
    pub fn LLVMAddGlobal(m: ModuleRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMGetNamedGlobal(m: ModuleRef, name: *const c_char) -> ValueRef;
    pub fn LLVMSetInitializer(g: ValueRef, v: ValueRef);
    pub fn LLVMSetGlobalConstant(g: ValueRef, is_const: Bool);
    pub fn LLVMSetUnnamedAddress(g: ValueRef, kind: c_uint);
    pub fn LLVMAppendBasicBlockInContext(c: ContextRef, f: ValueRef, name: *const c_char) -> BasicBlockRef;
    pub fn LLVMGetInsertBlock(b: BuilderRef) -> BasicBlockRef;
    pub fn LLVMGetBasicBlockTerminator(bb: BasicBlockRef) -> ValueRef;
    pub fn LLVMGetBasicBlockParent(bb: BasicBlockRef) -> ValueRef;
    pub fn LLVMDeleteBasicBlock(bb: BasicBlockRef);
    pub fn LLVMGetEntryBasicBlock(f: ValueRef) -> BasicBlockRef;
    pub fn LLVMGetFirstInstruction(bb: BasicBlockRef) -> ValueRef;
    pub fn LLVMPositionBuilderBefore(b: BuilderRef, instr: ValueRef);

    // Builder
    pub fn LLVMCreateBuilderInContext(c: ContextRef) -> BuilderRef;
    pub fn LLVMDisposeBuilder(b: BuilderRef);
    pub fn LLVMPositionBuilderAtEnd(b: BuilderRef, bb: BasicBlockRef);
    pub fn LLVMBuildRet(b: BuilderRef, v: ValueRef) -> ValueRef;
    pub fn LLVMBuildRetVoid(b: BuilderRef) -> ValueRef;
    pub fn LLVMBuildBr(b: BuilderRef, dest: BasicBlockRef) -> ValueRef;
    pub fn LLVMBuildCondBr(b: BuilderRef, cond: ValueRef, then: BasicBlockRef, els: BasicBlockRef) -> ValueRef;
    pub fn LLVMBuildSwitch(b: BuilderRef, v: ValueRef, els: BasicBlockRef, num_cases: c_uint) -> ValueRef;
    pub fn LLVMAddCase(switch: ValueRef, on_val: ValueRef, dest: BasicBlockRef);
    pub fn LLVMBuildUnreachable(b: BuilderRef) -> ValueRef;
    pub fn LLVMBuildCall2(b: BuilderRef, ty: TypeRef, f: ValueRef, args: *mut ValueRef, n: c_uint, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildAdd(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildSub(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildMul(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildSDiv(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildSRem(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildAnd(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildOr(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildXor(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildShl(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildAShr(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildLShr(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildNeg(b: BuilderRef, v: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildNot(b: BuilderRef, v: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFAdd(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFSub(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFMul(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFDiv(b: BuilderRef, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFNeg(b: BuilderRef, v: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildICmp(b: BuilderRef, pred: c_uint, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFCmp(b: BuilderRef, pred: c_uint, l: ValueRef, r: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildAlloca(b: BuilderRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildLoad2(b: BuilderRef, ty: TypeRef, ptr: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildStore(b: BuilderRef, v: ValueRef, ptr: ValueRef) -> ValueRef;
    pub fn LLVMBuildGEP2(b: BuilderRef, ty: TypeRef, ptr: ValueRef, idx: *mut ValueRef, n: c_uint, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildStructGEP2(b: BuilderRef, ty: TypeRef, ptr: ValueRef, idx: c_uint, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildPhi(b: BuilderRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMAddIncoming(phi: ValueRef, vals: *mut ValueRef, blocks: *mut BasicBlockRef, count: c_uint);
    pub fn LLVMBuildGlobalStringPtr(b: BuilderRef, s: *const c_char, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildIntCast2(b: BuilderRef, v: ValueRef, ty: TypeRef, is_signed: Bool, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildPtrToInt(b: BuilderRef, v: ValueRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildIntToPtr(b: BuilderRef, v: ValueRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildSIToFP(b: BuilderRef, v: ValueRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildFPToSI(b: BuilderRef, v: ValueRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildBitCast(b: BuilderRef, v: ValueRef, ty: TypeRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildSelect(b: BuilderRef, cond: ValueRef, then: ValueRef, els: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildIsNull(b: BuilderRef, v: ValueRef, name: *const c_char) -> ValueRef;
    pub fn LLVMBuildIsNotNull(b: BuilderRef, v: ValueRef, name: *const c_char) -> ValueRef;

    // Attributes
    pub fn LLVMGetEnumAttributeKindForName(name: *const c_char, len: usize) -> c_uint;
    pub fn LLVMCreateEnumAttribute(c: ContextRef, kind: c_uint, val: u64) -> AttributeRef;
    pub fn LLVMAddAttributeAtIndex(f: ValueRef, idx: c_uint, a: AttributeRef);
    pub fn LLVMGetFirstFunction(m: ModuleRef) -> ValueRef;
    pub fn LLVMGetNextFunction(f: ValueRef) -> ValueRef;
    pub fn LLVMIsDeclaration(v: ValueRef) -> Bool;

    // Targets
    pub fn LLVMInitializeX86TargetInfo();
    pub fn LLVMInitializeX86Target();
    pub fn LLVMInitializeX86TargetMC();
    pub fn LLVMInitializeX86AsmPrinter();
    pub fn LLVMInitializeX86AsmParser();
    pub fn LLVMInitializeAArch64TargetInfo();
    pub fn LLVMInitializeAArch64Target();
    pub fn LLVMInitializeAArch64TargetMC();
    pub fn LLVMInitializeAArch64AsmPrinter();
    pub fn LLVMInitializeAArch64AsmParser();
    pub fn LLVMGetDefaultTargetTriple() -> *mut c_char;
    pub fn LLVMGetHostCPUName() -> *mut c_char;
    pub fn LLVMGetHostCPUFeatures() -> *mut c_char;
    pub fn LLVMGetTargetFromTriple(triple: *const c_char, out: *mut TargetRef, err: *mut *mut c_char) -> Bool;
    pub fn LLVMCreateTargetMachine(t: TargetRef, triple: *const c_char, cpu: *const c_char, features: *const c_char, level: c_uint, reloc: c_uint, code_model: c_uint) -> TargetMachineRef;
    pub fn LLVMDisposeTargetMachine(tm: TargetMachineRef);
    pub fn LLVMCreateTargetDataLayout(tm: TargetMachineRef) -> TargetDataRef;
    pub fn LLVMDisposeTargetData(td: TargetDataRef);
    pub fn LLVMTargetMachineEmitToMemoryBuffer(tm: TargetMachineRef, m: ModuleRef, codegen: c_uint, err: *mut *mut c_char, out: *mut MemoryBufferRef) -> Bool;
    pub fn LLVMGetBufferStart(b: MemoryBufferRef) -> *const c_char;
    pub fn LLVMGetBufferSize(b: MemoryBufferRef) -> usize;
    pub fn LLVMDisposeMemoryBuffer(b: MemoryBufferRef);

    // New pass manager
    pub fn LLVMCreatePassBuilderOptions() -> PassBuilderOptionsRef;
    pub fn LLVMDisposePassBuilderOptions(o: PassBuilderOptionsRef);
    pub fn LLVMRunPasses(m: ModuleRef, passes: *const c_char, tm: TargetMachineRef, o: PassBuilderOptionsRef) -> ErrorRef;

    // Errors
    pub fn LLVMGetErrorMessage(e: ErrorRef) -> *mut c_char;
    pub fn LLVMDisposeErrorMessage(msg: *mut c_char);
    pub fn LLVMConsumeError(e: ErrorRef);

    // ORC JIT
    pub fn LLVMOrcCreateLLJIT(out: *mut OrcLLJITRef, builder: OrcLLJITBuilderRef) -> ErrorRef;
    pub fn LLVMOrcDisposeLLJIT(j: OrcLLJITRef) -> ErrorRef;
    pub fn LLVMOrcLLJITGetMainJITDylib(j: OrcLLJITRef) -> OrcJITDylibRef;
    pub fn LLVMOrcLLJITGetGlobalPrefix(j: OrcLLJITRef) -> c_char;
    pub fn LLVMOrcLLJITAddLLVMIRModule(j: OrcLLJITRef, jd: OrcJITDylibRef, tsm: OrcThreadSafeModuleRef) -> ErrorRef;
    pub fn LLVMOrcLLJITLookup(j: OrcLLJITRef, out: *mut OrcExecutorAddress, name: *const c_char) -> ErrorRef;
    pub fn LLVMOrcCreateNewThreadSafeContextFromLLVMContext(c: ContextRef) -> OrcThreadSafeContextRef;
    pub fn LLVMOrcDisposeThreadSafeContext(c: OrcThreadSafeContextRef);
    pub fn LLVMOrcCreateNewThreadSafeModule(m: ModuleRef, c: OrcThreadSafeContextRef) -> OrcThreadSafeModuleRef;
    pub fn LLVMOrcDisposeThreadSafeModule(m: OrcThreadSafeModuleRef);
    pub fn LLVMOrcJITDylibAddGenerator(jd: OrcJITDylibRef, g: OrcDefinitionGeneratorRef);
    pub fn LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(out: *mut OrcDefinitionGeneratorRef, global_prefix: c_char, filter: *const c_void, filter_ctx: *mut c_void) -> ErrorRef;
}
