//! Declarations of the runtime's C entry points used from Rust.

#![allow(non_camel_case_types, non_snake_case)]

use std::ffi::{c_char, c_int, c_longlong, c_void};

#[repr(C)]
pub struct PyObject { _private: [u8; 0] }
pub type PyObjectRef = *mut PyObject;
pub type Py_ssize_t = isize;

unsafe extern "C" {
    pub fn piper_initialize();

    // refcounts
    pub fn Py_IncRef(o: PyObjectRef);
    pub fn Py_DecRef(o: PyObjectRef);
    pub fn Py_REFCNT(o: PyObjectRef) -> Py_ssize_t;

    // singletons
    pub fn piper_none() -> PyObjectRef;
    pub fn piper_true() -> PyObjectRef;
    pub fn piper_false() -> PyObjectRef;

    // int / float / str / bytes
    pub fn PyLong_FromLongLong(v: c_longlong) -> PyObjectRef;
    pub fn PyLong_AsLongLong(o: PyObjectRef) -> c_longlong;
    pub fn PyLong_FromString(s: *const c_char, end: *mut *mut c_char, base: c_int) -> PyObjectRef;
    pub fn PyFloat_FromDouble(v: f64) -> PyObjectRef;
    pub fn PyFloat_AsDouble(o: PyObjectRef) -> f64;
    pub fn PyUnicode_FromStringAndSize(s: *const c_char, n: Py_ssize_t) -> PyObjectRef;
    pub fn PyUnicode_AsUTF8AndSize(o: PyObjectRef, n: *mut Py_ssize_t) -> *const c_char;
    pub fn PyUnicode_GetLength(o: PyObjectRef) -> Py_ssize_t;
    pub fn PyBytes_FromStringAndSize(s: *const c_char, n: Py_ssize_t) -> PyObjectRef;

    // containers
    pub fn PyList_New(n: Py_ssize_t) -> PyObjectRef;
    pub fn PyList_Append(l: PyObjectRef, v: PyObjectRef) -> c_int;
    pub fn PyList_Size(l: PyObjectRef) -> Py_ssize_t;
    pub fn PyList_GetItem(l: PyObjectRef, i: Py_ssize_t) -> PyObjectRef;
    pub fn PyTuple_New(n: Py_ssize_t) -> PyObjectRef;
    pub fn PyTuple_SetItem(t: PyObjectRef, i: Py_ssize_t, v: PyObjectRef) -> c_int;
    pub fn PyTuple_Pack(n: Py_ssize_t, ...) -> PyObjectRef;
    pub fn PyDict_New() -> PyObjectRef;
    pub fn PyDict_SetItem(d: PyObjectRef, k: PyObjectRef, v: PyObjectRef) -> c_int;
    pub fn PyDict_GetItemWithError(d: PyObjectRef, k: PyObjectRef) -> PyObjectRef;
    pub fn PyDict_SetItemString(d: PyObjectRef, k: *const c_char, v: PyObjectRef) -> c_int;
    pub fn PyDict_GetItemString(d: PyObjectRef, k: *const c_char) -> PyObjectRef;
    pub fn PyDict_Size(d: PyObjectRef) -> Py_ssize_t;

    // generic protocol
    pub fn PyObject_Repr(o: PyObjectRef) -> PyObjectRef;
    pub fn PyObject_Str(o: PyObjectRef) -> PyObjectRef;
    pub fn PyObject_Hash(o: PyObjectRef) -> Py_ssize_t;
    pub fn PyObject_IsTrue(o: PyObjectRef) -> c_int;
    pub fn PyObject_RichCompareBool(a: PyObjectRef, b: PyObjectRef, op: c_int) -> c_int;
    pub fn PyObject_RichCompare(a: PyObjectRef, b: PyObjectRef, op: c_int) -> PyObjectRef;
    pub fn PyObject_GetAttrString(o: PyObjectRef, name: *const c_char) -> PyObjectRef;
    pub fn PyObject_SetAttrString(o: PyObjectRef, name: *const c_char, v: PyObjectRef) -> c_int;
    pub fn PyObject_GetItem(o: PyObjectRef, k: PyObjectRef) -> PyObjectRef;
    pub fn PyObject_SetItem(o: PyObjectRef, k: PyObjectRef, v: PyObjectRef) -> c_int;
    pub fn PyObject_GetIter(o: PyObjectRef) -> PyObjectRef;
    pub fn PyIter_Next(o: PyObjectRef) -> PyObjectRef;
    pub fn PyObject_Call(f: PyObjectRef, args: PyObjectRef, kwargs: PyObjectRef) -> PyObjectRef;
    pub fn PyObject_CallNoArgs(f: PyObjectRef) -> PyObjectRef;
    pub fn PyObject_Length(o: PyObjectRef) -> Py_ssize_t;
    pub fn PyObject_Type(o: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Add(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Subtract(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Multiply(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_TrueDivide(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_FloorDivide(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Remainder(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Power(a: PyObjectRef, b: PyObjectRef, m: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Negative(a: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_Lshift(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;
    pub fn PyNumber_And(a: PyObjectRef, b: PyObjectRef) -> PyObjectRef;

    // errors
    pub fn PyErr_Occurred() -> PyObjectRef;
    pub fn PyErr_SetString(t: PyObjectRef, msg: *const c_char);
    pub fn PyErr_Clear();
    pub fn PyErr_GetRaisedException() -> PyObjectRef;
    pub fn PyErr_SetRaisedException(e: PyObjectRef);
    pub fn PyErr_ExceptionMatches(t: PyObjectRef) -> c_int;
    pub fn piper_exc_type(name: *const c_char) -> PyObjectRef;

    // modules / builtins
    pub fn piper_builtins() -> PyObjectRef;
    pub fn PyModule_New(name: *const c_char) -> PyObjectRef;
    pub fn PyModule_GetDict(m: PyObjectRef) -> PyObjectRef;
    pub fn PyImport_AddModule(name: *const c_char) -> PyObjectRef;

    // capsules
    pub fn PyCapsule_New(pointer: *mut c_void, name: *const c_char, destructor: Option<unsafe extern "C" fn(PyObjectRef)>) -> PyObjectRef;
    pub fn PyCapsule_IsValid(capsule: PyObjectRef, name: *const c_char) -> c_int;
    pub fn PyCapsule_GetPointer(capsule: PyObjectRef, name: *const c_char) -> *mut c_void;
    pub fn PyCapsule_GetContext(capsule: PyObjectRef) -> *mut c_void;
    pub fn PyCapsule_GetName(capsule: PyObjectRef) -> *const c_char;
    pub fn PyCapsule_SetPointer(capsule: PyObjectRef, pointer: *mut c_void) -> c_int;
    pub fn PyCapsule_SetContext(capsule: PyObjectRef, context: *mut c_void) -> c_int;
    pub fn PyCapsule_SetName(capsule: PyObjectRef, name: *const c_char) -> c_int;

    // generators
    pub fn piper_generator_new(func: PyObjectRef, resume: *mut c_void, slot_count: Py_ssize_t, kind: c_int) -> PyObjectRef;
    pub fn piper_generator_slot(generator: PyObjectRef, index: Py_ssize_t) -> *mut PyObjectRef;
    pub fn piper_generator_state(generator: PyObjectRef) -> c_int;
    pub fn piper_generator_set_state(generator: PyObjectRef, state: c_int);
    pub fn piper_generator_finish(generator: PyObjectRef, value: PyObjectRef) -> PyObjectRef;

    // memory
    pub fn PyMem_Malloc(n: usize) -> *mut c_void;
    pub fn PyMem_Free(p: *mut c_void);
}

pub const PY_LT: c_int = 0;
pub const PY_LE: c_int = 1;
pub const PY_EQ: c_int = 2;
pub const PY_NE: c_int = 3;
pub const PY_GT: c_int = 4;
pub const PY_GE: c_int = 5;
