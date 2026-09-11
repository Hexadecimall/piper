/* Python 3.14 template-string runtime objects. */
#include "internal.h"

typedef struct { PyObject_HEAD PyObject *value, *expression, *conversion, *format_spec; } interpolationobject;
typedef struct { PyObject_HEAD PyObject *strings, *interpolations, *values, *items; } templateobject;

static void interpolation_dealloc(PyObject *o) { interpolationobject *i = (interpolationobject *)o; Py_DECREF(i->value); Py_DECREF(i->expression); Py_DECREF(i->conversion); Py_DECREF(i->format_spec); PyObject_Free(o); }
static PyObject *interpolation_repr(PyObject *o) { interpolationobject *i = (interpolationobject *)o; return PyUnicode_FromFormat("Interpolation(%R, %R, %R, %R)", i->value, i->expression, i->conversion, i->format_spec); }
#define IGET(name) static PyObject *i_##name(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((interpolationobject *)o)->name); }
IGET(value) IGET(expression) IGET(conversion) IGET(format_spec)
static PyGetSetDef interpolation_getsets[] = { { "value", i_value, NULL, NULL, NULL }, { "expression", i_expression, NULL, NULL, NULL }, { "conversion", i_conversion, NULL, NULL, NULL }, { "format_spec", i_format_spec, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyInterpolation_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "string.templatelib.Interpolation", .tp_basicsize = sizeof(interpolationobject), .tp_dealloc = interpolation_dealloc, .tp_repr = interpolation_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_getset = interpolation_getsets };

static void template_dealloc(PyObject *o) { templateobject *t = (templateobject *)o; Py_DECREF(t->strings); Py_DECREF(t->interpolations); Py_DECREF(t->values); Py_DECREF(t->items); PyObject_Free(o); }
static PyObject *template_repr(PyObject *o) { templateobject *t = (templateobject *)o; return PyUnicode_FromFormat("Template(strings=%R, interpolations=%R)", t->strings, t->interpolations); }
static PyObject *template_iter(PyObject *o) { return PyObject_GetIter(((templateobject *)o)->items); }
#define TGET(name) static PyObject *t_##name(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((templateobject *)o)->name); }
TGET(strings) TGET(interpolations) TGET(values)
static PyGetSetDef template_getsets[] = { { "strings", t_strings, NULL, NULL, NULL }, { "interpolations", t_interpolations, NULL, NULL, NULL }, { "values", t_values, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyTemplate_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "string.templatelib.Template", .tp_basicsize = sizeof(templateobject), .tp_dealloc = template_dealloc, .tp_repr = template_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_getset = template_getsets, .tp_iter = template_iter };

PyObject *piper_interpolation_new(PyObject *value, PyObject *expression, int conversion, PyObject *format_spec) {
    interpolationobject *i = PyObject_New(interpolationobject, &PyInterpolation_Type); if (!i) return NULL;
    i->value = Py_NewRef(value); i->expression = Py_NewRef(expression);
    if (conversion < 0) i->conversion = Py_NewRef(Py_None); else { char c[2] = { (char)conversion, 0 }; i->conversion = PyUnicode_FromString(c); }
    i->format_spec = format_spec ? Py_NewRef(format_spec) : PyUnicode_FromString("");
    if (!i->conversion || !i->format_spec) { Py_DECREF(i); return NULL; }
    return (PyObject *)i;
}

PyObject *piper_template_new(PyObject *const *parts, Py_ssize_t count) {
    PyObject *strings = PyList_New(0), *interps = PyList_New(0), *values = PyList_New(0), *items = PyList_New(0);
    if (!strings || !interps || !values || !items) { Py_XDECREF(strings); Py_XDECREF(interps); Py_XDECREF(values); Py_XDECREF(items); return NULL; }
    int after_interp = 1;
    for (Py_ssize_t n = 0; n < count; n++) {
        PyObject *part = parts[n];
        if (Py_IS_TYPE(part, &PyInterpolation_Type)) {
            if (after_interp) { PyObject *empty = PyUnicode_FromString(""); PyList_Append(strings, empty); Py_DECREF(empty); }
            PyList_Append(interps, part); PyList_Append(values, ((interpolationobject *)part)->value); PyList_Append(items, part); after_interp = 1;
        } else { PyList_Append(strings, part); if (PyUnicode_GET_LENGTH(part)) PyList_Append(items, part); after_interp = 0; }
    }
    if (after_interp) { PyObject *empty = PyUnicode_FromString(""); PyList_Append(strings, empty); Py_DECREF(empty); }
    templateobject *t = PyObject_New(templateobject, &PyTemplate_Type);
    if (!t) { Py_DECREF(strings); Py_DECREF(interps); Py_DECREF(values); Py_DECREF(items); return NULL; }
    t->strings = PyList_AsTuple(strings); t->interpolations = PyList_AsTuple(interps); t->values = PyList_AsTuple(values); t->items = items;
    Py_DECREF(strings); Py_DECREF(interps); Py_DECREF(values); return (PyObject *)t;
}

static PyObject *template_convert(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m); if (piper_args_range("convert", n, 2, 2) < 0) return NULL; if (a[1] == Py_None) return Py_NewRef(a[0]);
    if (!PyUnicode_Check(a[1]) || PyUnicode_GET_LENGTH(a[1]) != 1) { PyErr_SetString(PyExc_ValueError, "invalid conversion specifier"); return NULL; }
    Py_UCS4 c = PyUnicode_READ_CHAR(a[1], 0); if (c == 's') return PyObject_Str(a[0]); if (c == 'r') return PyObject_Repr(a[0]); if (c == 'a') return PyObject_ASCII(a[0]);
    PyErr_SetString(PyExc_ValueError, "invalid conversion specifier"); return NULL;
}
static PyMethodDef methods[] = { { "convert", (PyCFunction)(void (*)(void))template_convert, METH_FASTCALL, NULL }, { NULL, NULL, 0, NULL } };
void piper_init_templatelib(void) {
    PyObject *module = piper_new_stdlib_module("string.templatelib"); if (!module) return;
    PyModule_AddObjectRef(module, "Template", (PyObject *)&PyTemplate_Type); PyModule_AddObjectRef(module, "Interpolation", (PyObject *)&PyInterpolation_Type); PyModule_AddFunctions(module, methods);
}
