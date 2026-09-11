/* Resumable generator objects backed by compiler-owned heap frames. */
#include "internal.h"

typedef PyObject *(*piper_generator_resume)(PyObject *, PyObject *, int);
typedef struct {
    PyObject_HEAD
    PyObject *func;
    piper_generator_resume resume;
    PyObject **slots;
    Py_ssize_t slot_count;
    int state;
    unsigned char running, closed, started, kind;
} generatorobject;
typedef struct { PyObject_HEAD generatorobject *generator; PyObject *sent; int used, op; } asyncsendobject;

static PyObject *generator_send_impl(generatorobject *g, PyObject *value) {
    if (g->closed) { PyErr_SetNone(PyExc_StopIteration); return NULL; }
    if (g->running) { PyErr_SetString(PyExc_ValueError, "generator already executing"); return NULL; }
    if (!g->started && value != Py_None) { PyErr_SetString(PyExc_TypeError, "can't send non-None value to a just-started generator"); return NULL; }
    g->running = 1; g->started = 1;
    PyObject *result = g->resume((PyObject *)g, value, 0);
    g->running = 0;
    return result;
}

static PyObject *generator_next(PyObject *o) { return generator_send_impl((generatorobject *)o, Py_None); }
static PyObject *generator_send(PyObject *o, PyObject *value) { return generator_send_impl((generatorobject *)o, value); }
static PyObject *generator_close(PyObject *o, PyObject *unused) {
    PIPER_UNUSED(unused); generatorobject *g = (generatorobject *)o;
    if (!g->closed) { g->closed = 1; if (g->started && g->resume) { g->running = 1; PyObject *r = g->resume(o, Py_None, 1); g->running = 0; Py_XDECREF(r); if (PyErr_Occurred()) PyErr_Clear(); } }
    Py_RETURN_NONE;
}
static void generator_dealloc(PyObject *o) {
    generatorobject *g = (generatorobject *)o;
    for (Py_ssize_t i = 0; i < g->slot_count; i++) Py_XDECREF(g->slots[i]);
    PyMem_Free(g->slots); Py_XDECREF(g->func); PyObject_Free(o);
}
static PyObject *generator_repr(PyObject *o) {
    generatorobject *g = (generatorobject *)o;
    PyObject *name = g->func && Py_IS_TYPE(g->func, &PyFunction_Type) ? ((PiperFunctionObject *)g->func)->qualname : Py_None;
    const char *format = g->kind == 2 ? "<async_generator object %U at %p>" : g->kind == 1 ? "<coroutine object %U at %p>" : "<generator object %U at %p>";
    return PyUnicode_FromFormat(format, name, o);
}
static PyObject *generator_name(PyObject *o, void *c) { PIPER_UNUSED(c); generatorobject *g = (generatorobject *)o; return Py_NewRef(g->func && Py_IS_TYPE(g->func, &PyFunction_Type) ? ((PiperFunctionObject *)g->func)->name : Py_None); }
static PyObject *generator_qualname(PyObject *o, void *c) { PIPER_UNUSED(c); generatorobject *g = (generatorobject *)o; return Py_NewRef(g->func && Py_IS_TYPE(g->func, &PyFunction_Type) ? ((PiperFunctionObject *)g->func)->qualname : Py_None); }
static PyObject *generator_code(PyObject *o, void *c) { PIPER_UNUSED(c); generatorobject *g = (generatorobject *)o; extern PyObject *piper_code_for_function(PyObject *); return g->func ? piper_code_for_function(g->func) : Py_NewRef(Py_None); }
static PyObject *generator_frame(PyObject *o, void *c) { PIPER_UNUSED(c); return ((generatorobject *)o)->closed ? Py_NewRef(Py_None) : Py_NewRef(Py_None); }
static PyObject *generator_running(PyObject *o, void *c) { PIPER_UNUSED(c); return PyBool_FromLong(((generatorobject *)o)->running); }
static PyObject *generator_suspended(PyObject *o, void *c) { PIPER_UNUSED(c); generatorobject *g = (generatorobject *)o; return PyBool_FromLong(g->started && !g->closed && !g->running); }
static PyObject *coroutine_await(PyObject *o, PyObject *unused) { PIPER_UNUSED(unused); return Py_NewRef(o); }
static PyObject *async_generator_asend(PyObject *o, PyObject *value);
static PyObject *async_generator_anext(PyObject *o, PyObject *unused) { PIPER_UNUSED(unused); return async_generator_asend(o, Py_None); }
static PyObject *async_generator_aclose(PyObject *o, PyObject *unused);
static PyMethodDef generator_methods[] = { { "send", generator_send, METH_O, NULL }, { "close", generator_close, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyMethodDef coroutine_methods[] = { { "send", generator_send, METH_O, NULL }, { "close", generator_close, METH_NOARGS, NULL }, { "__await__", coroutine_await, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyMethodDef async_generator_methods[] = { { "__aiter__", coroutine_await, METH_NOARGS, NULL }, { "__anext__", async_generator_anext, METH_NOARGS, NULL }, { "asend", async_generator_asend, METH_O, NULL }, { "aclose", async_generator_aclose, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyGetSetDef generator_getsets[] = {
    { "__name__", generator_name, NULL, NULL, NULL }, { "__qualname__", generator_qualname, NULL, NULL, NULL },
    { "gi_code", generator_code, NULL, NULL, NULL }, { "gi_frame", generator_frame, NULL, NULL, NULL },
    { "gi_running", generator_running, NULL, NULL, NULL }, { "gi_suspended", generator_suspended, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};
static PyObject *coroutine_awaiting(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); return Py_NewRef(Py_None); }
static PyObject *coroutine_origin(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); return Py_NewRef(Py_None); }
static PyGetSetDef coroutine_getsets[] = {
    { "__name__", generator_name, NULL, NULL, NULL }, { "__qualname__", generator_qualname, NULL, NULL, NULL },
    { "cr_code", generator_code, NULL, NULL, NULL }, { "cr_frame", generator_frame, NULL, NULL, NULL },
    { "cr_running", generator_running, NULL, NULL, NULL }, { "cr_suspended", generator_suspended, NULL, NULL, NULL },
    { "cr_await", coroutine_awaiting, NULL, NULL, NULL }, { "cr_origin", coroutine_origin, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};
static PyGetSetDef async_generator_getsets[] = {
    { "__name__", generator_name, NULL, NULL, NULL }, { "__qualname__", generator_qualname, NULL, NULL, NULL },
    { "ag_code", generator_code, NULL, NULL, NULL }, { "ag_frame", generator_frame, NULL, NULL, NULL },
    { "ag_running", generator_running, NULL, NULL, NULL }, { "ag_await", coroutine_awaiting, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};
PyTypeObject PyGen_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "generator", .tp_basicsize = sizeof(generatorobject), .tp_dealloc = generator_dealloc, .tp_repr = generator_repr,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = generator_next,
    .tp_methods = generator_methods, .tp_getset = generator_getsets,
};
PyTypeObject PyCoro_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "coroutine", .tp_basicsize = sizeof(generatorobject), .tp_dealloc = generator_dealloc, .tp_repr = generator_repr,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = generator_next,
    .tp_methods = coroutine_methods, .tp_getset = coroutine_getsets,
};
PyTypeObject PyAsyncGen_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "async_generator", .tp_basicsize = sizeof(generatorobject), .tp_dealloc = generator_dealloc, .tp_repr = generator_repr,
    .tp_getattro = PyObject_GenericGetAttr, .tp_methods = async_generator_methods, .tp_getset = async_generator_getsets,
};

static void asyncsend_dealloc(PyObject *o) { asyncsendobject *send = (asyncsendobject *)o; Py_XDECREF(send->generator); Py_XDECREF(send->sent); PyObject_Free(o); }
static PyObject *asyncsend_next(PyObject *o) {
    asyncsendobject *send = (asyncsendobject *)o;
    if (send->used) { PyErr_SetString(PyExc_RuntimeError, "cannot reuse already awaited __anext__()/asend()"); return NULL; }
    send->used = 1;
    PyObject *result = send->op == 2 ? Py_NewRef(Py_None) : send->op ? generator_close((PyObject *)send->generator, NULL) : generator_send_impl(send->generator, send->sent);
    if (!result) return NULL;
    PyErr_SetObject(PyExc_StopIteration, result); Py_DECREF(result); return NULL;
}
static PyObject *asyncsend_await(PyObject *o, PyObject *unused) { PIPER_UNUSED(unused); return Py_NewRef(o); }
static PyMethodDef asyncsend_methods[] = { { "__await__", asyncsend_await, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyAsyncGenASend_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "async_generator_asend", .tp_basicsize = sizeof(asyncsendobject), .tp_dealloc = asyncsend_dealloc,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = asyncsend_next, .tp_methods = asyncsend_methods,
};
static PyObject *async_generator_asend(PyObject *o, PyObject *value) {
    asyncsendobject *send = PyObject_New(asyncsendobject, &PyAsyncGenASend_Type);
    if (!send) return NULL;
    send->generator = (generatorobject *)Py_NewRef(o); send->sent = Py_NewRef(value); send->used = 0; send->op = 0;
    return (PyObject *)send;
}
static PyObject *async_generator_aclose(PyObject *o, PyObject *unused) {
    PIPER_UNUSED(unused);
    PyObject *closed = generator_close(o, NULL);
    if (!closed) return NULL;
    Py_DECREF(closed);
    asyncsendobject *send = PyObject_New(asyncsendobject, &PyAsyncGenASend_Type);
    if (!send) return NULL;
    send->generator = NULL; send->sent = Py_NewRef(Py_None); send->used = 0; send->op = 2;
    return (PyObject *)send;
}

PyObject *piper_generator_new(PyObject *func, void *resume, Py_ssize_t slot_count, int kind) {
    generatorobject *g = PyObject_New(generatorobject, kind == 2 ? &PyAsyncGen_Type : kind == 1 ? &PyCoro_Type : &PyGen_Type); if (!g) return NULL;
    g->func = Py_NewRef(func); g->resume = (piper_generator_resume)resume; g->slot_count = slot_count;
    g->slots = PyMem_Calloc((size_t)(slot_count ? slot_count : 1), sizeof(PyObject *));
    if (!g->slots) { g->slot_count = 0; Py_DECREF(g); return PyErr_NoMemory(); }
    g->state = 0; g->running = g->closed = g->started = 0; g->kind = (unsigned char)kind;
    return (PyObject *)g;
}

PyObject *piper_await_iter(PyObject *awaitable) {
    PyObject *method = PyObject_GetAttrString(awaitable, "__await__");
    if (!method) { PyErr_Format(PyExc_TypeError, "object %s can't be used in 'await' expression", Py_TYPE(awaitable)->tp_name); return NULL; }
    PyObject *iterator = PyObject_CallNoArgs(method);
    Py_DECREF(method);
    if (!iterator) return NULL;
    if (!PyIter_Check(iterator)) { PyErr_Format(PyExc_TypeError, "__await__() returned non-iterator of type '%s'", Py_TYPE(iterator)->tp_name); Py_DECREF(iterator); return NULL; }
    return iterator;
}

PyObject *piper_async_iter(PyObject *iterable) {
    PyObject *method = PyObject_GetAttrString(iterable, "__aiter__");
    if (!method) { PyErr_Format(PyExc_TypeError, "'async for' requires an object with __aiter__ method, got %s", Py_TYPE(iterable)->tp_name); return NULL; }
    PyObject *iterator = PyObject_CallNoArgs(method);
    Py_DECREF(method);
    if (!iterator) return NULL;
    if (!PyObject_HasAttrString(iterator, "__anext__")) { PyErr_Format(PyExc_TypeError, "aiter() returned not an async iterator of type '%s'", Py_TYPE(iterator)->tp_name); Py_DECREF(iterator); return NULL; }
    return iterator;
}
PyObject *piper_async_next(PyObject *iterator) {
    PyObject *method = PyObject_GetAttrString(iterator, "__anext__");
    if (!method) return NULL;
    PyObject *awaitable = PyObject_CallNoArgs(method);
    Py_DECREF(method);
    return awaitable;
}
int piper_async_iteration_done(void) {
    if (!PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) return 0;
    PyErr_Clear();
    return 1;
}
PyObject *piper_async_with_enter(PyObject *manager, PyObject **exit) {
    PyObject *enter = PyObject_GetAttrString(manager, "__aenter__");
    if (!enter) return NULL;
    *exit = PyObject_GetAttrString(manager, "__aexit__");
    if (!*exit) { Py_DECREF(enter); return NULL; }
    PyObject *awaitable = PyObject_CallNoArgs(enter);
    Py_DECREF(enter);
    if (!awaitable) Py_CLEAR(*exit);
    return awaitable;
}
PyObject *piper_async_with_exit(PyObject *exit, PyObject *exc) {
    PyObject *type = exc ? Py_NewRef((PyObject *)Py_TYPE(exc)) : Py_NewRef(Py_None);
    PyObject *value = exc ? Py_NewRef(exc) : Py_NewRef(Py_None);
    PyObject *traceback = Py_NewRef(Py_None);
    PyObject *args[3] = { type, value, traceback };
    PyObject *awaitable = PyObject_Vectorcall(exit, args, 3, NULL);
    Py_DECREF(type); Py_DECREF(value); Py_DECREF(traceback);
    return awaitable;
}
PyObject *piper_generator_function(PyObject *o) { return ((generatorobject *)o)->func; }
PyObject **piper_generator_slot(PyObject *o, Py_ssize_t index) { generatorobject *g = (generatorobject *)o; return index >= 0 && index < g->slot_count ? &g->slots[index] : NULL; }
int piper_generator_state(PyObject *o) { return ((generatorobject *)o)->state; }
void piper_generator_set_state(PyObject *o, int state) { ((generatorobject *)o)->state = state; }
PyObject *piper_generator_finish(PyObject *o, PyObject *value) { generatorobject *g = (generatorobject *)o; g->closed = 1; PyErr_SetObject(g->kind == 2 ? PyExc_StopAsyncIteration : PyExc_StopIteration, value ? value : Py_None); return NULL; }
void piper_generator_abort(PyObject *o) { ((generatorobject *)o)->closed = 1; }

/* Return (finished, value). A yielded value and a delegation return value are
 * both owned by the result tuple. */
PyObject *piper_yield_from_next(PyObject *iterator, PyObject *sent, int started) {
    PyObject *value;
    if (started && sent != Py_None) {
        PyObject *send = PyObject_GetAttrString(iterator, "send");
        if (!send) return NULL;
        value = PyObject_CallOneArg(send, sent);
        Py_DECREF(send);
    } else {
        iternextfunc next = Py_TYPE(iterator)->tp_iternext;
        if (!next) { PyErr_Format(PyExc_TypeError, "'%s' object is not an iterator", Py_TYPE(iterator)->tp_name); return NULL; }
        value = next(iterator);
    }
    int finished = 0;
    if (!value) {
        if (!PyErr_Occurred()) value = Py_NewRef(Py_None);
        else {
            if (!PyErr_ExceptionMatches(PyExc_StopIteration)) return NULL;
            PyObject *stop = PyErr_GetRaisedException();
            value = PyObject_GetAttrString(stop, "value");
            Py_DECREF(stop);
            if (!value) return NULL;
        }
        finished = 1;
    }
    PyObject *flag = PyBool_FromLong(finished);
    PyObject *result = PyTuple_Pack(2, flag, value);
    Py_DECREF(flag); Py_DECREF(value);
    return result;
}
