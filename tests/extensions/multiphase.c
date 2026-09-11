#include "piper/object.h"

static int execute(PyObject *module) { return PyModule_AddIntConstant(module, "answer", 84); }

static PyModuleDef_Slot slots[] = {
    { Py_mod_exec, execute },
    { 0, NULL },
};

static PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "multiphase",
    NULL,
    8,
    NULL,
    slots,
    NULL,
    NULL,
    NULL,
};

PyObject *PyInit_multiphase(void) { return PyModuleDef_Init(&module); }
