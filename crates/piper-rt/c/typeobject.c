/* Type objects: readiness, MRO, attribute lookup, instantiation, and
 * heap types created by class statements. */
#include "internal.h"

/* Heap type layout: PyTypeObject plus the slot tables it owns. */
typedef struct {
    PyTypeObject ht_type;
    PyAsyncMethods as_async;
    PyNumberMethods as_number;
    PyMappingMethods as_mapping;
    PySequenceMethods as_sequence;
    PyBufferProcs as_buffer;
    PyObject *ht_name, *ht_slots, *ht_qualname, *ht_module;
} PyHeapTypeObject;

const char *_PyType_Name(PyTypeObject *tp) {
    const char *dot = strrchr(tp->tp_name, '.');
    return dot ? dot + 1 : tp->tp_name;
}

PyObject *PyType_GetName(PyTypeObject *tp) { return PyUnicode_FromString(_PyType_Name(tp)); }
PyObject *PyType_GetQualName(PyTypeObject *tp) {
    if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE) return Py_NewRef(((PyHeapTypeObject *)tp)->ht_qualname);
    return PyType_GetName(tp);
}
unsigned long PyType_GetFlags(PyTypeObject *tp) { return tp->tp_flags; }
void PyType_Modified(PyTypeObject *tp) { tp->tp_version_tag++; }

int PyType_IsSubtype(PyTypeObject *a, PyTypeObject *b) {
    PyObject *mro = a->tp_mro;
    if (mro) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i++) if (PyTuple_GET_ITEM(mro, i) == (PyObject *)b) return 1;
        return 0;
    }
    do { if (a == b) return 1; a = a->tp_base; } while (a);
    return b == &PyBaseObject_Type;
}

/* Lookup through the MRO. Borrowed reference. */
PyObject *piper_type_lookup(PyTypeObject *tp, PyObject *name) {
    PyObject *mro = tp->tp_mro;
    if (!mro) {
        if (PyType_Ready(tp) < 0) { PyErr_Clear(); return NULL; }
        mro = tp->tp_mro;
        if (!mro) return NULL;
    }
    Py_hash_t h = PyObject_Hash(name);
    if (h == -1) { PyErr_Clear(); return NULL; }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if (!base->tp_dict) continue;
        PyObject *r = piper_dict_lookup(base->tp_dict, name, h);
        if (r) return r;
    }
    return NULL;
}
PyObject *_PyType_Lookup(PyTypeObject *tp, PyObject *name) { return piper_type_lookup(tp, name); }
PyObject *_PyType_LookupStr(PyTypeObject *tp, const char *name) {
    PyObject *n = piper_intern(name);
    return n ? piper_type_lookup(tp, n) : NULL;
}

/* ---- MRO (C3) ------------------------------------------------------------- */

static int tail_contains(PyObject *list, Py_ssize_t from, PyObject *o) {
    for (Py_ssize_t i = from; i < PyList_GET_SIZE(list); i++) if (PyList_GET_ITEM(list, i) == o) return 1;
    return 0;
}

static PyObject *mro_c3(PyTypeObject *tp) {
    PyObject *bases = tp->tp_bases;
    Py_ssize_t nb = PyTuple_GET_SIZE(bases);
    if (nb == 1) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(bases, 0);
        PyObject *bmro = base->tp_mro;
        PyObject *r = PyTuple_New(PyTuple_GET_SIZE(bmro) + 1);
        if (!r) return NULL;
        PyTuple_SET_ITEM(r, 0, Py_NewRef((PyObject *)tp));
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bmro); i++) PyTuple_SET_ITEM(r, i + 1, Py_NewRef(PyTuple_GET_ITEM(bmro, i)));
        return r;
    }
    /* seqs: mro of each base, then the bases list */
    Py_ssize_t nseq = nb + 1;
    PyObject **seqs = PyMem_Calloc((size_t)nseq, sizeof(PyObject *));
    Py_ssize_t *pos = PyMem_Calloc((size_t)nseq, sizeof(Py_ssize_t));
    PyObject *result = PyList_New(0);
    if (!seqs || !pos || !result) goto fail;
    if (PyList_Append(result, (PyObject *)tp) < 0) goto fail;
    for (Py_ssize_t i = 0; i < nb; i++) {
        seqs[i] = PySequence_List(((PyTypeObject *)PyTuple_GET_ITEM(bases, i))->tp_mro);
        if (!seqs[i]) goto fail;
    }
    seqs[nb] = PySequence_List(bases);
    if (!seqs[nb]) goto fail;
    for (;;) {
        int empty = 1;
        PyObject *cand = NULL;
        for (Py_ssize_t i = 0; i < nseq && !cand; i++) {
            if (pos[i] >= PyList_GET_SIZE(seqs[i])) continue;
            empty = 0;
            PyObject *c = PyList_GET_ITEM(seqs[i], pos[i]);
            int ok = 1;
            for (Py_ssize_t j = 0; j < nseq; j++) {
                if (tail_contains(seqs[j], pos[j] + 1, c)) { ok = 0; break; }
            }
            if (ok) cand = c;
        }
        if (empty) break;
        if (!cand) {
            PyErr_SetString(PyExc_TypeError, "Cannot create a consistent method resolution order (MRO) for bases");
            goto fail;
        }
        if (PyList_Append(result, cand) < 0) goto fail;
        for (Py_ssize_t j = 0; j < nseq; j++) {
            if (pos[j] < PyList_GET_SIZE(seqs[j]) && PyList_GET_ITEM(seqs[j], pos[j]) == cand) pos[j]++;
        }
    }
    PyObject *t = PyList_AsTuple(result);
    for (Py_ssize_t i = 0; i < nseq; i++) Py_XDECREF(seqs[i]);
    PyMem_Free(seqs); PyMem_Free(pos); Py_DECREF(result);
    return t;
fail:
    for (Py_ssize_t i = 0; seqs && i < nseq; i++) Py_XDECREF(seqs[i]);
    PyMem_Free(seqs); PyMem_Free(pos); Py_XDECREF(result);
    return NULL;
}

/* ---- PyType_Ready --------------------------------------------------------- */

#define COPYSLOT(base, tp, slot) if (!(tp)->slot && (base)->slot) (tp)->slot = (base)->slot

static void inherit_slots(PyTypeObject *tp, PyTypeObject *base) {
    if (tp->tp_as_number && base->tp_as_number) {
        PyNumberMethods *a = tp->tp_as_number, *b = base->tp_as_number;
#define CN(s) if (!a->s && b->s) a->s = b->s
        CN(nb_add); CN(nb_subtract); CN(nb_multiply); CN(nb_remainder); CN(nb_divmod); CN(nb_power); CN(nb_negative);
        CN(nb_positive); CN(nb_absolute); CN(nb_bool); CN(nb_invert); CN(nb_lshift); CN(nb_rshift); CN(nb_and); CN(nb_xor);
        CN(nb_or); CN(nb_int); CN(nb_float); CN(nb_inplace_add); CN(nb_inplace_subtract); CN(nb_inplace_multiply);
        CN(nb_inplace_remainder); CN(nb_inplace_power); CN(nb_inplace_lshift); CN(nb_inplace_rshift); CN(nb_inplace_and);
        CN(nb_inplace_xor); CN(nb_inplace_or); CN(nb_floor_divide); CN(nb_true_divide); CN(nb_inplace_floor_divide);
        CN(nb_inplace_true_divide); CN(nb_index); CN(nb_matrix_multiply); CN(nb_inplace_matrix_multiply);
#undef CN
    } else if (!tp->tp_as_number) tp->tp_as_number = base->tp_as_number;
    if (tp->tp_as_sequence && base->tp_as_sequence) {
        PySequenceMethods *a = tp->tp_as_sequence, *b = base->tp_as_sequence;
#define CS(s) if (!a->s && b->s) a->s = b->s
        CS(sq_length); CS(sq_concat); CS(sq_repeat); CS(sq_item); CS(sq_ass_item); CS(sq_contains); CS(sq_inplace_concat); CS(sq_inplace_repeat);
#undef CS
    } else if (!tp->tp_as_sequence) tp->tp_as_sequence = base->tp_as_sequence;
    if (tp->tp_as_mapping && base->tp_as_mapping) {
        PyMappingMethods *a = tp->tp_as_mapping, *b = base->tp_as_mapping;
        if (!a->mp_length && b->mp_length) a->mp_length = b->mp_length;
        if (!a->mp_subscript && b->mp_subscript) a->mp_subscript = b->mp_subscript;
        if (!a->mp_ass_subscript && b->mp_ass_subscript) a->mp_ass_subscript = b->mp_ass_subscript;
    } else if (!tp->tp_as_mapping) tp->tp_as_mapping = base->tp_as_mapping;
    if (tp->tp_as_async && base->tp_as_async) {
        PyAsyncMethods *a = tp->tp_as_async, *b = base->tp_as_async;
        if (!a->am_await && b->am_await) a->am_await = b->am_await;
        if (!a->am_aiter && b->am_aiter) a->am_aiter = b->am_aiter;
        if (!a->am_anext && b->am_anext) a->am_anext = b->am_anext;
    } else if (!tp->tp_as_async) tp->tp_as_async = base->tp_as_async;
    if (!tp->tp_as_buffer) tp->tp_as_buffer = base->tp_as_buffer;

    COPYSLOT(base, tp, tp_dealloc);
    if (!tp->tp_getattr && !tp->tp_getattro) { tp->tp_getattr = base->tp_getattr; tp->tp_getattro = base->tp_getattro; }
    if (!tp->tp_setattr && !tp->tp_setattro) { tp->tp_setattr = base->tp_setattr; tp->tp_setattro = base->tp_setattro; }
    COPYSLOT(base, tp, tp_repr);
    COPYSLOT(base, tp, tp_call);
    COPYSLOT(base, tp, tp_str);
    if (!tp->tp_richcompare && !tp->tp_hash) { tp->tp_richcompare = base->tp_richcompare; tp->tp_hash = base->tp_hash; }
    COPYSLOT(base, tp, tp_iter);
    COPYSLOT(base, tp, tp_iternext);
    COPYSLOT(base, tp, tp_descr_get);
    COPYSLOT(base, tp, tp_descr_set);
    COPYSLOT(base, tp, tp_dictoffset);
    COPYSLOT(base, tp, tp_init);
    COPYSLOT(base, tp, tp_alloc);
    COPYSLOT(base, tp, tp_new);
    COPYSLOT(base, tp, tp_free);
    COPYSLOT(base, tp, tp_is_gc);
    COPYSLOT(base, tp, tp_finalize);
    COPYSLOT(base, tp, tp_traverse);
    COPYSLOT(base, tp, tp_clear);
    if (!tp->tp_vectorcall_offset && (base->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL) && !(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        tp->tp_vectorcall_offset = base->tp_vectorcall_offset;
        tp->tp_flags |= Py_TPFLAGS_HAVE_VECTORCALL;
    }
    tp->tp_flags |= base->tp_flags & (Py_TPFLAGS_LONG_SUBCLASS | Py_TPFLAGS_LIST_SUBCLASS | Py_TPFLAGS_TUPLE_SUBCLASS |
        Py_TPFLAGS_BYTES_SUBCLASS | Py_TPFLAGS_UNICODE_SUBCLASS | Py_TPFLAGS_DICT_SUBCLASS | Py_TPFLAGS_BASE_EXC_SUBCLASS |
        Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_SEQUENCE | Py_TPFLAGS_MAPPING | Py_TPFLAGS_MATCH_SELF);
}

static int add_methods(PyTypeObject *tp, PyMethodDef *defs) {
    for (; defs && defs->ml_name; defs++) {
        PyObject *d;
        if (defs->ml_flags & METH_CLASS) d = PyDescr_NewClassMethod(tp, defs);
        else if (defs->ml_flags & METH_STATIC) { PyObject *f = PyCFunction_NewEx(defs, (PyObject *)tp, NULL); if (!f) return -1; d = PyStaticMethod_New(f); Py_DECREF(f); }
        else d = PyDescr_NewMethod(tp, defs);
        if (!d) return -1;
        int r = PyDict_SetItemString(tp->tp_dict, defs->ml_name, d);
        Py_DECREF(d);
        if (r < 0) return -1;
    }
    return 0;
}

static int add_members(PyTypeObject *tp, PyMemberDef *defs) {
    for (; defs && defs->name; defs++) {
        PyObject *d = PyDescr_NewMember(tp, defs);
        if (!d) return -1;
        int r = PyDict_SetItemString(tp->tp_dict, defs->name, d);
        Py_DECREF(d);
        if (r < 0) return -1;
    }
    return 0;
}

static int add_getsets(PyTypeObject *tp, PyGetSetDef *defs) {
    for (; defs && defs->name; defs++) {
        PyObject *d = PyDescr_NewGetSet(tp, defs);
        if (!d) return -1;
        int r = PyDict_SetItemString(tp->tp_dict, defs->name, d);
        Py_DECREF(d);
        if (r < 0) return -1;
    }
    return 0;
}

int PyType_Ready(PyTypeObject *tp) {
    if (tp->tp_flags & Py_TPFLAGS_READY) return 0;
    if (tp->tp_flags & Py_TPFLAGS_READYING) return 0;
    tp->tp_flags |= Py_TPFLAGS_READYING;
    if (!Py_TYPE(tp)) Py_SET_TYPE(tp, &PyType_Type);
    if (Py_REFCNT(tp) == 0) tp->ob_base.ob_base.ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT;
    PyTypeObject *base = tp->tp_base;
    if (!base && tp != &PyBaseObject_Type) base = tp->tp_base = &PyBaseObject_Type;
    if (base && !(base->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(base) < 0) goto fail;
    if (!tp->tp_bases) {
        tp->tp_bases = base ? PyTuple_Pack(1, (PyObject *)base) : PyTuple_New(0);
        if (!tp->tp_bases) goto fail;
    }
    if (!tp->tp_dict) { tp->tp_dict = PyDict_New(); if (!tp->tp_dict) goto fail; }
    if (add_methods(tp, tp->tp_methods) < 0 || add_members(tp, tp->tp_members) < 0 || add_getsets(tp, tp->tp_getset) < 0) goto fail;
    if (tp->tp_doc && !PyDict_GetItemString(tp->tp_dict, "__doc__")) {
        PyObject *doc = PyUnicode_FromString(tp->tp_doc);
        if (!doc) goto fail;
        PyDict_SetItemString(tp->tp_dict, "__doc__", doc);
        Py_DECREF(doc);
    } else if (!PyDict_GetItemString(tp->tp_dict, "__doc__")) {
        PyDict_SetItemString(tp->tp_dict, "__doc__", Py_None);
    }
    /* MRO */
    if (!tp->tp_mro) {
        if (PyTuple_GET_SIZE(tp->tp_bases) == 0) tp->tp_mro = PyTuple_Pack(1, (PyObject *)tp);
        else tp->tp_mro = mro_c3(tp);
        if (!tp->tp_mro) goto fail;
    }
    /* Inherit from every base in the MRO. */
    for (Py_ssize_t i = 1; i < PyTuple_GET_SIZE(tp->tp_mro); i++) inherit_slots(tp, (PyTypeObject *)PyTuple_GET_ITEM(tp->tp_mro, i));
    if (!tp->tp_alloc) tp->tp_alloc = PyType_GenericAlloc;
    if (!tp->tp_free) tp->tp_free = PyObject_Free;
    if (!tp->tp_getattro && !tp->tp_getattr) tp->tp_getattro = PyObject_GenericGetAttr;
    if (tp->tp_hash == NULL && tp->tp_richcompare != NULL && !PyDict_GetItemString(tp->tp_dict, "__hash__")) {
        /* __eq__ without __hash__ makes instances unhashable */
        tp->tp_hash = PyObject_HashNotImplemented;
        PyDict_SetItemString(tp->tp_dict, "__hash__", Py_None);
    }
    /* Subclass flags for the builtin fast checks. */
    if (PyType_IsSubtype(tp, &PyLong_Type)) tp->tp_flags |= Py_TPFLAGS_LONG_SUBCLASS;
    if (PyType_IsSubtype(tp, &PyList_Type)) tp->tp_flags |= Py_TPFLAGS_LIST_SUBCLASS;
    if (PyType_IsSubtype(tp, &PyTuple_Type)) tp->tp_flags |= Py_TPFLAGS_TUPLE_SUBCLASS;
    if (PyType_IsSubtype(tp, &PyBytes_Type)) tp->tp_flags |= Py_TPFLAGS_BYTES_SUBCLASS;
    if (PyType_IsSubtype(tp, &PyUnicode_Type)) tp->tp_flags |= Py_TPFLAGS_UNICODE_SUBCLASS;
    if (PyType_IsSubtype(tp, &PyDict_Type)) tp->tp_flags |= Py_TPFLAGS_DICT_SUBCLASS;
    if (PyType_IsSubtype(tp, &PyType_Type)) tp->tp_flags |= Py_TPFLAGS_TYPE_SUBCLASS;
    if (PyExc_BaseException && PyType_IsSubtype(tp, (PyTypeObject *)PyExc_BaseException)) tp->tp_flags |= Py_TPFLAGS_BASE_EXC_SUBCLASS;
    tp->tp_flags = (tp->tp_flags & ~Py_TPFLAGS_READYING) | Py_TPFLAGS_READY;
    return 0;
fail:
    tp->tp_flags &= ~Py_TPFLAGS_READYING;
    return -1;
}

/* ---- type as an object ---------------------------------------------------- */

static PyObject *type_repr(PyObject *self) {
    PyTypeObject *tp = (PyTypeObject *)self;
    PyObject *mod = tp->tp_dict ? PyDict_GetItemString(tp->tp_dict, "__module__") : NULL;
    if (mod && PyUnicode_Check(mod) && !PyUnicode_EqualToUTF8(mod, "builtins")) {
        PyObject *qn = PyType_GetQualName(tp);
        PyObject *r = PyUnicode_FromFormat("<class '%U.%U'>", mod, qn);
        Py_DECREF(qn);
        return r;
    }
    return PyUnicode_FromFormat("<class '%s'>", tp->tp_name);
}

static PyObject *type_getattro(PyObject *self, PyObject *name) {
    PyTypeObject *tp = (PyTypeObject *)self;
    PyTypeObject *meta = Py_TYPE(self);
    if (!(tp->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(tp) < 0) return NULL;
    PyObject *meta_attr = piper_type_lookup(meta, name);
    descrgetfunc meta_get = NULL;
    if (meta_attr) {
        Py_INCREF(meta_attr);
        meta_get = Py_TYPE(meta_attr)->tp_descr_get;
        if (meta_get && Py_TYPE(meta_attr)->tp_descr_set) {
            PyObject *r = meta_get(meta_attr, self, (PyObject *)meta);
            Py_DECREF(meta_attr);
            return r;
        }
    }
    PyObject *attr = piper_type_lookup(tp, name);
    if (attr) {
        Py_INCREF(attr);
        descrgetfunc local_get = Py_TYPE(attr)->tp_descr_get;
        Py_XDECREF(meta_attr);
        if (local_get) {
            PyObject *r = local_get(attr, NULL, self);
            Py_DECREF(attr);
            return r;
        }
        return attr;
    }
    if (meta_get) {
        PyObject *r = meta_get(meta_attr, self, (PyObject *)meta);
        Py_DECREF(meta_attr);
        return r;
    }
    if (meta_attr) return meta_attr;
    PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%U'", tp->tp_name, name);
    return NULL;
}

static int type_setattro(PyObject *self, PyObject *name, PyObject *v) {
    PyTypeObject *tp = (PyTypeObject *)self;
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_Format(PyExc_TypeError, "cannot set '%U' attribute of immutable type '%s'", name, tp->tp_name);
        return -1;
    }
    PyObject *meta_attr = piper_type_lookup(Py_TYPE(self), name);
    if (meta_attr && Py_TYPE(meta_attr)->tp_descr_set) return Py_TYPE(meta_attr)->tp_descr_set(meta_attr, self, v);
    int r = v ? PyDict_SetItem(tp->tp_dict, name, v) : PyDict_DelItem(tp->tp_dict, name);
    if (r < 0 && !v && PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_Clear();
        PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '%U'", tp->tp_name, name);
    }
    PyType_Modified(tp);
    /* Update slots derived from dunder methods. */
    extern void piper_type_update_slot(PyTypeObject *tp, PyObject *name);
    piper_type_update_slot(tp, name);
    return r;
}

PyObject *piper_type_call(PyObject *self, PyObject *args, PyObject *kwds) {
    PyTypeObject *tp = (PyTypeObject *)self;
    if (!(tp->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(tp) < 0) return NULL;
    if (tp == &PyType_Type && PyTuple_GET_SIZE(args) == 1 && (!kwds || !PyDict_Size(kwds))) return PyObject_Type(PyTuple_GET_ITEM(args, 0));
    if (!tp->tp_new) { PyErr_Format(PyExc_TypeError, "cannot create '%s' instances", tp->tp_name); return NULL; }
    PyObject *obj = tp->tp_new(tp, args, kwds);
    if (!obj) return NULL;
    if (!PyObject_TypeCheck(obj, tp)) return obj;
    tp = Py_TYPE(obj);
    if (tp->tp_init) {
        int r = tp->tp_init(obj, args, kwds);
        if (r < 0) { Py_DECREF(obj); return NULL; }
    }
    return obj;
}

static PyObject *type_vectorcall(PyObject *self, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    PyTypeObject *tp = (PyTypeObject *)self;
    if (tp == &PyType_Type && nargs == 1 && (!kwnames || !PyTuple_GET_SIZE(kwnames))) return PyObject_Type(args[0]);
    if (tp->tp_vectorcall && tp->tp_vectorcall != type_vectorcall) return tp->tp_vectorcall(self, args, nargsf, kwnames);
    PyObject *t = PyTuple_FromArray(args, nargs);
    if (!t) return NULL;
    PyObject *kw = NULL;
    if (kwnames && PyTuple_GET_SIZE(kwnames)) {
        kw = PyDict_New();
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) PyDict_SetItem(kw, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]);
    }
    PyObject *r = piper_type_call(self, t, kw);
    Py_DECREF(t);
    Py_XDECREF(kw);
    return r;
}

/* ---- creating classes ------------------------------------------------------ */

static void heaptype_dealloc(PyObject *self) {
    PyHeapTypeObject *ht = (PyHeapTypeObject *)self;
    PyTypeObject *tp = &ht->ht_type;
    Py_XDECREF(tp->tp_dict);
    Py_XDECREF(tp->tp_bases);
    Py_XDECREF(tp->tp_mro);
    Py_XDECREF(ht->ht_name);
    Py_XDECREF(ht->ht_qualname);
    Py_XDECREF(ht->ht_slots);
    Py_XDECREF(ht->ht_module);
    PyMem_Free((void *)tp->tp_name);
    Py_TYPE(self)->tp_free(self);
}

static void subtype_dealloc(PyObject *self) {
    PyTypeObject *tp = Py_TYPE(self);
    if (tp->tp_finalize) {
        PyObject *e = PyErr_GetRaisedException();
        tp->tp_finalize(self);
        PyErr_SetRaisedException(e);
    }
    /* __del__ */
    PyObject *del = piper_type_lookup(tp, piper_intern("__del__"));
    if (del) {
        PyObject *e = PyErr_GetRaisedException();
        self->ob_refcnt = 1;
        PyObject *r = PyObject_CallOneArg(del, self);
        if (!r) PyErr_WriteUnraisable(del); else Py_DECREF(r);
        PyErr_SetRaisedException(e);
        if (--self->ob_refcnt > 0) return; /* resurrected */
    }
    PyObject **dp = _PyObject_GetDictPtr(self);
    if (dp) Py_CLEAR(*dp);
    /* slots */
    PyTypeObject *base = tp;
    while (base && (base->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyObject *slots = ((PyHeapTypeObject *)base)->ht_slots;
        if (slots) {
            PyMemberDef *m = base->tp_members;
            for (; m && m->name; m++) {
                if (m->type == Py_T_OBJECT_EX) { PyObject **p = (PyObject **)((char *)self + m->offset); Py_CLEAR(*p); }
            }
        }
        base = base->tp_base;
    }
    /* find the first non-heap base dealloc */
    base = tp;
    while (base->tp_dealloc == subtype_dealloc) base = base->tp_base;
    if (base->tp_dealloc) base->tp_dealloc(self);
    else tp->tp_free(self);
    Py_DECREF(tp);
}

static int type_has_extra_layout(PyTypeObject *type, PyTypeObject *base) {
    if (type->tp_itemsize || base->tp_itemsize) return type->tp_basicsize != base->tp_basicsize || type->tp_itemsize != base->tp_itemsize;
    Py_ssize_t size = type->tp_basicsize;
    if (type->tp_weaklistoffset && !base->tp_weaklistoffset && type->tp_weaklistoffset + (Py_ssize_t)sizeof(PyObject *) == size) size -= (Py_ssize_t)sizeof(PyObject *);
    if (type->tp_dictoffset > 0 && !base->tp_dictoffset && type->tp_dictoffset + (Py_ssize_t)sizeof(PyObject *) == size) size -= (Py_ssize_t)sizeof(PyObject *);
    return size != base->tp_basicsize;
}

static PyTypeObject *solid_base(PyTypeObject *type) {
    while (type->tp_base && !type_has_extra_layout(type, type->tp_base)) type = type->tp_base;
    return type;
}

static PyTypeObject *best_base(PyObject *bases) {
    PyTypeObject *winner = NULL, *winner_solid = NULL;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        PyObject *b = PyTuple_GET_ITEM(bases, i);
        if (!PyType_Check(b)) { PyErr_SetString(PyExc_TypeError, "bases must be types"); return NULL; }
        PyTypeObject *t = (PyTypeObject *)b;
        if (!(t->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(t) < 0) return NULL;
        if (!(t->tp_flags & Py_TPFLAGS_BASETYPE)) { PyErr_Format(PyExc_TypeError, "type '%s' is not an acceptable base type", t->tp_name); return NULL; }
        PyTypeObject *candidate = solid_base(t);
        if (!winner) { winner = t; winner_solid = candidate; }
        else if (PyType_IsSubtype(winner_solid, candidate)) { }
        else if (PyType_IsSubtype(candidate, winner_solid)) { winner = t; winner_solid = candidate; }
        else { PyErr_SetString(PyExc_TypeError, "multiple bases have instance lay-out conflict"); return NULL; }
    }
    return winner;
}

/* slot wrappers: dunder methods in the class dict drive the C slots */
extern void piper_type_fixup_slots(PyTypeObject *tp);

PyObject *PyType_New3(const char *cname, PyObject *bases, PyObject *dict) {
    PyObject *name = PyUnicode_FromString(cname);
    if (!name) return NULL;
    PyObject *args = PyTuple_Pack(3, name, bases, dict);
    Py_DECREF(name);
    if (!args) return NULL;
    PyObject *r = piper_type_call((PyObject *)&PyType_Type, args, NULL);
    Py_DECREF(args);
    return r;
}

static PyObject *type_new(PyTypeObject *metatype, PyObject *args, PyObject *kwds) {
    if (PyTuple_GET_SIZE(args) != 3) {
        if (PyTuple_GET_SIZE(args) == 1 && (!kwds || !PyDict_Size(kwds))) return PyObject_Type(PyTuple_GET_ITEM(args, 0));
        PyErr_SetString(PyExc_TypeError, "type() takes 1 or 3 arguments");
        return NULL;
    }
    PyObject *name = PyTuple_GET_ITEM(args, 0), *bases = PyTuple_GET_ITEM(args, 1), *orig_dict = PyTuple_GET_ITEM(args, 2);
    if (!PyUnicode_Check(name)) { PyErr_Format(PyExc_TypeError, "type.__new__() argument 1 must be str, not %s", Py_TYPE(name)->tp_name); return NULL; }
    if (!PyTuple_Check(bases)) { PyErr_Format(PyExc_TypeError, "type.__new__() argument 2 must be tuple, not %s", Py_TYPE(bases)->tp_name); return NULL; }
    if (!PyDict_Check(orig_dict)) { PyErr_Format(PyExc_TypeError, "type.__new__() argument 3 must be dict, not %s", Py_TYPE(orig_dict)->tp_name); return NULL; }
    /* metaclass calculation */
    PyTypeObject *winner = metatype;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        PyTypeObject *bm = Py_TYPE(PyTuple_GET_ITEM(bases, i));
        if (PyType_IsSubtype(winner, bm)) continue;
        if (PyType_IsSubtype(bm, winner)) { winner = bm; continue; }
        PyErr_SetString(PyExc_TypeError, "metaclass conflict: the metaclass of a derived class must be a (non-strict) subclass of the metaclasses of all its bases");
        return NULL;
    }
    if (winner != metatype && winner->tp_new != type_new) return winner->tp_new(winner, args, kwds);
    metatype = winner;
    if (PyTuple_GET_SIZE(bases) == 0) { bases = PyTuple_Pack(1, (PyObject *)&PyBaseObject_Type); if (!bases) return NULL; } else Py_INCREF(bases);
    PyTypeObject *base = best_base(bases);
    if (!base) { Py_DECREF(bases); return NULL; }
    PyObject *dict = PyDict_Copy(orig_dict);
    if (!dict) { Py_DECREF(bases); return NULL; }

    /* __slots__ */
    PyObject *slots = PyDict_GetItemString(dict, "__slots__");
    Py_ssize_t nslots = 0;
    int add_dict = 0, add_weak = 0;
    PyObject *slot_names = NULL;
    if (slots) {
        if (PyUnicode_Check(slots)) slot_names = PyTuple_Pack(1, slots);
        else slot_names = PySequence_Tuple(slots);
        if (!slot_names) goto fail;
        PyObject *filtered = PyList_New(0);
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(slot_names); i++) {
            PyObject *s = PyTuple_GET_ITEM(slot_names, i);
            if (!PyUnicode_Check(s)) { PyErr_SetString(PyExc_TypeError, "__slots__ items must be strings"); Py_DECREF(filtered); goto fail; }
            if (PyUnicode_EqualToUTF8(s, "__dict__")) { add_dict = 1; continue; }
            if (PyUnicode_EqualToUTF8(s, "__weakref__")) { add_weak = 1; continue; }
            PyList_Append(filtered, s);
        }
        Py_SETREF(slot_names, PyList_AsTuple(filtered));
        Py_DECREF(filtered);
        nslots = PyTuple_GET_SIZE(slot_names);
    } else {
        add_dict = base->tp_dictoffset == 0;
        add_weak = 1;
    }

    PyHeapTypeObject *ht = (PyHeapTypeObject *)metatype->tp_alloc(metatype, nslots);
    if (!ht) goto fail;
    PyTypeObject *tp = &ht->ht_type;
    ht->ht_name = Py_NewRef(name);
    PyObject *qn = PyDict_GetItemString(dict, "__qualname__");
    if (qn) { if (!PyUnicode_Check(qn)) { PyErr_Format(PyExc_TypeError, "type __qualname__ must be a str, not %s", Py_TYPE(qn)->tp_name); Py_DECREF(ht); goto fail; } ht->ht_qualname = Py_NewRef(qn); PyDict_DelItemString(dict, "__qualname__"); }
    else ht->ht_qualname = Py_NewRef(name);
    ht->ht_slots = slot_names;
    slot_names = NULL;
    Py_ssize_t nlen;
    const char *ns = PyUnicode_AsUTF8AndSize(name, &nlen);
    char *tpname = PyMem_Malloc((size_t)nlen + 1);
    memcpy(tpname, ns, (size_t)nlen + 1);
    tp->tp_name = tpname;
    tp->tp_flags = Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC;
    tp->tp_as_async = &ht->as_async;
    tp->tp_as_number = &ht->as_number;
    tp->tp_as_sequence = &ht->as_sequence;
    tp->tp_as_mapping = &ht->as_mapping;
    tp->tp_as_buffer = &ht->as_buffer;
    tp->tp_bases = bases;
    bases = NULL;
    tp->tp_base = base;
    Py_INCREF(base);
    tp->tp_dict = dict;
    dict = NULL;
    tp->tp_dealloc = subtype_dealloc;
    tp->tp_alloc = PyType_GenericAlloc;
    tp->tp_free = PyObject_Free;
    tp->tp_basicsize = base->tp_basicsize;
    tp->tp_itemsize = base->tp_itemsize;
    tp->tp_dictoffset = base->tp_dictoffset;
    tp->tp_weaklistoffset = base->tp_weaklistoffset;
    /* slot members */
    if (nslots) {
        PyMemberDef *members = PyMem_Calloc((size_t)nslots + 1, sizeof(PyMemberDef));
        Py_ssize_t off = tp->tp_basicsize;
        for (Py_ssize_t i = 0; i < nslots; i++) {
            PyObject *s = PyTuple_GET_ITEM(ht->ht_slots, i);
            members[i].name = PyUnicode_AsUTF8(s);
            members[i].type = Py_T_OBJECT_EX;
            members[i].offset = off;
            off += (Py_ssize_t)sizeof(PyObject *);
        }
        tp->tp_basicsize = off;
        tp->tp_members = members;
    }
    if (add_dict && tp->tp_dictoffset == 0) {
        if (tp->tp_itemsize) tp->tp_dictoffset = -(Py_ssize_t)sizeof(PyObject *);
        else tp->tp_dictoffset = tp->tp_basicsize;
        tp->tp_basicsize += (Py_ssize_t)sizeof(PyObject *);
    }
    if (add_weak && tp->tp_weaklistoffset == 0 && !tp->tp_itemsize) {
        tp->tp_weaklistoffset = tp->tp_basicsize;
        tp->tp_basicsize += (Py_ssize_t)sizeof(PyObject *);
    }
    /* __module__ */
    if (!PyDict_GetItemString(tp->tp_dict, "__module__")) {
        PyObject *g = piper_frame_globals();
        PyObject *modname = g ? PyDict_GetItemString(g, "__name__") : NULL;
        if (modname) PyDict_SetItemString(tp->tp_dict, "__module__", modname);
    }
    /* __new__ in the dict becomes a static method */
    PyObject *newfn = PyDict_GetItemString(tp->tp_dict, "__new__");
    if (newfn && Py_IS_TYPE(newfn, &PyFunction_Type)) {
        PyObject *sm = PyStaticMethod_New(newfn);
        PyDict_SetItemString(tp->tp_dict, "__new__", sm);
        Py_DECREF(sm);
    }
    PyObject *initsub = PyDict_GetItemString(tp->tp_dict, "__init_subclass__");
    if (initsub && Py_IS_TYPE(initsub, &PyFunction_Type)) {
        PyObject *cm = PyClassMethod_New(initsub);
        PyDict_SetItemString(tp->tp_dict, "__init_subclass__", cm);
        Py_DECREF(cm);
    }
    PyObject *cgi = PyDict_GetItemString(tp->tp_dict, "__class_getitem__");
    if (cgi && Py_IS_TYPE(cgi, &PyFunction_Type)) {
        PyObject *cm = PyClassMethod_New(cgi);
        PyDict_SetItemString(tp->tp_dict, "__class_getitem__", cm);
        Py_DECREF(cm);
    }
    if (PyType_Ready(tp) < 0) { Py_DECREF(ht); return NULL; }
    piper_type_fixup_slots(tp);
    /* __set_name__ */
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    PyObject *items = PyDict_Items(tp->tp_dict);
    for (Py_ssize_t i = 0; items && i < PyList_GET_SIZE(items); i++) {
        PyObject *kv = PyList_GET_ITEM(items, i);
        k = PyTuple_GET_ITEM(kv, 0); v = PyTuple_GET_ITEM(kv, 1);
        PyObject *sn;
        if (PyObject_GetOptionalAttrString(v, "__set_name__", &sn) < 0) { Py_DECREF(items); Py_DECREF(ht); return NULL; }
        if (sn) {
            PyObject *a[2] = { (PyObject *)tp, k };
            PyObject *r = PyObject_Vectorcall(sn, a, 2, NULL);
            Py_DECREF(sn);
            if (!r) { Py_DECREF(items); Py_DECREF(ht); return NULL; }
            Py_DECREF(r);
        }
    }
    Py_XDECREF(items);
    PIPER_UNUSED(pos);
    /* __init_subclass__ on the parent */
    PyObject *super = piper_super_new(tp, (PyObject *)tp);
    if (super) {
        PyObject *is = PyObject_GetAttrString(super, "__init_subclass__");
        Py_DECREF(super);
        if (is) {
            PyObject *r = PyObject_VectorcallDict(is, NULL, 0, kwds);
            Py_DECREF(is);
            if (!r) { Py_DECREF(ht); return NULL; }
            Py_DECREF(r);
        } else PyErr_Clear();
    } else PyErr_Clear();
    return (PyObject *)tp;
fail:
    Py_XDECREF(bases);
    Py_XDECREF(dict);
    Py_XDECREF(slot_names);
    return NULL;
}

static int type_init(PyObject *self, PyObject *args, PyObject *kwds) { PIPER_UNUSED(self); PIPER_UNUSED(args); PIPER_UNUSED(kwds); return 0; }

static PyObject *type_name_get(PyObject *self, void *c) { PIPER_UNUSED(c); return PyType_GetName((PyTypeObject *)self); }
static PyObject *type_qualname_get(PyObject *self, void *c) { PIPER_UNUSED(c); return PyType_GetQualName((PyTypeObject *)self); }
static PyObject *type_bases_get(PyObject *self, void *c) { PIPER_UNUSED(c); PyTypeObject *tp = (PyTypeObject *)self; if (!tp->tp_bases && PyType_Ready(tp) < 0) return NULL; return Py_NewRef(tp->tp_bases); }
static PyObject *type_base_get(PyObject *self, void *c) { PIPER_UNUSED(c); PyTypeObject *tp = (PyTypeObject *)self; return Py_NewRef(tp->tp_base ? (PyObject *)tp->tp_base : Py_None); }
static PyObject *type_mro_get(PyObject *self, void *c) { PIPER_UNUSED(c); PyTypeObject *tp = (PyTypeObject *)self; if (!tp->tp_mro && PyType_Ready(tp) < 0) return NULL; return Py_NewRef(tp->tp_mro); }
static PyObject *type_dict_get(PyObject *self, void *c) { PIPER_UNUSED(c); PyTypeObject *tp = (PyTypeObject *)self; if (!tp->tp_dict && PyType_Ready(tp) < 0) return NULL; return PyDictProxy_New(tp->tp_dict); }
static PyObject *type_module_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    PyTypeObject *tp = (PyTypeObject *)self;
    if (tp->tp_dict) { PyObject *m = PyDict_GetItemString(tp->tp_dict, "__module__"); if (m) return Py_NewRef(m); }
    const char *dot = strrchr(tp->tp_name, '.');
    if (dot) return PyUnicode_FromStringAndSize(tp->tp_name, dot - tp->tp_name);
    return PyUnicode_FromString("builtins");
}
static int type_module_set(PyObject *self, PyObject *v, void *c) { PIPER_UNUSED(c); return PyDict_SetItemString(((PyTypeObject *)self)->tp_dict, "__module__", v); }
static PyObject *type_doc_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    PyTypeObject *tp = (PyTypeObject *)self;
    PyObject *d = tp->tp_dict ? PyDict_GetItemString(tp->tp_dict, "__doc__") : NULL;
    if (!d) Py_RETURN_NONE;
    if (Py_TYPE(d)->tp_descr_get && !(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) return Py_TYPE(d)->tp_descr_get(d, NULL, self);
    return Py_NewRef(d);
}
static int type_doc_set(PyObject *self, PyObject *v, void *c) { PIPER_UNUSED(c); return PyDict_SetItemString(((PyTypeObject *)self)->tp_dict, "__doc__", v ? v : Py_None); }
static PyObject *type_abstractmethods_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    PyObject *r = PyDict_GetItemString(((PyTypeObject *)self)->tp_dict, "__abstractmethods__");
    if (!r) { PyErr_SetString(PyExc_AttributeError, "__abstractmethods__"); return NULL; }
    return Py_NewRef(r);
}
static int type_abstractmethods_set(PyObject *self, PyObject *v, void *c) {
    PIPER_UNUSED(c);
    PyTypeObject *tp = (PyTypeObject *)self;
    int r = v ? PyDict_SetItemString(tp->tp_dict, "__abstractmethods__", v) : PyDict_DelItemString(tp->tp_dict, "__abstractmethods__");
    if (r == 0) {
        int abstract = v ? PyObject_IsTrue(v) : 0;
        if (abstract < 0) return -1;
        if (abstract) tp->tp_flags |= Py_TPFLAGS_IS_ABSTRACT; else tp->tp_flags &= ~Py_TPFLAGS_IS_ABSTRACT;
    }
    return r;
}
static PyObject *type_annotations_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    PyTypeObject *tp = (PyTypeObject *)self;
    PyObject *a = PyDict_GetItemString(tp->tp_dict, "__annotations__");
    if (a) return Py_NewRef(a);
    PyObject *annotate = PyDict_GetItemString(tp->tp_dict, "__annotate__");
    if (annotate && annotate != Py_None) {
        PyObject *one = PyLong_FromLong(1);
        PyObject *r = PyObject_CallOneArg(annotate, one);
        Py_DECREF(one);
        if (r) PyDict_SetItemString(tp->tp_dict, "__annotations__", r);
        return r;
    }
    if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) { PyErr_Format(PyExc_AttributeError, "type object '%s' has no attribute '__annotations__'", tp->tp_name); return NULL; }
    a = PyDict_New();
    if (a) PyDict_SetItemString(tp->tp_dict, "__annotations__", a);
    return a;
}
static int type_annotations_set(PyObject *self, PyObject *v, void *c) { PIPER_UNUSED(c); return v ? PyDict_SetItemString(((PyTypeObject *)self)->tp_dict, "__annotations__", v) : PyDict_DelItemString(((PyTypeObject *)self)->tp_dict, "__annotations__"); }
static PyObject *type_annotate_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    PyObject *a = PyDict_GetItemString(((PyTypeObject *)self)->tp_dict, "__annotate__");
    return Py_NewRef(a ? a : Py_None);
}
static int type_annotate_set(PyObject *self, PyObject *v, void *c) { PIPER_UNUSED(c); return PyDict_SetItemString(((PyTypeObject *)self)->tp_dict, "__annotate__", v ? v : Py_None); }
static PyObject *type_basicsize_get(PyObject *self, void *c) { PIPER_UNUSED(c); return PyLong_FromSsize_t(((PyTypeObject *)self)->tp_basicsize); }
static PyObject *type_flags_get(PyObject *self, void *c) { PIPER_UNUSED(c); return PyLong_FromUnsignedLong(((PyTypeObject *)self)->tp_flags); }
static PyObject *type_text_signature_get(PyObject *self, void *c) { PIPER_UNUSED(self); PIPER_UNUSED(c); Py_RETURN_NONE; }
static PyObject *type_type_params_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    PyObject *p = PyDict_GetItemString(((PyTypeObject *)self)->tp_dict, "__type_params__");
    return p ? Py_NewRef(p) : PyTuple_New(0);
}
static int type_type_params_set(PyObject *self, PyObject *v, void *c) { PIPER_UNUSED(c); return PyDict_SetItemString(((PyTypeObject *)self)->tp_dict, "__type_params__", v); }

static PyGetSetDef type_getsets[] = {
    { "__name__", type_name_get, NULL, NULL, NULL },
    { "__qualname__", type_qualname_get, NULL, NULL, NULL },
    { "__bases__", type_bases_get, NULL, NULL, NULL },
    { "__base__", type_base_get, NULL, NULL, NULL },
    { "__mro__", type_mro_get, NULL, NULL, NULL },
    { "__dict__", type_dict_get, NULL, NULL, NULL },
    { "__module__", type_module_get, type_module_set, NULL, NULL },
    { "__doc__", type_doc_get, type_doc_set, NULL, NULL },
    { "__abstractmethods__", type_abstractmethods_get, type_abstractmethods_set, NULL, NULL },
    { "__annotations__", type_annotations_get, type_annotations_set, NULL, NULL },
    { "__annotate__", type_annotate_get, type_annotate_set, NULL, NULL },
    { "__basicsize__", type_basicsize_get, NULL, NULL, NULL },
    { "__flags__", type_flags_get, NULL, NULL, NULL },
    { "__text_signature__", type_text_signature_get, NULL, NULL, NULL },
    { "__type_params__", type_type_params_get, type_type_params_set, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};

static PyObject *type_mro_method(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); PyTypeObject *tp = (PyTypeObject *)self; PyObject *m = mro_c3(tp); if (!m) return NULL; PyObject *l = PySequence_List(m); Py_DECREF(m); return l; }
static PyObject *type_subclasses(PyObject *self, PyObject *unused) { PIPER_UNUSED(self); PIPER_UNUSED(unused); return PyList_New(0); }
static PyObject *type_prepare(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) { PIPER_UNUSED(self); PIPER_UNUSED(args); PIPER_UNUSED(nargs); PIPER_UNUSED(kwnames); return PyDict_New(); }
static PyObject *type_instancecheck(PyObject *self, PyObject *inst) {
    if (!PyType_Check(self)) { PyErr_SetString(PyExc_TypeError, "__instancecheck__ requires a type"); return NULL; }
    Py_RETURN_BOOL(PyObject_TypeCheck(inst, (PyTypeObject *)self));
}
static PyObject *type_subclasscheck(PyObject *self, PyObject *sub) {
    if (!PyType_Check(self) || !PyType_Check(sub)) { PyErr_SetString(PyExc_TypeError, "issubclass() arg 1 must be a class"); return NULL; }
    Py_RETURN_BOOL(PyType_IsSubtype((PyTypeObject *)sub, (PyTypeObject *)self));
}
static PyObject *type_dir(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    PyObject *d = PyDict_New();
    PyObject *mro = ((PyTypeObject *)self)->tp_mro;
    for (Py_ssize_t i = 0; mro && i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject *t = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if (t->tp_dict) PyDict_Update(d, t->tp_dict);
    }
    PyObject *keys = PyDict_Keys(d);
    Py_DECREF(d);
    return keys;
}
static PyObject *type_sizeof(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return PyLong_FromSsize_t(Py_TYPE(self)->tp_basicsize); }
static PyObject *type_or(PyObject *a, PyObject *b) {
    extern PyObject *piper_union_type_new(PyObject *a, PyObject *b);
    return piper_union_type_new(a, b);
}

static PyMethodDef type_methods[] = {
    { "mro", type_mro_method, METH_NOARGS, NULL },
    { "__subclasses__", type_subclasses, METH_NOARGS, NULL },
    { "__prepare__", (PyCFunction)(void (*)(void))type_prepare, METH_FASTCALL | METH_KEYWORDS | METH_CLASS, NULL },
    { "__instancecheck__", type_instancecheck, METH_O, NULL },
    { "__subclasscheck__", type_subclasscheck, METH_O, NULL },
    { "__dir__", type_dir, METH_NOARGS, NULL },
    { "__sizeof__", type_sizeof, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyNumberMethods type_as_number = { .nb_or = type_or };

PyTypeObject PyType_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "type",
    .tp_basicsize = sizeof(PyHeapTypeObject),
    .tp_itemsize = sizeof(PyMemberDef),
    .tp_dealloc = heaptype_dealloc,
    .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
    .tp_repr = type_repr,
    .tp_as_number = &type_as_number,
    .tp_call = piper_type_call,
    .tp_getattro = type_getattro,
    .tp_setattro = type_setattro,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_methods = type_methods,
    .tp_getset = type_getsets,
    .tp_base = &PyBaseObject_Type,
    .tp_dictoffset = offsetof(PyTypeObject, tp_dict),
    .tp_init = type_init,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = type_new,
    .tp_free = PyObject_Free,
    .tp_vectorcall = type_vectorcall,
};
