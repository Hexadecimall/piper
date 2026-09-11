/* Generic object protocol, type objects, and the singletons. */
#include "internal.h"

/* ---- singletons ------------------------------------------------------- */

static PyObject *none_repr(PyObject *o) { PIPER_UNUSED(o); return PyUnicode_FromString("None"); }
static int none_bool(PyObject *o) { PIPER_UNUSED(o); return 0; }
static PyNumberMethods none_as_number = { .nb_bool = none_bool };
static PyObject *none_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(tp);
    if ((args && PyTuple_GET_SIZE(args)) || (kwds && PyDict_Size(kwds))) { PyErr_SetString(PyExc_TypeError, "NoneType takes no arguments"); return NULL; }
    Py_RETURN_NONE;
}
PyTypeObject _PyNone_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "NoneType", .tp_basicsize = sizeof(PyObject), .tp_repr = none_repr, .tp_as_number = &none_as_number,
    .tp_new = none_new,
};
PyObject _Py_NoneStruct = { { .ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &_PyNone_Type };

static PyObject *notimpl_repr(PyObject *o) { PIPER_UNUSED(o); return PyUnicode_FromString("NotImplemented"); }
PyTypeObject _PyNotImplemented_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "NotImplementedType", .tp_basicsize = sizeof(PyObject), .tp_repr = notimpl_repr,
};
PyObject _Py_NotImplementedStruct = { { .ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &_PyNotImplemented_Type };

static PyObject *ellipsis_repr(PyObject *o) { PIPER_UNUSED(o); return PyUnicode_FromString("Ellipsis"); }
PyTypeObject PyEllipsis_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "ellipsis", .tp_basicsize = sizeof(PyObject), .tp_repr = ellipsis_repr,
};
PyObject _Py_EllipsisObject = { { .ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &PyEllipsis_Type };

PyObject *piper_none(void) { return Py_None; }
PyObject *piper_true(void) { return Py_True; }
PyObject *piper_false(void) { return Py_False; }
PyObject *piper_not_implemented(void) { return Py_NotImplemented; }
PyObject *piper_ellipsis(void) { return Py_Ellipsis; }
PyObject *piper_bool(int v) { return v ? Py_True : Py_False; }
int Py_IsNone(PyObject *o) { return o == Py_None; }
int Py_IsTrue(PyObject *o) { return o == Py_True; }
int Py_IsFalse(PyObject *o) { return o == Py_False; }
int Py_Is(PyObject *a, PyObject *b) { return a == b; }

/* ---- dealloc ----------------------------------------------------------- */

void _Py_Dealloc(PyObject *op) {
    destructor d = Py_TYPE(op)->tp_dealloc;
    if (d) d(op);
    else PyObject_Free(op);
}

static void object_dealloc(PyObject *op) { Py_TYPE(op)->tp_free(op); }

/* ---- hashing ----------------------------------------------------------- */

Py_hash_t _Py_HashPointer(const void *p) {
    size_t y = (size_t)p;
    y = (y >> 4) | (y << (8 * sizeof(void *) - 4));
    Py_hash_t x = (Py_hash_t)y;
    return x == -1 ? -2 : x;
}

Py_hash_t _Py_HashDouble(PyObject *inst, double v) {
    int e, sign;
    double m;
    Py_uhash_t x, y;
    if (!isfinite(v)) {
        if (isinf(v)) return v > 0 ? _PyHASH_INF : -_PyHASH_INF;
        return _Py_HashPointer(inst);
    }
    m = frexp(v, &e);
    sign = 1;
    if (m < 0) { sign = -1; m = -m; }
    x = 0;
    while (m) {
        x = ((x << 28) & _PyHASH_MODULUS) | x >> (_PyHASH_BITS - 28);
        m *= 268435456.0;
        e -= 28;
        y = (Py_uhash_t)m;
        m -= y;
        x += y;
        if (x >= _PyHASH_MODULUS) x -= _PyHASH_MODULUS;
    }
    e = e >= 0 ? e % _PyHASH_BITS : _PyHASH_BITS - 1 - ((-1 - e) % _PyHASH_BITS);
    x = ((x << e) & _PyHASH_MODULUS) | x >> (_PyHASH_BITS - e);
    x = x * (Py_uhash_t)sign;
    if (x == (Py_uhash_t)-1) x = (Py_uhash_t)-2;
    return (Py_hash_t)x;
}

Py_hash_t piper_hash_bytes(const void *p, Py_ssize_t n) {
    /* FNV-1a 64; deterministic across runs. */
    const unsigned char *s = p;
    uint64_t h = 1469598103934665603ULL;
    if (n == 0) return 0;
    for (Py_ssize_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ULL; }
    Py_hash_t x = (Py_hash_t)h;
    return x == -1 ? -2 : x;
}
Py_hash_t _Py_HashBytes(const void *p, Py_ssize_t n) { return piper_hash_bytes(p, n); }

Py_hash_t PyObject_HashNotImplemented(PyObject *o) {
    PyErr_Format(PyExc_TypeError, "unhashable type: '%s'", Py_TYPE(o)->tp_name);
    return -1;
}

Py_hash_t PyObject_Hash(PyObject *o) {
    PyTypeObject *tp = Py_TYPE(o);
    if (tp->tp_hash) return tp->tp_hash(o);
    if (!(tp->tp_flags & Py_TPFLAGS_READY)) { if (PyType_Ready(tp) < 0) return -1; if (tp->tp_hash) return tp->tp_hash(o); }
    return PyObject_HashNotImplemented(o);
}

/* ---- repr / str -------------------------------------------------------- */

static PyObject *repr_stack = NULL;
int Py_ReprEnter(PyObject *o) {
    if (!repr_stack) { repr_stack = PyList_New(0); if (!repr_stack) return -1; }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(repr_stack); i++) if (PyList_GET_ITEM(repr_stack, i) == o) return 1;
    return PyList_Append(repr_stack, o);
}
void Py_ReprLeave(PyObject *o) {
    if (!repr_stack) return;
    for (Py_ssize_t i = PyList_GET_SIZE(repr_stack) - 1; i >= 0; i--) {
        if (PyList_GET_ITEM(repr_stack, i) == o) { PyList_SetSlice(repr_stack, i, i + 1, NULL); return; }
    }
}

PyObject *piper_generic_repr(PyObject *o) {
    return PyUnicode_FromFormat("<%s object at %p>", Py_TYPE(o)->tp_name, o);
}

static int recursion_depth = 0;
#define RECURSION_LIMIT 1000
int Py_EnterRecursiveCall(const char *where) {
    if (++recursion_depth > RECURSION_LIMIT) {
        recursion_depth--;
        PyErr_Format(PyExc_RecursionError, "maximum recursion depth exceeded%s", where ? where : "");
        return -1;
    }
    return 0;
}
void Py_LeaveRecursiveCall(void) { recursion_depth--; }

PyObject *PyObject_Repr(PyObject *o) {
    if (!o) return PyUnicode_FromString("<NULL>");
    reprfunc f = Py_TYPE(o)->tp_repr;
    if (!f) return piper_generic_repr(o);
    if (Py_EnterRecursiveCall(" while getting the repr of an object")) return NULL;
    PyObject *r = f(o);
    Py_LeaveRecursiveCall();
    if (r && !PyUnicode_Check(r)) {
        PyErr_Format(PyExc_TypeError, "__repr__ returned non-string (type %s)", Py_TYPE(r)->tp_name);
        Py_DECREF(r);
        return NULL;
    }
    return r;
}

PyObject *PyObject_Str(PyObject *o) {
    if (!o) return PyUnicode_FromString("<NULL>");
    if (PyUnicode_CheckExact(o)) return Py_NewRef(o);
    reprfunc f = Py_TYPE(o)->tp_str;
    if (!f) return PyObject_Repr(o);
    if (Py_EnterRecursiveCall(" while getting the str of an object")) return NULL;
    PyObject *r = f(o);
    Py_LeaveRecursiveCall();
    if (r && !PyUnicode_Check(r)) {
        PyErr_Format(PyExc_TypeError, "__str__ returned non-string (type %s)", Py_TYPE(r)->tp_name);
        Py_DECREF(r);
        return NULL;
    }
    return r;
}

PyObject *PyObject_ASCII(PyObject *o) {
    PyObject *r = PyObject_Repr(o);
    if (!r) return NULL;
    if (PyUnicode_IS_ASCII(r)) return r;
    /* escape non-ASCII */
    Py_ssize_t n = PyUnicode_GET_LENGTH(r);
    PyObject *out = PyUnicode_New(n * 10, 0x7f);
    Py_ssize_t j = 0;
    char buf[16];
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ_CHAR(r, i);
        if (c < 0x80) { PyUnicode_WriteChar(out, j++, c); continue; }
        int len = c < 0x100 ? snprintf(buf, sizeof buf, "\\x%02x", c) : c < 0x10000 ? snprintf(buf, sizeof buf, "\\u%04x", c) : snprintf(buf, sizeof buf, "\\U%08x", c);
        for (int k = 0; k < len; k++) PyUnicode_WriteChar(out, j++, (Py_UCS4)buf[k]);
    }
    PyObject *res = PyUnicode_Substring(out, 0, j);
    Py_DECREF(out);
    Py_DECREF(r);
    return res;
}

/* ---- truth ------------------------------------------------------------- */

int PyObject_IsTrue(PyObject *o) {
    if (o == Py_True) return 1;
    if (o == Py_False || o == Py_None) return 0;
    PyTypeObject *tp = Py_TYPE(o);
    Py_ssize_t r;
    if (tp->tp_as_number && tp->tp_as_number->nb_bool) r = tp->tp_as_number->nb_bool(o);
    else if (tp->tp_as_mapping && tp->tp_as_mapping->mp_length) r = tp->tp_as_mapping->mp_length(o);
    else if (tp->tp_as_sequence && tp->tp_as_sequence->sq_length) r = tp->tp_as_sequence->sq_length(o);
    else return 1;
    return r > 0 ? 1 : (int)r;
}

int PyObject_Not(PyObject *o) { int r = PyObject_IsTrue(o); return r < 0 ? r : !r; }

/* ---- comparison -------------------------------------------------------- */

static const int swapped_op[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
static const char *const opstrings[] = { "<", "<=", "==", "!=", ">", ">=" };

static PyObject *do_richcompare(PyObject *v, PyObject *w, int op) {
    richcmpfunc f;
    PyObject *res;
    int checked_reverse = 0;
    if (!Py_IS_TYPE(v, Py_TYPE(w)) && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v)) && (f = Py_TYPE(w)->tp_richcompare)) {
        checked_reverse = 1;
        res = f(w, v, swapped_op[op]);
        if (res != Py_NotImplemented) return res;
        Py_DECREF(res);
    }
    if ((f = Py_TYPE(v)->tp_richcompare)) {
        res = f(v, w, op);
        if (res != Py_NotImplemented) return res;
        Py_DECREF(res);
    }
    if (!checked_reverse && (f = Py_TYPE(w)->tp_richcompare)) {
        res = f(w, v, swapped_op[op]);
        if (res != Py_NotImplemented) return res;
        Py_DECREF(res);
    }
    switch (op) {
    case Py_EQ: Py_RETURN_BOOL(v == w);
    case Py_NE: Py_RETURN_BOOL(v != w);
    default:
        PyErr_Format(PyExc_TypeError, "'%s' not supported between instances of '%s' and '%s'", opstrings[op], Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
        return NULL;
    }
}

PyObject *PyObject_RichCompare(PyObject *v, PyObject *w, int op) {
    if (Py_EnterRecursiveCall(" in comparison")) return NULL;
    PyObject *r = do_richcompare(v, w, op);
    Py_LeaveRecursiveCall();
    return r;
}

int PyObject_RichCompareBool(PyObject *v, PyObject *w, int op) {
    if (v == w) { if (op == Py_EQ) return 1; if (op == Py_NE) return 0; }
    PyObject *r = PyObject_RichCompare(v, w, op);
    if (!r) return -1;
    int ok = r == Py_True ? 1 : r == Py_False ? 0 : PyObject_IsTrue(r);
    Py_DECREF(r);
    return ok;
}

PyObject *piper_compare(int op, PyObject *a, PyObject *b) { return PyObject_RichCompare(a, b, op); }

PyObject *piper_is(PyObject *a, PyObject *b, int negate) { Py_RETURN_BOOL((a == b) != (negate != 0)); }

PyObject *piper_contains(PyObject *item, PyObject *container, int negate) {
    int r = PySequence_Contains(container, item);
    if (r < 0) return NULL;
    Py_RETURN_BOOL((r != 0) != (negate != 0));
}

/* ---- attributes -------------------------------------------------------- */

PyObject *PyObject_GetAttr(PyObject *o, PyObject *name) {
    PyTypeObject *tp = Py_TYPE(o);
    if (!PyUnicode_Check(name)) { PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%s'", Py_TYPE(name)->tp_name); return NULL; }
    if (tp->tp_getattro) return tp->tp_getattro(o, name);
    if (tp->tp_getattr) return tp->tp_getattr(o, (char *)PyUnicode_AsUTF8(name));
    PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%U'", tp->tp_name, name);
    return NULL;
}

PyObject *PyObject_GetAttrString(PyObject *o, const char *name) {
    PyObject *n = PyUnicode_FromString(name);
    if (!n) return NULL;
    PyObject *r = PyObject_GetAttr(o, n);
    Py_DECREF(n);
    return r;
}

int PyObject_GetOptionalAttr(PyObject *o, PyObject *name, PyObject **result) {
    *result = PyObject_GetAttr(o, name);
    if (*result) return 1;
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); return 0; }
    return -1;
}

int PyObject_GetOptionalAttrString(PyObject *o, const char *name, PyObject **result) {
    PyObject *n = PyUnicode_FromString(name);
    if (!n) { *result = NULL; return -1; }
    int r = PyObject_GetOptionalAttr(o, n, result);
    Py_DECREF(n);
    return r;
}

int PyObject_HasAttrWithError(PyObject *o, PyObject *name) {
    PyObject *r;
    int rc = PyObject_GetOptionalAttr(o, name, &r);
    Py_XDECREF(r);
    return rc;
}
int PyObject_HasAttr(PyObject *o, PyObject *name) { int r = PyObject_HasAttrWithError(o, name); if (r < 0) { PyErr_Clear(); return 0; } return r; }
int PyObject_HasAttrString(PyObject *o, const char *name) { PyObject *r; int rc = PyObject_GetOptionalAttrString(o, name, &r); Py_XDECREF(r); if (rc < 0) { PyErr_Clear(); return 0; } return rc; }

int PyObject_SetAttr(PyObject *o, PyObject *name, PyObject *v) {
    PyTypeObject *tp = Py_TYPE(o);
    if (!PyUnicode_Check(name)) { PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%s'", Py_TYPE(name)->tp_name); return -1; }
    if (tp->tp_setattro) return tp->tp_setattro(o, name, v);
    if (tp->tp_setattr) return tp->tp_setattr(o, (char *)PyUnicode_AsUTF8(name), v);
    if (tp->tp_getattro == NULL && tp->tp_getattr == NULL)
        PyErr_Format(PyExc_TypeError, "'%s' object has no attributes (%s .%U)", tp->tp_name, v ? "assign to" : "del", name);
    else
        PyErr_Format(PyExc_TypeError, "'%s' object has only read-only attributes (%s .%U)", tp->tp_name, v ? "assign to" : "del", name);
    return -1;
}

int PyObject_SetAttrString(PyObject *o, const char *name, PyObject *v) {
    PyObject *n = PyUnicode_FromString(name);
    if (!n) return -1;
    int r = PyObject_SetAttr(o, n, v);
    Py_DECREF(n);
    return r;
}

int PyObject_DelAttr(PyObject *o, PyObject *name) { return PyObject_SetAttr(o, name, NULL); }

PyObject **_PyObject_GetDictPtr(PyObject *o) {
    Py_ssize_t off = Py_TYPE(o)->tp_dictoffset;
    if (off == 0) return NULL;
    if (off < 0) {
        Py_ssize_t tsize = PyLong_Check(o)
            ? piper_long_ndigits((PyLongObject *)o)
            : Py_SIZE(o);
        if (tsize < 0) tsize = -tsize;
        size_t size = (size_t)Py_TYPE(o)->tp_basicsize + (size_t)tsize * (size_t)Py_TYPE(o)->tp_itemsize;
        size = (size + 7) & ~(size_t)7;
        off += (Py_ssize_t)size;
    }
    return (PyObject **)((char *)o + off);
}

PyObject *PyObject_GenericGetDict(PyObject *o, void *ctx) {
    PIPER_UNUSED(ctx);
    PyObject **dp = _PyObject_GetDictPtr(o);
    if (!dp) { PyErr_SetString(PyExc_AttributeError, "This object has no __dict__"); return NULL; }
    if (!*dp) { *dp = PyDict_New(); if (!*dp) return NULL; }
    return Py_NewRef(*dp);
}

int PyObject_GenericSetDict(PyObject *o, PyObject *v, void *ctx) {
    PIPER_UNUSED(ctx);
    PyObject **dp = _PyObject_GetDictPtr(o);
    if (!dp) { PyErr_SetString(PyExc_AttributeError, "This object has no __dict__"); return -1; }
    if (!v || !PyDict_Check(v)) { PyErr_SetString(PyExc_TypeError, "__dict__ must be set to a dictionary"); return -1; }
    Py_XSETREF(*dp, Py_NewRef(v));
    return 0;
}

PyObject *PyObject_GenericGetAttr(PyObject *o, PyObject *name) {
    PyTypeObject *tp = Py_TYPE(o);
    if (!(tp->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(tp) < 0) return NULL;
    PyObject *descr = piper_type_lookup(tp, name);
    descrgetfunc f = NULL;
    if (descr) {
        Py_INCREF(descr);
        f = Py_TYPE(descr)->tp_descr_get;
        if (f && Py_TYPE(descr)->tp_descr_set) {
            PyObject *r = f(descr, o, (PyObject *)tp);
            Py_DECREF(descr);
            return r;
        }
    }
    if (PyUnicode_EqualToUTF8(name, "__dict__") && _PyObject_GetDictPtr(o)) {
        Py_XDECREF(descr);
        return PyObject_GenericGetDict(o, NULL);
    }
    PyObject **dp = _PyObject_GetDictPtr(o);
    if (dp && *dp) {
        PyObject *r = PyDict_GetItemWithError(*dp, name);
        if (r) { Py_XDECREF(descr); return Py_NewRef(r); }
        if (PyErr_Occurred()) { Py_XDECREF(descr); return NULL; }
    }
    if (f) {
        PyObject *r = f(descr, o, (PyObject *)tp);
        Py_DECREF(descr);
        return r;
    }
    if (descr) return descr;
    PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%U'", tp->tp_name, name);
    return NULL;
}

int PyObject_GenericSetAttr(PyObject *o, PyObject *name, PyObject *v) {
    PyTypeObject *tp = Py_TYPE(o);
    if (!(tp->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(tp) < 0) return -1;
    if (PyUnicode_EqualToUTF8(name, "__dict__") && _PyObject_GetDictPtr(o)) return PyObject_GenericSetDict(o, v, NULL);
    PyObject *descr = piper_type_lookup(tp, name);
    if (descr) {
        descrsetfunc f = Py_TYPE(descr)->tp_descr_set;
        if (f) return f(descr, o, v);
    }
    PyObject **dp = _PyObject_GetDictPtr(o);
    if (!dp) {
        if (descr) PyErr_Format(PyExc_AttributeError, "'%s' object attribute '%U' is read-only", tp->tp_name, name);
        else PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%U' and no __dict__ for setting new attributes", tp->tp_name, name);
        return -1;
    }
    if (!*dp) {
        if (!v) { PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%U'", tp->tp_name, name); return -1; }
        *dp = PyDict_New();
        if (!*dp) return -1;
    }
    if (v) return PyDict_SetItem(*dp, name, v);
    int r = PyDict_DelItem(*dp, name);
    if (r < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_Clear();
        PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%U'", tp->tp_name, name);
    }
    return r;
}

/* ---- items / iteration ------------------------------------------------- */

PyObject *PyObject_GetItem(PyObject *o, PyObject *k) {
    PyTypeObject *tp = Py_TYPE(o);
    if (tp->tp_as_mapping && tp->tp_as_mapping->mp_subscript) return tp->tp_as_mapping->mp_subscript(o, k);
    if (tp->tp_as_sequence && tp->tp_as_sequence->sq_item) {
        if (PyIndex_Check(k)) {
            Py_ssize_t i = PyNumber_AsSsize_t(k, PyExc_IndexError);
            if (i == -1 && PyErr_Occurred()) return NULL;
            return PySequence_GetItem(o, i);
        }
        PyErr_Format(PyExc_TypeError, "sequence index must be integer, not '%s'", Py_TYPE(k)->tp_name);
        return NULL;
    }
    if (PyType_Check(o)) {
        PyObject *meth;
        if (PyObject_GetOptionalAttrString(o, "__class_getitem__", &meth) < 0) return NULL;
        if (meth) { PyObject *r = PyObject_CallOneArg(meth, k); Py_DECREF(meth); return r; }
        PyErr_Format(PyExc_TypeError, "type '%s' is not subscriptable", ((PyTypeObject *)o)->tp_name);
        return NULL;
    }
    PyErr_Format(PyExc_TypeError, "'%s' object is not subscriptable", tp->tp_name);
    return NULL;
}

int PyObject_SetItem(PyObject *o, PyObject *k, PyObject *v) {
    PyTypeObject *tp = Py_TYPE(o);
    if (tp->tp_as_mapping && tp->tp_as_mapping->mp_ass_subscript) return tp->tp_as_mapping->mp_ass_subscript(o, k, v);
    if (tp->tp_as_sequence && tp->tp_as_sequence->sq_ass_item) {
        if (PyIndex_Check(k)) {
            Py_ssize_t i = PyNumber_AsSsize_t(k, PyExc_IndexError);
            if (i == -1 && PyErr_Occurred()) return -1;
            return v ? PySequence_SetItem(o, i, v) : PySequence_DelItem(o, i);
        }
        PyErr_Format(PyExc_TypeError, "sequence index must be integer, not '%s'", Py_TYPE(k)->tp_name);
        return -1;
    }
    if (v) PyErr_Format(PyExc_TypeError, "'%s' object does not support item assignment", tp->tp_name);
    else PyErr_Format(PyExc_TypeError, "'%s' object does not support item deletion", tp->tp_name);
    return -1;
}

int PyObject_DelItem(PyObject *o, PyObject *k) { return PyObject_SetItem(o, k, NULL); }

Py_ssize_t PyObject_Size(PyObject *o) {
    PyTypeObject *tp = Py_TYPE(o);
    if (tp->tp_as_sequence && tp->tp_as_sequence->sq_length) return tp->tp_as_sequence->sq_length(o);
    if (tp->tp_as_mapping && tp->tp_as_mapping->mp_length) return tp->tp_as_mapping->mp_length(o);
    PyErr_Format(PyExc_TypeError, "object of type '%s' has no len()", tp->tp_name);
    return -1;
}
Py_ssize_t PyObject_Length(PyObject *o) { return PyObject_Size(o); }

Py_ssize_t PyObject_LengthHint(PyObject *o, Py_ssize_t dflt) {
    PyTypeObject *tp = Py_TYPE(o);
    if ((tp->tp_as_sequence && tp->tp_as_sequence->sq_length) || (tp->tp_as_mapping && tp->tp_as_mapping->mp_length)) {
        Py_ssize_t n = PyObject_Size(o);
        if (n >= 0) return n;
        if (!PyErr_ExceptionMatches(PyExc_TypeError)) return -1;
        PyErr_Clear();
    }
    PyObject *hint;
    if (PyObject_GetOptionalAttrString(o, "__length_hint__", &hint) < 0) return -1;
    if (!hint) return dflt;
    PyObject *r = PyObject_CallNoArgs(hint);
    Py_DECREF(hint);
    if (!r) { if (PyErr_ExceptionMatches(PyExc_TypeError)) { PyErr_Clear(); return dflt; } return -1; }
    if (r == Py_NotImplemented) { Py_DECREF(r); return dflt; }
    Py_ssize_t n = PyLong_AsSsize_t(r);
    Py_DECREF(r);
    if (n < 0 && !PyErr_Occurred()) { PyErr_SetString(PyExc_ValueError, "__length_hint__() should return >= 0"); return -1; }
    return n;
}

PyObject *PyObject_GetIter(PyObject *o) {
    PyTypeObject *tp = Py_TYPE(o);
    getiterfunc f = tp->tp_iter;
    if (f) {
        PyObject *it = f(o);
        if (it && !PyIter_Check(it)) {
            PyErr_Format(PyExc_TypeError, "iter() returned non-iterator of type '%s'", Py_TYPE(it)->tp_name);
            Py_DECREF(it);
            return NULL;
        }
        return it;
    }
    if (tp->tp_as_sequence && tp->tp_as_sequence->sq_item) return PySeqIter_New(o);
    PyErr_Format(PyExc_TypeError, "'%s' object is not iterable", tp->tp_name);
    return NULL;
}

PyObject *PyObject_SelfIter(PyObject *o) { return Py_NewRef(o); }

PyObject *PyIter_Next(PyObject *it) {
    iternextfunc f = Py_TYPE(it)->tp_iternext;
    if (!f) { PyErr_Format(PyExc_TypeError, "'%s' object is not an iterator", Py_TYPE(it)->tp_name); return NULL; }
    PyObject *r = f(it);
    if (!r && PyErr_Occurred() && PyErr_ExceptionMatches(PyExc_StopIteration)) PyErr_Clear();
    return r;
}

int PyIter_NextItem(PyObject *it, PyObject **item) {
    *item = PyIter_Next(it);
    if (*item) return 1;
    return PyErr_Occurred() ? -1 : 0;
}

PyObject *PyObject_Type(PyObject *o) { return Py_NewRef((PyObject *)Py_TYPE(o)); }

/* ---- isinstance / issubclass ------------------------------------------- */

static int abstract_issubclass(PyObject *derived, PyObject *cls) {
    if (derived == cls) return 1;
    PyObject *bases;
    if (PyObject_GetOptionalAttrString(derived, "__bases__", &bases) < 0) return -1;
    if (!bases || !PyTuple_Check(bases)) { Py_XDECREF(bases); return 0; }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        int r = abstract_issubclass(PyTuple_GET_ITEM(bases, i), cls);
        if (r) { Py_DECREF(bases); return r; }
    }
    Py_DECREF(bases);
    return 0;
}

int PyObject_IsSubclass(PyObject *a, PyObject *b) {
    if (PyType_CheckExact(b)) {
        if (a == b) return 1;
        if (PyType_Check(a)) return PyType_IsSubtype((PyTypeObject *)a, (PyTypeObject *)b);
    }
    if (PyTuple_Check(b)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(b); i++) {
            int r = PyObject_IsSubclass(a, PyTuple_GET_ITEM(b, i));
            if (r) return r;
        }
        return 0;
    }
    PyObject *checker;
    if (PyObject_GetOptionalAttrString(b, "__subclasscheck__", &checker) < 0) return -1;
    if (checker) {
        PyObject *r = PyObject_CallOneArg(checker, a);
        Py_DECREF(checker);
        if (!r) return -1;
        int ok = PyObject_IsTrue(r);
        Py_DECREF(r);
        return ok;
    }
    if (!PyType_Check(b)) { PyErr_SetString(PyExc_TypeError, "issubclass() arg 2 must be a class, a tuple of classes, or a union"); return -1; }
    if (!PyType_Check(a)) { PyErr_SetString(PyExc_TypeError, "issubclass() arg 1 must be a class"); return -1; }
    return abstract_issubclass(a, b);
}

int PyObject_IsInstance(PyObject *o, PyObject *cls) {
    if ((PyObject *)Py_TYPE(o) == cls) return 1;
    if (PyType_CheckExact(cls)) return PyObject_TypeCheck(o, (PyTypeObject *)cls);
    if (PyTuple_Check(cls)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(cls); i++) {
            int r = PyObject_IsInstance(o, PyTuple_GET_ITEM(cls, i));
            if (r) return r;
        }
        return 0;
    }
    PyObject *checker;
    if (PyObject_GetOptionalAttrString(cls, "__instancecheck__", &checker) < 0) return -1;
    if (checker) {
        PyObject *r = PyObject_CallOneArg(checker, o);
        Py_DECREF(checker);
        if (!r) return -1;
        int ok = PyObject_IsTrue(r);
        Py_DECREF(r);
        return ok;
    }
    if (PyType_Check(cls)) {
        if (PyObject_TypeCheck(o, (PyTypeObject *)cls)) return 1;
        PyObject *c;
        if (PyObject_GetOptionalAttrString(o, "__class__", &c) < 0) return -1;
        int r = 0;
        if (c && c != (PyObject *)Py_TYPE(o) && PyType_Check(c)) r = PyType_IsSubtype((PyTypeObject *)c, (PyTypeObject *)cls);
        Py_XDECREF(c);
        return r;
    }
    PyErr_SetString(PyExc_TypeError, "isinstance() arg 2 must be a type, a tuple of types, or a union");
    return -1;
}

int PyCallable_Check(PyObject *o) { return o && Py_TYPE(o)->tp_call != NULL; }
int PyIter_Check(PyObject *o) { return o && Py_TYPE(o)->tp_iternext != NULL; }

/* ---- calls ------------------------------------------------------------- */

vectorcallfunc PyVectorcall_Function(PyObject *f) {
    PyTypeObject *tp = Py_TYPE(f);
    if (!(tp->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL) || tp->tp_vectorcall_offset <= 0) return NULL;
    return *(vectorcallfunc *)((char *)f + tp->tp_vectorcall_offset);
}

PyObject *_PyObject_MakeTpCall(PyObject *f, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    ternaryfunc call = Py_TYPE(f)->tp_call;
    if (!call) { PyErr_Format(PyExc_TypeError, "'%s' object is not callable", Py_TYPE(f)->tp_name); return NULL; }
    PyObject *argstuple = PyTuple_FromArray(args, nargs);
    if (!argstuple) return NULL;
    PyObject *kwdict = NULL;
    if (kwnames && PyTuple_GET_SIZE(kwnames)) {
        kwdict = PyDict_New();
        if (!kwdict) { Py_DECREF(argstuple); return NULL; }
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
            if (PyDict_SetItem(kwdict, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]) < 0) { Py_DECREF(argstuple); Py_DECREF(kwdict); return NULL; }
        }
    }
    if (Py_EnterRecursiveCall(" while calling a Python object")) { Py_DECREF(argstuple); Py_XDECREF(kwdict); return NULL; }
    PyObject *r = call(f, argstuple, kwdict);
    Py_LeaveRecursiveCall();
    Py_DECREF(argstuple);
    Py_XDECREF(kwdict);
    if (!r && !PyErr_Occurred()) PyErr_SetString(PyExc_SystemError, "NULL result without error in PyObject_Call");
    return r;
}

PyObject *PyObject_Vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    vectorcallfunc vc = PyVectorcall_Function(f);
    if (vc) {
        if (Py_EnterRecursiveCall(" while calling a Python object")) return NULL;
        PyObject *r = vc(f, args, nargsf, kwnames);
        Py_LeaveRecursiveCall();
        if (!r && !PyErr_Occurred()) PyErr_SetString(PyExc_SystemError, "NULL result without error in vectorcall");
        return r;
    }
    return _PyObject_MakeTpCall(f, args, PyVectorcall_NARGS(nargsf), kwnames);
}

PyObject *piper_call(PyObject *callable, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    return PyObject_Vectorcall(callable, args, (size_t)nargs, kwnames);
}

PyObject *PyObject_VectorcallDict(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwargs) {
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (!kwargs || PyDict_Size(kwargs) == 0) return PyObject_Vectorcall(f, args, (size_t)nargs, NULL);
    Py_ssize_t nkw = PyDict_Size(kwargs);
    PyObject **buf = PyMem_Malloc(sizeof(PyObject *) * (size_t)(nargs + nkw));
    if (!buf) return PyErr_NoMemory();
    memcpy(buf, args, sizeof(PyObject *) * (size_t)nargs);
    PyObject *kwnames = PyTuple_New(nkw);
    Py_ssize_t pos = 0, i = 0;
    PyObject *k, *v;
    while (PyDict_Next(kwargs, &pos, &k, &v)) {
        PyTuple_SET_ITEM(kwnames, i, Py_NewRef(k));
        buf[nargs + i] = v;
        i++;
    }
    PyObject *r = PyObject_Vectorcall(f, buf, (size_t)nargs, kwnames);
    Py_DECREF(kwnames);
    PyMem_Free(buf);
    return r;
}

PyObject *PyVectorcall_Call(PyObject *f, PyObject *args, PyObject *kwargs) {
    return PyObject_VectorcallDict(f, PyTuple_ITEMS(args), (size_t)PyTuple_GET_SIZE(args), kwargs);
}

PyObject *PyObject_Call(PyObject *f, PyObject *args, PyObject *kwargs) {
    if (!PyTuple_Check(args)) { PyErr_SetString(PyExc_TypeError, "argument list must be a tuple"); return NULL; }
    if (kwargs && !PyDict_Check(kwargs)) { PyErr_SetString(PyExc_TypeError, "keyword list must be a dictionary"); return NULL; }
    vectorcallfunc vc = PyVectorcall_Function(f);
    if (vc) return PyVectorcall_Call(f, args, kwargs);
    ternaryfunc call = Py_TYPE(f)->tp_call;
    if (!call) { PyErr_Format(PyExc_TypeError, "'%s' object is not callable", Py_TYPE(f)->tp_name); return NULL; }
    if (Py_EnterRecursiveCall(" while calling a Python object")) return NULL;
    PyObject *r = call(f, args, kwargs);
    Py_LeaveRecursiveCall();
    if (!r && !PyErr_Occurred()) PyErr_SetString(PyExc_SystemError, "NULL result without error in PyObject_Call");
    return r;
}

PyObject *piper_call_ex(PyObject *callable, PyObject *args, PyObject *kwargs) {
    PyObject *t = args;
    if (!PyTuple_Check(args)) { t = PySequence_Tuple(args); if (!t) return NULL; } else Py_INCREF(t);
    PyObject *r = PyObject_Call(callable, t, kwargs);
    Py_DECREF(t);
    return r;
}

PyObject *PyObject_CallObject(PyObject *f, PyObject *args) {
    if (!args) return PyObject_CallNoArgs(f);
    return PyObject_Call(f, args, NULL);
}
PyObject *PyObject_CallNoArgs(PyObject *f) { return PyObject_Vectorcall(f, NULL, 0, NULL); }
PyObject *PyObject_CallOneArg(PyObject *f, PyObject *arg) { PyObject *a[1] = { arg }; return PyObject_Vectorcall(f, a, 1, NULL); }

PyObject *PyObject_CallFunctionObjArgs(PyObject *f, ...) {
    PyObject *args[32];
    Py_ssize_t n = 0;
    va_list va;
    va_start(va, f);
    PyObject *a;
    while ((a = va_arg(va, PyObject *)) && n < 32) args[n++] = a;
    va_end(va);
    return PyObject_Vectorcall(f, args, (size_t)n, NULL);
}

PyObject *PyObject_CallMethodObjArgs(PyObject *o, PyObject *name, ...) {
    PyObject *m = PyObject_GetAttr(o, name);
    if (!m) return NULL;
    PyObject *args[32];
    Py_ssize_t n = 0;
    va_list va;
    va_start(va, name);
    PyObject *a;
    while ((a = va_arg(va, PyObject *)) && n < 32) args[n++] = a;
    va_end(va);
    PyObject *r = PyObject_Vectorcall(m, args, (size_t)n, NULL);
    Py_DECREF(m);
    return r;
}

PyObject *PyObject_CallMethodNoArgs(PyObject *o, PyObject *name) {
    PyObject *m = PyObject_GetAttr(o, name);
    if (!m) return NULL;
    PyObject *r = PyObject_CallNoArgs(m);
    Py_DECREF(m);
    return r;
}

PyObject *PyObject_CallMethodOneArg(PyObject *o, PyObject *name, PyObject *arg) {
    PyObject *m = PyObject_GetAttr(o, name);
    if (!m) return NULL;
    PyObject *r = PyObject_CallOneArg(m, arg);
    Py_DECREF(m);
    return r;
}

PyObject *PyObject_VectorcallMethod(PyObject *name, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyObject *self = args[0];
    PyObject *m = PyObject_GetAttr(self, name);
    if (!m) return NULL;
    PyObject *r = PyObject_Vectorcall(m, args + 1, (size_t)(PyVectorcall_NARGS(nargsf) - 1) | (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET), kwnames);
    Py_DECREF(m);
    return r;
}

PyObject *PyObject_CallMethod(PyObject *o, const char *name, const char *fmt, ...) {
    PyObject *m = PyObject_GetAttrString(o, name);
    if (!m) return NULL;
    PyObject *args;
    if (fmt && *fmt) {
        va_list va;
        va_start(va, fmt);
        args = Py_VaBuildValue(fmt, va);
        va_end(va);
        if (!args) { Py_DECREF(m); return NULL; }
        if (!PyTuple_Check(args)) { PyObject *t = PyTuple_Pack(1, args); Py_DECREF(args); args = t; }
    } else args = PyTuple_New(0);
    PyObject *r = PyObject_Call(m, args, NULL);
    Py_DECREF(args);
    Py_DECREF(m);
    return r;
}

PyObject *PyObject_CallFunction(PyObject *f, const char *fmt, ...) {
    PyObject *args;
    if (fmt && *fmt) {
        va_list va;
        va_start(va, fmt);
        args = Py_VaBuildValue(fmt, va);
        va_end(va);
        if (!args) return NULL;
        if (!PyTuple_Check(args)) { PyObject *t = PyTuple_Pack(1, args); Py_DECREF(args); args = t; }
    } else args = PyTuple_New(0);
    PyObject *r = PyObject_Call(f, args, NULL);
    Py_DECREF(args);
    return r;
}

PyObject *piper_load_method(PyObject *obj, PyObject *name, PyObject **self_out) {
    /* Avoid creating a bound method when the attribute is a plain method descriptor. */
    PyTypeObject *tp = Py_TYPE(obj);
    if (tp->tp_getattro == PyObject_GenericGetAttr) {
        if (!(tp->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(tp) < 0) return NULL;
        PyObject *descr = piper_type_lookup(tp, name);
        if (descr && (Py_TYPE(descr)->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR)) {
            PyObject **dp = _PyObject_GetDictPtr(obj);
            int shadowed = 0;
            if (dp && *dp) {
                PyObject *r = PyDict_GetItemWithError(*dp, name);
                if (r) shadowed = 1; else if (PyErr_Occurred()) return NULL;
            }
            if (!shadowed) { *self_out = obj; return Py_NewRef(descr); }
        }
    }
    *self_out = NULL;
    return PyObject_GetAttr(obj, name);
}

/* ---- number protocol --------------------------------------------------- */

#define NB_SLOT(tp, off) (*(binaryfunc *)((char *)(tp)->tp_as_number + (off)))
#define NB_BINOP(off, name) \
    PyObject *name(PyObject *v, PyObject *w) { return binary_op(v, w, off, #name); }

static PyObject *binary_op1(PyObject *v, PyObject *w, size_t off) {
    binaryfunc slotv = NULL, slotw = NULL;
    if (Py_TYPE(v)->tp_as_number) slotv = NB_SLOT(Py_TYPE(v), off);
    if (!Py_IS_TYPE(w, Py_TYPE(v)) && Py_TYPE(w)->tp_as_number) {
        slotw = NB_SLOT(Py_TYPE(w), off);
        if (slotw == slotv) slotw = NULL;
    }
    if (slotv) {
        PyObject *x;
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w);
            if (x != Py_NotImplemented) return x;
            Py_DECREF(x);
            slotw = NULL;
        }
        x = slotv(v, w);
        if (x != Py_NotImplemented) return x;
        Py_DECREF(x);
    }
    if (slotw) {
        PyObject *x = slotw(v, w);
        if (x != Py_NotImplemented) return x;
        Py_DECREF(x);
    }
    Py_RETURN_NOTIMPLEMENTED;
}

static const char *op_symbol(const char *fname) {
    if (!strcmp(fname, "PyNumber_Add")) return "+";
    if (!strcmp(fname, "PyNumber_Subtract")) return "-";
    if (!strcmp(fname, "PyNumber_Multiply")) return "*";
    if (!strcmp(fname, "PyNumber_MatrixMultiply")) return "@";
    if (!strcmp(fname, "PyNumber_TrueDivide")) return "/";
    if (!strcmp(fname, "PyNumber_FloorDivide")) return "//";
    if (!strcmp(fname, "PyNumber_Remainder")) return "%";
    if (!strcmp(fname, "PyNumber_Divmod")) return "divmod()";
    if (!strcmp(fname, "PyNumber_Lshift")) return "<<";
    if (!strcmp(fname, "PyNumber_Rshift")) return ">>";
    if (!strcmp(fname, "PyNumber_And")) return "&";
    if (!strcmp(fname, "PyNumber_Xor")) return "^";
    if (!strcmp(fname, "PyNumber_Or")) return "|";
    return "?";
}

static PyObject *binop_type_error(PyObject *v, PyObject *w, const char *sym) {
    PyErr_Format(PyExc_TypeError, "unsupported operand type(s) for %s: '%s' and '%s'", sym, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    return NULL;
}

static PyObject *binary_op(PyObject *v, PyObject *w, size_t off, const char *fname) {
    PyObject *r = binary_op1(v, w, off);
    if (r == Py_NotImplemented) {
        Py_DECREF(r);
        if (off == offsetof(PyNumberMethods, nb_add) || off == offsetof(PyNumberMethods, nb_multiply)) {
            /* sequence concat / repeat fallbacks */
            PySequenceMethods *m = Py_TYPE(v)->tp_as_sequence;
            if (off == offsetof(PyNumberMethods, nb_add)) {
                if (m && m->sq_concat) return m->sq_concat(v, w);
            } else {
                if (m && m->sq_repeat && PyIndex_Check(w)) {
                    Py_ssize_t n = PyNumber_AsSsize_t(w, PyExc_OverflowError);
                    if (n == -1 && PyErr_Occurred()) return NULL;
                    return m->sq_repeat(v, n);
                }
                PySequenceMethods *mw = Py_TYPE(w)->tp_as_sequence;
                if (mw && mw->sq_repeat && PyIndex_Check(v)) {
                    Py_ssize_t n = PyNumber_AsSsize_t(v, PyExc_OverflowError);
                    if (n == -1 && PyErr_Occurred()) return NULL;
                    return mw->sq_repeat(w, n);
                }
            }
        }
        return binop_type_error(v, w, op_symbol(fname));
    }
    return r;
}

NB_BINOP(offsetof(PyNumberMethods, nb_add), PyNumber_Add)
NB_BINOP(offsetof(PyNumberMethods, nb_subtract), PyNumber_Subtract)
NB_BINOP(offsetof(PyNumberMethods, nb_multiply), PyNumber_Multiply)
NB_BINOP(offsetof(PyNumberMethods, nb_matrix_multiply), PyNumber_MatrixMultiply)
NB_BINOP(offsetof(PyNumberMethods, nb_true_divide), PyNumber_TrueDivide)
NB_BINOP(offsetof(PyNumberMethods, nb_floor_divide), PyNumber_FloorDivide)
NB_BINOP(offsetof(PyNumberMethods, nb_remainder), PyNumber_Remainder)
NB_BINOP(offsetof(PyNumberMethods, nb_divmod), PyNumber_Divmod)
NB_BINOP(offsetof(PyNumberMethods, nb_lshift), PyNumber_Lshift)
NB_BINOP(offsetof(PyNumberMethods, nb_rshift), PyNumber_Rshift)
NB_BINOP(offsetof(PyNumberMethods, nb_and), PyNumber_And)
NB_BINOP(offsetof(PyNumberMethods, nb_xor), PyNumber_Xor)
NB_BINOP(offsetof(PyNumberMethods, nb_or), PyNumber_Or)

PyObject *PyNumber_Power(PyObject *v, PyObject *w, PyObject *z) {
    ternaryfunc slotv = NULL, slotw = NULL;
    if (Py_TYPE(v)->tp_as_number) slotv = Py_TYPE(v)->tp_as_number->nb_power;
    if (!Py_IS_TYPE(w, Py_TYPE(v)) && Py_TYPE(w)->tp_as_number) { slotw = Py_TYPE(w)->tp_as_number->nb_power; if (slotw == slotv) slotw = NULL; }
    PyObject *x;
    if (slotv) {
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w, z);
            if (x != Py_NotImplemented) return x;
            Py_DECREF(x);
            slotw = NULL;
        }
        x = slotv(v, w, z);
        if (x != Py_NotImplemented) return x;
        Py_DECREF(x);
    }
    if (slotw) {
        x = slotw(v, w, z);
        if (x != Py_NotImplemented) return x;
        Py_DECREF(x);
    }
    if (z == Py_None) return binop_type_error(v, w, "** or pow()");
    PyErr_Format(PyExc_TypeError, "unsupported operand type(s) for ** or pow(): '%s', '%s', '%s'", Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name, Py_TYPE(z)->tp_name);
    return NULL;
}

static PyObject *inplace_op(PyObject *v, PyObject *w, size_t ioff, size_t off, const char *fname) {
    if (Py_TYPE(v)->tp_as_number) {
        binaryfunc f = NB_SLOT(Py_TYPE(v), ioff);
        if (f) {
            PyObject *x = f(v, w);
            if (x != Py_NotImplemented) return x;
            Py_DECREF(x);
        }
    }
    PyObject *r = binary_op1(v, w, off);
    if (r == Py_NotImplemented) {
        Py_DECREF(r);
        PySequenceMethods *m = Py_TYPE(v)->tp_as_sequence;
        if (off == offsetof(PyNumberMethods, nb_add) && m) {
            if (m->sq_inplace_concat) return m->sq_inplace_concat(v, w);
            if (m->sq_concat) return m->sq_concat(v, w);
        }
        if (off == offsetof(PyNumberMethods, nb_multiply) && m && (m->sq_inplace_repeat || m->sq_repeat) && PyIndex_Check(w)) {
            Py_ssize_t n = PyNumber_AsSsize_t(w, PyExc_OverflowError);
            if (n == -1 && PyErr_Occurred()) return NULL;
            return (m->sq_inplace_repeat ? m->sq_inplace_repeat : m->sq_repeat)(v, n);
        }
        char sym[16];
        snprintf(sym, sizeof sym, "%s=", op_symbol(fname));
        return binop_type_error(v, w, sym);
    }
    return r;
}

#define NB_IBINOP(ioff, off, name, base) \
    PyObject *name(PyObject *v, PyObject *w) { return inplace_op(v, w, ioff, off, #base); }
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_add), offsetof(PyNumberMethods, nb_add), PyNumber_InPlaceAdd, PyNumber_Add)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_subtract), offsetof(PyNumberMethods, nb_subtract), PyNumber_InPlaceSubtract, PyNumber_Subtract)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_multiply), offsetof(PyNumberMethods, nb_multiply), PyNumber_InPlaceMultiply, PyNumber_Multiply)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_matrix_multiply), offsetof(PyNumberMethods, nb_matrix_multiply), PyNumber_InPlaceMatrixMultiply, PyNumber_MatrixMultiply)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_true_divide), offsetof(PyNumberMethods, nb_true_divide), PyNumber_InPlaceTrueDivide, PyNumber_TrueDivide)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_floor_divide), offsetof(PyNumberMethods, nb_floor_divide), PyNumber_InPlaceFloorDivide, PyNumber_FloorDivide)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_remainder), offsetof(PyNumberMethods, nb_remainder), PyNumber_InPlaceRemainder, PyNumber_Remainder)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_lshift), offsetof(PyNumberMethods, nb_lshift), PyNumber_InPlaceLshift, PyNumber_Lshift)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_rshift), offsetof(PyNumberMethods, nb_rshift), PyNumber_InPlaceRshift, PyNumber_Rshift)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_and), offsetof(PyNumberMethods, nb_and), PyNumber_InPlaceAnd, PyNumber_And)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_xor), offsetof(PyNumberMethods, nb_xor), PyNumber_InPlaceXor, PyNumber_Xor)
NB_IBINOP(offsetof(PyNumberMethods, nb_inplace_or), offsetof(PyNumberMethods, nb_or), PyNumber_InPlaceOr, PyNumber_Or)

PyObject *PyNumber_InPlacePower(PyObject *v, PyObject *w, PyObject *z) {
    if (Py_TYPE(v)->tp_as_number && Py_TYPE(v)->tp_as_number->nb_inplace_power) {
        PyObject *x = Py_TYPE(v)->tp_as_number->nb_inplace_power(v, w, z);
        if (x != Py_NotImplemented) return x;
        Py_DECREF(x);
    }
    return PyNumber_Power(v, w, z);
}

PyObject *piper_binop(int op, PyObject *a, PyObject *b) {
    switch (op) {
    case PIPER_OP_ADD: return PyNumber_Add(a, b);
    case PIPER_OP_SUB: return PyNumber_Subtract(a, b);
    case PIPER_OP_MUL: return PyNumber_Multiply(a, b);
    case PIPER_OP_MATMUL: return PyNumber_MatrixMultiply(a, b);
    case PIPER_OP_DIV: return PyNumber_TrueDivide(a, b);
    case PIPER_OP_MOD: return PyNumber_Remainder(a, b);
    case PIPER_OP_POW: return PyNumber_Power(a, b, Py_None);
    case PIPER_OP_LSHIFT: return PyNumber_Lshift(a, b);
    case PIPER_OP_RSHIFT: return PyNumber_Rshift(a, b);
    case PIPER_OP_OR: return PyNumber_Or(a, b);
    case PIPER_OP_XOR: return PyNumber_Xor(a, b);
    case PIPER_OP_AND: return PyNumber_And(a, b);
    case PIPER_OP_FLOORDIV: return PyNumber_FloorDivide(a, b);
    }
    PyErr_SetString(PyExc_SystemError, "bad binary op");
    return NULL;
}

PyObject *piper_inplace_binop(int op, PyObject *a, PyObject *b) {
    switch (op) {
    case PIPER_OP_ADD: return PyNumber_InPlaceAdd(a, b);
    case PIPER_OP_SUB: return PyNumber_InPlaceSubtract(a, b);
    case PIPER_OP_MUL: return PyNumber_InPlaceMultiply(a, b);
    case PIPER_OP_MATMUL: return PyNumber_InPlaceMatrixMultiply(a, b);
    case PIPER_OP_DIV: return PyNumber_InPlaceTrueDivide(a, b);
    case PIPER_OP_MOD: return PyNumber_InPlaceRemainder(a, b);
    case PIPER_OP_POW: return PyNumber_InPlacePower(a, b, Py_None);
    case PIPER_OP_LSHIFT: return PyNumber_InPlaceLshift(a, b);
    case PIPER_OP_RSHIFT: return PyNumber_InPlaceRshift(a, b);
    case PIPER_OP_OR: return PyNumber_InPlaceOr(a, b);
    case PIPER_OP_XOR: return PyNumber_InPlaceXor(a, b);
    case PIPER_OP_AND: return PyNumber_InPlaceAnd(a, b);
    case PIPER_OP_FLOORDIV: return PyNumber_InPlaceFloorDivide(a, b);
    }
    PyErr_SetString(PyExc_SystemError, "bad inplace op");
    return NULL;
}

#define UNARY(name, slot, sym) \
    PyObject *name(PyObject *o) { \
        PyNumberMethods *m = Py_TYPE(o)->tp_as_number; \
        if (m && m->slot) return m->slot(o); \
        PyErr_Format(PyExc_TypeError, "bad operand type for %s: '%s'", sym, Py_TYPE(o)->tp_name); \
        return NULL; \
    }
UNARY(PyNumber_Negative, nb_negative, "unary -")
UNARY(PyNumber_Positive, nb_positive, "unary +")
UNARY(PyNumber_Absolute, nb_absolute, "abs()")
UNARY(PyNumber_Invert, nb_invert, "unary ~")

int PyNumber_Check(PyObject *o) {
    PyNumberMethods *m = Py_TYPE(o)->tp_as_number;
    return m && (m->nb_index || m->nb_int || m->nb_float || PyComplex_Type.tp_name && PyObject_TypeCheck(o, &PyComplex_Type));
}

int PyIndex_Check(PyObject *o) { return o && Py_TYPE(o)->tp_as_number && Py_TYPE(o)->tp_as_number->nb_index; }

PyObject *PyNumber_Index(PyObject *o) {
    if (PyLong_Check(o)) return Py_NewRef(o);
    if (!PyIndex_Check(o)) { PyErr_Format(PyExc_TypeError, "'%s' object cannot be interpreted as an integer", Py_TYPE(o)->tp_name); return NULL; }
    PyObject *r = Py_TYPE(o)->tp_as_number->nb_index(o);
    if (r && !PyLong_Check(r)) { PyErr_Format(PyExc_TypeError, "__index__ returned non-int (type %s)", Py_TYPE(r)->tp_name); Py_DECREF(r); return NULL; }
    return r;
}

Py_ssize_t PyNumber_AsSsize_t(PyObject *o, PyObject *exc) {
    PyObject *v = PyNumber_Index(o);
    if (!v) return -1;
    int overflow;
    long long r = PyLong_AsLongLongAndOverflow(v, &overflow);
    Py_DECREF(v);
    if (!overflow) return (Py_ssize_t)r;
    if (!exc) return overflow < 0 ? PY_SSIZE_T_MIN : PY_SSIZE_T_MAX;
    PyErr_Format(exc, "cannot fit '%s' into an index-sized integer", Py_TYPE(o)->tp_name);
    return -1;
}

int piper_index_ssize(PyObject *o, Py_ssize_t *out, PyObject *exc_type) {
    Py_ssize_t v = PyNumber_AsSsize_t(o, exc_type);
    if (v == -1 && PyErr_Occurred()) return -1;
    *out = v;
    return 0;
}

PyObject *PyNumber_Long(PyObject *o) {
    if (PyLong_CheckExact(o)) return Py_NewRef(o);
    PyNumberMethods *m = Py_TYPE(o)->tp_as_number;
    if (m && m->nb_int) {
        PyObject *r = m->nb_int(o);
        if (r && !PyLong_Check(r)) { PyErr_Format(PyExc_TypeError, "__int__ returned non-int (type %s)", Py_TYPE(r)->tp_name); Py_DECREF(r); return NULL; }
        return r;
    }
    if (m && m->nb_index) return PyNumber_Index(o);
    PyObject *trunc;
    if (PyObject_GetOptionalAttrString(o, "__trunc__", &trunc) < 0) return NULL;
    if (trunc) { PyObject *r = PyObject_CallNoArgs(trunc); Py_DECREF(trunc); if (!r) return NULL; PyObject *i = PyNumber_Index(r); Py_DECREF(r); return i; }
    if (PyUnicode_Check(o)) return PyLong_FromUnicodeObject(o, 10);
    if (PyBytes_Check(o)) return PyLong_FromString(PyBytes_AS_STRING(o), NULL, 10);
    PyErr_Format(PyExc_TypeError, "int() argument must be a string, a bytes-like object or a real number, not '%s'", Py_TYPE(o)->tp_name);
    return NULL;
}

PyObject *PyNumber_Float(PyObject *o) {
    if (PyFloat_CheckExact(o)) return Py_NewRef(o);
    PyNumberMethods *m = Py_TYPE(o)->tp_as_number;
    if (m && m->nb_float) {
        PyObject *r = m->nb_float(o);
        if (r && !PyFloat_Check(r)) { PyErr_Format(PyExc_TypeError, "%s.__float__ returned non-float (type %s)", Py_TYPE(o)->tp_name, Py_TYPE(r)->tp_name); Py_DECREF(r); return NULL; }
        return r;
    }
    if (m && m->nb_index) {
        PyObject *i = PyNumber_Index(o);
        if (!i) return NULL;
        double d = PyLong_AsDouble(i);
        Py_DECREF(i);
        if (d == -1.0 && PyErr_Occurred()) return NULL;
        return PyFloat_FromDouble(d);
    }
    if (PyUnicode_Check(o)) return PyFloat_FromString(o);
    if (PyBytes_Check(o)) { PyObject *s = PyUnicode_FromStringAndSize(PyBytes_AS_STRING(o), PyBytes_GET_SIZE(o)); if (!s) return NULL; PyObject *r = PyFloat_FromString(s); Py_DECREF(s); return r; }
    PyErr_Format(PyExc_TypeError, "float() argument must be a string or a real number, not '%s'", Py_TYPE(o)->tp_name);
    return NULL;
}

PyObject *PyNumber_ToBase(PyObject *n, int base) {
    PyObject *i = PyNumber_Index(n);
    if (!i) return NULL;
    PyObject *r = _PyLong_Format(i, base);
    Py_DECREF(i);
    return r;
}

/* ---- sequence / mapping protocol --------------------------------------- */

int PySequence_Check(PyObject *o) {
    if (PyDict_Check(o)) return 0;
    return Py_TYPE(o)->tp_as_sequence && Py_TYPE(o)->tp_as_sequence->sq_item != NULL;
}

Py_ssize_t PySequence_Size(PyObject *o) {
    PySequenceMethods *m = Py_TYPE(o)->tp_as_sequence;
    if (m && m->sq_length) return m->sq_length(o);
    if (Py_TYPE(o)->tp_as_mapping && Py_TYPE(o)->tp_as_mapping->mp_length) { PyErr_Format(PyExc_TypeError, "%s is not a sequence", Py_TYPE(o)->tp_name); return -1; }
    PyErr_Format(PyExc_TypeError, "object of type '%s' has no len()", Py_TYPE(o)->tp_name);
    return -1;
}
Py_ssize_t PySequence_Length(PyObject *o) { return PySequence_Size(o); }

PyObject *PySequence_GetItem(PyObject *o, Py_ssize_t i) {
    PySequenceMethods *m = Py_TYPE(o)->tp_as_sequence;
    if (m && m->sq_item) {
        if (i < 0 && m->sq_length) {
            Py_ssize_t n = m->sq_length(o);
            if (n < 0) return NULL;
            i += n;
        }
        return m->sq_item(o, i);
    }
    if (Py_TYPE(o)->tp_as_mapping && Py_TYPE(o)->tp_as_mapping->mp_subscript) { PyErr_Format(PyExc_TypeError, "%s is not a sequence", Py_TYPE(o)->tp_name); return NULL; }
    PyErr_Format(PyExc_TypeError, "'%s' object does not support indexing", Py_TYPE(o)->tp_name);
    return NULL;
}

int PySequence_SetItem(PyObject *o, Py_ssize_t i, PyObject *v) {
    PySequenceMethods *m = Py_TYPE(o)->tp_as_sequence;
    if (m && m->sq_ass_item) {
        if (i < 0 && m->sq_length) { Py_ssize_t n = m->sq_length(o); if (n < 0) return -1; i += n; }
        return m->sq_ass_item(o, i, v);
    }
    PyErr_Format(PyExc_TypeError, "'%s' object does not support item assignment", Py_TYPE(o)->tp_name);
    return -1;
}
int PySequence_DelItem(PyObject *o, Py_ssize_t i) { return PySequence_SetItem(o, i, NULL); }

PyObject *PySequence_GetSlice(PyObject *o, Py_ssize_t a, Py_ssize_t b) {
    PyMappingMethods *m = Py_TYPE(o)->tp_as_mapping;
    if (m && m->mp_subscript) {
        PyObject *sa = PyLong_FromSsize_t(a), *sb = PyLong_FromSsize_t(b);
        PyObject *sl = PySlice_New(sa, sb, NULL);
        Py_DECREF(sa); Py_DECREF(sb);
        if (!sl) return NULL;
        PyObject *r = m->mp_subscript(o, sl);
        Py_DECREF(sl);
        return r;
    }
    PyErr_Format(PyExc_TypeError, "'%s' object is unsliceable", Py_TYPE(o)->tp_name);
    return NULL;
}

int PySequence_Contains(PyObject *o, PyObject *v) {
    PySequenceMethods *m = Py_TYPE(o)->tp_as_sequence;
    if (m && m->sq_contains) return m->sq_contains(o, v);
    PyObject *it = PyObject_GetIter(o);
    if (!it) {
        if (PyErr_ExceptionMatches(PyExc_TypeError)) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "argument of type '%s' is not a container or iterable", Py_TYPE(o)->tp_name); }
        return -1;
    }
    for (;;) {
        PyObject *x = PyIter_Next(it);
        if (!x) { Py_DECREF(it); return PyErr_Occurred() ? -1 : 0; }
        int r = PyObject_RichCompareBool(x, v, Py_EQ);
        Py_DECREF(x);
        if (r) { Py_DECREF(it); return r; }
    }
}

PyObject *PySequence_Concat(PyObject *a, PyObject *b) {
    PySequenceMethods *m = Py_TYPE(a)->tp_as_sequence;
    if (m && m->sq_concat) return m->sq_concat(a, b);
    return PyNumber_Add(a, b);
}
PyObject *PySequence_Repeat(PyObject *a, Py_ssize_t n) {
    PySequenceMethods *m = Py_TYPE(a)->tp_as_sequence;
    if (m && m->sq_repeat) return m->sq_repeat(a, n);
    PyObject *k = PyLong_FromSsize_t(n);
    PyObject *r = PyNumber_Multiply(a, k);
    Py_DECREF(k);
    return r;
}

PyObject *PySequence_Tuple(PyObject *o) {
    if (PyTuple_CheckExact(o)) return Py_NewRef(o);
    if (PyList_CheckExact(o)) return PyList_AsTuple(o);
    PyObject *it = PyObject_GetIter(o);
    if (!it) return NULL;
    PyObject *r = piper_tuple_from_iterable(it);
    Py_DECREF(it);
    return r;
}

PyObject *PySequence_List(PyObject *o) {
    PyObject *l = PyList_New(0);
    if (!l) return NULL;
    if (PyList_Extend(l, o) < 0) { Py_DECREF(l); return NULL; }
    return l;
}

PyObject *PySequence_Fast(PyObject *o, const char *msg) {
    if (PyList_CheckExact(o) || PyTuple_CheckExact(o)) return Py_NewRef(o);
    PyObject *it = PyObject_GetIter(o);
    if (!it) { if (PyErr_ExceptionMatches(PyExc_TypeError)) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, msg); } return NULL; }
    PyObject *l = piper_list_from_iterable(it);
    Py_DECREF(it);
    return l;
}

Py_ssize_t PySequence_Index(PyObject *o, PyObject *v) {
    PyObject *it = PyObject_GetIter(o);
    if (!it) return -1;
    Py_ssize_t i = 0;
    for (;; i++) {
        PyObject *x = PyIter_Next(it);
        if (!x) { Py_DECREF(it); if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "sequence.index(x): x not in sequence"); return -1; }
        int r = PyObject_RichCompareBool(x, v, Py_EQ);
        Py_DECREF(x);
        if (r < 0) { Py_DECREF(it); return -1; }
        if (r) { Py_DECREF(it); return i; }
    }
}

Py_ssize_t PySequence_Count(PyObject *o, PyObject *v) {
    PyObject *it = PyObject_GetIter(o);
    if (!it) return -1;
    Py_ssize_t n = 0;
    for (;;) {
        PyObject *x = PyIter_Next(it);
        if (!x) { Py_DECREF(it); return PyErr_Occurred() ? -1 : n; }
        int r = PyObject_RichCompareBool(x, v, Py_EQ);
        Py_DECREF(x);
        if (r < 0) { Py_DECREF(it); return -1; }
        n += r;
    }
}

int PyMapping_Check(PyObject *o) { return Py_TYPE(o)->tp_as_mapping && Py_TYPE(o)->tp_as_mapping->mp_subscript; }
Py_ssize_t PyMapping_Size(PyObject *o) {
    PyMappingMethods *m = Py_TYPE(o)->tp_as_mapping;
    if (m && m->mp_length) return m->mp_length(o);
    PyErr_Format(PyExc_TypeError, "object of type '%s' has no len()", Py_TYPE(o)->tp_name);
    return -1;
}
static PyObject *mapping_method(PyObject *o, const char *name) {
    PyObject *r = PyObject_CallMethod(o, name, NULL);
    if (!r) return NULL;
    if (PyList_CheckExact(r)) return r;
    PyObject *l = PySequence_List(r);
    Py_DECREF(r);
    return l;
}
PyObject *PyMapping_Keys(PyObject *o) { return PyDict_CheckExact(o) ? PyDict_Keys(o) : mapping_method(o, "keys"); }
PyObject *PyMapping_Values(PyObject *o) { return PyDict_CheckExact(o) ? PyDict_Values(o) : mapping_method(o, "values"); }
PyObject *PyMapping_Items(PyObject *o) { return PyDict_CheckExact(o) ? PyDict_Items(o) : mapping_method(o, "items"); }
PyObject *PyMapping_GetItemString(PyObject *o, const char *k) { PyObject *ks = PyUnicode_FromString(k); if (!ks) return NULL; PyObject *r = PyObject_GetItem(o, ks); Py_DECREF(ks); return r; }
int PyMapping_SetItemString(PyObject *o, const char *k, PyObject *v) { PyObject *ks = PyUnicode_FromString(k); if (!ks) return -1; int r = PyObject_SetItem(o, ks, v); Py_DECREF(ks); return r; }
int PyMapping_GetOptionalItem(PyObject *o, PyObject *k, PyObject **result) {
    if (PyDict_CheckExact(o)) return PyDict_GetItemRef(o, k, result);
    *result = PyObject_GetItem(o, k);
    if (*result) return 1;
    if (PyErr_ExceptionMatches(PyExc_KeyError)) { PyErr_Clear(); return 0; }
    return -1;
}
int PyMapping_HasKey(PyObject *o, PyObject *k) { PyObject *r; int rc = PyMapping_GetOptionalItem(o, k, &r); Py_XDECREF(r); if (rc < 0) { PyErr_Clear(); return 0; } return rc; }
int PyMapping_HasKeyString(PyObject *o, const char *k) { PyObject *r = PyMapping_GetItemString(o, k); if (!r) { PyErr_Clear(); return 0; } Py_DECREF(r); return 1; }

/* ---- buffers ----------------------------------------------------------- */

int PyObject_CheckBuffer(PyObject *o) { PyBufferProcs *b = Py_TYPE(o)->tp_as_buffer; return b && b->bf_getbuffer; }
int PyObject_GetBuffer(PyObject *o, Py_buffer *view, int flags) {
    PyBufferProcs *b = Py_TYPE(o)->tp_as_buffer;
    if (!b || !b->bf_getbuffer) { PyErr_Format(PyExc_TypeError, "a bytes-like object is required, not '%s'", Py_TYPE(o)->tp_name); return -1; }
    return b->bf_getbuffer(o, view, flags);
}
void PyBuffer_Release(Py_buffer *view) {
    PyObject *o = view->obj;
    if (!o) return;
    PyBufferProcs *b = Py_TYPE(o)->tp_as_buffer;
    if (b && b->bf_releasebuffer) b->bf_releasebuffer(o, view);
    view->obj = NULL;
    Py_DECREF(o);
}
int PyBuffer_FillInfo(Py_buffer *view, PyObject *o, void *buf, Py_ssize_t len, int readonly, int flags) {
    if (!view) { PyErr_SetString(PyExc_BufferError, "PyBuffer_FillInfo: view==NULL argument is obsolete"); return -1; }
    if ((flags & PyBUF_WRITABLE) && readonly) { PyErr_SetString(PyExc_BufferError, "Object is not writable."); return -1; }
    view->obj = Py_XNewRef(o);
    view->buf = buf;
    view->len = len;
    view->readonly = readonly;
    view->itemsize = 1;
    view->format = (flags & PyBUF_FORMAT) ? "B" : NULL;
    view->ndim = 1;
    view->shape = (flags & PyBUF_ND) ? &view->len : NULL;
    view->strides = ((flags & PyBUF_STRIDES) == PyBUF_STRIDES) ? &view->itemsize : NULL;
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

int PyBuffer_IsContiguous(const Py_buffer *view, char order) {
    if (!view || view->ndim == 0) return 1;
    if (order == 'A') return PyBuffer_IsContiguous(view, 'C') || PyBuffer_IsContiguous(view, 'F');
    if (order != 'C' && order != 'F') return 0;
    if (!view->shape) return 1;
    if (view->suboffsets) {
        for (int i = 0; i < view->ndim; i++) if (view->suboffsets[i] >= 0) return 0;
    }
    if (!view->strides) return order == 'C' || view->ndim <= 1;
    Py_ssize_t expected = view->itemsize;
    if (order == 'C') {
        for (int i = view->ndim - 1; i >= 0; i--) {
            if (view->shape[i] > 1 && view->strides[i] != expected) return 0;
            expected *= view->shape[i];
        }
    } else {
        for (int i = 0; i < view->ndim; i++) {
            if (view->shape[i] > 1 && view->strides[i] != expected) return 0;
            expected *= view->shape[i];
        }
    }
    return 1;
}

void *PyBuffer_GetPointer(const Py_buffer *view, const Py_ssize_t *indices) {
    char *pointer = (char *)view->buf;
    for (int i = 0; i < view->ndim; i++) {
        Py_ssize_t stride = view->strides ? view->strides[i] : view->itemsize;
        pointer += indices[i] * stride;
        if (view->suboffsets && view->suboffsets[i] >= 0) pointer = *(char **)pointer + view->suboffsets[i];
    }
    return pointer;
}

Py_ssize_t PyBuffer_SizeFromFormat(const char *format) {
    if (!format || !*format) return 1;
    while (*format == '@' || *format == '=' || *format == '<' || *format == '>' || *format == '!') format++;
    Py_ssize_t repeat = 0;
    while (*format >= '0' && *format <= '9') repeat = repeat * 10 + (*format++ - '0');
    if (repeat == 0) repeat = 1;
    Py_ssize_t size;
    switch (*format) {
        case 'x': case 'c': case 'b': case 'B': case '?': size = 1; break;
        case 'h': case 'H': size = (Py_ssize_t)sizeof(short); break;
        case 'i': case 'I': size = (Py_ssize_t)sizeof(int); break;
        case 'l': case 'L': size = (Py_ssize_t)sizeof(long); break;
        case 'q': case 'Q': size = (Py_ssize_t)sizeof(long long); break;
        case 'n': case 'N': size = (Py_ssize_t)sizeof(Py_ssize_t); break;
        case 'e': size = 2; break;
        case 'f': size = (Py_ssize_t)sizeof(float); break;
        case 'd': size = (Py_ssize_t)sizeof(double); break;
        case 'P': size = (Py_ssize_t)sizeof(void *); break;
        case 's': case 'p': size = 1; break;
        default: return -1;
    }
    return repeat * size;
}

void PyBuffer_FillContiguousStrides(int ndims, const Py_ssize_t *shape, Py_ssize_t *strides, int itemsize, char order) {
    Py_ssize_t stride = itemsize;
    if (order == 'F') {
        for (int i = 0; i < ndims; i++) { strides[i] = stride; stride *= shape[i]; }
    } else {
        for (int i = ndims - 1; i >= 0; i--) { strides[i] = stride; stride *= shape[i]; }
    }
}

static void buffer_indices(const Py_buffer *view, Py_ssize_t linear, char order, Py_ssize_t *indices) {
    if (order == 'F') {
        for (int i = 0; i < view->ndim; i++) { indices[i] = linear % view->shape[i]; linear /= view->shape[i]; }
    } else {
        for (int i = view->ndim - 1; i >= 0; i--) { indices[i] = linear % view->shape[i]; linear /= view->shape[i]; }
    }
}

static int buffer_copy_contiguous(void *destination, const void *source, const Py_buffer *view, Py_ssize_t len, char order, int into_view) {
    if (!view || len < 0 || len > view->len) { PyErr_SetString(PyExc_ValueError, "buffer length is out of range"); return -1; }
    if (len == 0) return 0;
    if (PyBuffer_IsContiguous(view, order)) {
        if (into_view) memcpy(view->buf, source, (size_t)len); else memcpy(destination, view->buf, (size_t)len);
        return 0;
    }
    if (!view->shape || view->itemsize <= 0 || view->ndim <= 0) { PyErr_SetString(PyExc_BufferError, "buffer has no multidimensional shape"); return -1; }
    Py_ssize_t *indices = PyMem_Calloc((size_t)view->ndim, sizeof(Py_ssize_t));
    if (!indices) return PyErr_NoMemory(), -1;
    Py_ssize_t count = len / view->itemsize;
    for (Py_ssize_t i = 0; i < count; i++) {
        buffer_indices(view, i, order == 'F' ? 'F' : 'C', indices);
        void *item = PyBuffer_GetPointer(view, indices);
        if (into_view) memcpy(item, (const char *)source + i * view->itemsize, (size_t)view->itemsize);
        else memcpy((char *)destination + i * view->itemsize, item, (size_t)view->itemsize);
    }
    PyMem_Free(indices);
    return 0;
}

int PyBuffer_ToContiguous(void *buf, const Py_buffer *view, Py_ssize_t len, char order) {
    return buffer_copy_contiguous(buf, NULL, view, len, order, 0);
}

int PyBuffer_FromContiguous(const Py_buffer *view, const void *buf, Py_ssize_t len, char order) {
    if (view && view->readonly) { PyErr_SetString(PyExc_BufferError, "cannot write to a read-only buffer"); return -1; }
    return buffer_copy_contiguous(NULL, buf, view, len, order, 1);
}

PyObject *PyObject_Bytes(PyObject *o) {
    if (PyBytes_CheckExact(o)) return Py_NewRef(o);
    PyObject *m;
    if (PyObject_GetOptionalAttrString(o, "__bytes__", &m) < 0) return NULL;
    if (m) {
        PyObject *r = PyObject_CallNoArgs(m);
        Py_DECREF(m);
        if (r && !PyBytes_Check(r)) { PyErr_Format(PyExc_TypeError, "__bytes__ returned non-bytes (type %s)", Py_TYPE(r)->tp_name); Py_DECREF(r); return NULL; }
        return r;
    }
    return PyBytes_FromObject(o);
}

/* ---- format ------------------------------------------------------------ */

PyObject *PyObject_Format(PyObject *o, PyObject *spec) {
    PyObject *empty = NULL;
    if (!spec) { empty = PyUnicode_FromString(""); spec = empty; }
    PyObject *r = NULL;
    if (PyUnicode_GET_LENGTH(spec) == 0) {
        if (PyUnicode_CheckExact(o)) { r = Py_NewRef(o); goto done; }
        if (PyLong_CheckExact(o)) { r = PyObject_Str(o); goto done; }
    }
    PyObject *m = piper_type_lookup(Py_TYPE(o), piper_intern("__format__"));
    if (!m) { PyErr_Format(PyExc_TypeError, "Type %s doesn't define __format__", Py_TYPE(o)->tp_name); goto done; }
    PyObject *args[2] = { o, spec };
    r = PyObject_Vectorcall(m, args, 2, NULL);
    if (r && !PyUnicode_Check(r)) { PyErr_Format(PyExc_TypeError, "__format__ must return a str, not %s", Py_TYPE(r)->tp_name); Py_CLEAR(r); }
done:
    Py_XDECREF(empty);
    return r;
}

PyObject *piper_format_value(PyObject *v, int conversion, PyObject *spec) {
    PyObject *conv = NULL;
    switch (conversion) {
    case 's': conv = PyObject_Str(v); break;
    case 'r': conv = PyObject_Repr(v); break;
    case 'a': conv = PyObject_ASCII(v); break;
    default: conv = Py_NewRef(v);
    }
    if (!conv) return NULL;
    PyObject *r = PyObject_Format(conv, spec);
    Py_DECREF(conv);
    return r;
}

PyObject *piper_build_string(PyObject *const *parts, Py_ssize_t n) {
    PyObject *empty = PyUnicode_FromString("");
    PyObject *r = _PyUnicode_JoinArray(empty, parts, n);
    Py_DECREF(empty);
    return r;
}

PyObject *PyObject_Dir(PyObject *o) {
    PyObject *m = PyObject_GetAttrString(o, "__dir__");
    if (!m) return NULL;
    PyObject *r = PyObject_CallNoArgs(m);
    Py_DECREF(m);
    if (!r) return NULL;
    PyObject *l = PySequence_List(r);
    Py_DECREF(r);
    if (l && PyList_Sort(l) < 0) Py_CLEAR(l);
    return l;
}

int PyObject_Print(PyObject *o, void *fp, int flags) {
    PyObject *s = (flags & 1) ? PyObject_Str(o) : PyObject_Repr(o);
    if (!s) return -1;
    fputs(PyUnicode_AsUTF8(s), (FILE *)fp);
    Py_DECREF(s);
    return 0;
}

/* ---- object type --------------------------------------------------------- */

static PyObject *object_repr(PyObject *o) {
    PyTypeObject *tp = Py_TYPE(o);
    PyObject *mod = NULL;
    if (tp->tp_dict) {
        mod = PyDict_GetItemString(tp->tp_dict, "__module__");
        if (mod && (!PyUnicode_Check(mod) || PyUnicode_EqualToUTF8(mod, "builtins"))) mod = NULL;
    }
    if (mod) return PyUnicode_FromFormat("<%U.%s object at %p>", mod, _PyType_Name(tp), o);
    return PyUnicode_FromFormat("<%s object at %p>", tp->tp_name, o);
}

static PyObject *object_str(PyObject *o) { return PyObject_Repr(o); }
static Py_hash_t object_hash(PyObject *o) { return _Py_HashPointer(o); }

static PyObject *object_richcompare(PyObject *a, PyObject *b, int op) {
    switch (op) {
    case Py_EQ: if (a == b) Py_RETURN_TRUE; Py_RETURN_NOTIMPLEMENTED;
    case Py_NE: {
        if (Py_TYPE(a)->tp_richcompare == NULL) Py_RETURN_NOTIMPLEMENTED;
        PyObject *r = Py_TYPE(a)->tp_richcompare(a, b, Py_EQ);
        if (!r || r == Py_NotImplemented) return r;
        int ok = PyObject_IsTrue(r);
        Py_DECREF(r);
        if (ok < 0) return NULL;
        Py_RETURN_BOOL(!ok);
    }
    default: Py_RETURN_NOTIMPLEMENTED;
    }
}

static int excess_args(PyObject *args, PyObject *kwds) { return (args && PyTuple_GET_SIZE(args)) || (kwds && PyDict_Size(kwds)); }

PyObject *piper_object_new_default(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (excess_args(args, kwds)) {
        if (tp->tp_new != piper_object_new_default) { PyErr_SetString(PyExc_TypeError, "object.__new__() takes exactly one argument (the type to instantiate)"); return NULL; }
        if (tp->tp_init == piper_object_init_default) { PyErr_Format(PyExc_TypeError, "%s() takes no arguments", tp->tp_name); return NULL; }
    }
    if (tp->tp_flags & Py_TPFLAGS_IS_ABSTRACT) { PyErr_Format(PyExc_TypeError, "Can't instantiate abstract class %s", tp->tp_name); return NULL; }
    return tp->tp_alloc(tp, 0);
}

int piper_object_init_default(PyObject *self, PyObject *args, PyObject *kwds) {
    PyTypeObject *tp = Py_TYPE(self);
    if (excess_args(args, kwds)) {
        if (tp->tp_init != piper_object_init_default) { PyErr_SetString(PyExc_TypeError, "object.__init__() takes exactly one argument (the instance to initialize)"); return -1; }
        if (tp->tp_new == piper_object_new_default) { PyErr_Format(PyExc_TypeError, "%s.__init__() takes exactly one argument (the instance to initialize)", tp->tp_name); return -1; }
    }
    return 0;
}

static PyObject *object_class_get(PyObject *self, void *c) { PIPER_UNUSED(c); return Py_NewRef((PyObject *)Py_TYPE(self)); }
static int object_class_set(PyObject *self, PyObject *v, void *c) {
    PIPER_UNUSED(c);
    if (!v || !PyType_Check(v)) { PyErr_SetString(PyExc_TypeError, "__class__ must be set to a class"); return -1; }
    PyTypeObject *nt = (PyTypeObject *)v, *ot = Py_TYPE(self);
    if (!(nt->tp_flags & Py_TPFLAGS_HEAPTYPE) || !(ot->tp_flags & Py_TPFLAGS_HEAPTYPE)) { PyErr_SetString(PyExc_TypeError, "__class__ assignment only supported for mutable types or ModuleType subclasses"); return -1; }
    if (nt->tp_basicsize != ot->tp_basicsize || nt->tp_dictoffset != ot->tp_dictoffset) { PyErr_Format(PyExc_TypeError, "__class__ assignment: '%s' object layout differs from '%s'", nt->tp_name, ot->tp_name); return -1; }
    Py_INCREF(nt);
    Py_SET_TYPE(self, nt);
    Py_DECREF(ot);
    return 0;
}

static PyObject *object_dir(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    PyObject *d = PyDict_New();
    if (!d) return NULL;
    PyObject **dp = _PyObject_GetDictPtr(self);
    if (dp && *dp && PyDict_Update(d, *dp) < 0) { Py_DECREF(d); return NULL; }
    PyObject *mro = Py_TYPE(self)->tp_mro;
    for (Py_ssize_t i = 0; mro && i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject *t = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if (t->tp_dict && PyDict_Update(d, t->tp_dict) < 0) { Py_DECREF(d); return NULL; }
    }
    PyObject *keys = PyDict_Keys(d);
    Py_DECREF(d);
    return keys;
}

static PyObject *object_format(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (piper_args_range("__format__", nargs, 1, 1) < 0) return NULL;
    if (!PyUnicode_Check(args[0])) { PyErr_Format(PyExc_TypeError, "__format__() argument must be str, not %s", Py_TYPE(args[0])->tp_name); return NULL; }
    if (PyUnicode_GET_LENGTH(args[0]) > 0) { PyErr_Format(PyExc_TypeError, "unsupported format string passed to %s.__format__", Py_TYPE(self)->tp_name); return NULL; }
    return PyObject_Str(self);
}

static PyObject *object_sizeof(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    Py_ssize_t n = Py_TYPE(self)->tp_basicsize;
    if (Py_TYPE(self)->tp_itemsize) n += Py_ABS(Py_SIZE(self)) * Py_TYPE(self)->tp_itemsize;
    return PyLong_FromSsize_t(n);
}

static PyObject *object_reduce_ex(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    PIPER_UNUSED(args); PIPER_UNUSED(nargs);
    PyErr_Format(PyExc_TypeError, "cannot pickle '%s' object", Py_TYPE(self)->tp_name);
    return NULL;
}

static PyObject *object_init_subclass(PyObject *cls, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    PIPER_UNUSED(cls); PIPER_UNUSED(args); PIPER_UNUSED(nargs);
    if (kwnames && PyTuple_GET_SIZE(kwnames)) { PyErr_Format(PyExc_TypeError, "%U.__init_subclass__() takes no keyword arguments", PyTuple_GET_ITEM(kwnames, 0)); return NULL; }
    Py_RETURN_NONE;
}

static PyObject *object_subclasshook(PyObject *cls, PyObject *const *args, Py_ssize_t nargs) { PIPER_UNUSED(cls); PIPER_UNUSED(args); PIPER_UNUSED(nargs); Py_RETURN_NOTIMPLEMENTED; }

static PyObject *object_getstate(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    PyObject **dp = _PyObject_GetDictPtr(self);
    if (dp && *dp) return PyDict_Copy(*dp);
    Py_RETURN_NONE;
}

static PyMethodDef object_methods[] = {
    { "__dir__", object_dir, METH_NOARGS, NULL },
    { "__format__", (PyCFunction)(void (*)(void))object_format, METH_FASTCALL, NULL },
    { "__sizeof__", object_sizeof, METH_NOARGS, NULL },
    { "__reduce_ex__", (PyCFunction)(void (*)(void))object_reduce_ex, METH_FASTCALL, NULL },
    { "__reduce__", (PyCFunction)(void (*)(void))object_reduce_ex, METH_FASTCALL, NULL },
    { "__init_subclass__", (PyCFunction)(void (*)(void))object_init_subclass, METH_FASTCALL | METH_KEYWORDS | METH_CLASS, NULL },
    { "__subclasshook__", (PyCFunction)(void (*)(void))object_subclasshook, METH_FASTCALL | METH_CLASS, NULL },
    { "__getstate__", object_getstate, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyGetSetDef object_getsets[] = {
    { "__class__", object_class_get, object_class_set, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};

PyTypeObject PyBaseObject_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "object",
    .tp_basicsize = sizeof(PyObject),
    .tp_dealloc = object_dealloc,
    .tp_repr = object_repr,
    .tp_hash = object_hash,
    .tp_str = object_str,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_setattro = PyObject_GenericSetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE,
    .tp_richcompare = object_richcompare,
    .tp_methods = object_methods,
    .tp_getset = object_getsets,
    .tp_init = piper_object_init_default,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = piper_object_new_default,
    .tp_free = PyObject_Free,
};
