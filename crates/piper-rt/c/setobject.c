/* set and frozenset: open addressing with insertion-ordered entries. */
#include "internal.h"

typedef struct { Py_hash_t hash; PyObject *key; } SetEntry;
typedef struct {
    PyObject_HEAD
    Py_ssize_t used, nentries, size;
    Py_ssize_t *indices;
    SetEntry *entries;
    Py_hash_t hash;
} PySetObject;

#define S(o) ((PySetObject *)(o))
#define EMPTY (-1)
#define DUMMY (-2)
#define MIN_SIZE 8
#define USABLE(size) (((size) << 1) / 3)

static int set_alloc(PySetObject *s, Py_ssize_t size) {
    s->indices = PyMem_Malloc((size_t)size * sizeof(Py_ssize_t));
    s->entries = PyMem_Calloc((size_t)USABLE(size), sizeof(SetEntry));
    if (!s->indices || !s->entries) { PyErr_NoMemory(); return -1; }
    for (Py_ssize_t i = 0; i < size; i++) s->indices[i] = EMPTY;
    s->size = size; s->used = 0; s->nentries = 0;
    return 0;
}

static PyObject *make_set(PyTypeObject *tp) {
    PySetObject *s = (PySetObject *)tp->tp_alloc(tp, 0);
    if (!s) return NULL;
    s->hash = -1;
    if (set_alloc(s, MIN_SIZE) < 0) { Py_DECREF(s); return NULL; }
    return (PyObject *)s;
}

static void set_dealloc(PyObject *o) {
    PySetObject *s = S(o);
    for (Py_ssize_t i = 0; i < s->nentries; i++) Py_XDECREF(s->entries[i].key);
    PyMem_Free(s->indices); PyMem_Free(s->entries);
    Py_TYPE(o)->tp_free(o);
}

static Py_ssize_t set_lookup(PySetObject *s, PyObject *key, Py_hash_t hash, Py_ssize_t *slot) {
    size_t mask = (size_t)s->size - 1, i = (size_t)hash & mask, perturb = (size_t)hash;
    Py_ssize_t freeslot = -1;
    for (;;) {
        Py_ssize_t ix = s->indices[i];
        if (ix == EMPTY) { if (slot) *slot = freeslot >= 0 ? freeslot : (Py_ssize_t)i; return -1; }
        if (ix == DUMMY) { if (freeslot < 0) freeslot = (Py_ssize_t)i; }
        else if (s->entries[ix].key == key) return ix;
        else if (s->entries[ix].hash == hash) {
            PyObject *k = s->entries[ix].key;
            Py_INCREF(k);
            int c = PyObject_RichCompareBool(k, key, Py_EQ);
            Py_DECREF(k);
            if (c < 0) return -3;
            if (c) return ix;
        }
        perturb >>= 5;
        i = (i * 5 + perturb + 1) & mask;
    }
}

static int set_resize(PySetObject *s, Py_ssize_t minsize) {
    Py_ssize_t newsize = MIN_SIZE;
    while (newsize <= minsize) newsize <<= 1;
    SetEntry *old = s->entries;
    Py_ssize_t oldn = s->nentries;
    PyMem_Free(s->indices);
    if (set_alloc(s, newsize) < 0) return -1;
    for (Py_ssize_t k = 0; k < oldn; k++) {
        if (!old[k].key) continue;
        size_t mask = (size_t)newsize - 1, i = (size_t)old[k].hash & mask, perturb = (size_t)old[k].hash;
        while (s->indices[i] != EMPTY) { perturb >>= 5; i = (i * 5 + perturb + 1) & mask; }
        s->indices[i] = s->nentries;
        s->entries[s->nentries++] = old[k];
        s->used++;
    }
    PyMem_Free(old);
    return 0;
}

static int set_add_hash(PySetObject *s, PyObject *key, Py_hash_t hash) {
    Py_ssize_t slot, ix = set_lookup(s, key, hash, &slot);
    if (ix == -3) return -1;
    if (ix >= 0) return 0;
    if (s->nentries >= USABLE(s->size)) { if (set_resize(s, s->used * 3) < 0) return -1; ix = set_lookup(s, key, hash, &slot); if (ix == -3) return -1; }
    s->indices[slot] = s->nentries;
    s->entries[s->nentries].hash = hash;
    s->entries[s->nentries].key = Py_NewRef(key);
    s->nentries++;
    s->used++;
    return 0;
}

int PySet_Add(PyObject *o, PyObject *key) {
    Py_hash_t h = PyObject_Hash(key);
    if (h == -1) return -1;
    return set_add_hash(S(o), key, h);
}
int piper_set_add(PyObject *set, PyObject *v) { return PySet_Add(set, v); }

int PySet_Contains(PyObject *o, PyObject *key) {
    Py_hash_t h = PyObject_Hash(key);
    if (h == -1) {
        if (PySet_Check(key) && PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_Clear();
            PyObject *f = PyFrozenSet_New(key);
            if (!f) return -1;
            int r = PySet_Contains(o, f);
            Py_DECREF(f);
            return r;
        }
        return -1;
    }
    Py_ssize_t ix = set_lookup(S(o), key, h, NULL);
    return ix == -3 ? -1 : ix >= 0;
}

int PySet_Discard(PyObject *o, PyObject *key) {
    Py_hash_t h = PyObject_Hash(key);
    if (h == -1) return -1;
    PySetObject *s = S(o);
    Py_ssize_t ix = set_lookup(s, key, h, NULL);
    if (ix == -3) return -1;
    if (ix < 0) return 0;
    size_t mask = (size_t)s->size - 1, i = (size_t)h & mask, perturb = (size_t)h;
    while (s->indices[i] != ix) { perturb >>= 5; i = (i * 5 + perturb + 1) & mask; }
    s->indices[i] = DUMMY;
    PyObject *k = s->entries[ix].key;
    s->entries[ix].key = NULL;
    s->used--;
    Py_DECREF(k);
    return 1;
}

Py_ssize_t PySet_Size(PyObject *o) { return S(o)->used; }
int PySet_Clear(PyObject *o) {
    PySetObject *s = S(o);
    SetEntry *old = s->entries; Py_ssize_t n = s->nentries;
    PyMem_Free(s->indices);
    set_alloc(s, MIN_SIZE);
    for (Py_ssize_t i = 0; i < n; i++) Py_XDECREF(old[i].key);
    PyMem_Free(old);
    return 0;
}
int _PySet_NextEntry(PyObject *o, Py_ssize_t *pos, PyObject **k, Py_hash_t *hash) {
    PySetObject *s = S(o);
    Py_ssize_t i = *pos;
    while (i < s->nentries && !s->entries[i].key) i++;
    if (i >= s->nentries) return 0;
    *k = s->entries[i].key;
    if (hash) *hash = s->entries[i].hash;
    *pos = i + 1;
    return 1;
}
PyObject *PySet_Pop(PyObject *o) {
    PySetObject *s = S(o);
    if (s->used == 0) { PyErr_SetString(PyExc_KeyError, "pop from an empty set"); return NULL; }
    Py_ssize_t i = 0;
    while (!s->entries[i].key) i++;
    PyObject *k = Py_NewRef(s->entries[i].key);
    PySet_Discard(o, k);
    return k;
}

static int set_update_internal(PyObject *o, PyObject *iterable) {
    if (PyAnySet_Check(iterable)) {
        Py_ssize_t pos = 0; PyObject *k; Py_hash_t h;
        while (_PySet_NextEntry(iterable, &pos, &k, &h)) if (set_add_hash(S(o), k, h) < 0) return -1;
        return 0;
    }
    if (PyDict_CheckExact(iterable)) {
        Py_ssize_t pos = 0; PyObject *k; Py_hash_t h;
        while (piper_dict_next(iterable, &pos, &k, NULL, &h)) if (set_add_hash(S(o), k, h) < 0) return -1;
        return 0;
    }
    PyObject *it = PyObject_GetIter(iterable);
    if (!it) return -1;
    for (;;) {
        PyObject *k = PyIter_Next(it);
        if (!k) break;
        int r = PySet_Add(o, k);
        Py_DECREF(k);
        if (r < 0) { Py_DECREF(it); return -1; }
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
int piper_set_update(PyObject *set, PyObject *iterable) { return set_update_internal(set, iterable); }

PyObject *PySet_New(PyObject *iterable) {
    PyObject *s = make_set(&PySet_Type);
    if (!s) return NULL;
    if (iterable && set_update_internal(s, iterable) < 0) { Py_DECREF(s); return NULL; }
    return s;
}
PyObject *PyFrozenSet_New(PyObject *iterable) {
    PyObject *s = make_set(&PyFrozenSet_Type);
    if (!s) return NULL;
    if (iterable && set_update_internal(s, iterable) < 0) { Py_DECREF(s); return NULL; }
    return s;
}

static PyObject *set_copy_as(PyObject *o, PyTypeObject *tp) {
    PyObject *r = make_set(tp);
    if (!r) return NULL;
    if (set_update_internal(r, o) < 0) { Py_DECREF(r); return NULL; }
    return r;
}
static PyTypeObject *result_type(PyObject *o) { return PyFrozenSet_Check(o) ? &PyFrozenSet_Type : &PySet_Type; }

/* ---- slots ------------------------------------------------------------- */

static PyObject *set_repr(PyObject *o) {
    PySetObject *s = S(o);
    const char *name = Py_TYPE(o)->tp_name;
    if (s->used == 0) return PyUnicode_FromFormat("%s()", name);
    int rc = Py_ReprEnter(o);
    if (rc != 0) return rc > 0 ? PyUnicode_FromFormat("%s(...)", name) : NULL;
    PyObject *keys = PyList_New(0);
    Py_ssize_t pos = 0; PyObject *k;
    while (_PySet_NextEntry(o, &pos, &k, NULL)) PyList_Append(keys, k);
    PyObject *inner = piper_repr_join("{", PyList_ITEMS(keys), PyList_GET_SIZE(keys), "}", 0);
    Py_DECREF(keys);
    Py_ReprLeave(o);
    if (!inner) return NULL;
    if (Py_IS_TYPE(o, &PySet_Type)) return inner;
    PyObject *r = PyUnicode_FromFormat("%s(%U)", name, inner);
    Py_DECREF(inner);
    return r;
}
static Py_ssize_t set_len(PyObject *o) { return S(o)->used; }
static int set_contains(PyObject *o, PyObject *k) { return PySet_Contains(o, k); }

static Py_hash_t frozenset_hash(PyObject *o) {
    PySetObject *s = S(o);
    if (s->hash != -1) return s->hash;
    Py_uhash_t hash = 0;
    Py_ssize_t pos = 0; PyObject *k; Py_hash_t h;
    while (_PySet_NextEntry(o, &pos, &k, &h)) {
        Py_uhash_t x = (Py_uhash_t)h;
        hash ^= ((x ^ 89869747UL) ^ (x << 16)) * 3644798167UL;
    }
    hash ^= ((Py_uhash_t)s->used + 1) * 1927868237UL;
    hash ^= (hash >> 11) ^ (hash >> 25);
    hash = hash * 69069U + 907133923UL;
    if (hash == (Py_uhash_t)-1) hash = 590923713UL;
    s->hash = (Py_hash_t)hash;
    return s->hash;
}

static int issubset(PyObject *a, PyObject *b) {
    if (S(a)->used > S(b)->used) return 0;
    Py_ssize_t pos = 0; PyObject *k; Py_hash_t h;
    while (_PySet_NextEntry(a, &pos, &k, &h)) { Py_ssize_t ix = set_lookup(S(b), k, h, NULL); if (ix == -3) return -1; if (ix < 0) return 0; }
    return 1;
}

static PyObject *set_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED;
    int r;
    switch (op) {
    case Py_EQ: if (S(a)->used != S(b)->used) Py_RETURN_FALSE; r = issubset(a, b); break;
    case Py_NE: { PyObject *e = set_richcompare(a, b, Py_EQ); if (!e) return NULL; int t = e == Py_True; Py_DECREF(e); Py_RETURN_BOOL(!t); }
    case Py_LE: r = issubset(a, b); break;
    case Py_GE: r = issubset(b, a); break;
    case Py_LT: if (S(a)->used >= S(b)->used) Py_RETURN_FALSE; r = issubset(a, b); break;
    default: if (S(a)->used <= S(b)->used) Py_RETURN_FALSE; r = issubset(b, a); break;
    }
    if (r < 0) return NULL;
    Py_RETURN_BOOL(r);
}

static PyObject *set_union_impl(PyObject *a, PyObject *b) {
    PyObject *r = set_copy_as(a, result_type(a));
    if (!r) return NULL;
    if (set_update_internal(r, b) < 0) { Py_DECREF(r); return NULL; }
    return r;
}
static PyObject *set_intersection_impl(PyObject *a, PyObject *b) {
    PyObject *r = make_set(result_type(a));
    if (!r) return NULL;
    PyObject *other = PyAnySet_Check(b) ? Py_NewRef(b) : PySet_New(b);
    if (!other) { Py_DECREF(r); return NULL; }
    PyObject *small = S(a)->used <= S(other)->used ? a : other, *big = small == a ? other : a;
    Py_ssize_t pos = 0; PyObject *k; Py_hash_t h;
    while (_PySet_NextEntry(small, &pos, &k, &h)) { Py_ssize_t ix = set_lookup(S(big), k, h, NULL); if (ix == -3) { Py_DECREF(r); Py_DECREF(other); return NULL; } if (ix >= 0 && set_add_hash(S(r), k, h) < 0) { Py_DECREF(r); Py_DECREF(other); return NULL; } }
    Py_DECREF(other);
    return r;
}
static PyObject *set_difference_impl(PyObject *a, PyObject *b) {
    PyObject *r = make_set(result_type(a));
    if (!r) return NULL;
    PyObject *other = PyAnySet_Check(b) ? Py_NewRef(b) : PyDict_CheckExact(b) ? Py_NewRef(b) : PySet_New(b);
    if (!other) { Py_DECREF(r); return NULL; }
    Py_ssize_t pos = 0; PyObject *k; Py_hash_t h;
    while (_PySet_NextEntry(a, &pos, &k, &h)) {
        int in;
        if (PyDict_CheckExact(other)) in = PyDict_Contains(other, k); else { Py_ssize_t ix = set_lookup(S(other), k, h, NULL); in = ix == -3 ? -1 : ix >= 0; }
        if (in < 0) { Py_DECREF(r); Py_DECREF(other); return NULL; }
        if (!in && set_add_hash(S(r), k, h) < 0) { Py_DECREF(r); Py_DECREF(other); return NULL; }
    }
    Py_DECREF(other);
    return r;
}
static PyObject *set_symdiff_impl(PyObject *a, PyObject *b) {
    PyObject *other = PyAnySet_Check(b) ? Py_NewRef(b) : PySet_New(b);
    if (!other) return NULL;
    PyObject *r = set_difference_impl(a, other);
    if (!r) { Py_DECREF(other); return NULL; }
    PyObject *r2 = set_difference_impl(other, a);
    Py_DECREF(other);
    if (!r2) { Py_DECREF(r); return NULL; }
    int rc = set_update_internal(r, r2);
    Py_DECREF(r2);
    if (rc < 0) { Py_DECREF(r); return NULL; }
    return r;
}

static PyObject *set_or(PyObject *a, PyObject *b) { if (!PyAnySet_Check(a) || !PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; return set_union_impl(a, b); }
static PyObject *set_and(PyObject *a, PyObject *b) { if (!PyAnySet_Check(a) || !PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; return set_intersection_impl(a, b); }
static PyObject *set_sub(PyObject *a, PyObject *b) { if (!PyAnySet_Check(a) || !PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; return set_difference_impl(a, b); }
static PyObject *set_xor(PyObject *a, PyObject *b) { if (!PyAnySet_Check(a) || !PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; return set_symdiff_impl(a, b); }
static PyObject *set_ior(PyObject *a, PyObject *b) { if (!PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; if (set_update_internal(a, b) < 0) return NULL; return Py_NewRef(a); }
static PyObject *set_iand(PyObject *a, PyObject *b) { if (!PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; PyObject *r = set_intersection_impl(a, b); if (!r) return NULL; PySet_Clear(a); set_update_internal(a, r); Py_DECREF(r); return Py_NewRef(a); }
static PyObject *set_isub(PyObject *a, PyObject *b) { if (!PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; Py_ssize_t pos = 0; PyObject *k; while (_PySet_NextEntry(b, &pos, &k, NULL)) if (PySet_Discard(a, k) < 0) return NULL; return Py_NewRef(a); }
static PyObject *set_ixor(PyObject *a, PyObject *b) { if (!PyAnySet_Check(b)) Py_RETURN_NOTIMPLEMENTED; PyObject *r = set_symdiff_impl(a, b); if (!r) return NULL; PySet_Clear(a); set_update_internal(a, r); Py_DECREF(r); return Py_NewRef(a); }

/* ---- iterator --------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *set; Py_ssize_t pos; Py_ssize_t used; } setiterobject;
static void setiter_dealloc(PyObject *o) { Py_XDECREF(((setiterobject *)o)->set); PyObject_Free(o); }
static PyObject *setiter_next(PyObject *o) {
    setiterobject *it = (setiterobject *)o;
    if (!it->set) return NULL;
    if (S(it->set)->used != it->used) { it->used = -1; PyErr_SetString(PyExc_RuntimeError, "Set changed size during iteration"); return NULL; }
    PyObject *k;
    if (!_PySet_NextEntry(it->set, &it->pos, &k, NULL)) { Py_CLEAR(it->set); return NULL; }
    return Py_NewRef(k);
}
static PyObject *setiter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); setiterobject *it = (setiterobject *)o; return PyLong_FromSsize_t(it->set ? S(it->set)->used : 0); }
static PyMethodDef setiter_methods[] = { { "__length_hint__", setiter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PySetIter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "set_iterator", .tp_basicsize = sizeof(setiterobject), .tp_dealloc = setiter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = setiter_next, .tp_methods = setiter_methods };
static PyObject *set_iter(PyObject *o) {
    setiterobject *it = PyObject_New(setiterobject, &PySetIter_Type);
    if (!it) return NULL;
    it->set = Py_NewRef(o);
    it->pos = 0;
    it->used = S(o)->used;
    return (PyObject *)it;
}

/* ---- methods ----------------------------------------------------------------- */

static PyObject *set_add_m(PyObject *o, PyObject *k) { if (PySet_Add(o, k) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *set_discard_m(PyObject *o, PyObject *k) { if (PySet_Discard(o, k) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *set_remove_m(PyObject *o, PyObject *k) { int r = PySet_Discard(o, k); if (r < 0) return NULL; if (!r) { PyErr_SetObject(PyExc_KeyError, k); return NULL; } Py_RETURN_NONE; }
static PyObject *set_pop_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PySet_Pop(o); }
static PyObject *set_clear_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PySet_Clear(o); Py_RETURN_NONE; }
static PyObject *set_copy_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); if (PyFrozenSet_Check(o) && Py_IS_TYPE(o, &PyFrozenSet_Type)) return Py_NewRef(o); return set_copy_as(o, result_type(o)); }
static PyObject *set_update_m(PyObject *o, PyObject *const *a, Py_ssize_t n) { for (Py_ssize_t i = 0; i < n; i++) if (set_update_internal(o, a[i]) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *set_union_m(PyObject *o, PyObject *const *a, Py_ssize_t n) { PyObject *r = set_copy_as(o, result_type(o)); for (Py_ssize_t i = 0; r && i < n; i++) if (set_update_internal(r, a[i]) < 0) Py_CLEAR(r); return r; }
static PyObject *set_intersection_m(PyObject *o, PyObject *const *a, Py_ssize_t n) { PyObject *r = set_copy_as(o, result_type(o)); for (Py_ssize_t i = 0; r && i < n; i++) { PyObject *t = set_intersection_impl(r, a[i]); Py_DECREF(r); r = t; } return r; }
static PyObject *set_difference_m(PyObject *o, PyObject *const *a, Py_ssize_t n) { PyObject *r = set_copy_as(o, result_type(o)); for (Py_ssize_t i = 0; r && i < n; i++) { PyObject *t = set_difference_impl(r, a[i]); Py_DECREF(r); r = t; } return r; }
static PyObject *set_symdiff_m(PyObject *o, PyObject *b) { return set_symdiff_impl(o, b); }
static PyObject *set_intersection_update_m(PyObject *o, PyObject *const *a, Py_ssize_t n) { PyObject *r = set_intersection_m(o, a, n); if (!r) return NULL; PySet_Clear(o); set_update_internal(o, r); Py_DECREF(r); Py_RETURN_NONE; }
static PyObject *set_difference_update_m(PyObject *o, PyObject *const *a, Py_ssize_t n) { for (Py_ssize_t i = 0; i < n; i++) { PyObject *it = PyObject_GetIter(a[i]); if (!it) return NULL; for (;;) { PyObject *k = PyIter_Next(it); if (!k) break; int r = PySet_Discard(o, k); Py_DECREF(k); if (r < 0) { Py_DECREF(it); return NULL; } } Py_DECREF(it); if (PyErr_Occurred()) return NULL; } Py_RETURN_NONE; }
static PyObject *set_symdiff_update_m(PyObject *o, PyObject *b) { PyObject *r = set_symdiff_impl(o, b); if (!r) return NULL; PySet_Clear(o); set_update_internal(o, r); Py_DECREF(r); Py_RETURN_NONE; }
static PyObject *set_issubset_m(PyObject *o, PyObject *b) { PyObject *other = PyAnySet_Check(b) ? Py_NewRef(b) : PySet_New(b); if (!other) return NULL; int r = issubset(o, other); Py_DECREF(other); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *set_issuperset_m(PyObject *o, PyObject *b) { PyObject *other = PyAnySet_Check(b) ? Py_NewRef(b) : PySet_New(b); if (!other) return NULL; int r = issubset(other, o); Py_DECREF(other); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *set_isdisjoint_m(PyObject *o, PyObject *b) { PyObject *r = set_intersection_impl(o, b); if (!r) return NULL; int d = S(r)->used == 0; Py_DECREF(r); Py_RETURN_BOOL(d); }
static PyObject *set_contains_m(PyObject *o, PyObject *k) { int r = PySet_Contains(o, k); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *set_sizeof_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyLong_FromSsize_t((Py_ssize_t)sizeof(PySetObject) + S(o)->size * (Py_ssize_t)sizeof(Py_ssize_t)); }
static PyObject *set_reduce_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyObject *l = PySequence_List(o); PyObject *args = PyTuple_Pack(1, l); Py_DECREF(l); PyObject *r = PyTuple_Pack(2, (PyObject *)Py_TYPE(o), args); Py_DECREF(args); return r; }
static PyObject *set_class_getitem(PyObject *cls, PyObject *arg) { extern PyObject *piper_generic_alias_new(PyObject *origin, PyObject *args); return piper_generic_alias_new(cls, arg); }

static PyMethodDef set_methods[] = {
    { "add", set_add_m, METH_O, NULL }, { "discard", set_discard_m, METH_O, NULL }, { "remove", set_remove_m, METH_O, NULL },
    { "pop", set_pop_m, METH_NOARGS, NULL }, { "clear", set_clear_m, METH_NOARGS, NULL }, { "copy", set_copy_m, METH_NOARGS, NULL },
    { "update", (PyCFunction)(void (*)(void))set_update_m, METH_FASTCALL, NULL },
    { "union", (PyCFunction)(void (*)(void))set_union_m, METH_FASTCALL, NULL },
    { "intersection", (PyCFunction)(void (*)(void))set_intersection_m, METH_FASTCALL, NULL },
    { "difference", (PyCFunction)(void (*)(void))set_difference_m, METH_FASTCALL, NULL },
    { "symmetric_difference", set_symdiff_m, METH_O, NULL },
    { "intersection_update", (PyCFunction)(void (*)(void))set_intersection_update_m, METH_FASTCALL, NULL },
    { "difference_update", (PyCFunction)(void (*)(void))set_difference_update_m, METH_FASTCALL, NULL },
    { "symmetric_difference_update", set_symdiff_update_m, METH_O, NULL },
    { "issubset", set_issubset_m, METH_O, NULL }, { "issuperset", set_issuperset_m, METH_O, NULL }, { "isdisjoint", set_isdisjoint_m, METH_O, NULL },
    { "__contains__", set_contains_m, METH_O, NULL }, { "__sizeof__", set_sizeof_m, METH_NOARGS, NULL }, { "__reduce__", set_reduce_m, METH_NOARGS, NULL },
    { "__class_getitem__", set_class_getitem, METH_O | METH_CLASS, NULL },
    { NULL, NULL, 0, NULL },
};
static PyMethodDef frozenset_methods[] = {
    { "copy", set_copy_m, METH_NOARGS, NULL },
    { "union", (PyCFunction)(void (*)(void))set_union_m, METH_FASTCALL, NULL },
    { "intersection", (PyCFunction)(void (*)(void))set_intersection_m, METH_FASTCALL, NULL },
    { "difference", (PyCFunction)(void (*)(void))set_difference_m, METH_FASTCALL, NULL },
    { "symmetric_difference", set_symdiff_m, METH_O, NULL },
    { "issubset", set_issubset_m, METH_O, NULL }, { "issuperset", set_issuperset_m, METH_O, NULL }, { "isdisjoint", set_isdisjoint_m, METH_O, NULL },
    { "__contains__", set_contains_m, METH_O, NULL }, { "__sizeof__", set_sizeof_m, METH_NOARGS, NULL }, { "__reduce__", set_reduce_m, METH_NOARGS, NULL },
    { "__class_getitem__", set_class_getitem, METH_O | METH_CLASS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyObject *set_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) { PIPER_UNUSED(args); PIPER_UNUSED(kwds); return make_set(tp); }
static int set_init(PyObject *o, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "set() takes no keyword arguments"); return -1; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "set expected at most 1 argument, got %zd", n); return -1; }
    if (S(o)->used) PySet_Clear(o);
    if (n == 1) return set_update_internal(o, PyTuple_GET_ITEM(args, 0));
    return 0;
}
static PyObject *frozenset_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "frozenset() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "frozenset expected at most 1 argument, got %zd", n); return NULL; }
    if (n == 1 && tp == &PyFrozenSet_Type && Py_IS_TYPE(PyTuple_GET_ITEM(args, 0), &PyFrozenSet_Type)) return Py_NewRef(PyTuple_GET_ITEM(args, 0));
    PyObject *s = make_set(tp);
    if (!s) return NULL;
    if (n == 1 && set_update_internal(s, PyTuple_GET_ITEM(args, 0)) < 0) { Py_DECREF(s); return NULL; }
    return s;
}

static PySequenceMethods set_as_sequence = { .sq_length = set_len, .sq_contains = set_contains };
static PyNumberMethods set_as_number = { .nb_subtract = set_sub, .nb_and = set_and, .nb_xor = set_xor, .nb_or = set_or, .nb_inplace_subtract = set_isub, .nb_inplace_and = set_iand, .nb_inplace_xor = set_ixor, .nb_inplace_or = set_ior };
static PyNumberMethods frozenset_as_number = { .nb_subtract = set_sub, .nb_and = set_and, .nb_xor = set_xor, .nb_or = set_or };

PyTypeObject PySet_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "set", .tp_basicsize = sizeof(PySetObject), .tp_dealloc = set_dealloc, .tp_repr = set_repr,
    .tp_as_number = &set_as_number, .tp_as_sequence = &set_as_sequence, .tp_hash = PyObject_HashNotImplemented,
    .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_MATCH_SELF, .tp_richcompare = set_richcompare,
    .tp_iter = set_iter, .tp_methods = set_methods, .tp_init = set_init, .tp_alloc = PyType_GenericAlloc, .tp_new = set_new, .tp_free = PyObject_Free,
};
PyTypeObject PyFrozenSet_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "frozenset", .tp_basicsize = sizeof(PySetObject), .tp_dealloc = set_dealloc, .tp_repr = set_repr,
    .tp_as_number = &frozenset_as_number, .tp_as_sequence = &set_as_sequence, .tp_hash = frozenset_hash,
    .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_MATCH_SELF, .tp_richcompare = set_richcompare,
    .tp_iter = set_iter, .tp_methods = frozenset_methods, .tp_alloc = PyType_GenericAlloc, .tp_new = frozenset_new, .tp_free = PyObject_Free,
};
