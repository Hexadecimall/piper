/* The builtins module. */
#include "internal.h"

static PyObject *builtins_dict = NULL;
PyObject *piper_builtins(void) { return builtins_dict; }
PyObject *PyEval_GetBuiltins(void) { return builtins_dict; }

/* ---- print ---------------------------------------------------------------- */

static PyObject *b_print(PyObject *m, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    PIPER_UNUSED(m);
    PyObject *sep = NULL, *end = NULL, *file = NULL, *flush = NULL;
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kwnames, i), *v = args[nargs + i];
        if (PyUnicode_EqualToUTF8(k, "sep")) sep = v; else if (PyUnicode_EqualToUTF8(k, "end")) end = v; else if (PyUnicode_EqualToUTF8(k, "file")) file = v; else if (PyUnicode_EqualToUTF8(k, "flush")) flush = v;
        else { PyErr_Format(PyExc_TypeError, "print() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    if (sep == Py_None) sep = NULL;
    if (end == Py_None) end = NULL;
    if (sep && !PyUnicode_Check(sep)) { PyErr_Format(PyExc_TypeError, "sep must be None or a string, not %s", Py_TYPE(sep)->tp_name); return NULL; }
    if (end && !PyUnicode_Check(end)) { PyErr_Format(PyExc_TypeError, "end must be None or a string, not %s", Py_TYPE(end)->tp_name); return NULL; }
    if (!file || file == Py_None) { file = piper_sys_stdout(); if (!file) return NULL; if (file == Py_None) Py_RETURN_NONE; }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        if (i) { if (sep) { if (piper_file_write(file, sep) < 0) return NULL; } else { PyObject *sp = piper_intern(" "); if (piper_file_write(file, sp) < 0) return NULL; } }
        PyObject *s = PyObject_Str(args[i]);
        if (!s) return NULL;
        int r = piper_file_write(file, s);
        Py_DECREF(s);
        if (r < 0) return NULL;
    }
    if (end) { if (piper_file_write(file, end) < 0) return NULL; } else { PyObject *nl = piper_intern("\n"); if (piper_file_write(file, nl) < 0) return NULL; }
    if (flush && PyObject_IsTrue(flush) > 0) { if (piper_file_flush(file) < 0) return NULL; }
    Py_RETURN_NONE;
}

PyObject *piper_builtin_print(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    return b_print(NULL, args, nargs, kwnames);
}

/* ---- simple builtins ------------------------------------------------------- */

static PyObject *b_repr(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyObject_Repr(o); }
static PyObject *b_ascii(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyObject_ASCII(o); }
static PyObject *b_len(PyObject *m, PyObject *o) { PIPER_UNUSED(m); Py_ssize_t n = PyObject_Size(o); if (n < 0) return NULL; return PyLong_FromSsize_t(n); }
static PyObject *b_abs(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyNumber_Absolute(o); }
static PyObject *b_hash(PyObject *m, PyObject *o) { PIPER_UNUSED(m); Py_hash_t h = PyObject_Hash(o); if (h == -1 && PyErr_Occurred()) return NULL; return PyLong_FromSsize_t(h); }
static PyObject *b_id(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyLong_FromVoidPtr(o); }
static PyObject *b_callable(PyObject *m, PyObject *o) { PIPER_UNUSED(m); Py_RETURN_BOOL(PyCallable_Check(o)); }
static PyObject *b_chr(PyObject *m, PyObject *o) { PIPER_UNUSED(m); long v = PyLong_AsLong(o); if (v == -1 && PyErr_Occurred()) { if (PyErr_ExceptionMatches(PyExc_OverflowError)) { PyErr_Clear(); PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C int"); } return NULL; } if (v < 0 || v > 0x10ffff) { PyErr_SetString(PyExc_ValueError, "chr() arg not in range(0x110000)"); return NULL; } return PyUnicode_FromOrdinal((int)v); }
static PyObject *b_ord(PyObject *m, PyObject *o) {
    PIPER_UNUSED(m);
    if (PyUnicode_Check(o)) { if (PyUnicode_GET_LENGTH(o) != 1) { PyErr_Format(PyExc_TypeError, "ord() expected a character, but string of length %zd found", PyUnicode_GET_LENGTH(o)); return NULL; } return PyLong_FromLong((long)PyUnicode_READ_CHAR(o, 0)); }
    if (PyBytes_Check(o)) { if (Py_SIZE(o) != 1) { PyErr_Format(PyExc_TypeError, "ord() expected a character, but string of length %zd found", Py_SIZE(o)); return NULL; } return PyLong_FromLong((unsigned char)PyBytes_AS_STRING(o)[0]); }
    PyErr_Format(PyExc_TypeError, "ord() expected string of length 1, but %s found", Py_TYPE(o)->tp_name);
    return NULL;
}
static PyObject *b_hex(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyNumber_ToBase(o, 16); }
static PyObject *b_oct(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyNumber_ToBase(o, 8); }
static PyObject *b_bin(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyNumber_ToBase(o, 2); }
static PyObject *b_iter(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("iter", n, 1, 2) < 0) return NULL; if (n == 2) { if (!PyCallable_Check(a[0])) { PyErr_SetString(PyExc_TypeError, "iter(v, w): v must be callable"); return NULL; } return PyCallIter_New(a[0], a[1]); } return PyObject_GetIter(a[0]); }
static PyObject *b_next(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m);
    if (piper_args_range("next", n, 1, 2) < 0) return NULL;
    if (!PyIter_Check(a[0])) { PyErr_Format(PyExc_TypeError, "'%s' object is not an iterator", Py_TYPE(a[0])->tp_name); return NULL; }
    PyObject *r = Py_TYPE(a[0])->tp_iternext(a[0]);
    if (r) return r;
    if (PyErr_Occurred()) { if (n == 2 && PyErr_ExceptionMatches(PyExc_StopIteration)) { PyErr_Clear(); return Py_NewRef(a[1]); } return NULL; }
    if (n == 2) return Py_NewRef(a[1]);
    PyErr_SetNone(PyExc_StopIteration);
    return NULL;
}
static PyObject *b_isinstance(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("isinstance", n, 2, 2) < 0) return NULL; int r = PyObject_IsInstance(a[0], a[1]); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *b_issubclass(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("issubclass", n, 2, 2) < 0) return NULL; int r = PyObject_IsSubclass(a[0], a[1]); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *b_getattr(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m);
    if (piper_args_range("getattr", n, 2, 3) < 0) return NULL;
    if (!PyUnicode_Check(a[1])) { PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%s'", Py_TYPE(a[1])->tp_name); return NULL; }
    PyObject *r = PyObject_GetAttr(a[0], a[1]);
    if (!r && n == 3 && PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); return Py_NewRef(a[2]); }
    return r;
}
static PyObject *b_setattr(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("setattr", n, 3, 3) < 0) return NULL; if (PyObject_SetAttr(a[0], a[1], a[2]) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *b_delattr(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("delattr", n, 2, 2) < 0) return NULL; if (PyObject_SetAttr(a[0], a[1], NULL) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *b_hasattr(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("hasattr", n, 2, 2) < 0) return NULL; if (!PyUnicode_Check(a[1])) { PyErr_SetString(PyExc_TypeError, "hasattr(): attribute name must be string"); return NULL; } int r = PyObject_HasAttrWithError(a[0], a[1]); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *b_divmod(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("divmod", n, 2, 2) < 0) return NULL; return PyNumber_Divmod(a[0], a[1]); }
static PyObject *b_pow(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(m);
    PyObject *base = NULL, *exp = NULL, *mod = Py_None;
    if (n >= 1) base = a[0]; if (n >= 2) exp = a[1]; if (n >= 3) mod = a[2];
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) { PyObject *k = PyTuple_GET_ITEM(kw, i); if (PyUnicode_EqualToUTF8(k, "base")) base = a[n + i]; else if (PyUnicode_EqualToUTF8(k, "exp")) exp = a[n + i]; else if (PyUnicode_EqualToUTF8(k, "mod")) mod = a[n + i]; else { PyErr_Format(PyExc_TypeError, "pow() got an unexpected keyword argument '%U'", k); return NULL; } }
    if (!base || !exp) { PyErr_SetString(PyExc_TypeError, "pow() missing required argument"); return NULL; }
    return PyNumber_Power(base, exp, mod);
}
static PyObject *b_round(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(m);
    PyObject *number = n >= 1 ? a[0] : NULL, *nd = n >= 2 ? a[1] : NULL;
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) { PyObject *k = PyTuple_GET_ITEM(kw, i); if (PyUnicode_EqualToUTF8(k, "number")) number = a[n + i]; else if (PyUnicode_EqualToUTF8(k, "ndigits")) nd = a[n + i]; else { PyErr_Format(PyExc_TypeError, "round() got an unexpected keyword argument '%U'", k); return NULL; } }
    if (!number) { PyErr_SetString(PyExc_TypeError, "round() missing required argument 'number' (pos 1)"); return NULL; }
    PyObject *meth = piper_type_lookup(Py_TYPE(number), piper_intern("__round__"));
    if (!meth) { PyErr_Format(PyExc_TypeError, "type %s doesn't define __round__ method", Py_TYPE(number)->tp_name); return NULL; }
    PyObject *args[2] = { number, nd };
    return PyObject_Vectorcall(meth, args, nd ? 2 : 1, NULL);
}
static PyObject *b_format(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("format", n, 1, 2) < 0) return NULL; if (n == 2 && !PyUnicode_Check(a[1])) { PyErr_Format(PyExc_TypeError, "format() argument 2 must be str, not %s", Py_TYPE(a[1])->tp_name); return NULL; } return PyObject_Format(a[0], n == 2 ? a[1] : NULL); }

static PyObject *minmax(PyObject *const *a, Py_ssize_t n, PyObject *kw, int op, const char *name) {
    PyObject *key = NULL, *dflt = NULL;
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) { PyObject *k = PyTuple_GET_ITEM(kw, i); if (PyUnicode_EqualToUTF8(k, "key")) key = a[n + i]; else if (PyUnicode_EqualToUTF8(k, "default")) dflt = a[n + i]; else { PyErr_Format(PyExc_TypeError, "%s() got an unexpected keyword argument '%U'", name, k); return NULL; } }
    if (key == Py_None) key = NULL;
    PyObject *it;
    if (n == 0) { PyErr_Format(PyExc_TypeError, "%s expected at least 1 argument, got 0", name); return NULL; }
    if (n == 1) { it = PyObject_GetIter(a[0]); if (!it) return NULL; }
    else { if (dflt) { PyErr_Format(PyExc_TypeError, "Cannot specify a default for %s() with multiple positional arguments", name); return NULL; } PyObject *t = PyTuple_FromArray(a, n); it = PyObject_GetIter(t); Py_DECREF(t); }
    PyObject *best = NULL, *bestkey = NULL;
    for (;;) {
        PyObject *x = PyIter_Next(it);
        if (!x) break;
        PyObject *kx = key ? PyObject_CallOneArg(key, x) : Py_NewRef(x);
        if (!kx) { Py_DECREF(x); goto fail; }
        if (!best) { best = x; bestkey = kx; continue; }
        int c = PyObject_RichCompareBool(kx, bestkey, op);
        if (c < 0) { Py_DECREF(x); Py_DECREF(kx); goto fail; }
        if (c) { Py_DECREF(best); Py_DECREF(bestkey); best = x; bestkey = kx; } else { Py_DECREF(x); Py_DECREF(kx); }
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) { Py_XDECREF(best); Py_XDECREF(bestkey); return NULL; }
    if (!best) { if (dflt) return Py_NewRef(dflt); PyErr_Format(PyExc_ValueError, "%s() iterable argument is empty", name); return NULL; }
    Py_DECREF(bestkey);
    return best;
fail:
    Py_DECREF(it); Py_XDECREF(best); Py_XDECREF(bestkey);
    return NULL;
}
static PyObject *b_min(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) { PIPER_UNUSED(m); return minmax(a, n, kw, Py_LT, "min"); }
static PyObject *b_max(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) { PIPER_UNUSED(m); return minmax(a, n, kw, Py_GT, "max"); }

static PyObject *b_sum(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(m);
    PyObject *start = n >= 2 ? a[1] : NULL;
    if (kw && PyTuple_GET_SIZE(kw) == 1 && PyUnicode_EqualToUTF8(PyTuple_GET_ITEM(kw, 0), "start")) start = a[n];
    if (n < 1) { PyErr_SetString(PyExc_TypeError, "sum() takes at least 1 positional argument (0 given)"); return NULL; }
    if (start && (PyUnicode_Check(start) || PyBytes_Check(start))) { PyErr_SetString(PyExc_TypeError, PyUnicode_Check(start) ? "sum() can't sum strings [use ''.join(seq) instead]" : "sum() can't sum bytes [use b''.join(seq) instead]"); return NULL; }
    PyObject *it = PyObject_GetIter(a[0]);
    if (!it) return NULL;
    PyObject *acc = start ? Py_NewRef(start) : PyLong_FromLong(0);
    /* fast paths for int and float */
    for (;;) {
        PyObject *x = PyIter_Next(it);
        if (!x) break;
        PyObject *t;
        if (PyFloat_CheckExact(acc) && PyFloat_CheckExact(x)) t = PyFloat_FromDouble(PyFloat_AS_DOUBLE(acc) + PyFloat_AS_DOUBLE(x));
        else t = PyNumber_Add(acc, x);
        Py_DECREF(x); Py_DECREF(acc);
        if (!t) { Py_DECREF(it); return NULL; }
        acc = t;
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) { Py_DECREF(acc); return NULL; }
    return acc;
}
static PyObject *b_any(PyObject *m, PyObject *o) { PIPER_UNUSED(m); PyObject *it = PyObject_GetIter(o); if (!it) return NULL; for (;;) { PyObject *x = PyIter_Next(it); if (!x) break; int t = PyObject_IsTrue(x); Py_DECREF(x); if (t < 0) { Py_DECREF(it); return NULL; } if (t) { Py_DECREF(it); Py_RETURN_TRUE; } } Py_DECREF(it); if (PyErr_Occurred()) return NULL; Py_RETURN_FALSE; }
static PyObject *b_all(PyObject *m, PyObject *o) { PIPER_UNUSED(m); PyObject *it = PyObject_GetIter(o); if (!it) return NULL; for (;;) { PyObject *x = PyIter_Next(it); if (!x) break; int t = PyObject_IsTrue(x); Py_DECREF(x); if (t < 0) { Py_DECREF(it); return NULL; } if (!t) { Py_DECREF(it); Py_RETURN_FALSE; } } Py_DECREF(it); if (PyErr_Occurred()) return NULL; Py_RETURN_TRUE; }
static PyObject *b_sorted(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(m);
    if (n != 1) { PyErr_Format(PyExc_TypeError, "sorted expected 1 argument, got %zd", n); return NULL; }
    PyObject *l = PySequence_List(a[0]);
    if (!l) return NULL;
    PyObject *key = NULL; int reverse = 0;
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) { PyObject *k = PyTuple_GET_ITEM(kw, i); if (PyUnicode_EqualToUTF8(k, "key")) key = a[n + i]; else if (PyUnicode_EqualToUTF8(k, "reverse")) { reverse = PyObject_IsTrue(a[n + i]); if (reverse < 0) { Py_DECREF(l); return NULL; } } else { Py_DECREF(l); PyErr_Format(PyExc_TypeError, "sorted() got an unexpected keyword argument '%U'", k); return NULL; } }
    if (piper_list_sort_impl(l, key, reverse) < 0) { Py_DECREF(l); return NULL; }
    return l;
}
static PyObject *b_vars(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m);
    if (piper_args_range("vars", n, 0, 1) < 0) return NULL;
    if (n == 0) { PyObject *g = piper_frame_globals(); return g ? Py_NewRef(g) : PyDict_New(); }
    PyObject *d;
    if (PyObject_GetOptionalAttrString(a[0], "__dict__", &d) < 0) return NULL;
    if (!d) { PyErr_SetString(PyExc_TypeError, "vars() argument must have __dict__ attribute"); return NULL; }
    return d;
}
static PyObject *b_dir(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m);
    if (piper_args_range("dir", n, 0, 1) < 0) return NULL;
    if (n == 1) return PyObject_Dir(a[0]);
    PyObject *g = piper_frame_globals();
    PyObject *keys = g ? PyDict_Keys(g) : PyList_New(0);
    if (keys) PyList_Sort(keys);
    return keys;
}
static PyObject *b_globals(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); PyObject *g = piper_frame_globals(); return g ? Py_NewRef(g) : PyDict_New(); }
static PyObject *b_locals(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); PyObject *g = piper_frame_globals(); return g ? Py_NewRef(g) : PyDict_New(); }
static PyObject *b_input(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m);
    if (piper_args_range("input", n, 0, 1) < 0) return NULL;
    PyObject *out = piper_sys_stdout();
    if (n == 1 && out && out != Py_None) { PyObject *s = PyObject_Str(a[0]); if (!s) return NULL; piper_file_write(out, s); Py_DECREF(s); piper_file_flush(out); }
    else if (out && out != Py_None) piper_file_flush(out);
    PyObject *in = PySys_GetObject("stdin");
    if (in && in != Py_None) {
        PyObject *line = PyObject_CallMethod(in, "readline", NULL);
        if (!line) return NULL;
        if (!PyUnicode_Check(line)) { Py_DECREF(line); PyErr_SetString(PyExc_TypeError, "object.readline() returned non-string"); return NULL; }
        Py_ssize_t len = PyUnicode_GET_LENGTH(line);
        if (len == 0) { Py_DECREF(line); PyErr_SetNone(PyExc_EOFError); return NULL; }
        if (PyUnicode_READ_CHAR(line, len - 1) == '\n') { PyObject *t = PyUnicode_Substring(line, 0, len - 1); Py_DECREF(line); line = t; }
        return line;
    }
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) { PyErr_SetNone(PyExc_EOFError); return NULL; }
    size_t l = strlen(buf);
    if (l && buf[l - 1] == '\n') buf[--l] = 0;
    return PyUnicode_FromStringAndSize(buf, (Py_ssize_t)l);
}

PyObject *piper_builtin_input(PyObject *const *args, Py_ssize_t nargs) {
    return b_input(NULL, args, nargs);
}
static PyObject *b_open(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) { PIPER_UNUSED(m); return piper_open_impl(a, n, kw); }
static PyObject *b_import(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(m);
    PyObject *name = n >= 1 ? a[0] : NULL, *globals = n >= 2 ? a[1] : NULL, *locals = n >= 3 ? a[2] : NULL, *fromlist = n >= 4 ? a[3] : NULL;
    int level = 0;
    if (n >= 5) { level = (int)PyLong_AsLong(a[4]); if (level == -1 && PyErr_Occurred()) return NULL; }
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) { PyObject *k = PyTuple_GET_ITEM(kw, i); PyObject *v = a[n + i]; if (PyUnicode_EqualToUTF8(k, "name")) name = v; else if (PyUnicode_EqualToUTF8(k, "globals")) globals = v; else if (PyUnicode_EqualToUTF8(k, "locals")) locals = v; else if (PyUnicode_EqualToUTF8(k, "fromlist")) fromlist = v; else if (PyUnicode_EqualToUTF8(k, "level")) { level = (int)PyLong_AsLong(v); if (level == -1 && PyErr_Occurred()) return NULL; } }
    if (!name) { PyErr_SetString(PyExc_TypeError, "__import__() missing required argument 'name'"); return NULL; }
    return PyImport_ImportModuleLevelObject(name, globals, locals, fromlist, level);
}
static PyObject *b_build_class(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(m);
    if (n < 2) { PyErr_SetString(PyExc_TypeError, "__build_class__: not enough arguments"); return NULL; }
    PyObject *func = a[0], *name = a[1];
    PyObject *bases = PyTuple_FromArray(a + 2, n - 2);
    PyObject *kwds = NULL;
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    if (nkw) { kwds = PyDict_New(); for (Py_ssize_t i = 0; i < nkw; i++) PyDict_SetItem(kwds, PyTuple_GET_ITEM(kw, i), a[n + i]); }
    PyObject *r = piper_make_class(func, name, bases, kwds, NULL);
    Py_DECREF(bases); Py_XDECREF(kwds);
    return r;
}
static PyObject *b_exit(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("exit", n, 0, 1) < 0) return NULL; PyErr_SetObject(PyExc_SystemExit, n ? a[0] : Py_None); return NULL; }
static PyObject *b_breakpoint(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) { PIPER_UNUSED(m); PIPER_UNUSED(a); PIPER_UNUSED(n); PIPER_UNUSED(kw); Py_RETURN_NONE; }
static PyObject *b_aiter(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return piper_get_aiter(o); }
static PyObject *b_anext(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("anext", n, 1, 2) < 0) return NULL; PyObject *r = piper_get_anext(a[0]); if (!r && n == 2) { extern PyObject *piper_anext_with_default(PyObject *, PyObject *); PyErr_Clear(); return piper_anext_with_default(a[0], a[1]); } return r; }
static PyObject *b_eval(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); PIPER_UNUSED(a); PIPER_UNUSED(n); PyErr_SetString(PyExc_NotImplementedError, "eval() is not available in compiled programs yet"); return NULL; }
static PyObject *b_exec(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); PIPER_UNUSED(a); PIPER_UNUSED(n); PyErr_SetString(PyExc_NotImplementedError, "exec() is not available in compiled programs yet"); return NULL; }
static PyObject *b_compile(PyObject *m, PyObject *const *a, Py_ssize_t n, PyObject *kw) { PIPER_UNUSED(m); PIPER_UNUSED(a); PIPER_UNUSED(n); PIPER_UNUSED(kw); PyErr_SetString(PyExc_NotImplementedError, "compile() is not available in compiled programs yet"); return NULL; }

static PyMethodDef builtin_methods[] = {
    { "print", (PyCFunction)(void (*)(void))b_print, METH_FASTCALL | METH_KEYWORDS, "Prints the values to a stream, or to sys.stdout by default." },
    { "repr", b_repr, METH_O, NULL }, { "ascii", b_ascii, METH_O, NULL }, { "len", b_len, METH_O, NULL }, { "abs", b_abs, METH_O, NULL },
    { "hash", b_hash, METH_O, NULL }, { "id", b_id, METH_O, NULL }, { "callable", b_callable, METH_O, NULL }, { "chr", b_chr, METH_O, NULL },
    { "ord", b_ord, METH_O, NULL }, { "hex", b_hex, METH_O, NULL }, { "oct", b_oct, METH_O, NULL }, { "bin", b_bin, METH_O, NULL },
    { "iter", (PyCFunction)(void (*)(void))b_iter, METH_FASTCALL, NULL }, { "next", (PyCFunction)(void (*)(void))b_next, METH_FASTCALL, NULL },
    { "isinstance", (PyCFunction)(void (*)(void))b_isinstance, METH_FASTCALL, NULL }, { "issubclass", (PyCFunction)(void (*)(void))b_issubclass, METH_FASTCALL, NULL },
    { "getattr", (PyCFunction)(void (*)(void))b_getattr, METH_FASTCALL, NULL }, { "setattr", (PyCFunction)(void (*)(void))b_setattr, METH_FASTCALL, NULL },
    { "delattr", (PyCFunction)(void (*)(void))b_delattr, METH_FASTCALL, NULL }, { "hasattr", (PyCFunction)(void (*)(void))b_hasattr, METH_FASTCALL, NULL },
    { "divmod", (PyCFunction)(void (*)(void))b_divmod, METH_FASTCALL, NULL }, { "pow", (PyCFunction)(void (*)(void))b_pow, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "round", (PyCFunction)(void (*)(void))b_round, METH_FASTCALL | METH_KEYWORDS, NULL }, { "format", (PyCFunction)(void (*)(void))b_format, METH_FASTCALL, NULL },
    { "min", (PyCFunction)(void (*)(void))b_min, METH_FASTCALL | METH_KEYWORDS, NULL }, { "max", (PyCFunction)(void (*)(void))b_max, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "sum", (PyCFunction)(void (*)(void))b_sum, METH_FASTCALL | METH_KEYWORDS, NULL }, { "any", b_any, METH_O, NULL }, { "all", b_all, METH_O, NULL },
    { "sorted", (PyCFunction)(void (*)(void))b_sorted, METH_FASTCALL | METH_KEYWORDS, NULL }, { "vars", (PyCFunction)(void (*)(void))b_vars, METH_FASTCALL, NULL },
    { "dir", (PyCFunction)(void (*)(void))b_dir, METH_FASTCALL, NULL }, { "globals", b_globals, METH_NOARGS, NULL }, { "locals", b_locals, METH_NOARGS, NULL },
    { "input", (PyCFunction)(void (*)(void))b_input, METH_FASTCALL, NULL }, { "open", (PyCFunction)(void (*)(void))b_open, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "__import__", (PyCFunction)(void (*)(void))b_import, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "__build_class__", (PyCFunction)(void (*)(void))b_build_class, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "exit", (PyCFunction)(void (*)(void))b_exit, METH_FASTCALL, NULL }, { "quit", (PyCFunction)(void (*)(void))b_exit, METH_FASTCALL, NULL },
    { "breakpoint", (PyCFunction)(void (*)(void))b_breakpoint, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "aiter", b_aiter, METH_O, NULL }, { "anext", (PyCFunction)(void (*)(void))b_anext, METH_FASTCALL, NULL },
    { "eval", (PyCFunction)(void (*)(void))b_eval, METH_FASTCALL, NULL }, { "exec", (PyCFunction)(void (*)(void))b_exec, METH_FASTCALL, NULL },
    { "compile", (PyCFunction)(void (*)(void))b_compile, METH_FASTCALL | METH_KEYWORDS, NULL },
    { NULL, NULL, 0, NULL },
};

extern PyTypeObject PyGenericAlias_Type, PyUnion_Type;

void piper_init_builtins_core(void) {
    if (builtins_dict) return;
    PyObject *m = piper_new_stdlib_module("builtins");
    builtins_dict = PyModule_GetDict(m);
    struct { const char *name; PyObject *o; } consts[] = {
        { "None", Py_None }, { "True", Py_True }, { "False", Py_False }, { "NotImplemented", Py_NotImplemented }, { "Ellipsis", Py_Ellipsis },
        { NULL, NULL },
    };
    for (int i = 0; consts[i].name; i++) PyDict_SetItemString(builtins_dict, consts[i].name, consts[i].o);
    PyDict_SetItemString(builtins_dict, "__debug__", Py_True);
    PyDict_SetItemString(builtins_dict, "__name__", piper_intern("builtins"));
}

void piper_init_builtins(void) {
    piper_init_builtins_core();
    PyObject *m = PyDict_GetItemString(piper_modules_dict(), "builtins");
    struct { const char *name; PyObject *o; } consts[] = {
        { "object", (PyObject *)&PyBaseObject_Type }, { "type", (PyObject *)&PyType_Type }, { "int", (PyObject *)&PyLong_Type }, { "bool", (PyObject *)&PyBool_Type },
        { "float", (PyObject *)&PyFloat_Type }, { "complex", (PyObject *)&PyComplex_Type }, { "str", (PyObject *)&PyUnicode_Type }, { "bytes", (PyObject *)&PyBytes_Type },
        { "bytearray", (PyObject *)&PyByteArray_Type }, { "memoryview", (PyObject *)&PyMemoryView_Type }, { "list", (PyObject *)&PyList_Type }, { "tuple", (PyObject *)&PyTuple_Type },
        { "dict", (PyObject *)&PyDict_Type }, { "set", (PyObject *)&PySet_Type }, { "frozenset", (PyObject *)&PyFrozenSet_Type }, { "slice", (PyObject *)&PySlice_Type },
        { "range", (PyObject *)&PyRange_Type }, { "enumerate", (PyObject *)&PyEnum_Type }, { "zip", (PyObject *)&PyZip_Type }, { "map", (PyObject *)&PyMap_Type },
        { "filter", (PyObject *)&PyFilter_Type }, { "reversed", (PyObject *)&PyReversed_Type }, { "property", (PyObject *)&PyProperty_Type },
        { "staticmethod", (PyObject *)&PyStaticMethod_Type }, { "classmethod", (PyObject *)&PyClassMethod_Type }, { "super", (PyObject *)&PySuper_Type },
        { NULL, NULL },
    };
    for (int i = 0; consts[i].name; i++) PyDict_SetItemString(builtins_dict, consts[i].name, consts[i].o);
    PyModule_AddFunctions(m, builtin_methods);
    const char *excs[] = { "BaseException", "BaseExceptionGroup", "ExceptionGroup", "Exception", "TypeError", "StopIteration", "StopAsyncIteration", "GeneratorExit", "KeyboardInterrupt", "SystemExit",
        "ArithmeticError", "OverflowError", "ZeroDivisionError", "FloatingPointError", "AssertionError", "LookupError", "IndexError", "KeyError", "ValueError", "UnicodeError",
        "UnicodeDecodeError", "UnicodeEncodeError", "UnicodeTranslateError", "RuntimeError", "RecursionError", "NotImplementedError", "PythonFinalizationError", "MemoryError", "SystemError",
        "ReferenceError", "BufferError", "EOFError", "ImportError", "ModuleNotFoundError", "NameError", "UnboundLocalError", "AttributeError", "SyntaxError", "IndentationError", "TabError",
        "OSError", "FileNotFoundError", "FileExistsError", "PermissionError", "IsADirectoryError", "NotADirectoryError", "TimeoutError", "BlockingIOError", "InterruptedError",
        "ChildProcessError", "ProcessLookupError", "ConnectionError", "BrokenPipeError", "ConnectionAbortedError", "ConnectionRefusedError", "ConnectionResetError",
        "Warning", "UserWarning", "DeprecationWarning", "PendingDeprecationWarning", "SyntaxWarning", "RuntimeWarning", "FutureWarning", "ImportWarning", "UnicodeWarning",
        "BytesWarning", "ResourceWarning", "EncodingWarning", NULL };
    for (int i = 0; excs[i]; i++) { PyObject *e = piper_exc_type(excs[i]); if (e) PyDict_SetItemString(builtins_dict, excs[i], e); }
    PyDict_SetItemString(builtins_dict, "EnvironmentError", PyExc_OSError);
    PyDict_SetItemString(builtins_dict, "IOError", PyExc_OSError);
}

PyObject *piper_debug_true(void) { return Py_True; }
