/* Allocators. Plain malloc for now; a pymalloc-style arena comes later. */
#include "internal.h"

void *PyMem_RawMalloc(size_t n) { return malloc(n ? n : 1); }
void *PyMem_RawCalloc(size_t nelem, size_t elsize) { return calloc(nelem ? nelem : 1, elsize ? elsize : 1); }
void *PyMem_RawRealloc(void *p, size_t n) { return realloc(p, n ? n : 1); }
void PyMem_RawFree(void *p) { free(p); }

void *PyMem_Malloc(size_t n) { return PyMem_RawMalloc(n); }
void *PyMem_Calloc(size_t nelem, size_t elsize) { return PyMem_RawCalloc(nelem, elsize); }
void *PyMem_Realloc(void *p, size_t n) { return PyMem_RawRealloc(p, n); }
void PyMem_Free(void *p) { PyMem_RawFree(p); }

void *PyObject_Malloc(size_t n) { return PyMem_RawMalloc(n); }
void *PyObject_Calloc(size_t nelem, size_t elsize) { return PyMem_RawCalloc(nelem, elsize); }
void *PyObject_Realloc(void *p, size_t n) { return PyMem_RawRealloc(p, n); }
void PyObject_Free(void *p) { PyMem_RawFree(p); }

PyObject *PyObject_Init(PyObject *op, PyTypeObject *tp) {
    op->ob_refcnt_full = 0;
    op->ob_refcnt = 1;
    op->ob_type = tp;
    return op;
}

PyVarObject *PyObject_InitVar(PyVarObject *op, PyTypeObject *tp, Py_ssize_t size) {
    PyObject_Init((PyObject *)op, tp);
    op->ob_size = size;
    return op;
}

PyObject *_PyObject_New(PyTypeObject *tp) {
    PyObject *op = PyObject_Malloc((size_t)tp->tp_basicsize);
    if (!op) return PyErr_NoMemory();
    return PyObject_Init(op, tp);
}

PyVarObject *_PyObject_NewVar(PyTypeObject *tp, Py_ssize_t nitems) {
    size_t size = (size_t)tp->tp_basicsize + (size_t)nitems * (size_t)tp->tp_itemsize;
    PyVarObject *op = PyObject_Malloc(size);
    if (!op) { PyErr_NoMemory(); return NULL; }
    return PyObject_InitVar(op, tp, nitems);
}

PyObject *PyType_GenericAlloc(PyTypeObject *tp, Py_ssize_t nitems) {
    size_t size = (size_t)tp->tp_basicsize + (size_t)(nitems + 1) * (size_t)tp->tp_itemsize;
    PyObject *op = PyObject_Calloc(1, size);
    if (!op) return PyErr_NoMemory();
    if (tp->tp_itemsize) PyObject_InitVar((PyVarObject *)op, tp, nitems);
    else PyObject_Init(op, tp);
    if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE) Py_INCREF(tp);
    return op;
}

PyObject *PyType_GenericNew(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(args); PIPER_UNUSED(kwds);
    return tp->tp_alloc(tp, 0);
}

/* GC hooks are no-ops until the cycle collector lands. */
void PyObject_GC_Track(void *op) { PIPER_UNUSED(op); }
void PyObject_GC_UnTrack(void *op) { PIPER_UNUSED(op); }
PyObject *_PyObject_GC_New(PyTypeObject *tp) { return _PyObject_New(tp); }
PyVarObject *_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems) { return _PyObject_NewVar(tp, nitems); }
void PyObject_GC_Del(void *op) { PyObject_Free(op); }
int PyObject_IS_GC(PyObject *op) { return (Py_TYPE(op)->tp_flags & Py_TPFLAGS_HAVE_GC) != 0; }

void Py_IncRef(PyObject *o) { Py_XINCREF(o); }
void Py_DecRef(PyObject *o) { Py_XDECREF(o); }
Py_ssize_t Py_REFCNT_fn(PyObject *o) { return Py_REFCNT(o); }
/* Exported under the macro's name for FFI users. */
#undef Py_REFCNT
Py_ssize_t Py_REFCNT(PyObject *o) { return _Py_REFCNT(o); }
