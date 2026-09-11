//! Behavior of the core object model, exercised through the C-API.

use piper_rt::ffi::*;
use std::ffi::{CStr, CString};
use std::sync::{Mutex, MutexGuard, Once};

/// The runtime has no GIL yet, so tests take turns.
fn init() -> MutexGuard<'static, ()> {
    static ONCE: Once = Once::new();
    static LOCK: Mutex<()> = Mutex::new(());
    let guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    ONCE.call_once(|| unsafe { piper_initialize() });
    guard
}

fn int(v: i64) -> PyObjectRef { unsafe { PyLong_FromLongLong(v) } }
fn float(v: f64) -> PyObjectRef { unsafe { PyFloat_FromDouble(v) } }
fn s(v: &str) -> PyObjectRef { unsafe { PyUnicode_FromStringAndSize(v.as_ptr() as *const _, v.len() as isize) } }
fn as_str(o: PyObjectRef) -> String {
    unsafe {
        let mut n = 0;
        let p = PyUnicode_AsUTF8AndSize(o, &mut n);
        assert!(!p.is_null(), "not a str");
        String::from_utf8(std::slice::from_raw_parts(p as *const u8, n as usize).to_vec()).unwrap()
    }
}
fn repr(o: PyObjectRef) -> String { unsafe { let r = PyObject_Repr(o); assert!(!r.is_null(), "repr failed: {}", err()); let t = as_str(r); Py_DecRef(r); t } }
fn str_(o: PyObjectRef) -> String { unsafe { let r = PyObject_Str(o); assert!(!r.is_null()); let t = as_str(r); Py_DecRef(r); t } }
fn err() -> String {
    unsafe {
        let e = PyErr_GetRaisedException();
        if e.is_null() { return "<no error>".into(); }
        let t = PyObject_Type(e);
        let name = PyObject_GetAttrString(t, c"__name__".as_ptr());
        let r = format!("{}: {}", as_str(name), str_(e));
        Py_DecRef(e);
        r
    }
}
fn cs(v: &str) -> CString { CString::new(v).unwrap() }
fn eq(a: PyObjectRef, b: PyObjectRef) -> bool { unsafe { PyObject_RichCompareBool(a, b, PY_EQ) == 1 } }

unsafe extern "C" fn resume_probe(generator: PyObjectRef, sent: PyObjectRef, op: i32) -> PyObjectRef {
    unsafe {
        if op != 0 { return piper_generator_finish(generator, piper_none()); }
        match piper_generator_state(generator) {
            0 => { piper_generator_set_state(generator, 1); int(10) }
            1 => {
                Py_IncRef(sent);
                *piper_generator_slot(generator, 0) = sent;
                piper_generator_set_state(generator, 2);
                int(20)
            }
            _ => piper_generator_finish(generator, piper_none()),
        }
    }
}

#[test]
fn new_objects_have_refcount_one_and_singletons_are_immortal() {
    let _g = init();
    unsafe {
        let x = int(1000);
        assert_eq!(Py_REFCNT(x), 1);
        Py_IncRef(x);
        assert_eq!(Py_REFCNT(x), 2);
        Py_DecRef(x);
        Py_DecRef(x);
        let n = piper_none();
        let before = Py_REFCNT(n);
        Py_IncRef(n);
        assert_eq!(Py_REFCNT(n), before, "None must be immortal");
    }
}

#[test]
fn generators_suspend_send_and_finish() {
    let _g = init();
    unsafe {
        let name = s("sample");
        let generator = piper_generator_new(name, resume_probe as *const () as *mut _, 1, 0);
        assert!(!generator.is_null(), "{}", err());
        let first = PyIter_Next(generator);
        assert_eq!(PyLong_AsLongLong(first), 10);
        Py_DecRef(first);

        let send = PyObject_GetAttrString(generator, c"send".as_ptr());
        let value = int(7);
        let args = PyTuple_Pack(1, value);
        let second = PyObject_Call(send, args, std::ptr::null_mut());
        assert_eq!(PyLong_AsLongLong(second), 20);
        assert_eq!(PyLong_AsLongLong(*piper_generator_slot(generator, 0)), 7);
        Py_DecRef(second); Py_DecRef(args); Py_DecRef(value); Py_DecRef(send);

        assert!(PyIter_Next(generator).is_null());
        assert!(PyErr_Occurred().is_null());
        Py_DecRef(generator); Py_DecRef(name);
    }
}

#[test]
fn int_arithmetic_and_python_division_semantics() {
    let _g = init();
    unsafe {
        assert_eq!(PyLong_AsLongLong(PyNumber_Add(int(5), int(3))), 8);
        assert_eq!(PyLong_AsLongLong(PyNumber_FloorDivide(int(-7), int(2))), -4);
        assert_eq!(PyLong_AsLongLong(PyNumber_Remainder(int(-7), int(2))), 1);
        assert_eq!(PyLong_AsLongLong(PyNumber_Remainder(int(7), int(-2))), -1);
        assert_eq!(PyLong_AsLongLong(PyNumber_Power(int(2), int(10), piper_none())), 1024);
        assert_eq!(PyLong_AsLongLong(PyNumber_Negative(int(5))), -5);
        assert_eq!(PyLong_AsLongLong(PyNumber_And(int(12), int(10))), 8);
        assert_eq!(repr(PyNumber_TrueDivide(int(1), int(4))), "0.25");
        assert_eq!(repr(int(-3)), "-3");
        assert_eq!(str_(int(0)), "0");
    }
}

#[test]
fn ints_are_arbitrary_precision() {
    let _g = init();
    unsafe {
        let big = PyNumber_Lshift(int(1), int(64));
        assert_eq!(repr(big), "18446744073709551616");
        let sq = PyNumber_Multiply(big, big);
        assert_eq!(repr(sq), "340282366920938463463374607431768211456");
        let back = PyNumber_FloorDivide(sq, big);
        assert!(eq(back, big));
        assert_eq!(repr(PyNumber_Subtract(int(0), big)), "-18446744073709551616");
        assert_eq!(repr(PyNumber_Power(int(3), int(100), piper_none())), "515377520732011331036461129765621272702107522001");
        let parsed = PyLong_FromString(cs("123456789012345678901234567890").as_ptr(), std::ptr::null_mut(), 10);
        assert_eq!(repr(parsed), "123456789012345678901234567890");
        assert_eq!(repr(PyNumber_Remainder(parsed, int(97))), "52");
        assert_eq!(PyObject_RichCompareBool(big, int(5), PY_GT), 1);
    }
}

#[test]
fn float_repr_matches_python() {
    let _g = init();
    for (v, want) in [(0.1, "0.1"), (1e16, "1e+16"), (3.0, "3.0"), (2.5e-7, "2.5e-07"), (1234567890123456.0, "1234567890123456.0"), (f64::INFINITY, "inf"), (-0.0, "-0.0")] {
        assert_eq!(repr(float(v)), want);
    }
    unsafe {
        assert_eq!(repr(PyNumber_Add(int(1), float(0.5))), "1.5");
        assert_eq!(repr(PyNumber_Multiply(float(2.0), int(3))), "6.0");
        assert_eq!(PyObject_RichCompareBool(int(1), float(1.0), PY_EQ), 1);
        assert_eq!(PyFloat_AsDouble(PyNumber_TrueDivide(float(1.0), int(3))), 1.0 / 3.0);
    }
}

#[test]
fn strings_are_unicode_aware() {
    let _g = init();
    unsafe {
        let h = s("héllo");
        assert_eq!(PyUnicode_GetLength(h), 5);
        assert_eq!(as_str(h), "héllo");
        assert_eq!(repr(h), "'héllo'");
        assert_eq!(repr(s("it's")), "\"it's\"");
        assert_eq!(repr(s("a\nb\u{1F600}\x01")), "'a\\nb😀\\x01'");
        let joined = PyNumber_Add(s("ab"), s("cd"));
        assert_eq!(as_str(joined), "abcd");
        assert!(eq(s("x"), s("x")));
        assert_eq!(PyObject_Hash(s("hello")), PyObject_Hash(s("hello")));
        assert_ne!(PyObject_Hash(s("hello")), PyObject_Hash(s("hellp")));
        assert_eq!(as_str(PyObject_GetItem(h, int(1))), "é");
        assert_eq!(as_str(PyObject_GetItem(h, int(-1))), "o");
        assert_eq!(PyObject_Length(s("😀😀")), 2);
        assert_eq!(as_str(PyNumber_Multiply(s("ab"), int(3))), "ababab");
        assert_eq!(PyObject_RichCompareBool(s("a"), s("b"), PY_LT), 1);
    }
}

#[test]
fn string_methods_via_getattr_and_call() {
    let _g = init();
    unsafe {
        let up = PyObject_GetAttrString(s("abc"), c"upper".as_ptr());
        assert!(!up.is_null(), "{}", err());
        let r = PyObject_CallNoArgs(up);
        assert!(!r.is_null(), "{}", err());
        assert_eq!(as_str(r), "ABC");
        let join = PyObject_GetAttrString(s(", "), c"join".as_ptr());
        let l = PyList_New(0);
        PyList_Append(l, s("a"));
        PyList_Append(l, s("b"));
        let r = PyObject_Call(join, PyTuple_Pack(1, l), std::ptr::null_mut());
        assert!(!r.is_null(), "{}", err());
        assert_eq!(as_str(r), "a, b");
        let missing = PyObject_GetAttrString(s("abc"), c"nope".as_ptr());
        assert!(missing.is_null());
        assert_eq!(PyErr_ExceptionMatches(piper_exc_type(c"AttributeError".as_ptr())), 1);
        assert_eq!(err(), "AttributeError: 'str' object has no attribute 'nope'");
    }
}

#[test]
fn lists_tuples_and_dicts() {
    let _g = init();
    unsafe {
        let l = PyList_New(0);
        PyList_Append(l, int(1));
        PyList_Append(l, s("a"));
        PyList_Append(l, piper_none());
        assert_eq!(PyList_Size(l), 3);
        assert_eq!(repr(l), "[1, 'a', None]");
        assert_eq!(PyLong_AsLongLong(PyList_GetItem(l, 0)), 1);
        assert_eq!(as_str(PyObject_GetItem(l, int(-2))), "a");
        assert_eq!(PyObject_SetItem(l, int(0), int(9)), 0);
        assert_eq!(repr(l), "[9, 'a', None]");
        let t = PyTuple_Pack(1, int(1));
        assert_eq!(repr(t), "(1,)");
        assert_eq!(repr(PyTuple_New(0)), "()");
        let d = PyDict_New();
        PyDict_SetItem(d, s("a"), int(1));
        PyDict_SetItem(d, int(2), l);
        assert_eq!(PyDict_Size(d), 2);
        assert_eq!(repr(d), "{'a': 1, 2: [9, 'a', None]}");
        assert_eq!(PyLong_AsLongLong(PyDict_GetItemWithError(d, s("a"))), 1);
        assert!(PyDict_GetItemWithError(d, s("zz")).is_null());
        assert!(PyErr_Occurred().is_null(), "missing key is not an error for GetItemWithError");
        assert!(PyObject_GetItem(d, s("zz")).is_null());
        assert_eq!(err(), "KeyError: 'zz'");
        PyDict_SetItem(d, s("a"), int(5));
        assert_eq!(PyDict_Size(d), 2);
        assert_eq!(PyLong_AsLongLong(PyDict_GetItemString(d, c"a".as_ptr())), 5);
        // many keys force resizes
        let big = PyDict_New();
        for i in 0..1000 { PyDict_SetItem(big, int(i), int(i * 2)); }
        assert_eq!(PyDict_Size(big), 1000);
        assert_eq!(PyLong_AsLongLong(PyDict_GetItemWithError(big, int(777))), 1554);
    }
}

#[test]
fn truthiness_and_comparisons() {
    let _g = init();
    unsafe {
        assert_eq!(PyObject_IsTrue(int(0)), 0);
        assert_eq!(PyObject_IsTrue(int(3)), 1);
        assert_eq!(PyObject_IsTrue(s("")), 0);
        assert_eq!(PyObject_IsTrue(PyList_New(0)), 0);
        assert_eq!(PyObject_IsTrue(piper_none()), 0);
        assert_eq!(PyObject_IsTrue(piper_true()), 1);
        assert_eq!(repr(piper_true()), "True");
        assert_eq!(repr(piper_none()), "None");
        assert_eq!(repr(PyObject_RichCompare(int(1), int(2), PY_LT)), "True");
        assert_eq!(PyObject_RichCompareBool(int(1), s("1"), PY_EQ), 0);
        assert!(PyObject_RichCompare(int(1), s("1"), PY_LT).is_null());
        assert_eq!(err(), "TypeError: '<' not supported between instances of 'int' and 'str'");
    }
}

#[test]
fn iteration_protocol() {
    let _g = init();
    unsafe {
        let l = PyList_New(0);
        for i in 0..3 { PyList_Append(l, int(i)); }
        let it = PyObject_GetIter(l);
        assert!(!it.is_null());
        let mut got = Vec::new();
        loop {
            let x = PyIter_Next(it);
            if x.is_null() { break; }
            got.push(PyLong_AsLongLong(x));
        }
        assert!(PyErr_Occurred().is_null());
        assert_eq!(got, vec![0, 1, 2]);
        assert!(PyObject_GetIter(int(1)).is_null());
        assert_eq!(err(), "TypeError: 'int' object is not iterable");
    }
}

#[test]
fn builtins_are_callable() {
    let _g = init();
    unsafe {
        let b = piper_builtins();
        let len = PyDict_GetItemString(b, c"len".as_ptr());
        assert!(!len.is_null());
        let r = PyObject_Call(len, PyTuple_Pack(1, s("abcd")), std::ptr::null_mut());
        assert_eq!(PyLong_AsLongLong(r), 4);
        let range = PyDict_GetItemString(b, c"range".as_ptr());
        let r = PyObject_Call(range, PyTuple_Pack(2, int(2), int(5)), std::ptr::null_mut());
        assert_eq!(repr(r), "range(2, 5)");
        let list = PyDict_GetItemString(b, c"list".as_ptr());
        let l = PyObject_Call(list, PyTuple_Pack(1, r), std::ptr::null_mut());
        assert_eq!(repr(l), "[2, 3, 4]");
        let strf = PyDict_GetItemString(b, c"str".as_ptr());
        assert_eq!(as_str(PyObject_Call(strf, PyTuple_Pack(1, int(42)), std::ptr::null_mut())), "42");
        let intf = PyDict_GetItemString(b, c"int".as_ptr());
        assert_eq!(PyLong_AsLongLong(PyObject_Call(intf, PyTuple_Pack(1, s("17")), std::ptr::null_mut())), 17);
        assert!(PyObject_Call(intf, PyTuple_Pack(1, s("x")), std::ptr::null_mut()).is_null());
        assert_eq!(err(), "ValueError: invalid literal for int() with base 10: 'x'");
        let typ = PyDict_GetItemString(b, c"type".as_ptr());
        let t = PyObject_Call(typ, PyTuple_Pack(1, int(1)), std::ptr::null_mut());
        assert_eq!(repr(t), "<class 'int'>");
        let isinstance = PyDict_GetItemString(b, c"isinstance".as_ptr());
        assert_eq!(repr(PyObject_Call(isinstance, PyTuple_Pack(2, piper_true(), t), std::ptr::null_mut())), "True");
    }
}

#[test]
fn errors_carry_type_and_message() {
    let _g = init();
    unsafe {
        let ve = piper_exc_type(c"ValueError".as_ptr());
        PyErr_SetString(ve, c"bad value".as_ptr());
        assert!(!PyErr_Occurred().is_null());
        assert_eq!(PyErr_ExceptionMatches(piper_exc_type(c"Exception".as_ptr())), 1);
        assert_eq!(PyErr_ExceptionMatches(piper_exc_type(c"KeyError".as_ptr())), 0);
        let e = PyErr_GetRaisedException();
        assert!(PyErr_Occurred().is_null());
        assert_eq!(str_(e), "bad value");
        assert_eq!(repr(e), "ValueError('bad value')");
        let args = PyObject_GetAttrString(e, c"args".as_ptr());
        assert_eq!(repr(args), "('bad value',)");
        PyErr_SetRaisedException(e);
        assert!(!PyErr_Occurred().is_null());
        PyErr_Clear();
        assert!(PyErr_Occurred().is_null());
    }
}

#[test]
fn modules_have_dicts() {
    let _g = init();
    unsafe {
        let m = PyModule_New(c"mymod".as_ptr());
        let d = PyModule_GetDict(m);
        PyDict_SetItemString(d, c"x".as_ptr(), int(1));
        assert_eq!(PyLong_AsLongLong(PyObject_GetAttrString(m, c"x".as_ptr())), 1);
        assert_eq!(PyObject_SetAttrString(m, c"y".as_ptr(), int(2)), 0);
        assert_eq!(PyLong_AsLongLong(PyDict_GetItemString(d, c"y".as_ptr())), 2);
        assert_eq!(repr(m), "<module 'mymod'>");
        let sysm = PyImport_AddModule(c"sys".as_ptr());
        assert!(!sysm.is_null());
        let _ = CStr::from_ptr(c"".as_ptr());
    }
}

#[test]
fn capsules_preserve_opaque_pointers_names_and_context() {
    let _g = init();
    unsafe {
        let name = c"sample.pointer";
        let other = c"sample.other";
        let mut first = 7u64;
        let mut second = 9u64;
        let mut context = 11u64;
        let capsule = PyCapsule_New((&mut first as *mut u64).cast(), name.as_ptr(), None);
        assert!(!capsule.is_null(), "{}", err());
        assert_eq!(PyCapsule_IsValid(capsule, name.as_ptr()), 1);
        assert_eq!(PyCapsule_IsValid(capsule, other.as_ptr()), 0);
        assert_eq!(PyCapsule_GetPointer(capsule, name.as_ptr()), (&mut first as *mut u64).cast());
        assert_eq!(CStr::from_ptr(PyCapsule_GetName(capsule)), name);
        assert_eq!(PyCapsule_SetContext(capsule, (&mut context as *mut u64).cast()), 0);
        assert_eq!(PyCapsule_GetContext(capsule), (&mut context as *mut u64).cast());
        assert_eq!(PyCapsule_SetPointer(capsule, (&mut second as *mut u64).cast()), 0);
        assert_eq!(PyCapsule_GetPointer(capsule, name.as_ptr()), (&mut second as *mut u64).cast());
        assert!(PyCapsule_GetPointer(capsule, other.as_ptr()).is_null());
        assert_eq!(err(), "ValueError: PyCapsule_GetPointer called with incorrect name");
        Py_DecRef(capsule);
    }
}
