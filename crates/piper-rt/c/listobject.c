/* list */
#include "internal.h"

static int list_resize(PyListObject *l, Py_ssize_t n) {
    if (n <= l->allocated && n >= l->allocated / 2) { Py_SET_SIZE(l, n); return 0; }
    Py_ssize_t alloc = n + (n >> 3) + (n < 9 ? 3 : 6);
    if (n == 0) alloc = 0;
    PyObject **items = PyMem_Realloc(l->ob_item, (size_t)(alloc ? alloc : 1) * sizeof(PyObject *));
    if (!items) { PyErr_NoMemory(); return -1; }
    l->ob_item = items;
    Py_SET_SIZE(l, n);
    l->allocated = alloc;
    return 0;
}

PyObject *PyList_New(Py_ssize_t n) {
    if (n < 0) { PyErr_BadInternalCall(); return NULL; }
    PyListObject *l = PyObject_Malloc(sizeof(PyListObject));
    if (!l) return PyErr_NoMemory();
    PyObject_InitVar((PyVarObject *)l, &PyList_Type, n);
    l->ob_item = PyMem_Calloc((size_t)(n ? n : 1), sizeof(PyObject *));
    l->allocated = n;
    return (PyObject *)l;
}

PyObject *PyList_FromArray(PyObject *const *items, Py_ssize_t n) {
    PyObject *l = PyList_New(n);
    if (!l) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) PyList_SET_ITEM(l, i, Py_NewRef(items[i]));
    return l;
}

static void list_dealloc(PyObject *o) {
    PyListObject *l = (PyListObject *)o;
    for (Py_ssize_t i = Py_SIZE(l); --i >= 0;) Py_XDECREF(l->ob_item[i]);
    PyMem_Free(l->ob_item);
    Py_TYPE(o)->tp_free(o);
}

Py_ssize_t PyList_Size(PyObject *l) { if (!PyList_Check(l)) { PyErr_BadInternalCall(); return -1; } return Py_SIZE(l); }
PyObject *PyList_GetItem(PyObject *l, Py_ssize_t i) {
    if (!PyList_Check(l)) { PyErr_BadInternalCall(); return NULL; }
    if (i < 0 || i >= Py_SIZE(l)) { PyErr_SetString(PyExc_IndexError, "list index out of range"); return NULL; }
    return PyList_GET_ITEM(l, i);
}
PyObject *PyList_GetItemRef(PyObject *l, Py_ssize_t i) { PyObject *r = PyList_GetItem(l, i); return Py_XNewRef(r); }
int PyList_SetItem(PyObject *l, Py_ssize_t i, PyObject *v) {
    if (!PyList_Check(l)) { Py_XDECREF(v); PyErr_BadInternalCall(); return -1; }
    if (i < 0 || i >= Py_SIZE(l)) { Py_XDECREF(v); PyErr_SetString(PyExc_IndexError, "list assignment index out of range"); return -1; }
    Py_XSETREF(PyList_GET_ITEM(l, i), v);
    return 0;
}
int PyList_Append(PyObject *o, PyObject *v) {
    if (!PyList_Check(o) || !v) { PyErr_BadInternalCall(); return -1; }
    PyListObject *l = (PyListObject *)o;
    Py_ssize_t n = Py_SIZE(l);
    if (n + 1 > l->allocated) { if (list_resize(l, n + 1) < 0) return -1; } else Py_SET_SIZE(l, n + 1);
    l->ob_item[n] = Py_NewRef(v);
    return 0;
}
int PyList_Insert(PyObject *o, Py_ssize_t where, PyObject *v) {
    PyListObject *l = (PyListObject *)o;
    Py_ssize_t n = Py_SIZE(l);
    if (list_resize(l, n + 1) < 0) return -1;
    if (where < 0) { where += n; if (where < 0) where = 0; }
    if (where > n) where = n;
    memmove(l->ob_item + where + 1, l->ob_item + where, (size_t)(n - where) * sizeof(PyObject *));
    l->ob_item[where] = Py_NewRef(v);
    return 0;
}
int PyList_Clear(PyObject *o) {
    PyListObject *l = (PyListObject *)o;
    PyObject **items = l->ob_item;
    Py_ssize_t n = Py_SIZE(l);
    l->ob_item = PyMem_Calloc(1, sizeof(PyObject *));
    Py_SET_SIZE(l, 0);
    l->allocated = 0;
    for (Py_ssize_t i = 0; i < n; i++) Py_XDECREF(items[i]);
    PyMem_Free(items);
    return 0;
}
PyObject *PyList_GetSlice(PyObject *o, Py_ssize_t a, Py_ssize_t b) {
    Py_ssize_t n = Py_SIZE(o);
    if (a < 0) a = 0;
    if (b > n) b = n;
    if (b < a) b = a;
    return PyList_FromArray(PyList_ITEMS(o) + a, b - a);
}

/* Replace l[a:b] with the items of v (or delete when v is NULL). */
int PyList_SetSlice(PyObject *o, Py_ssize_t a, Py_ssize_t b, PyObject *v) {
    PyListObject *l = (PyListObject *)o;
    Py_ssize_t n = Py_SIZE(l);
    if (a < 0) a = 0; if (a > n) a = n;
    if (b < a) b = a; if (b > n) b = n;
    PyObject *seq = NULL;
    Py_ssize_t m = 0;
    if (v) {
        seq = (PyObject *)l == v ? PyList_GetSlice(v, 0, Py_SIZE(v)) : PySequence_Fast(v, "can only assign an iterable");
        if (!seq) return -1;
        m = PySequence_Fast_GET_SIZE(seq);
    }
    Py_ssize_t d = m - (b - a);
    PyObject **old = PyMem_Malloc((size_t)(b - a + 1) * sizeof(PyObject *));
    memcpy(old, l->ob_item + a, (size_t)(b - a) * sizeof(PyObject *));
    if (d < 0) {
        memmove(l->ob_item + b + d, l->ob_item + b, (size_t)(n - b) * sizeof(PyObject *));
        list_resize(l, n + d);
    } else if (d > 0) {
        if (list_resize(l, n + d) < 0) { PyMem_Free(old); Py_XDECREF(seq); return -1; }
        memmove(l->ob_item + b + d, l->ob_item + b, (size_t)(n - b) * sizeof(PyObject *));
    }
    for (Py_ssize_t i = 0; i < m; i++) l->ob_item[a + i] = Py_NewRef(PySequence_Fast_GET_ITEM(seq, i));
    for (Py_ssize_t i = 0; i < b - a; i++) Py_XDECREF(old[i]);
    PyMem_Free(old);
    Py_XDECREF(seq);
    return 0;
}

int PyList_Extend(PyObject *o, PyObject *iterable) {
    PyListObject *l = (PyListObject *)o;
    if (PyList_CheckExact(iterable) || PyTuple_CheckExact(iterable)) {
        Py_ssize_t n = Py_SIZE(l), m = Py_SIZE(iterable);
        if (m == 0) return 0;
        PyObject **src = PySequence_Fast_ITEMS(iterable);
        if (list_resize(l, n + m) < 0) return -1;
        for (Py_ssize_t i = 0; i < m; i++) l->ob_item[n + i] = Py_NewRef(src[i]);
        return 0;
    }
    PyObject *it = PyObject_GetIter(iterable);
    if (!it) return -1;
    for (;;) {
        PyObject *x = PyIter_Next(it);
        if (!x) break;
        int r = PyList_Append(o, x);
        Py_DECREF(x);
        if (r < 0) { Py_DECREF(it); return -1; }
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
PyObject *piper_list_extend(PyObject *list, PyObject *iterable) {
    if (PyList_Extend(list, iterable) < 0) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) && !Py_TYPE(iterable)->tp_iter && !PySequence_Check(iterable)) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError, "Value after * must be an iterable, not %s", Py_TYPE(iterable)->tp_name);
        }
        return NULL;
    }
    Py_RETURN_NONE;
}
PyObject *piper_list_to_tuple(PyObject *list) { return PyList_AsTuple(list); }

PyObject *piper_list_from_iterable(PyObject *it) {
    PyObject *l = PyList_New(0);
    if (!l) return NULL;
    for (;;) {
        PyObject *x = PyIter_Next(it);
        if (!x) { if (PyErr_Occurred()) { Py_DECREF(l); return NULL; } break; }
        int r = PyList_Append(l, x);
        Py_DECREF(x);
        if (r < 0) { Py_DECREF(l); return NULL; }
    }
    return l;
}

PyObject *PyList_AsTuple(PyObject *l) { return PyTuple_FromArray(PyList_ITEMS(l), Py_SIZE(l)); }

int PyList_Reverse(PyObject *o) {
    PyObject **items = PyList_ITEMS(o);
    for (Py_ssize_t i = 0, j = Py_SIZE(o) - 1; i < j; i++, j--) { PyObject *t = items[i]; items[i] = items[j]; items[j] = t; }
    return 0;
}

/* ---- sort: stable merge sort with optional key ------------------------------- */

static int lt(PyObject *a, PyObject *b, int *err) {
    int r = PyObject_RichCompareBool(a, b, Py_LT);
    if (r < 0) { *err = 1; return 0; }
    return r;
}

static void merge_sort(PyObject **items, PyObject **keys, PyObject **tmp, PyObject **tmpk, Py_ssize_t n, int *err) {
    if (n < 2 || *err) return;
    if (n <= 12) {
        for (Py_ssize_t i = 1; i < n && !*err; i++) {
            PyObject *v = items[i], *k = keys ? keys[i] : NULL;
            Py_ssize_t j = i;
            while (j > 0 && lt(keys ? k : v, keys ? keys[j - 1] : items[j - 1], err)) { items[j] = items[j - 1]; if (keys) keys[j] = keys[j - 1]; j--; }
            items[j] = v;
            if (keys) keys[j] = k;
        }
        return;
    }
    Py_ssize_t mid = n / 2;
    merge_sort(items, keys, tmp, tmpk, mid, err);
    merge_sort(items + mid, keys ? keys + mid : NULL, tmp, tmpk, n - mid, err);
    if (*err) return;
    memcpy(tmp, items, (size_t)mid * sizeof(PyObject *));
    if (keys) memcpy(tmpk, keys, (size_t)mid * sizeof(PyObject *));
    Py_ssize_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (lt(keys ? keys[j] : items[j], keys ? tmpk[i] : tmp[i], err)) { items[k] = items[j]; if (keys) keys[k] = keys[j]; j++; }
        else { items[k] = tmp[i]; if (keys) keys[k] = tmpk[i]; i++; }
        k++;
        if (*err) { /* restore remaining to keep every item present */ while (i < mid) { items[k] = tmp[i]; if (keys) keys[k] = tmpk[i]; i++; k++; } return; }
    }
    while (i < mid) { items[k] = tmp[i]; if (keys) keys[k] = tmpk[i]; i++; k++; }
}

int piper_list_sort_impl(PyObject *o, PyObject *key, int reverse) {
    PyListObject *l = (PyListObject *)o;
    Py_ssize_t n = Py_SIZE(l);
    PyObject **items = l->ob_item;
    /* detach during sort so mutation is detected */
    l->ob_item = PyMem_Calloc(1, sizeof(PyObject *));
    Py_SET_SIZE(l, 0);
    Py_ssize_t saved_alloc = l->allocated;
    l->allocated = -1;
    PyObject **keys = NULL;
    int err = 0;
    if (key && key != Py_None) {
        keys = PyMem_Malloc((size_t)(n ? n : 1) * sizeof(PyObject *));
        for (Py_ssize_t i = 0; i < n; i++) {
            keys[i] = PyObject_CallOneArg(key, items[i]);
            if (!keys[i]) { for (Py_ssize_t j = 0; j < i; j++) Py_DECREF(keys[j]); PyMem_Free(keys); keys = NULL; err = 1; break; }
        }
    }
    if (!err) {
        if (reverse) { PyList_Reverse((PyObject *)&(PyListObject){ .ob_base = { .ob_size = n }, .ob_item = items }); if (keys) for (Py_ssize_t i = 0, j = n - 1; i < j; i++, j--) { PyObject *t = keys[i]; keys[i] = keys[j]; keys[j] = t; } }
        PyObject **tmp = PyMem_Malloc((size_t)(n / 2 + 1) * sizeof(PyObject *));
        PyObject **tmpk = keys ? PyMem_Malloc((size_t)(n / 2 + 1) * sizeof(PyObject *)) : NULL;
        merge_sort(items, keys, tmp, tmpk, n, &err);
        PyMem_Free(tmp); PyMem_Free(tmpk);
        if (reverse) { PyListObject fake = { .ob_base = { .ob_size = n }, .ob_item = items }; PyList_Reverse((PyObject *)&fake); }
    }
    if (keys) { for (Py_ssize_t i = 0; i < n; i++) Py_DECREF(keys[i]); PyMem_Free(keys); }
    int mutated = Py_SIZE(l) != 0 || l->allocated != -1;
    PyObject **leftover = l->ob_item;
    Py_ssize_t leftover_n = Py_SIZE(l);
    l->ob_item = items;
    Py_SET_SIZE(l, n);
    l->allocated = saved_alloc;
    for (Py_ssize_t i = 0; i < leftover_n; i++) Py_XDECREF(leftover[i]);
    PyMem_Free(leftover);
    if (mutated && !err) { PyErr_SetString(PyExc_ValueError, "list modified during sort"); return -1; }
    return err ? -1 : 0;
}
int PyList_Sort(PyObject *l) { return piper_list_sort_impl(l, NULL, 0); }

/* ---- slots ------------------------------------------------------------------- */

static PyObject *list_repr(PyObject *o) {
    if (Py_SIZE(o) == 0) return PyUnicode_FromString("[]");
    int rc = Py_ReprEnter(o);
    if (rc != 0) return rc > 0 ? PyUnicode_FromString("[...]") : NULL;
    PyObject *r = piper_repr_join("[", PyList_ITEMS(o), Py_SIZE(o), "]", 0);
    Py_ReprLeave(o);
    return r;
}
static Py_ssize_t list_length(PyObject *o) { return Py_SIZE(o); }
static PyObject *list_item(PyObject *o, Py_ssize_t i) {
    if (i < 0) i += Py_SIZE(o);
    if (i < 0 || i >= Py_SIZE(o)) { PyErr_SetString(PyExc_IndexError, "list index out of range"); return NULL; }
    return Py_NewRef(PyList_GET_ITEM(o, i));
}
static int list_ass_item(PyObject *o, Py_ssize_t i, PyObject *v) {
    if (i < 0) i += Py_SIZE(o);
    if (i < 0 || i >= Py_SIZE(o)) { PyErr_SetString(PyExc_IndexError, "list assignment index out of range"); return -1; }
    if (!v) return PyList_SetSlice(o, i, i + 1, NULL);
    Py_SETREF(PyList_GET_ITEM(o, i), Py_NewRef(v));
    return 0;
}
static PyObject *list_subscript(PyObject *o, PyObject *item) {
    if (PyIndex_Check(item)) { Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError); if (i == -1 && PyErr_Occurred()) return NULL; return list_item(o, i); }
    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(item, Py_SIZE(o), &start, &stop, &step, &len) < 0) return NULL;
        if (step == 1) return PyList_GetSlice(o, start, stop);
        PyObject *r = PyList_New(len);
        for (Py_ssize_t i = 0, cur = start; i < len; i++, cur += step) PyList_SET_ITEM(r, i, Py_NewRef(PyList_GET_ITEM(o, cur)));
        return r;
    }
    PyErr_Format(PyExc_TypeError, "list indices must be integers or slices, not %s", Py_TYPE(item)->tp_name);
    return NULL;
}
static int list_ass_subscript(PyObject *o, PyObject *item, PyObject *v) {
    if (PyIndex_Check(item)) { Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError); if (i == -1 && PyErr_Occurred()) return -1; return list_ass_item(o, i, v); }
    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(item, Py_SIZE(o), &start, &stop, &step, &len) < 0) return -1;
        if (step == 1) return PyList_SetSlice(o, start, stop, v);
        if (!v) {
            /* delete extended slice: remove from the end */
            if (len == 0) return 0;
            if (step < 0) { stop = start + 1; start = stop + step * (len - 1) - 1; step = -step; }
            PyObject **items = PyList_ITEMS(o);
            Py_ssize_t n = Py_SIZE(o);
            PyObject **garbage = PyMem_Malloc((size_t)len * sizeof(PyObject *));
            Py_ssize_t g = 0, w = start;
            for (Py_ssize_t cur = start, lim; cur < n; cur = lim + 1) {
                garbage[g++] = items[cur];
                lim = cur + step - 1;
                if (lim >= n) lim = n - 1;
                if (g == len) lim = n - 1;
                for (Py_ssize_t k = cur + 1; k <= lim; k++) items[w++] = items[k];
                if (g == len) break;
            }
            Py_SET_SIZE(o, w);
            for (Py_ssize_t i = 0; i < g; i++) Py_DECREF(garbage[i]);
            PyMem_Free(garbage);
            return 0;
        }
        PyObject *seq = PySequence_Fast(v, "must assign iterable to extended slice");
        if (!seq) return -1;
        if (PySequence_Fast_GET_SIZE(seq) != len) { PyErr_Format(PyExc_ValueError, "attempt to assign sequence of size %zd to extended slice of size %zd", PySequence_Fast_GET_SIZE(seq), len); Py_DECREF(seq); return -1; }
        for (Py_ssize_t i = 0, cur = start; i < len; i++, cur += step) Py_SETREF(PyList_GET_ITEM(o, cur), Py_NewRef(PySequence_Fast_GET_ITEM(seq, i)));
        Py_DECREF(seq);
        return 0;
    }
    PyErr_Format(PyExc_TypeError, "list indices must be integers or slices, not %s", Py_TYPE(item)->tp_name);
    return -1;
}
static PyObject *list_concat(PyObject *a, PyObject *b) {
    if (!PyList_Check(b)) { PyErr_Format(PyExc_TypeError, "can only concatenate list (not \"%s\") to list", Py_TYPE(b)->tp_name); return NULL; }
    PyObject *r = PyList_New(Py_SIZE(a) + Py_SIZE(b));
    if (!r) return NULL;
    for (Py_ssize_t i = 0; i < Py_SIZE(a); i++) PyList_SET_ITEM(r, i, Py_NewRef(PyList_GET_ITEM(a, i)));
    for (Py_ssize_t i = 0; i < Py_SIZE(b); i++) PyList_SET_ITEM(r, Py_SIZE(a) + i, Py_NewRef(PyList_GET_ITEM(b, i)));
    return r;
}
static PyObject *list_inplace_concat(PyObject *a, PyObject *b) { if (PyList_Extend(a, b) < 0) return NULL; return Py_NewRef(a); }
static PyObject *list_repeat(PyObject *a, Py_ssize_t n) {
    if (n < 0) n = 0;
    Py_ssize_t len = Py_SIZE(a);
    if (len && n > PY_SSIZE_T_MAX / len) return PyErr_NoMemory();
    PyObject *r = PyList_New(len * n);
    if (!r) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) for (Py_ssize_t j = 0; j < len; j++) PyList_SET_ITEM(r, i * len + j, Py_NewRef(PyList_GET_ITEM(a, j)));
    return r;
}
static PyObject *list_inplace_repeat(PyObject *a, Py_ssize_t n) {
    Py_ssize_t len = Py_SIZE(a);
    if (n <= 0) { PyList_Clear(a); return Py_NewRef(a); }
    if (list_resize((PyListObject *)a, len * n) < 0) return NULL;
    for (Py_ssize_t i = 1; i < n; i++) for (Py_ssize_t j = 0; j < len; j++) PyList_SET_ITEM(a, i * len + j, Py_NewRef(PyList_GET_ITEM(a, j)));
    return Py_NewRef(a);
}
static int list_contains(PyObject *o, PyObject *v) {
    for (Py_ssize_t i = 0; i < Py_SIZE(o); i++) { int r = PyObject_RichCompareBool(PyList_GET_ITEM(o, i), v, Py_EQ); if (r) return r; }
    return 0;
}
static PyObject *list_richcompare(PyObject *v, PyObject *w, int op) {
    if (!PyList_Check(v) || !PyList_Check(w)) Py_RETURN_NOTIMPLEMENTED;
    Py_ssize_t nv = Py_SIZE(v), nw = Py_SIZE(w), i;
    if (nv != nw && (op == Py_EQ || op == Py_NE)) Py_RETURN_BOOL(op == Py_NE);
    for (i = 0; i < nv && i < nw; i++) {
        int k = PyObject_RichCompareBool(PyList_GET_ITEM(v, i), PyList_GET_ITEM(w, i), Py_EQ);
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
    return PyObject_RichCompare(PyList_GET_ITEM(v, i), PyList_GET_ITEM(w, i), op);
}

/* ---- methods ------------------------------------------------------------------ */

static PyObject *list_append_m(PyObject *o, PyObject *v) { if (PyList_Append(o, v) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *list_extend_m(PyObject *o, PyObject *v) { if (PyList_Extend(o, v) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *list_insert_m(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("insert", n, 2, 2) < 0) return NULL;
    Py_ssize_t i = PyNumber_AsSsize_t(a[0], PyExc_OverflowError);
    if (i == -1 && PyErr_Occurred()) return NULL;
    if (PyList_Insert(o, i, a[1]) < 0) return NULL;
    Py_RETURN_NONE;
}
static PyObject *list_pop_m(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("pop", n, 0, 1) < 0) return NULL;
    Py_ssize_t i = -1;
    if (n == 1) { i = PyNumber_AsSsize_t(a[0], PyExc_OverflowError); if (i == -1 && PyErr_Occurred()) return NULL; }
    if (Py_SIZE(o) == 0) { PyErr_SetString(PyExc_IndexError, "pop from empty list"); return NULL; }
    if (i < 0) i += Py_SIZE(o);
    if (i < 0 || i >= Py_SIZE(o)) { PyErr_SetString(PyExc_IndexError, "pop index out of range"); return NULL; }
    PyObject *v = Py_NewRef(PyList_GET_ITEM(o, i));
    if (PyList_SetSlice(o, i, i + 1, NULL) < 0) { Py_DECREF(v); return NULL; }
    return v;
}
static PyObject *list_remove_m(PyObject *o, PyObject *v) {
    for (Py_ssize_t i = 0; i < Py_SIZE(o); i++) {
        int r = PyObject_RichCompareBool(PyList_GET_ITEM(o, i), v, Py_EQ);
        if (r < 0) return NULL;
        if (r) { if (PyList_SetSlice(o, i, i + 1, NULL) < 0) return NULL; Py_RETURN_NONE; }
    }
    PyErr_SetString(PyExc_ValueError, "list.remove(x): x not in list");
    return NULL;
}
static PyObject *list_index_m(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("index", n, 1, 3) < 0) return NULL;
    Py_ssize_t start = 0, stop = Py_SIZE(o);
    if (n >= 2) { start = PyNumber_AsSsize_t(a[1], NULL); if (start == -1 && PyErr_Occurred()) return NULL; if (start < 0) { start += Py_SIZE(o); if (start < 0) start = 0; } }
    if (n >= 3) { stop = PyNumber_AsSsize_t(a[2], NULL); if (stop == -1 && PyErr_Occurred()) return NULL; if (stop < 0) stop += Py_SIZE(o); if (stop > Py_SIZE(o)) stop = Py_SIZE(o); }
    for (Py_ssize_t i = start; i < stop && i < Py_SIZE(o); i++) { int r = PyObject_RichCompareBool(PyList_GET_ITEM(o, i), a[0], Py_EQ); if (r < 0) return NULL; if (r) return PyLong_FromSsize_t(i); }
    PyObject *rp = PyObject_Repr(a[0]);
    PyErr_Format(PyExc_ValueError, "%U is not in list", rp);
    Py_XDECREF(rp);
    return NULL;
}
static PyObject *list_count_m(PyObject *o, PyObject *v) {
    Py_ssize_t c = 0;
    for (Py_ssize_t i = 0; i < Py_SIZE(o); i++) { int r = PyObject_RichCompareBool(PyList_GET_ITEM(o, i), v, Py_EQ); if (r < 0) return NULL; c += r; }
    return PyLong_FromSsize_t(c);
}
static PyObject *list_reverse_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyList_Reverse(o); Py_RETURN_NONE; }
static PyObject *list_copy_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyList_GetSlice(o, 0, Py_SIZE(o)); }
static PyObject *list_clear_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyList_Clear(o); Py_RETURN_NONE; }
static PyObject *list_sort_m(PyObject *o, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    if (n) { PyErr_SetString(PyExc_TypeError, "sort() takes no positional arguments"); return NULL; }
    PyObject *key = NULL;
    int reverse = 0;
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kw, i);
        if (PyUnicode_EqualToUTF8(k, "key")) key = a[i];
        else if (PyUnicode_EqualToUTF8(k, "reverse")) { reverse = PyObject_IsTrue(a[i]); if (reverse < 0) return NULL; }
        else { PyErr_Format(PyExc_TypeError, "sort() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    if (piper_list_sort_impl(o, key, reverse) < 0) return NULL;
    Py_RETURN_NONE;
}
static PyObject *list_sizeof(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyLong_FromSsize_t((Py_ssize_t)sizeof(PyListObject) + ((PyListObject *)o)->allocated * (Py_ssize_t)sizeof(PyObject *)); }
static PyObject *list_class_getitem(PyObject *cls, PyObject *arg) { extern PyObject *piper_generic_alias_new(PyObject *origin, PyObject *args); return piper_generic_alias_new(cls, arg); }

typedef struct { PyObject_HEAD PyObject *seq; Py_ssize_t index; } listiterobject;
static void listiter_dealloc(PyObject *o) { Py_XDECREF(((listiterobject *)o)->seq); PyObject_Free(o); }
static PyObject *listiter_next(PyObject *o) {
    listiterobject *it = (listiterobject *)o;
    if (!it->seq) return NULL;
    if (it->index < Py_SIZE(it->seq)) return Py_NewRef(PyList_GET_ITEM(it->seq, it->index++));
    Py_CLEAR(it->seq);
    return NULL;
}
static PyObject *listiter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); listiterobject *it = (listiterobject *)o; Py_ssize_t n = it->seq ? Py_SIZE(it->seq) - it->index : 0; return PyLong_FromSsize_t(n > 0 ? n : 0); }
static PyMethodDef listiter_methods[] = { { "__length_hint__", listiter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyListIter_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "list_iterator", .tp_basicsize = sizeof(listiterobject), .tp_dealloc = listiter_dealloc,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = listiter_next, .tp_methods = listiter_methods,
};
static PyObject *list_iter(PyObject *o) {
    listiterobject *it = PyObject_New(listiterobject, &PyListIter_Type);
    if (!it) return NULL;
    it->seq = Py_NewRef(o);
    it->index = 0;
    return (PyObject *)it;
}
typedef struct { PyObject_HEAD PyObject *seq; Py_ssize_t index; } listreviterobject;
static PyObject *listreviter_next(PyObject *o) {
    listreviterobject *it = (listreviterobject *)o;
    if (!it->seq) return NULL;
    if (it->index >= 0 && it->index < Py_SIZE(it->seq)) return Py_NewRef(PyList_GET_ITEM(it->seq, it->index--));
    Py_CLEAR(it->seq);
    return NULL;
}
static PyObject *listreviter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); listreviterobject *it = (listreviterobject *)o; return PyLong_FromSsize_t(it->seq ? it->index + 1 : 0); }
static PyMethodDef listreviter_methods[] = { { "__length_hint__", listreviter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyTypeObject PyListRevIter_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "list_reverseiterator", .tp_basicsize = sizeof(listreviterobject), .tp_dealloc = listiter_dealloc,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = listreviter_next, .tp_methods = listreviter_methods,
};
static PyObject *list_reversed(PyObject *o, PyObject *u) {
    PIPER_UNUSED(u);
    listreviterobject *it = PyObject_New(listreviterobject, &PyListRevIter_Type);
    if (!it) return NULL;
    it->seq = Py_NewRef(o);
    it->index = Py_SIZE(o) - 1;
    return (PyObject *)it;
}

static PyMethodDef list_methods[] = {
    { "append", list_append_m, METH_O, NULL }, { "extend", list_extend_m, METH_O, NULL },
    { "insert", (PyCFunction)(void (*)(void))list_insert_m, METH_FASTCALL, NULL },
    { "pop", (PyCFunction)(void (*)(void))list_pop_m, METH_FASTCALL, NULL },
    { "remove", list_remove_m, METH_O, NULL },
    { "index", (PyCFunction)(void (*)(void))list_index_m, METH_FASTCALL, NULL },
    { "count", list_count_m, METH_O, NULL }, { "reverse", list_reverse_m, METH_NOARGS, NULL },
    { "copy", list_copy_m, METH_NOARGS, NULL }, { "clear", list_clear_m, METH_NOARGS, NULL },
    { "sort", (PyCFunction)(void (*)(void))list_sort_m, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "__reversed__", list_reversed, METH_NOARGS, NULL }, { "__sizeof__", list_sizeof, METH_NOARGS, NULL },
    { "__class_getitem__", list_class_getitem, METH_O | METH_CLASS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyObject *list_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(args); PIPER_UNUSED(kwds);
    PyObject *l = tp->tp_alloc(tp, 0);
    if (!l) return NULL;
    ((PyListObject *)l)->ob_item = PyMem_Calloc(1, sizeof(PyObject *));
    ((PyListObject *)l)->allocated = 0;
    Py_SET_SIZE(l, 0);
    return l;
}
static int list_init(PyObject *o, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "list() takes no keyword arguments"); return -1; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "list expected at most 1 argument, got %zd", n); return -1; }
    if (Py_SIZE(o)) PyList_Clear(o);
    if (n == 1) return PyList_Extend(o, PyTuple_GET_ITEM(args, 0));
    return 0;
}

static PySequenceMethods list_as_sequence = { .sq_length = list_length, .sq_concat = list_concat, .sq_repeat = list_repeat, .sq_item = list_item, .sq_ass_item = list_ass_item, .sq_contains = list_contains, .sq_inplace_concat = list_inplace_concat, .sq_inplace_repeat = list_inplace_repeat };
static PyMappingMethods list_as_mapping = { .mp_length = list_length, .mp_subscript = list_subscript, .mp_ass_subscript = list_ass_subscript };

PyTypeObject PyList_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "list",
    .tp_basicsize = sizeof(PyListObject),
    .tp_dealloc = list_dealloc,
    .tp_repr = list_repr,
    .tp_as_sequence = &list_as_sequence,
    .tp_as_mapping = &list_as_mapping,
    .tp_hash = PyObject_HashNotImplemented,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_LIST_SUBCLASS | Py_TPFLAGS_SEQUENCE | Py_TPFLAGS_MATCH_SELF,
    .tp_richcompare = list_richcompare,
    .tp_iter = list_iter,
    .tp_methods = list_methods,
    .tp_init = list_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = list_new,
    .tp_free = PyObject_Free,
};
