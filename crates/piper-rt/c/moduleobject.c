/* module objects and the module registry. */
#include "internal.h"

static PyObject *modules = NULL;

PyObject *piper_modules_dict(void) { if (!modules) modules = PyDict_New(); return modules; }
PyObject *PyImport_GetModuleDict(void) { return piper_modules_dict(); }

PyObject *PyModule_NewObject(PyObject *name) {
    PyModuleObject *m = (PyModuleObject *)PyType_GenericAlloc(&PyModule_Type, 0);
    if (!m) return NULL;
    m->md_dict = PyDict_New();
    m->md_name = Py_NewRef(name);
    m->md_def = NULL; m->md_state = NULL; m->md_weaklist = NULL;
    PyDict_SetItemString(m->md_dict, "__name__", name);
    PyDict_SetItemString(m->md_dict, "__doc__", Py_None);
    PyDict_SetItemString(m->md_dict, "__package__", Py_None);
    PyDict_SetItemString(m->md_dict, "__loader__", Py_None);
    PyDict_SetItemString(m->md_dict, "__spec__", Py_None);
    return (PyObject *)m;
}
PyObject *PyModule_New(const char *name) { PyObject *n = PyUnicode_FromString(name); if (!n) return NULL; PyObject *m = PyModule_NewObject(n); Py_DECREF(n); return m; }

static void module_dealloc(PyObject *o) {
    PyModuleObject *m = (PyModuleObject *)o;
    PyModuleDef *def = (PyModuleDef *)m->md_def;
    if (def && def->m_clear) def->m_clear(o);
    if (def && def->m_free) def->m_free(o);
    PyMem_Free(m->md_state);
    Py_XDECREF(m->md_dict); Py_XDECREF(m->md_name); Py_TYPE(o)->tp_free(o);
}
PyObject *PyModule_GetDict(PyObject *m) { if (!PyModule_Check(m)) { PyErr_BadInternalCall(); return NULL; } return ((PyModuleObject *)m)->md_dict; }
PyObject *PyModule_GetNameObject(PyObject *m) {
    PyObject *n = PyDict_GetItemString(((PyModuleObject *)m)->md_dict, "__name__");
    if (!n || !PyUnicode_Check(n)) { PyErr_SetString(PyExc_SystemError, "nameless module"); return NULL; }
    return Py_NewRef(n);
}
const char *PyModule_GetName(PyObject *m) { PyObject *n = PyModule_GetNameObject(m); if (!n) return NULL; const char *s = PyUnicode_AsUTF8(n); Py_DECREF(n); return s; }
int PyModule_AddObjectRef(PyObject *m, const char *name, PyObject *v) { if (!v) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_SystemError, "PyModule_AddObjectRef() NULL value"); return -1; } return PyDict_SetItemString(PyModule_GetDict(m), name, v); }
int PyModule_Add(PyObject *m, const char *name, PyObject *v) { int r = PyModule_AddObjectRef(m, name, v); Py_XDECREF(v); return r; }
int PyModule_AddObject(PyObject *m, const char *name, PyObject *v) { int r = PyModule_AddObjectRef(m, name, v); if (r == 0) Py_DECREF(v); return r; }
int PyModule_AddIntConstant(PyObject *m, const char *name, long v) { return PyModule_Add(m, name, PyLong_FromLong(v)); }
int PyModule_AddStringConstant(PyObject *m, const char *name, const char *v) { return PyModule_Add(m, name, PyUnicode_FromString(v)); }
int PyModule_AddFunctions(PyObject *m, PyMethodDef *defs) {
    PyObject *name = PyModule_GetNameObject(m);
    for (; defs && defs->ml_name; defs++) {
        PyObject *f = PyCFunction_NewEx(defs, m, name);
        if (!f || PyModule_AddObjectRef(m, defs->ml_name, f) < 0) { Py_XDECREF(f); Py_XDECREF(name); return -1; }
        Py_DECREF(f);
    }
    Py_XDECREF(name);
    return 0;
}
int PyModule_AddType(PyObject *m, PyTypeObject *tp) { if (PyType_Ready(tp) < 0) return -1; return PyModule_AddObjectRef(m, _PyType_Name(tp), (PyObject *)tp); }
void *PyModule_GetState(PyObject *m) { if (!PyModule_Check(m)) { PyErr_BadInternalCall(); return NULL; } return ((PyModuleObject *)m)->md_state; }

PyObject *PyState_FindModule(PyModuleDef *def) {
    PyObject *key, *module;
    Py_ssize_t position = 0;
    while (PyDict_Next(piper_modules_dict(), &position, &key, &module)) {
        if (PyModule_Check(module) && ((PyModuleObject *)module)->md_def == def) return module;
    }
    return NULL;
}

int PyState_AddModule(PyObject *module, PyModuleDef *def) {
    if (!PyModule_Check(module) || !def) { PyErr_BadInternalCall(); return -1; }
    ((PyModuleObject *)module)->md_def = def;
    const char *name = PyModule_GetName(module);
    return name ? PyDict_SetItemString(piper_modules_dict(), name, module) : -1;
}

int PyState_RemoveModule(PyModuleDef *def) {
    PyObject *module = PyState_FindModule(def);
    if (!module) return 0;
    PyObject *name = PyModule_GetNameObject(module);
    if (!name) return -1;
    int result = PyDict_DelItem(piper_modules_dict(), name);
    Py_DECREF(name);
    return result;
}

PyTypeObject PyModuleDef_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "moduledef", .tp_basicsize = sizeof(PyModuleDef), .tp_getattro = PyObject_GenericGetAttr,
};

PyObject *PyModuleDef_Init(PyModuleDef *def) {
    if (!def) { PyErr_BadInternalCall(); return NULL; }
    if (!Py_TYPE((PyObject *)def)) Py_SET_TYPE((PyObject *)def, &PyModuleDef_Type);
    return (PyObject *)def;
}

PyObject *piper_module_from_def(PyModuleDef *def, const char *requested_name) {
    if (!def) { PyErr_BadInternalCall(); return NULL; }
    const char *name = requested_name && *requested_name ? requested_name : def->m_name;
    PyObject *m = PyModule_New(name ? name : "");
    if (!m) return NULL;
    PyModuleObject *mo = (PyModuleObject *)m;
    mo->md_def = def;
    if (def->m_size > 0) {
        mo->md_state = PyMem_Calloc(1, (size_t)def->m_size);
        if (!mo->md_state) { Py_DECREF(m); return PyErr_NoMemory(); }
    }
    if (def->m_doc) PyModule_AddStringConstant(m, "__doc__", def->m_doc);
    if (def->m_methods && PyModule_AddFunctions(m, def->m_methods) < 0) { Py_DECREF(m); return NULL; }
    if (def->m_slots) {
        for (PyModuleDef_Slot *s = def->m_slots; s->slot; s++) {
            if (s->slot == Py_mod_create) continue;
            if (s->slot == Py_mod_exec) {
                int (*exec)(PyObject *) = (int (*)(PyObject *))s->value;
                if (exec(m) < 0) { Py_DECREF(m); return NULL; }
            }
        }
    }
    piper_register_module(name ? name : "", m);
    return m;
}

PyObject *PyModule_Create2(PyModuleDef *def, int api_version) {
    PIPER_UNUSED(api_version);
    PyModuleDef_Init(def);
    return piper_module_from_def(def, def->m_name);
}

static PyObject *module_repr(PyObject *o) {
    PyObject *d = ((PyModuleObject *)o)->md_dict;
    PyObject *name = PyDict_GetItemString(d, "__name__");
    PyObject *file = PyDict_GetItemString(d, "__file__");
    if (file && file != Py_None) return PyUnicode_FromFormat("<module %R from %R>", name ? name : Py_None, file);
    return PyUnicode_FromFormat("<module %R>", name ? name : Py_None);
}
static PyObject *module_getattro(PyObject *o, PyObject *name) {
    PyObject *r = PyObject_GenericGetAttr(o, name);
    if (r || !PyErr_ExceptionMatches(PyExc_AttributeError)) return r;
    PyErr_Clear();
    PyObject *d = ((PyModuleObject *)o)->md_dict;
    PyObject *getattr = PyDict_GetItemString(d, "__getattr__");
    if (getattr) return PyObject_CallOneArg(getattr, name);
    PyObject *modname = PyDict_GetItemString(d, "__name__");
    if (modname && PyUnicode_Check(modname)) PyErr_Format(PyExc_AttributeError, "module '%U' has no attribute '%U'", modname, name);
    else PyErr_Format(PyExc_AttributeError, "module has no attribute '%U'", name);
    return NULL;
}
static int module_init(PyObject *o, PyObject *args, PyObject *kwds) {
    PyObject *name = NULL, *doc = Py_None;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n >= 1) name = PyTuple_GET_ITEM(args, 0);
    if (n >= 2) doc = PyTuple_GET_ITEM(args, 1);
    if (kwds) { PyObject *v; if ((v = PyDict_GetItemString(kwds, "name"))) name = v; if ((v = PyDict_GetItemString(kwds, "doc"))) doc = v; }
    if (!name || !PyUnicode_Check(name)) { PyErr_SetString(PyExc_TypeError, "module.__init__() argument 'name' must be str"); return -1; }
    PyModuleObject *m = (PyModuleObject *)o;
    if (!m->md_dict) m->md_dict = PyDict_New();
    Py_XSETREF(m->md_name, Py_NewRef(name));
    PyDict_SetItemString(m->md_dict, "__name__", name);
    PyDict_SetItemString(m->md_dict, "__doc__", doc);
    if (!PyDict_GetItemString(m->md_dict, "__package__")) PyDict_SetItemString(m->md_dict, "__package__", Py_None);
    if (!PyDict_GetItemString(m->md_dict, "__loader__")) PyDict_SetItemString(m->md_dict, "__loader__", Py_None);
    if (!PyDict_GetItemString(m->md_dict, "__spec__")) PyDict_SetItemString(m->md_dict, "__spec__", Py_None);
    return 0;
}
static PyObject *module_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(args); PIPER_UNUSED(kwds);
    PyModuleObject *m = (PyModuleObject *)tp->tp_alloc(tp, 0);
    if (!m) return NULL;
    m->md_dict = PyDict_New();
    m->md_name = NULL;
    return (PyObject *)m;
}
static PyObject *module_dir(PyObject *o, PyObject *u) {
    PIPER_UNUSED(u);
    PyObject *d = ((PyModuleObject *)o)->md_dict;
    PyObject *dirfn = PyDict_GetItemString(d, "__dir__");
    if (dirfn) return PyObject_CallNoArgs(dirfn);
    return PyDict_Keys(d);
}
static PyObject *module_annotations_get(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    PyObject *d = ((PyModuleObject *)o)->md_dict;
    PyObject *a = PyDict_GetItemString(d, "__annotations__");
    if (a) return Py_NewRef(a);
    PyObject *annotate = PyDict_GetItemString(d, "__annotate__");
    if (annotate && annotate != Py_None) { PyObject *one = PyLong_FromLong(1); PyObject *r = PyObject_CallOneArg(annotate, one); Py_DECREF(one); if (r) PyDict_SetItemString(d, "__annotations__", r); return r; }
    a = PyDict_New();
    PyDict_SetItemString(d, "__annotations__", a);
    return a;
}
static int module_annotations_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); PyObject *d = ((PyModuleObject *)o)->md_dict; return v ? PyDict_SetItemString(d, "__annotations__", v) : PyDict_DelItemString(d, "__annotations__"); }
static PyObject *module_annotate_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *a = PyDict_GetItemString(((PyModuleObject *)o)->md_dict, "__annotate__"); return Py_NewRef(a ? a : Py_None); }
static int module_annotate_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); return PyDict_SetItemString(((PyModuleObject *)o)->md_dict, "__annotate__", v ? v : Py_None); }
static PyMethodDef module_methods[] = { { "__dir__", module_dir, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyGetSetDef module_getsets[] = { { "__annotations__", module_annotations_get, module_annotations_set, NULL, NULL }, { "__annotate__", module_annotate_get, module_annotate_set, NULL, NULL }, { "__dict__", PyObject_GenericGetDict, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };

PyTypeObject PyModule_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "module", .tp_basicsize = sizeof(PyModuleObject), .tp_dealloc = module_dealloc, .tp_repr = module_repr,
    .tp_getattro = module_getattro, .tp_setattro = PyObject_GenericSetAttr, .tp_flags = Py_TPFLAGS_BASETYPE,
    .tp_methods = module_methods, .tp_getset = module_getsets, .tp_dictoffset = offsetof(PyModuleObject, md_dict),
    .tp_init = module_init, .tp_alloc = PyType_GenericAlloc, .tp_new = module_new, .tp_free = PyObject_Free,
};

void piper_register_module(const char *name, PyObject *m) { PyDict_SetItemString(piper_modules_dict(), name, m); }
PyObject *piper_new_stdlib_module(const char *name) {
    PyObject *m = PyModule_New(name);
    if (m) piper_register_module(name, m);
    return m;
}

PyObject *PyImport_AddModuleRef(const char *name) {
    PyObject *d = piper_modules_dict();
    PyObject *m = PyDict_GetItemString(d, name);
    if (m) return Py_NewRef(m);
    m = PyModule_New(name);
    if (!m) return NULL;
    PyDict_SetItemString(d, name, m);
    return m;
}
PyObject *PyImport_AddModule(const char *name) { PyObject *m = PyImport_AddModuleRef(name); if (m) Py_DECREF(m); return m; }
PyObject *PyImport_GetModule(PyObject *name) { PyObject *m = PyDict_GetItemWithError(piper_modules_dict(), name); return Py_XNewRef(m); }
