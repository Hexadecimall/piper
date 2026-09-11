/* Native support for abstract base classes. */
#include "internal.h"

static unsigned long long abc_token;

static PyObject *abc_attr(PyObject *cls, const char *name) {
    PyObject *value = PyObject_GetAttrString(cls, name);
    if (!value && PyErr_ExceptionMatches(PyExc_AttributeError)) PyErr_Clear();
    return value;
}

static int abc_set(PyObject *cls, const char *name, PyObject *value) {
    return PyObject_SetAttrString(cls, name, value);
}

static PyObject *abc_new_set(void) { return PySet_New(NULL); }

static PyObject *abc_init(PyObject *module, PyObject *cls) {
    PIPER_UNUSED(module);
    PyObject *abstracts = abc_new_set();
    PyObject *dict = abc_attr(cls, "__dict__");
    if (!abstracts || !dict) { Py_XDECREF(abstracts); Py_XDECREF(dict); return NULL; }
    PyObject *items = PyMapping_Items(dict);
    Py_DECREF(dict);
    if (!items) { Py_DECREF(abstracts); return NULL; }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i++) {
        PyObject *pair = PyList_GET_ITEM(items, i);
        PyObject *name = PyTuple_GET_ITEM(pair, 0), *value = PyTuple_GET_ITEM(pair, 1);
        PyObject *flag = abc_attr(value, "__isabstractmethod__");
        if (flag) {
            int yes = PyObject_IsTrue(flag);
            Py_DECREF(flag);
            if (yes < 0 || (yes && PySet_Add(abstracts, name) < 0)) { Py_DECREF(items); Py_DECREF(abstracts); return NULL; }
        } else if (PyErr_Occurred()) { Py_DECREF(items); Py_DECREF(abstracts); return NULL; }
    }
    Py_DECREF(items);
    PyObject *bases = abc_attr(cls, "__bases__");
    for (Py_ssize_t i = 0; bases && i < PyTuple_GET_SIZE(bases); i++) {
        PyObject *inherited = abc_attr(PyTuple_GET_ITEM(bases, i), "__abstractmethods__");
        PyObject *it = inherited ? PyObject_GetIter(inherited) : NULL;
        Py_XDECREF(inherited);
        if (!it && PyErr_Occurred()) { Py_XDECREF(bases); Py_DECREF(abstracts); return NULL; }
        PyObject *name;
        while (it && (name = PyIter_Next(it))) {
            PyObject *value = PyObject_GetAttr(cls, name);
            PyObject *flag = value ? abc_attr(value, "__isabstractmethod__") : NULL;
            Py_XDECREF(value);
            int yes = flag ? PyObject_IsTrue(flag) : 0;
            Py_XDECREF(flag);
            if (yes > 0) PySet_Add(abstracts, name);
            Py_DECREF(name);
        }
        Py_XDECREF(it);
    }
    Py_XDECREF(bases);
    PyObject *frozen = PyFrozenSet_New(abstracts);
    Py_DECREF(abstracts);
    if (!frozen) return NULL;
    int rc = abc_set(cls, "__abstractmethods__", frozen);
    Py_DECREF(frozen);
    if (rc < 0) return NULL;
    const char *names[] = { "_abc_registry", "_abc_cache", "_abc_negative_cache", NULL };
    for (int i = 0; names[i]; i++) {
        PyObject *set = abc_new_set();
        if (!set || abc_set(cls, names[i], set) < 0) { Py_XDECREF(set); return NULL; }
        Py_DECREF(set);
    }
    PyObject *version = PyLong_FromUnsignedLongLong(abc_token);
    rc = version ? abc_set(cls, "_abc_negative_cache_version", version) : -1;
    Py_XDECREF(version);
    if (rc < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *abc_register(PyObject *module, PyObject *const *args, Py_ssize_t nargs) {
    PIPER_UNUSED(module);
    if (piper_args_range("_abc_register", nargs, 2, 2) < 0) return NULL;
    if (!PyType_Check(args[1])) { PyErr_SetString(PyExc_TypeError, "Can only register classes"); return NULL; }
    if (PyObject_IsSubclass(args[1], args[0]) > 0) return Py_NewRef(args[1]);
    PyObject *registry = abc_attr(args[0], "_abc_registry");
    if (!registry) return NULL;
    int rc = PySet_Add(registry, args[1]);
    Py_DECREF(registry);
    if (rc < 0) return NULL;
    abc_token++;
    return Py_NewRef(args[1]);
}

static int abc_subclass(PyObject *cls, PyObject *sub) {
    if (!PyType_Check(sub)) { PyErr_SetString(PyExc_TypeError, "issubclass() arg 1 must be a class"); return -1; }
    if (PyType_Check(cls) && PyType_IsSubtype((PyTypeObject *)sub, (PyTypeObject *)cls)) return 1;
    PyObject *registry = abc_attr(cls, "_abc_registry");
    PyObject *it = registry ? PyObject_GetIter(registry) : NULL;
    Py_XDECREF(registry);
    if (!it) return PyErr_Occurred() ? -1 : 0;
    PyObject *entry;
    while ((entry = PyIter_Next(it))) {
        int yes = PyObject_IsSubclass(sub, entry);
        Py_DECREF(entry);
        if (yes) { Py_DECREF(it); return yes; }
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}

static PyObject *abc_subclasscheck(PyObject *module, PyObject *const *args, Py_ssize_t nargs) {
    PIPER_UNUSED(module);
    if (piper_args_range("_abc_subclasscheck", nargs, 2, 2) < 0) return NULL;
    int result = abc_subclass(args[0], args[1]);
    if (result < 0) return NULL;
    return PyBool_FromLong(result);
}

static PyObject *abc_instancecheck(PyObject *module, PyObject *const *args, Py_ssize_t nargs) {
    PIPER_UNUSED(module);
    if (piper_args_range("_abc_instancecheck", nargs, 2, 2) < 0) return NULL;
    int result = abc_subclass(args[0], (PyObject *)Py_TYPE(args[1]));
    if (result < 0) return NULL;
    return PyBool_FromLong(result);
}

static PyObject *abc_token_get(PyObject *module, PyObject *unused) { PIPER_UNUSED(module); PIPER_UNUSED(unused); return PyLong_FromUnsignedLongLong(abc_token); }
static PyObject *abc_reset_registry(PyObject *module, PyObject *cls) { PIPER_UNUSED(module); PyObject *set = abc_new_set(); if (!set) return NULL; int rc = abc_set(cls, "_abc_registry", set); Py_DECREF(set); if (rc < 0) return NULL; Py_RETURN_NONE; }
static PyObject *abc_reset_caches(PyObject *module, PyObject *cls) { PIPER_UNUSED(module); const char *names[] = { "_abc_cache", "_abc_negative_cache" }; for (int i = 0; i < 2; i++) { PyObject *set = abc_new_set(); if (!set) return NULL; int rc = abc_set(cls, names[i], set); Py_DECREF(set); if (rc < 0) return NULL; } Py_RETURN_NONE; }
static PyObject *abc_dump(PyObject *module, PyObject *cls) {
    PIPER_UNUSED(module); PyObject *r = PyTuple_New(4); if (!r) return NULL;
    PyObject *registry = abc_attr(cls, "_abc_registry"), *cache = abc_attr(cls, "_abc_cache"), *negative = abc_attr(cls, "_abc_negative_cache");
    PyObject *version = abc_attr(cls, "_abc_negative_cache_version");
    if (!registry || !cache || !negative || !version) { Py_XDECREF(registry); Py_XDECREF(cache); Py_XDECREF(negative); Py_XDECREF(version); Py_DECREF(r); return NULL; }
    PyTuple_SET_ITEM(r, 0, registry); PyTuple_SET_ITEM(r, 1, cache); PyTuple_SET_ITEM(r, 2, negative); PyTuple_SET_ITEM(r, 3, version); return r;
}

static PyMethodDef abc_methods[] = {
    { "get_cache_token", abc_token_get, METH_NOARGS, NULL }, { "_abc_init", abc_init, METH_O, NULL },
    { "_abc_register", (PyCFunction)(void (*)(void))abc_register, METH_FASTCALL, NULL },
    { "_abc_instancecheck", (PyCFunction)(void (*)(void))abc_instancecheck, METH_FASTCALL, NULL },
    { "_abc_subclasscheck", (PyCFunction)(void (*)(void))abc_subclasscheck, METH_FASTCALL, NULL },
    { "_get_dump", abc_dump, METH_O, NULL }, { "_reset_registry", abc_reset_registry, METH_O, NULL },
    { "_reset_caches", abc_reset_caches, METH_O, NULL }, { NULL, NULL, 0, NULL },
};

void piper_init_abc(void) { PyObject *module = piper_new_stdlib_module("_abc"); if (module) PyModule_AddFunctions(module, abc_methods); }
