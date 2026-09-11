/* Exception types and the per-thread error state. */
#include "internal.h"

/* ---- error state ------------------------------------------------------- */

static __thread PyObject *current_exc = NULL;
/* Stack of exceptions being handled (for implicit chaining / sys.exc_info).
 * Heap allocated so thread-local storage stays small. */
static __thread PyObject **handled_stack = NULL;
static __thread int handled_depth = 0;
static __thread int handled_cap = 0;

PyObject *PyErr_Occurred(void) { return current_exc ? (PyObject *)Py_TYPE(current_exc) : NULL; }
void PyErr_Clear(void) { Py_CLEAR(current_exc); }
PyObject *PyErr_GetRaisedException(void) { PyObject *e = current_exc; current_exc = NULL; return e; }
void PyErr_SetRaisedException(PyObject *e) { Py_XSETREF(current_exc, e); }
void piper_exc_info_push(PyObject *exc) {
    if (handled_depth == handled_cap) {
        int cap = handled_cap ? handled_cap * 2 : 32;
        PyObject **a = PyMem_RawRealloc(handled_stack, (size_t)cap * sizeof(PyObject *));
        if (!a) return;
        handled_stack = a;
        handled_cap = cap;
    }
    handled_stack[handled_depth++] = Py_XNewRef(exc);
}
PyObject *piper_exc_info_pop(void) { if (handled_depth == 0) return NULL; return handled_stack[--handled_depth]; }
PyObject *piper_current_handled_exception(void) { return handled_depth ? handled_stack[handled_depth - 1] : NULL; }

/* Normalize (type, value) into an exception instance. */
static PyObject *make_exception(PyObject *type, PyObject *value) {
    if (value && PyExceptionInstance_Check(value)) {
        if (!type || PyObject_TypeCheck(value, (PyTypeObject *)type)) return Py_NewRef(value);
    }
    if (!type) type = (PyObject *)Py_TYPE(value);
    if (!PyExceptionClass_Check(type)) {
        PyErr_Format(PyExc_TypeError, "exceptions must derive from BaseException");
        return NULL;
    }
    PyObject *r;
    if (!value || value == Py_None) r = PyObject_CallNoArgs(type);
    else r = PyObject_CallOneArg(type, value);
    return r;
}

void PyErr_SetObject(PyObject *type, PyObject *value) {
    PyObject *e = make_exception(type, value);
    if (!e) return; /* error already set by make_exception */
    /* implicit chaining */
    PyObject *ctx = piper_current_handled_exception();
    if (ctx && ctx != e) {
        PyBaseExceptionObject *be = (PyBaseExceptionObject *)e;
        if (!be->context) {
            /* avoid cycles */
            PyObject *o = ctx;
            int cycle = 0;
            while (o) { if (o == e) { cycle = 1; break; } o = ((PyBaseExceptionObject *)o)->context; }
            if (!cycle) be->context = Py_NewRef(ctx);
        }
    }
    Py_XSETREF(current_exc, e);
}
void PyErr_SetString(PyObject *type, const char *msg) {
    PyObject *s = PyUnicode_FromString(msg);
    if (!s) return;
    PyErr_SetObject(type, s);
    Py_DECREF(s);
}
void PyErr_SetNone(PyObject *type) { PyErr_SetObject(type, NULL); }
PyObject *PyErr_FormatV(PyObject *type, const char *fmt, va_list va) {
    PyObject *s = PyUnicode_FromFormatV(fmt, va);
    if (!s) return NULL;
    PyErr_SetObject(type, s);
    Py_DECREF(s);
    return NULL;
}
PyObject *PyErr_Format(PyObject *type, const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    PyErr_FormatV(type, fmt, va);
    va_end(va);
    return NULL;
}
PyObject *PyErr_NoMemory(void) { PyErr_SetNone(PyExc_MemoryError); return NULL; }
void PyErr_BadInternalCall(void) { PyErr_SetString(PyExc_SystemError, "bad argument to internal function"); }
int PyErr_BadArgument(void) { PyErr_SetString(PyExc_TypeError, "bad argument type for built-in operation"); return 0; }

void PyErr_Fetch(PyObject **type, PyObject **value, PyObject **tb) {
    PyObject *e = PyErr_GetRaisedException();
    if (!e) { *type = *value = *tb = NULL; return; }
    *type = Py_NewRef((PyObject *)Py_TYPE(e));
    *value = e;
    *tb = PyException_GetTraceback(e);
}
void PyErr_Restore(PyObject *type, PyObject *value, PyObject *tb) {
    if (!type) { PyErr_Clear(); Py_XDECREF(value); Py_XDECREF(tb); return; }
    PyObject *e = make_exception(type, value);
    Py_DECREF(type); Py_XDECREF(value);
    if (e && tb && tb != Py_None) PyException_SetTraceback(e, tb);
    Py_XDECREF(tb);
    if (e) Py_XSETREF(current_exc, e);
}
void PyErr_NormalizeException(PyObject **type, PyObject **value, PyObject **tb) {
    PIPER_UNUSED(tb);
    if (!*type) return;
    PyObject *e = make_exception(*type, *value);
    if (!e) return;
    Py_XDECREF(*value);
    *value = e;
}

int PyErr_GivenExceptionMatches(PyObject *given, PyObject *type) {
    if (!given || !type) return 0;
    if (PyTuple_Check(type)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(type); i++) if (PyErr_GivenExceptionMatches(given, PyTuple_GET_ITEM(type, i))) return 1;
        return 0;
    }
    if (PyExceptionInstance_Check(given)) given = (PyObject *)Py_TYPE(given);
    if (PyExceptionClass_Check(given) && PyExceptionClass_Check(type)) return PyType_IsSubtype((PyTypeObject *)given, (PyTypeObject *)type);
    return given == type;
}
int PyErr_ExceptionMatches(PyObject *type) { return PyErr_GivenExceptionMatches(PyErr_Occurred(), type); }
int piper_exception_matches(PyObject *exc, PyObject *type) {
    if (PyTuple_Check(type)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(type); i++) { int r = piper_exception_matches(exc, PyTuple_GET_ITEM(type, i)); if (r) return r; }
        return 0;
    }
    if (!PyExceptionClass_Check(type)) { PyErr_SetString(PyExc_TypeError, "catching classes that do not inherit from BaseException is not allowed"); return -1; }
    return PyErr_GivenExceptionMatches(exc, type);
}

PyObject *PyErr_SetFromErrnoWithFilenameObjects(PyObject *type, PyObject *f1, PyObject *f2) {
    int err = errno;
    PyObject *msg = PyUnicode_FromString(err ? strerror(err) : "Error");
    PyObject *args = f1 ? (f2 ? Py_BuildValue("(iOOiO)", err, msg, f1, 0, f2) : Py_BuildValue("(iOO)", err, msg, f1)) : Py_BuildValue("(iO)", err, msg);
    Py_DECREF(msg);
    PyErr_SetObject(type, args);
    Py_XDECREF(args);
    return NULL;
}
PyObject *PyErr_SetFromErrnoWithFilenameObject(PyObject *type, PyObject *f) { return PyErr_SetFromErrnoWithFilenameObjects(type, f, NULL); }
PyObject *PyErr_SetFromErrnoWithFilename(PyObject *type, const char *filename) { PyObject *f = filename ? PyUnicode_DecodeFSDefault(filename) : NULL; PyObject *r = PyErr_SetFromErrnoWithFilenameObjects(type, f, NULL); Py_XDECREF(f); return r; }
PyObject *PyErr_SetFromErrno(PyObject *type) { return PyErr_SetFromErrnoWithFilenameObjects(type, NULL, NULL); }

int PyErr_WarnEx(PyObject *category, const char *msg, Py_ssize_t stack_level) {
    PIPER_UNUSED(stack_level);
    PyObject *w = PyImport_GetModule(piper_intern("warnings"));
    if (w) {
        PyObject *fn = PyObject_GetAttrString(w, "warn");
        Py_DECREF(w);
        if (fn) { PyObject *m = PyUnicode_FromString(msg); PyObject *r = PyObject_CallFunctionObjArgs(fn, m, category ? category : PyExc_UserWarning, NULL); Py_DECREF(m); Py_DECREF(fn); if (!r) return -1; Py_DECREF(r); return 0; }
        PyErr_Clear();
    } else PyErr_Clear();
    fprintf(stderr, "%s: %s\n", ((PyTypeObject *)(category ? category : PyExc_UserWarning))->tp_name, msg);
    return 0;
}
int PyErr_WarnFormat(PyObject *category, Py_ssize_t stack_level, const char *fmt, ...) {
    va_list va; va_start(va, fmt);
    PyObject *s = PyUnicode_FromFormatV(fmt, va);
    va_end(va);
    if (!s) return -1;
    int r = PyErr_WarnEx(category, PyUnicode_AsUTF8(s), stack_level);
    Py_DECREF(s);
    return r;
}
int PyErr_ResourceWarning(PyObject *source, Py_ssize_t stack_level, const char *fmt, ...) { PIPER_UNUSED(source); PIPER_UNUSED(stack_level); PIPER_UNUSED(fmt); return 0; }
int PyErr_CheckSignals(void) { return 0; }
void PyErr_SetInterrupt(void) {}

void PyErr_WriteUnraisable(PyObject *obj) {
    PyObject *e = PyErr_GetRaisedException();
    if (!e) return;
    PyObject *r = obj ? PyObject_Repr(obj) : NULL;
    if (r) fprintf(stderr, "Exception ignored in: %s\n", PyUnicode_AsUTF8(r)); else PyErr_Clear();
    Py_XDECREF(r);
    PyErr_SetRaisedException(e);
    PyErr_Print();
}

int _PyErr_ChainExceptions1(PyObject *exc) {
    if (!exc) return 0;
    if (PyErr_Occurred()) { PyObject *cur = PyErr_GetRaisedException(); PyException_SetContext(cur, exc); PyErr_SetRaisedException(cur); }
    else PyErr_SetRaisedException(exc);
    return 0;
}
void _PyErr_ChainExceptions(PyObject *t, PyObject *v, PyObject *tb) { Py_XDECREF(t); Py_XDECREF(tb); _PyErr_ChainExceptions1(v); }

/* ---- BaseException ------------------------------------------------------- */

static PyObject *baseexc_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(kwds);
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)tp->tp_alloc(tp, 0);
    if (!e) return NULL;
    e->dict = NULL; e->notes = NULL; e->traceback = NULL; e->context = NULL; e->cause = NULL; e->suppress_context = 0;
    e->args = args ? Py_NewRef(args) : PyTuple_New(0);
    return (PyObject *)e;
}
static int baseexc_init(PyObject *self, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds) && Py_TYPE(self)->tp_init == PyBaseObject_Type.tp_init) {}
    if (kwds && PyDict_Size(kwds) && !(Py_TYPE(self)->tp_flags & Py_TPFLAGS_HEAPTYPE)) { PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", Py_TYPE(self)->tp_name); return -1; }
    Py_XSETREF(((PyBaseExceptionObject *)self)->args, Py_NewRef(args));
    return 0;
}
static void baseexc_dealloc(PyObject *o) {
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)o;
    Py_XDECREF(e->dict); Py_XDECREF(e->args); Py_XDECREF(e->notes); Py_XDECREF(e->traceback); Py_XDECREF(e->context); Py_XDECREF(e->cause);
    Py_TYPE(o)->tp_free(o);
}
PyObject *piper_exception_str(PyObject *o) {
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)o;
    Py_ssize_t n = e->args ? PyTuple_GET_SIZE(e->args) : 0;
    if (n == 0) return PyUnicode_New(0, 0);
    if (n == 1) return PyObject_Str(PyTuple_GET_ITEM(e->args, 0));
    return PyObject_Str(e->args);
}
static PyObject *baseexc_str(PyObject *o) { return piper_exception_str(o); }
static PyObject *baseexc_repr(PyObject *o) {
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)o;
    const char *name = _PyType_Name(Py_TYPE(o));
    Py_ssize_t n = e->args ? PyTuple_GET_SIZE(e->args) : 0;
    if (n == 1) return PyUnicode_FromFormat("%s(%R)", name, PyTuple_GET_ITEM(e->args, 0));
    return PyUnicode_FromFormat("%s%R", name, e->args ? e->args : Py_None);
}
static PyObject *baseexc_args_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *a = ((PyBaseExceptionObject *)o)->args; return Py_NewRef(a ? a : Py_None); }
static int baseexc_args_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (!v) { PyErr_SetString(PyExc_TypeError, "args may not be deleted"); return -1; } PyObject *t = PySequence_Tuple(v); if (!t) return -1; Py_XSETREF(((PyBaseExceptionObject *)o)->args, t); return 0; }
static PyObject *baseexc_tb_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *t = ((PyBaseExceptionObject *)o)->traceback; return Py_NewRef(t ? t : Py_None); }
static int baseexc_tb_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (!v) { PyErr_SetString(PyExc_TypeError, "__traceback__ may not be deleted"); return -1; } if (v != Py_None && !Py_IS_TYPE(v, &PyTraceBack_Type)) { PyErr_SetString(PyExc_TypeError, "__traceback__ must be a traceback or None"); return -1; } Py_XSETREF(((PyBaseExceptionObject *)o)->traceback, v == Py_None ? NULL : Py_NewRef(v)); return 0; }
static PyObject *baseexc_context_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *t = ((PyBaseExceptionObject *)o)->context; return Py_NewRef(t ? t : Py_None); }
static int baseexc_context_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (!v) { PyErr_SetString(PyExc_TypeError, "__context__ may not be deleted"); return -1; } if (v != Py_None && !PyExceptionInstance_Check(v)) { PyErr_SetString(PyExc_TypeError, "exception context must be None or derive from BaseException"); return -1; } Py_XSETREF(((PyBaseExceptionObject *)o)->context, v == Py_None ? NULL : Py_NewRef(v)); return 0; }
static PyObject *baseexc_cause_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyObject *t = ((PyBaseExceptionObject *)o)->cause; return Py_NewRef(t ? t : Py_None); }
static int baseexc_cause_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); if (!v) { PyErr_SetString(PyExc_TypeError, "__cause__ may not be deleted"); return -1; } if (v != Py_None && !PyExceptionInstance_Check(v)) { PyErr_SetString(PyExc_TypeError, "exception cause must be None or derive from BaseException"); return -1; } PyBaseExceptionObject *e = (PyBaseExceptionObject *)o; Py_XSETREF(e->cause, v == Py_None ? NULL : Py_NewRef(v)); e->suppress_context = 1; return 0; }
static PyObject *baseexc_suppress_get(PyObject *o, void *c) { PIPER_UNUSED(c); Py_RETURN_BOOL(((PyBaseExceptionObject *)o)->suppress_context); }
static int baseexc_suppress_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); int t = PyObject_IsTrue(v); if (t < 0) return -1; ((PyBaseExceptionObject *)o)->suppress_context = (char)t; return 0; }
static PyObject *baseexc_with_traceback(PyObject *o, PyObject *tb) { if (baseexc_tb_set(o, tb, NULL) < 0) return NULL; return Py_NewRef(o); }
static PyObject *baseexc_add_note(PyObject *o, PyObject *note) {
    if (!PyUnicode_Check(note)) { PyErr_Format(PyExc_TypeError, "note must be a str, not '%s'", Py_TYPE(note)->tp_name); return NULL; }
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)o;
    if (!e->notes) e->notes = PyList_New(0);
    if (PyList_Append(e->notes, note) < 0) return NULL;
    Py_RETURN_NONE;
}
static PyObject *baseexc_notes_get(PyObject *o, void *c) { PIPER_UNUSED(c); PyBaseExceptionObject *e = (PyBaseExceptionObject *)o; if (!e->notes) { PyErr_SetString(PyExc_AttributeError, "__notes__"); return NULL; } return Py_NewRef(e->notes); }
static int baseexc_notes_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); Py_XSETREF(((PyBaseExceptionObject *)o)->notes, Py_XNewRef(v)); return 0; }
static PyObject *baseexc_reduce(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyBaseExceptionObject *e = (PyBaseExceptionObject *)o; if (e->dict && PyDict_Size(e->dict)) return PyTuple_Pack(3, (PyObject *)Py_TYPE(o), e->args, e->dict); return PyTuple_Pack(2, (PyObject *)Py_TYPE(o), e->args); }
static PyObject *baseexc_setstate(PyObject *o, PyObject *state) {
    if (state != Py_None) {
        if (!PyDict_Check(state)) { PyErr_SetString(PyExc_TypeError, "state is not a dictionary"); return NULL; }
        Py_ssize_t pos = 0; PyObject *k, *v;
        while (PyDict_Next(state, &pos, &k, &v)) if (PyObject_SetAttr(o, k, v) < 0) return NULL;
    }
    Py_RETURN_NONE;
}
static PyMethodDef baseexc_methods[] = { { "with_traceback", baseexc_with_traceback, METH_O, NULL }, { "add_note", baseexc_add_note, METH_O, NULL }, { "__reduce__", baseexc_reduce, METH_NOARGS, NULL }, { "__setstate__", baseexc_setstate, METH_O, NULL }, { NULL, NULL, 0, NULL } };
static PyGetSetDef baseexc_getsets[] = {
    { "args", baseexc_args_get, baseexc_args_set, NULL, NULL }, { "__traceback__", baseexc_tb_get, baseexc_tb_set, NULL, NULL },
    { "__context__", baseexc_context_get, baseexc_context_set, NULL, NULL }, { "__cause__", baseexc_cause_get, baseexc_cause_set, NULL, NULL },
    { "__suppress_context__", baseexc_suppress_get, baseexc_suppress_set, NULL, NULL }, { "__notes__", baseexc_notes_get, baseexc_notes_set, NULL, NULL },
    { "__dict__", PyObject_GenericGetDict, PyObject_GenericSetDict, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};

static PyTypeObject _PyExc_BaseException = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "BaseException", .tp_basicsize = sizeof(PyBaseExceptionObject), .tp_dealloc = baseexc_dealloc, .tp_repr = baseexc_repr,
    .tp_str = baseexc_str, .tp_getattro = PyObject_GenericGetAttr, .tp_setattro = PyObject_GenericSetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Common base class for all exceptions",
    .tp_methods = baseexc_methods, .tp_getset = baseexc_getsets, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict),
    .tp_init = baseexc_init, .tp_alloc = PyType_GenericAlloc, .tp_new = baseexc_new, .tp_free = PyObject_Free,
};
PyObject *PyExc_BaseException = (PyObject *)&_PyExc_BaseException;

/* Simple subclasses share BaseException's layout. */
#define SIMPLE_EXC(NAME, BASE, DOC) \
    static PyTypeObject _PyExc_##NAME = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = #NAME, .tp_basicsize = sizeof(PyBaseExceptionObject), \
        .tp_dealloc = baseexc_dealloc, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = DOC, .tp_base = &_PyExc_##BASE, \
        .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = baseexc_init, .tp_new = baseexc_new }; \
    PyObject *PyExc_##NAME = (PyObject *)&_PyExc_##NAME;

SIMPLE_EXC(Exception, BaseException, "Common base class for all non-exit exceptions.")
SIMPLE_EXC(TypeError, Exception, "Inappropriate argument type.")
SIMPLE_EXC(StopAsyncIteration, Exception, "Signal the end from iterator.__anext__().")
SIMPLE_EXC(GeneratorExit, BaseException, "Request that a generator exit.")
SIMPLE_EXC(KeyboardInterrupt, BaseException, "Program interrupted by user.")
SIMPLE_EXC(ArithmeticError, Exception, "Base class for arithmetic errors.")
SIMPLE_EXC(OverflowError, ArithmeticError, "Result too large to be represented.")
SIMPLE_EXC(ZeroDivisionError, ArithmeticError, "Second argument to a division or modulo operation was zero.")
SIMPLE_EXC(FloatingPointError, ArithmeticError, "Floating-point operation failed.")
SIMPLE_EXC(AssertionError, Exception, "Assertion failed.")
SIMPLE_EXC(LookupError, Exception, "Base class for lookup errors.")
SIMPLE_EXC(IndexError, LookupError, "Sequence index out of range.")
SIMPLE_EXC(ValueError, Exception, "Inappropriate argument value (of correct type).")
SIMPLE_EXC(UnicodeError, ValueError, "Unicode related error.")
SIMPLE_EXC(RuntimeError, Exception, "Unspecified run-time error.")
SIMPLE_EXC(RecursionError, RuntimeError, "Recursion limit exceeded.")
SIMPLE_EXC(NotImplementedError, RuntimeError, "Method or function hasn't been implemented yet.")
SIMPLE_EXC(PythonFinalizationError, RuntimeError, "Operation blocked during Python finalization.")
SIMPLE_EXC(MemoryError, Exception, "Out of memory.")
SIMPLE_EXC(SystemError, Exception, "Internal error in the Python interpreter.")
SIMPLE_EXC(ReferenceError, Exception, "Weak ref proxy used after referent went away.")
SIMPLE_EXC(BufferError, Exception, "Buffer error.")
SIMPLE_EXC(EOFError, Exception, "Read beyond end of file.")
SIMPLE_EXC(Warning, Exception, "Base class for warning categories.")
SIMPLE_EXC(UserWarning, Warning, "Base class for warnings generated by user code.")
SIMPLE_EXC(DeprecationWarning, Warning, "Base class for warnings about deprecated features.")
SIMPLE_EXC(PendingDeprecationWarning, Warning, "Base class for warnings about features which will be deprecated in the future.")
SIMPLE_EXC(SyntaxWarning, Warning, "Base class for warnings about dubious syntax.")
SIMPLE_EXC(RuntimeWarning, Warning, "Base class for warnings about dubious runtime behavior.")
SIMPLE_EXC(FutureWarning, Warning, "Base class for warnings about constructs that will change semantically in the future.")
SIMPLE_EXC(ImportWarning, Warning, "Base class for warnings about probable mistakes in module imports")
SIMPLE_EXC(UnicodeWarning, Warning, "Base class for warnings about Unicode related problems.")
SIMPLE_EXC(BytesWarning, Warning, "Base class for warnings about bytes and buffer related problems.")
SIMPLE_EXC(ResourceWarning, Warning, "Base class for warnings about resource usage.")
SIMPLE_EXC(EncodingWarning, Warning, "Base class for warnings about encodings.")

/* KeyError: str() reprs a single arg */
static PyObject *keyerror_str(PyObject *o) {
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)o;
    if (e->args && PyTuple_GET_SIZE(e->args) == 1) return PyObject_Repr(PyTuple_GET_ITEM(e->args, 0));
    return baseexc_str(o);
}
static PyTypeObject _PyExc_KeyError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "KeyError", .tp_basicsize = sizeof(PyBaseExceptionObject), .tp_dealloc = baseexc_dealloc, .tp_str = keyerror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Mapping key not found.", .tp_base = &_PyExc_LookupError, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = baseexc_init, .tp_new = baseexc_new };
PyObject *PyExc_KeyError = (PyObject *)&_PyExc_KeyError;

/* Exceptions with extra fields */
typedef struct { PyBaseExceptionObject base; PyObject *value; } StopIterationObject;
static int stopiter_init(PyObject *self, PyObject *args, PyObject *kwds) {
    if (baseexc_init(self, args, kwds) < 0) return -1;
    StopIterationObject *s = (StopIterationObject *)self;
    Py_XSETREF(s->value, Py_NewRef(PyTuple_GET_SIZE(args) ? PyTuple_GET_ITEM(args, 0) : Py_None));
    return 0;
}
static void stopiter_dealloc(PyObject *o) { Py_XDECREF(((StopIterationObject *)o)->value); baseexc_dealloc(o); }
static PyMemberDef stopiter_members[] = { { "value", _Py_T_OBJECT, offsetof(StopIterationObject, value), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyTypeObject _PyExc_StopIteration = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "StopIteration", .tp_basicsize = sizeof(StopIterationObject), .tp_dealloc = stopiter_dealloc, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Signal the end from iterator.__next__().", .tp_members = stopiter_members, .tp_base = &_PyExc_Exception, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = stopiter_init, .tp_new = baseexc_new };
PyObject *PyExc_StopIteration = (PyObject *)&_PyExc_StopIteration;

typedef struct { PyBaseExceptionObject base; PyObject *code; } SystemExitObject;
static int systemexit_init(PyObject *self, PyObject *args, PyObject *kwds) {
    if (baseexc_init(self, args, kwds) < 0) return -1;
    SystemExitObject *s = (SystemExitObject *)self;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    Py_XSETREF(s->code, Py_NewRef(n == 0 ? Py_None : n == 1 ? PyTuple_GET_ITEM(args, 0) : args));
    return 0;
}
static void systemexit_dealloc(PyObject *o) { Py_XDECREF(((SystemExitObject *)o)->code); baseexc_dealloc(o); }
static PyMemberDef systemexit_members[] = { { "code", _Py_T_OBJECT, offsetof(SystemExitObject, code), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyTypeObject _PyExc_SystemExit = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "SystemExit", .tp_basicsize = sizeof(SystemExitObject), .tp_dealloc = systemexit_dealloc, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Request to exit from the interpreter.", .tp_members = systemexit_members, .tp_base = &_PyExc_BaseException, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = systemexit_init, .tp_new = baseexc_new };
PyObject *PyExc_SystemExit = (PyObject *)&_PyExc_SystemExit;

typedef struct { PyBaseExceptionObject base; PyObject *msg, *name, *path, *name_from; } ImportErrorObject;
static int importerror_init(PyObject *self, PyObject *args, PyObject *kwds) {
    ImportErrorObject *e = (ImportErrorObject *)self;
    PyObject *name = NULL, *path = NULL, *name_from = NULL;
    if (kwds) {
        name = PyDict_GetItemString(kwds, "name"); path = PyDict_GetItemString(kwds, "path"); name_from = PyDict_GetItemString(kwds, "name_from");
        if (PyDict_Size(kwds) > (name ? 1 : 0) + (path ? 1 : 0) + (name_from ? 1 : 0)) { PyErr_SetString(PyExc_TypeError, "ImportError() got an unexpected keyword argument"); return -1; }
    }
    Py_XSETREF(e->base.args, Py_NewRef(args));
    Py_XSETREF(e->name, Py_XNewRef(name)); Py_XSETREF(e->path, Py_XNewRef(path)); Py_XSETREF(e->name_from, Py_XNewRef(name_from));
    Py_XSETREF(e->msg, PyTuple_GET_SIZE(args) == 1 ? Py_NewRef(PyTuple_GET_ITEM(args, 0)) : NULL);
    return 0;
}
static void importerror_dealloc(PyObject *o) { ImportErrorObject *e = (ImportErrorObject *)o; Py_XDECREF(e->msg); Py_XDECREF(e->name); Py_XDECREF(e->path); Py_XDECREF(e->name_from); baseexc_dealloc(o); }
static PyObject *importerror_str(PyObject *o) { ImportErrorObject *e = (ImportErrorObject *)o; if (e->msg && PyUnicode_Check(e->msg)) return Py_NewRef(e->msg); return baseexc_str(o); }
static PyMemberDef importerror_members[] = { { "msg", _Py_T_OBJECT, offsetof(ImportErrorObject, msg), 0, NULL }, { "name", _Py_T_OBJECT, offsetof(ImportErrorObject, name), 0, NULL }, { "path", _Py_T_OBJECT, offsetof(ImportErrorObject, path), 0, NULL }, { "name_from", _Py_T_OBJECT, offsetof(ImportErrorObject, name_from), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyTypeObject _PyExc_ImportError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "ImportError", .tp_basicsize = sizeof(ImportErrorObject), .tp_dealloc = importerror_dealloc, .tp_str = importerror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Import can't find module, or can't find name in module.", .tp_members = importerror_members, .tp_base = &_PyExc_Exception, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = importerror_init, .tp_new = baseexc_new };
PyObject *PyExc_ImportError = (PyObject *)&_PyExc_ImportError;
static PyTypeObject _PyExc_ModuleNotFoundError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "ModuleNotFoundError", .tp_basicsize = sizeof(ImportErrorObject), .tp_dealloc = importerror_dealloc, .tp_str = importerror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Module not found.", .tp_base = &_PyExc_ImportError, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = importerror_init, .tp_new = baseexc_new };
PyObject *PyExc_ModuleNotFoundError = (PyObject *)&_PyExc_ModuleNotFoundError;

typedef struct { PyBaseExceptionObject base; PyObject *name; } NameErrorObject;
static int nameerror_init(PyObject *self, PyObject *args, PyObject *kwds) {
    NameErrorObject *e = (NameErrorObject *)self;
    PyObject *name = kwds ? PyDict_GetItemString(kwds, "name") : NULL;
    if (kwds && PyDict_Size(kwds) > (name ? 1 : 0)) { PyErr_SetString(PyExc_TypeError, "NameError() got an unexpected keyword argument"); return -1; }
    Py_XSETREF(e->base.args, Py_NewRef(args));
    Py_XSETREF(e->name, Py_XNewRef(name));
    return 0;
}
static void nameerror_dealloc(PyObject *o) { Py_XDECREF(((NameErrorObject *)o)->name); baseexc_dealloc(o); }
static PyMemberDef nameerror_members[] = { { "name", _Py_T_OBJECT, offsetof(NameErrorObject, name), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyTypeObject _PyExc_NameError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "NameError", .tp_basicsize = sizeof(NameErrorObject), .tp_dealloc = nameerror_dealloc, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Name not found globally.", .tp_members = nameerror_members, .tp_base = &_PyExc_Exception, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = nameerror_init, .tp_new = baseexc_new };
PyObject *PyExc_NameError = (PyObject *)&_PyExc_NameError;
static PyTypeObject _PyExc_UnboundLocalError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "UnboundLocalError", .tp_basicsize = sizeof(NameErrorObject), .tp_dealloc = nameerror_dealloc, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Local name referenced but not bound to a value.", .tp_base = &_PyExc_NameError, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = nameerror_init, .tp_new = baseexc_new };
PyObject *PyExc_UnboundLocalError = (PyObject *)&_PyExc_UnboundLocalError;

typedef struct { PyBaseExceptionObject base; PyObject *obj, *name; } AttributeErrorObject;
static int attributeerror_init(PyObject *self, PyObject *args, PyObject *kwds) {
    AttributeErrorObject *e = (AttributeErrorObject *)self;
    PyObject *name = kwds ? PyDict_GetItemString(kwds, "name") : NULL, *obj = kwds ? PyDict_GetItemString(kwds, "obj") : NULL;
    if (kwds && PyDict_Size(kwds) > (name ? 1 : 0) + (obj ? 1 : 0)) { PyErr_SetString(PyExc_TypeError, "AttributeError() got an unexpected keyword argument"); return -1; }
    Py_XSETREF(e->base.args, Py_NewRef(args));
    Py_XSETREF(e->name, Py_XNewRef(name)); Py_XSETREF(e->obj, Py_XNewRef(obj));
    return 0;
}
static void attributeerror_dealloc(PyObject *o) { AttributeErrorObject *e = (AttributeErrorObject *)o; Py_XDECREF(e->obj); Py_XDECREF(e->name); baseexc_dealloc(o); }
static PyMemberDef attributeerror_members[] = { { "name", _Py_T_OBJECT, offsetof(AttributeErrorObject, name), 0, NULL }, { "obj", _Py_T_OBJECT, offsetof(AttributeErrorObject, obj), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyTypeObject _PyExc_AttributeError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "AttributeError", .tp_basicsize = sizeof(AttributeErrorObject), .tp_dealloc = attributeerror_dealloc, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Attribute not found.", .tp_members = attributeerror_members, .tp_base = &_PyExc_Exception, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = attributeerror_init, .tp_new = baseexc_new };
PyObject *PyExc_AttributeError = (PyObject *)&_PyExc_AttributeError;

typedef struct { PyBaseExceptionObject base; PyObject *msg, *filename, *lineno, *offset, *text, *end_lineno, *end_offset, *print_file_and_line; } SyntaxErrorObject;
static int syntaxerror_init(PyObject *self, PyObject *args, PyObject *kwds) {
    if (baseexc_init(self, args, kwds) < 0) return -1;
    SyntaxErrorObject *e = (SyntaxErrorObject *)self;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n >= 1) Py_XSETREF(e->msg, Py_NewRef(PyTuple_GET_ITEM(args, 0)));
    if (n == 2) {
        PyObject *info = PySequence_Tuple(PyTuple_GET_ITEM(args, 1));
        if (!info) return -1;
        Py_ssize_t m = PyTuple_GET_SIZE(info);
        if (m < 4 || m > 6) { Py_DECREF(info); PyErr_SetString(PyExc_IndexError, "tuple index out of range"); return -1; }
        Py_XSETREF(e->filename, Py_NewRef(PyTuple_GET_ITEM(info, 0))); Py_XSETREF(e->lineno, Py_NewRef(PyTuple_GET_ITEM(info, 1)));
        Py_XSETREF(e->offset, Py_NewRef(PyTuple_GET_ITEM(info, 2))); Py_XSETREF(e->text, Py_NewRef(PyTuple_GET_ITEM(info, 3)));
        if (m >= 5) Py_XSETREF(e->end_lineno, Py_NewRef(PyTuple_GET_ITEM(info, 4)));
        if (m >= 6) Py_XSETREF(e->end_offset, Py_NewRef(PyTuple_GET_ITEM(info, 5)));
        Py_DECREF(info);
    }
    return 0;
}
static void syntaxerror_dealloc(PyObject *o) { SyntaxErrorObject *e = (SyntaxErrorObject *)o; Py_XDECREF(e->msg); Py_XDECREF(e->filename); Py_XDECREF(e->lineno); Py_XDECREF(e->offset); Py_XDECREF(e->text); Py_XDECREF(e->end_lineno); Py_XDECREF(e->end_offset); Py_XDECREF(e->print_file_and_line); baseexc_dealloc(o); }
static PyObject *syntaxerror_str(PyObject *o) {
    SyntaxErrorObject *e = (SyntaxErrorObject *)o;
    PyObject *msg = e->msg ? PyObject_Str(e->msg) : PyUnicode_FromString("None");
    if (!msg) return NULL;
    PyObject *fn = NULL;
    if (e->filename && PyUnicode_Check(e->filename)) { const char *s = PyUnicode_AsUTF8(e->filename); const char *slash = strrchr(s, '/'); fn = PyUnicode_FromString(slash ? slash + 1 : s); }
    int has_line = e->lineno && PyLong_Check(e->lineno);
    PyObject *r;
    if (fn && has_line) r = PyUnicode_FromFormat("%U (%U, line %R)", msg, fn, e->lineno);
    else if (fn) r = PyUnicode_FromFormat("%U (%U)", msg, fn);
    else if (has_line) r = PyUnicode_FromFormat("%U (line %R)", msg, e->lineno);
    else r = Py_NewRef(msg);
    Py_DECREF(msg); Py_XDECREF(fn);
    return r;
}
static PyMemberDef syntaxerror_members[] = {
    { "msg", _Py_T_OBJECT, offsetof(SyntaxErrorObject, msg), 0, NULL }, { "filename", _Py_T_OBJECT, offsetof(SyntaxErrorObject, filename), 0, NULL },
    { "lineno", _Py_T_OBJECT, offsetof(SyntaxErrorObject, lineno), 0, NULL }, { "offset", _Py_T_OBJECT, offsetof(SyntaxErrorObject, offset), 0, NULL },
    { "text", _Py_T_OBJECT, offsetof(SyntaxErrorObject, text), 0, NULL }, { "end_lineno", _Py_T_OBJECT, offsetof(SyntaxErrorObject, end_lineno), 0, NULL },
    { "end_offset", _Py_T_OBJECT, offsetof(SyntaxErrorObject, end_offset), 0, NULL }, { "print_file_and_line", _Py_T_OBJECT, offsetof(SyntaxErrorObject, print_file_and_line), 0, NULL },
    { NULL, 0, 0, 0, NULL },
};
#define SYNTAX_EXC(NAME, BASE, DOC) static PyTypeObject _PyExc_##NAME = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = #NAME, .tp_basicsize = sizeof(SyntaxErrorObject), .tp_dealloc = syntaxerror_dealloc, .tp_str = syntaxerror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = DOC, .tp_members = syntaxerror_members, .tp_base = &_PyExc_##BASE, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = syntaxerror_init, .tp_new = baseexc_new }; PyObject *PyExc_##NAME = (PyObject *)&_PyExc_##NAME;
SYNTAX_EXC(SyntaxError, Exception, "Invalid syntax.")
SYNTAX_EXC(IndentationError, SyntaxError, "Improper indentation.")
SYNTAX_EXC(TabError, IndentationError, "Improper mixture of spaces and tabs.")

typedef struct { PyBaseExceptionObject base; PyObject *myerrno, *strerror, *filename, *filename2; Py_ssize_t written; } OSErrorObject;
static PyObject *oserror_subclass_for_errno(int err);
static PyObject *oserror_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (tp == (PyTypeObject *)PyExc_OSError && PyTuple_GET_SIZE(args) >= 2 && PyLong_Check(PyTuple_GET_ITEM(args, 0))) {
        int err = (int)PyLong_AsLong(PyTuple_GET_ITEM(args, 0));
        PyObject *sub = oserror_subclass_for_errno(err);
        if (sub) tp = (PyTypeObject *)sub;
    }
    OSErrorObject *e = (OSErrorObject *)baseexc_new(tp, args, kwds);
    if (!e) return NULL;
    e->myerrno = e->strerror = e->filename = e->filename2 = NULL;
    e->written = -1;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n >= 2 && n <= 5) {
        e->myerrno = Py_NewRef(PyTuple_GET_ITEM(args, 0));
        e->strerror = Py_NewRef(PyTuple_GET_ITEM(args, 1));
        if (n >= 3) { e->filename = Py_NewRef(PyTuple_GET_ITEM(args, 2)); }
        if (n >= 5) { e->filename2 = Py_NewRef(PyTuple_GET_ITEM(args, 4)); }
        if (n >= 3) { Py_SETREF(e->base.args, PyTuple_GetSlice(args, 0, 2)); }
    }
    return (PyObject *)e;
}
static int oserror_init(PyObject *self, PyObject *args, PyObject *kwds) { PIPER_UNUSED(self); PIPER_UNUSED(args); PIPER_UNUSED(kwds); return 0; }
static void oserror_dealloc(PyObject *o) { OSErrorObject *e = (OSErrorObject *)o; Py_XDECREF(e->myerrno); Py_XDECREF(e->strerror); Py_XDECREF(e->filename); Py_XDECREF(e->filename2); baseexc_dealloc(o); }
static PyObject *oserror_str(PyObject *o) {
    OSErrorObject *e = (OSErrorObject *)o;
    if (e->filename && e->filename2) return PyUnicode_FromFormat("[Errno %S] %S: %R -> %R", e->myerrno ? e->myerrno : Py_None, e->strerror ? e->strerror : Py_None, e->filename, e->filename2);
    if (e->filename) return PyUnicode_FromFormat("[Errno %S] %S: %R", e->myerrno ? e->myerrno : Py_None, e->strerror ? e->strerror : Py_None, e->filename);
    if (e->myerrno && e->strerror) return PyUnicode_FromFormat("[Errno %S] %S", e->myerrno, e->strerror);
    return baseexc_str(o);
}
static PyObject *oserror_written_get(PyObject *o, void *c) { PIPER_UNUSED(c); OSErrorObject *e = (OSErrorObject *)o; if (e->written == -1) { PyErr_SetString(PyExc_AttributeError, "characters_written"); return NULL; } return PyLong_FromSsize_t(e->written); }
static int oserror_written_set(PyObject *o, PyObject *v, void *c) { PIPER_UNUSED(c); OSErrorObject *e = (OSErrorObject *)o; if (!v) { e->written = -1; return 0; } Py_ssize_t n = PyNumber_AsSsize_t(v, PyExc_ValueError); if (n == -1 && PyErr_Occurred()) return -1; e->written = n; return 0; }
static PyMemberDef oserror_members[] = { { "errno", _Py_T_OBJECT, offsetof(OSErrorObject, myerrno), 0, NULL }, { "strerror", _Py_T_OBJECT, offsetof(OSErrorObject, strerror), 0, NULL }, { "filename", _Py_T_OBJECT, offsetof(OSErrorObject, filename), 0, NULL }, { "filename2", _Py_T_OBJECT, offsetof(OSErrorObject, filename2), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyGetSetDef oserror_getsets[] = { { "characters_written", oserror_written_get, oserror_written_set, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyTypeObject _PyExc_OSError = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "OSError", .tp_basicsize = sizeof(OSErrorObject), .tp_dealloc = oserror_dealloc, .tp_str = oserror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "Base class for I/O related errors.", .tp_members = oserror_members, .tp_getset = oserror_getsets, .tp_base = &_PyExc_Exception, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = oserror_init, .tp_new = oserror_new };
PyObject *PyExc_OSError = (PyObject *)&_PyExc_OSError;
#define OS_EXC(NAME, BASE, DOC) static PyTypeObject _PyExc_##NAME = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = #NAME, .tp_basicsize = sizeof(OSErrorObject), .tp_dealloc = oserror_dealloc, .tp_str = oserror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = DOC, .tp_base = &_PyExc_##BASE, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = oserror_init, .tp_new = oserror_new }; PyObject *PyExc_##NAME = (PyObject *)&_PyExc_##NAME;
OS_EXC(FileNotFoundError, OSError, "File not found.")
OS_EXC(FileExistsError, OSError, "File already exists.")
OS_EXC(PermissionError, OSError, "Not enough permissions.")
OS_EXC(IsADirectoryError, OSError, "Operation doesn't work on directories.")
OS_EXC(NotADirectoryError, OSError, "Operation only works on directories.")
OS_EXC(TimeoutError, OSError, "Timeout expired.")
OS_EXC(BlockingIOError, OSError, "I/O operation would block.")
OS_EXC(InterruptedError, OSError, "Interrupted by signal.")
OS_EXC(ChildProcessError, OSError, "Child process error.")
OS_EXC(ProcessLookupError, OSError, "Process not found.")
OS_EXC(ConnectionError, OSError, "Connection error.")
OS_EXC(BrokenPipeError, ConnectionError, "Broken pipe.")
OS_EXC(ConnectionAbortedError, ConnectionError, "Connection aborted.")
OS_EXC(ConnectionRefusedError, ConnectionError, "Connection refused.")
OS_EXC(ConnectionResetError, ConnectionError, "Connection reset.")
static PyObject *oserror_subclass_for_errno(int err) {
    switch (err) {
    case ENOENT: return PyExc_FileNotFoundError; case EEXIST: return PyExc_FileExistsError; case EACCES: case EPERM: return PyExc_PermissionError;
    case EISDIR: return PyExc_IsADirectoryError; case ENOTDIR: return PyExc_NotADirectoryError; case ETIMEDOUT: return PyExc_TimeoutError;
    case EAGAIN: case EINPROGRESS: case EALREADY: return PyExc_BlockingIOError; case EINTR: return PyExc_InterruptedError; case ECHILD: return PyExc_ChildProcessError;
    case ESRCH: return PyExc_ProcessLookupError; case EPIPE: return PyExc_BrokenPipeError; case ECONNABORTED: return PyExc_ConnectionAbortedError;
    case ECONNREFUSED: return PyExc_ConnectionRefusedError; case ECONNRESET: return PyExc_ConnectionResetError;
    }
    return NULL;
}

typedef struct { PyBaseExceptionObject base; PyObject *encoding, *object; Py_ssize_t start, end; PyObject *reason; } UnicodeErrorObject;
static int unicodeerror_init(PyObject *self, PyObject *args, PyObject *kwds) {
    if (baseexc_init(self, args, kwds) < 0) return -1;
    UnicodeErrorObject *e = (UnicodeErrorObject *)self;
    if (PyTuple_GET_SIZE(args) == 5) {
        Py_XSETREF(e->encoding, Py_NewRef(PyTuple_GET_ITEM(args, 0))); Py_XSETREF(e->object, Py_NewRef(PyTuple_GET_ITEM(args, 1)));
        e->start = PyLong_AsSsize_t(PyTuple_GET_ITEM(args, 2)); e->end = PyLong_AsSsize_t(PyTuple_GET_ITEM(args, 3));
        Py_XSETREF(e->reason, Py_NewRef(PyTuple_GET_ITEM(args, 4)));
        if (PyErr_Occurred()) return -1;
    }
    return 0;
}
static void unicodeerror_dealloc(PyObject *o) { UnicodeErrorObject *e = (UnicodeErrorObject *)o; Py_XDECREF(e->encoding); Py_XDECREF(e->object); Py_XDECREF(e->reason); baseexc_dealloc(o); }
static PyObject *unicodeerror_str(PyObject *o) {
    UnicodeErrorObject *e = (UnicodeErrorObject *)o;
    if (!e->object || !e->encoding) return baseexc_str(o);
    int decode = Py_TYPE(o) == (PyTypeObject *)PyExc_UnicodeDecodeError || PyType_IsSubtype(Py_TYPE(o), (PyTypeObject *)PyExc_UnicodeDecodeError);
    if (e->end == e->start + 1) {
        if (decode && PyBytes_Check(e->object) && e->start < PyBytes_GET_SIZE(e->object)) return PyUnicode_FromFormat("'%U' codec can't decode byte 0x%02x in position %zd: %U", e->encoding, (unsigned char)PyBytes_AS_STRING(e->object)[e->start], e->start, e->reason);
        if (!decode && PyUnicode_Check(e->object) && e->start < PyUnicode_GET_LENGTH(e->object)) { Py_UCS4 c = PyUnicode_READ_CHAR(e->object, e->start); return PyUnicode_FromFormat(c < 0x100 ? "'%U' codec can't encode character '\\x%02x' in position %zd: %U" : c < 0x10000 ? "'%U' codec can't encode character '\\u%04x' in position %zd: %U" : "'%U' codec can't encode character '\\U%08x' in position %zd: %U", e->encoding, c, e->start, e->reason); }
    }
    return PyUnicode_FromFormat("'%U' codec can't %s %s in position %zd-%zd: %U", e->encoding, decode ? "decode" : "encode", decode ? "bytes" : "characters", e->start, e->end - 1, e->reason);
}
static PyMemberDef unicodeerror_members[] = { { "encoding", _Py_T_OBJECT, offsetof(UnicodeErrorObject, encoding), 0, NULL }, { "object", _Py_T_OBJECT, offsetof(UnicodeErrorObject, object), 0, NULL }, { "start", Py_T_PYSSIZET, offsetof(UnicodeErrorObject, start), 0, NULL }, { "end", Py_T_PYSSIZET, offsetof(UnicodeErrorObject, end), 0, NULL }, { "reason", _Py_T_OBJECT, offsetof(UnicodeErrorObject, reason), 0, NULL }, { NULL, 0, 0, 0, NULL } };
#define UNI_EXC(NAME, DOC) static PyTypeObject _PyExc_##NAME = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = #NAME, .tp_basicsize = sizeof(UnicodeErrorObject), .tp_dealloc = unicodeerror_dealloc, .tp_str = unicodeerror_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = DOC, .tp_members = unicodeerror_members, .tp_base = &_PyExc_UnicodeError, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = unicodeerror_init, .tp_new = baseexc_new }; PyObject *PyExc_##NAME = (PyObject *)&_PyExc_##NAME;
UNI_EXC(UnicodeDecodeError, "Unicode decoding error.")
UNI_EXC(UnicodeEncodeError, "Unicode encoding error.")
UNI_EXC(UnicodeTranslateError, "Unicode translation error.")

/* ExceptionGroup */
typedef struct { PyBaseExceptionObject base; PyObject *msg, *excs; } ExceptionGroupObject;
static PyObject *excgroup_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (PyTuple_GET_SIZE(args) != 2) { PyErr_Format(PyExc_TypeError, "%s.__new__() takes exactly 2 arguments (%zd given)", tp->tp_name, PyTuple_GET_SIZE(args)); return NULL; }
    PyObject *msg = PyTuple_GET_ITEM(args, 0), *excs = PyTuple_GET_ITEM(args, 1);
    if (!PyUnicode_Check(msg)) { PyErr_SetString(PyExc_TypeError, "argument 1 must be str"); return NULL; }
    PyObject *t = PySequence_Tuple(excs);
    if (!t) { PyErr_Clear(); PyErr_SetString(PyExc_TypeError, "second argument (exceptions) must be a sequence"); return NULL; }
    if (PyTuple_GET_SIZE(t) == 0) { Py_DECREF(t); PyErr_SetString(PyExc_ValueError, "second argument (exceptions) must be a non-empty sequence"); return NULL; }
    int all_exc = 1;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(t); i++) { PyObject *x = PyTuple_GET_ITEM(t, i); if (!PyExceptionInstance_Check(x)) { Py_DECREF(t); PyErr_Format(PyExc_ValueError, "Item %zd of second argument (exceptions) is not an exception", i); return NULL; } if (!PyObject_TypeCheck(x, (PyTypeObject *)PyExc_Exception)) all_exc = 0; }
    if (tp == (PyTypeObject *)PyExc_ExceptionGroup && !all_exc) { Py_DECREF(t); PyErr_SetString(PyExc_TypeError, "Cannot nest BaseExceptions in an ExceptionGroup"); return NULL; }
    if (tp == (PyTypeObject *)PyExc_BaseExceptionGroup && all_exc) tp = (PyTypeObject *)PyExc_ExceptionGroup;
    ExceptionGroupObject *e = (ExceptionGroupObject *)baseexc_new(tp, args, kwds);
    if (!e) { Py_DECREF(t); return NULL; }
    e->msg = Py_NewRef(msg);
    e->excs = t;
    return (PyObject *)e;
}
static int excgroup_init(PyObject *self, PyObject *args, PyObject *kwds) { PIPER_UNUSED(self); PIPER_UNUSED(args); PIPER_UNUSED(kwds); return 0; }
static void excgroup_dealloc(PyObject *o) { ExceptionGroupObject *e = (ExceptionGroupObject *)o; Py_XDECREF(e->msg); Py_XDECREF(e->excs); baseexc_dealloc(o); }
static PyObject *excgroup_str(PyObject *o) { ExceptionGroupObject *e = (ExceptionGroupObject *)o; Py_ssize_t n = PyTuple_GET_SIZE(e->excs); return PyUnicode_FromFormat("%U (%zd sub-exception%s)", e->msg, n, n == 1 ? "" : "s"); }
static PyObject *excgroup_derive(PyObject *o, PyObject *excs) {
    ExceptionGroupObject *e = (ExceptionGroupObject *)o;
    PyObject *args = PyTuple_Pack(2, e->msg, excs);
    PyObject *r = PyObject_Call(PyExc_BaseExceptionGroup, args, NULL);
    Py_DECREF(args);
    return r;
}
static PyObject *copy_metadata(PyObject *from, PyObject *to) {
    if (!to) return NULL;
    PyBaseExceptionObject *a = (PyBaseExceptionObject *)from, *b = (PyBaseExceptionObject *)to;
    Py_XSETREF(b->traceback, Py_XNewRef(a->traceback)); Py_XSETREF(b->context, Py_XNewRef(a->context)); Py_XSETREF(b->cause, Py_XNewRef(a->cause));
    if (a->notes) Py_XSETREF(b->notes, PySequence_List(a->notes));
    return to;
}
static int matcher_matches(PyObject *cond, PyObject *exc) {
    if (PyExceptionClass_Check(cond) || PyTuple_Check(cond)) return PyErr_GivenExceptionMatches(exc, cond);
    PyObject *r = PyObject_CallOneArg(cond, exc);
    if (!r) return -1;
    int t = PyObject_IsTrue(r);
    Py_DECREF(r);
    return t;
}
/* Split into (match, rest) by a condition. Both may come back as NULL/None. */
static int excgroup_split_impl(PyObject *o, PyObject *cond, PyObject **match, PyObject **rest) {
    int m = matcher_matches(cond, o);
    if (m < 0) return -1;
    if (m) { *match = Py_NewRef(o); *rest = NULL; return 0; }
    if (!PyObject_TypeCheck(o, (PyTypeObject *)PyExc_BaseExceptionGroup)) { *match = NULL; *rest = Py_NewRef(o); return 0; }
    ExceptionGroupObject *e = (ExceptionGroupObject *)o;
    PyObject *ml = PyList_New(0), *rl = PyList_New(0);
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(e->excs); i++) {
        PyObject *sm, *sr;
        if (excgroup_split_impl(PyTuple_GET_ITEM(e->excs, i), cond, &sm, &sr) < 0) { Py_DECREF(ml); Py_DECREF(rl); return -1; }
        if (sm) { PyList_Append(ml, sm); Py_DECREF(sm); }
        if (sr) { PyList_Append(rl, sr); Py_DECREF(sr); }
    }
    *match = PyList_GET_SIZE(ml) ? copy_metadata(o, PyObject_CallMethod(o, "derive", "O", ml)) : NULL;
    *rest = PyList_GET_SIZE(rl) ? copy_metadata(o, PyObject_CallMethod(o, "derive", "O", rl)) : NULL;
    Py_DECREF(ml); Py_DECREF(rl);
    if (PyErr_Occurred()) return -1;
    return 0;
}
int piper_exception_group_match(PyObject *exc, PyObject *type, PyObject **match, PyObject **rest) {
    if (!PyObject_TypeCheck(exc, (PyTypeObject *)PyExc_BaseExceptionGroup)) {
        int m = piper_exception_matches(exc, type);
        if (m < 0) return -1;
        if (m) { PyObject *args = PyTuple_Pack(1, exc); PyObject *t = PyTuple_Pack(2, PyUnicode_FromString(""), args); *match = PyObject_Call(PyExc_BaseExceptionGroup, t, NULL); Py_DECREF(t); Py_DECREF(args); copy_metadata(exc, *match); *rest = NULL; return *match ? 0 : -1; }
        *match = NULL; *rest = Py_NewRef(exc); return 0;
    }
    return excgroup_split_impl(exc, type, match, rest);
}
PyObject *piper_exception_group_merge(PyObject *raised, PyObject *rest) {
    if (!raised) return Py_XNewRef(rest);
    if (!rest) return Py_NewRef(raised);
    PyObject *items = PyTuple_Pack(2, raised, rest);
    if (!items) return NULL;
    PyObject *message = PyUnicode_FromString("");
    PyObject *args = message ? PyTuple_Pack(2, message, items) : NULL;
    Py_XDECREF(message);
    Py_DECREF(items);
    if (!args) return NULL;
    PyObject *merged = PyObject_Call(PyExc_BaseExceptionGroup, args, NULL);
    Py_DECREF(args);
    return merged;
}
static PyObject *excgroup_split(PyObject *o, PyObject *cond) { PyObject *m, *r; if (excgroup_split_impl(o, cond, &m, &r) < 0) return NULL; PyObject *t = PyTuple_Pack(2, m ? m : Py_None, r ? r : Py_None); Py_XDECREF(m); Py_XDECREF(r); return t; }
static PyObject *excgroup_subgroup(PyObject *o, PyObject *cond) { PyObject *m, *r; if (excgroup_split_impl(o, cond, &m, &r) < 0) return NULL; Py_XDECREF(r); return m ? m : Py_NewRef(Py_None); }
static PyMemberDef excgroup_members[] = { { "message", _Py_T_OBJECT, offsetof(ExceptionGroupObject, msg), Py_READONLY, NULL }, { "exceptions", _Py_T_OBJECT, offsetof(ExceptionGroupObject, excs), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
static PyMethodDef excgroup_methods[] = { { "derive", excgroup_derive, METH_O, NULL }, { "split", excgroup_split, METH_O, NULL }, { "subgroup", excgroup_subgroup, METH_O, NULL }, { NULL, NULL, 0, NULL } };
static PyTypeObject _PyExc_BaseExceptionGroup = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "BaseExceptionGroup", .tp_basicsize = sizeof(ExceptionGroupObject), .tp_dealloc = excgroup_dealloc, .tp_str = excgroup_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_doc = "A combination of multiple unrelated exceptions.", .tp_methods = excgroup_methods, .tp_members = excgroup_members, .tp_base = &_PyExc_BaseException, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = excgroup_init, .tp_new = excgroup_new };
PyObject *PyExc_BaseExceptionGroup = (PyObject *)&_PyExc_BaseExceptionGroup;
static PyTypeObject _PyExc_ExceptionGroup = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "ExceptionGroup", .tp_basicsize = sizeof(ExceptionGroupObject), .tp_dealloc = excgroup_dealloc, .tp_str = excgroup_str, .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_HAVE_GC, .tp_base = &_PyExc_BaseExceptionGroup, .tp_dictoffset = offsetof(PyBaseExceptionObject, dict), .tp_init = excgroup_init, .tp_new = excgroup_new };
PyObject *PyExc_ExceptionGroup = (PyObject *)&_PyExc_ExceptionGroup;

/* ---- registry ---------------------------------------------------------------------- */

static PyObject **all_exc[] = {
    &PyExc_BaseException, &PyExc_Exception, &PyExc_TypeError, &PyExc_StopIteration, &PyExc_StopAsyncIteration, &PyExc_GeneratorExit,
    &PyExc_KeyboardInterrupt, &PyExc_SystemExit, &PyExc_ArithmeticError, &PyExc_OverflowError, &PyExc_ZeroDivisionError, &PyExc_FloatingPointError,
    &PyExc_AssertionError, &PyExc_LookupError, &PyExc_IndexError, &PyExc_KeyError, &PyExc_ValueError, &PyExc_UnicodeError, &PyExc_UnicodeDecodeError,
    &PyExc_UnicodeEncodeError, &PyExc_UnicodeTranslateError, &PyExc_RuntimeError, &PyExc_RecursionError, &PyExc_NotImplementedError,
    &PyExc_PythonFinalizationError, &PyExc_MemoryError, &PyExc_SystemError, &PyExc_ReferenceError, &PyExc_BufferError, &PyExc_EOFError,
    &PyExc_ImportError, &PyExc_ModuleNotFoundError, &PyExc_NameError, &PyExc_UnboundLocalError, &PyExc_AttributeError, &PyExc_SyntaxError,
    &PyExc_IndentationError, &PyExc_TabError, &PyExc_OSError, &PyExc_FileNotFoundError, &PyExc_FileExistsError, &PyExc_PermissionError,
    &PyExc_IsADirectoryError, &PyExc_NotADirectoryError, &PyExc_TimeoutError, &PyExc_BlockingIOError, &PyExc_InterruptedError,
    &PyExc_ChildProcessError, &PyExc_ProcessLookupError, &PyExc_ConnectionError, &PyExc_BrokenPipeError, &PyExc_ConnectionAbortedError,
    &PyExc_ConnectionRefusedError, &PyExc_ConnectionResetError, &PyExc_Warning, &PyExc_UserWarning, &PyExc_DeprecationWarning,
    &PyExc_PendingDeprecationWarning, &PyExc_SyntaxWarning, &PyExc_RuntimeWarning, &PyExc_FutureWarning, &PyExc_ImportWarning,
    &PyExc_UnicodeWarning, &PyExc_BytesWarning, &PyExc_ResourceWarning, &PyExc_EncodingWarning, &PyExc_BaseExceptionGroup, &PyExc_ExceptionGroup,
};

void piper_init_exceptions(void) {
    for (size_t i = 0; i < sizeof(all_exc) / sizeof(all_exc[0]); i++) {
        PyTypeObject *t = (PyTypeObject *)*all_exc[i];
        if (!t->tp_alloc) t->tp_alloc = PyType_GenericAlloc;
        if (!t->tp_free) t->tp_free = PyObject_Free;
        if (!t->tp_getattro) t->tp_getattro = PyObject_GenericGetAttr;
        if (!t->tp_setattro) t->tp_setattro = PyObject_GenericSetAttr;
        PyType_Ready(t);
    }
}

PyObject *piper_exc_type(const char *name) {
    for (size_t i = 0; i < sizeof(all_exc) / sizeof(all_exc[0]); i++) if (!strcmp(((PyTypeObject *)*all_exc[i])->tp_name, name)) return *all_exc[i];
    return NULL;
}
PyObject *piper_exc_type_name(PyObject *e) { return PyUnicode_FromString(Py_TYPE(e)->tp_name); }

PyObject *PyErr_NewExceptionWithDoc(const char *name, const char *doc, PyObject *base, PyObject *dict) {
    const char *dot = strrchr(name, '.');
    if (!dot) { PyErr_SetString(PyExc_SystemError, "PyErr_NewException: name must be module.class"); return NULL; }
    PyObject *d = dict ? PyDict_Copy(dict) : PyDict_New();
    PyObject *mod = PyUnicode_FromStringAndSize(name, dot - name);
    PyDict_SetItemString(d, "__module__", mod);
    Py_DECREF(mod);
    if (doc) { PyObject *ds = PyUnicode_FromString(doc); PyDict_SetItemString(d, "__doc__", ds); Py_DECREF(ds); }
    PyObject *bases = base ? (PyTuple_Check(base) ? Py_NewRef(base) : PyTuple_Pack(1, base)) : PyTuple_Pack(1, PyExc_Exception);
    PyObject *r = PyType_New3(dot + 1, bases, d);
    Py_DECREF(bases); Py_DECREF(d);
    return r;
}
PyObject *PyErr_NewException(const char *name, PyObject *base, PyObject *dict) { return PyErr_NewExceptionWithDoc(name, NULL, base, dict); }

PyObject *piper_exception_new(PyObject *type, PyObject *args) { return make_exception(type, args); }
PyObject *PyException_GetTraceback(PyObject *e) { return Py_XNewRef(((PyBaseExceptionObject *)e)->traceback); }
int PyException_SetTraceback(PyObject *e, PyObject *tb) { Py_XSETREF(((PyBaseExceptionObject *)e)->traceback, tb == Py_None ? NULL : Py_XNewRef(tb)); return 0; }
PyObject *PyException_GetCause(PyObject *e) { return Py_XNewRef(((PyBaseExceptionObject *)e)->cause); }
void PyException_SetCause(PyObject *e, PyObject *cause) { PyBaseExceptionObject *b = (PyBaseExceptionObject *)e; Py_XSETREF(b->cause, cause); b->suppress_context = 1; }
PyObject *PyException_GetContext(PyObject *e) { return Py_XNewRef(((PyBaseExceptionObject *)e)->context); }
void PyException_SetContext(PyObject *e, PyObject *ctx) { Py_XSETREF(((PyBaseExceptionObject *)e)->context, ctx); }
PyObject *PyException_GetArgs(PyObject *e) { return Py_NewRef(((PyBaseExceptionObject *)e)->args); }
void PyException_SetArgs(PyObject *e, PyObject *args) { Py_XSETREF(((PyBaseExceptionObject *)e)->args, Py_NewRef(args)); }

/* ---- raise helpers for compiled code ---------------------------------------------- */

PyObject *piper_raise(PyObject *exc, PyObject *cause) {
    PyObject *type, *value;
    if (PyExceptionClass_Check(exc)) { type = exc; value = PyObject_CallNoArgs(exc); if (!value) return NULL; if (!PyExceptionInstance_Check(value)) { PyErr_Format(PyExc_TypeError, "calling %R should have returned an instance of BaseException, not %s", exc, Py_TYPE(value)->tp_name); Py_DECREF(value); return NULL; } }
    else if (PyExceptionInstance_Check(exc)) { type = (PyObject *)Py_TYPE(exc); value = Py_NewRef(exc); }
    else { PyErr_SetString(PyExc_TypeError, "exceptions must derive from BaseException"); return NULL; }
    PIPER_UNUSED(type);
    if (cause) {
        PyObject *fixed;
        if (PyExceptionClass_Check(cause)) { fixed = PyObject_CallNoArgs(cause); if (!fixed) { Py_DECREF(value); return NULL; } }
        else if (PyExceptionInstance_Check(cause)) fixed = Py_NewRef(cause);
        else if (cause == Py_None) fixed = NULL;
        else { Py_DECREF(value); PyErr_SetString(PyExc_TypeError, "exception causes must derive from BaseException"); return NULL; }
        PyException_SetCause(value, fixed);
    }
    PyErr_SetObject((PyObject *)Py_TYPE(value), value);
    Py_DECREF(value);
    return NULL;
}

PyObject *piper_reraise(void) {
    PyObject *e = piper_current_handled_exception();
    if (!e) { PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise"); return NULL; }
    PyErr_SetRaisedException(Py_NewRef(e));
    return NULL;
}

int piper_assert_failed(PyObject *msg) {
    if (msg) PyErr_SetObject(PyExc_AssertionError, msg); else PyErr_SetNone(PyExc_AssertionError);
    return -1;
}

/* ---- tracebacks ---------------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *tb_next; PyObject *filename, *funcname; int tb_lineno; } tracebackobject;
static void tb_dealloc(PyObject *o) { tracebackobject *t = (tracebackobject *)o; Py_XDECREF(t->tb_next); Py_XDECREF(t->filename); Py_XDECREF(t->funcname); PyObject_Free(o); }
static PyObject *tb_frame_get(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    extern PyObject *piper_fake_frame_new(PyObject *filename, PyObject *funcname, int lineno);
    tracebackobject *t = (tracebackobject *)o;
    return piper_fake_frame_new(t->filename, t->funcname, t->tb_lineno);
}
static PyObject *tb_lineno_get(PyObject *o, void *c) { PIPER_UNUSED(c); return PyLong_FromLong(((tracebackobject *)o)->tb_lineno); }
static PyObject *tb_lasti_get(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); return PyLong_FromLong(0); }
static PyMemberDef tb_members[] = { { "tb_next", _Py_T_OBJECT, offsetof(tracebackobject, tb_next), 0, NULL }, { NULL, 0, 0, 0, NULL } };
static PyGetSetDef tb_getsets[] = { { "tb_frame", tb_frame_get, NULL, NULL, NULL }, { "tb_lineno", tb_lineno_get, NULL, NULL, NULL }, { "tb_lasti", tb_lasti_get, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyTraceBack_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "traceback", .tp_basicsize = sizeof(tracebackobject), .tp_dealloc = tb_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_setattro = PyObject_GenericSetAttr, .tp_members = tb_members, .tp_getset = tb_getsets };

/* Called by compiled code when an exception propagates out of a frame. */
void piper_traceback_add(const char *filename, const char *funcname, int lineno) {
    if (!current_exc) return;
    tracebackobject *t = PyObject_New(tracebackobject, &PyTraceBack_Type);
    if (!t) { PyErr_Clear(); return; }
    t->filename = PyUnicode_FromString(filename ? filename : "<unknown>");
    t->funcname = PyUnicode_FromString(funcname ? funcname : "<module>");
    t->tb_lineno = lineno;
    PyBaseExceptionObject *e = (PyBaseExceptionObject *)current_exc;
    /* new entry goes in front (outermost frame first when printed) */
    t->tb_next = e->traceback;
    e->traceback = (PyObject *)t;
}
PyObject *piper_traceback_new(void) { Py_RETURN_NONE; }

void piper_print_traceback_lines(PyObject *tb) {
    for (tracebackobject *t = (tracebackobject *)tb; t; t = (tracebackobject *)t->tb_next) {
        fprintf(stderr, "  File \"%s\", line %d, in %s\n", PyUnicode_AsUTF8(t->filename), t->tb_lineno, PyUnicode_AsUTF8(t->funcname));
        /* source line */
        FILE *f = fopen(PyUnicode_AsUTF8(t->filename), "r");
        if (f) {
            char line[1024]; int n = 0;
            while (fgets(line, sizeof line, f)) { if (++n == t->tb_lineno) { char *s = line; while (*s == ' ' || *s == '\t') s++; size_t l = strlen(s); while (l && (s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = 0; fprintf(stderr, "    %s\n", s); break; } }
            fclose(f);
        }
    }
}

static void display_exception(PyObject *e, int depth) {
    if (depth > 10) return;
    PyBaseExceptionObject *be = (PyBaseExceptionObject *)e;
    if (be->cause) { display_exception(be->cause, depth + 1); fprintf(stderr, "\nThe above exception was the direct cause of the following exception:\n\n"); }
    else if (be->context && !be->suppress_context) { display_exception(be->context, depth + 1); fprintf(stderr, "\nDuring handling of the above exception, another exception occurred:\n\n"); }
    if (be->traceback) { fprintf(stderr, "Traceback (most recent call last):\n"); piper_print_traceback_lines(be->traceback); }
    PyObject *mod = PyDict_GetItemString(Py_TYPE(e)->tp_dict, "__module__");
    const char *name = _PyType_Name(Py_TYPE(e));
    PyObject *s = PyObject_Str(e);
    if (!s) { PyErr_Clear(); s = PyUnicode_FromString("<exception str() failed>"); }
    if (mod && PyUnicode_Check(mod) && !PyUnicode_EqualToUTF8(mod, "builtins") && !PyUnicode_EqualToUTF8(mod, "__main__")) fprintf(stderr, "%s.%s", PyUnicode_AsUTF8(mod), name);
    else fprintf(stderr, "%s", name);
    if (PyUnicode_GET_LENGTH(s)) fprintf(stderr, ": %s\n", PyUnicode_AsUTF8(s)); else fprintf(stderr, "\n");
    Py_DECREF(s);
    if (be->notes && PyList_Check(be->notes)) for (Py_ssize_t i = 0; i < PyList_GET_SIZE(be->notes); i++) { PyObject *n = PyObject_Str(PyList_GET_ITEM(be->notes, i)); if (n) { fprintf(stderr, "%s\n", PyUnicode_AsUTF8(n)); Py_DECREF(n); } else PyErr_Clear(); }
}
void PyErr_DisplayException(PyObject *e) { fflush(stdout); display_exception(e, 0); fflush(stderr); }
void PyErr_Display(PyObject *unused, PyObject *value, PyObject *tb) { PIPER_UNUSED(unused); PIPER_UNUSED(tb); PyErr_DisplayException(value); }
void PyErr_PrintEx(int set_sys_last_vars) {
    PIPER_UNUSED(set_sys_last_vars);
    PyObject *e = PyErr_GetRaisedException();
    if (!e) return;
    if (PyErr_GivenExceptionMatches(e, PyExc_SystemExit)) { extern void piper_handle_system_exit(PyObject *e); piper_handle_system_exit(e); }
    PyObject *hook = PySys_GetObject("excepthook");
    if (hook && hook != Py_None) {
        PyObject *tb = PyException_GetTraceback(e);
        PyObject *r = PyObject_CallFunctionObjArgs(hook, (PyObject *)Py_TYPE(e), e, tb ? tb : Py_None, NULL);
        Py_XDECREF(tb);
        if (r) { Py_DECREF(r); Py_DECREF(e); return; }
        PyErr_Clear();
    }
    PyErr_DisplayException(e);
    Py_DECREF(e);
}
void PyErr_Print(void) { PyErr_PrintEx(1); }
int piper_print_exception(void) { PyErr_Print(); return 1; }
