/* Runtime type exports used by the Python-level types module. */
#include "internal.h"

typedef struct { PyObject_HEAD PyObject *dict; } namespaceobject;

static PyObject *namespace_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    if (PyTuple_GET_SIZE(args)) { PyErr_SetString(PyExc_TypeError, "no positional arguments expected"); return NULL; }
    namespaceobject *self = PyObject_New(namespaceobject, type);
    if (!self) return NULL;
    self->dict = kwargs ? PyDict_Copy(kwargs) : PyDict_New();
    if (!self->dict) { Py_DECREF(self); return NULL; }
    return (PyObject *)self;
}

static void namespace_dealloc(PyObject *object) { Py_XDECREF(((namespaceobject *)object)->dict); PyObject_Free(object); }
static PyObject *namespace_repr(PyObject *object) {
    PyObject *items = PyDict_Items(((namespaceobject *)object)->dict);
    if (!items) return NULL;
    PyObject *parts = PyList_New(PyList_GET_SIZE(items));
    if (!parts) { Py_DECREF(items); return NULL; }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i++) {
        PyObject *item = PyList_GET_ITEM(items, i);
        PyObject *part = PyUnicode_FromFormat("%U=%R", PyTuple_GET_ITEM(item, 0), PyTuple_GET_ITEM(item, 1));
        if (!part) { Py_DECREF(items); Py_DECREF(parts); return NULL; }
        PyList_SET_ITEM(parts, i, part);
    }
    Py_DECREF(items);
    PyObject *separator = PyUnicode_FromString(", ");
    PyObject *body = separator ? PyUnicode_Join(separator, parts) : NULL;
    Py_XDECREF(separator); Py_DECREF(parts);
    if (!body) return NULL;
    PyObject *result = PyUnicode_FromFormat("namespace(%U)", body);
    Py_DECREF(body);
    return result;
}
static PyObject *namespace_richcompare(PyObject *left, PyObject *right, int op) {
    if (!Py_IS_TYPE(right, &PySimpleNamespace_Type)) return Py_NewRef(Py_NotImplemented);
    return PyObject_RichCompare(((namespaceobject *)left)->dict, ((namespaceobject *)right)->dict, op);
}

PyTypeObject PySimpleNamespace_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "types.SimpleNamespace", .tp_basicsize = sizeof(namespaceobject), .tp_dealloc = namespace_dealloc,
    .tp_repr = namespace_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_setattro = PyObject_GenericSetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE, .tp_dictoffset = offsetof(namespaceobject, dict),
    .tp_richcompare = namespace_richcompare, .tp_new = namespace_new,
};

static void add_type(PyObject *module, const char *name, PyTypeObject *type) { PyModule_AddObjectRef(module, name, (PyObject *)type); }

void piper_init_types_module(void) {
    PyObject *module = piper_new_stdlib_module("_types");
    if (!module) return;
    add_type(module, "FunctionType", &PyFunction_Type); add_type(module, "LambdaType", &PyFunction_Type);
    add_type(module, "CodeType", &PyCode_Type); add_type(module, "MappingProxyType", &PyDictProxy_Type);
    add_type(module, "SimpleNamespace", &PySimpleNamespace_Type); add_type(module, "CellType", &PyCell_Type);
    add_type(module, "GeneratorType", &PyGen_Type); add_type(module, "CoroutineType", &PyCoro_Type);
    add_type(module, "AsyncGeneratorType", &PyAsyncGen_Type); add_type(module, "MethodType", &PyMethod_Type);
    add_type(module, "BuiltinFunctionType", &PyCFunction_Type); add_type(module, "BuiltinMethodType", &PyCFunction_Type);
    add_type(module, "WrapperDescriptorType", &PyWrapperDescr_Type); add_type(module, "MethodWrapperType", &PyMethodWrapper_Type);
    add_type(module, "MethodDescriptorType", &PyMethodDescr_Type); add_type(module, "ClassMethodDescriptorType", &PyClassMethodDescr_Type);
    add_type(module, "ModuleType", &PyModule_Type); add_type(module, "TracebackType", &PyTraceBack_Type);
    add_type(module, "FrameType", &PyFrame_Type); add_type(module, "GetSetDescriptorType", &PyGetSetDescr_Type);
    add_type(module, "MemberDescriptorType", &PyMemberDescr_Type); add_type(module, "GenericAlias", &PyGenericAlias_Type);
    add_type(module, "UnionType", &PyUnion_Type); add_type(module, "EllipsisType", &PyEllipsis_Type);
    add_type(module, "NoneType", &_PyNone_Type); add_type(module, "NotImplementedType", &_PyNotImplemented_Type);
}
