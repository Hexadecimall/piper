//! LLVM bindings (hand-written, over the stable LLVM-C API), IR lowering,
//! in-process JIT, and object emission.

pub mod sys;
mod codegen;
pub mod link;
pub mod lower;
pub mod target;

pub use codegen::{Codegen, Jit, OptLevel, host_triple};
