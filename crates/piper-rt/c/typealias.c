/* Lazy runtime objects for Python type statements. */
#include "internal.h"

typedef struct {
    PyObject_HEAD
    PyObject *name;
    PyObject *thunk;
    PyObject *value;
    PyObject *type_params;
} typealiasobject;

typedef struct { PyObject_HEAD PyObject *name; PyObject *bound; PyObject *default_value; int kind; } typeparamobject;
typedef struct { PyObject_HEAD } nodefaultobject;
static PyObject *nodefault_repr(PyObject *o) { PIPER_UNUSED(o); return PyUnicode_FromString("typing.NoDefault"); }
static PyTypeObject PyNoDefault_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "typing._NoDefaultType", .tp_basicsize = sizeof(nodefaultobject), .tp_repr = nodefault_repr, .tp_getattro = PyObject_GenericGetAttr };
static PyObject PyNoDefault = { { .ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, &PyNoDefault_Type };
static void typeparam_dealloc(PyObject *o) { typeparamobject *parameter = (typeparamobject *)o; Py_XDECREF(parameter->name); Py_XDECREF(parameter->bound); Py_XDECREF(parameter->default_value); PyObject_Free(o); }
static PyObject *typeparam_repr(PyObject *o) { return Py_NewRef(((typeparamobject *)o)->name); }
static PyObject *typeparam_name(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((typeparamobject *)o)->name); }
static PyObject *typeparam_bound(PyObject *o, void *c) { PIPER_UNUSED(c); typeparamobject *parameter = (typeparamobject *)o; return Py_NewRef(parameter->bound ? parameter->bound : Py_None); }
static PyObject *typeparam_default(PyObject *o, void *c) { PIPER_UNUSED(c); typeparamobject *parameter = (typeparamobject *)o; return Py_NewRef(parameter->default_value ? parameter->default_value : &PyNoDefault); }
static PyObject *typeparam_constraints(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); return PyTuple_New(0); }
static PyObject *typeparam_false(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); Py_RETURN_FALSE; }
static PyObject *typeparam_infer(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); Py_RETURN_TRUE; }
static PyObject *typeparam_has_default(PyObject *o, PyObject *unused) { PIPER_UNUSED(unused); Py_RETURN_BOOL(((typeparamobject *)o)->default_value != NULL); }
static PyMethodDef typeparam_methods[] = { { "has_default", typeparam_has_default, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyGetSetDef typeparam_getsets[] = {
    { "__name__", typeparam_name, NULL, NULL, NULL }, { "__bound__", typeparam_bound, NULL, NULL, NULL },
    { "__constraints__", typeparam_constraints, NULL, NULL, NULL }, { "__default__", typeparam_default, NULL, NULL, NULL },
    { "__covariant__", typeparam_false, NULL, NULL, NULL }, { "__contravariant__", typeparam_false, NULL, NULL, NULL },
    { "__infer_variance__", typeparam_infer, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL }
};
PyTypeObject PyTypeVar_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "typing.TypeVar", .tp_basicsize = sizeof(typeparamobject), .tp_dealloc = typeparam_dealloc, .tp_repr = typeparam_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_methods = typeparam_methods, .tp_getset = typeparam_getsets };
PyTypeObject PyParamSpec_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "typing.ParamSpec", .tp_basicsize = sizeof(typeparamobject), .tp_dealloc = typeparam_dealloc, .tp_repr = typeparam_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_methods = typeparam_methods, .tp_getset = typeparam_getsets };
PyTypeObject PyTypeVarTuple_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "typing.TypeVarTuple", .tp_basicsize = sizeof(typeparamobject), .tp_dealloc = typeparam_dealloc, .tp_repr = typeparam_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_methods = typeparam_methods, .tp_getset = typeparam_getsets };

PyObject *piper_type_param_new(PyObject *name, int kind, PyObject *bound, PyObject *default_value) {
    PyTypeObject *type = kind == 1 ? &PyParamSpec_Type : kind == 2 ? &PyTypeVarTuple_Type : &PyTypeVar_Type;
    typeparamobject *parameter = PyObject_New(typeparamobject, type);
    if (!parameter) return NULL;
    parameter->name = Py_NewRef(name); parameter->bound = Py_XNewRef(bound); parameter->default_value = Py_XNewRef(default_value); parameter->kind = kind;
    return (PyObject *)parameter;
}

static void typealias_dealloc(PyObject *o) {
    typealiasobject *alias = (typealiasobject *)o;
    Py_XDECREF(alias->name); Py_XDECREF(alias->thunk); Py_XDECREF(alias->value); Py_XDECREF(alias->type_params);
    PyObject_Free(o);
}
static PyObject *typealias_repr(PyObject *o) { return Py_NewRef(((typealiasobject *)o)->name); }
static PyObject *typealias_name(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((typealiasobject *)o)->name); }
static PyObject *typealias_module(PyObject *o, void *c) {
    PIPER_UNUSED(c); typealiasobject *alias = (typealiasobject *)o;
    return alias->thunk ? PyObject_GetAttrString(alias->thunk, "__module__") : Py_NewRef(Py_None);
}
static PyObject *typealias_params(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((typealiasobject *)o)->type_params); }
static PyObject *typealias_value(PyObject *o, void *c) {
    PIPER_UNUSED(c); typealiasobject *alias = (typealiasobject *)o;
    if (!alias->value) {
        alias->value = PyTuple_GET_SIZE(alias->type_params) ? PyObject_Call(alias->thunk, alias->type_params, NULL) : PyObject_CallNoArgs(alias->thunk);
        if (!alias->value) return NULL;
        Py_CLEAR(alias->thunk);
    }
    return Py_NewRef(alias->value);
}
static PyGetSetDef typealias_getsets[] = {
    { "__name__", typealias_name, NULL, NULL, NULL }, { "__module__", typealias_module, NULL, NULL, NULL },
    { "__type_params__", typealias_params, NULL, NULL, NULL }, { "__value__", typealias_value, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};
PyTypeObject PyTypeAlias_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "typing.TypeAliasType", .tp_basicsize = sizeof(typealiasobject), .tp_dealloc = typealias_dealloc,
    .tp_repr = typealias_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_getset = typealias_getsets,
};

PyObject *piper_type_alias_new(PyObject *name, PyObject *thunk, PyObject *type_params) {
    typealiasobject *alias = PyObject_New(typealiasobject, &PyTypeAlias_Type);
    if (!alias) return NULL;
    alias->name = Py_NewRef(name); alias->thunk = Py_NewRef(thunk); alias->value = NULL;
    alias->type_params = type_params ? Py_NewRef(type_params) : PyTuple_New(0);
    if (!alias->type_params) { Py_DECREF(alias); return NULL; }
    return (PyObject *)alias;
}

void piper_init_typealias(void) {
    PyType_Ready(&PyNoDefault_Type);
    PyType_Ready(&PyTypeAlias_Type);
    PyType_Ready(&PyTypeVar_Type); PyType_Ready(&PyParamSpec_Type); PyType_Ready(&PyTypeVarTuple_Type);
    PyObject *module = piper_new_stdlib_module("typing");
    if (module) {
        PyObject *dict = PyModule_GetDict(module);
        PyDict_SetItemString(dict, "TypeAliasType", (PyObject *)&PyTypeAlias_Type);
        PyDict_SetItemString(dict, "TypeVar", (PyObject *)&PyTypeVar_Type);
        PyDict_SetItemString(dict, "ParamSpec", (PyObject *)&PyParamSpec_Type);
        PyDict_SetItemString(dict, "TypeVarTuple", (PyObject *)&PyTypeVarTuple_Type);
        PyDict_SetItemString(dict, "NoDefault", &PyNoDefault);
    }
}
