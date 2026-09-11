/* Builtin functions, method/member/getset descriptors, bound methods,
 * property, staticmethod, classmethod, super, and generic aliases. */
#include "internal.h"

/* ---- dispatch of a PyMethodDef with (self, args, nargs, kwnames) --------- */

static PyObject *call_methoddef(PyMethodDef *ml, PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    int flags = ml->ml_flags & ~(METH_CLASS | METH_STATIC | METH_COEXIST);
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    switch (flags) {
    case METH_NOARGS:
        if (nkw) { PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", ml->ml_name); return NULL; }
        if (nargs != 0) { PyErr_Format(PyExc_TypeError, "%s() takes no arguments (%zd given)", ml->ml_name, nargs); return NULL; }
        return ml->ml_meth(self, NULL);
    case METH_O:
        if (nkw) { PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", ml->ml_name); return NULL; }
        if (nargs != 1) { PyErr_Format(PyExc_TypeError, "%s() takes exactly one argument (%zd given)", ml->ml_name, nargs); return NULL; }
        return ml->ml_meth(self, args[0]);
    case METH_FASTCALL:
        if (nkw) { PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", ml->ml_name); return NULL; }
        return ((PyCFunctionFast)(void (*)(void))ml->ml_meth)(self, args, nargs);
    case METH_FASTCALL | METH_KEYWORDS:
        return ((PyCFunctionFastWithKeywords)(void (*)(void))ml->ml_meth)(self, args, nargs, kwnames);
    case METH_VARARGS:
    case METH_VARARGS | METH_KEYWORDS: {
        PyObject *t = PyTuple_FromArray(args, nargs);
        if (!t) return NULL;
        PyObject *kw = NULL;
        if (nkw) {
            if (!(flags & METH_KEYWORDS)) { Py_DECREF(t); PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", ml->ml_name); return NULL; }
            kw = PyDict_New();
            for (Py_ssize_t i = 0; i < nkw; i++) PyDict_SetItem(kw, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]);
        }
        PyObject *r = (flags & METH_KEYWORDS) ? ((PyCFunctionWithKeywords)(void (*)(void))ml->ml_meth)(self, t, kw) : ml->ml_meth(self, t);
        Py_DECREF(t); Py_XDECREF(kw);
        return r;
    }
    case METH_METHOD | METH_FASTCALL | METH_KEYWORDS: {
        typedef PyObject *(*PyCMethod)(PyObject *, PyTypeObject *, PyObject *const *, size_t, PyObject *);
        return ((PyCMethod)(void (*)(void))ml->ml_meth)(self, Py_TYPE(self), args, (size_t)nargs, kwnames);
    }
    default:
        PyErr_Format(PyExc_SystemError, "bad call flags for %s", ml->ml_name);
        return NULL;
    }
}

/* ---- builtin_function_or_method ---------------------------------------------- */

static PyObject *cfunction_vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyCFunctionObject *cf = (PyCFunctionObject *)f;
    return call_methoddef(cf->m_ml, cf->m_self, args, PyVectorcall_NARGS(nargsf), kwnames);
}
static void cfunction_dealloc(PyObject *o) { PyCFunctionObject *cf = (PyCFunctionObject *)o; Py_XDECREF(cf->m_self); Py_XDECREF(cf->m_module); PyObject_Free(o); }
static PyObject *cfunction_repr(PyObject *o) {
    PyCFunctionObject *cf = (PyCFunctionObject *)o;
    if (!cf->m_self || PyModule_Check(cf->m_self)) return PyUnicode_FromFormat("<built-in function %s>", cf->m_ml->ml_name);
    return PyUnicode_FromFormat("<built-in method %s of %s object at %p>", cf->m_ml->ml_name, Py_TYPE(cf->m_self)->tp_name, cf->m_self);
}
static PyObject *cfunction_name(PyObject *o, void *c) { PIPER_UNUSED(c); return PyUnicode_FromString(((PyCFunctionObject *)o)->m_ml->ml_name); }
static PyObject *cfunction_qualname(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    PyCFunctionObject *cf = (PyCFunctionObject *)o;
    if (!cf->m_self || PyModule_Check(cf->m_self)) return PyUnicode_FromString(cf->m_ml->ml_name);
    PyObject *tn = PyType_Check(cf->m_self) ? PyType_GetQualName((PyTypeObject *)cf->m_self) : PyType_GetQualName(Py_TYPE(cf->m_self));
    PyObject *r = PyUnicode_FromFormat("%U.%s", tn, cf->m_ml->ml_name);
    Py_DECREF(tn);
    return r;
}
static PyObject *cfunction_doc(PyObject *o, void *c) { PIPER_UNUSED(c); const char *d = ((PyCFunctionObject *)o)->m_ml->ml_doc; return d ? PyUnicode_FromString(d) : Py_NewRef(Py_None); }
static PyObject *cfunction_self(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *s = ((PyCFunctionObject *)o)->m_self; return Py_NewRef(s ? s : Py_None); }
static PyObject *cfunction_module(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *m = ((PyCFunctionObject *)o)->m_module; return Py_NewRef(m ? m : Py_None); }
static PyObject *cfunction_text_signature(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); Py_RETURN_NONE; }
static PyGetSetDef cfunction_getsets[] = {
    { "__name__", cfunction_name, NULL, NULL, NULL }, { "__qualname__", cfunction_qualname, NULL, NULL, NULL }, { "__doc__", cfunction_doc, NULL, NULL, NULL },
    { "__self__", cfunction_self, NULL, NULL, NULL }, { "__module__", cfunction_module, NULL, NULL, NULL }, { "__text_signature__", cfunction_text_signature, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};
static PyObject *cfunction_richcompare(PyObject *a, PyObject *b, int op) {
    if ((op != Py_EQ && op != Py_NE) || !PyCFunction_Check(b)) Py_RETURN_NOTIMPLEMENTED;
    PyCFunctionObject *x = (PyCFunctionObject *)a, *y = (PyCFunctionObject *)b;
    int eq = x->m_ml == y->m_ml && x->m_self == y->m_self;
    Py_RETURN_BOOL(op == Py_EQ ? eq : !eq);
}
static Py_hash_t cfunction_hash(PyObject *o) { PyCFunctionObject *cf = (PyCFunctionObject *)o; Py_hash_t x = _Py_HashPointer(cf->m_self), y = _Py_HashPointer((void *)cf->m_ml->ml_meth); x ^= y; return x == -1 ? -2 : x; }
static PyObject *cfunction_reduce(PyObject *o, PyObject *u) { PIPER_UNUSED(u); return cfunction_name(o, NULL); }
static PyMethodDef cfunction_methods[] = { { "__reduce__", cfunction_reduce, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyCFunction_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "builtin_function_or_method", .tp_basicsize = sizeof(PyCFunctionObject), .tp_dealloc = cfunction_dealloc,
    .tp_vectorcall_offset = offsetof(PyCFunctionObject, vectorcall), .tp_repr = cfunction_repr, .tp_hash = cfunction_hash,
    .tp_call = PyVectorcall_Call, .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_richcompare = cfunction_richcompare, .tp_methods = cfunction_methods, .tp_getset = cfunction_getsets,
};
PyObject *PyCFunction_NewEx(PyMethodDef *ml, PyObject *self, PyObject *module) {
    PyCFunctionObject *f = PyObject_New(PyCFunctionObject, &PyCFunction_Type);
    if (!f) return NULL;
    f->m_ml = ml;
    f->m_self = Py_XNewRef(self);
    f->m_module = Py_XNewRef(module);
    f->m_weakreflist = NULL;
    f->vectorcall = cfunction_vectorcall;
    return (PyObject *)f;
}
PyObject *PyCFunction_New(PyMethodDef *ml, PyObject *self) { return PyCFunction_NewEx(ml, self, NULL); }
PyObject *PyCMethod_New(PyMethodDef *ml, PyObject *self, PyObject *module, PyTypeObject *cls) { PIPER_UNUSED(cls); return PyCFunction_NewEx(ml, self, module); }

/* ---- descriptors --------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyTypeObject *d_type; PyObject *d_name; PyObject *d_qualname; } PyDescrObject;
typedef struct { PyDescrObject d_common; PyMethodDef *d_method; vectorcallfunc vectorcall; } PyMethodDescrObject;
typedef struct { PyDescrObject d_common; PyMemberDef *d_member; } PyMemberDescrObject;
typedef struct { PyDescrObject d_common; PyGetSetDef *d_getset; } PyGetSetDescrObject;

static void descr_dealloc(PyObject *o) { PyDescrObject *d = (PyDescrObject *)o; Py_XDECREF(d->d_type); Py_XDECREF(d->d_name); Py_XDECREF(d->d_qualname); PyObject_Free(o); }
static int descr_check(PyDescrObject *d, PyObject *obj) {
    if (!PyObject_TypeCheck(obj, d->d_type)) {
        PyErr_Format(PyExc_TypeError, "descriptor '%U' for '%s' objects doesn't apply to a '%s' object", d->d_name, d->d_type->tp_name, Py_TYPE(obj)->tp_name);
        return -1;
    }
    return 0;
}
static PyObject *descr_name(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((PyDescrObject *)o)->d_name); }
static PyObject *descr_qualname(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    PyDescrObject *d = (PyDescrObject *)o;
    if (!d->d_qualname) { PyObject *tn = PyType_GetQualName(d->d_type); d->d_qualname = PyUnicode_FromFormat("%U.%U", tn, d->d_name); Py_DECREF(tn); }
    return Py_NewRef(d->d_qualname);
}
static PyObject *descr_objclass(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef((PyObject *)((PyDescrObject *)o)->d_type); }
static PyObject *descr_reduce(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyDescrObject *d = (PyDescrObject *)o; PyObject *b = piper_builtins(); PyObject *ga = PyDict_GetItemString(b, "getattr"); PyObject *args = PyTuple_Pack(2, (PyObject *)d->d_type, d->d_name); PyObject *r = PyTuple_Pack(2, ga, args); Py_DECREF(args); return r; }

static PyObject *method_get(PyObject *self, PyObject *obj, PyObject *type) {
    PIPER_UNUSED(type);
    if (!obj) return Py_NewRef(self);
    if (descr_check((PyDescrObject *)self, obj) < 0) return NULL;
    return piper_bound_method_new(self, obj);
}
static PyObject *method_vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyMethodDescrObject *d = (PyMethodDescrObject *)f;
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs < 1) { PyErr_Format(PyExc_TypeError, "unbound method %U() needs an argument", d->d_common.d_name); return NULL; }
    if (descr_check(&d->d_common, args[0]) < 0) return NULL;
    return call_methoddef(d->d_method, args[0], args + 1, nargs - 1, kwnames);
}
static PyObject *method_repr(PyObject *o) { PyDescrObject *d = (PyDescrObject *)o; return PyUnicode_FromFormat("<method '%U' of '%s' objects>", d->d_name, d->d_type->tp_name); }
static PyObject *method_doc(PyObject *o, void *c) { PIPER_UNUSED(c); const char *doc = ((PyMethodDescrObject *)o)->d_method->ml_doc; return doc ? PyUnicode_FromString(doc) : Py_NewRef(Py_None); }
static PyObject *method_text_signature(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); Py_RETURN_NONE; }
static PyGetSetDef method_getsets[] = { { "__name__", descr_name, NULL, NULL, NULL }, { "__qualname__", descr_qualname, NULL, NULL, NULL }, { "__objclass__", descr_objclass, NULL, NULL, NULL }, { "__doc__", method_doc, NULL, NULL, NULL }, { "__text_signature__", method_text_signature, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyMethodDef descr_methods[] = { { "__reduce__", descr_reduce, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyMethodDescr_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "method_descriptor", .tp_basicsize = sizeof(PyMethodDescrObject), .tp_dealloc = descr_dealloc,
    .tp_vectorcall_offset = offsetof(PyMethodDescrObject, vectorcall), .tp_repr = method_repr, .tp_call = PyVectorcall_Call,
    .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_METHOD_DESCRIPTOR,
    .tp_methods = descr_methods, .tp_getset = method_getsets, .tp_descr_get = method_get,
};

static PyObject *classmethod_descr_get(PyObject *self, PyObject *obj, PyObject *type) {
    PyMethodDescrObject *d = (PyMethodDescrObject *)self;
    if (!type) type = (PyObject *)Py_TYPE(obj);
    if (!PyType_Check(type) || !PyType_IsSubtype((PyTypeObject *)type, d->d_common.d_type)) {
        PyErr_Format(PyExc_TypeError, "descriptor '%U' for type '%s' doesn't apply to '%s'", d->d_common.d_name, d->d_common.d_type->tp_name, PyType_Check(type) ? ((PyTypeObject *)type)->tp_name : Py_TYPE(type)->tp_name);
        return NULL;
    }
    return PyCFunction_NewEx(d->d_method, type, NULL);
}
static PyObject *classmethod_descr_vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyMethodDescrObject *d = (PyMethodDescrObject *)f;
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs < 1 || !PyType_Check(args[0])) { PyErr_Format(PyExc_TypeError, "descriptor '%U' needs a type as first argument", d->d_common.d_name); return NULL; }
    return call_methoddef(d->d_method, args[0], args + 1, nargs - 1, kwnames);
}
PyTypeObject PyClassMethodDescr_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "classmethod_descriptor", .tp_basicsize = sizeof(PyMethodDescrObject), .tp_dealloc = descr_dealloc,
    .tp_vectorcall_offset = offsetof(PyMethodDescrObject, vectorcall), .tp_repr = method_repr, .tp_call = PyVectorcall_Call,
    .tp_getattro = PyObject_GenericGetAttr, .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL, .tp_methods = descr_methods, .tp_getset = method_getsets, .tp_descr_get = classmethod_descr_get,
};

static PyObject *descr_new(PyTypeObject *tp, PyTypeObject *type, const char *name) {
    PyDescrObject *d = (PyDescrObject *)PyObject_Malloc(tp->tp_basicsize);
    if (!d) return PyErr_NoMemory();
    PyObject_Init((PyObject *)d, tp);
    d->d_type = (PyTypeObject *)Py_NewRef((PyObject *)type);
    d->d_name = PyUnicode_InternFromString(name);
    d->d_qualname = NULL;
    return (PyObject *)d;
}
PyObject *PyDescr_NewMethod(PyTypeObject *tp, PyMethodDef *ml) {
    PyMethodDescrObject *d = (PyMethodDescrObject *)descr_new(&PyMethodDescr_Type, tp, ml->ml_name);
    if (!d) return NULL;
    d->d_method = ml;
    d->vectorcall = method_vectorcall;
    return (PyObject *)d;
}
PyObject *PyDescr_NewClassMethod(PyTypeObject *tp, PyMethodDef *ml) {
    PyMethodDescrObject *d = (PyMethodDescrObject *)descr_new(&PyClassMethodDescr_Type, tp, ml->ml_name);
    if (!d) return NULL;
    d->d_method = ml;
    d->vectorcall = classmethod_descr_vectorcall;
    return (PyObject *)d;
}

/* member descriptors */
static PyObject *member_get_value(PyMemberDef *m, PyObject *obj) {
    char *addr = (char *)obj + m->offset;
    switch (m->type) {
    case Py_T_SHORT: return PyLong_FromLong(*(short *)addr);
    case Py_T_INT: return PyLong_FromLong(*(int *)addr);
    case Py_T_LONG: return PyLong_FromLong(*(long *)addr);
    case Py_T_FLOAT: return PyFloat_FromDouble(*(float *)addr);
    case Py_T_DOUBLE: return PyFloat_FromDouble(*(double *)addr);
    case Py_T_STRING: { char *s = *(char **)addr; return s ? PyUnicode_FromString(s) : Py_NewRef(Py_None); }
    case Py_T_STRING_INPLACE: return PyUnicode_FromString(addr);
    case Py_T_CHAR: return PyUnicode_FromStringAndSize(addr, 1);
    case Py_T_BYTE: return PyLong_FromLong(*(signed char *)addr);
    case Py_T_UBYTE: return PyLong_FromUnsignedLong(*(unsigned char *)addr);
    case Py_T_USHORT: return PyLong_FromUnsignedLong(*(unsigned short *)addr);
    case Py_T_UINT: return PyLong_FromUnsignedLong(*(unsigned int *)addr);
    case Py_T_ULONG: return PyLong_FromUnsignedLong(*(unsigned long *)addr);
    case Py_T_BOOL: return PyBool_FromLong(*(char *)addr);
    case Py_T_LONGLONG: return PyLong_FromLongLong(*(long long *)addr);
    case Py_T_ULONGLONG: return PyLong_FromUnsignedLongLong(*(unsigned long long *)addr);
    case Py_T_PYSSIZET: return PyLong_FromSsize_t(*(Py_ssize_t *)addr);
    case _Py_T_NONE: Py_RETURN_NONE;
    case _Py_T_OBJECT: { PyObject *o = *(PyObject **)addr; return Py_NewRef(o ? o : Py_None); }
    case Py_T_OBJECT_EX: { PyObject *o = *(PyObject **)addr; if (!o) { PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", Py_TYPE(obj)->tp_name, m->name); return NULL; } return Py_NewRef(o); }
    }
    PyErr_SetString(PyExc_SystemError, "bad memberdescr type");
    return NULL;
}
static int member_set_value(PyMemberDef *m, PyObject *obj, PyObject *v) {
    char *addr = (char *)obj + m->offset;
    if (m->flags & Py_READONLY) { PyErr_SetString(PyExc_AttributeError, "readonly attribute"); return -1; }
    if (!v) {
        if (m->type == Py_T_OBJECT_EX) { PyObject **p = (PyObject **)addr; if (!*p) { PyErr_Format(PyExc_AttributeError, "'%s' object has no attribute '%s'", Py_TYPE(obj)->tp_name, m->name); return -1; } Py_CLEAR(*p); return 0; }
        if (m->type == _Py_T_OBJECT) { Py_CLEAR(*(PyObject **)addr); return 0; }
        PyErr_SetString(PyExc_TypeError, "can't delete numeric/char attribute");
        return -1;
    }
    switch (m->type) {
    case Py_T_OBJECT_EX: case _Py_T_OBJECT: Py_XSETREF(*(PyObject **)addr, Py_NewRef(v)); return 0;
    case Py_T_BOOL: { if (!PyBool_Check(v)) { PyErr_SetString(PyExc_TypeError, "attribute value type must be bool"); return -1; } *(char *)addr = v == Py_True; return 0; }
    case Py_T_INT: { long x = PyLong_AsLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(int *)addr = (int)x; return 0; }
    case Py_T_LONG: { long x = PyLong_AsLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(long *)addr = x; return 0; }
    case Py_T_PYSSIZET: { Py_ssize_t x = PyLong_AsSsize_t(v); if (x == -1 && PyErr_Occurred()) return -1; *(Py_ssize_t *)addr = x; return 0; }
    case Py_T_DOUBLE: { double x = PyFloat_AsDouble(v); if (x == -1.0 && PyErr_Occurred()) return -1; *(double *)addr = x; return 0; }
    case Py_T_FLOAT: { double x = PyFloat_AsDouble(v); if (x == -1.0 && PyErr_Occurred()) return -1; *(float *)addr = (float)x; return 0; }
    case Py_T_LONGLONG: { long long x = PyLong_AsLongLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(long long *)addr = x; return 0; }
    case Py_T_ULONGLONG: { unsigned long long x = PyLong_AsUnsignedLongLong(v); if (x == (unsigned long long)-1 && PyErr_Occurred()) return -1; *(unsigned long long *)addr = x; return 0; }
    case Py_T_UINT: { unsigned long x = PyLong_AsUnsignedLong(v); if (x == (unsigned long)-1 && PyErr_Occurred()) return -1; *(unsigned int *)addr = (unsigned)x; return 0; }
    case Py_T_ULONG: { unsigned long x = PyLong_AsUnsignedLong(v); if (x == (unsigned long)-1 && PyErr_Occurred()) return -1; *(unsigned long *)addr = x; return 0; }
    case Py_T_SHORT: { long x = PyLong_AsLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(short *)addr = (short)x; return 0; }
    case Py_T_BYTE: { long x = PyLong_AsLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(signed char *)addr = (signed char)x; return 0; }
    case Py_T_UBYTE: { long x = PyLong_AsLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(unsigned char *)addr = (unsigned char)x; return 0; }
    case Py_T_USHORT: { long x = PyLong_AsLong(v); if (x == -1 && PyErr_Occurred()) return -1; *(unsigned short *)addr = (unsigned short)x; return 0; }
    case Py_T_CHAR: { if (!PyUnicode_Check(v) || PyUnicode_GET_LENGTH(v) != 1) { PyErr_BadArgument(); return -1; } *addr = (char)PyUnicode_READ_CHAR(v, 0); return 0; }
    }
    PyErr_SetString(PyExc_TypeError, "readonly attribute");
    return -1;
}
static PyObject *member_get(PyObject *self, PyObject *obj, PyObject *type) {
    PIPER_UNUSED(type);
    if (!obj) return Py_NewRef(self);
    PyMemberDescrObject *d = (PyMemberDescrObject *)self;
    if (descr_check(&d->d_common, obj) < 0) return NULL;
    return member_get_value(d->d_member, obj);
}
static int member_set(PyObject *self, PyObject *obj, PyObject *v) {
    PyMemberDescrObject *d = (PyMemberDescrObject *)self;
    if (descr_check(&d->d_common, obj) < 0) return -1;
    return member_set_value(d->d_member, obj, v);
}
static PyObject *member_repr(PyObject *o) { PyDescrObject *d = (PyDescrObject *)o; return PyUnicode_FromFormat("<member '%U' of '%s' objects>", d->d_name, d->d_type->tp_name); }
static PyObject *member_doc(PyObject *o, void *c) { PIPER_UNUSED(c); const char *doc = ((PyMemberDescrObject *)o)->d_member->doc; return doc ? PyUnicode_FromString(doc) : Py_NewRef(Py_None); }
static PyGetSetDef member_getsets[] = { { "__name__", descr_name, NULL, NULL, NULL }, { "__qualname__", descr_qualname, NULL, NULL, NULL }, { "__objclass__", descr_objclass, NULL, NULL, NULL }, { "__doc__", member_doc, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyMemberDescr_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "member_descriptor", .tp_basicsize = sizeof(PyMemberDescrObject), .tp_dealloc = descr_dealloc, .tp_repr = member_repr,
    .tp_getattro = PyObject_GenericGetAttr, .tp_methods = descr_methods, .tp_getset = member_getsets, .tp_descr_get = member_get, .tp_descr_set = member_set,
};
PyObject *PyDescr_NewMember(PyTypeObject *tp, PyMemberDef *m) {
    PyMemberDescrObject *d = (PyMemberDescrObject *)descr_new(&PyMemberDescr_Type, tp, m->name);
    if (!d) return NULL;
    d->d_member = m;
    return (PyObject *)d;
}

/* getset descriptors */
static PyObject *getset_get(PyObject *self, PyObject *obj, PyObject *type) {
    PIPER_UNUSED(type);
    if (!obj) return Py_NewRef(self);
    PyGetSetDescrObject *d = (PyGetSetDescrObject *)self;
    if (descr_check(&d->d_common, obj) < 0) return NULL;
    if (!d->d_getset->get) { PyErr_Format(PyExc_AttributeError, "attribute '%U' of '%s' objects is not readable", d->d_common.d_name, d->d_common.d_type->tp_name); return NULL; }
    return d->d_getset->get(obj, d->d_getset->closure);
}
static int getset_set(PyObject *self, PyObject *obj, PyObject *v) {
    PyGetSetDescrObject *d = (PyGetSetDescrObject *)self;
    if (descr_check(&d->d_common, obj) < 0) return -1;
    if (!d->d_getset->set) { PyErr_Format(PyExc_AttributeError, "attribute '%U' of '%s' objects is not writable", d->d_common.d_name, d->d_common.d_type->tp_name); return -1; }
    return d->d_getset->set(obj, v, d->d_getset->closure);
}
static PyObject *getset_repr(PyObject *o) { PyDescrObject *d = (PyDescrObject *)o; return PyUnicode_FromFormat("<attribute '%U' of '%s' objects>", d->d_name, d->d_type->tp_name); }
static PyObject *getset_doc(PyObject *o, void *c) { PIPER_UNUSED(c); const char *doc = ((PyGetSetDescrObject *)o)->d_getset->doc; return doc ? PyUnicode_FromString(doc) : Py_NewRef(Py_None); }
static PyGetSetDef getset_getsets[] = { { "__name__", descr_name, NULL, NULL, NULL }, { "__qualname__", descr_qualname, NULL, NULL, NULL }, { "__objclass__", descr_objclass, NULL, NULL, NULL }, { "__doc__", getset_doc, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyGetSetDescr_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "getset_descriptor", .tp_basicsize = sizeof(PyGetSetDescrObject), .tp_dealloc = descr_dealloc, .tp_repr = getset_repr,
    .tp_getattro = PyObject_GenericGetAttr, .tp_getset = getset_getsets, .tp_descr_get = getset_get, .tp_descr_set = getset_set,
};
PyObject *PyDescr_NewGetSet(PyTypeObject *tp, PyGetSetDef *g) {
    PyGetSetDescrObject *d = (PyGetSetDescrObject *)descr_new(&PyGetSetDescr_Type, tp, g->name);
    if (!d) return NULL;
    d->d_getset = g;
    return (PyObject *)d;
}

/* ---- bound method ------------------------------------------------------------------ */

typedef struct { PyObject_HEAD PyObject *im_func; PyObject *im_self; vectorcallfunc vectorcall; } PyMethodObject;
static void method_dealloc(PyObject *o) { PyMethodObject *m = (PyMethodObject *)o; Py_XDECREF(m->im_func); Py_XDECREF(m->im_self); PyObject_Free(o); }
static PyObject *bound_vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PyMethodObject *m = (PyMethodObject *)f;
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    if (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET) {
        PyObject **a = (PyObject **)args - 1;
        PyObject *saved = a[0];
        a[0] = m->im_self;
        PyObject *r = PyObject_Vectorcall(m->im_func, a, (size_t)nargs + 1, kwnames);
        a[0] = saved;
        return r;
    }
    PyObject *stack[8];
    PyObject **buf = nargs + nkw + 1 <= 8 ? stack : PyMem_Malloc((size_t)(nargs + nkw + 1) * sizeof(PyObject *));
    buf[0] = m->im_self;
    memcpy(buf + 1, args, (size_t)(nargs + nkw) * sizeof(PyObject *));
    PyObject *r = PyObject_Vectorcall(m->im_func, buf, (size_t)nargs + 1, kwnames);
    if (buf != stack) PyMem_Free(buf);
    return r;
}
static PyObject *method_obj_repr(PyObject *o) {
    PyMethodObject *m = (PyMethodObject *)o;
    PyObject *name = PyObject_GetAttrString(m->im_func, "__qualname__");
    if (!name) { PyErr_Clear(); name = PyObject_GetAttrString(m->im_func, "__name__"); if (!name) { PyErr_Clear(); name = PyUnicode_FromString("?"); } }
    PyObject *r = PyUnicode_FromFormat("<bound method %U of %R>", name, m->im_self);
    Py_DECREF(name);
    return r;
}
static PyObject *method_getattro(PyObject *o, PyObject *name) {
    PyMethodObject *m = (PyMethodObject *)o;
    PyObject *descr = piper_type_lookup(Py_TYPE(o), name);
    if (descr) { descrgetfunc f = Py_TYPE(descr)->tp_descr_get; if (f) return f(descr, o, (PyObject *)Py_TYPE(o)); return Py_NewRef(descr); }
    return PyObject_GetAttr(m->im_func, name);
}
static PyObject *method_richcompare(PyObject *a, PyObject *b, int op) {
    if ((op != Py_EQ && op != Py_NE) || !Py_IS_TYPE(b, &PyMethod_Type)) Py_RETURN_NOTIMPLEMENTED;
    PyMethodObject *x = (PyMethodObject *)a, *y = (PyMethodObject *)b;
    int eq = x->im_self == y->im_self;
    if (eq) { eq = PyObject_RichCompareBool(x->im_func, y->im_func, Py_EQ); if (eq < 0) return NULL; }
    Py_RETURN_BOOL(op == Py_EQ ? eq : !eq);
}
static Py_hash_t method_hash(PyObject *o) { PyMethodObject *m = (PyMethodObject *)o; Py_hash_t x = _Py_HashPointer(m->im_self), y = PyObject_Hash(m->im_func); if (y == -1) return -1; x ^= y; return x == -1 ? -2 : x; }
static PyObject *method_reduce(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyMethodObject *m = (PyMethodObject *)o; PyObject *name = PyObject_GetAttrString(m->im_func, "__name__"); if (!name) return NULL; PyObject *b = piper_builtins(); PyObject *args = PyTuple_Pack(2, m->im_self, name); Py_DECREF(name); PyObject *r = PyTuple_Pack(2, PyDict_GetItemString(b, "getattr"), args); Py_DECREF(args); return r; }
static PyMethodDef method_obj_methods[] = { { "__reduce__", method_reduce, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyMemberDef method_members[] = { { "__func__", _Py_T_OBJECT, offsetof(PyMethodObject, im_func), Py_READONLY, NULL }, { "__self__", _Py_T_OBJECT, offsetof(PyMethodObject, im_self), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
static PyObject *method_doc_get(PyObject *o, void *c) { PIPER_UNUSED(c); return PyObject_GetAttrString(((PyMethodObject *)o)->im_func, "__doc__"); }
static PyGetSetDef method_obj_getsets[] = { { "__doc__", method_doc_get, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyObject *method_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) { PIPER_UNUSED(tp); PIPER_UNUSED(kwds); if (PyTuple_GET_SIZE(args) != 2) { PyErr_SetString(PyExc_TypeError, "method expected 2 arguments"); return NULL; } return PyMethod_New(PyTuple_GET_ITEM(args, 0), PyTuple_GET_ITEM(args, 1)); }
PyTypeObject PyMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "method", .tp_basicsize = sizeof(PyMethodObject), .tp_dealloc = method_dealloc, .tp_vectorcall_offset = offsetof(PyMethodObject, vectorcall),
    .tp_repr = method_obj_repr, .tp_hash = method_hash, .tp_call = PyVectorcall_Call, .tp_getattro = method_getattro,
    .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL, .tp_richcompare = method_richcompare, .tp_methods = method_obj_methods, .tp_members = method_members, .tp_getset = method_obj_getsets, .tp_new = method_new,
};
PyObject *PyMethod_New(PyObject *func, PyObject *self) {
    if (!self) { PyErr_BadInternalCall(); return NULL; }
    PyMethodObject *m = PyObject_New(PyMethodObject, &PyMethod_Type);
    if (!m) return NULL;
    m->im_func = Py_NewRef(func);
    m->im_self = Py_NewRef(self);
    m->vectorcall = bound_vectorcall;
    return (PyObject *)m;
}
PyObject *piper_bound_method_new(PyObject *func, PyObject *self) { return PyMethod_New(func, self); }
PyObject *PyMethod_Function(PyObject *m) { return ((PyMethodObject *)m)->im_func; }
PyObject *PyMethod_Self(PyObject *m) { return ((PyMethodObject *)m)->im_self; }

/* ---- staticmethod / classmethod ---------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *callable; PyObject *dict; } wrapperobject;
static void wrapper_dealloc(PyObject *o) { wrapperobject *w = (wrapperobject *)o; Py_XDECREF(w->callable); Py_XDECREF(w->dict); Py_TYPE(o)->tp_free(o); }
static int wrapper_init(PyObject *o, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(kwds);
    if (PyTuple_GET_SIZE(args) != 1) { PyErr_Format(PyExc_TypeError, "%s expected 1 argument, got %zd", Py_TYPE(o)->tp_name, PyTuple_GET_SIZE(args)); return -1; }
    wrapperobject *w = (wrapperobject *)o;
    Py_XSETREF(w->callable, Py_NewRef(PyTuple_GET_ITEM(args, 0)));
    /* copy metadata like functools.wraps */
    const char *names[] = { "__module__", "__name__", "__qualname__", "__doc__", "__annotations__", NULL };
    if (!w->dict) w->dict = PyDict_New();
    for (int i = 0; names[i]; i++) { PyObject *v; if (PyObject_GetOptionalAttrString(w->callable, names[i], &v) < 0) return -1; if (v) { PyDict_SetItemString(w->dict, names[i], v); Py_DECREF(v); } }
    return 0;
}
static PyObject *staticmethod_get(PyObject *self, PyObject *obj, PyObject *type) { PIPER_UNUSED(obj); PIPER_UNUSED(type); return Py_NewRef(((wrapperobject *)self)->callable); }
static PyObject *staticmethod_call(PyObject *self, PyObject *args, PyObject *kwds) { return PyObject_Call(((wrapperobject *)self)->callable, args, kwds); }
static PyObject *classmethod_get(PyObject *self, PyObject *obj, PyObject *type) {
    if (!type) type = (PyObject *)Py_TYPE(obj);
    return PyMethod_New(((wrapperobject *)self)->callable, type);
}
static PyObject *wrapper_repr(PyObject *o) { return PyUnicode_FromFormat("<%s(%R)>", Py_TYPE(o)->tp_name, ((wrapperobject *)o)->callable); }
static PyObject *wrapper_isabstract(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *r; if (PyObject_GetOptionalAttrString(((wrapperobject *)o)->callable, "__isabstractmethod__", &r) < 0) return NULL; if (!r) Py_RETURN_FALSE; int t = PyObject_IsTrue(r); Py_DECREF(r); if (t < 0) return NULL; Py_RETURN_BOOL(t); }
static PyObject *wrapper_wrapped(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((wrapperobject *)o)->callable); }
static PyGetSetDef wrapper_getsets[] = { { "__isabstractmethod__", wrapper_isabstract, NULL, NULL, NULL }, { "__wrapped__", wrapper_wrapped, NULL, NULL, NULL }, { "__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyMemberDef wrapper_members[] = { { "__func__", _Py_T_OBJECT, offsetof(wrapperobject, callable), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
PyTypeObject PyStaticMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "staticmethod", .tp_basicsize = sizeof(wrapperobject), .tp_dealloc = wrapper_dealloc, .tp_repr = wrapper_repr, .tp_call = staticmethod_call,
    .tp_getattro = PyObject_GenericGetAttr, .tp_setattro = PyObject_GenericSetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_members = wrapper_members, .tp_getset = wrapper_getsets,
    .tp_descr_get = staticmethod_get, .tp_dictoffset = offsetof(wrapperobject, dict), .tp_init = wrapper_init, .tp_alloc = PyType_GenericAlloc, .tp_new = PyType_GenericNew, .tp_free = PyObject_Free,
};
PyTypeObject PyClassMethod_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "classmethod", .tp_basicsize = sizeof(wrapperobject), .tp_dealloc = wrapper_dealloc, .tp_repr = wrapper_repr,
    .tp_getattro = PyObject_GenericGetAttr, .tp_setattro = PyObject_GenericSetAttr, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_members = wrapper_members, .tp_getset = wrapper_getsets,
    .tp_descr_get = classmethod_get, .tp_dictoffset = offsetof(wrapperobject, dict), .tp_init = wrapper_init, .tp_alloc = PyType_GenericAlloc, .tp_new = PyType_GenericNew, .tp_free = PyObject_Free,
};
static PyObject *make_wrapper(PyTypeObject *tp, PyObject *f) {
    wrapperobject *w = (wrapperobject *)tp->tp_alloc(tp, 0);
    if (!w) return NULL;
    PyObject *args = PyTuple_Pack(1, f);
    int r = wrapper_init((PyObject *)w, args, NULL);
    Py_DECREF(args);
    if (r < 0) { Py_DECREF(w); return NULL; }
    return (PyObject *)w;
}
PyObject *PyStaticMethod_New(PyObject *f) { return make_wrapper(&PyStaticMethod_Type, f); }
PyObject *PyClassMethod_New(PyObject *f) { return make_wrapper(&PyClassMethod_Type, f); }

/* ---- property ------------------------------------------------------------------------ */

typedef struct { PyObject_HEAD PyObject *fget, *fset, *fdel, *doc, *name; int getter_doc; } propertyobject;
static void property_dealloc(PyObject *o) { propertyobject *p = (propertyobject *)o; Py_XDECREF(p->fget); Py_XDECREF(p->fset); Py_XDECREF(p->fdel); Py_XDECREF(p->doc); Py_XDECREF(p->name); Py_TYPE(o)->tp_free(o); }
static int property_init(PyObject *o, PyObject *args, PyObject *kwds) {
    propertyobject *p = (propertyobject *)o;
    PyObject *fget = NULL, *fset = NULL, *fdel = NULL, *doc = NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 4) { PyErr_SetString(PyExc_TypeError, "property() takes at most 4 arguments"); return -1; }
    if (n >= 1) fget = PyTuple_GET_ITEM(args, 0);
    if (n >= 2) fset = PyTuple_GET_ITEM(args, 1);
    if (n >= 3) fdel = PyTuple_GET_ITEM(args, 2);
    if (n >= 4) doc = PyTuple_GET_ITEM(args, 3);
    if (kwds) { PyObject *v; if ((v = PyDict_GetItemString(kwds, "fget"))) fget = v; if ((v = PyDict_GetItemString(kwds, "fset"))) fset = v; if ((v = PyDict_GetItemString(kwds, "fdel"))) fdel = v; if ((v = PyDict_GetItemString(kwds, "doc"))) doc = v; }
    if (fget == Py_None) fget = NULL;
    if (fset == Py_None) fset = NULL;
    if (fdel == Py_None) fdel = NULL;
    Py_XSETREF(p->fget, Py_XNewRef(fget));
    Py_XSETREF(p->fset, Py_XNewRef(fset));
    Py_XSETREF(p->fdel, Py_XNewRef(fdel));
    p->getter_doc = 0;
    if ((!doc || doc == Py_None) && fget) {
        PyObject *d;
        if (PyObject_GetOptionalAttrString(fget, "__doc__", &d) < 0) return -1;
        if (d && d != Py_None) { doc = d; p->getter_doc = 1; Py_XSETREF(p->doc, doc); return 0; }
        Py_XDECREF(d);
        doc = NULL;
    }
    Py_XSETREF(p->doc, Py_XNewRef(doc));
    return 0;
}
static PyObject *property_descr_get(PyObject *self, PyObject *obj, PyObject *type) {
    PIPER_UNUSED(type);
    if (!obj || obj == Py_None) return Py_NewRef(self);
    propertyobject *p = (propertyobject *)self;
    if (!p->fget) {
        if (p->name) PyErr_Format(PyExc_AttributeError, "property '%U' of '%s' object has no getter", p->name, Py_TYPE(obj)->tp_name);
        else PyErr_Format(PyExc_AttributeError, "property of '%s' object has no getter", Py_TYPE(obj)->tp_name);
        return NULL;
    }
    return PyObject_CallOneArg(p->fget, obj);
}
static int property_descr_set(PyObject *self, PyObject *obj, PyObject *v) {
    propertyobject *p = (propertyobject *)self;
    PyObject *f = v ? p->fset : p->fdel;
    if (!f) {
        const char *what = v ? "setter" : "deleter";
        if (p->name) PyErr_Format(PyExc_AttributeError, "property '%U' of '%s' object has no %s", p->name, Py_TYPE(obj)->tp_name, what);
        else PyErr_Format(PyExc_AttributeError, "property of '%s' object has no %s", Py_TYPE(obj)->tp_name, what);
        return -1;
    }
    PyObject *r = v ? PyObject_CallFunctionObjArgs(f, obj, v, NULL) : PyObject_CallOneArg(f, obj);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
static PyObject *property_copy(PyObject *self, PyObject *fget, PyObject *fset, PyObject *fdel) {
    propertyobject *p = (propertyobject *)self;
    PyObject *g = fget ? fget : (p->fget ? p->fget : Py_None), *s = fset ? fset : (p->fset ? p->fset : Py_None), *d = fdel ? fdel : (p->fdel ? p->fdel : Py_None);
    PyObject *doc = p->getter_doc && fget ? Py_None : (p->doc ? p->doc : Py_None);
    PyObject *args = PyTuple_Pack(4, g, s, d, doc);
    PyObject *r = PyObject_Call((PyObject *)Py_TYPE(self), args, NULL);
    Py_DECREF(args);
    if (r && p->name) Py_XSETREF(((propertyobject *)r)->name, Py_NewRef(p->name));
    return r;
}
static PyObject *property_getter(PyObject *self, PyObject *f) { return property_copy(self, f, NULL, NULL); }
static PyObject *property_setter(PyObject *self, PyObject *f) { return property_copy(self, NULL, f, NULL); }
static PyObject *property_deleter(PyObject *self, PyObject *f) { return property_copy(self, NULL, NULL, f); }
static PyObject *property_set_name(PyObject *self, PyObject *const *a, Py_ssize_t n) { if (n != 2) { PyErr_SetString(PyExc_TypeError, "__set_name__() takes 2 positional arguments"); return NULL; } Py_XSETREF(((propertyobject *)self)->name, Py_NewRef(a[1])); Py_RETURN_NONE; }
static PyObject *property_isabstract(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    propertyobject *p = (propertyobject *)o;
    PyObject *fs[3] = { p->fget, p->fset, p->fdel };
    for (int i = 0; i < 3; i++) { if (!fs[i]) continue; PyObject *r; if (PyObject_GetOptionalAttrString(fs[i], "__isabstractmethod__", &r) < 0) return NULL; if (r) { int t = PyObject_IsTrue(r); Py_DECREF(r); if (t < 0) return NULL; if (t) Py_RETURN_TRUE; } }
    Py_RETURN_FALSE;
}
static PyObject *property_doc_get(PyObject *o, void *c) { PIPER_UNUSED(c); propertyobject *p = (propertyobject *)o; return Py_NewRef(p->doc ? p->doc : Py_None); }
static int property_doc_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); Py_XSETREF(((propertyobject *)o)->doc, Py_XNewRef(v)); return 0; }
static PyObject *property_name_get(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    propertyobject *p = (propertyobject *)o;
    if (p->name) return Py_NewRef(p->name);
    if (p->fget) { PyObject *n; if (PyObject_GetOptionalAttrString(p->fget, "__name__", &n) < 0) return NULL; if (n) return n; }
    PyErr_SetString(PyExc_AttributeError, "'property' object has no attribute '__name__'");
    return NULL;
}
static int property_name_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); Py_XSETREF(((propertyobject *)o)->name, Py_XNewRef(v)); return 0; }
static PyMethodDef property_methods[] = { { "getter", property_getter, METH_O, NULL }, { "setter", property_setter, METH_O, NULL }, { "deleter", property_deleter, METH_O, NULL }, { "__set_name__", (PyCFunction)(void (*)(void))property_set_name, METH_FASTCALL, NULL }, { NULL, NULL, 0, NULL } };
static PyMemberDef property_members[] = { { "fget", _Py_T_OBJECT, offsetof(propertyobject, fget), Py_READONLY, NULL }, { "fset", _Py_T_OBJECT, offsetof(propertyobject, fset), Py_READONLY, NULL }, { "fdel", _Py_T_OBJECT, offsetof(propertyobject, fdel), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
static PyGetSetDef property_getsets[] = { { "__isabstractmethod__", property_isabstract, NULL, NULL, NULL }, { "__doc__", property_doc_get, property_doc_set, NULL, NULL }, { "__name__", property_name_get, property_name_set, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyProperty_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "property", .tp_basicsize = sizeof(propertyobject), .tp_dealloc = property_dealloc, .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE, .tp_methods = property_methods, .tp_members = property_members, .tp_getset = property_getsets,
    .tp_descr_get = property_descr_get, .tp_descr_set = property_descr_set, .tp_init = property_init, .tp_alloc = PyType_GenericAlloc, .tp_new = PyType_GenericNew, .tp_free = PyObject_Free,
};
PyObject *piper_property_new(PyObject *fget, PyObject *fset, PyObject *fdel, PyObject *doc) {
    PyObject *args = PyTuple_Pack(4, fget ? fget : Py_None, fset ? fset : Py_None, fdel ? fdel : Py_None, doc ? doc : Py_None);
    PyObject *r = PyObject_Call((PyObject *)&PyProperty_Type, args, NULL);
    Py_DECREF(args);
    return r;
}

/* ---- super ----------------------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyTypeObject *type; PyObject *obj; PyTypeObject *obj_type; } superobject;
static void super_dealloc(PyObject *o) { superobject *s = (superobject *)o; Py_XDECREF(s->type); Py_XDECREF(s->obj); Py_XDECREF(s->obj_type); Py_TYPE(o)->tp_free(o); }
static PyTypeObject *supercheck(PyTypeObject *type, PyObject *obj) {
    if (PyType_Check(obj) && PyType_IsSubtype((PyTypeObject *)obj, type)) return (PyTypeObject *)Py_NewRef(obj);
    if (PyType_IsSubtype(Py_TYPE(obj), type)) return (PyTypeObject *)Py_NewRef((PyObject *)Py_TYPE(obj));
    PyObject *cls;
    if (PyObject_GetOptionalAttrString(obj, "__class__", &cls) < 0) return NULL;
    if (cls && PyType_Check(cls) && cls != (PyObject *)Py_TYPE(obj) && PyType_IsSubtype((PyTypeObject *)cls, type)) return (PyTypeObject *)cls;
    Py_XDECREF(cls);
    PyErr_SetString(PyExc_TypeError, "super(type, obj): obj (instance of " "%s) is not an instance or subtype of type (%s).");
    PyErr_Clear();
    PyErr_Format(PyExc_TypeError, "super(type, obj): obj (instance of %s) is not an instance or subtype of type (%s).", Py_TYPE(obj)->tp_name, type->tp_name);
    return NULL;
}
static PyObject *super_getattro(PyObject *self, PyObject *name) {
    superobject *su = (superobject *)self;
    if (!su->obj_type || PyUnicode_EqualToUTF8(name, "__class__")) return PyObject_GenericGetAttr(self, name);
    PyObject *mro = su->obj_type->tp_mro;
    Py_ssize_t n = PyTuple_GET_SIZE(mro), i;
    for (i = 0; i + 1 < n; i++) if ((PyObject *)su->type == PyTuple_GET_ITEM(mro, i)) break;
    i++;
    for (; i < n; i++) {
        PyTypeObject *t = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if (!t->tp_dict) continue;
        PyObject *res = PyDict_GetItemWithError(t->tp_dict, name);
        if (res) {
            Py_INCREF(res);
            descrgetfunc f = Py_TYPE(res)->tp_descr_get;
            if (f) { PyObject *r = f(res, (su->obj == (PyObject *)su->obj_type) ? NULL : su->obj, (PyObject *)su->obj_type); Py_DECREF(res); return r; }
            return res;
        }
        if (PyErr_Occurred()) return NULL;
    }
    return PyObject_GenericGetAttr(self, name);
}
static PyObject *super_descr_get(PyObject *self, PyObject *obj, PyObject *type) {
    PIPER_UNUSED(type);
    superobject *su = (superobject *)self;
    if (!obj || obj == Py_None || su->obj) return Py_NewRef(self);
    return piper_super_new(su->type, obj);
}
static int super_init(PyObject *self, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(kwds);
    superobject *su = (superobject *)self;
    PyObject *type = NULL, *obj = NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n >= 1) type = PyTuple_GET_ITEM(args, 0);
    if (n >= 2) obj = PyTuple_GET_ITEM(args, 1);
    if (n > 2) { PyErr_SetString(PyExc_TypeError, "super() takes at most 2 arguments"); return -1; }
    if (!type) {
        extern int piper_super_zero_arg(PyObject **type, PyObject **obj);
        if (piper_super_zero_arg(&type, &obj) < 0) return -1;
    }
    if (!PyType_Check(type)) { PyErr_Format(PyExc_TypeError, "super() argument 1 must be a type, not %s", Py_TYPE(type)->tp_name); return -1; }
    if (obj == Py_None) obj = NULL;
    PyTypeObject *obj_type = NULL;
    if (obj) { obj_type = supercheck((PyTypeObject *)type, obj); if (!obj_type) return -1; }
    Py_XSETREF(su->type, (PyTypeObject *)Py_NewRef(type));
    Py_XSETREF(su->obj, Py_XNewRef(obj));
    Py_XSETREF(su->obj_type, obj_type);
    return 0;
}
static PyObject *super_repr(PyObject *self) {
    superobject *su = (superobject *)self;
    if (su->obj_type) return PyUnicode_FromFormat("<super: <class '%s'>, <%s object>>", su->type ? su->type->tp_name : "NULL", su->obj_type->tp_name);
    return PyUnicode_FromFormat("<super: <class '%s'>, NULL>", su->type ? su->type->tp_name : "NULL");
}
static PyMemberDef super_members[] = { { "__thisclass__", _Py_T_OBJECT, offsetof(superobject, type), Py_READONLY, NULL }, { "__self__", _Py_T_OBJECT, offsetof(superobject, obj), Py_READONLY, NULL }, { "__self_class__", _Py_T_OBJECT, offsetof(superobject, obj_type), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
PyTypeObject PySuper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "super", .tp_basicsize = sizeof(superobject), .tp_dealloc = super_dealloc, .tp_repr = super_repr, .tp_getattro = super_getattro,
    .tp_flags = Py_TPFLAGS_BASETYPE, .tp_members = super_members, .tp_descr_get = super_descr_get, .tp_init = super_init, .tp_alloc = PyType_GenericAlloc, .tp_new = PyType_GenericNew, .tp_free = PyObject_Free,
};
PyObject *piper_super_new(PyTypeObject *type, PyObject *obj) {
    PyObject *args = PyTuple_Pack(2, (PyObject *)type, obj);
    PyObject *r = PyObject_Call((PyObject *)&PySuper_Type, args, NULL);
    Py_DECREF(args);
    return r;
}

/* ---- types.GenericAlias and types.UnionType (minimal) -------------------------------- */

typedef struct { PyObject_HEAD PyObject *origin; PyObject *args; } gaobject;
static void ga_dealloc(PyObject *o) { gaobject *g = (gaobject *)o; Py_XDECREF(g->origin); Py_XDECREF(g->args); PyObject_Free(o); }
static PyObject *ga_repr_item(PyObject *p) {
    if (p == Py_Ellipsis) return PyUnicode_FromString("...");
    if (PyType_Check(p)) {
        PyObject *mod = PyObject_GetAttrString(p, "__module__");
        PyObject *qn = PyObject_GetAttrString(p, "__qualname__");
        PyObject *r;
        if (mod && qn && PyUnicode_Check(mod) && !PyUnicode_EqualToUTF8(mod, "builtins")) r = PyUnicode_FromFormat("%U.%U", mod, qn);
        else r = qn ? Py_NewRef(qn) : PyObject_Repr(p);
        PyErr_Clear();
        Py_XDECREF(mod); Py_XDECREF(qn);
        return r;
    }
    return PyObject_Repr(p);
}
static PyObject *ga_repr(PyObject *o) {
    gaobject *g = (gaobject *)o;
    PyObject *origin = ga_repr_item(g->origin);
    if (!origin) return NULL;
    PyObject *parts = PyList_New(0);
    PyObject *sep = PyUnicode_FromString(", ");
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(g->args); i++) { if (i) PyList_Append(parts, sep); PyObject *s = ga_repr_item(PyTuple_GET_ITEM(g->args, i)); if (!s) { Py_DECREF(parts); Py_DECREF(sep); Py_DECREF(origin); return NULL; } PyList_Append(parts, s); Py_DECREF(s); }
    Py_DECREF(sep);
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *inner = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts);
    PyObject *r = PyUnicode_FromFormat("%U[%U]", origin, inner);
    Py_DECREF(origin); Py_DECREF(inner);
    return r;
}
static PyObject *ga_call(PyObject *o, PyObject *args, PyObject *kwds) {
    gaobject *g = (gaobject *)o;
    PyObject *r = PyObject_Call(g->origin, args, kwds);
    if (r) { if (PyObject_SetAttrString(r, "__orig_class__", o) < 0) PyErr_Clear(); }
    return r;
}
static PyObject *ga_getattro(PyObject *o, PyObject *name) {
    gaobject *g = (gaobject *)o;
    const char *n = PyUnicode_AsUTF8(name);
    if (n && strncmp(n, "__", 2) == 0 && (!strcmp(n, "__origin__") || !strcmp(n, "__args__") || !strcmp(n, "__parameters__") || !strcmp(n, "__class__") || !strcmp(n, "__mro_entries__") || !strcmp(n, "__reduce__") || !strcmp(n, "__unpacked__") || !strcmp(n, "__typing_unpacked_tuple_args__"))) return PyObject_GenericGetAttr(o, name);
    return PyObject_GetAttr(g->origin, name);
}
static PyObject *ga_mro_entries(PyObject *o, PyObject *bases) { PIPER_UNUSED(bases); return PyTuple_Pack(1, ((gaobject *)o)->origin); }
static PyObject *ga_getitem(PyObject *o, PyObject *item) { return PyObject_GetItem(((gaobject *)o)->origin, item); }
static PyObject *ga_richcompare(PyObject *a, PyObject *b, int op) {
    if ((op != Py_EQ && op != Py_NE) || !Py_IS_TYPE(b, Py_TYPE(a))) Py_RETURN_NOTIMPLEMENTED;
    gaobject *x = (gaobject *)a, *y = (gaobject *)b;
    int e1 = PyObject_RichCompareBool(x->origin, y->origin, Py_EQ);
    if (e1 < 0) return NULL;
    int e2 = e1 ? PyObject_RichCompareBool(x->args, y->args, Py_EQ) : 0;
    if (e2 < 0) return NULL;
    Py_RETURN_BOOL(op == Py_EQ ? (e1 && e2) : !(e1 && e2));
}
static Py_hash_t ga_hash(PyObject *o) { gaobject *g = (gaobject *)o; Py_hash_t h1 = PyObject_Hash(g->origin), h2 = PyObject_Hash(g->args); if (h1 == -1 || h2 == -1) return -1; Py_hash_t h = h1 ^ h2; return h == -1 ? -2 : h; }
static PyObject *ga_parameters(PyObject *o, void *c) { PIPER_UNUSED(c); PIPER_UNUSED(o); return PyTuple_New(0); }
static PyObject *ga_instancecheck(PyObject *o, PyObject *inst) { PIPER_UNUSED(o); PIPER_UNUSED(inst); PyErr_SetString(PyExc_TypeError, "isinstance() argument 2 cannot be a parameterized generic"); return NULL; }
static PyMemberDef ga_members[] = { { "__origin__", _Py_T_OBJECT, offsetof(gaobject, origin), Py_READONLY, NULL }, { "__args__", _Py_T_OBJECT, offsetof(gaobject, args), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
static PyGetSetDef ga_getsets[] = { { "__parameters__", ga_parameters, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyMethodDef ga_methods[] = { { "__mro_entries__", ga_mro_entries, METH_O, NULL }, { "__instancecheck__", ga_instancecheck, METH_O, NULL }, { "__subclasscheck__", ga_instancecheck, METH_O, NULL }, { NULL, NULL, 0, NULL } };
static PyMappingMethods ga_as_mapping = { .mp_subscript = ga_getitem };
static PyObject *ga_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) { PIPER_UNUSED(tp); PIPER_UNUSED(kwds); if (PyTuple_GET_SIZE(args) != 2) { PyErr_SetString(PyExc_TypeError, "GenericAlias expected 2 arguments"); return NULL; } extern PyObject *piper_generic_alias_new(PyObject *, PyObject *); return piper_generic_alias_new(PyTuple_GET_ITEM(args, 0), PyTuple_GET_ITEM(args, 1)); }
static PyObject *ga_or(PyObject *a, PyObject *b) { extern PyObject *piper_union_type_new(PyObject *a, PyObject *b); return piper_union_type_new(a, b); }
static PyNumberMethods ga_as_number = { .nb_or = ga_or };
static PyObject *ga_iter(PyObject *o) { PyObject *t = PyTuple_Pack(1, o); PyObject *it = PyObject_GetIter(t); Py_DECREF(t); return it; }
PyTypeObject PyGenericAlias_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "types.GenericAlias", .tp_basicsize = sizeof(gaobject), .tp_dealloc = ga_dealloc, .tp_repr = ga_repr, .tp_as_number = &ga_as_number, .tp_as_mapping = &ga_as_mapping,
    .tp_hash = ga_hash, .tp_call = ga_call, .tp_getattro = ga_getattro, .tp_flags = Py_TPFLAGS_BASETYPE, .tp_richcompare = ga_richcompare, .tp_iter = ga_iter,
    .tp_methods = ga_methods, .tp_members = ga_members, .tp_getset = ga_getsets, .tp_alloc = PyType_GenericAlloc, .tp_new = ga_new, .tp_free = PyObject_Free,
};
PyObject *piper_generic_alias_new(PyObject *origin, PyObject *args) {
    gaobject *g = PyObject_New(gaobject, &PyGenericAlias_Type);
    if (!g) return NULL;
    g->origin = Py_NewRef(origin);
    g->args = PyTuple_Check(args) ? Py_NewRef(args) : PyTuple_Pack(1, args);
    return (PyObject *)g;
}

typedef struct { PyObject_HEAD PyObject *args; } unionobject;
static void union_dealloc(PyObject *o) { Py_XDECREF(((unionobject *)o)->args); PyObject_Free(o); }
static PyObject *union_repr(PyObject *o) {
    unionobject *u = (unionobject *)o;
    PyObject *parts = PyList_New(0);
    PyObject *sep = PyUnicode_FromString(" | ");
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(u->args); i++) {
        if (i) PyList_Append(parts, sep);
        PyObject *p = PyTuple_GET_ITEM(u->args, i);
        PyObject *s = p == Py_None || p == (PyObject *)&_PyNone_Type ? PyUnicode_FromString("None") : ga_repr_item(p);
        if (!s) { Py_DECREF(parts); Py_DECREF(sep); return NULL; }
        PyList_Append(parts, s); Py_DECREF(s);
    }
    Py_DECREF(sep);
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts);
    return r;
}
static PyObject *union_instancecheck(PyObject *o, PyObject *inst) {
    unionobject *u = (unionobject *)o;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(u->args); i++) {
        PyObject *t = PyTuple_GET_ITEM(u->args, i);
        if (t == Py_None) t = (PyObject *)&_PyNone_Type;
        int r = PyObject_IsInstance(inst, t);
        if (r < 0) return NULL;
        if (r) Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}
static PyObject *union_subclasscheck(PyObject *o, PyObject *sub) {
    unionobject *u = (unionobject *)o;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(u->args); i++) {
        PyObject *t = PyTuple_GET_ITEM(u->args, i);
        if (t == Py_None) t = (PyObject *)&_PyNone_Type;
        int r = PyObject_IsSubclass(sub, t);
        if (r < 0) return NULL;
        if (r) Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}
static PyObject *union_richcompare(PyObject *a, PyObject *b, int op) {
    if ((op != Py_EQ && op != Py_NE) || !Py_IS_TYPE(b, Py_TYPE(a))) Py_RETURN_NOTIMPLEMENTED;
    PyObject *sa = PyFrozenSet_New(((unionobject *)a)->args), *sb = PyFrozenSet_New(((unionobject *)b)->args);
    if (!sa || !sb) { Py_XDECREF(sa); Py_XDECREF(sb); return NULL; }
    PyObject *r = PyObject_RichCompare(sa, sb, op);
    Py_DECREF(sa); Py_DECREF(sb);
    return r;
}
static Py_hash_t union_hash(PyObject *o) { PyObject *s = PyFrozenSet_New(((unionobject *)o)->args); if (!s) return -1; Py_hash_t h = PyObject_Hash(s); Py_DECREF(s); return h; }
static PyObject *union_or(PyObject *a, PyObject *b) { extern PyObject *piper_union_type_new(PyObject *a, PyObject *b); return piper_union_type_new(a, b); }
static PyMemberDef union_members[] = { { "__args__", _Py_T_OBJECT, offsetof(unionobject, args), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
static PyMethodDef union_methods[] = { { "__instancecheck__", union_instancecheck, METH_O, NULL }, { "__subclasscheck__", union_subclasscheck, METH_O, NULL }, { NULL, NULL, 0, NULL } };
static PyNumberMethods union_as_number = { .nb_or = union_or };
PyTypeObject PyUnion_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "types.UnionType", .tp_basicsize = sizeof(unionobject), .tp_dealloc = union_dealloc, .tp_repr = union_repr, .tp_as_number = &union_as_number,
    .tp_hash = union_hash, .tp_getattro = PyObject_GenericGetAttr, .tp_richcompare = union_richcompare, .tp_methods = union_methods, .tp_members = union_members,
};
static int union_acceptable(PyObject *o) { return o == Py_None || PyType_Check(o) || Py_IS_TYPE(o, &PyGenericAlias_Type) || Py_IS_TYPE(o, &PyUnion_Type) || PyObject_HasAttrString(o, "__origin__") || PyObject_HasAttrString(o, "__typing_is_unpacked_typevartuple__") || !strcmp(Py_TYPE(o)->tp_name, "TypeVar") || !strcmp(Py_TYPE(o)->tp_name, "typing.TypeVar") || !strcmp(Py_TYPE(o)->tp_name, "ForwardRef") || !strcmp(Py_TYPE(o)->tp_name, "TypeAliasType") || !strcmp(Py_TYPE(o)->tp_name, "ParamSpec") || !strcmp(Py_TYPE(o)->tp_name, "TypeVarTuple") || !strcmp(Py_TYPE(o)->tp_name, "NewType"); }
PyObject *piper_union_type_new(PyObject *a, PyObject *b) {
    if (!union_acceptable(a) || !union_acceptable(b)) Py_RETURN_NOTIMPLEMENTED;
    PyObject *items = PyList_New(0);
    PyObject *srcs[2] = { a, b };
    for (int s = 0; s < 2; s++) {
        PyObject *src = srcs[s];
        if (Py_IS_TYPE(src, &PyUnion_Type)) { PyObject *args = ((unionobject *)src)->args; for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); i++) { PyObject *x = PyTuple_GET_ITEM(args, i); if (!PySequence_Contains(items, x)) PyList_Append(items, x); } }
        else { PyObject *x = src == Py_None ? (PyObject *)&_PyNone_Type : src; int c = PySequence_Contains(items, x); if (c < 0) { Py_DECREF(items); return NULL; } if (!c) PyList_Append(items, x); }
    }
    if (PyList_GET_SIZE(items) == 1) { PyObject *r = Py_NewRef(PyList_GET_ITEM(items, 0)); Py_DECREF(items); return r; }
    unionobject *u = PyObject_New(unionobject, &PyUnion_Type);
    if (!u) { Py_DECREF(items); return NULL; }
    u->args = PyList_AsTuple(items);
    Py_DECREF(items);
    return (PyObject *)u;
}
