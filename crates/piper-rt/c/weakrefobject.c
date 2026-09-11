#include "internal.h"

typedef struct _PyWeakReference {
    PyObject_HEAD
    PyObject *wr_object;
    PyObject *wr_callback;
    Py_hash_t hash;
    struct _PyWeakReference *wr_prev;
    struct _PyWeakReference *wr_next;
    vectorcallfunc vectorcall;
} PyWeakReference;

static PyObject **weaklist_slot(PyObject *object) {
    Py_ssize_t offset = Py_TYPE(object)->tp_weaklistoffset;
    if (!offset) return NULL;
    if (offset < 0) offset += Py_TYPE(object)->tp_basicsize;
    return (PyObject **)((char *)object + offset);
}

static void weakref_unlink(PyWeakReference *ref) {
    if (!ref->wr_object || ref->wr_object == Py_None) return;
    PyObject **head = weaklist_slot(ref->wr_object);
    if (ref->wr_prev) ref->wr_prev->wr_next = ref->wr_next;
    else if (head) *head = (PyObject *)ref->wr_next;
    if (ref->wr_next) ref->wr_next->wr_prev = ref->wr_prev;
    ref->wr_prev = NULL;
    ref->wr_next = NULL;
}

static void weakref_dealloc(PyObject *self) {
    PyWeakReference *ref = (PyWeakReference *)self;
    weakref_unlink(ref);
    Py_XDECREF(ref->wr_callback);
    PyObject_Free(self);
}

static PyObject *weakref_call(PyObject *self, PyObject *args, PyObject *kwargs) {
    PIPER_UNUSED(kwargs);
    if (PyTuple_GET_SIZE(args) != 0) { PyErr_SetString(PyExc_TypeError, "weakref expected 0 arguments"); return NULL; }
    PyObject *object = ((PyWeakReference *)self)->wr_object;
    return Py_NewRef(object && object != Py_None ? object : Py_None);
}

static Py_hash_t weakref_hash(PyObject *self) {
    PyWeakReference *ref = (PyWeakReference *)self;
    if (ref->hash != -1) return ref->hash;
    if (!ref->wr_object || ref->wr_object == Py_None) { PyErr_SetString(PyExc_TypeError, "weak object has gone away"); return -1; }
    ref->hash = PyObject_Hash(ref->wr_object);
    return ref->hash;
}

static PyObject *weakref_repr(PyObject *self) {
    PyWeakReference *ref = (PyWeakReference *)self;
    if (!ref->wr_object || ref->wr_object == Py_None) return PyUnicode_FromFormat("<weakref at %p; dead>", self);
    return PyUnicode_FromFormat("<weakref at %p; to '%s' at %p>", self, Py_TYPE(ref->wr_object)->tp_name, ref->wr_object);
}

static PyObject *weakref_new_for_type(PyTypeObject *type, PyObject *object, PyObject *callback) {
    PyObject **head = weaklist_slot(object);
    if (!head) {
        PyErr_Format(PyExc_TypeError, "cannot create weak reference to '%s' object", Py_TYPE(object)->tp_name);
        return NULL;
    }
    if (callback == Py_None) callback = NULL;
    if (callback && !PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable");
        return NULL;
    }
    PyWeakReference *ref = (PyWeakReference *)type->tp_alloc(type, 0);
    if (!ref) return NULL;
    ref->wr_object = object;
    ref->wr_callback = Py_XNewRef(callback);
    ref->hash = -1;
    ref->wr_prev = NULL;
    ref->wr_next = (PyWeakReference *)*head;
    ref->vectorcall = NULL;
    if (ref->wr_next) ref->wr_next->wr_prev = ref;
    *head = (PyObject *)ref;
    return (PyObject *)ref;
}

static PyObject *weakref_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *object = NULL, *callback = Py_None;
    if (PyTuple_GET_SIZE(args) > 0) object = PyTuple_GET_ITEM(args, 0);
    if (PyTuple_GET_SIZE(args) > 1) callback = PyTuple_GET_ITEM(args, 1);
    if (kwargs) {
        PyObject *value;
        if ((value = PyDict_GetItemString(kwargs, "object"))) object = value;
        if ((value = PyDict_GetItemString(kwargs, "callback"))) callback = value;
    }
    if (!object || PyTuple_GET_SIZE(args) > 2) { PyErr_SetString(PyExc_TypeError, "ref() takes at most 2 arguments"); return NULL; }
    return weakref_new_for_type(type, object, callback);
}

PyTypeObject _PyWeakref_RefType = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "weakref.ReferenceType",
    .tp_basicsize = sizeof(PyWeakReference),
    .tp_dealloc = weakref_dealloc,
    .tp_repr = weakref_repr,
    .tp_hash = weakref_hash,
    .tp_call = weakref_call,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = weakref_new,
    .tp_free = PyObject_Free,
};

PyObject *PyWeakref_NewRef(PyObject *object, PyObject *callback) {
    return weakref_new_for_type(&_PyWeakref_RefType, object, callback);
}

int PyWeakref_GetRef(PyObject *reference, PyObject **object) {
    if (!reference || !PyObject_TypeCheck(reference, &_PyWeakref_RefType)) {
        PyErr_SetString(PyExc_TypeError, "expected a weak reference");
        if (object) *object = NULL;
        return -1;
    }
    PyObject *value = ((PyWeakReference *)reference)->wr_object;
    if (!value || value == Py_None) { if (object) *object = NULL; return 0; }
    if (object) *object = Py_NewRef(value);
    return 1;
}

PyObject *PyWeakref_GetObject(PyObject *reference) {
    if (!reference || !PyObject_TypeCheck(reference, &_PyWeakref_RefType)) {
        PyErr_SetString(PyExc_TypeError, "expected a weak reference");
        return NULL;
    }
    PyObject *value = ((PyWeakReference *)reference)->wr_object;
    return value && value != Py_None ? value : Py_None;
}

void PyObject_ClearWeakRefs(PyObject *object) {
    PyObject **head = weaklist_slot(object);
    if (!head) return;
    while (*head) {
        PyWeakReference *ref = (PyWeakReference *)*head;
        PyObject *callback = ref->wr_callback ? Py_NewRef(ref->wr_callback) : NULL;
        Py_INCREF(ref);
        weakref_unlink(ref);
        ref->wr_object = Py_None;
        if (callback) {
            PyObject *result = PyObject_CallOneArg(callback, (PyObject *)ref);
            Py_DECREF(callback);
            if (!result) PyErr_WriteUnraisable((PyObject *)ref);
            else Py_DECREF(result);
        }
        Py_DECREF(ref);
    }
}

static PyObject *weakref_count(PyObject *module, PyObject *object) {
    PIPER_UNUSED(module);
    PyObject **head = weaklist_slot(object);
    Py_ssize_t count = 0;
    for (PyWeakReference *ref = head ? (PyWeakReference *)*head : NULL; ref; ref = ref->wr_next) count++;
    return PyLong_FromSsize_t(count);
}

static PyObject *weakref_refs(PyObject *module, PyObject *object) {
    PIPER_UNUSED(module);
    PyObject *result = PyList_New(0);
    if (!result) return NULL;
    PyObject **head = weaklist_slot(object);
    for (PyWeakReference *ref = head ? (PyWeakReference *)*head : NULL; ref; ref = ref->wr_next) {
        if (PyList_Append(result, (PyObject *)ref) < 0) { Py_DECREF(result); return NULL; }
    }
    return result;
}

static PyObject *weakref_proxy(PyObject *module, PyObject *const *args, Py_ssize_t count) {
    PIPER_UNUSED(module);
    if (piper_args_range("proxy", count, 1, 2) < 0) return NULL;
    return Py_NewRef(args[0]);
}

static PyObject *weakref_remove_dead(PyObject *module, PyObject *const *args, Py_ssize_t count) {
    PIPER_UNUSED(module);
    if (piper_args_range("_remove_dead_weakref", count, 2, 2) < 0) return NULL;
    PyObject *reference = PyDict_GetItem(args[0], args[1]);
    if (reference && Py_IS_TYPE(reference, &_PyWeakref_RefType) && PyWeakref_GetObject(reference) == Py_None) {
        if (PyDict_DelItem(args[0], args[1]) < 0) return NULL;
    }
    Py_RETURN_NONE;
}

static PyMethodDef weakref_methods[] = {
    { "getweakrefcount", weakref_count, METH_O, NULL },
    { "getweakrefs", weakref_refs, METH_O, NULL },
    { "proxy", (PyCFunction)(void (*)(void))weakref_proxy, METH_FASTCALL, NULL },
    { "_remove_dead_weakref", (PyCFunction)(void (*)(void))weakref_remove_dead, METH_FASTCALL, NULL },
    { NULL, NULL, 0, NULL },
};

void piper_init_weakref(void) {
    PyObject *module = piper_new_stdlib_module("_weakref");
    if (!module) return;
    PyModule_AddFunctions(module, weakref_methods);
    PyModule_AddObjectRef(module, "ref", (PyObject *)&_PyWeakref_RefType);
    PyModule_AddObjectRef(module, "ReferenceType", (PyObject *)&_PyWeakref_RefType);
    PyModule_AddObjectRef(module, "ProxyType", (PyObject *)&_PyWeakref_RefType);
    PyModule_AddObjectRef(module, "CallableProxyType", (PyObject *)&_PyWeakref_RefType);
}
