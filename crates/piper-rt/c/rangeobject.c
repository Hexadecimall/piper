/* slice, range, and the small iterator types: seqiter, calliter, enumerate,
 * zip, map, filter, reversed. */
#include "internal.h"

/* ---- slice --------------------------------------------------------------- */

PyObject *PySlice_New(PyObject *start, PyObject *stop, PyObject *step) {
    PySliceObject *s = PyObject_New(PySliceObject, &PySlice_Type);
    if (!s) return NULL;
    s->start = Py_NewRef(start ? start : Py_None);
    s->stop = Py_NewRef(stop ? stop : Py_None);
    s->step = Py_NewRef(step ? step : Py_None);
    return (PyObject *)s;
}
PyObject *piper_build_slice(PyObject *a, PyObject *b, PyObject *c) { return PySlice_New(a, b, c); }
static void slice_dealloc(PyObject *o) { PySliceObject *s = (PySliceObject *)o; Py_DECREF(s->start); Py_DECREF(s->stop); Py_DECREF(s->step); PyObject_Free(o); }
static PyObject *slice_repr(PyObject *o) { PySliceObject *s = (PySliceObject *)o; return PyUnicode_FromFormat("slice(%R, %R, %R)", s->start, s->stop, s->step); }

int PySlice_Unpack(PyObject *o, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step) {
    PySliceObject *s = (PySliceObject *)o;
    if (s->step == Py_None) *step = 1;
    else {
        if (piper_index_ssize(s->step, step, NULL) < 0) return -1;
        if (*step == 0) { PyErr_SetString(PyExc_ValueError, "slice step cannot be zero"); return -1; }
        if (*step < -PY_SSIZE_T_MAX) *step = -PY_SSIZE_T_MAX;
    }
    if (s->start == Py_None) *start = *step < 0 ? PY_SSIZE_T_MAX : 0;
    else if (piper_index_ssize(s->start, start, NULL) < 0) return -1;
    if (s->stop == Py_None) *stop = *step < 0 ? PY_SSIZE_T_MIN : PY_SSIZE_T_MAX;
    else if (piper_index_ssize(s->stop, stop, NULL) < 0) return -1;
    return 0;
}
Py_ssize_t PySlice_AdjustIndices(Py_ssize_t length, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t step) {
    if (*start < 0) { *start += length; if (*start < 0) *start = step < 0 ? -1 : 0; }
    else if (*start >= length) *start = step < 0 ? length - 1 : length;
    if (*stop < 0) { *stop += length; if (*stop < 0) *stop = step < 0 ? -1 : 0; }
    else if (*stop >= length) *stop = step < 0 ? length - 1 : length;
    if (step < 0) { if (*stop < *start) return (*start - *stop - 1) / (-step) + 1; }
    else { if (*start < *stop) return (*stop - *start - 1) / step + 1; }
    return 0;
}
int PySlice_GetIndicesEx(PyObject *s, Py_ssize_t length, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step, Py_ssize_t *slicelength) {
    if (PySlice_Unpack(s, start, stop, step) < 0) return -1;
    *slicelength = PySlice_AdjustIndices(length, start, stop, *step);
    return 0;
}
static PyObject *slice_indices(PyObject *o, PyObject *len) {
    Py_ssize_t n = PyNumber_AsSsize_t(len, PyExc_OverflowError);
    if (n == -1 && PyErr_Occurred()) return NULL;
    if (n < 0) { PyErr_SetString(PyExc_ValueError, "length should not be negative"); return NULL; }
    Py_ssize_t start, stop, step;
    if (PySlice_Unpack(o, &start, &stop, &step) < 0) return NULL;
    PySlice_AdjustIndices(n, &start, &stop, step);
    PyObject *a = PyLong_FromSsize_t(start), *b = PyLong_FromSsize_t(stop), *c = PyLong_FromSsize_t(step);
    PyObject *r = PyTuple_Pack(3, a, b, c);
    Py_DECREF(a); Py_DECREF(b); Py_DECREF(c);
    return r;
}
static PyObject *slice_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PySlice_Check(b)) Py_RETURN_NOTIMPLEMENTED;
    PySliceObject *x = (PySliceObject *)a, *y = (PySliceObject *)b;
    PyObject *t1 = PyTuple_Pack(3, x->start, x->stop, x->step), *t2 = PyTuple_Pack(3, y->start, y->stop, y->step);
    PyObject *r = PyObject_RichCompare(t1, t2, op);
    Py_DECREF(t1); Py_DECREF(t2);
    return r;
}
static Py_hash_t slice_hash(PyObject *o) { PySliceObject *s = (PySliceObject *)o; PyObject *t = PyTuple_Pack(3, s->start, s->stop, s->step); Py_hash_t h = PyObject_Hash(t); Py_DECREF(t); return h; }
static PyObject *slice_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(tp);
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "slice() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n == 1) return PySlice_New(NULL, PyTuple_GET_ITEM(args, 0), NULL);
    if (n == 2) return PySlice_New(PyTuple_GET_ITEM(args, 0), PyTuple_GET_ITEM(args, 1), NULL);
    if (n == 3) return PySlice_New(PyTuple_GET_ITEM(args, 0), PyTuple_GET_ITEM(args, 1), PyTuple_GET_ITEM(args, 2));
    PyErr_Format(PyExc_TypeError, "slice expected at least 1 argument, got %zd", n);
    return NULL;
}
static PyMemberDef slice_members[] = {
    { "start", _Py_T_OBJECT, offsetof(PySliceObject, start), Py_READONLY, NULL },
    { "stop", _Py_T_OBJECT, offsetof(PySliceObject, stop), Py_READONLY, NULL },
    { "step", _Py_T_OBJECT, offsetof(PySliceObject, step), Py_READONLY, NULL },
    { NULL, 0, 0, 0, NULL },
};
static PyMethodDef slice_methods[] = { { "indices", slice_indices, METH_O, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PySlice_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "slice", .tp_basicsize = sizeof(PySliceObject), .tp_dealloc = slice_dealloc, .tp_repr = slice_repr, .tp_hash = slice_hash,
    .tp_getattro = PyObject_GenericGetAttr, .tp_richcompare = slice_richcompare, .tp_methods = slice_methods, .tp_members = slice_members, .tp_new = slice_new,
};

/* ---- range --------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *start, *stop, *step, *length; } rangeobject;

static PyObject *compute_range_length(PyObject *start, PyObject *stop, PyObject *step) {
    PyObject *zero = PyLong_FromLong(0), *one = PyLong_FromLong(1);
    int step_pos = PyObject_RichCompareBool(step, zero, Py_GT);
    PyObject *lo, *hi, *st;
    if (step_pos) { lo = start; hi = stop; st = Py_NewRef(step); }
    else { lo = stop; hi = start; st = PyNumber_Negative(step); }
    PyObject *result;
    if (PyObject_RichCompareBool(lo, hi, Py_GE)) result = Py_NewRef(zero);
    else {
        PyObject *diff = PyNumber_Subtract(hi, lo);
        PyObject *d1 = PyNumber_Subtract(diff, one);
        PyObject *q = PyNumber_FloorDivide(d1, st);
        result = PyNumber_Add(q, one);
        Py_DECREF(diff); Py_DECREF(d1); Py_DECREF(q);
    }
    Py_DECREF(st); Py_DECREF(zero); Py_DECREF(one);
    return result;
}

PyObject *piper_range_new(PyObject *start, PyObject *stop, PyObject *step) {
    rangeobject *r = PyObject_New(rangeobject, &PyRange_Type);
    if (!r) return NULL;
    r->start = Py_NewRef(start); r->stop = Py_NewRef(stop); r->step = Py_NewRef(step);
    r->length = compute_range_length(start, stop, step);
    if (!r->length) { Py_DECREF(r); return NULL; }
    return (PyObject *)r;
}
static void range_dealloc(PyObject *o) { rangeobject *r = (rangeobject *)o; Py_DECREF(r->start); Py_DECREF(r->stop); Py_DECREF(r->step); Py_DECREF(r->length); PyObject_Free(o); }
static PyObject *range_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(tp);
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "range() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    PyObject *start = NULL, *stop = NULL, *step = NULL, *r = NULL;
    if (n == 1) { stop = PyNumber_Index(PyTuple_GET_ITEM(args, 0)); if (!stop) return NULL; start = PyLong_FromLong(0); step = PyLong_FromLong(1); }
    else if (n == 2 || n == 3) {
        start = PyNumber_Index(PyTuple_GET_ITEM(args, 0)); if (!start) return NULL;
        stop = PyNumber_Index(PyTuple_GET_ITEM(args, 1)); if (!stop) { Py_DECREF(start); return NULL; }
        step = n == 3 ? PyNumber_Index(PyTuple_GET_ITEM(args, 2)) : PyLong_FromLong(1);
        if (!step) { Py_DECREF(start); Py_DECREF(stop); return NULL; }
        if (_PyLong_Sign(step) == 0) { PyErr_SetString(PyExc_ValueError, "range() arg 3 must not be zero"); goto done; }
    } else { PyErr_Format(PyExc_TypeError, "range expected at least 1 argument, got %zd", n); return NULL; }
    r = piper_range_new(start, stop, step);
done:
    Py_DECREF(start); Py_DECREF(stop); Py_DECREF(step);
    return r;
}
static PyObject *range_repr(PyObject *o) {
    rangeobject *r = (rangeobject *)o;
    long step = PyLong_AsLong(r->step);
    if (step == 1) return PyUnicode_FromFormat("range(%R, %R)", r->start, r->stop);
    return PyUnicode_FromFormat("range(%R, %R, %R)", r->start, r->stop, r->step);
}
static Py_ssize_t range_length(PyObject *o) { return PyLong_AsSsize_t(((rangeobject *)o)->length); }
static PyObject *range_item_obj(rangeobject *r, PyObject *i) {
    /* start + i*step */
    PyObject *m = PyNumber_Multiply(i, r->step);
    if (!m) return NULL;
    PyObject *v = PyNumber_Add(r->start, m);
    Py_DECREF(m);
    return v;
}
static PyObject *range_item(PyObject *o, Py_ssize_t i) {
    rangeobject *r = (rangeobject *)o;
    Py_ssize_t n = range_length(o);
    if (i < 0) i += n;
    if (i < 0 || i >= n) { PyErr_SetString(PyExc_IndexError, "range object index out of range"); return NULL; }
    PyObject *idx = PyLong_FromSsize_t(i);
    PyObject *v = range_item_obj(r, idx);
    Py_DECREF(idx);
    return v;
}
static PyObject *range_subscript(PyObject *o, PyObject *item) {
    rangeobject *r = (rangeobject *)o;
    if (PyIndex_Check(item)) {
        PyObject *i = PyNumber_Index(item);
        if (!i) return NULL;
        if (_PyLong_Sign(i) < 0) { PyObject *t = PyNumber_Add(i, r->length); Py_DECREF(i); i = t; }
        int in_range = PyObject_RichCompareBool(i, r->length, Py_LT) == 1 && _PyLong_Sign(i) >= 0;
        if (!in_range) { Py_DECREF(i); PyErr_SetString(PyExc_IndexError, "range object index out of range"); return NULL; }
        PyObject *v = range_item_obj(r, i);
        Py_DECREF(i);
        return v;
    }
    if (PySlice_Check(item)) {
        Py_ssize_t n = range_length(o), start, stop, step, len;
        if (PySlice_GetIndicesEx(item, n, &start, &stop, &step, &len) < 0) return NULL;
        PyObject *s = PyLong_FromSsize_t(start), *e = PyLong_FromSsize_t(stop), *st = PyLong_FromSsize_t(step);
        PyObject *nstart = range_item_obj(r, s), *nstop = range_item_obj(r, e), *nstep = PyNumber_Multiply(r->step, st);
        PyObject *res = piper_range_new(nstart, nstop, nstep);
        Py_DECREF(s); Py_DECREF(e); Py_DECREF(st); Py_DECREF(nstart); Py_DECREF(nstop); Py_DECREF(nstep);
        return res;
    }
    PyErr_Format(PyExc_TypeError, "range indices must be integers or slices, not %s", Py_TYPE(item)->tp_name);
    return NULL;
}
static int range_contains(PyObject *o, PyObject *v) {
    rangeobject *r = (rangeobject *)o;
    if (PyLong_CheckExact(v) || PyBool_Check(v)) {
        int step_pos = _PyLong_Sign(r->step) > 0;
        int c1 = PyObject_RichCompareBool(v, r->start, step_pos ? Py_GE : Py_LE);
        int c2 = PyObject_RichCompareBool(v, r->stop, step_pos ? Py_LT : Py_GT);
        if (c1 < 0 || c2 < 0) return -1;
        if (!c1 || !c2) return 0;
        PyObject *diff = PyNumber_Subtract(v, r->start);
        PyObject *m = PyNumber_Remainder(diff, r->step);
        int zero = _PyLong_Sign(m) == 0;
        Py_DECREF(diff); Py_DECREF(m);
        return zero;
    }
    PyObject *it = PyObject_GetIter(o);
    for (;;) { PyObject *x = PyIter_Next(it); if (!x) break; int c = PyObject_RichCompareBool(x, v, Py_EQ); Py_DECREF(x); if (c) { Py_DECREF(it); return c; } }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
static PyObject *range_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PyRange_Check(b) || (op != Py_EQ && op != Py_NE)) Py_RETURN_NOTIMPLEMENTED;
    rangeobject *x = (rangeobject *)a, *y = (rangeobject *)b;
    int eq;
    if (a == b) eq = 1;
    else {
        int l = PyObject_RichCompareBool(x->length, y->length, Py_EQ);
        if (l < 0) return NULL;
        if (!l) eq = 0;
        else if (_PyLong_Sign(x->length) == 0) eq = 1;
        else {
            int s = PyObject_RichCompareBool(x->start, y->start, Py_EQ);
            if (s < 0) return NULL;
            if (!s) eq = 0;
            else {
                PyObject *one = PyLong_FromLong(1);
                int single = PyObject_RichCompareBool(x->length, one, Py_EQ);
                Py_DECREF(one);
                eq = single ? 1 : PyObject_RichCompareBool(x->step, y->step, Py_EQ);
            }
        }
    }
    Py_RETURN_BOOL(op == Py_EQ ? eq : !eq);
}
static Py_hash_t range_hash(PyObject *o) {
    rangeobject *r = (rangeobject *)o;
    PyObject *t;
    if (_PyLong_Sign(r->length) == 0) t = PyTuple_Pack(3, r->length, Py_None, Py_None);
    else { PyObject *one = PyLong_FromLong(1); int single = PyObject_RichCompareBool(r->length, one, Py_EQ); Py_DECREF(one); t = single ? PyTuple_Pack(3, r->length, r->start, Py_None) : PyTuple_Pack(3, r->length, r->start, r->step); }
    Py_hash_t h = PyObject_Hash(t);
    Py_DECREF(t);
    return h;
}
static PyObject *range_count(PyObject *o, PyObject *v) { int c = range_contains(o, v); if (c < 0) return NULL; return PyLong_FromLong(c); }
static PyObject *range_index(PyObject *o, PyObject *v) {
    rangeobject *r = (rangeobject *)o;
    int c = range_contains(o, v);
    if (c < 0) return NULL;
    if (!c) { PyErr_Format(PyExc_ValueError, "%R is not in range", v); return NULL; }
    PyObject *diff = PyNumber_Subtract(v, r->start);
    PyObject *idx = PyNumber_FloorDivide(diff, r->step);
    Py_DECREF(diff);
    return idx;
}
static PyObject *range_reduce(PyObject *o, PyObject *u) { PIPER_UNUSED(u); rangeobject *r = (rangeobject *)o; PyObject *args = PyTuple_Pack(3, r->start, r->stop, r->step); PyObject *t = PyTuple_Pack(2, (PyObject *)&PyRange_Type, args); Py_DECREF(args); return t; }
static PyObject *range_reversed(PyObject *o, PyObject *u) {
    PIPER_UNUSED(u);
    rangeobject *r = (rangeobject *)o;
    /* reversed(range(start, stop, step)) == range(start + (len-1)*step, start - step, -step) */
    PyObject *one = PyLong_FromLong(1);
    PyObject *lm1 = PyNumber_Subtract(r->length, one);
    PyObject *last = range_item_obj(r, lm1);
    PyObject *nstop = PyNumber_Subtract(r->start, r->step);
    PyObject *nstep = PyNumber_Negative(r->step);
    PyObject *rng = piper_range_new(last, nstop, nstep);
    Py_DECREF(one); Py_DECREF(lm1); Py_DECREF(last); Py_DECREF(nstop); Py_DECREF(nstep);
    if (!rng) return NULL;
    PyObject *it = PyObject_GetIter(rng);
    Py_DECREF(rng);
    return it;
}
static PyMethodDef range_methods[] = { { "count", range_count, METH_O, NULL }, { "index", range_index, METH_O, NULL }, { "__reduce__", range_reduce, METH_NOARGS, NULL }, { "__reversed__", range_reversed, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyMemberDef range_members[] = {
    { "start", _Py_T_OBJECT, offsetof(rangeobject, start), Py_READONLY, NULL },
    { "stop", _Py_T_OBJECT, offsetof(rangeobject, stop), Py_READONLY, NULL },
    { "step", _Py_T_OBJECT, offsetof(rangeobject, step), Py_READONLY, NULL },
    { NULL, 0, 0, 0, NULL },
};

typedef struct { PyObject_HEAD long start, step, len, index; } rangeiterobject;
typedef struct { PyObject_HEAD PyObject *start, *step, *len, *index; } longrangeiterobject;
static PyObject *rangeiter_next(PyObject *o) {
    rangeiterobject *it = (rangeiterobject *)o;
    if (it->index < it->len) return PyLong_FromLong(it->start + it->index++ * it->step);
    return NULL;
}
static PyObject *rangeiter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); rangeiterobject *it = (rangeiterobject *)o; return PyLong_FromLong(it->len - it->index); }
static PyMethodDef rangeiter_methods[] = { { "__length_hint__", rangeiter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static void rangeiter_dealloc(PyObject *o) { PyObject_Free(o); }
PyTypeObject PyRangeIter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "range_iterator", .tp_basicsize = sizeof(rangeiterobject), .tp_dealloc = rangeiter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = rangeiter_next, .tp_methods = rangeiter_methods };
static void longrangeiter_dealloc(PyObject *o) { longrangeiterobject *it = (longrangeiterobject *)o; Py_DECREF(it->start); Py_DECREF(it->step); Py_DECREF(it->len); Py_DECREF(it->index); PyObject_Free(o); }
static PyObject *longrangeiter_next(PyObject *o) {
    longrangeiterobject *it = (longrangeiterobject *)o;
    if (PyObject_RichCompareBool(it->index, it->len, Py_LT) != 1) return NULL;
    PyObject *m = PyNumber_Multiply(it->index, it->step);
    PyObject *v = PyNumber_Add(it->start, m);
    Py_DECREF(m);
    PyObject *one = PyLong_FromLong(1);
    PyObject *ni = PyNumber_Add(it->index, one);
    Py_DECREF(one);
    Py_SETREF(it->index, ni);
    return v;
}
static PyTypeObject PyLongRangeIter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "longrange_iterator", .tp_basicsize = sizeof(longrangeiterobject), .tp_dealloc = longrangeiter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = longrangeiter_next };
static PyObject *range_iter(PyObject *o) {
    rangeobject *r = (rangeobject *)o;
    int o1, o2, o3;
    long start = PyLong_AsLongAndOverflow(r->start, &o1), step = PyLong_AsLongAndOverflow(r->step, &o2), len = PyLong_AsLongAndOverflow(r->length, &o3);
    if (!o1 && !o2 && !o3 && (long long)start + (long long)step * len < LONG_MAX / 2 && (long long)start + (long long)step * len > LONG_MIN / 2) {
        rangeiterobject *it = PyObject_New(rangeiterobject, &PyRangeIter_Type);
        if (!it) return NULL;
        it->start = start; it->step = step; it->len = len; it->index = 0;
        return (PyObject *)it;
    }
    longrangeiterobject *it = PyObject_New(longrangeiterobject, &PyLongRangeIter_Type);
    if (!it) return NULL;
    it->start = Py_NewRef(r->start); it->step = Py_NewRef(r->step); it->len = Py_NewRef(r->length); it->index = PyLong_FromLong(0);
    return (PyObject *)it;
}
static int range_bool(PyObject *o) { return _PyLong_Sign(((rangeobject *)o)->length) != 0; }
static PySequenceMethods range_as_sequence = { .sq_length = range_length, .sq_item = range_item, .sq_contains = range_contains };
static PyMappingMethods range_as_mapping = { .mp_length = range_length, .mp_subscript = range_subscript };
static PyNumberMethods range_as_number = { .nb_bool = range_bool };
PyTypeObject PyRange_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "range", .tp_basicsize = sizeof(rangeobject), .tp_dealloc = range_dealloc, .tp_repr = range_repr,
    .tp_as_number = &range_as_number, .tp_as_sequence = &range_as_sequence, .tp_as_mapping = &range_as_mapping, .tp_hash = range_hash,
    .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_SEQUENCE, .tp_richcompare = range_richcompare, .tp_iter = range_iter,
    .tp_methods = range_methods, .tp_members = range_members, .tp_new = range_new,
};

/* ---- seqiter / calliter --------------------------------------------------- */

typedef struct { PyObject_HEAD Py_ssize_t index; PyObject *seq; } seqiterobject;
static void seqiter_dealloc(PyObject *o) { Py_XDECREF(((seqiterobject *)o)->seq); PyObject_Free(o); }
static PyObject *seqiter_next(PyObject *o) {
    seqiterobject *it = (seqiterobject *)o;
    if (!it->seq) return NULL;
    PyObject *r = PySequence_GetItem(it->seq, it->index);
    if (r) { it->index++; return r; }
    if (PyErr_ExceptionMatches(PyExc_IndexError) || PyErr_ExceptionMatches(PyExc_StopIteration)) { PyErr_Clear(); Py_CLEAR(it->seq); }
    return NULL;
}
PyTypeObject PySeqIter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "iterator", .tp_basicsize = sizeof(seqiterobject), .tp_dealloc = seqiter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = seqiter_next };
PyObject *PySeqIter_New(PyObject *seq) {
    seqiterobject *it = PyObject_New(seqiterobject, &PySeqIter_Type);
    if (!it) return NULL;
    it->index = 0;
    it->seq = Py_NewRef(seq);
    return (PyObject *)it;
}
PyObject *piper_seq_iter_new(PyObject *seq) { return PySeqIter_New(seq); }

typedef struct { PyObject_HEAD PyObject *callable, *sentinel; } calliterobject;
static void calliter_dealloc(PyObject *o) { calliterobject *it = (calliterobject *)o; Py_XDECREF(it->callable); Py_XDECREF(it->sentinel); PyObject_Free(o); }
static PyObject *calliter_next(PyObject *o) {
    calliterobject *it = (calliterobject *)o;
    if (!it->callable) return NULL;
    PyObject *r = PyObject_CallNoArgs(it->callable);
    if (!r) { if (PyErr_ExceptionMatches(PyExc_StopIteration)) { PyErr_Clear(); Py_CLEAR(it->callable); Py_CLEAR(it->sentinel); } return NULL; }
    int eq = PyObject_RichCompareBool(it->sentinel, r, Py_EQ);
    if (eq == 0) return r;
    Py_DECREF(r);
    if (eq > 0) { Py_CLEAR(it->callable); Py_CLEAR(it->sentinel); }
    return NULL;
}
PyTypeObject PyCallIter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "callable_iterator", .tp_basicsize = sizeof(calliterobject), .tp_dealloc = calliter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = calliter_next };
PyObject *PyCallIter_New(PyObject *callable, PyObject *sentinel) {
    calliterobject *it = PyObject_New(calliterobject, &PyCallIter_Type);
    if (!it) return NULL;
    it->callable = Py_NewRef(callable);
    it->sentinel = Py_NewRef(sentinel);
    return (PyObject *)it;
}

/* ---- enumerate --------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *it; PyObject *index; } enumobject;
static void enum_dealloc(PyObject *o) { enumobject *e = (enumobject *)o; Py_XDECREF(e->it); Py_XDECREF(e->index); PyObject_Free(o); }
static PyObject *enum_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PyObject *iterable = NULL, *start = NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n >= 1) iterable = PyTuple_GET_ITEM(args, 0);
    if (n >= 2) start = PyTuple_GET_ITEM(args, 1);
    if (kwds) { PyObject *v; if ((v = PyDict_GetItemString(kwds, "iterable"))) iterable = v; if ((v = PyDict_GetItemString(kwds, "start"))) start = v; }
    if (!iterable) { PyErr_SetString(PyExc_TypeError, "enumerate() missing required argument 'iterable'"); return NULL; }
    if (n > 2) { PyErr_Format(PyExc_TypeError, "enumerate() takes at most 2 arguments (%zd given)", n); return NULL; }
    enumobject *e = (enumobject *)tp->tp_alloc(tp, 0);
    if (!e) return NULL;
    e->it = PyObject_GetIter(iterable);
    if (!e->it) { Py_DECREF(e); return NULL; }
    e->index = start ? PyNumber_Index(start) : PyLong_FromLong(0);
    if (!e->index) { Py_DECREF(e); return NULL; }
    return (PyObject *)e;
}
static PyObject *enum_next(PyObject *o) {
    enumobject *e = (enumobject *)o;
    PyObject *item = PyIter_Next(e->it);
    if (!item) return NULL;
    PyObject *t = PyTuple_Pack(2, e->index, item);
    Py_DECREF(item);
    PyObject *one = PyLong_FromLong(1);
    PyObject *ni = PyNumber_Add(e->index, one);
    Py_DECREF(one);
    Py_SETREF(e->index, ni);
    return t;
}
PyTypeObject PyEnum_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "enumerate", .tp_basicsize = sizeof(enumobject), .tp_dealloc = enum_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_iter = PyObject_SelfIter, .tp_iternext = enum_next, .tp_alloc = PyType_GenericAlloc, .tp_new = enum_new, .tp_free = PyObject_Free };

/* ---- zip ------------------------------------------------------------------------ */

typedef struct { PyObject_HEAD PyObject *iters; int strict; } zipobject;
static void zip_dealloc(PyObject *o) { Py_XDECREF(((zipobject *)o)->iters); PyObject_Free(o); }
static PyObject *zip_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    int strict = 0;
    if (kwds) {
        PyObject *s = PyDict_GetItemString(kwds, "strict");
        if (s) { strict = PyObject_IsTrue(s); if (strict < 0) return NULL; }
        if (PyDict_Size(kwds) > (s ? 1 : 0)) { PyErr_SetString(PyExc_TypeError, "zip() takes at most one keyword argument 'strict'"); return NULL; }
    }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    PyObject *iters = PyTuple_New(n);
    if (!iters) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *it = PyObject_GetIter(PyTuple_GET_ITEM(args, i));
        if (!it) { Py_DECREF(iters); return NULL; }
        PyTuple_SET_ITEM(iters, i, it);
    }
    zipobject *z = (zipobject *)tp->tp_alloc(tp, 0);
    if (!z) { Py_DECREF(iters); return NULL; }
    z->iters = iters;
    z->strict = strict;
    return (PyObject *)z;
}
static PyObject *zip_next(PyObject *o) {
    zipobject *z = (zipobject *)o;
    Py_ssize_t n = PyTuple_GET_SIZE(z->iters);
    if (n == 0) return NULL;
    PyObject *t = PyTuple_New(n);
    if (!t) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyIter_Next(PyTuple_GET_ITEM(z->iters, i));
        if (!item) {
            Py_DECREF(t);
            if (PyErr_Occurred()) return NULL;
            if (z->strict) {
                if (i > 0) { PyErr_Format(PyExc_ValueError, "zip() argument %zd is shorter than argument%s%zd", i + 1, i == 1 ? " " : "s 1-", i); return NULL; }
                for (Py_ssize_t j = 1; j < n; j++) {
                    PyObject *x = PyIter_Next(PyTuple_GET_ITEM(z->iters, j));
                    if (x) { Py_DECREF(x); PyErr_Format(PyExc_ValueError, "zip() argument %zd is longer than argument%s%zd", j + 1, j == 1 ? " " : "s 1-", j); return NULL; }
                    if (PyErr_Occurred()) return NULL;
                }
            }
            return NULL;
        }
        PyTuple_SET_ITEM(t, i, item);
    }
    return t;
}
PyTypeObject PyZip_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "zip", .tp_basicsize = sizeof(zipobject), .tp_dealloc = zip_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_iter = PyObject_SelfIter, .tp_iternext = zip_next, .tp_alloc = PyType_GenericAlloc, .tp_new = zip_new, .tp_free = PyObject_Free };

/* ---- map / filter ---------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *func; PyObject *iters; } mapobject;
static void map_dealloc(PyObject *o) { mapobject *m = (mapobject *)o; Py_XDECREF(m->func); Py_XDECREF(m->iters); PyObject_Free(o); }
static PyObject *map_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "map() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n < 2) { PyErr_SetString(PyExc_TypeError, "map() must have at least two arguments."); return NULL; }
    PyObject *iters = PyTuple_New(n - 1);
    for (Py_ssize_t i = 1; i < n; i++) { PyObject *it = PyObject_GetIter(PyTuple_GET_ITEM(args, i)); if (!it) { Py_DECREF(iters); return NULL; } PyTuple_SET_ITEM(iters, i - 1, it); }
    mapobject *m = (mapobject *)tp->tp_alloc(tp, 0);
    if (!m) { Py_DECREF(iters); return NULL; }
    m->func = Py_NewRef(PyTuple_GET_ITEM(args, 0));
    m->iters = iters;
    return (PyObject *)m;
}
static PyObject *map_next(PyObject *o) {
    mapobject *m = (mapobject *)o;
    Py_ssize_t n = PyTuple_GET_SIZE(m->iters);
    PyObject *stack[8];
    PyObject **argv = n <= 8 ? stack : PyMem_Malloc((size_t)n * sizeof(PyObject *));
    Py_ssize_t i;
    for (i = 0; i < n; i++) {
        argv[i] = PyIter_Next(PyTuple_GET_ITEM(m->iters, i));
        if (!argv[i]) break;
    }
    PyObject *r = NULL;
    if (i == n) r = PyObject_Vectorcall(m->func, argv, (size_t)n, NULL);
    for (Py_ssize_t j = 0; j < i; j++) Py_DECREF(argv[j]);
    if (argv != stack) PyMem_Free(argv);
    return r;
}
PyTypeObject PyMap_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "map", .tp_basicsize = sizeof(mapobject), .tp_dealloc = map_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_iter = PyObject_SelfIter, .tp_iternext = map_next, .tp_alloc = PyType_GenericAlloc, .tp_new = map_new, .tp_free = PyObject_Free };

typedef struct { PyObject_HEAD PyObject *func; PyObject *it; } filterobject;
static void filter_dealloc(PyObject *o) { filterobject *f = (filterobject *)o; Py_XDECREF(f->func); Py_XDECREF(f->it); PyObject_Free(o); }
static PyObject *filter_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "filter() takes no keyword arguments"); return NULL; }
    if (PyTuple_GET_SIZE(args) != 2) { PyErr_Format(PyExc_TypeError, "filter expected 2 arguments, got %zd", PyTuple_GET_SIZE(args)); return NULL; }
    PyObject *it = PyObject_GetIter(PyTuple_GET_ITEM(args, 1));
    if (!it) return NULL;
    filterobject *f = (filterobject *)tp->tp_alloc(tp, 0);
    if (!f) { Py_DECREF(it); return NULL; }
    f->func = Py_NewRef(PyTuple_GET_ITEM(args, 0));
    f->it = it;
    return (PyObject *)f;
}
static PyObject *filter_next(PyObject *o) {
    filterobject *f = (filterobject *)o;
    for (;;) {
        PyObject *item = PyIter_Next(f->it);
        if (!item) return NULL;
        int ok;
        if (f->func == Py_None || f->func == (PyObject *)&PyBool_Type) ok = PyObject_IsTrue(item);
        else { PyObject *r = PyObject_CallOneArg(f->func, item); if (!r) { Py_DECREF(item); return NULL; } ok = PyObject_IsTrue(r); Py_DECREF(r); }
        if (ok > 0) return item;
        Py_DECREF(item);
        if (ok < 0) return NULL;
    }
}
PyTypeObject PyFilter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "filter", .tp_basicsize = sizeof(filterobject), .tp_dealloc = filter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_iter = PyObject_SelfIter, .tp_iternext = filter_next, .tp_alloc = PyType_GenericAlloc, .tp_new = filter_new, .tp_free = PyObject_Free };

/* ---- reversed ----------------------------------------------------------------------- */

typedef struct { PyObject_HEAD Py_ssize_t index; PyObject *seq; } reversedobject;
static void reversed_dealloc(PyObject *o) { Py_XDECREF(((reversedobject *)o)->seq); PyObject_Free(o); }
static PyObject *reversed_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "reversed() takes no keyword arguments"); return NULL; }
    if (PyTuple_GET_SIZE(args) != 1) { PyErr_Format(PyExc_TypeError, "reversed expected 1 argument, got %zd", PyTuple_GET_SIZE(args)); return NULL; }
    PyObject *seq = PyTuple_GET_ITEM(args, 0);
    PyObject *rev = piper_type_lookup(Py_TYPE(seq), piper_intern("__reversed__"));
    if (rev) { if (rev == Py_None) { PyErr_Format(PyExc_TypeError, "'%s' object is not reversible", Py_TYPE(seq)->tp_name); return NULL; } return PyObject_CallOneArg(rev, seq); }
    if (!PySequence_Check(seq)) { PyErr_Format(PyExc_TypeError, "'%s' object is not reversible", Py_TYPE(seq)->tp_name); return NULL; }
    Py_ssize_t n = PySequence_Size(seq);
    if (n < 0) return NULL;
    reversedobject *r = (reversedobject *)tp->tp_alloc(tp, 0);
    if (!r) return NULL;
    r->index = n - 1;
    r->seq = Py_NewRef(seq);
    return (PyObject *)r;
}
static PyObject *reversed_next(PyObject *o) {
    reversedobject *r = (reversedobject *)o;
    if (r->index >= 0 && r->seq) {
        PyObject *item = PySequence_GetItem(r->seq, r->index);
        if (item) { r->index--; return item; }
        if (PyErr_ExceptionMatches(PyExc_IndexError) || PyErr_ExceptionMatches(PyExc_StopIteration)) PyErr_Clear();
    }
    r->index = -1;
    Py_CLEAR(r->seq);
    return NULL;
}
static PyObject *reversed_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); reversedobject *r = (reversedobject *)o; return PyLong_FromSsize_t(r->seq ? r->index + 1 : 0); }
static PyMethodDef reversed_methods[] = { { "__length_hint__", reversed_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyReversed_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "reversed", .tp_basicsize = sizeof(reversedobject), .tp_dealloc = reversed_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_iter = PyObject_SelfIter, .tp_iternext = reversed_next, .tp_methods = reversed_methods, .tp_alloc = PyType_GenericAlloc, .tp_new = reversed_new, .tp_free = PyObject_Free };
