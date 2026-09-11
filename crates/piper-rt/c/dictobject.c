/* dict: insertion-ordered open-addressing hash table (compact layout). */
#include "internal.h"

typedef struct {
    Py_hash_t hash;
    PyObject *key;
    PyObject *value;
} DictEntry;

struct _dictobject {
    PyObject_HEAD
    Py_ssize_t used;        /* live entries */
    Py_ssize_t nentries;    /* entries used in `entries` incl. deleted */
    Py_ssize_t size;        /* index table size (power of two) */
    Py_ssize_t version;
    Py_ssize_t *indices;    /* size slots: -1 empty, -2 dummy, else entry index */
    DictEntry *entries;     /* capacity = size * 2 / 3 */
};

#define D(o) ((PyDictObject *)(o))
#define EMPTY (-1)
#define DUMMY (-2)
#define MIN_SIZE 8
#define USABLE(size) (((size) << 1) / 3)

static int dict_alloc_tables(PyDictObject *d, Py_ssize_t size) {
    d->indices = PyMem_Malloc((size_t)size * sizeof(Py_ssize_t));
    d->entries = PyMem_Calloc((size_t)USABLE(size), sizeof(DictEntry));
    if (!d->indices || !d->entries) { PyMem_Free(d->indices); PyMem_Free(d->entries); PyErr_NoMemory(); return -1; }
    for (Py_ssize_t i = 0; i < size; i++) d->indices[i] = EMPTY;
    d->size = size;
    d->used = 0;
    d->nentries = 0;
    return 0;
}

PyObject *piper_dict_new_presized(Py_ssize_t n) {
    PyDictObject *d = PyObject_Malloc(sizeof(PyDictObject));
    if (!d) return PyErr_NoMemory();
    PyObject_Init((PyObject *)d, &PyDict_Type);
    Py_ssize_t size = MIN_SIZE;
    while (USABLE(size) < n) size <<= 1;
    d->version = 0;
    if (dict_alloc_tables(d, size) < 0) { PyObject_Free(d); return NULL; }
    return (PyObject *)d;
}
PyObject *PyDict_New(void) { return piper_dict_new_presized(0); }

static void dict_dealloc(PyObject *o) {
    PyDictObject *d = D(o);
    for (Py_ssize_t i = 0; i < d->nentries; i++) { Py_XDECREF(d->entries[i].key); Py_XDECREF(d->entries[i].value); }
    PyMem_Free(d->indices);
    PyMem_Free(d->entries);
    Py_TYPE(o)->tp_free(o);
}

/* Find the index slot for key. Returns entry index (>=0), -1 if absent
 * (and *slot_out is a usable empty/dummy slot), -3 on error. */
static Py_ssize_t lookup(PyDictObject *d, PyObject *key, Py_hash_t hash, Py_ssize_t *slot_out) {
    size_t mask = (size_t)d->size - 1;
    size_t i = (size_t)hash & mask;
    size_t perturb = (size_t)hash;
    Py_ssize_t freeslot = -1;
    for (;;) {
        Py_ssize_t ix = d->indices[i];
        if (ix == EMPTY) { if (slot_out) *slot_out = freeslot >= 0 ? freeslot : (Py_ssize_t)i; return -1; }
        if (ix == DUMMY) { if (freeslot < 0) freeslot = (Py_ssize_t)i; }
        else {
            DictEntry *e = &d->entries[ix];
            if (e->key == key) return ix;
            if (e->hash == hash) {
                PyObject *startkey = e->key;
                Py_INCREF(startkey);
                int cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
                Py_DECREF(startkey);
                if (cmp < 0) return -3;
                if (d->entries != NULL && ix < d->nentries && d->entries[ix].key == startkey) { if (cmp) return ix; }
                else { /* mutated during compare: restart */ return lookup(d, key, hash, slot_out); }
            }
        }
        perturb >>= 5;
        i = (i * 5 + perturb + 1) & mask;
    }
}

static int dict_resize(PyDictObject *d, Py_ssize_t minsize) {
    Py_ssize_t newsize = MIN_SIZE;
    while (newsize <= minsize) newsize <<= 1;
    DictEntry *old = d->entries;
    Py_ssize_t oldn = d->nentries;
    PyMem_Free(d->indices);
    if (dict_alloc_tables(d, newsize) < 0) return -1;
    for (Py_ssize_t k = 0; k < oldn; k++) {
        if (!old[k].key) continue;
        size_t mask = (size_t)newsize - 1;
        size_t i = (size_t)old[k].hash & mask, perturb = (size_t)old[k].hash;
        while (d->indices[i] != EMPTY) { perturb >>= 5; i = (i * 5 + perturb + 1) & mask; }
        d->indices[i] = d->nentries;
        d->entries[d->nentries++] = old[k];
        d->used++;
    }
    PyMem_Free(old);
    return 0;
}

int piper_dict_insert(PyObject *o, PyObject *key, Py_hash_t hash, PyObject *value) {
    PyDictObject *d = D(o);
    Py_ssize_t slot;
    Py_ssize_t ix = lookup(d, key, hash, &slot);
    if (ix == -3) return -1;
    if (ix >= 0) {
        PyObject *old = d->entries[ix].value;
        d->entries[ix].value = Py_NewRef(value);
        Py_DECREF(old);
        d->version++;
        return 0;
    }
    if (d->nentries >= USABLE(d->size)) {
        if (dict_resize(d, d->used * 3) < 0) return -1;
        ix = lookup(d, key, hash, &slot);
        if (ix == -3) return -1;
    }
    d->indices[slot] = d->nentries;
    DictEntry *e = &d->entries[d->nentries++];
    e->hash = hash;
    e->key = Py_NewRef(key);
    e->value = Py_NewRef(value);
    d->used++;
    d->version++;
    return 0;
}

PyObject *piper_dict_lookup(PyObject *o, PyObject *key, Py_hash_t hash) {
    Py_ssize_t ix = lookup(D(o), key, hash, NULL);
    if (ix < 0) { if (ix == -3) PyErr_Clear(); return NULL; }
    return D(o)->entries[ix].value;
}

static Py_hash_t key_hash(PyObject *key) {
    if (PyUnicode_CheckExact(key)) return piper_unicode_hash(key);
    return PyObject_Hash(key);
}

int PyDict_SetItem(PyObject *o, PyObject *key, PyObject *value) {
    if (!PyDict_Check(o)) { PyErr_BadInternalCall(); return -1; }
    Py_hash_t h = key_hash(key);
    if (h == -1) return -1;
    return piper_dict_insert(o, key, h, value);
}
int PyDict_SetItemString(PyObject *o, const char *key, PyObject *value) {
    PyObject *k = PyUnicode_FromString(key);
    if (!k) return -1;
    int r = PyDict_SetItem(o, k, value);
    Py_DECREF(k);
    return r;
}

PyObject *PyDict_GetItemWithError(PyObject *o, PyObject *key) {
    if (!PyDict_Check(o)) { PyErr_BadInternalCall(); return NULL; }
    Py_hash_t h = key_hash(key);
    if (h == -1) return NULL;
    Py_ssize_t ix = lookup(D(o), key, h, NULL);
    if (ix < 0) return NULL;
    return D(o)->entries[ix].value;
}
PyObject *PyDict_GetItem(PyObject *o, PyObject *key) {
    PyObject *e = PyErr_GetRaisedException();
    PyObject *r = PyDict_GetItemWithError(o, key);
    PyErr_Clear();
    PyErr_SetRaisedException(e);
    return r;
}
PyObject *PyDict_GetItemString(PyObject *o, const char *key) {
    PyObject *k = PyUnicode_FromString(key);
    if (!k) { PyErr_Clear(); return NULL; }
    PyObject *r = PyDict_GetItem(o, k);
    Py_DECREF(k);
    return r;
}
int PyDict_GetItemRef(PyObject *o, PyObject *key, PyObject **result) {
    *result = PyDict_GetItemWithError(o, key);
    if (*result) { Py_INCREF(*result); return 1; }
    return PyErr_Occurred() ? -1 : 0;
}
int PyDict_GetItemStringRef(PyObject *o, const char *key, PyObject **result) {
    PyObject *k = PyUnicode_FromString(key);
    if (!k) { *result = NULL; return -1; }
    int r = PyDict_GetItemRef(o, k, result);
    Py_DECREF(k);
    return r;
}

int PyDict_Contains(PyObject *o, PyObject *key) {
    Py_hash_t h = key_hash(key);
    if (h == -1) return -1;
    Py_ssize_t ix = lookup(D(o), key, h, NULL);
    if (ix == -3) return -1;
    return ix >= 0;
}
int PyDict_ContainsString(PyObject *o, const char *key) { PyObject *k = PyUnicode_FromString(key); if (!k) return -1; int r = PyDict_Contains(o, k); Py_DECREF(k); return r; }

static int del_at(PyDictObject *d, Py_ssize_t ix, Py_hash_t hash) {
    size_t mask = (size_t)d->size - 1;
    size_t i = (size_t)hash & mask, perturb = (size_t)hash;
    while (d->indices[i] != ix) { perturb >>= 5; i = (i * 5 + perturb + 1) & mask; }
    d->indices[i] = DUMMY;
    DictEntry *e = &d->entries[ix];
    PyObject *k = e->key, *v = e->value;
    e->key = NULL; e->value = NULL;
    d->used--;
    d->version++;
    Py_DECREF(k); Py_DECREF(v);
    return 0;
}

int PyDict_DelItem(PyObject *o, PyObject *key) {
    Py_hash_t h = key_hash(key);
    if (h == -1) return -1;
    Py_ssize_t ix = lookup(D(o), key, h, NULL);
    if (ix == -3) return -1;
    if (ix < 0) { PyErr_SetObject(PyExc_KeyError, key); return -1; }
    return del_at(D(o), ix, h);
}
int PyDict_DelItemString(PyObject *o, const char *key) { PyObject *k = PyUnicode_FromString(key); if (!k) return -1; int r = PyDict_DelItem(o, k); Py_DECREF(k); return r; }

int PyDict_Pop(PyObject *o, PyObject *key, PyObject **result) {
    Py_hash_t h = key_hash(key);
    if (h == -1) { if (result) *result = NULL; return -1; }
    Py_ssize_t ix = lookup(D(o), key, h, NULL);
    if (ix == -3) { if (result) *result = NULL; return -1; }
    if (ix < 0) { if (result) *result = NULL; return 0; }
    PyObject *v = Py_NewRef(D(o)->entries[ix].value);
    del_at(D(o), ix, h);
    if (result) *result = v; else Py_DECREF(v);
    return 1;
}
int PyDict_PopString(PyObject *o, const char *key, PyObject **result) { PyObject *k = PyUnicode_FromString(key); if (!k) return -1; int r = PyDict_Pop(o, k, result); Py_DECREF(k); return r; }

Py_ssize_t PyDict_Size(PyObject *o) { if (!PyDict_Check(o)) { PyErr_BadInternalCall(); return -1; } return D(o)->used; }
Py_ssize_t piper_dict_version(PyObject *o) { return D(o)->version; }

void PyDict_Clear(PyObject *o) {
    PyDictObject *d = D(o);
    DictEntry *old = d->entries;
    Py_ssize_t n = d->nentries;
    PyMem_Free(d->indices);
    dict_alloc_tables(d, MIN_SIZE);
    d->version++;
    for (Py_ssize_t i = 0; i < n; i++) { Py_XDECREF(old[i].key); Py_XDECREF(old[i].value); }
    PyMem_Free(old);
}

int piper_dict_next(PyObject *o, Py_ssize_t *pos, PyObject **k, PyObject **v, Py_hash_t *hash) {
    PyDictObject *d = D(o);
    Py_ssize_t i = *pos;
    while (i < d->nentries && !d->entries[i].key) i++;
    if (i >= d->nentries) return 0;
    if (k) *k = d->entries[i].key;
    if (v) *v = d->entries[i].value;
    if (hash) *hash = d->entries[i].hash;
    *pos = i + 1;
    return 1;
}
int PyDict_Next(PyObject *o, Py_ssize_t *pos, PyObject **k, PyObject **v) { return piper_dict_next(o, pos, k, v, NULL); }

PyObject *PyDict_Keys(PyObject *o) {
    PyObject *l = PyList_New(D(o)->used);
    Py_ssize_t pos = 0, i = 0; PyObject *k;
    while (PyDict_Next(o, &pos, &k, NULL)) PyList_SET_ITEM(l, i++, Py_NewRef(k));
    return l;
}
PyObject *PyDict_Values(PyObject *o) {
    PyObject *l = PyList_New(D(o)->used);
    Py_ssize_t pos = 0, i = 0; PyObject *v;
    while (PyDict_Next(o, &pos, NULL, &v)) PyList_SET_ITEM(l, i++, Py_NewRef(v));
    return l;
}
PyObject *PyDict_Items(PyObject *o) {
    PyObject *l = PyList_New(D(o)->used);
    Py_ssize_t pos = 0, i = 0; PyObject *k, *v;
    while (PyDict_Next(o, &pos, &k, &v)) { PyObject *t = PyTuple_Pack(2, k, v); PyList_SET_ITEM(l, i++, t); }
    return l;
}
PyObject *PyDict_Copy(PyObject *o) {
    PyObject *r = piper_dict_new_presized(D(o)->used);
    if (!r) return NULL;
    Py_ssize_t pos = 0; PyObject *k, *v; Py_hash_t h;
    while (piper_dict_next(o, &pos, &k, &v, &h)) if (piper_dict_insert(r, k, h, v) < 0) { Py_DECREF(r); return NULL; }
    return r;
}

int PyDict_Merge(PyObject *a, PyObject *b, int override) {
    if (PyDict_Check(b)) {
        if (a == b || D(b)->used == 0) return 0;
        Py_ssize_t pos = 0; PyObject *k, *v; Py_hash_t h;
        while (piper_dict_next(b, &pos, &k, &v, &h)) {
            if (!override) { Py_ssize_t ix = lookup(D(a), k, h, NULL); if (ix == -3) return -1; if (ix >= 0) continue; }
            Py_INCREF(k); Py_INCREF(v);
            int r = piper_dict_insert(a, k, h, v);
            Py_DECREF(k); Py_DECREF(v);
            if (r < 0) return -1;
        }
        return 0;
    }
    PyObject *keys = PyMapping_Keys(b);
    if (!keys) return -1;
    PyObject *it = PyObject_GetIter(keys);
    Py_DECREF(keys);
    if (!it) return -1;
    for (;;) {
        PyObject *k = PyIter_Next(it);
        if (!k) break;
        if (!override && PyDict_Contains(a, k) == 1) { Py_DECREF(k); continue; }
        PyObject *v = PyObject_GetItem(b, k);
        if (!v) { Py_DECREF(k); Py_DECREF(it); return -1; }
        int r = PyDict_SetItem(a, k, v);
        Py_DECREF(k); Py_DECREF(v);
        if (r < 0) { Py_DECREF(it); return -1; }
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
int PyDict_Update(PyObject *a, PyObject *b) { return PyDict_Merge(a, b, 1); }
int piper_dict_update(PyObject *d, PyObject *other) {
    if (PyDict_Check(other) || PyObject_HasAttrString(other, "keys")) return PyDict_Merge(d, other, 1);
    PyErr_Format(PyExc_TypeError, "'%s' object is not a mapping", Py_TYPE(other)->tp_name);
    return -1;
}
int piper_dict_merge_call(PyObject *d, PyObject *other, PyObject *func) {
    if (!PyDict_Check(other) && !PyObject_HasAttrString(other, "keys")) {
        PyObject *name = func ? PyObject_GetAttrString(func, "__qualname__") : NULL;
        if (name) { PyErr_Format(PyExc_TypeError, "%U() argument after ** must be a mapping, not %s", name, Py_TYPE(other)->tp_name); Py_DECREF(name); }
        else { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "argument after ** must be a mapping, not %s", Py_TYPE(other)->tp_name); }
        return -1;
    }
    /* duplicate keyword detection */
    if (PyDict_Check(other)) {
        Py_ssize_t pos = 0; PyObject *k;
        while (PyDict_Next(other, &pos, &k, NULL)) {
            int c = PyDict_Contains(d, k);
            if (c < 0) return -1;
            if (c) {
                PyObject *name = func ? PyObject_GetAttrString(func, "__qualname__") : NULL;
                if (name) { PyErr_Format(PyExc_TypeError, "%U() got multiple values for keyword argument '%U'", name, k); Py_DECREF(name); }
                else { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "got multiple values for keyword argument '%U'", k); }
                return -1;
            }
        }
    }
    return PyDict_Merge(d, other, 1);
}
int PyDict_MergeFromSeq2(PyObject *d, PyObject *seq, int override) {
    PyObject *it = PyObject_GetIter(seq);
    if (!it) return -1;
    Py_ssize_t i = 0;
    for (;; i++) {
        PyObject *item = PyIter_Next(it);
        if (!item) break;
        PyObject *fast = PySequence_Fast(item, "");
        if (!fast) {
            if (PyErr_ExceptionMatches(PyExc_TypeError)) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "cannot convert dictionary update sequence element #%zd to a sequence", i); }
            Py_DECREF(item); Py_DECREF(it); return -1;
        }
        if (PySequence_Fast_GET_SIZE(fast) != 2) {
            PyErr_Format(PyExc_ValueError, "dictionary update sequence element #%zd has length %zd; 2 is required", i, PySequence_Fast_GET_SIZE(fast));
            Py_DECREF(fast); Py_DECREF(item); Py_DECREF(it); return -1;
        }
        PyObject *k = PySequence_Fast_GET_ITEM(fast, 0), *v = PySequence_Fast_GET_ITEM(fast, 1);
        int r = 0;
        if (override || PyDict_Contains(d, k) == 0) r = PyDict_SetItem(d, k, v);
        Py_DECREF(fast); Py_DECREF(item);
        if (r < 0) { Py_DECREF(it); return -1; }
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}

int PyDict_SetDefaultRef(PyObject *o, PyObject *key, PyObject *dflt, PyObject **result) {
    Py_hash_t h = key_hash(key);
    if (h == -1) { if (result) *result = NULL; return -1; }
    Py_ssize_t ix = lookup(D(o), key, h, NULL);
    if (ix == -3) { if (result) *result = NULL; return -1; }
    if (ix >= 0) { if (result) *result = Py_NewRef(D(o)->entries[ix].value); return 1; }
    if (piper_dict_insert(o, key, h, dflt) < 0) { if (result) *result = NULL; return -1; }
    if (result) *result = Py_NewRef(dflt);
    return 0;
}
PyObject *PyDict_SetDefault(PyObject *o, PyObject *key, PyObject *dflt) {
    PyObject *r;
    if (PyDict_SetDefaultRef(o, key, dflt, &r) < 0) return NULL;
    return r;
}

/* ---- slots ------------------------------------------------------------------- */

static PyObject *dict_repr(PyObject *o) {
    if (D(o)->used == 0) return PyUnicode_FromString("{}");
    int rc = Py_ReprEnter(o);
    if (rc != 0) return rc > 0 ? PyUnicode_FromString("{...}") : NULL;
    PyObject *parts = PyList_New(0);
    PyObject *sep = PyUnicode_FromString(", "), *colon = PyUnicode_FromString(": ");
    PyObject *s = PyUnicode_FromString("{"); PyList_Append(parts, s); Py_DECREF(s);
    Py_ssize_t pos = 0; PyObject *k, *v; int first = 1;
    while (PyDict_Next(o, &pos, &k, &v)) {
        if (!first) PyList_Append(parts, sep);
        first = 0;
        Py_INCREF(k); Py_INCREF(v);
        PyObject *kr = PyObject_Repr(k), *vr = kr ? PyObject_Repr(v) : NULL;
        Py_DECREF(k); Py_DECREF(v);
        if (!kr || !vr) { Py_XDECREF(kr); Py_XDECREF(vr); Py_DECREF(parts); Py_DECREF(sep); Py_DECREF(colon); Py_ReprLeave(o); return NULL; }
        PyList_Append(parts, kr); PyList_Append(parts, colon); PyList_Append(parts, vr);
        Py_DECREF(kr); Py_DECREF(vr);
    }
    s = PyUnicode_FromString("}"); PyList_Append(parts, s); Py_DECREF(s);
    Py_DECREF(sep); Py_DECREF(colon);
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts);
    Py_ReprLeave(o);
    return r;
}
static Py_ssize_t dict_length(PyObject *o) { return D(o)->used; }
static PyObject *dict_subscript(PyObject *o, PyObject *key) {
    Py_hash_t h = key_hash(key);
    if (h == -1) return NULL;
    Py_ssize_t ix = lookup(D(o), key, h, NULL);
    if (ix == -3) return NULL;
    if (ix >= 0) return Py_NewRef(D(o)->entries[ix].value);
    if (!PyDict_CheckExact(o)) {
        PyObject *missing = piper_type_lookup(Py_TYPE(o), piper_intern("__missing__"));
        if (missing) { PyObject *a[2] = { o, key }; return PyObject_Vectorcall(missing, a, 2, NULL); }
    }
    PyErr_SetObject(PyExc_KeyError, key);
    return NULL;
}
static int dict_ass_subscript(PyObject *o, PyObject *key, PyObject *v) { return v ? PyDict_SetItem(o, key, v) : PyDict_DelItem(o, key); }
static int dict_contains(PyObject *o, PyObject *key) { return PyDict_Contains(o, key); }

static PyObject *dict_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PyDict_Check(a) || !PyDict_Check(b) || (op != Py_EQ && op != Py_NE)) Py_RETURN_NOTIMPLEMENTED;
    int eq = 1;
    if (D(a)->used != D(b)->used) eq = 0;
    else {
        Py_ssize_t pos = 0; PyObject *k, *v; Py_hash_t h;
        while (piper_dict_next(a, &pos, &k, &v, &h)) {
            Py_ssize_t ix = lookup(D(b), k, h, NULL);
            if (ix == -3) return NULL;
            if (ix < 0) { eq = 0; break; }
            Py_INCREF(v);
            PyObject *bv = D(b)->entries[ix].value;
            Py_INCREF(bv);
            int c = PyObject_RichCompareBool(v, bv, Py_EQ);
            Py_DECREF(v); Py_DECREF(bv);
            if (c < 0) return NULL;
            if (!c) { eq = 0; break; }
        }
    }
    Py_RETURN_BOOL(op == Py_EQ ? eq : !eq);
}

/* ---- iterators and views ------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *dict; Py_ssize_t pos; Py_ssize_t used; Py_ssize_t version; int kind; Py_ssize_t remaining; } dictiterobject;
static void dictiter_dealloc(PyObject *o) { Py_XDECREF(((dictiterobject *)o)->dict); PyObject_Free(o); }
static PyObject *dictiter_next(PyObject *o) {
    dictiterobject *it = (dictiterobject *)o;
    if (!it->dict) return NULL;
    if (D(it->dict)->used != it->used) { it->used = -1; PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration"); return NULL; }
    PyObject *k, *v;
    if (!PyDict_Next(it->dict, &it->pos, &k, &v)) { Py_CLEAR(it->dict); return NULL; }
    it->remaining--;
    switch (it->kind) { case 0: return Py_NewRef(k); case 1: return Py_NewRef(v); default: return PyTuple_Pack(2, k, v); }
}
static PyObject *dictiter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); dictiterobject *it = (dictiterobject *)o; return PyLong_FromSsize_t(it->dict && it->remaining > 0 ? it->remaining : 0); }
static PyMethodDef dictiter_methods[] = { { "__length_hint__", dictiter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
#define DICTITER_TYPE(var, name) PyTypeObject var = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = name, .tp_basicsize = sizeof(dictiterobject), .tp_dealloc = dictiter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = dictiter_next, .tp_methods = dictiter_methods };
DICTITER_TYPE(PyDictIterKey_Type, "dict_keyiterator")
DICTITER_TYPE(PyDictIterValue_Type, "dict_valueiterator")
DICTITER_TYPE(PyDictIterItem_Type, "dict_itemiterator")

static PyObject *dict_iter_kind(PyObject *d, int kind) {
    PyTypeObject *tp = kind == 0 ? &PyDictIterKey_Type : kind == 1 ? &PyDictIterValue_Type : &PyDictIterItem_Type;
    dictiterobject *it = PyObject_New(dictiterobject, tp);
    if (!it) return NULL;
    it->dict = Py_NewRef(d);
    it->pos = 0;
    it->used = D(d)->used;
    it->version = D(d)->version;
    it->kind = kind;
    it->remaining = D(d)->used;
    return (PyObject *)it;
}
static PyObject *dict_iter(PyObject *d) { return dict_iter_kind(d, 0); }

typedef struct { PyObject_HEAD PyObject *dict; int kind; } dictviewobject;
static void dictview_dealloc(PyObject *o) { Py_XDECREF(((dictviewobject *)o)->dict); PyObject_Free(o); }
static Py_ssize_t dictview_len(PyObject *o) { return D(((dictviewobject *)o)->dict)->used; }
static PyObject *dictview_iter(PyObject *o) { return dict_iter_kind(((dictviewobject *)o)->dict, ((dictviewobject *)o)->kind); }
static int dictview_contains(PyObject *o, PyObject *v) {
    dictviewobject *dv = (dictviewobject *)o;
    if (dv->kind == 0) return PyDict_Contains(dv->dict, v);
    if (dv->kind == 2) {
        if (!PyTuple_Check(v) || PyTuple_GET_SIZE(v) != 2) return 0;
        PyObject *found = PyDict_GetItemWithError(dv->dict, PyTuple_GET_ITEM(v, 0));
        if (!found) return PyErr_Occurred() ? -1 : 0;
        return PyObject_RichCompareBool(found, PyTuple_GET_ITEM(v, 1), Py_EQ);
    }
    PyObject *it = dictview_iter(o);
    for (;;) { PyObject *x = PyIter_Next(it); if (!x) break; int r = PyObject_RichCompareBool(x, v, Py_EQ); Py_DECREF(x); if (r) { Py_DECREF(it); return r; } }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
static PyObject *dictview_repr(PyObject *o) {
    dictviewobject *dv = (dictviewobject *)o;
    PyObject *l = dv->kind == 0 ? PyDict_Keys(dv->dict) : dv->kind == 1 ? PyDict_Values(dv->dict) : PyDict_Items(dv->dict);
    if (!l) return NULL;
    PyObject *lr = PyObject_Repr(l);
    Py_DECREF(l);
    if (!lr) return NULL;
    PyObject *r = PyUnicode_FromFormat("%s(%U)", Py_TYPE(o)->tp_name, lr);
    Py_DECREF(lr);
    return r;
}
static PyObject *dictview_to_set(PyObject *o) { return PySet_New(o); }
static PyObject *dictview_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PyAnySet_Check(b) && !(Py_IS_TYPE(b, &PyDictKeys_Type) || Py_IS_TYPE(b, &PyDictItems_Type))) Py_RETURN_NOTIMPLEMENTED;
    PyObject *sa = dictview_to_set(a), *sb = PySet_New(b);
    if (!sa || !sb) { Py_XDECREF(sa); Py_XDECREF(sb); return NULL; }
    PyObject *r = PyObject_RichCompare(sa, sb, op);
    Py_DECREF(sa); Py_DECREF(sb);
    return r;
}
static PyObject *dictview_setop(PyObject *a, PyObject *b, const char *op) {
    PyObject *sa = PyAnySet_Check(a) ? Py_NewRef(a) : PySet_New(a);
    if (!sa) return NULL;
    PyObject *r = PyObject_CallMethod(sa, op, "O", b);
    Py_DECREF(sa);
    return r;
}
static PyObject *dictview_sub(PyObject *a, PyObject *b) { return dictview_setop(a, b, "difference"); }
static PyObject *dictview_and(PyObject *a, PyObject *b) { return dictview_setop(a, b, "intersection"); }
static PyObject *dictview_or(PyObject *a, PyObject *b) { return dictview_setop(a, b, "union"); }
static PyObject *dictview_xor(PyObject *a, PyObject *b) { return dictview_setop(a, b, "symmetric_difference"); }
static PyObject *dictview_isdisjoint(PyObject *a, PyObject *b) { PyObject *sa = dictview_to_set(a); if (!sa) return NULL; PyObject *r = PyObject_CallMethod(sa, "isdisjoint", "O", b); Py_DECREF(sa); return r; }
static PyObject *dictview_mapping(PyObject *o, void *c) { PIPER_UNUSED(c); return PyDictProxy_New(((dictviewobject *)o)->dict); }
static PyObject *dictview_reversed(PyObject *o, PyObject *u) {
    PIPER_UNUSED(u);
    dictviewobject *dv = (dictviewobject *)o;
    PyObject *l = dv->kind == 0 ? PyDict_Keys(dv->dict) : dv->kind == 1 ? PyDict_Values(dv->dict) : PyDict_Items(dv->dict);
    if (!l) return NULL;
    PyList_Reverse(l);
    PyObject *it = PyObject_GetIter(l);
    Py_DECREF(l);
    return it;
}
static PyMethodDef dictview_methods[] = { { "isdisjoint", dictview_isdisjoint, METH_O, NULL }, { "__reversed__", dictview_reversed, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyGetSetDef dictview_getsets[] = { { "mapping", dictview_mapping, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PySequenceMethods dictview_as_sequence = { .sq_length = dictview_len, .sq_contains = dictview_contains };
static PySequenceMethods dictvalues_as_sequence = { .sq_length = dictview_len };
static PyNumberMethods dictview_as_number = { .nb_subtract = dictview_sub, .nb_and = dictview_and, .nb_or = dictview_or, .nb_xor = dictview_xor };
PyTypeObject PyDictKeys_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "dict_keys", .tp_basicsize = sizeof(dictviewobject), .tp_dealloc = dictview_dealloc, .tp_repr = dictview_repr, .tp_as_number = &dictview_as_number, .tp_as_sequence = &dictview_as_sequence, .tp_getattro = PyObject_GenericGetAttr, .tp_richcompare = dictview_richcompare, .tp_iter = dictview_iter, .tp_methods = dictview_methods, .tp_getset = dictview_getsets };
PyTypeObject PyDictItems_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "dict_items", .tp_basicsize = sizeof(dictviewobject), .tp_dealloc = dictview_dealloc, .tp_repr = dictview_repr, .tp_as_number = &dictview_as_number, .tp_as_sequence = &dictview_as_sequence, .tp_getattro = PyObject_GenericGetAttr, .tp_richcompare = dictview_richcompare, .tp_iter = dictview_iter, .tp_methods = dictview_methods, .tp_getset = dictview_getsets };
PyTypeObject PyDictValues_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "dict_values", .tp_basicsize = sizeof(dictviewobject), .tp_dealloc = dictview_dealloc, .tp_repr = dictview_repr, .tp_as_sequence = &dictvalues_as_sequence, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = dictview_iter, .tp_methods = dictview_methods, .tp_getset = dictview_getsets };

PyObject *piper_dict_keys_view(PyObject *d, int kind) {
    PyTypeObject *tp = kind == 0 ? &PyDictKeys_Type : kind == 1 ? &PyDictValues_Type : &PyDictItems_Type;
    dictviewobject *v = PyObject_New(dictviewobject, tp);
    if (!v) return NULL;
    v->dict = Py_NewRef(d);
    v->kind = kind;
    return (PyObject *)v;
}

/* ---- methods ------------------------------------------------------------------- */

static PyObject *dict_keys_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return piper_dict_keys_view(o, 0); }
static PyObject *dict_values_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return piper_dict_keys_view(o, 1); }
static PyObject *dict_items_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return piper_dict_keys_view(o, 2); }
static PyObject *dict_get_m(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("get", n, 1, 2) < 0) return NULL;
    PyObject *v = PyDict_GetItemWithError(o, a[0]);
    if (v) return Py_NewRef(v);
    if (PyErr_Occurred()) return NULL;
    return Py_NewRef(n == 2 ? a[1] : Py_None);
}
static PyObject *dict_setdefault_m(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("setdefault", n, 1, 2) < 0) return NULL;
    PyObject *r;
    if (PyDict_SetDefaultRef(o, a[0], n == 2 ? a[1] : Py_None, &r) < 0) return NULL;
    return r;
}
static PyObject *dict_pop_m(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("pop", n, 1, 2) < 0) return NULL;
    PyObject *r;
    int rc = PyDict_Pop(o, a[0], &r);
    if (rc < 0) return NULL;
    if (rc == 1) return r;
    if (n == 2) return Py_NewRef(a[1]);
    PyErr_SetObject(PyExc_KeyError, a[0]);
    return NULL;
}
static PyObject *dict_popitem_m(PyObject *o, PyObject *u) {
    PIPER_UNUSED(u);
    PyDictObject *d = D(o);
    if (d->used == 0) { PyErr_SetString(PyExc_KeyError, "popitem(): dictionary is empty"); return NULL; }
    Py_ssize_t i = d->nentries - 1;
    while (i >= 0 && !d->entries[i].key) i--;
    PyObject *t = PyTuple_Pack(2, d->entries[i].key, d->entries[i].value);
    del_at(d, i, d->entries[i].hash);
    d->nentries = i;
    return t;
}
static PyObject *dict_update_m(PyObject *o, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    if (n > 1) { PyErr_Format(PyExc_TypeError, "update expected at most 1 argument, got %zd", n); return NULL; }
    if (n == 1) {
        PyObject *other = a[0];
        int r;
        if (PyDict_Check(other)) r = PyDict_Merge(o, other, 1);
        else if (PyObject_HasAttrString(other, "keys")) r = PyDict_Merge(o, other, 1);
        else r = PyDict_MergeFromSeq2(o, other, 1);
        if (r < 0) return NULL;
    }
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) if (PyDict_SetItem(o, PyTuple_GET_ITEM(kw, i), a[n + i]) < 0) return NULL;
    Py_RETURN_NONE;
}
static PyObject *dict_copy_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyDict_Copy(o); }
static PyObject *dict_clear_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyDict_Clear(o); Py_RETURN_NONE; }
static PyObject *dict_fromkeys_m(PyObject *cls, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("fromkeys", n, 1, 2) < 0) return NULL;
    PyObject *value = n == 2 ? a[1] : Py_None;
    PyObject *d = PyObject_CallNoArgs(cls);
    if (!d) return NULL;
    PyObject *it = PyObject_GetIter(a[0]);
    if (!it) { Py_DECREF(d); return NULL; }
    for (;;) {
        PyObject *k = PyIter_Next(it);
        if (!k) break;
        int r = PyObject_SetItem(d, k, value);
        Py_DECREF(k);
        if (r < 0) { Py_DECREF(it); Py_DECREF(d); return NULL; }
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) { Py_DECREF(d); return NULL; }
    return d;
}
static PyObject *dict_contains_m(PyObject *o, PyObject *k) { int r = PyDict_Contains(o, k); if (r < 0) return NULL; Py_RETURN_BOOL(r); }
static PyObject *dict_getitem_m(PyObject *o, PyObject *k) { return dict_subscript(o, k); }
static PyObject *dict_sizeof_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyLong_FromSsize_t((Py_ssize_t)sizeof(PyDictObject) + D(o)->size * (Py_ssize_t)sizeof(Py_ssize_t) + USABLE(D(o)->size) * (Py_ssize_t)sizeof(DictEntry)); }
static PyObject *dict_reversed_m(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyObject *l = PyDict_Keys(o); PyList_Reverse(l); PyObject *it = PyObject_GetIter(l); Py_DECREF(l); return it; }
static PyObject *dict_or(PyObject *a, PyObject *b) { if (!PyDict_Check(a) || !PyDict_Check(b)) Py_RETURN_NOTIMPLEMENTED; PyObject *r = PyDict_Copy(a); if (!r) return NULL; if (PyDict_Update(r, b) < 0) { Py_DECREF(r); return NULL; } return r; }
static PyObject *dict_ior(PyObject *a, PyObject *b) { if (piper_dict_update(a, b) < 0) return NULL; return Py_NewRef(a); }
static PyObject *dict_class_getitem(PyObject *cls, PyObject *arg) { extern PyObject *piper_generic_alias_new(PyObject *origin, PyObject *args); return piper_generic_alias_new(cls, arg); }

static PyMethodDef dict_methods[] = {
    { "keys", dict_keys_m, METH_NOARGS, NULL }, { "values", dict_values_m, METH_NOARGS, NULL }, { "items", dict_items_m, METH_NOARGS, NULL },
    { "get", (PyCFunction)(void (*)(void))dict_get_m, METH_FASTCALL, NULL },
    { "setdefault", (PyCFunction)(void (*)(void))dict_setdefault_m, METH_FASTCALL, NULL },
    { "pop", (PyCFunction)(void (*)(void))dict_pop_m, METH_FASTCALL, NULL },
    { "popitem", dict_popitem_m, METH_NOARGS, NULL },
    { "update", (PyCFunction)(void (*)(void))dict_update_m, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "copy", dict_copy_m, METH_NOARGS, NULL }, { "clear", dict_clear_m, METH_NOARGS, NULL },
    { "fromkeys", (PyCFunction)(void (*)(void))dict_fromkeys_m, METH_FASTCALL | METH_CLASS, NULL },
    { "__contains__", dict_contains_m, METH_O, NULL }, { "__getitem__", dict_getitem_m, METH_O, NULL },
    { "__sizeof__", dict_sizeof_m, METH_NOARGS, NULL }, { "__reversed__", dict_reversed_m, METH_NOARGS, NULL },
    { "__class_getitem__", dict_class_getitem, METH_O | METH_CLASS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyObject *dict_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(args); PIPER_UNUSED(kwds);
    PyDictObject *d = (PyDictObject *)tp->tp_alloc(tp, 0);
    if (!d) return NULL;
    d->version = 0;
    if (dict_alloc_tables(d, MIN_SIZE) < 0) { Py_DECREF(d); return NULL; }
    return (PyObject *)d;
}
static int dict_init(PyObject *o, PyObject *args, PyObject *kwds) {
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "dict expected at most 1 argument, got %zd", n); return -1; }
    if (n == 1) {
        PyObject *other = PyTuple_GET_ITEM(args, 0);
        int r = (PyDict_Check(other) || PyObject_HasAttrString(other, "keys")) ? PyDict_Merge(o, other, 1) : PyDict_MergeFromSeq2(o, other, 1);
        if (r < 0) return -1;
    }
    if (kwds && PyDict_Merge(o, kwds, 1) < 0) return -1;
    return 0;
}

static PySequenceMethods dict_as_sequence = { .sq_contains = dict_contains };
static PyMappingMethods dict_as_mapping = { .mp_length = dict_length, .mp_subscript = dict_subscript, .mp_ass_subscript = dict_ass_subscript };
static PyNumberMethods dict_as_number = { .nb_or = dict_or, .nb_inplace_or = dict_ior };

PyTypeObject PyDict_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "dict",
    .tp_basicsize = sizeof(PyDictObject),
    .tp_dealloc = dict_dealloc,
    .tp_repr = dict_repr,
    .tp_as_number = &dict_as_number,
    .tp_as_sequence = &dict_as_sequence,
    .tp_as_mapping = &dict_as_mapping,
    .tp_hash = PyObject_HashNotImplemented,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_DICT_SUBCLASS | Py_TPFLAGS_MAPPING | Py_TPFLAGS_MATCH_SELF,
    .tp_richcompare = dict_richcompare,
    .tp_iter = dict_iter,
    .tp_methods = dict_methods,
    .tp_init = dict_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = dict_new,
    .tp_free = PyObject_Free,
};

/* ---- mappingproxy ---------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *mapping; } mappingproxyobject;
static void mappingproxy_dealloc(PyObject *o) { Py_XDECREF(((mappingproxyobject *)o)->mapping); PyObject_Free(o); }
static PyObject *mappingproxy_getitem(PyObject *o, PyObject *k) { return PyObject_GetItem(((mappingproxyobject *)o)->mapping, k); }
static Py_ssize_t mappingproxy_len(PyObject *o) { return PyObject_Size(((mappingproxyobject *)o)->mapping); }
static int mappingproxy_contains(PyObject *o, PyObject *k) { return PySequence_Contains(((mappingproxyobject *)o)->mapping, k); }
static PyObject *mappingproxy_iter(PyObject *o) { return PyObject_GetIter(((mappingproxyobject *)o)->mapping); }
static PyObject *mappingproxy_repr(PyObject *o) { PyObject *r = PyObject_Repr(((mappingproxyobject *)o)->mapping); if (!r) return NULL; PyObject *s = PyUnicode_FromFormat("mappingproxy(%U)", r); Py_DECREF(r); return s; }
static PyObject *mappingproxy_get(PyObject *o, PyObject *const *a, Py_ssize_t n) { PyObject *m = PyObject_GetAttrString(((mappingproxyobject *)o)->mapping, "get"); if (!m) return NULL; PyObject *r = PyObject_Vectorcall(m, a, (size_t)n, NULL); Py_DECREF(m); return r; }
static PyObject *mappingproxy_keys(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyObject_CallMethod(((mappingproxyobject *)o)->mapping, "keys", NULL); }
static PyObject *mappingproxy_values(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyObject_CallMethod(((mappingproxyobject *)o)->mapping, "values", NULL); }
static PyObject *mappingproxy_items(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyObject_CallMethod(((mappingproxyobject *)o)->mapping, "items", NULL); }
static PyObject *mappingproxy_copy(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return PyObject_CallMethod(((mappingproxyobject *)o)->mapping, "copy", NULL); }
static PyObject *mappingproxy_richcompare(PyObject *a, PyObject *b, int op) { return PyObject_RichCompare(((mappingproxyobject *)a)->mapping, b, op); }
static PyMethodDef mappingproxy_methods[] = { { "get", (PyCFunction)(void (*)(void))mappingproxy_get, METH_FASTCALL, NULL }, { "keys", mappingproxy_keys, METH_NOARGS, NULL }, { "values", mappingproxy_values, METH_NOARGS, NULL }, { "items", mappingproxy_items, METH_NOARGS, NULL }, { "copy", mappingproxy_copy, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PySequenceMethods mappingproxy_as_sequence = { .sq_contains = mappingproxy_contains };
static PyMappingMethods mappingproxy_as_mapping = { .mp_length = mappingproxy_len, .mp_subscript = mappingproxy_getitem };
static PyObject *mappingproxy_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) { PIPER_UNUSED(tp); PIPER_UNUSED(kwds); if (PyTuple_GET_SIZE(args) != 1) { PyErr_SetString(PyExc_TypeError, "mappingproxy() takes exactly one argument"); return NULL; } return PyDictProxy_New(PyTuple_GET_ITEM(args, 0)); }
PyTypeObject PyDictProxy_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "mappingproxy", .tp_basicsize = sizeof(mappingproxyobject), .tp_dealloc = mappingproxy_dealloc, .tp_repr = mappingproxy_repr,
    .tp_as_sequence = &mappingproxy_as_sequence, .tp_as_mapping = &mappingproxy_as_mapping, .tp_getattro = PyObject_GenericGetAttr,
    .tp_richcompare = mappingproxy_richcompare, .tp_iter = mappingproxy_iter, .tp_methods = mappingproxy_methods, .tp_new = mappingproxy_new,
};
PyObject *PyDictProxy_New(PyObject *mapping) {
    if (!PyMapping_Check(mapping)) { PyErr_Format(PyExc_TypeError, "mappingproxy() argument must be a mapping, not %s", Py_TYPE(mapping)->tp_name); return NULL; }
    mappingproxyobject *p = PyObject_New(mappingproxyobject, &PyDictProxy_Type);
    if (!p) return NULL;
    p->mapping = Py_NewRef(mapping);
    return (PyObject *)p;
}
