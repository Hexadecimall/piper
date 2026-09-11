/* Dunder methods defined in a class body drive the C-level slots of heap
 * types: __add__ -> nb_add, __getitem__ -> mp_subscript, and so on. */
#include "internal.h"

static PyObject *call_dunder(PyObject *self, const char *name, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *m = piper_type_lookup(Py_TYPE(self), piper_intern(name));
    if (!m) { PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", Py_TYPE(self)->tp_name, name); return NULL; }
    PyObject *stack[8];
    stack[0] = self;
    for (Py_ssize_t i = 0; i < nargs; i++) stack[i + 1] = args[i];
    descrgetfunc get = Py_TYPE(m)->tp_descr_get;
    if (get && !(Py_TYPE(m)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR)) {
        PyObject *bound = get(m, self, (PyObject *)Py_TYPE(self));
        if (!bound) return NULL;
        PyObject *r = PyObject_Vectorcall(bound, args, (size_t)nargs, NULL);
        Py_DECREF(bound);
        return r;
    }
    return PyObject_Vectorcall(m, stack, (size_t)nargs + 1, NULL);
}
static int has_dunder(PyTypeObject *tp, const char *name) { return piper_type_lookup(tp, piper_intern(name)) != NULL; }

#define SLOT_UNARY(fn, name) static PyObject *fn(PyObject *self) { return call_dunder(self, name, NULL, 0); }
SLOT_UNARY(slot_repr, "__repr__")
SLOT_UNARY(slot_str, "__str__")
SLOT_UNARY(slot_iter, "__iter__")
SLOT_UNARY(slot_neg, "__neg__")
SLOT_UNARY(slot_pos, "__pos__")
SLOT_UNARY(slot_abs, "__abs__")
SLOT_UNARY(slot_invert, "__invert__")
SLOT_UNARY(slot_int, "__int__")
SLOT_UNARY(slot_float, "__float__")
SLOT_UNARY(slot_index, "__index__")
SLOT_UNARY(slot_await, "__await__")
SLOT_UNARY(slot_aiter, "__aiter__")
SLOT_UNARY(slot_anext, "__anext__")

static PyObject *slot_next(PyObject *self) {
    PyObject *r = call_dunder(self, "__next__", NULL, 0);
    return r;
}

/* Binary ops: try self.__op__, then other.__rop__ when types differ. */
static PyObject *binary_slot(PyObject *a, PyObject *b, const char *name, const char *rname) {
    int a_has = has_dunder(Py_TYPE(a), name);
    int b_has = !Py_IS_TYPE(b, Py_TYPE(a)) && has_dunder(Py_TYPE(b), rname);
    /* If b is a subclass of a's type overriding rop, try it first. */
    if (b_has && PyType_IsSubtype(Py_TYPE(b), Py_TYPE(a))) {
        PyObject *r = call_dunder(b, rname, &a, 1);
        if (!r) return NULL;
        if (r != Py_NotImplemented) return r;
        Py_DECREF(r);
        b_has = 0;
    }
    if (a_has) {
        PyObject *r = call_dunder(a, name, &b, 1);
        if (!r) return NULL;
        if (r != Py_NotImplemented) return r;
        Py_DECREF(r);
    }
    if (b_has) {
        PyObject *r = call_dunder(b, rname, &a, 1);
        if (!r) return NULL;
        return r;
    }
    Py_RETURN_NOTIMPLEMENTED;
}
#define SLOT_BINARY(fn, name, rname) static PyObject *fn(PyObject *a, PyObject *b) { return binary_slot(a, b, name, rname); }
SLOT_BINARY(slot_add, "__add__", "__radd__")
SLOT_BINARY(slot_sub, "__sub__", "__rsub__")
SLOT_BINARY(slot_mul, "__mul__", "__rmul__")
SLOT_BINARY(slot_matmul, "__matmul__", "__rmatmul__")
SLOT_BINARY(slot_truediv, "__truediv__", "__rtruediv__")
SLOT_BINARY(slot_floordiv, "__floordiv__", "__rfloordiv__")
SLOT_BINARY(slot_mod, "__mod__", "__rmod__")
SLOT_BINARY(slot_divmod, "__divmod__", "__rdivmod__")
SLOT_BINARY(slot_lshift, "__lshift__", "__rlshift__")
SLOT_BINARY(slot_rshift, "__rshift__", "__rrshift__")
SLOT_BINARY(slot_and, "__and__", "__rand__")
SLOT_BINARY(slot_or, "__or__", "__ror__")
SLOT_BINARY(slot_xor, "__xor__", "__rxor__")
static PyObject *slot_pow(PyObject *a, PyObject *b, PyObject *z) {
    if (z == Py_None) return binary_slot(a, b, "__pow__", "__rpow__");
    if (has_dunder(Py_TYPE(a), "__pow__")) { PyObject *args[2] = { b, z }; return call_dunder(a, "__pow__", args, 2); }
    Py_RETURN_NOTIMPLEMENTED;
}
#define SLOT_INPLACE(fn, name) static PyObject *fn(PyObject *a, PyObject *b) { if (!has_dunder(Py_TYPE(a), name)) Py_RETURN_NOTIMPLEMENTED; return call_dunder(a, name, &b, 1); }
SLOT_INPLACE(slot_iadd, "__iadd__")
SLOT_INPLACE(slot_isub, "__isub__")
SLOT_INPLACE(slot_imul, "__imul__")
SLOT_INPLACE(slot_imatmul, "__imatmul__")
SLOT_INPLACE(slot_itruediv, "__itruediv__")
SLOT_INPLACE(slot_ifloordiv, "__ifloordiv__")
SLOT_INPLACE(slot_imod, "__imod__")
SLOT_INPLACE(slot_ilshift, "__ilshift__")
SLOT_INPLACE(slot_irshift, "__irshift__")
SLOT_INPLACE(slot_iand, "__iand__")
SLOT_INPLACE(slot_ior, "__ior__")
SLOT_INPLACE(slot_ixor, "__ixor__")
static PyObject *slot_ipow(PyObject *a, PyObject *b, PyObject *z) { PIPER_UNUSED(z); if (!has_dunder(Py_TYPE(a), "__ipow__")) Py_RETURN_NOTIMPLEMENTED; return call_dunder(a, "__ipow__", &b, 1); }

static int slot_bool(PyObject *self) {
    PyObject *r;
    if (has_dunder(Py_TYPE(self), "__bool__")) {
        r = call_dunder(self, "__bool__", NULL, 0);
        if (!r) return -1;
        if (!PyBool_Check(r)) { PyErr_Format(PyExc_TypeError, "__bool__ should return bool, returned %s", Py_TYPE(r)->tp_name); Py_DECREF(r); return -1; }
        int t = r == Py_True;
        Py_DECREF(r);
        return t;
    }
    if (has_dunder(Py_TYPE(self), "__len__")) {
        r = call_dunder(self, "__len__", NULL, 0);
        if (!r) return -1;
        Py_ssize_t n = PyNumber_AsSsize_t(r, PyExc_OverflowError);
        Py_DECREF(r);
        if (n == -1 && PyErr_Occurred()) return -1;
        if (n < 0) { PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0"); return -1; }
        return n != 0;
    }
    return 1;
}
static Py_ssize_t slot_len(PyObject *self) {
    PyObject *r = call_dunder(self, "__len__", NULL, 0);
    if (!r) return -1;
    Py_ssize_t n = PyNumber_AsSsize_t(r, PyExc_OverflowError);
    Py_DECREF(r);
    if (n == -1 && PyErr_Occurred()) return -1;
    if (n < 0) { PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0"); return -1; }
    return n;
}
static PyObject *slot_getitem(PyObject *self, PyObject *k) { return call_dunder(self, "__getitem__", &k, 1); }
static int slot_setitem(PyObject *self, PyObject *k, PyObject *v) {
    PyObject *r;
    if (v) { PyObject *args[2] = { k, v }; r = call_dunder(self, "__setitem__", args, 2); }
    else r = call_dunder(self, "__delitem__", &k, 1);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
static PyObject *slot_sq_item(PyObject *self, Py_ssize_t i) { PyObject *idx = PyLong_FromSsize_t(i); PyObject *r = call_dunder(self, "__getitem__", &idx, 1); Py_DECREF(idx); return r; }
static int slot_contains(PyObject *self, PyObject *v) {
    if (has_dunder(Py_TYPE(self), "__contains__")) {
        PyObject *r = call_dunder(self, "__contains__", &v, 1);
        if (!r) return -1;
        int t = PyObject_IsTrue(r);
        Py_DECREF(r);
        return t;
    }
    /* fall back to iteration */
    PyObject *it = PyObject_GetIter(self);
    if (!it) return -1;
    for (;;) { PyObject *x = PyIter_Next(it); if (!x) break; int c = PyObject_RichCompareBool(x, v, Py_EQ); Py_DECREF(x); if (c) { Py_DECREF(it); return c; } }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
static Py_hash_t slot_hash(PyObject *self) {
    PyObject *r = call_dunder(self, "__hash__", NULL, 0);
    if (!r) return -1;
    if (!PyLong_Check(r)) { Py_DECREF(r); PyErr_SetString(PyExc_TypeError, "__hash__ method should return an integer"); return -1; }
    Py_hash_t h;
    if (piper_long_is_compact((PyLongObject *)r)) h = (Py_hash_t)piper_long_compact_value((PyLongObject *)r);
    else h = PyObject_Hash(r);
    Py_DECREF(r);
    return h == -1 ? -2 : h;
}
static PyObject *slot_call(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *m = piper_type_lookup(Py_TYPE(self), piper_intern("__call__"));
    if (!m) { PyErr_Format(PyExc_TypeError, "'%s' object is not callable", Py_TYPE(self)->tp_name); return NULL; }
    descrgetfunc get = Py_TYPE(m)->tp_descr_get;
    PyObject *bound = get ? get(m, self, (PyObject *)Py_TYPE(self)) : Py_NewRef(m);
    if (!bound) return NULL;
    PyObject *r = PyObject_Call(bound, args, kwds);
    Py_DECREF(bound);
    return r;
}
static const char *const cmp_names[] = { "__lt__", "__le__", "__eq__", "__ne__", "__gt__", "__ge__" };
static PyObject *slot_richcompare(PyObject *a, PyObject *b, int op) {
    if (!has_dunder(Py_TYPE(a), cmp_names[op])) Py_RETURN_NOTIMPLEMENTED;
    return call_dunder(a, cmp_names[op], &b, 1);
}
static PyObject *slot_getattro(PyObject *self, PyObject *name) {
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *ga = piper_type_lookup(tp, piper_intern("__getattribute__"));
    PyObject *r;
    if (ga && !(Py_IS_TYPE(ga, &PyWrapperDescr_Type))) {
        r = call_dunder(self, "__getattribute__", &name, 1);
    } else r = PyObject_GenericGetAttr(self, name);
    if (r || !PyErr_ExceptionMatches(PyExc_AttributeError)) return r;
    PyObject *hook = piper_type_lookup(tp, piper_intern("__getattr__"));
    if (!hook) return NULL;
    PyErr_Clear();
    return call_dunder(self, "__getattr__", &name, 1);
}
static int slot_setattro(PyObject *self, PyObject *name, PyObject *v) {
    PyObject *r;
    if (v) { PyObject *args[2] = { name, v }; r = call_dunder(self, "__setattr__", args, 2); }
    else r = call_dunder(self, "__delattr__", &name, 1);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
static PyObject *slot_descr_get(PyObject *self, PyObject *obj, PyObject *type) {
    PyObject *args[2] = { obj ? obj : Py_None, type ? type : Py_None };
    return call_dunder(self, "__get__", args, 2);
}
static int slot_descr_set(PyObject *self, PyObject *obj, PyObject *v) {
    PyObject *r;
    if (v) { PyObject *args[2] = { obj, v }; r = call_dunder(self, "__set__", args, 2); }
    else r = call_dunder(self, "__delete__", &obj, 1);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
static int slot_init(PyObject *self, PyObject *args, PyObject *kwds) {
    PyObject *m = piper_type_lookup(Py_TYPE(self), piper_intern("__init__"));
    if (!m) return 0;
    if (Py_IS_TYPE(m, &PyWrapperDescr_Type)) return ((PyTypeObject *)((PyObject **)m)[2])->tp_init ? ((PyTypeObject *)((PyObject **)m)[2])->tp_init(self, args, kwds) : 0;
    descrgetfunc get = Py_TYPE(m)->tp_descr_get;
    PyObject *bound = get ? get(m, self, (PyObject *)Py_TYPE(self)) : Py_NewRef(m);
    if (!bound) return -1;
    PyObject *r = PyObject_Call(bound, args, kwds);
    Py_DECREF(bound);
    if (!r) return -1;
    if (r != Py_None) { PyErr_Format(PyExc_TypeError, "__init__() should return None, not '%s'", Py_TYPE(r)->tp_name); Py_DECREF(r); return -1; }
    Py_DECREF(r);
    return 0;
}
static PyObject *slot_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PyObject *m = piper_type_lookup(tp, piper_intern("__new__"));
    if (!m) return PyType_GenericNew(tp, args, kwds);
    /* staticmethod wrapping __new__ */
    PyObject *fn = Py_IS_TYPE(m, &PyStaticMethod_Type) ? PyObject_GetAttrString(m, "__func__") : Py_NewRef(m);
    if (!fn) return NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    PyObject *full = PyTuple_New(n + 1);
    PyTuple_SET_ITEM(full, 0, Py_NewRef((PyObject *)tp));
    for (Py_ssize_t i = 0; i < n; i++) PyTuple_SET_ITEM(full, i + 1, Py_NewRef(PyTuple_GET_ITEM(args, i)));
    PyObject *r = PyObject_Call(fn, full, kwds);
    Py_DECREF(full); Py_DECREF(fn);
    return r;
}
static void slot_finalize(PyObject *self) {
    PyObject *e = PyErr_GetRaisedException();
    PyObject *r = call_dunder(self, "__del__", NULL, 0);
    if (!r) PyErr_WriteUnraisable(self); else Py_DECREF(r);
    PyErr_SetRaisedException(e);
}
static int slot_getbuffer(PyObject *self, Py_buffer *view, int flags) {
    PyObject *f = PyLong_FromLong(flags);
    PyObject *r = call_dunder(self, "__buffer__", &f, 1);
    Py_DECREF(f);
    if (!r) return -1;
    if (!PyObject_CheckBuffer(r)) { Py_DECREF(r); PyErr_SetString(PyExc_TypeError, "__buffer__ returned non-memoryview object"); return -1; }
    int rc = PyObject_GetBuffer(r, view, flags);
    Py_DECREF(r);
    return rc;
}

/* Slot wrapper descriptors expose C slots as dunder methods (e.g. so
 * `int.__add__` exists and `super().__init__` reaches object's tp_init). */
typedef struct { PyObject_HEAD PyObject *name; PyTypeObject *type; const char *cname; int kind; } wrapperdescr;
static void wd_dealloc(PyObject *o) { Py_XDECREF(((wrapperdescr *)o)->name); PyObject_Free(o); }
static PyObject *wd_repr(PyObject *o) { wrapperdescr *w = (wrapperdescr *)o; return PyUnicode_FromFormat("<slot wrapper '%U' of '%s' objects>", w->name, w->type->tp_name); }

enum { WK_INIT, WK_NEW, WK_REPR, WK_STR, WK_HASH, WK_EQ, WK_NE, WK_LT, WK_LE, WK_GT, WK_GE, WK_GETATTRIBUTE, WK_SETATTR, WK_DELATTR, WK_LEN, WK_GETITEM, WK_SETITEM, WK_DELITEM, WK_ITER, WK_NEXT, WK_CONTAINS, WK_CALL, WK_BOOL,
       WK_ADD, WK_SUB, WK_MUL, WK_TRUEDIV, WK_FLOORDIV, WK_MOD, WK_POW, WK_NEG, WK_POS, WK_ABS, WK_INVERT, WK_LSHIFT, WK_RSHIFT, WK_AND, WK_OR, WK_XOR, WK_INT, WK_FLOAT, WK_INDEX, WK_MATMUL, WK_DIVMOD,
       WK_RADD, WK_RSUB, WK_RMUL, WK_RTRUEDIV, WK_RFLOORDIV, WK_RMOD, WK_RPOW, WK_RLSHIFT, WK_RRSHIFT, WK_RAND, WK_ROR, WK_RXOR, WK_RMATMUL, WK_RDIVMOD,
       WK_IADD, WK_ISUB, WK_IMUL, WK_ITRUEDIV, WK_IFLOORDIV, WK_IMOD, WK_IPOW, WK_ILSHIFT, WK_IRSHIFT, WK_IAND, WK_IOR, WK_IXOR, WK_IMATMUL, WK_GET, WK_SET, WK_DELETE };

static PyObject *wd_call_impl(wrapperdescr *w, PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    PyTypeObject *tp = w->type;
    PyObject *tuple, *kwd = NULL;
#define NEED(n) if (nargs != (n)) { PyErr_Format(PyExc_TypeError, "expected %d argument%s, got %zd", n, (n) == 1 ? "" : "s", nargs); return NULL; }
#define BIN(slot) NEED(1); if (!tp->tp_as_number || !tp->tp_as_number->slot) Py_RETURN_NOTIMPLEMENTED; return tp->tp_as_number->slot(self, args[0]);
#define RBIN(slot) NEED(1); if (!tp->tp_as_number || !tp->tp_as_number->slot) Py_RETURN_NOTIMPLEMENTED; return tp->tp_as_number->slot(args[0], self);
#define UN(slot) NEED(0); if (!tp->tp_as_number || !tp->tp_as_number->slot) { PyErr_SetString(PyExc_TypeError, "bad operand"); return NULL; } return tp->tp_as_number->slot(self);
    switch (w->kind) {
    case WK_INIT: case WK_CALL: {
        tuple = PyTuple_FromArray(args, nargs);
        if (kwnames && PyTuple_GET_SIZE(kwnames)) { kwd = PyDict_New(); for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) PyDict_SetItem(kwd, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]); }
        PyObject *r;
        if (w->kind == WK_INIT) { int rc = tp->tp_init ? tp->tp_init(self, tuple, kwd) : 0; r = rc < 0 ? NULL : Py_NewRef(Py_None); }
        else r = tp->tp_call(self, tuple, kwd);
        Py_DECREF(tuple); Py_XDECREF(kwd);
        return r;
    }
    case WK_NEW: {
        if (!PyType_Check(self)) { PyErr_Format(PyExc_TypeError, "%s.__new__(X): X is not a type object", tp->tp_name); return NULL; }
        PyTypeObject *sub = (PyTypeObject *)self;
        if (!PyType_IsSubtype(sub, tp)) { PyErr_Format(PyExc_TypeError, "%s.__new__(%s): %s is not a subtype of %s", tp->tp_name, sub->tp_name, sub->tp_name, tp->tp_name); return NULL; }
        tuple = PyTuple_FromArray(args, nargs);
        if (kwnames && PyTuple_GET_SIZE(kwnames)) { kwd = PyDict_New(); for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) PyDict_SetItem(kwd, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]); }
        PyObject *r = tp->tp_new(sub, tuple, kwd);
        Py_DECREF(tuple); Py_XDECREF(kwd);
        return r;
    }
    case WK_REPR: NEED(0); return tp->tp_repr(self);
    case WK_STR: NEED(0); return tp->tp_str ? tp->tp_str(self) : tp->tp_repr(self);
    case WK_HASH: { NEED(0); Py_hash_t h = tp->tp_hash(self); if (h == -1 && PyErr_Occurred()) return NULL; return PyLong_FromSsize_t(h); }
    case WK_EQ: case WK_NE: case WK_LT: case WK_LE: case WK_GT: case WK_GE: {
        NEED(1);
        int op = w->kind == WK_EQ ? Py_EQ : w->kind == WK_NE ? Py_NE : w->kind == WK_LT ? Py_LT : w->kind == WK_LE ? Py_LE : w->kind == WK_GT ? Py_GT : Py_GE;
        if (!tp->tp_richcompare) Py_RETURN_NOTIMPLEMENTED;
        return tp->tp_richcompare(self, args[0], op);
    }
    case WK_GETATTRIBUTE: NEED(1); if (!PyUnicode_Check(args[0])) { PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%s'", Py_TYPE(args[0])->tp_name); return NULL; } return tp->tp_getattro(self, args[0]);
    case WK_SETATTR: { NEED(2); if (tp->tp_setattro(self, args[0], args[1]) < 0) return NULL; Py_RETURN_NONE; }
    case WK_DELATTR: { NEED(1); if (tp->tp_setattro(self, args[0], NULL) < 0) return NULL; Py_RETURN_NONE; }
    case WK_LEN: { NEED(0); Py_ssize_t n = tp->tp_as_mapping && tp->tp_as_mapping->mp_length ? tp->tp_as_mapping->mp_length(self) : tp->tp_as_sequence->sq_length(self); if (n < 0) return NULL; return PyLong_FromSsize_t(n); }
    case WK_GETITEM: NEED(1); if (tp->tp_as_mapping && tp->tp_as_mapping->mp_subscript) return tp->tp_as_mapping->mp_subscript(self, args[0]); { Py_ssize_t i = PyNumber_AsSsize_t(args[0], PyExc_IndexError); if (i == -1 && PyErr_Occurred()) return NULL; return tp->tp_as_sequence->sq_item(self, i); }
    case WK_SETITEM: { NEED(2); int rc = tp->tp_as_mapping && tp->tp_as_mapping->mp_ass_subscript ? tp->tp_as_mapping->mp_ass_subscript(self, args[0], args[1]) : PySequence_SetItem(self, PyNumber_AsSsize_t(args[0], PyExc_IndexError), args[1]); if (rc < 0) return NULL; Py_RETURN_NONE; }
    case WK_DELITEM: { NEED(1); int rc = tp->tp_as_mapping && tp->tp_as_mapping->mp_ass_subscript ? tp->tp_as_mapping->mp_ass_subscript(self, args[0], NULL) : PySequence_DelItem(self, PyNumber_AsSsize_t(args[0], PyExc_IndexError)); if (rc < 0) return NULL; Py_RETURN_NONE; }
    case WK_ITER: NEED(0); return tp->tp_iter(self);
    case WK_NEXT: { NEED(0); PyObject *r = tp->tp_iternext(self); if (!r && !PyErr_Occurred()) PyErr_SetNone(PyExc_StopIteration); return r; }
    case WK_CONTAINS: { NEED(1); int r = tp->tp_as_sequence->sq_contains(self, args[0]); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
    case WK_BOOL: { NEED(0); int r = tp->tp_as_number->nb_bool(self); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
    case WK_ADD: BIN(nb_add) case WK_SUB: BIN(nb_subtract) case WK_MUL: BIN(nb_multiply) case WK_TRUEDIV: BIN(nb_true_divide) case WK_FLOORDIV: BIN(nb_floor_divide)
    case WK_MOD: BIN(nb_remainder) case WK_LSHIFT: BIN(nb_lshift) case WK_RSHIFT: BIN(nb_rshift) case WK_AND: BIN(nb_and) case WK_OR: BIN(nb_or) case WK_XOR: BIN(nb_xor) case WK_MATMUL: BIN(nb_matrix_multiply) case WK_DIVMOD: BIN(nb_divmod)
    case WK_RADD: RBIN(nb_add) case WK_RSUB: RBIN(nb_subtract) case WK_RMUL: RBIN(nb_multiply) case WK_RTRUEDIV: RBIN(nb_true_divide) case WK_RFLOORDIV: RBIN(nb_floor_divide)
    case WK_RMOD: RBIN(nb_remainder) case WK_RLSHIFT: RBIN(nb_lshift) case WK_RRSHIFT: RBIN(nb_rshift) case WK_RAND: RBIN(nb_and) case WK_ROR: RBIN(nb_or) case WK_RXOR: RBIN(nb_xor) case WK_RMATMUL: RBIN(nb_matrix_multiply) case WK_RDIVMOD: RBIN(nb_divmod)
    case WK_IADD: BIN(nb_inplace_add) case WK_ISUB: BIN(nb_inplace_subtract) case WK_IMUL: BIN(nb_inplace_multiply) case WK_ITRUEDIV: BIN(nb_inplace_true_divide) case WK_IFLOORDIV: BIN(nb_inplace_floor_divide)
    case WK_IMOD: BIN(nb_inplace_remainder) case WK_ILSHIFT: BIN(nb_inplace_lshift) case WK_IRSHIFT: BIN(nb_inplace_rshift) case WK_IAND: BIN(nb_inplace_and) case WK_IOR: BIN(nb_inplace_or) case WK_IXOR: BIN(nb_inplace_xor) case WK_IMATMUL: BIN(nb_inplace_matrix_multiply)
    case WK_POW: { if (nargs < 1 || nargs > 2) { PyErr_SetString(PyExc_TypeError, "expected 1 or 2 arguments"); return NULL; } if (!tp->tp_as_number || !tp->tp_as_number->nb_power) Py_RETURN_NOTIMPLEMENTED; return tp->tp_as_number->nb_power(self, args[0], nargs == 2 ? args[1] : Py_None); }
    case WK_RPOW: { NEED(1); if (!tp->tp_as_number || !tp->tp_as_number->nb_power) Py_RETURN_NOTIMPLEMENTED; return tp->tp_as_number->nb_power(args[0], self, Py_None); }
    case WK_IPOW: { NEED(1); if (!tp->tp_as_number || !tp->tp_as_number->nb_inplace_power) Py_RETURN_NOTIMPLEMENTED; return tp->tp_as_number->nb_inplace_power(self, args[0], Py_None); }
    case WK_NEG: UN(nb_negative) case WK_POS: UN(nb_positive) case WK_ABS: UN(nb_absolute) case WK_INVERT: UN(nb_invert) case WK_INT: UN(nb_int) case WK_FLOAT: UN(nb_float) case WK_INDEX: UN(nb_index)
    case WK_GET: { if (nargs < 1 || nargs > 2) { PyErr_SetString(PyExc_TypeError, "__get__ expected 1 or 2 arguments"); return NULL; } PyObject *obj = args[0] == Py_None ? NULL : args[0]; PyObject *type = nargs == 2 && args[1] != Py_None ? args[1] : NULL; if (!obj && !type) { PyErr_SetString(PyExc_TypeError, "__get__(None, None) is invalid"); return NULL; } return tp->tp_descr_get(self, obj, type); }
    case WK_SET: { NEED(2); if (tp->tp_descr_set(self, args[0], args[1]) < 0) return NULL; Py_RETURN_NONE; }
    case WK_DELETE: { NEED(1); if (tp->tp_descr_set(self, args[0], NULL) < 0) return NULL; Py_RETURN_NONE; }
    }
#undef NEED
#undef BIN
#undef RBIN
#undef UN
    PyErr_SetString(PyExc_SystemError, "bad slot wrapper");
    return NULL;
}
static PyObject *wd_vectorcall(PyObject *o, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    wrapperdescr *w = (wrapperdescr *)o;
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs < 1) { PyErr_Format(PyExc_TypeError, "descriptor '%U' of '%s' object needs an argument", w->name, w->type->tp_name); return NULL; }
    int valid = w->kind == WK_NEW ? PyType_Check(args[0]) && PyType_IsSubtype((PyTypeObject *)args[0], w->type) : PyObject_TypeCheck(args[0], w->type);
    if (!valid) { PyErr_Format(PyExc_TypeError, "descriptor '%U' requires a '%s' object but received a '%s'", w->name, w->type->tp_name, Py_TYPE(args[0])->tp_name); return NULL; }
    return wd_call_impl(w, args[0], args + 1, nargs - 1, kwnames);
}
static PyObject *wd_call(PyObject *o, PyObject *args, PyObject *kwds) { return PyObject_VectorcallDict(o, PyTuple_ITEMS(args), (size_t)PyTuple_GET_SIZE(args), kwds); }
typedef struct { PyObject_HEAD wrapperdescr *descr; PyObject *self; vectorcallfunc vectorcall; } methodwrapper;
static void mw_dealloc(PyObject *o) { methodwrapper *m = (methodwrapper *)o; Py_XDECREF(m->descr); Py_XDECREF(m->self); PyObject_Free(o); }
static PyObject *mw_vectorcall(PyObject *o, PyObject *const *args, size_t nargsf, PyObject *kwnames) { methodwrapper *m = (methodwrapper *)o; return wd_call_impl(m->descr, m->self, args, PyVectorcall_NARGS(nargsf), kwnames); }
static PyObject *mw_repr(PyObject *o) { methodwrapper *m = (methodwrapper *)o; return PyUnicode_FromFormat("<method-wrapper '%U' of %s object at %p>", m->descr->name, Py_TYPE(m->self)->tp_name, m->self); }
static PyObject *mw_self_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((methodwrapper *)o)->self); }
static PyObject *mw_name_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((methodwrapper *)o)->descr->name); }
static PyGetSetDef mw_getsets[] = { { "__self__", mw_self_get, NULL, NULL, NULL }, { "__name__", mw_name_get, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyMethodWrapper_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "method-wrapper", .tp_basicsize = sizeof(methodwrapper), .tp_dealloc = mw_dealloc, .tp_vectorcall_offset = offsetof(methodwrapper, vectorcall), .tp_repr = mw_repr, .tp_call = PyVectorcall_Call, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL, .tp_getset = mw_getsets };
static PyObject *wd_get(PyObject *o, PyObject *obj, PyObject *type) {
    PIPER_UNUSED(type);
    if (!obj || ((wrapperdescr *)o)->kind == WK_NEW) return Py_NewRef(o);
    methodwrapper *m = PyObject_New(methodwrapper, &PyMethodWrapper_Type);
    if (!m) return NULL;
    m->descr = (wrapperdescr *)Py_NewRef(o);
    m->self = Py_NewRef(obj);
    m->vectorcall = mw_vectorcall;
    return (PyObject *)m;
}
static PyObject *wd_name_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((wrapperdescr *)o)->name); }
static PyObject *wd_objclass_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef((PyObject *)((wrapperdescr *)o)->type); }
static PyObject *wd_qualname_get(PyObject *o, void *c) { PIPER_UNUSED(c); wrapperdescr *w = (wrapperdescr *)o; PyObject *tn = PyType_GetQualName(w->type); PyObject *r = PyUnicode_FromFormat("%U.%U", tn, w->name); Py_DECREF(tn); return r; }
static PyObject *wd_doc_get(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); Py_RETURN_NONE; }
static PyGetSetDef wd_getsets[] = { { "__name__", wd_name_get, NULL, NULL, NULL }, { "__qualname__", wd_qualname_get, NULL, NULL, NULL }, { "__objclass__", wd_objclass_get, NULL, NULL, NULL }, { "__doc__", wd_doc_get, NULL, NULL, NULL }, { "__text_signature__", wd_doc_get, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyWrapperDescr_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "wrapper_descriptor", .tp_basicsize = sizeof(wrapperdescr) + sizeof(vectorcallfunc), .tp_dealloc = wd_dealloc,
    .tp_vectorcall_offset = sizeof(wrapperdescr), .tp_repr = wd_repr, .tp_call = wd_call, .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_METHOD_DESCRIPTOR, .tp_getset = wd_getsets, .tp_descr_get = wd_get,
};

static const struct { const char *name; int kind; } wrapper_table[] = {
    { "__init__", WK_INIT }, { "__new__", WK_NEW }, { "__repr__", WK_REPR }, { "__str__", WK_STR }, { "__hash__", WK_HASH },
    { "__eq__", WK_EQ }, { "__ne__", WK_NE }, { "__lt__", WK_LT }, { "__le__", WK_LE }, { "__gt__", WK_GT }, { "__ge__", WK_GE },
    { "__getattribute__", WK_GETATTRIBUTE }, { "__setattr__", WK_SETATTR }, { "__delattr__", WK_DELATTR }, { "__len__", WK_LEN },
    { "__getitem__", WK_GETITEM }, { "__setitem__", WK_SETITEM }, { "__delitem__", WK_DELITEM }, { "__iter__", WK_ITER }, { "__next__", WK_NEXT },
    { "__contains__", WK_CONTAINS }, { "__call__", WK_CALL }, { "__bool__", WK_BOOL },
    { "__add__", WK_ADD }, { "__sub__", WK_SUB }, { "__mul__", WK_MUL }, { "__truediv__", WK_TRUEDIV }, { "__floordiv__", WK_FLOORDIV }, { "__mod__", WK_MOD }, { "__pow__", WK_POW },
    { "__neg__", WK_NEG }, { "__pos__", WK_POS }, { "__abs__", WK_ABS }, { "__invert__", WK_INVERT }, { "__lshift__", WK_LSHIFT }, { "__rshift__", WK_RSHIFT }, { "__and__", WK_AND }, { "__or__", WK_OR }, { "__xor__", WK_XOR },
    { "__int__", WK_INT }, { "__float__", WK_FLOAT }, { "__index__", WK_INDEX }, { "__matmul__", WK_MATMUL }, { "__divmod__", WK_DIVMOD },
    { "__radd__", WK_RADD }, { "__rsub__", WK_RSUB }, { "__rmul__", WK_RMUL }, { "__rtruediv__", WK_RTRUEDIV }, { "__rfloordiv__", WK_RFLOORDIV }, { "__rmod__", WK_RMOD }, { "__rpow__", WK_RPOW },
    { "__rlshift__", WK_RLSHIFT }, { "__rrshift__", WK_RRSHIFT }, { "__rand__", WK_RAND }, { "__ror__", WK_ROR }, { "__rxor__", WK_RXOR }, { "__rmatmul__", WK_RMATMUL }, { "__rdivmod__", WK_RDIVMOD },
    { "__iadd__", WK_IADD }, { "__isub__", WK_ISUB }, { "__imul__", WK_IMUL }, { "__itruediv__", WK_ITRUEDIV }, { "__ifloordiv__", WK_IFLOORDIV }, { "__imod__", WK_IMOD }, { "__ipow__", WK_IPOW },
    { "__ilshift__", WK_ILSHIFT }, { "__irshift__", WK_IRSHIFT }, { "__iand__", WK_IAND }, { "__ior__", WK_IOR }, { "__ixor__", WK_IXOR }, { "__imatmul__", WK_IMATMUL },
    { "__get__", WK_GET }, { "__set__", WK_SET }, { "__delete__", WK_DELETE },
};

static int slot_present(PyTypeObject *tp, int kind) {
    PyNumberMethods *nb = tp->tp_as_number; PySequenceMethods *sq = tp->tp_as_sequence; PyMappingMethods *mp = tp->tp_as_mapping;
    switch (kind) {
    case WK_INIT: return tp->tp_init != NULL; case WK_NEW: return tp->tp_new != NULL; case WK_REPR: return tp->tp_repr != NULL; case WK_STR: return tp->tp_str != NULL;
    case WK_HASH: return tp->tp_hash != NULL && tp->tp_hash != PyObject_HashNotImplemented; case WK_EQ: case WK_NE: case WK_LT: case WK_LE: case WK_GT: case WK_GE: return tp->tp_richcompare != NULL;
    case WK_GETATTRIBUTE: return tp->tp_getattro != NULL; case WK_SETATTR: case WK_DELATTR: return tp->tp_setattro != NULL;
    case WK_LEN: return (mp && mp->mp_length) || (sq && sq->sq_length); case WK_GETITEM: return (mp && mp->mp_subscript) || (sq && sq->sq_item);
    case WK_SETITEM: case WK_DELITEM: return (mp && mp->mp_ass_subscript) || (sq && sq->sq_ass_item); case WK_ITER: return tp->tp_iter != NULL; case WK_NEXT: return tp->tp_iternext != NULL;
    case WK_CONTAINS: return sq && sq->sq_contains; case WK_CALL: return tp->tp_call != NULL; case WK_BOOL: return nb && nb->nb_bool;
    case WK_ADD: case WK_RADD: return nb && nb->nb_add; case WK_SUB: case WK_RSUB: return nb && nb->nb_subtract; case WK_MUL: case WK_RMUL: return nb && nb->nb_multiply;
    case WK_TRUEDIV: case WK_RTRUEDIV: return nb && nb->nb_true_divide; case WK_FLOORDIV: case WK_RFLOORDIV: return nb && nb->nb_floor_divide; case WK_MOD: case WK_RMOD: return nb && nb->nb_remainder;
    case WK_POW: case WK_RPOW: return nb && nb->nb_power; case WK_NEG: return nb && nb->nb_negative; case WK_POS: return nb && nb->nb_positive; case WK_ABS: return nb && nb->nb_absolute; case WK_INVERT: return nb && nb->nb_invert;
    case WK_LSHIFT: case WK_RLSHIFT: return nb && nb->nb_lshift; case WK_RSHIFT: case WK_RRSHIFT: return nb && nb->nb_rshift; case WK_AND: case WK_RAND: return nb && nb->nb_and; case WK_OR: case WK_ROR: return nb && nb->nb_or; case WK_XOR: case WK_RXOR: return nb && nb->nb_xor;
    case WK_INT: return nb && nb->nb_int; case WK_FLOAT: return nb && nb->nb_float; case WK_INDEX: return nb && nb->nb_index; case WK_MATMUL: case WK_RMATMUL: return nb && nb->nb_matrix_multiply; case WK_DIVMOD: case WK_RDIVMOD: return nb && nb->nb_divmod;
    case WK_IADD: return nb && nb->nb_inplace_add; case WK_ISUB: return nb && nb->nb_inplace_subtract; case WK_IMUL: return nb && nb->nb_inplace_multiply; case WK_ITRUEDIV: return nb && nb->nb_inplace_true_divide; case WK_IFLOORDIV: return nb && nb->nb_inplace_floor_divide;
    case WK_IMOD: return nb && nb->nb_inplace_remainder; case WK_IPOW: return nb && nb->nb_inplace_power; case WK_ILSHIFT: return nb && nb->nb_inplace_lshift; case WK_IRSHIFT: return nb && nb->nb_inplace_rshift; case WK_IAND: return nb && nb->nb_inplace_and; case WK_IOR: return nb && nb->nb_inplace_or; case WK_IXOR: return nb && nb->nb_inplace_xor; case WK_IMATMUL: return nb && nb->nb_inplace_matrix_multiply;
    case WK_GET: return tp->tp_descr_get != NULL; case WK_SET: case WK_DELETE: return tp->tp_descr_set != NULL;
    }
    return 0;
}

/* Add slot wrappers to a static type's dict for every C slot it defines. */
void piper_type_add_slot_wrappers(PyTypeObject *tp) {
    if (!tp->tp_dict) return;
    for (size_t i = 0; i < sizeof(wrapper_table) / sizeof(wrapper_table[0]); i++) {
        if (!slot_present(tp, wrapper_table[i].kind)) continue;
        /* inherited slots: only add when the base doesn't already define it identically */
        if (PyDict_GetItemString(tp->tp_dict, wrapper_table[i].name)) continue;
        if (tp->tp_base) {
            PyObject *inherited = piper_type_lookup(tp->tp_base, piper_intern(wrapper_table[i].name));
            if (inherited && Py_IS_TYPE(inherited, &PyWrapperDescr_Type)) {
                wrapperdescr *iw = (wrapperdescr *)inherited;
                int same = 0;
                PyTypeObject *b = iw->type;
                switch (wrapper_table[i].kind) {
                case WK_INIT: same = b->tp_init == tp->tp_init; break; case WK_NEW: same = b->tp_new == tp->tp_new; break; case WK_REPR: same = b->tp_repr == tp->tp_repr; break; case WK_STR: same = b->tp_str == tp->tp_str; break;
                case WK_HASH: same = b->tp_hash == tp->tp_hash; break; case WK_EQ: case WK_NE: case WK_LT: case WK_LE: case WK_GT: case WK_GE: same = b->tp_richcompare == tp->tp_richcompare; break;
                case WK_GETATTRIBUTE: same = b->tp_getattro == tp->tp_getattro; break; case WK_SETATTR: case WK_DELATTR: same = b->tp_setattro == tp->tp_setattro; break;
                case WK_ITER: same = b->tp_iter == tp->tp_iter; break; case WK_NEXT: same = b->tp_iternext == tp->tp_iternext; break; case WK_CALL: same = b->tp_call == tp->tp_call; break;
                case WK_GET: same = b->tp_descr_get == tp->tp_descr_get; break; case WK_SET: case WK_DELETE: same = b->tp_descr_set == tp->tp_descr_set; break;
                default: same = b->tp_as_number == tp->tp_as_number && b->tp_as_sequence == tp->tp_as_sequence && b->tp_as_mapping == tp->tp_as_mapping; break;
                }
                if (same) continue;
            }
        }
        wrapperdescr *w = (wrapperdescr *)PyObject_Malloc(PyWrapperDescr_Type.tp_basicsize);
        if (!w) return;
        PyObject_Init((PyObject *)w, &PyWrapperDescr_Type);
        w->name = PyUnicode_InternFromString(wrapper_table[i].name);
        w->type = tp;
        w->cname = wrapper_table[i].name;
        w->kind = wrapper_table[i].kind;
        *(vectorcallfunc *)((char *)w + sizeof(wrapperdescr)) = wd_vectorcall;
        PyDict_SetItemString(tp->tp_dict, wrapper_table[i].name, (PyObject *)w);
        Py_DECREF(w);
    }
}

/* Does the class (or a heap-type ancestor) define this dunder as something
 * other than an inherited slot wrapper? */
static int defines(PyTypeObject *tp, const char *name) {
    PyObject *v = piper_type_lookup(tp, piper_intern(name));
    return v && !Py_IS_TYPE(v, &PyWrapperDescr_Type);
}

void piper_type_fixup_slots(PyTypeObject *tp) {
    PyNumberMethods *nb = tp->tp_as_number; PySequenceMethods *sq = tp->tp_as_sequence; PyMappingMethods *mp = tp->tp_as_mapping; PyAsyncMethods *am = tp->tp_as_async;
#define FIX(name, slot, fn) if (defines(tp, name)) slot = fn;
    FIX("__repr__", tp->tp_repr, slot_repr)
    FIX("__str__", tp->tp_str, slot_str)
    FIX("__iter__", tp->tp_iter, slot_iter)
    FIX("__next__", tp->tp_iternext, slot_next)
    FIX("__call__", tp->tp_call, slot_call)
    FIX("__getattribute__", tp->tp_getattro, slot_getattro)
    FIX("__getattr__", tp->tp_getattro, slot_getattro)
    FIX("__setattr__", tp->tp_setattro, slot_setattro)
    FIX("__delattr__", tp->tp_setattro, slot_setattro)
    FIX("__get__", tp->tp_descr_get, slot_descr_get)
    FIX("__set__", tp->tp_descr_set, slot_descr_set)
    FIX("__delete__", tp->tp_descr_set, slot_descr_set)
    FIX("__init__", tp->tp_init, slot_init)
    FIX("__new__", tp->tp_new, slot_new)
    FIX("__del__", tp->tp_finalize, slot_finalize)
    for (int i = 0; i < 6; i++) if (defines(tp, cmp_names[i])) tp->tp_richcompare = slot_richcompare;
    if (defines(tp, "__hash__")) {
        PyObject *h = piper_type_lookup(tp, piper_intern("__hash__"));
        tp->tp_hash = h == Py_None ? PyObject_HashNotImplemented : slot_hash;
    } else if (defines(tp, "__eq__") && !PyDict_GetItemString(tp->tp_dict, "__hash__")) {
        /* class defines __eq__ without __hash__: unhashable */
        PyObject *own_eq = PyDict_GetItemString(tp->tp_dict, "__eq__");
        if (own_eq) { tp->tp_hash = PyObject_HashNotImplemented; PyDict_SetItemString(tp->tp_dict, "__hash__", Py_None); }
    }
    FIX("__len__", mp->mp_length, slot_len)
    FIX("__len__", sq->sq_length, slot_len)
    FIX("__getitem__", mp->mp_subscript, slot_getitem)
    FIX("__getitem__", sq->sq_item, slot_sq_item)
    FIX("__setitem__", mp->mp_ass_subscript, slot_setitem)
    FIX("__delitem__", mp->mp_ass_subscript, slot_setitem)
    FIX("__contains__", sq->sq_contains, slot_contains)
    if (defines(tp, "__bool__") || defines(tp, "__len__")) nb->nb_bool = slot_bool;
    FIX("__add__", nb->nb_add, slot_add) FIX("__radd__", nb->nb_add, slot_add)
    FIX("__sub__", nb->nb_subtract, slot_sub) FIX("__rsub__", nb->nb_subtract, slot_sub)
    FIX("__mul__", nb->nb_multiply, slot_mul) FIX("__rmul__", nb->nb_multiply, slot_mul)
    FIX("__matmul__", nb->nb_matrix_multiply, slot_matmul) FIX("__rmatmul__", nb->nb_matrix_multiply, slot_matmul)
    FIX("__truediv__", nb->nb_true_divide, slot_truediv) FIX("__rtruediv__", nb->nb_true_divide, slot_truediv)
    FIX("__floordiv__", nb->nb_floor_divide, slot_floordiv) FIX("__rfloordiv__", nb->nb_floor_divide, slot_floordiv)
    FIX("__mod__", nb->nb_remainder, slot_mod) FIX("__rmod__", nb->nb_remainder, slot_mod)
    FIX("__divmod__", nb->nb_divmod, slot_divmod) FIX("__rdivmod__", nb->nb_divmod, slot_divmod)
    FIX("__pow__", nb->nb_power, slot_pow) FIX("__rpow__", nb->nb_power, slot_pow)
    FIX("__lshift__", nb->nb_lshift, slot_lshift) FIX("__rlshift__", nb->nb_lshift, slot_lshift)
    FIX("__rshift__", nb->nb_rshift, slot_rshift) FIX("__rrshift__", nb->nb_rshift, slot_rshift)
    FIX("__and__", nb->nb_and, slot_and) FIX("__rand__", nb->nb_and, slot_and)
    FIX("__or__", nb->nb_or, slot_or) FIX("__ror__", nb->nb_or, slot_or)
    FIX("__xor__", nb->nb_xor, slot_xor) FIX("__rxor__", nb->nb_xor, slot_xor)
    FIX("__iadd__", nb->nb_inplace_add, slot_iadd) FIX("__isub__", nb->nb_inplace_subtract, slot_isub) FIX("__imul__", nb->nb_inplace_multiply, slot_imul)
    FIX("__imatmul__", nb->nb_inplace_matrix_multiply, slot_imatmul) FIX("__itruediv__", nb->nb_inplace_true_divide, slot_itruediv)
    FIX("__ifloordiv__", nb->nb_inplace_floor_divide, slot_ifloordiv) FIX("__imod__", nb->nb_inplace_remainder, slot_imod) FIX("__ipow__", nb->nb_inplace_power, slot_ipow)
    FIX("__ilshift__", nb->nb_inplace_lshift, slot_ilshift) FIX("__irshift__", nb->nb_inplace_rshift, slot_irshift)
    FIX("__iand__", nb->nb_inplace_and, slot_iand) FIX("__ior__", nb->nb_inplace_or, slot_ior) FIX("__ixor__", nb->nb_inplace_xor, slot_ixor)
    FIX("__neg__", nb->nb_negative, slot_neg) FIX("__pos__", nb->nb_positive, slot_pos) FIX("__abs__", nb->nb_absolute, slot_abs) FIX("__invert__", nb->nb_invert, slot_invert)
    FIX("__int__", nb->nb_int, slot_int) FIX("__float__", nb->nb_float, slot_float) FIX("__index__", nb->nb_index, slot_index)
    FIX("__await__", am->am_await, slot_await) FIX("__aiter__", am->am_aiter, slot_aiter) FIX("__anext__", am->am_anext, slot_anext)
    if (defines(tp, "__buffer__")) { tp->tp_as_buffer->bf_getbuffer = slot_getbuffer; }
#undef FIX
}

void piper_type_update_slot(PyTypeObject *tp, PyObject *name) {
    PIPER_UNUSED(name);
    piper_type_fixup_slots(tp);
}
