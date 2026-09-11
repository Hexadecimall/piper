#include "piper/object.h"

static PyObject *answer(PyObject *module, PyObject *unused) {
    (void)module; (void)unused;
    return PyLong_FromLong(42);
}

static PyMethodDef methods[] = {
    { "answer", answer, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "minimal",
    NULL,
    -1,
    methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyObject *PyInit_minimal(void) { return PyModule_Create(&module); }
