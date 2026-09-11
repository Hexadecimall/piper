/* Compiled piper functions, cells, and argument binding. */
#include "internal.h"

static PyObject *function_vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    PiperFunctionObject *fn = (PiperFunctionObject *)f;
    return fn->fn(f, args, PyVectorcall_NARGS(nargsf), kwnames);
}

PyObject *piper_function_new(piper_native_fn fn, PyObject *name, PyObject *qualname, PyObject *globals, PyObject *argnames,
                             int posonly, int argcount, int kwonly, int flags, PyObject *defaults, PyObject *kwdefaults, PyObject *closure) {
    PiperFunctionObject *f = PyObject_New(PiperFunctionObject, &PyFunction_Type);
    if (!f) return NULL;
    f->fn = fn;
    f->name = Py_NewRef(name);
    f->qualname = Py_NewRef(qualname ? qualname : name);
    f->globals = Py_NewRef(globals);
    f->builtins = Py_NewRef(piper_builtins());
    PyObject *modname = PyDict_GetItemString(globals, "__name__");
    f->module_name = Py_NewRef(modname ? modname : Py_None);
    f->defaults = Py_XNewRef(defaults);
    f->kwdefaults = Py_XNewRef(kwdefaults);
    f->closure = Py_XNewRef(closure);
    f->annotations = NULL;
    f->dict = NULL;
    f->doc = NULL;
    f->argnames = Py_NewRef(argnames);
    f->posonly_count = posonly;
    f->arg_count = argcount;
    f->kwonly_count = kwonly;
    f->has_varargs = (flags & PIPER_FN_VARARGS) != 0;
    f->has_varkw = (flags & PIPER_FN_VARKW) != 0;
    f->is_generator = (flags & PIPER_FN_GENERATOR) != 0;
    f->is_coroutine = (flags & PIPER_FN_COROUTINE) != 0;
    f->is_async_generator = (flags & PIPER_FN_ASYNC_GENERATOR) != 0;
    f->vectorcall = function_vectorcall;
    return (PyObject *)f;
}

static void function_dealloc(PyObject *o) {
    PiperFunctionObject *f = (PiperFunctionObject *)o;
    Py_XDECREF(f->name); Py_XDECREF(f->qualname); Py_XDECREF(f->module_name); Py_XDECREF(f->globals); Py_XDECREF(f->builtins);
    Py_XDECREF(f->defaults); Py_XDECREF(f->kwdefaults); Py_XDECREF(f->closure); Py_XDECREF(f->annotations); Py_XDECREF(f->dict);
    Py_XDECREF(f->doc); Py_XDECREF(f->argnames);
    PyObject_Free(o);
}

static PyObject *function_repr(PyObject *o) { return PyUnicode_FromFormat("<function %U at %p>", ((PiperFunctionObject *)o)->qualname, o); }
static PyObject *function_get(PyObject *self, PyObject *obj, PyObject *type) { PIPER_UNUSED(type); if (!obj || obj == Py_None) return Py_NewRef(self); return PyMethod_New(self, obj); }

/* Bind (args, kwnames) into slots following Python's rules. Slot layout:
 * [positional params][kwonly params][*args tuple][**kwargs dict]. Returns
 * the number of slots written (owned references), or -1. */
int piper_bind_args(PiperFunctionObject *f, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames, PyObject **slots) {
    Py_ssize_t argcount = f->arg_count, kwonly = f->kwonly_count, total = argcount + kwonly;
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    Py_ssize_t nslots = total + f->has_varargs + f->has_varkw;
    for (Py_ssize_t i = 0; i < nslots; i++) slots[i] = NULL;
    Py_ssize_t n = nargs < argcount ? nargs : argcount;
    for (Py_ssize_t i = 0; i < n; i++) slots[i] = Py_NewRef(args[i]);
    if (nargs > argcount) {
        if (!f->has_varargs) {
            Py_ssize_t ndef = f->defaults ? PyTuple_GET_SIZE(f->defaults) : 0;
            if (ndef) PyErr_Format(PyExc_TypeError, "%U() takes from %zd to %zd positional arguments but %zd were given", f->qualname, argcount - ndef, argcount, nargs);
            else PyErr_Format(PyExc_TypeError, "%U() takes %zd positional argument%s but %zd %s given", f->qualname, argcount, argcount == 1 ? "" : "s", nargs, nargs == 1 ? "was" : "were");
            goto fail;
        }
        slots[total] = PyTuple_FromArray(args + argcount, nargs - argcount);
    } else if (f->has_varargs) slots[total] = PyTuple_New(0);
    PyObject *kwdict = NULL;
    if (f->has_varkw) { kwdict = PyDict_New(); slots[total + f->has_varargs] = kwdict; }
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *key = PyTuple_GET_ITEM(kwnames, i);
        PyObject *val = args[nargs + i];
        Py_ssize_t j = -1;
        for (Py_ssize_t k = f->posonly_count; k < total; k++) {
            PyObject *pn = PyTuple_GET_ITEM(f->argnames, k);
            if (pn == key || (PyUnicode_Check(key) && piper_unicode_eq(pn, key))) { j = k; break; }
        }
        if (j >= 0) {
            if (slots[j]) { PyErr_Format(PyExc_TypeError, "%U() got multiple values for argument '%U'", f->qualname, key); goto fail; }
            slots[j] = Py_NewRef(val);
        } else if (kwdict) {
            if (PyDict_SetItem(kwdict, key, val) < 0) goto fail;
        } else {
            int posonly_hit = 0;
            for (Py_ssize_t k = 0; k < f->posonly_count; k++) if (piper_unicode_eq(PyTuple_GET_ITEM(f->argnames, k), key)) posonly_hit = 1;
            if (posonly_hit) PyErr_Format(PyExc_TypeError, "%U() got some positional-only arguments passed as keyword arguments: '%U'", f->qualname, key);
            else PyErr_Format(PyExc_TypeError, "%U() got an unexpected keyword argument '%U'", f->qualname, key);
            goto fail;
        }
    }
    /* defaults */
    Py_ssize_t ndef = f->defaults ? PyTuple_GET_SIZE(f->defaults) : 0;
    for (Py_ssize_t i = 0; i < argcount; i++) {
        if (slots[i]) continue;
        Py_ssize_t di = i - (argcount - ndef);
        if (di >= 0) { slots[i] = Py_NewRef(PyTuple_GET_ITEM(f->defaults, di)); continue; }
        /* missing positional */
        Py_ssize_t missing = 0;
        for (Py_ssize_t k = 0; k < argcount - ndef; k++) if (!slots[k]) missing++;
        PyObject *names = PyList_New(0);
        for (Py_ssize_t k = 0; k < argcount - ndef; k++) if (!slots[k]) { PyObject *r = PyObject_Repr(PyTuple_GET_ITEM(f->argnames, k)); PyList_Append(names, r); Py_DECREF(r); }
        PyObject *joined;
        if (missing == 1) joined = Py_NewRef(PyList_GET_ITEM(names, 0));
        else if (missing == 2) joined = PyUnicode_FromFormat("%U and %U", PyList_GET_ITEM(names, 0), PyList_GET_ITEM(names, 1));
        else {
            PyObject *sep = PyUnicode_FromString(", ");
            PyObject *head = _PyUnicode_JoinArray(sep, PyList_ITEMS(names), missing - 1);
            joined = PyUnicode_FromFormat("%U, and %U", head, PyList_GET_ITEM(names, missing - 1));
            Py_DECREF(sep); Py_DECREF(head);
        }
        PyErr_Format(PyExc_TypeError, "%U() missing %zd required positional argument%s: %U", f->qualname, missing, missing == 1 ? "" : "s", joined);
        Py_DECREF(names); Py_DECREF(joined);
        goto fail;
    }
    for (Py_ssize_t i = argcount; i < total; i++) {
        if (slots[i]) continue;
        PyObject *pn = PyTuple_GET_ITEM(f->argnames, i);
        PyObject *d = f->kwdefaults ? PyDict_GetItemWithError(f->kwdefaults, pn) : NULL;
        if (d) { slots[i] = Py_NewRef(d); continue; }
        if (PyErr_Occurred()) goto fail;
        Py_ssize_t missing = 0;
        for (Py_ssize_t k = argcount; k < total; k++) if (!slots[k] && !(f->kwdefaults && PyDict_GetItem(f->kwdefaults, PyTuple_GET_ITEM(f->argnames, k)))) missing++;
        PyErr_Format(PyExc_TypeError, "%U() missing %zd required keyword-only argument%s: '%U'", f->qualname, missing, missing == 1 ? "" : "s", pn);
        goto fail;
    }
    return (int)nslots;
fail:
    for (Py_ssize_t i = 0; i < nslots; i++) Py_CLEAR(slots[i]);
    return -1;
}

static PyObject *func_name_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((PiperFunctionObject *)o)->name); }
static int func_name_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (!v || !PyUnicode_Check(v)) { PyErr_SetString(PyExc_TypeError, "__name__ must be set to a string object"); return -1; } Py_SETREF(((PiperFunctionObject *)o)->name, Py_NewRef(v)); return 0; }
static PyObject *func_qualname_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((PiperFunctionObject *)o)->qualname); }
static int func_qualname_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (!v || !PyUnicode_Check(v)) { PyErr_SetString(PyExc_TypeError, "__qualname__ must be set to a string object"); return -1; } Py_SETREF(((PiperFunctionObject *)o)->qualname, Py_NewRef(v)); return 0; }
static PyObject *func_doc_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *d = ((PiperFunctionObject *)o)->doc; return Py_NewRef(d ? d : Py_None); }
static int func_doc_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); Py_XSETREF(((PiperFunctionObject *)o)->doc, Py_XNewRef(v)); return 0; }
static PyObject *func_module_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((PiperFunctionObject *)o)->module_name); }
static int func_module_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); Py_SETREF(((PiperFunctionObject *)o)->module_name, Py_NewRef(v ? v : Py_None)); return 0; }
static PyObject *func_defaults_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *d = ((PiperFunctionObject *)o)->defaults; return Py_NewRef(d && PyTuple_GET_SIZE(d) ? d : Py_None); }
static int func_defaults_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (v == Py_None) v = NULL; if (v && !PyTuple_Check(v)) { PyErr_SetString(PyExc_TypeError, "__defaults__ must be set to a tuple object"); return -1; } Py_XSETREF(((PiperFunctionObject *)o)->defaults, Py_XNewRef(v)); return 0; }
static PyObject *func_kwdefaults_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *d = ((PiperFunctionObject *)o)->kwdefaults; return Py_NewRef(d && PyDict_Size(d) ? d : Py_None); }
static int func_kwdefaults_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (v == Py_None) v = NULL; if (v && !PyDict_Check(v)) { PyErr_SetString(PyExc_TypeError, "__kwdefaults__ must be set to a dict object"); return -1; } Py_XSETREF(((PiperFunctionObject *)o)->kwdefaults, Py_XNewRef(v)); return 0; }
static PyObject *func_globals_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((PiperFunctionObject *)o)->globals); }
static PyObject *func_builtins_get(PyObject *o, void *c) { PIPER_UNUSED(c); return Py_NewRef(((PiperFunctionObject *)o)->builtins); }
static PyObject *func_closure_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *cl = ((PiperFunctionObject *)o)->closure; return Py_NewRef(cl ? cl : Py_None); }
static PyObject *func_annotations_get(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    PiperFunctionObject *f = (PiperFunctionObject *)o;
    if (!f->annotations) {
        PyObject *annotate = f->dict ? PyDict_GetItemString(f->dict, "__annotate__") : NULL;
        if (annotate && annotate != Py_None) { PyObject *one = PyLong_FromLong(1); f->annotations = PyObject_CallOneArg(annotate, one); Py_DECREF(one); if (!f->annotations) return NULL; }
        else f->annotations = PyDict_New();
    }
    return Py_NewRef(f->annotations);
}
static int func_annotations_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (v == Py_None) v = NULL; if (v && !PyDict_Check(v)) { PyErr_SetString(PyExc_TypeError, "__annotations__ must be set to a dict object"); return -1; } Py_XSETREF(((PiperFunctionObject *)o)->annotations, Py_XNewRef(v)); return 0; }
static PyObject *func_annotate_get(PyObject *o, void *c) { PIPER_UNUSED(c); PiperFunctionObject *f = (PiperFunctionObject *)o; PyObject *a = f->dict ? PyDict_GetItemString(f->dict, "__annotate__") : NULL; return Py_NewRef(a ? a : Py_None); }
static int func_annotate_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); PiperFunctionObject *f = (PiperFunctionObject *)o; if (!f->dict) f->dict = PyDict_New(); Py_CLEAR(f->annotations); return PyDict_SetItemString(f->dict, "__annotate__", v ? v : Py_None); }
static PyObject *func_code_get(PyObject *o, void *c) { PIPER_UNUSED(c); extern PyObject *piper_code_for_function(PyObject *f); return piper_code_for_function(o); }
static PyObject *func_type_params_get(PyObject *o, void *c) { PIPER_UNUSED(c); PiperFunctionObject *f = (PiperFunctionObject *)o; PyObject *t = f->dict ? PyDict_GetItemString(f->dict, "__type_params__") : NULL; return t ? Py_NewRef(t) : PyTuple_New(0); }
static int func_type_params_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); PiperFunctionObject *f = (PiperFunctionObject *)o; if (!f->dict) f->dict = PyDict_New(); return PyDict_SetItemString(f->dict, "__type_params__", v); }

static PyGetSetDef function_getsets[] = {
    { "__name__", func_name_get, func_name_set, NULL, NULL }, { "__qualname__", func_qualname_get, func_qualname_set, NULL, NULL },
    { "__doc__", func_doc_get, func_doc_set, NULL, NULL }, { "__module__", func_module_get, func_module_set, NULL, NULL },
    { "__defaults__", func_defaults_get, func_defaults_set, NULL, NULL }, { "__kwdefaults__", func_kwdefaults_get, func_kwdefaults_set, NULL, NULL },
    { "__globals__", func_globals_get, NULL, NULL, NULL }, { "__builtins__", func_builtins_get, NULL, NULL, NULL }, { "__closure__", func_closure_get, NULL, NULL, NULL },
    { "__annotations__", func_annotations_get, func_annotations_set, NULL, NULL }, { "__annotate__", func_annotate_get, func_annotate_set, NULL, NULL },
    { "__code__", func_code_get, NULL, NULL, NULL }, { "__type_params__", func_type_params_get, func_type_params_set, NULL, NULL },
    { "__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};
PyTypeObject PyFunction_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "function", .tp_basicsize = sizeof(PiperFunctionObject), .tp_dealloc = function_dealloc,
    .tp_vectorcall_offset = offsetof(PiperFunctionObject, vectorcall), .tp_repr = function_repr, .tp_call = PyVectorcall_Call,
    .tp_getattro = PyObject_GenericGetAttr, .tp_setattro = PyObject_GenericSetAttr, .tp_flags = Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_METHOD_DESCRIPTOR,
    .tp_getset = function_getsets, .tp_descr_get = function_get, .tp_dictoffset = offsetof(PiperFunctionObject, dict),
};

/* ---- cells ------------------------------------------------------------------ */

static void cell_dealloc(PyObject *o) { Py_XDECREF(((PyCellObject *)o)->ob_ref); PyObject_Free(o); }
static PyObject *cell_repr(PyObject *o) { PyCellObject *c = (PyCellObject *)o; if (!c->ob_ref) return PyUnicode_FromFormat("<cell at %p: empty>", o); return PyUnicode_FromFormat("<cell at %p: %s object at %p>", o, Py_TYPE(c->ob_ref)->tp_name, c->ob_ref); }
static PyObject *cell_contents_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *v = ((PyCellObject *)o)->ob_ref; if (!v) { PyErr_SetString(PyExc_ValueError, "Cell is empty"); return NULL; } return Py_NewRef(v); }
static int cell_contents_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); Py_XSETREF(((PyCellObject *)o)->ob_ref, Py_XNewRef(v)); return 0; }
static PyObject *cell_richcompare(PyObject *a, PyObject *b, int op) {
    if (!Py_IS_TYPE(b, &PyCell_Type)) Py_RETURN_NOTIMPLEMENTED;
    PyObject *x = ((PyCellObject *)a)->ob_ref, *y = ((PyCellObject *)b)->ob_ref;
    if (x && y) return PyObject_RichCompare(x, y, op);
    int c = (x != NULL) - (y != NULL);
    int r;
    switch (op) { case Py_LT: r = c < 0; break; case Py_LE: r = c <= 0; break; case Py_EQ: r = c == 0; break; case Py_NE: r = c != 0; break; case Py_GT: r = c > 0; break; default: r = c >= 0; }
    Py_RETURN_BOOL(r);
}
static PyGetSetDef cell_getsets[] = { { "cell_contents", cell_contents_get, cell_contents_set, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyCell_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "cell", .tp_basicsize = sizeof(PyCellObject), .tp_dealloc = cell_dealloc, .tp_repr = cell_repr, .tp_getattro = PyObject_GenericGetAttr,
    .tp_richcompare = cell_richcompare, .tp_getset = cell_getsets,
};
PyObject *PyCell_New(PyObject *v) { PyCellObject *c = PyObject_New(PyCellObject, &PyCell_Type); if (!c) return NULL; c->ob_ref = Py_XNewRef(v); return (PyObject *)c; }
PyObject *PyCell_Get(PyObject *c) { return Py_XNewRef(((PyCellObject *)c)->ob_ref); }
int PyCell_Set(PyObject *c, PyObject *v) { Py_XSETREF(((PyCellObject *)c)->ob_ref, Py_XNewRef(v)); return 0; }
PyObject *piper_cell_new(PyObject *v) { return PyCell_New(v); }
void piper_cell_set(PyObject *cell, PyObject *v) { PyCell_Set(cell, v); }
PyObject *piper_cell_get(PyObject *cell, PyObject *name, int is_free) {
    PyObject *v = ((PyCellObject *)cell)->ob_ref;
    if (!v) {
        if (is_free) PyErr_Format(PyExc_NameError, "cannot access free variable '%U' where it is not associated with a value in enclosing scope", name);
        else PyErr_Format(PyExc_UnboundLocalError, "cannot access local variable '%U' where it is not associated with a value", name);
        return NULL;
    }
    return Py_NewRef(v);
}

PyObject *piper_unbound_local(PyObject *name) { PyErr_Format(PyExc_UnboundLocalError, "cannot access local variable '%U' where it is not associated with a value", name); return NULL; }
PyObject *piper_unbound_free(PyObject *name) { PyErr_Format(PyExc_NameError, "cannot access free variable '%U' where it is not associated with a value in enclosing scope", name); return NULL; }

/* ---- code object stand-in ------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *func; } codeobject;
static void code_dealloc(PyObject *o) { Py_XDECREF(((codeobject *)o)->func); PyObject_Free(o); }
static PyObject *code_getattr(PyObject *o, PyObject *name) {
    PiperFunctionObject *f = (PiperFunctionObject *)((codeobject *)o)->func;
    const char *n = PyUnicode_AsUTF8(name);
    if (!strcmp(n, "co_name")) return Py_NewRef(f->name);
    if (!strcmp(n, "co_qualname")) return Py_NewRef(f->qualname);
    if (!strcmp(n, "co_argcount")) return PyLong_FromLong(f->arg_count);
    if (!strcmp(n, "co_posonlyargcount")) return PyLong_FromLong(f->posonly_count);
    if (!strcmp(n, "co_kwonlyargcount")) return PyLong_FromLong(f->kwonly_count);
    if (!strcmp(n, "co_varnames")) return Py_NewRef(f->argnames);
    if (!strcmp(n, "co_nlocals")) return PyLong_FromSsize_t(PyTuple_GET_SIZE(f->argnames));
    if (!strcmp(n, "co_flags")) return PyLong_FromLong((f->has_varargs ? 4 : 0) | (f->has_varkw ? 8 : 0) | (f->is_generator ? 0x20 : 0) | (f->is_coroutine ? 0x80 : 0) | (f->is_async_generator ? 0x200 : 0) | 3);
    if (!strcmp(n, "co_filename")) { PyObject *fn = PyDict_GetItemString(f->globals, "__file__"); return Py_NewRef(fn ? fn : Py_None); }
    if (!strcmp(n, "co_firstlineno")) return PyLong_FromLong(1);
    if (!strcmp(n, "co_freevars") || !strcmp(n, "co_cellvars") || !strcmp(n, "co_consts") || !strcmp(n, "co_names")) return PyTuple_New(0);
    if (!strcmp(n, "co_code")) return PyBytes_FromStringAndSize("", 0);
    return PyObject_GenericGetAttr(o, name);
}
static PyObject *code_repr(PyObject *o) { return PyUnicode_FromFormat("<code object %U at %p>", ((PiperFunctionObject *)((codeobject *)o)->func)->name, o); }
PyTypeObject PyCode_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "code", .tp_basicsize = sizeof(codeobject), .tp_dealloc = code_dealloc, .tp_repr = code_repr, .tp_getattro = code_getattr };
PyObject *piper_code_for_function(PyObject *f) { codeobject *c = PyObject_New(codeobject, &PyCode_Type); if (!c) return NULL; c->func = Py_NewRef(f); return (PyObject *)c; }

/* ---- accessors for generated code ------------------------------------------ */

/* Borrowed cell object at closure index i. */
PyObject *piper_closure_cell(PyObject *func, Py_ssize_t i) {
    PyObject *cl = ((PiperFunctionObject *)func)->closure;
    return PyTuple_GET_ITEM(cl, i);
}
PyObject *piper_function_globals(PyObject *func) { return ((PiperFunctionObject *)func)->globals; }
PyObject *piper_function_builtins(PyObject *func) { return ((PiperFunctionObject *)func)->builtins; }
