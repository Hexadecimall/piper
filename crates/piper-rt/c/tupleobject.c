/* tuple */
#include "internal.h"

static PyTupleObject *empty_tuple = NULL;

PyObject *PyTuple_New(Py_ssize_t n) {
    if (n < 0) { PyErr_BadInternalCall(); return NULL; }
    if (n == 0 && empty_tuple) return Py_NewRef((PyObject *)empty_tuple);
    PyTupleObject *t = PyObject_Malloc(offsetof(PyTupleObject, ob_item) + (size_t)(n ? n : 1) * sizeof(PyObject *));
    if (!t) return PyErr_NoMemory();
    PyObject_InitVar((PyVarObject *)t, &PyTuple_Type, n);
    t->ob_hash = -1;
    for (Py_ssize_t i = 0; i < n; i++) t->ob_item[i] = NULL;
    if (n == 0) { empty_tuple = t; t->ob_base.ob_base.ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT; }
    return (PyObject *)t;
}

static void tuple_dealloc(PyObject *o) {
    PyTupleObject *t = (PyTupleObject *)o;
    for (Py_ssize_t i = 0; i < Py_SIZE(t); i++) Py_XDECREF(t->ob_item[i]);
    Py_TYPE(o)->tp_free(o);
}

Py_ssize_t PyTuple_Size(PyObject *t) { if (!PyTuple_Check(t)) { PyErr_BadInternalCall(); return -1; } return Py_SIZE(t); }
PyObject *PyTuple_GetItem(PyObject *t, Py_ssize_t i) {
    if (!PyTuple_Check(t)) { PyErr_BadInternalCall(); return NULL; }
    if (i < 0 || i >= Py_SIZE(t)) { PyErr_SetString(PyExc_IndexError, "tuple index out of range"); return NULL; }
    return PyTuple_GET_ITEM(t, i);
}
int PyTuple_SetItem(PyObject *t, Py_ssize_t i, PyObject *v) {
    if (!PyTuple_Check(t) || Py_REFCNT(t) != 1) { Py_XDECREF(v); PyErr_BadInternalCall(); return -1; }
    if (i < 0 || i >= Py_SIZE(t)) { Py_XDECREF(v); PyErr_SetString(PyExc_IndexError, "tuple assignment index out of range"); return -1; }
    Py_XSETREF(PyTuple_GET_ITEM(t, i), v);
    return 0;
}
PyObject *PyTuple_FromArray(PyObject *const *items, Py_ssize_t n) {
    PyObject *t = PyTuple_New(n);
    if (!t) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (!items[i]) {
            Py_DECREF(t);
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_SystemError, "NULL object while constructing tuple");
            return NULL;
        }
        PyTuple_SET_ITEM(t, i, Py_NewRef(items[i]));
    }
    return t;
}
PyObject *piper_tuple_const(PyObject *const *items, Py_ssize_t n) { return PyTuple_FromArray(items, n); }
PyObject *PyTuple_Pack(Py_ssize_t n, ...) {
    va_list va;
    va_start(va, n);
    PyObject *t = PyTuple_New(n);
    if (!t) { va_end(va); return NULL; }
    for (Py_ssize_t i = 0; i < n; i++) PyTuple_SET_ITEM(t, i, Py_NewRef(va_arg(va, PyObject *)));
    va_end(va);
    return t;
}
PyObject *PyTuple_GetSlice(PyObject *t, Py_ssize_t a, Py_ssize_t b) {
    Py_ssize_t n = Py_SIZE(t);
    if (a < 0) a = 0;
    if (b > n) b = n;
    if (b < a) b = a;
    if (a == 0 && b == n && PyTuple_CheckExact(t)) return Py_NewRef(t);
    return PyTuple_FromArray(PyTuple_ITEMS(t) + a, b - a);
}
int _PyTuple_Resize(PyObject **pt, Py_ssize_t n) {
    PyTupleObject *t = (PyTupleObject *)*pt;
    Py_ssize_t old = Py_SIZE(t);
    if (old == n) return 0;
    if (n == 0) { Py_DECREF(t); *pt = PyTuple_New(0); return 0; }
    for (Py_ssize_t i = n; i < old; i++) Py_CLEAR(t->ob_item[i]);
    PyTupleObject *nt = PyObject_Realloc(t, offsetof(PyTupleObject, ob_item) + (size_t)n * sizeof(PyObject *));
    if (!nt) { *pt = NULL; PyErr_NoMemory(); return -1; }
    for (Py_ssize_t i = old; i < n; i++) nt->ob_item[i] = NULL;
    Py_SET_SIZE(nt, n);
    nt->ob_hash = -1;
    *pt = (PyObject *)nt;
    return 0;
}

PyObject *piper_tuple_from_iterable(PyObject *it) {
    PyObject *l = piper_list_from_iterable(it);
    if (!l) return NULL;
    PyObject *t = PyList_AsTuple(l);
    Py_DECREF(l);
    return t;
}

PyObject *piper_repr_join(const char *open, PyObject *const *items, Py_ssize_t n, const char *close, int trailing_comma_single) {
    PyObject *parts = PyList_New(0);
    if (!parts) return NULL;
    PyObject *s = PyUnicode_FromString(open);
    PyList_Append(parts, s); Py_DECREF(s);
    PyObject *sep = PyUnicode_FromString(", ");
    for (Py_ssize_t i = 0; i < n; i++) {
        if (i) PyList_Append(parts, sep);
        PyObject *r = PyObject_Repr(items[i]);
        if (!r) { Py_DECREF(parts); Py_DECREF(sep); return NULL; }
        PyList_Append(parts, r);
        Py_DECREF(r);
    }
    Py_DECREF(sep);
    if (n == 1 && trailing_comma_single) { s = PyUnicode_FromString(","); PyList_Append(parts, s); Py_DECREF(s); }
    s = PyUnicode_FromString(close);
    PyList_Append(parts, s); Py_DECREF(s);
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts);
    return r;
}

static PyObject *tuple_repr(PyObject *t) {
    if (Py_SIZE(t) == 0) return PyUnicode_FromString("()");
    int rc = Py_ReprEnter(t);
    if (rc != 0) return rc > 0 ? PyUnicode_FromString("(...)") : NULL;
    PyObject *r = piper_repr_join("(", PyTuple_ITEMS(t), Py_SIZE(t), ")", 1);
    Py_ReprLeave(t);
    return r;
}

static Py_hash_t tuple_hash(PyObject *o) {
    /* xxHash-based combination, as in CPython 3.8+ */
    PyTupleObject *t = (PyTupleObject *)o;
    Py_ssize_t n = Py_SIZE(t);
    Py_uhash_t acc = 2870177450012600261ULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_hash_t h = PyObject_Hash(t->ob_item[i]);
        if (h == -1) return -1;
        acc += (Py_uhash_t)h * 14029467366897019727ULL;
        acc = (acc << 31) | (acc >> 33);
        acc *= 11400714785074694791ULL;
    }
    acc += (Py_uhash_t)n ^ (2870177450012600261ULL ^ 3527539UL);
    if (acc == (Py_uhash_t)-1) return 1546275796;
    return (Py_hash_t)acc;
}

static Py_ssize_t tuple_length(PyObject *t) { return Py_SIZE(t); }
static PyObject *tuple_item(PyObject *t, Py_ssize_t i) {
    if (i < 0) i += Py_SIZE(t);
    if (i < 0 || i >= Py_SIZE(t)) { PyErr_SetString(PyExc_IndexError, "tuple index out of range"); return NULL; }
    return Py_NewRef(PyTuple_GET_ITEM(t, i));
}
static PyObject *tuple_subscript(PyObject *t, PyObject *item) {
    if (PyIndex_Check(item)) { Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError); if (i == -1 && PyErr_Occurred()) return NULL; return tuple_item(t, i); }
    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(item, Py_SIZE(t), &start, &stop, &step, &len) < 0) return NULL;
        if (step == 1) return PyTuple_GetSlice(t, start, stop);
        PyObject *r = PyTuple_New(len);
        for (Py_ssize_t i = 0, cur = start; i < len; i++, cur += step) PyTuple_SET_ITEM(r, i, Py_NewRef(PyTuple_GET_ITEM(t, cur)));
        return r;
    }
    PyErr_Format(PyExc_TypeError, "tuple indices must be integers or slices, not %s", Py_TYPE(item)->tp_name);
    return NULL;
}
static PyObject *tuple_concat(PyObject *a, PyObject *b) {
    if (!PyTuple_Check(b)) { PyErr_Format(PyExc_TypeError, "can only concatenate tuple (not \"%s\") to tuple", Py_TYPE(b)->tp_name); return NULL; }
    Py_ssize_t na = Py_SIZE(a), nb = Py_SIZE(b);
    if (nb == 0 && PyTuple_CheckExact(a)) return Py_NewRef(a);
    PyObject *r = PyTuple_New(na + nb);
    if (!r) return NULL;
    for (Py_ssize_t i = 0; i < na; i++) PyTuple_SET_ITEM(r, i, Py_NewRef(PyTuple_GET_ITEM(a, i)));
    for (Py_ssize_t i = 0; i < nb; i++) PyTuple_SET_ITEM(r, na + i, Py_NewRef(PyTuple_GET_ITEM(b, i)));
    return r;
}
static PyObject *tuple_repeat(PyObject *a, Py_ssize_t n) {
    if (n < 0) n = 0;
    Py_ssize_t len = Py_SIZE(a);
    if (n == 1 && PyTuple_CheckExact(a)) return Py_NewRef(a);
    if (len && n > PY_SSIZE_T_MAX / len) return PyErr_NoMemory();
    PyObject *r = PyTuple_New(len * n);
    if (!r) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) for (Py_ssize_t j = 0; j < len; j++) PyTuple_SET_ITEM(r, i * len + j, Py_NewRef(PyTuple_GET_ITEM(a, j)));
    return r;
}
static int tuple_contains(PyObject *t, PyObject *v) {
    for (Py_ssize_t i = 0; i < Py_SIZE(t); i++) { int r = PyObject_RichCompareBool(PyTuple_GET_ITEM(t, i), v, Py_EQ); if (r) return r; }
    return 0;
}

static PyObject *tuple_richcompare(PyObject *v, PyObject *w, int op) {
    if (!PyTuple_Check(v) || !PyTuple_Check(w)) Py_RETURN_NOTIMPLEMENTED;
    Py_ssize_t nv = Py_SIZE(v), nw = Py_SIZE(w), i;
    for (i = 0; i < nv && i < nw; i++) {
        int k = PyObject_RichCompareBool(PyTuple_GET_ITEM(v, i), PyTuple_GET_ITEM(w, i), Py_EQ);
        if (k < 0) return NULL;
        if (!k) break;
    }
    if (i >= nv || i >= nw) {
        int r;
        switch (op) { case Py_LT: r = nv < nw; break; case Py_LE: r = nv <= nw; break; case Py_EQ: r = nv == nw; break; case Py_NE: r = nv != nw; break; case Py_GT: r = nv > nw; break; default: r = nv >= nw; }
        Py_RETURN_BOOL(r);
    }
    if (op == Py_EQ) Py_RETURN_FALSE;
    if (op == Py_NE) Py_RETURN_TRUE;
    return PyObject_RichCompare(PyTuple_GET_ITEM(v, i), PyTuple_GET_ITEM(w, i), op);
}

static PyObject *tuple_index(PyObject *t, PyObject *const *a, Py_ssize_t n) {
    if (n < 1) { PyErr_SetString(PyExc_TypeError, "index expected at least 1 argument"); return NULL; }
    Py_ssize_t start = 0, stop = Py_SIZE(t);
    if (n >= 2) { start = PyNumber_AsSsize_t(a[1], NULL); if (start == -1 && PyErr_Occurred()) return NULL; if (start < 0) { start += Py_SIZE(t); if (start < 0) start = 0; } }
    if (n >= 3) { stop = PyNumber_AsSsize_t(a[2], NULL); if (stop == -1 && PyErr_Occurred()) return NULL; if (stop < 0) { stop += Py_SIZE(t); } if (stop > Py_SIZE(t)) stop = Py_SIZE(t); }
    for (Py_ssize_t i = start; i < stop; i++) { int r = PyObject_RichCompareBool(PyTuple_GET_ITEM(t, i), a[0], Py_EQ); if (r < 0) return NULL; if (r) return PyLong_FromSsize_t(i); }
    PyErr_SetString(PyExc_ValueError, "tuple.index(x): x not in tuple");
    return NULL;
}
static PyObject *tuple_count(PyObject *t, PyObject *v) {
    Py_ssize_t c = 0;
    for (Py_ssize_t i = 0; i < Py_SIZE(t); i++) { int r = PyObject_RichCompareBool(PyTuple_GET_ITEM(t, i), v, Py_EQ); if (r < 0) return NULL; c += r; }
    return PyLong_FromSsize_t(c);
}
static PyObject *tuple_getnewargs(PyObject *t, PyObject *u) { PIPER_UNUSED(u); PyObject *c = PyTuple_GetSlice(t, 0, Py_SIZE(t)); PyObject *r = PyTuple_Pack(1, c); Py_DECREF(c); return r; }
static PyObject *tuple_class_getitem(PyObject *cls, PyObject *arg) { extern PyObject *piper_generic_alias_new(PyObject *origin, PyObject *args); return piper_generic_alias_new(cls, arg); }

static PyMethodDef tuple_methods[] = {
    { "index", (PyCFunction)(void (*)(void))tuple_index, METH_FASTCALL, NULL },
    { "count", tuple_count, METH_O, NULL },
    { "__getnewargs__", tuple_getnewargs, METH_NOARGS, NULL },
    { "__class_getitem__", tuple_class_getitem, METH_O | METH_CLASS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyObject *tuple_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "tuple() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "tuple expected at most 1 argument, got %zd", n); return NULL; }
    PyObject *r = n == 0 ? PyTuple_New(0) : PySequence_Tuple(PyTuple_GET_ITEM(args, 0));
    if (!r || tp == &PyTuple_Type) return r;
    PyObject *sub = tp->tp_alloc(tp, Py_SIZE(r));
    if (sub) { ((PyTupleObject *)sub)->ob_hash = -1; for (Py_ssize_t i = 0; i < Py_SIZE(r); i++) PyTuple_SET_ITEM(sub, i, Py_NewRef(PyTuple_GET_ITEM(r, i))); }
    Py_DECREF(r);
    return sub;
}

typedef struct { PyObject_HEAD PyObject *seq; Py_ssize_t index; } tupleiterobject;
static void tupleiter_dealloc(PyObject *o) { Py_XDECREF(((tupleiterobject *)o)->seq); PyObject_Free(o); }
static PyObject *tupleiter_next(PyObject *o) {
    tupleiterobject *it = (tupleiterobject *)o;
    if (!it->seq) return NULL;
    if (it->index < Py_SIZE(it->seq)) return Py_NewRef(PyTuple_GET_ITEM(it->seq, it->index++));
    Py_CLEAR(it->seq);
    return NULL;
}
static PyObject *tupleiter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); tupleiterobject *it = (tupleiterobject *)o; return PyLong_FromSsize_t(it->seq ? Py_SIZE(it->seq) - it->index : 0); }
static PyMethodDef tupleiter_methods[] = { { "__length_hint__", tupleiter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyTupleIter_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "tuple_iterator", .tp_basicsize = sizeof(tupleiterobject), .tp_dealloc = tupleiter_dealloc,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = tupleiter_next, .tp_methods = tupleiter_methods,
};
static PyObject *tuple_iter(PyObject *t) {
    tupleiterobject *it = PyObject_New(tupleiterobject, &PyTupleIter_Type);
    if (!it) return NULL;
    it->seq = Py_NewRef(t);
    it->index = 0;
    return (PyObject *)it;
}

static PySequenceMethods tuple_as_sequence = { .sq_length = tuple_length, .sq_concat = tuple_concat, .sq_repeat = tuple_repeat, .sq_item = tuple_item, .sq_contains = tuple_contains };
static PyMappingMethods tuple_as_mapping = { .mp_length = tuple_length, .mp_subscript = tuple_subscript };

PyTypeObject PyTuple_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "tuple",
    .tp_basicsize = offsetof(PyTupleObject, ob_item),
    .tp_itemsize = sizeof(PyObject *),
    .tp_dealloc = tuple_dealloc,
    .tp_repr = tuple_repr,
    .tp_as_sequence = &tuple_as_sequence,
    .tp_as_mapping = &tuple_as_mapping,
    .tp_hash = tuple_hash,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_TUPLE_SUBCLASS | Py_TPFLAGS_SEQUENCE | Py_TPFLAGS_MATCH_SELF,
    .tp_richcompare = tuple_richcompare,
    .tp_iter = tuple_iter,
    .tp_methods = tuple_methods,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = tuple_new,
    .tp_free = PyObject_Free,
};
