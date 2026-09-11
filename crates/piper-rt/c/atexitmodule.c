#include "internal.h"

static PyObject *callbacks;
static int running;

static PyObject *atexit_register(PyObject *module, PyObject *args, PyObject *kwargs) {
    PIPER_UNUSED(module);
    if (PyTuple_GET_SIZE(args) < 1) {
        PyErr_SetString(PyExc_TypeError, "register() takes at least 1 argument (0 given)");
        return NULL;
    }
    PyObject *function = PyTuple_GET_ITEM(args, 0);
    if (!PyCallable_Check(function)) {
        PyErr_Format(PyExc_TypeError, "the first argument must be callable, not %s", Py_TYPE(function)->tp_name);
        return NULL;
    }
    PyObject *call_args = PyTuple_GetSlice(args, 1, PyTuple_GET_SIZE(args));
    if (!call_args) return NULL;
    PyObject *entry = PyTuple_Pack(3, function, call_args, kwargs ? kwargs : Py_None);
    Py_DECREF(call_args);
    if (!entry) return NULL;
    if (PyList_Append(callbacks, entry) < 0) { Py_DECREF(entry); return NULL; }
    Py_DECREF(entry);
    return Py_NewRef(function);
}

static PyObject *atexit_unregister(PyObject *module, PyObject *function) {
    PIPER_UNUSED(module);
    for (Py_ssize_t i = PyList_GET_SIZE(callbacks); i-- > 0;) {
        PyObject *entry = PyList_GET_ITEM(callbacks, i);
        int equal = PyObject_RichCompareBool(PyTuple_GET_ITEM(entry, 0), function, Py_EQ);
        if (equal < 0) return NULL;
        if (equal && PyList_SetSlice(callbacks, i, i + 1, NULL) < 0) return NULL;
    }
    Py_RETURN_NONE;
}

static void run_callbacks(void) {
    if (!callbacks || running) return;
    running = 1;
    while (PyList_GET_SIZE(callbacks) > 0) {
        Py_ssize_t index = PyList_GET_SIZE(callbacks) - 1;
        PyObject *entry = Py_NewRef(PyList_GET_ITEM(callbacks, index));
        if (PyList_SetSlice(callbacks, index, index + 1, NULL) < 0) { Py_DECREF(entry); PyErr_Print(); break; }
        PyObject *function = PyTuple_GET_ITEM(entry, 0);
        PyObject *args = PyTuple_GET_ITEM(entry, 1);
        PyObject *saved_kwargs = PyTuple_GET_ITEM(entry, 2);
        PyObject *result = PyObject_Call(function, args, saved_kwargs == Py_None ? NULL : saved_kwargs);
        if (!result) PyErr_Print();
        else Py_DECREF(result);
        Py_DECREF(entry);
    }
    running = 0;
}

static PyObject *atexit_run(PyObject *module, PyObject *unused) { PIPER_UNUSED(module); PIPER_UNUSED(unused); run_callbacks(); Py_RETURN_NONE; }
static PyObject *atexit_clear(PyObject *module, PyObject *unused) { PIPER_UNUSED(module); PIPER_UNUSED(unused); if (PyList_SetSlice(callbacks, 0, PyList_GET_SIZE(callbacks), NULL) < 0) return NULL; Py_RETURN_NONE; }
static PyObject *atexit_count(PyObject *module, PyObject *unused) { PIPER_UNUSED(module); PIPER_UNUSED(unused); return PyLong_FromSsize_t(PyList_GET_SIZE(callbacks)); }

static PyMethodDef atexit_methods[] = {
    { "register", (PyCFunction)(void (*)(void))atexit_register, METH_VARARGS | METH_KEYWORDS, NULL },
    { "unregister", atexit_unregister, METH_O, NULL },
    { "_run_exitfuncs", atexit_run, METH_NOARGS, NULL },
    { "_clear", atexit_clear, METH_NOARGS, NULL },
    { "_ncallbacks", atexit_count, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

void piper_init_atexit(void) {
    callbacks = PyList_New(0);
    if (!callbacks) return;
    PyObject *module = piper_new_stdlib_module("atexit");
    if (module) PyModule_AddFunctions(module, atexit_methods);
    Py_AtExit(run_callbacks);
}
