/* Core object model. Struct layouts that C extensions can observe match
 * CPython 3.14 (default build, 64-bit) field for field. */
#ifndef PIPER_OBJECT_H
#define PIPER_OBJECT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t Py_ssize_t;
typedef Py_ssize_t Py_hash_t;
typedef size_t Py_uhash_t;
typedef uint32_t Py_UCS4;
typedef uint16_t Py_UCS2;
typedef uint8_t Py_UCS1;

int PyOS_snprintf(char *str, size_t size, const char *format, ...);
int PyOS_vsnprintf(char *str, size_t size, const char *format, va_list va);

#define PY_SSIZE_T_MAX INTPTR_MAX
#define PY_SSIZE_T_MIN INTPTR_MIN

/* ---- object header --------------------------------------------------- */

typedef struct _typeobject PyTypeObject;

typedef struct _object {
    union {
        int64_t ob_refcnt_full;
        struct {
            uint32_t ob_refcnt;
            uint16_t ob_overflow;
            uint16_t ob_flags;
        };
    };
    PyTypeObject *ob_type;
} PyObject;

typedef struct {
    PyObject ob_base;
    Py_ssize_t ob_size;
} PyVarObject;

#define PyObject_HEAD PyObject ob_base;
#define PyObject_VAR_HEAD PyVarObject ob_base;

#define _Py_IMMORTAL_INITIAL_REFCNT ((uint32_t)(3u << 30))
#define _Py_IMMORTAL_MINIMUM_REFCNT ((uint32_t)(1u << 30))
#define _Py_STATIC_IMMORTAL_INITIAL_REFCNT ((int64_t)_Py_IMMORTAL_INITIAL_REFCNT | ((int64_t)5 << 48))

#define _PyObject_CAST(op) ((PyObject *)(op))
#define Py_TYPE(ob) (_PyObject_CAST(ob)->ob_type)
#define Py_SIZE(ob) (((PyVarObject *)(ob))->ob_size)
#define Py_SET_TYPE(ob, t) (_PyObject_CAST(ob)->ob_type = (t))
#define Py_SET_SIZE(ob, n) (((PyVarObject *)(ob))->ob_size = (n))
#define Py_IS_TYPE(ob, t) (Py_TYPE(ob) == (t))

static inline bool _Py_IsImmortal(PyObject *op) { return (int32_t)op->ob_refcnt < 0; }
static inline Py_ssize_t _Py_REFCNT(PyObject *op) { return (Py_ssize_t)op->ob_refcnt; }
#define Py_REFCNT(ob) _Py_REFCNT(_PyObject_CAST(ob))

void _Py_Dealloc(PyObject *op);

static inline void _Py_INCREF(PyObject *op) {
    uint32_t cur = op->ob_refcnt;
    if (cur >= _Py_IMMORTAL_MINIMUM_REFCNT) return;
    op->ob_refcnt = cur + 1;
}
static inline void _Py_DECREF(PyObject *op) {
    if (_Py_IsImmortal(op)) return;
    if (--op->ob_refcnt == 0) _Py_Dealloc(op);
}
#define Py_INCREF(op) _Py_INCREF(_PyObject_CAST(op))
#define Py_DECREF(op) _Py_DECREF(_PyObject_CAST(op))
#define Py_XINCREF(op) do { PyObject *_py_xi = _PyObject_CAST(op); if (_py_xi) Py_INCREF(_py_xi); } while (0)
#define Py_XDECREF(op) do { PyObject *_py_xd = _PyObject_CAST(op); if (_py_xd) Py_DECREF(_py_xd); } while (0)
#define Py_CLEAR(op) do { PyObject *_py_cl = _PyObject_CAST(op); if (_py_cl) { (op) = NULL; Py_DECREF(_py_cl); } } while (0)
#define Py_SETREF(dst, src) do { PyObject *_py_old = _PyObject_CAST(dst); (dst) = (src); Py_DECREF(_py_old); } while (0)
#define Py_XSETREF(dst, src) do { PyObject *_py_old = _PyObject_CAST(dst); (dst) = (src); Py_XDECREF(_py_old); } while (0)
static inline PyObject *_Py_NewRef(PyObject *o) { Py_INCREF(o); return o; }
static inline PyObject *_Py_XNewRef(PyObject *o) { Py_XINCREF(o); return o; }
#define Py_NewRef(o) _Py_NewRef(_PyObject_CAST(o))
#define Py_XNewRef(o) _Py_XNewRef(_PyObject_CAST(o))

/* Static object initializer: immortal. */
#define PyObject_HEAD_INIT(type) { { .ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT }, (type) },
#define PyVarObject_HEAD_INIT(type, size) { PyObject_HEAD_INIT(type) (size) },

/* ---- slot function types ---------------------------------------------- */

struct _Py_buffer;
typedef PyObject *(*unaryfunc)(PyObject *);
typedef PyObject *(*binaryfunc)(PyObject *, PyObject *);
typedef PyObject *(*ternaryfunc)(PyObject *, PyObject *, PyObject *);
typedef int (*inquiry)(PyObject *);
typedef Py_ssize_t (*lenfunc)(PyObject *);
typedef PyObject *(*ssizeargfunc)(PyObject *, Py_ssize_t);
typedef int (*ssizeobjargproc)(PyObject *, Py_ssize_t, PyObject *);
typedef int (*objobjargproc)(PyObject *, PyObject *, PyObject *);
typedef int (*objobjproc)(PyObject *, PyObject *);
typedef int (*visitproc)(PyObject *, void *);
typedef int (*traverseproc)(PyObject *, visitproc, void *);
typedef void (*freefunc)(void *);
typedef void (*destructor)(PyObject *);
typedef PyObject *(*getattrfunc)(PyObject *, char *);
typedef PyObject *(*getattrofunc)(PyObject *, PyObject *);
typedef int (*setattrfunc)(PyObject *, char *, PyObject *);
typedef int (*setattrofunc)(PyObject *, PyObject *, PyObject *);
typedef PyObject *(*reprfunc)(PyObject *);
typedef Py_hash_t (*hashfunc)(PyObject *);
typedef PyObject *(*richcmpfunc)(PyObject *, PyObject *, int);
typedef PyObject *(*getiterfunc)(PyObject *);
typedef PyObject *(*iternextfunc)(PyObject *);
typedef PyObject *(*descrgetfunc)(PyObject *, PyObject *, PyObject *);
typedef int (*descrsetfunc)(PyObject *, PyObject *, PyObject *);
typedef int (*initproc)(PyObject *, PyObject *, PyObject *);
typedef PyObject *(*newfunc)(PyTypeObject *, PyObject *, PyObject *);
typedef PyObject *(*allocfunc)(PyTypeObject *, Py_ssize_t);
typedef PyObject *(*vectorcallfunc)(PyObject *callable, PyObject *const *args, size_t nargsf, PyObject *kwnames);
typedef int (*getbufferproc)(PyObject *, struct _Py_buffer *, int);
typedef void (*releasebufferproc)(PyObject *, struct _Py_buffer *);
typedef PyObject *(*PyCFunction)(PyObject *, PyObject *);
typedef PyObject *(*PyCFunctionWithKeywords)(PyObject *, PyObject *, PyObject *);
typedef PyObject *(*PyCFunctionFast)(PyObject *, PyObject *const *, Py_ssize_t);
typedef PyObject *(*PyCFunctionFastWithKeywords)(PyObject *, PyObject *const *, Py_ssize_t, PyObject *);
typedef PyObject *(*getter)(PyObject *, void *);
typedef int (*setter)(PyObject *, PyObject *, void *);

typedef struct _Py_buffer {
    void *buf;
    PyObject *obj;
    Py_ssize_t len;
    Py_ssize_t itemsize;
    int readonly;
    int ndim;
    char *format;
    Py_ssize_t *shape;
    Py_ssize_t *strides;
    Py_ssize_t *suboffsets;
    void *internal;
} Py_buffer;

typedef struct {
    binaryfunc nb_add, nb_subtract, nb_multiply, nb_remainder, nb_divmod;
    ternaryfunc nb_power;
    unaryfunc nb_negative, nb_positive, nb_absolute;
    inquiry nb_bool;
    unaryfunc nb_invert;
    binaryfunc nb_lshift, nb_rshift, nb_and, nb_xor, nb_or;
    unaryfunc nb_int;
    void *nb_reserved;
    unaryfunc nb_float;
    binaryfunc nb_inplace_add, nb_inplace_subtract, nb_inplace_multiply, nb_inplace_remainder;
    ternaryfunc nb_inplace_power;
    binaryfunc nb_inplace_lshift, nb_inplace_rshift, nb_inplace_and, nb_inplace_xor, nb_inplace_or;
    binaryfunc nb_floor_divide, nb_true_divide, nb_inplace_floor_divide, nb_inplace_true_divide;
    unaryfunc nb_index;
    binaryfunc nb_matrix_multiply, nb_inplace_matrix_multiply;
} PyNumberMethods;

typedef struct {
    lenfunc sq_length;
    binaryfunc sq_concat;
    ssizeargfunc sq_repeat;
    ssizeargfunc sq_item;
    void *was_sq_slice;
    ssizeobjargproc sq_ass_item;
    void *was_sq_ass_slice;
    objobjproc sq_contains;
    binaryfunc sq_inplace_concat;
    ssizeargfunc sq_inplace_repeat;
} PySequenceMethods;

typedef struct {
    lenfunc mp_length;
    binaryfunc mp_subscript;
    objobjargproc mp_ass_subscript;
} PyMappingMethods;

typedef PyObject *(*sendfunc_result_placeholder)(void);
typedef struct {
    unaryfunc am_await, am_aiter, am_anext;
    void *am_send;
} PyAsyncMethods;

typedef struct {
    getbufferproc bf_getbuffer;
    releasebufferproc bf_releasebuffer;
} PyBufferProcs;

typedef struct PyMethodDef {
    const char *ml_name;
    PyCFunction ml_meth;
    int ml_flags;
    const char *ml_doc;
} PyMethodDef;

typedef struct PyModuleDef_Base {
    PyObject_HEAD
    PyObject *(*m_init)(void);
    Py_ssize_t m_index;
    PyObject *m_copy;
} PyModuleDef_Base;

typedef struct PyModuleDef_Slot {
    int slot;
    void *value;
} PyModuleDef_Slot;

typedef struct PyModuleDef {
    PyModuleDef_Base m_base;
    const char *m_name;
    const char *m_doc;
    Py_ssize_t m_size;
    PyMethodDef *m_methods;
    PyModuleDef_Slot *m_slots;
    traverseproc m_traverse;
    inquiry m_clear;
    freefunc m_free;
} PyModuleDef;

#define PyModuleDef_HEAD_INIT { PyObject_HEAD_INIT(NULL) NULL, 0, NULL }
#define Py_mod_create 1
#define Py_mod_exec 2
#define Py_mod_multiple_interpreters 3
#define Py_mod_gil 4
#define Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED ((void *)0)
#define Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED ((void *)1)
#define Py_MOD_PER_INTERPRETER_GIL_SUPPORTED ((void *)2)
#define Py_MOD_GIL_USED ((void *)0)
#define Py_MOD_GIL_NOT_USED ((void *)1)

typedef struct PyMemberDef {
    const char *name;
    int type;
    Py_ssize_t offset;
    int flags;
    const char *doc;
} PyMemberDef;

typedef struct PyGetSetDef {
    const char *name;
    getter get;
    setter set;
    const char *doc;
    void *closure;
} PyGetSetDef;

#define METH_VARARGS 0x0001
#define METH_KEYWORDS 0x0002
#define METH_NOARGS 0x0004
#define METH_O 0x0008
#define METH_CLASS 0x0010
#define METH_STATIC 0x0020
#define METH_COEXIST 0x0040
#define METH_FASTCALL 0x0080
#define METH_METHOD 0x0200

/* PyMemberDef types */
#define Py_T_SHORT 0
#define Py_T_INT 1
#define Py_T_LONG 2
#define Py_T_FLOAT 3
#define Py_T_DOUBLE 4
#define Py_T_STRING 5
#define _Py_T_OBJECT 6
#define Py_T_CHAR 7
#define Py_T_BYTE 8
#define Py_T_UBYTE 9
#define Py_T_USHORT 10
#define Py_T_UINT 11
#define Py_T_ULONG 12
#define Py_T_STRING_INPLACE 13
#define Py_T_BOOL 14
#define Py_T_OBJECT_EX 16
#define Py_T_LONGLONG 17
#define Py_T_ULONGLONG 18
#define Py_T_PYSSIZET 19
#define _Py_T_NONE 20
#define Py_READONLY 1

struct _typeobject {
    PyObject_VAR_HEAD
    const char *tp_name;
    Py_ssize_t tp_basicsize, tp_itemsize;
    destructor tp_dealloc;
    Py_ssize_t tp_vectorcall_offset;
    getattrfunc tp_getattr;
    setattrfunc tp_setattr;
    PyAsyncMethods *tp_as_async;
    reprfunc tp_repr;
    PyNumberMethods *tp_as_number;
    PySequenceMethods *tp_as_sequence;
    PyMappingMethods *tp_as_mapping;
    hashfunc tp_hash;
    ternaryfunc tp_call;
    reprfunc tp_str;
    getattrofunc tp_getattro;
    setattrofunc tp_setattro;
    PyBufferProcs *tp_as_buffer;
    unsigned long tp_flags;
    const char *tp_doc;
    traverseproc tp_traverse;
    inquiry tp_clear;
    richcmpfunc tp_richcompare;
    Py_ssize_t tp_weaklistoffset;
    getiterfunc tp_iter;
    iternextfunc tp_iternext;
    PyMethodDef *tp_methods;
    PyMemberDef *tp_members;
    PyGetSetDef *tp_getset;
    PyTypeObject *tp_base;
    PyObject *tp_dict;
    descrgetfunc tp_descr_get;
    descrsetfunc tp_descr_set;
    Py_ssize_t tp_dictoffset;
    initproc tp_init;
    allocfunc tp_alloc;
    newfunc tp_new;
    freefunc tp_free;
    inquiry tp_is_gc;
    PyObject *tp_bases;
    PyObject *tp_mro;
    PyObject *tp_cache;
    void *tp_subclasses;
    PyObject *tp_weaklist;
    destructor tp_del;
    unsigned int tp_version_tag;
    destructor tp_finalize;
    vectorcallfunc tp_vectorcall;
    unsigned char tp_watched;
    uint16_t tp_versions_used;
};

/* tp_flags */
#define Py_TPFLAGS_MANAGED_DICT (1UL << 4)
#define Py_TPFLAGS_SEQUENCE (1UL << 5)
#define Py_TPFLAGS_MAPPING (1UL << 6)
#define Py_TPFLAGS_DISALLOW_INSTANTIATION (1UL << 7)
#define Py_TPFLAGS_IMMUTABLETYPE (1UL << 8)
#define Py_TPFLAGS_HEAPTYPE (1UL << 9)
#define Py_TPFLAGS_BASETYPE (1UL << 10)
#define Py_TPFLAGS_HAVE_VECTORCALL (1UL << 11)
#define Py_TPFLAGS_READY (1UL << 12)
#define Py_TPFLAGS_READYING (1UL << 13)
#define Py_TPFLAGS_HAVE_GC (1UL << 14)
#define Py_TPFLAGS_METHOD_DESCRIPTOR (1UL << 17)
#define Py_TPFLAGS_VALID_VERSION_TAG (1UL << 19)
#define Py_TPFLAGS_IS_ABSTRACT (1UL << 20)
#define Py_TPFLAGS_MATCH_SELF (1UL << 22)
#define Py_TPFLAGS_ITEMS_AT_END (1UL << 23)
#define Py_TPFLAGS_LONG_SUBCLASS (1UL << 24)
#define Py_TPFLAGS_LIST_SUBCLASS (1UL << 25)
#define Py_TPFLAGS_TUPLE_SUBCLASS (1UL << 26)
#define Py_TPFLAGS_BYTES_SUBCLASS (1UL << 27)
#define Py_TPFLAGS_UNICODE_SUBCLASS (1UL << 28)
#define Py_TPFLAGS_DICT_SUBCLASS (1UL << 29)
#define Py_TPFLAGS_BASE_EXC_SUBCLASS (1UL << 30)
#define Py_TPFLAGS_TYPE_SUBCLASS (1UL << 31)
#define Py_TPFLAGS_DEFAULT (0)

#define PyType_HasFeature(t, f) (((t)->tp_flags & (f)) != 0)
#define PyType_FastSubclass(t, f) PyType_HasFeature(t, f)

/* Rich comparison ops */
#define Py_LT 0
#define Py_LE 1
#define Py_EQ 2
#define Py_NE 3
#define Py_GT 4
#define Py_GE 5

#define PY_VECTORCALL_ARGUMENTS_OFFSET ((size_t)1 << (8 * sizeof(size_t) - 1))
static inline Py_ssize_t PyVectorcall_NARGS(size_t n) { return (Py_ssize_t)(n & ~PY_VECTORCALL_ARGUMENTS_OFFSET); }

/* ---- concrete layouts extensions read through macros ------------------ */

typedef struct {
    PyObject_HEAD
    double ob_fval;
} PyFloatObject;

typedef uint32_t digit;
typedef int32_t sdigit;
typedef uint64_t twodigits;
typedef int64_t stwodigits;
#define PyLong_SHIFT 30
#define PyLong_BASE ((digit)1 << PyLong_SHIFT)
#define PyLong_MASK ((digit)(PyLong_BASE - 1))
#define _PyLong_SIGN_MASK 3
#define _PyLong_NON_SIZE_BITS 3
typedef struct _PyLongValue {
    uintptr_t lv_tag;
    digit ob_digit[1];
} _PyLongValue;
typedef struct {
    PyObject_HEAD
    _PyLongValue long_value;
} PyLongObject;

typedef struct {
    PyObject_HEAD
    Py_ssize_t length;
    Py_hash_t hash;
    struct {
        unsigned int interned : 2;
        unsigned int kind : 3;
        unsigned int compact : 1;
        unsigned int ascii : 1;
        unsigned int statically_allocated : 1;
        unsigned int : 24;
    } state;
} PyASCIIObject;
typedef struct {
    PyASCIIObject _base;
    Py_ssize_t utf8_length;
    char *utf8;
} PyCompactUnicodeObject;
typedef struct {
    PyCompactUnicodeObject _base;
    union { void *any; Py_UCS1 *latin1; Py_UCS2 *ucs2; Py_UCS4 *ucs4; } data;
} PyUnicodeObject;
enum PyUnicode_Kind { PyUnicode_1BYTE_KIND = 1, PyUnicode_2BYTE_KIND = 2, PyUnicode_4BYTE_KIND = 4 };

typedef struct {
    PyObject_VAR_HEAD
    Py_hash_t ob_hash;
    char ob_sval[1];
} PyBytesObject;

typedef struct {
    PyObject_VAR_HEAD
    Py_hash_t ob_hash;
    PyObject *ob_item[1];
} PyTupleObject;

typedef struct {
    PyObject_VAR_HEAD
    PyObject **ob_item;
    Py_ssize_t allocated;
} PyListObject;

typedef struct {
    PyObject_HEAD
    PyObject *dict;
    PyObject *args;
    PyObject *notes;
    PyObject *traceback;
    PyObject *context;
    PyObject *cause;
    char suppress_context;
} PyBaseExceptionObject;

typedef struct {
    PyObject_HEAD
    PyObject *start, *stop, *step;
} PySliceObject;

typedef void (*PyCapsule_Destructor)(PyObject *);

/* Piper-private layouts (never visible through public macros). */
typedef struct _dictobject PyDictObject;

/* ---- well-known objects ---------------------------------------------- */

extern PyTypeObject PyType_Type, PyBaseObject_Type, PyLong_Type, PyBool_Type, PyFloat_Type, PyUnicode_Type,
    PyBytes_Type, PyList_Type, PyTuple_Type, PyDict_Type, PySet_Type, PyFrozenSet_Type, PySlice_Type,
    PyRange_Type, PyModule_Type, PyCFunction_Type, PyMethodDescr_Type, PyMethod_Type, PyFunction_Type,
    PyCell_Type, PyProperty_Type, PyStaticMethod_Type, PyClassMethod_Type, _PyNone_Type,
    _PyNotImplemented_Type, PyEllipsis_Type, PyEnum_Type, PyZip_Type, PyMap_Type, PyFilter_Type,
    PyReversed_Type, PySeqIter_Type, PyCallIter_Type, PyRangeIter_Type, PyListIter_Type, PyTupleIter_Type,
    PyDictKeys_Type, PyDictValues_Type, PyDictItems_Type, PyDictIterKey_Type, PyDictIterValue_Type,
    PyDictIterItem_Type, PyUnicodeIter_Type, PySetIter_Type, PyGen_Type, PyCoro_Type, PyAsyncGen_Type, PyAsyncGenASend_Type, PyTypeAlias_Type, PyTypeVar_Type, PyParamSpec_Type, PyTypeVarTuple_Type, PyMemberDescr_Type, PyGetSetDescr_Type,
    PyWrapperDescr_Type, PySuper_Type, PyByteArray_Type, PyComplex_Type, PyTraceBack_Type, PyCode_Type, PyModuleDef_Type,
    PyFrame_Type, PyCapsule_Type, _PyWeakref_RefType, PyBytesIter_Type, PyMemoryView_Type, PyPiperFunction_Type,
    PyTemplate_Type, PyInterpolation_Type, PyDictProxy_Type, PyClassMethodDescr_Type, PyMethodWrapper_Type, PySimpleNamespace_Type,
    PyGenericAlias_Type, PyUnion_Type;

extern PyObject _Py_NoneStruct, _Py_NotImplementedStruct, _Py_EllipsisObject;
#define Py_None (&_Py_NoneStruct)
#define Py_NotImplemented (&_Py_NotImplementedStruct)
#define Py_Ellipsis (&_Py_EllipsisObject)
#define Py_RETURN_NONE return Py_None
#define Py_RETURN_NOTIMPLEMENTED return Py_NotImplemented
#define Py_RETURN_TRUE return Py_True
#define Py_RETURN_FALSE return Py_False
#define Py_RETURN_BOOL(c) return (c) ? Py_True : Py_False

extern PyLongObject _Py_TrueStruct, _Py_FalseStruct;
#define Py_True ((PyObject *)&_Py_TrueStruct)
#define Py_False ((PyObject *)&_Py_FalseStruct)

/* type checks */
#define PyObject_TypeCheck(ob, tp) (Py_IS_TYPE(ob, tp) || PyType_IsSubtype(Py_TYPE(ob), (tp)))
#define PyType_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_TYPE_SUBCLASS)
#define PyType_CheckExact(op) Py_IS_TYPE(op, &PyType_Type)
#define PyLong_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_LONG_SUBCLASS)
#define PyLong_CheckExact(op) Py_IS_TYPE(op, &PyLong_Type)
#define PyBool_Check(op) Py_IS_TYPE(op, &PyBool_Type)
#define PyCapsule_CheckExact(op) Py_IS_TYPE((op), &PyCapsule_Type)
#define PyFloat_Check(op) PyObject_TypeCheck(op, &PyFloat_Type)
#define PyFloat_CheckExact(op) Py_IS_TYPE(op, &PyFloat_Type)
#define PyUnicode_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_UNICODE_SUBCLASS)
#define PyUnicode_CheckExact(op) Py_IS_TYPE(op, &PyUnicode_Type)
#define PyBytes_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_BYTES_SUBCLASS)
#define PyBytes_CheckExact(op) Py_IS_TYPE(op, &PyBytes_Type)
#define PyList_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_LIST_SUBCLASS)
#define PyList_CheckExact(op) Py_IS_TYPE(op, &PyList_Type)
#define PyTuple_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_TUPLE_SUBCLASS)
#define PyTuple_CheckExact(op) Py_IS_TYPE(op, &PyTuple_Type)
#define PyDict_Check(op) PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_DICT_SUBCLASS)
#define PyDict_CheckExact(op) Py_IS_TYPE(op, &PyDict_Type)
#define PyExceptionClass_Check(x) (PyType_Check(x) && PyType_FastSubclass((PyTypeObject *)(x), Py_TPFLAGS_BASE_EXC_SUBCLASS))
#define PyExceptionInstance_Check(x) PyType_FastSubclass(Py_TYPE(x), Py_TPFLAGS_BASE_EXC_SUBCLASS)
#define PySlice_Check(op) Py_IS_TYPE(op, &PySlice_Type)
#define PyModule_Check(op) PyObject_TypeCheck(op, &PyModule_Type)
#define PySet_Check(op) (Py_IS_TYPE(op, &PySet_Type) || PyType_IsSubtype(Py_TYPE(op), &PySet_Type))
#define PyFrozenSet_Check(op) (Py_IS_TYPE(op, &PyFrozenSet_Type) || PyType_IsSubtype(Py_TYPE(op), &PyFrozenSet_Type))
#define PyAnySet_Check(op) (PySet_Check(op) || PyFrozenSet_Check(op))
#define PyCFunction_Check(op) PyObject_TypeCheck(op, &PyCFunction_Type)
#define PyRange_Check(op) Py_IS_TYPE(op, &PyRange_Type)
int PyIndex_Check(PyObject *op);
int PyCallable_Check(PyObject *op);
int PyIter_Check(PyObject *op);
#define PyWeakref_CheckRef(op) Py_IS_TYPE((op), &_PyWeakref_RefType)

/* ---- memory ---------------------------------------------------------- */

void *PyMem_Malloc(size_t n);
void *PyMem_Calloc(size_t nelem, size_t elsize);
void *PyMem_Realloc(void *p, size_t n);
void PyMem_Free(void *p);
void *PyMem_RawMalloc(size_t n);
void *PyMem_RawCalloc(size_t nelem, size_t elsize);
void *PyMem_RawRealloc(void *p, size_t n);
void PyMem_RawFree(void *p);
void *PyObject_Malloc(size_t n);
void *PyObject_Calloc(size_t nelem, size_t elsize);
void *PyObject_Realloc(void *p, size_t n);
void PyObject_Free(void *p);
PyObject *_PyObject_New(PyTypeObject *tp);
PyVarObject *_PyObject_NewVar(PyTypeObject *tp, Py_ssize_t nitems);
PyObject *PyType_GenericAlloc(PyTypeObject *tp, Py_ssize_t nitems);
PyObject *PyType_GenericNew(PyTypeObject *tp, PyObject *args, PyObject *kwds);
PyObject *PyObject_Init(PyObject *op, PyTypeObject *tp);
PyVarObject *PyObject_InitVar(PyVarObject *op, PyTypeObject *tp, Py_ssize_t size);
#define PyObject_New(type, tp) ((type *)_PyObject_New(tp))
#define PyObject_NewVar(type, tp, n) ((type *)_PyObject_NewVar((tp), (n)))
#define PyObject_Del PyObject_Free
void PyObject_GC_Track(void *op);
void PyObject_GC_UnTrack(void *op);
PyObject *_PyObject_GC_New(PyTypeObject *tp);
PyVarObject *_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems);
void PyObject_GC_Del(void *op);
#define PyObject_GC_New(type, tp) ((type *)_PyObject_GC_New(tp))
#define PyObject_GC_NewVar(type, tp, n) ((type *)_PyObject_GC_NewVar((tp), (n)))
#define Py_VISIT(op) do { if (op) { int _v = visit(_PyObject_CAST(op), arg); if (_v) return _v; } } while (0)
int PyObject_IS_GC(PyObject *op);

/* refcount functions (non-inline API) */
void Py_IncRef(PyObject *o);
void Py_DecRef(PyObject *o);
Py_ssize_t Py_REFCNT_fn(PyObject *o);

/* ---- type ------------------------------------------------------------ */

int PyType_Ready(PyTypeObject *tp);
int PyType_IsSubtype(PyTypeObject *a, PyTypeObject *b);
unsigned long PyType_GetFlags(PyTypeObject *tp);
PyObject *PyType_GetName(PyTypeObject *tp);
PyObject *PyType_GetQualName(PyTypeObject *tp);
const char *_PyType_Name(PyTypeObject *tp);
PyObject *_PyType_Lookup(PyTypeObject *tp, PyObject *name);
PyObject *_PyType_LookupStr(PyTypeObject *tp, const char *name);
void PyType_Modified(PyTypeObject *tp);
PyObject *PyType_New3(const char *name, PyObject *bases, PyObject *dict);

/* ---- generic object protocol ------------------------------------------ */

PyObject *PyObject_Repr(PyObject *o);
PyObject *PyObject_Str(PyObject *o);
PyObject *PyObject_ASCII(PyObject *o);
Py_hash_t PyObject_Hash(PyObject *o);
Py_hash_t PyObject_HashNotImplemented(PyObject *o);
Py_hash_t _Py_HashPointer(const void *p);
Py_hash_t _Py_HashDouble(PyObject *inst, double v);
Py_hash_t _Py_HashBytes(const void *p, Py_ssize_t len);
int PyObject_IsTrue(PyObject *o);
int PyObject_Not(PyObject *o);
PyObject *PyObject_RichCompare(PyObject *a, PyObject *b, int op);
int PyObject_RichCompareBool(PyObject *a, PyObject *b, int op);
PyObject *PyObject_GetAttr(PyObject *o, PyObject *name);
PyObject *PyObject_GetAttrString(PyObject *o, const char *name);
int PyObject_GetOptionalAttr(PyObject *o, PyObject *name, PyObject **result);
int PyObject_GetOptionalAttrString(PyObject *o, const char *name, PyObject **result);
int PyObject_SetAttr(PyObject *o, PyObject *name, PyObject *v);
int PyObject_SetAttrString(PyObject *o, const char *name, PyObject *v);
int PyObject_DelAttr(PyObject *o, PyObject *name);
int PyObject_HasAttr(PyObject *o, PyObject *name);
int PyObject_HasAttrString(PyObject *o, const char *name);
int PyObject_HasAttrWithError(PyObject *o, PyObject *name);
PyObject *PyObject_GenericGetAttr(PyObject *o, PyObject *name);
int PyObject_GenericSetAttr(PyObject *o, PyObject *name, PyObject *v);
PyObject **_PyObject_GetDictPtr(PyObject *o);
PyObject *PyObject_GenericGetDict(PyObject *o, void *ctx);
int PyObject_GenericSetDict(PyObject *o, PyObject *v, void *ctx);
PyObject *PyObject_GetItem(PyObject *o, PyObject *k);
int PyObject_SetItem(PyObject *o, PyObject *k, PyObject *v);
int PyObject_DelItem(PyObject *o, PyObject *k);
Py_ssize_t PyObject_Length(PyObject *o);
Py_ssize_t PyObject_Size(PyObject *o);
Py_ssize_t PyObject_LengthHint(PyObject *o, Py_ssize_t dflt);
PyObject *PyObject_GetIter(PyObject *o);
PyObject *PyIter_Next(PyObject *it);
int PyIter_NextItem(PyObject *it, PyObject **item);
PyObject *PyObject_Type(PyObject *o);
PyObject *PyObject_Dir(PyObject *o);
PyObject *PyObject_Format(PyObject *o, PyObject *spec);
int PyObject_IsInstance(PyObject *o, PyObject *cls);
int PyObject_IsSubclass(PyObject *a, PyObject *b);
PyObject *PyObject_SelfIter(PyObject *o);
PyObject *PyObject_Bytes(PyObject *o);
int PyObject_Print(PyObject *o, void *fp, int flags);
int PyObject_CheckBuffer(PyObject *o);
int PyObject_GetBuffer(PyObject *o, Py_buffer *view, int flags);
void PyBuffer_Release(Py_buffer *view);
int PyBuffer_FillInfo(Py_buffer *view, PyObject *o, void *buf, Py_ssize_t len, int readonly, int flags);
int PyBuffer_IsContiguous(const Py_buffer *view, char order);
void *PyBuffer_GetPointer(const Py_buffer *view, const Py_ssize_t *indices);
Py_ssize_t PyBuffer_SizeFromFormat(const char *format);
int PyBuffer_ToContiguous(void *buf, const Py_buffer *view, Py_ssize_t len, char order);
int PyBuffer_FromContiguous(const Py_buffer *view, const void *buf, Py_ssize_t len, char order);
void PyBuffer_FillContiguousStrides(int ndims, const Py_ssize_t *shape, Py_ssize_t *strides, int itemsize, char order);
#define PyBUF_SIMPLE 0
#define PyBUF_WRITABLE 0x0001
#define PyBUF_FORMAT 0x0004
#define PyBUF_ND 0x0008
#define PyBUF_STRIDES (0x0010 | PyBUF_ND)
#define PyBUF_C_CONTIGUOUS (0x0020 | PyBUF_STRIDES)
#define PyBUF_F_CONTIGUOUS (0x0040 | PyBUF_STRIDES)
#define PyBUF_ANY_CONTIGUOUS (0x0080 | PyBUF_STRIDES)
#define PyBUF_INDIRECT (0x0100 | PyBUF_STRIDES)
#define PyBUF_CONTIG (PyBUF_ND | PyBUF_WRITABLE)
#define PyBUF_CONTIG_RO (PyBUF_ND)
#define PyBUF_STRIDED (PyBUF_STRIDES | PyBUF_WRITABLE)
#define PyBUF_STRIDED_RO (PyBUF_STRIDES)
#define PyBUF_RECORDS (PyBUF_STRIDES | PyBUF_WRITABLE | PyBUF_FORMAT)
#define PyBUF_RECORDS_RO (PyBUF_STRIDES | PyBUF_FORMAT)
#define PyBUF_FULL (PyBUF_INDIRECT | PyBUF_WRITABLE | PyBUF_FORMAT)
#define PyBUF_FULL_RO (PyBUF_INDIRECT | PyBUF_FORMAT)
#define PyBUF_READ 0x100
#define PyBUF_WRITE 0x200

/* calls */
PyObject *PyObject_Call(PyObject *f, PyObject *args, PyObject *kwargs);
PyObject *PyObject_CallObject(PyObject *f, PyObject *args);
PyObject *PyObject_CallNoArgs(PyObject *f);
PyObject *PyObject_CallOneArg(PyObject *f, PyObject *arg);
PyObject *PyObject_CallFunctionObjArgs(PyObject *f, ...);
PyObject *PyObject_CallMethodObjArgs(PyObject *o, PyObject *name, ...);
PyObject *PyObject_CallMethod(PyObject *o, const char *name, const char *fmt, ...);
PyObject *PyObject_CallFunction(PyObject *f, const char *fmt, ...);
PyObject *PyObject_CallMethodNoArgs(PyObject *o, PyObject *name);
PyObject *PyObject_CallMethodOneArg(PyObject *o, PyObject *name, PyObject *arg);
PyObject *PyObject_Vectorcall(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames);
PyObject *PyObject_VectorcallDict(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwargs);
PyObject *PyObject_VectorcallMethod(PyObject *name, PyObject *const *args, size_t nargsf, PyObject *kwnames);
PyObject *PyVectorcall_Call(PyObject *f, PyObject *args, PyObject *kwargs);
vectorcallfunc PyVectorcall_Function(PyObject *f);
PyObject *_PyObject_MakeTpCall(PyObject *f, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);

/* number protocol */
PyObject *PyNumber_Add(PyObject *, PyObject *);
PyObject *PyNumber_Subtract(PyObject *, PyObject *);
PyObject *PyNumber_Multiply(PyObject *, PyObject *);
PyObject *PyNumber_MatrixMultiply(PyObject *, PyObject *);
PyObject *PyNumber_TrueDivide(PyObject *, PyObject *);
PyObject *PyNumber_FloorDivide(PyObject *, PyObject *);
PyObject *PyNumber_Remainder(PyObject *, PyObject *);
PyObject *PyNumber_Divmod(PyObject *, PyObject *);
PyObject *PyNumber_Power(PyObject *, PyObject *, PyObject *);
PyObject *PyNumber_Negative(PyObject *);
PyObject *PyNumber_Positive(PyObject *);
PyObject *PyNumber_Absolute(PyObject *);
PyObject *PyNumber_Invert(PyObject *);
PyObject *PyNumber_Lshift(PyObject *, PyObject *);
PyObject *PyNumber_Rshift(PyObject *, PyObject *);
PyObject *PyNumber_And(PyObject *, PyObject *);
PyObject *PyNumber_Xor(PyObject *, PyObject *);
PyObject *PyNumber_Or(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceAdd(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceSubtract(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceMultiply(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceMatrixMultiply(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceTrueDivide(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceFloorDivide(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceRemainder(PyObject *, PyObject *);
PyObject *PyNumber_InPlacePower(PyObject *, PyObject *, PyObject *);
PyObject *PyNumber_InPlaceLshift(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceRshift(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceAnd(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceXor(PyObject *, PyObject *);
PyObject *PyNumber_InPlaceOr(PyObject *, PyObject *);
PyObject *PyNumber_Index(PyObject *);
PyObject *PyNumber_Long(PyObject *);
PyObject *PyNumber_Float(PyObject *);
Py_ssize_t PyNumber_AsSsize_t(PyObject *, PyObject *exc);
int PyNumber_Check(PyObject *);
PyObject *PyNumber_ToBase(PyObject *n, int base);
/* op codes for piper_binop */
enum { PIPER_OP_ADD, PIPER_OP_SUB, PIPER_OP_MUL, PIPER_OP_MATMUL, PIPER_OP_DIV, PIPER_OP_MOD, PIPER_OP_POW,
       PIPER_OP_LSHIFT, PIPER_OP_RSHIFT, PIPER_OP_OR, PIPER_OP_XOR, PIPER_OP_AND, PIPER_OP_FLOORDIV, PIPER_OP_COUNT };
PyObject *piper_binop(int op, PyObject *a, PyObject *b);
PyObject *piper_inplace_binop(int op, PyObject *a, PyObject *b);

/* sequence / mapping protocol */
int PySequence_Check(PyObject *);
Py_ssize_t PySequence_Size(PyObject *);
Py_ssize_t PySequence_Length(PyObject *);
PyObject *PySequence_GetItem(PyObject *, Py_ssize_t);
int PySequence_SetItem(PyObject *, Py_ssize_t, PyObject *);
int PySequence_DelItem(PyObject *, Py_ssize_t);
PyObject *PySequence_GetSlice(PyObject *, Py_ssize_t, Py_ssize_t);
int PySequence_Contains(PyObject *, PyObject *);
PyObject *PySequence_Concat(PyObject *, PyObject *);
PyObject *PySequence_Repeat(PyObject *, Py_ssize_t);
PyObject *PySequence_Tuple(PyObject *);
PyObject *PySequence_List(PyObject *);
PyObject *PySequence_Fast(PyObject *, const char *msg);
#define PySequence_Fast_GET_SIZE(o) (PyList_Check(o) ? Py_SIZE(o) : Py_SIZE(o))
#define PySequence_Fast_ITEMS(o) (PyList_Check(o) ? ((PyListObject *)(o))->ob_item : ((PyTupleObject *)(o))->ob_item)
#define PySequence_Fast_GET_ITEM(o, i) (PySequence_Fast_ITEMS(o)[i])
Py_ssize_t PySequence_Index(PyObject *, PyObject *);
Py_ssize_t PySequence_Count(PyObject *, PyObject *);
int PyMapping_Check(PyObject *);
Py_ssize_t PyMapping_Size(PyObject *);
PyObject *PyMapping_Keys(PyObject *);
PyObject *PyMapping_Values(PyObject *);
PyObject *PyMapping_Items(PyObject *);
PyObject *PyMapping_GetItemString(PyObject *, const char *);
int PyMapping_SetItemString(PyObject *, const char *, PyObject *);
int PyMapping_HasKey(PyObject *, PyObject *);
int PyMapping_HasKeyString(PyObject *, const char *);
int PyMapping_GetOptionalItem(PyObject *, PyObject *, PyObject **);

/* ---- int ------------------------------------------------------------- */

PyObject *PyLong_FromLong(long v);
PyObject *PyLong_FromUnsignedLong(unsigned long v);
PyObject *PyLong_FromLongLong(long long v);
PyObject *PyLong_FromUnsignedLongLong(unsigned long long v);
PyObject *PyLong_FromSsize_t(Py_ssize_t v);
PyObject *PyLong_FromSize_t(size_t v);
PyObject *PyLong_FromDouble(double v);
PyObject *PyLong_FromString(const char *s, char **pend, int base);
PyObject *PyLong_FromUnicodeObject(PyObject *u, int base);
PyObject *PyLong_FromVoidPtr(void *p);
long PyLong_AsLong(PyObject *o);
long PyLong_AsLongAndOverflow(PyObject *o, int *overflow);
long long PyLong_AsLongLong(PyObject *o);
long long PyLong_AsLongLongAndOverflow(PyObject *o, int *overflow);
unsigned long PyLong_AsUnsignedLong(PyObject *o);
unsigned long long PyLong_AsUnsignedLongLong(PyObject *o);
unsigned long PyLong_AsUnsignedLongMask(PyObject *o);
unsigned long long PyLong_AsUnsignedLongLongMask(PyObject *o);
Py_ssize_t PyLong_AsSsize_t(PyObject *o);
size_t PyLong_AsSize_t(PyObject *o);
double PyLong_AsDouble(PyObject *o);
void *PyLong_AsVoidPtr(PyObject *o);
int PyLong_AsInt(PyObject *o);
PyObject *PyLong_GetInfo(void);
int _PyLong_Sign(PyObject *o);
int64_t PyLong_GetSign_fn(PyObject *o);
int PyLong_GetSign(PyObject *o, int *sign);
int64_t _PyLong_NumBits(PyObject *o);
PyObject *_PyLong_FromByteArray(const unsigned char *bytes, size_t n, int little_endian, int is_signed);
int _PyLong_AsByteArray(PyLongObject *v, unsigned char *bytes, size_t n, int little_endian, int is_signed, int with_exceptions);
PyObject *_PyLong_Format(PyObject *o, int base);
PyObject *PyLong_FromBool(int v);
int PyLong_IsPositive(PyObject *o);
int PyLong_IsNegative(PyObject *o);
int PyLong_IsZero(PyObject *o);
PyObject *piper_int_from_i64(int64_t v);
PyObject *piper_int_from_decimal(const char *digits);
double piper_int_to_double(PyObject *o, int *overflow);
int piper_int_fits_i64(PyObject *o, int64_t *out);

/* ---- bool ------------------------------------------------------------ */
PyObject *PyBool_FromLong(long v);

/* ---- float ----------------------------------------------------------- */
PyObject *PyFloat_FromDouble(double v);
PyObject *PyFloat_FromString(PyObject *s);
double PyFloat_AsDouble(PyObject *o);
#define PyFloat_AS_DOUBLE(o) (((PyFloatObject *)(o))->ob_fval)
PyObject *PyFloat_GetInfo(void);
double PyFloat_GetMax(void);
double PyFloat_GetMin(void);
char *PyOS_double_to_string(double v, char fmt, int precision, int flags, int *type);
double PyOS_string_to_double(const char *s, char **endptr, PyObject *overflow_exc);
#define Py_DTSF_SIGN 0x01
#define Py_DTSF_ADD_DOT_0 0x02
#define Py_DTSF_ALT 0x04
#define Py_DTSF_NO_NEG_0 0x08
int piper_float_repr(double v, char *buf, size_t n);

/* ---- str ------------------------------------------------------------- */
PyObject *PyUnicode_New(Py_ssize_t size, Py_UCS4 maxchar);
PyObject *PyUnicode_FromString(const char *s);
PyObject *PyUnicode_FromStringAndSize(const char *s, Py_ssize_t n);
PyObject *PyUnicode_FromKindAndData(int kind, const void *buf, Py_ssize_t n);
PyObject *PyUnicode_FromFormat(const char *fmt, ...);
PyObject *PyUnicode_FromFormatV(const char *fmt, va_list va);
PyObject *PyUnicode_FromOrdinal(int ord);
PyObject *PyUnicode_FromObject(PyObject *o);
PyObject *PyUnicode_FromEncodedObject(PyObject *o, const char *enc, const char *errors);
PyObject *PyUnicode_DecodeUTF8(const char *s, Py_ssize_t n, const char *errors);
PyObject *PyUnicode_DecodeUTF8Stateful(const char *s, Py_ssize_t n, const char *errors, Py_ssize_t *consumed);
PyObject *PyUnicode_DecodeLatin1(const char *s, Py_ssize_t n, const char *errors);
PyObject *PyUnicode_DecodeASCII(const char *s, Py_ssize_t n, const char *errors);
PyObject *PyUnicode_DecodeFSDefault(const char *s);
PyObject *PyUnicode_DecodeFSDefaultAndSize(const char *s, Py_ssize_t n);
PyObject *PyUnicode_Decode(const char *s, Py_ssize_t n, const char *enc, const char *errors);
PyObject *PyUnicode_AsUTF8String(PyObject *u);
PyObject *PyUnicode_AsEncodedString(PyObject *u, const char *enc, const char *errors);
PyObject *PyUnicode_AsASCIIString(PyObject *u);
PyObject *PyUnicode_AsLatin1String(PyObject *u);
PyObject *PyUnicode_EncodeFSDefault(PyObject *u);
const char *PyUnicode_AsUTF8AndSize(PyObject *u, Py_ssize_t *n);
const char *PyUnicode_AsUTF8(PyObject *u);
Py_ssize_t PyUnicode_GetLength(PyObject *u);
Py_UCS4 PyUnicode_ReadChar(PyObject *u, Py_ssize_t i);
int PyUnicode_WriteChar(PyObject *u, Py_ssize_t i, Py_UCS4 c);
PyObject *PyUnicode_Substring(PyObject *u, Py_ssize_t start, Py_ssize_t end);
PyObject *PyUnicode_Concat(PyObject *a, PyObject *b);
void PyUnicode_Append(PyObject **pleft, PyObject *right);
void PyUnicode_AppendAndDel(PyObject **pleft, PyObject *right);
PyObject *PyUnicode_Join(PyObject *sep, PyObject *seq);
PyObject *PyUnicode_Split(PyObject *u, PyObject *sep, Py_ssize_t maxsplit);
PyObject *PyUnicode_Splitlines(PyObject *u, int keepends);
PyObject *PyUnicode_Replace(PyObject *u, PyObject *a, PyObject *b, Py_ssize_t maxcount);
Py_ssize_t PyUnicode_Find(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end, int direction);
Py_ssize_t PyUnicode_FindChar(PyObject *u, Py_UCS4 c, Py_ssize_t start, Py_ssize_t end, int direction);
Py_ssize_t PyUnicode_Count(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end);
Py_ssize_t PyUnicode_Tailmatch(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end, int direction);
int PyUnicode_Compare(PyObject *a, PyObject *b);
int PyUnicode_CompareWithASCIIString(PyObject *a, const char *b);
int PyUnicode_EqualToUTF8(PyObject *a, const char *b);
int PyUnicode_EqualToUTF8AndSize(PyObject *a, const char *b, Py_ssize_t n);
int PyUnicode_Equal(PyObject *a, PyObject *b);
PyObject *PyUnicode_RichCompare(PyObject *a, PyObject *b, int op);
int PyUnicode_Contains(PyObject *u, PyObject *sub);
PyObject *PyUnicode_Format(PyObject *fmt, PyObject *args);
PyObject *PyUnicode_InternFromString(const char *s);
void PyUnicode_InternInPlace(PyObject **p);
void PyUnicode_InternImmortal(PyObject **p);
PyObject *PyUnicode_Type_fn(void);
int PyUnicode_IsIdentifier(PyObject *u);
Py_UCS4 PyUnicode_MaxChar(PyObject *u);
void *PyUnicode_DATA_fn(PyObject *u);
int PyUnicode_KIND_fn(PyObject *u);
Py_ssize_t PyUnicode_CopyCharacters(PyObject *to, Py_ssize_t to_start, PyObject *from, Py_ssize_t from_start, Py_ssize_t n);
Py_ssize_t PyUnicode_Fill(PyObject *u, Py_ssize_t start, Py_ssize_t length, Py_UCS4 c);
PyObject *_PyUnicode_JoinArray(PyObject *sep, PyObject *const *items, Py_ssize_t n);
PyObject *PyUnicode_AsUCS4Copy_fn(PyObject *u);
Py_UCS4 *PyUnicode_AsUCS4(PyObject *u, Py_UCS4 *buf, Py_ssize_t bufsize, int copy_null);
Py_UCS4 *PyUnicode_AsUCS4Copy(PyObject *u);
PyObject *PyUnicode_Partition(PyObject *u, PyObject *sep);
PyObject *PyUnicode_RPartition(PyObject *u, PyObject *sep);
PyObject *PyUnicode_RSplit(PyObject *u, PyObject *sep, Py_ssize_t maxsplit);
PyObject *PyUnicode_Translate(PyObject *u, PyObject *table, const char *errors);
PyObject *PyUnicode_BuildEncodingMap(PyObject *s);
Py_ssize_t PyUnicode_AsWideChar(PyObject *u, wchar_t *w, Py_ssize_t size);
wchar_t *PyUnicode_AsWideCharString(PyObject *u, Py_ssize_t *size);
PyObject *PyUnicode_FromWideChar(const wchar_t *w, Py_ssize_t size);
int PyUnicode_FSConverter(PyObject *o, void *result);
int PyUnicode_FSDecoder(PyObject *o, void *result);

static inline int PyUnicode_KIND(PyObject *op) { return ((PyASCIIObject *)op)->state.kind; }
static inline int PyUnicode_IS_ASCII(PyObject *op) { return ((PyASCIIObject *)op)->state.ascii; }
static inline int PyUnicode_IS_COMPACT(PyObject *op) { return ((PyASCIIObject *)op)->state.compact; }
static inline int PyUnicode_IS_COMPACT_ASCII(PyObject *op) { return PyUnicode_IS_ASCII(op) && PyUnicode_IS_COMPACT(op); }
static inline Py_ssize_t PyUnicode_GET_LENGTH(PyObject *op) { return ((PyASCIIObject *)op)->length; }
static inline void *PyUnicode_DATA(PyObject *op) {
    if (PyUnicode_IS_COMPACT(op)) {
        if (PyUnicode_IS_ASCII(op)) return (void *)(((PyASCIIObject *)op) + 1);
        return (void *)(((PyCompactUnicodeObject *)op) + 1);
    }
    return ((PyUnicodeObject *)op)->data.any;
}
#define PyUnicode_1BYTE_DATA(op) ((Py_UCS1 *)PyUnicode_DATA(op))
#define PyUnicode_2BYTE_DATA(op) ((Py_UCS2 *)PyUnicode_DATA(op))
#define PyUnicode_4BYTE_DATA(op) ((Py_UCS4 *)PyUnicode_DATA(op))
static inline Py_UCS4 PyUnicode_READ(int kind, const void *data, Py_ssize_t i) {
    if (kind == PyUnicode_1BYTE_KIND) return ((const Py_UCS1 *)data)[i];
    if (kind == PyUnicode_2BYTE_KIND) return ((const Py_UCS2 *)data)[i];
    return ((const Py_UCS4 *)data)[i];
}
static inline void PyUnicode_WRITE(int kind, void *data, Py_ssize_t i, Py_UCS4 v) {
    if (kind == PyUnicode_1BYTE_KIND) ((Py_UCS1 *)data)[i] = (Py_UCS1)v;
    else if (kind == PyUnicode_2BYTE_KIND) ((Py_UCS2 *)data)[i] = (Py_UCS2)v;
    else ((Py_UCS4 *)data)[i] = v;
}
static inline Py_UCS4 PyUnicode_READ_CHAR(PyObject *u, Py_ssize_t i) { return PyUnicode_READ(PyUnicode_KIND(u), PyUnicode_DATA(u), i); }
static inline Py_UCS4 PyUnicode_MAX_CHAR_VALUE(PyObject *op) {
    if (PyUnicode_IS_ASCII(op)) return 0x7f;
    int k = PyUnicode_KIND(op);
    return k == 1 ? 0xff : k == 2 ? 0xffff : 0x10ffff;
}
#define PyUnicode_READY(op) 0
#define PyUnicode_IS_READY(op) 1

/* ---- bytes ----------------------------------------------------------- */
PyObject *PyBytes_FromStringAndSize(const char *s, Py_ssize_t n);
PyObject *PyBytes_FromString(const char *s);
PyObject *PyBytes_FromFormat(const char *fmt, ...);
PyObject *PyBytes_FromObject(PyObject *o);
char *PyBytes_AsString(PyObject *o);
Py_ssize_t PyBytes_Size(PyObject *o);
int PyBytes_AsStringAndSize(PyObject *o, char **s, Py_ssize_t *n);
void PyBytes_Concat(PyObject **a, PyObject *b);
void PyBytes_ConcatAndDel(PyObject **a, PyObject *b);
int _PyBytes_Resize(PyObject **o, Py_ssize_t n);
PyObject *PyBytes_Repr(PyObject *o, int smartquotes);
PyObject *PyBytes_Join(PyObject *sep, PyObject *iterable);
#define PyBytes_AS_STRING(o) (((PyBytesObject *)(o))->ob_sval)
#define PyBytes_GET_SIZE(o) Py_SIZE(o)

/* ---- tuple ----------------------------------------------------------- */
PyObject *PyTuple_New(Py_ssize_t n);
Py_ssize_t PyTuple_Size(PyObject *t);
PyObject *PyTuple_GetItem(PyObject *t, Py_ssize_t i);
int PyTuple_SetItem(PyObject *t, Py_ssize_t i, PyObject *v);
PyObject *PyTuple_GetSlice(PyObject *t, Py_ssize_t a, Py_ssize_t b);
PyObject *PyTuple_Pack(Py_ssize_t n, ...);
PyObject *PyTuple_FromArray(PyObject *const *items, Py_ssize_t n);
int _PyTuple_Resize(PyObject **t, Py_ssize_t n);
#define PyTuple_GET_SIZE(t) Py_SIZE(t)
#define PyTuple_GET_ITEM(t, i) (((PyTupleObject *)(t))->ob_item[i])
#define PyTuple_SET_ITEM(t, i, v) (((PyTupleObject *)(t))->ob_item[i] = (v))
#define PyTuple_ITEMS(t) (((PyTupleObject *)(t))->ob_item)

/* ---- list ------------------------------------------------------------ */
PyObject *PyList_New(Py_ssize_t n);
Py_ssize_t PyList_Size(PyObject *l);
PyObject *PyList_GetItem(PyObject *l, Py_ssize_t i);
PyObject *PyList_GetItemRef(PyObject *l, Py_ssize_t i);
int PyList_SetItem(PyObject *l, Py_ssize_t i, PyObject *v);
int PyList_Insert(PyObject *l, Py_ssize_t i, PyObject *v);
int PyList_Append(PyObject *l, PyObject *v);
PyObject *PyList_GetSlice(PyObject *l, Py_ssize_t a, Py_ssize_t b);
int PyList_SetSlice(PyObject *l, Py_ssize_t a, Py_ssize_t b, PyObject *v);
int PyList_Sort(PyObject *l);
int PyList_Reverse(PyObject *l);
PyObject *PyList_AsTuple(PyObject *l);
int PyList_Extend(PyObject *l, PyObject *iterable);
int PyList_Clear(PyObject *l);
PyObject *PyList_FromArray(PyObject *const *items, Py_ssize_t n);
#define PyList_GET_SIZE(l) Py_SIZE(l)
#define PyList_GET_ITEM(l, i) (((PyListObject *)(l))->ob_item[i])
#define PyList_SET_ITEM(l, i, v) (((PyListObject *)(l))->ob_item[i] = (v))
#define PyList_ITEMS(l) (((PyListObject *)(l))->ob_item)

/* ---- dict ------------------------------------------------------------ */
PyObject *PyDict_New(void);
PyObject *PyDict_GetItem(PyObject *d, PyObject *k);
PyObject *PyDict_GetItemWithError(PyObject *d, PyObject *k);
PyObject *PyDict_GetItemString(PyObject *d, const char *k);
int PyDict_GetItemRef(PyObject *d, PyObject *k, PyObject **result);
int PyDict_GetItemStringRef(PyObject *d, const char *k, PyObject **result);
int PyDict_SetItem(PyObject *d, PyObject *k, PyObject *v);
int PyDict_SetItemString(PyObject *d, const char *k, PyObject *v);
int PyDict_DelItem(PyObject *d, PyObject *k);
int PyDict_DelItemString(PyObject *d, const char *k);
int PyDict_Contains(PyObject *d, PyObject *k);
int PyDict_ContainsString(PyObject *d, const char *k);
Py_ssize_t PyDict_Size(PyObject *d);
PyObject *PyDict_Keys(PyObject *d);
PyObject *PyDict_Values(PyObject *d);
PyObject *PyDict_Items(PyObject *d);
PyObject *PyDict_Copy(PyObject *d);
void PyDict_Clear(PyObject *d);
int PyDict_Next(PyObject *d, Py_ssize_t *pos, PyObject **k, PyObject **v);
int PyDict_Update(PyObject *d, PyObject *other);
int PyDict_Merge(PyObject *d, PyObject *other, int override);
int PyDict_MergeFromSeq2(PyObject *d, PyObject *seq, int override);
PyObject *PyDict_SetDefault(PyObject *d, PyObject *k, PyObject *dflt);
int PyDict_SetDefaultRef(PyObject *d, PyObject *k, PyObject *dflt, PyObject **result);
int PyDict_Pop(PyObject *d, PyObject *k, PyObject **result);
int PyDict_PopString(PyObject *d, const char *k, PyObject **result);
PyObject *PyDictProxy_New(PyObject *d);
#define PyDict_GET_SIZE(d) PyDict_Size(d)
PyObject *piper_dict_lookup(PyObject *d, PyObject *key, Py_hash_t hash);
int piper_dict_insert(PyObject *d, PyObject *key, Py_hash_t hash, PyObject *value);

/* ---- set ------------------------------------------------------------- */
PyObject *PySet_New(PyObject *iterable);
PyObject *PyFrozenSet_New(PyObject *iterable);
int PySet_Add(PyObject *s, PyObject *k);
int PySet_Discard(PyObject *s, PyObject *k);
int PySet_Contains(PyObject *s, PyObject *k);
Py_ssize_t PySet_Size(PyObject *s);
PyObject *PySet_Pop(PyObject *s);
int PySet_Clear(PyObject *s);
int _PySet_NextEntry(PyObject *s, Py_ssize_t *pos, PyObject **k, Py_hash_t *hash);
#define PySet_GET_SIZE(s) PySet_Size(s)

/* ---- slice / range --------------------------------------------------- */
PyObject *PySlice_New(PyObject *start, PyObject *stop, PyObject *step);
int PySlice_Unpack(PyObject *s, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step);
Py_ssize_t PySlice_AdjustIndices(Py_ssize_t length, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t step);
int PySlice_GetIndicesEx(PyObject *s, Py_ssize_t length, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step, Py_ssize_t *slicelength);
PyObject *piper_range_new(PyObject *start, PyObject *stop, PyObject *step);

/* ---- iterators ------------------------------------------------------- */
PyObject *PySeqIter_New(PyObject *seq);
PyObject *PyCallIter_New(PyObject *callable, PyObject *sentinel);

/* ---- functions / methods / modules ----------------------------------- */
typedef struct {
    PyObject_HEAD
    PyMethodDef *m_ml;
    PyObject *m_self;
    PyObject *m_module;
    PyObject *m_weakreflist;
    vectorcallfunc vectorcall;
} PyCFunctionObject;
PyObject *PyCFunction_NewEx(PyMethodDef *ml, PyObject *self, PyObject *module);
PyObject *PyCFunction_New(PyMethodDef *ml, PyObject *self);
PyObject *PyCMethod_New(PyMethodDef *ml, PyObject *self, PyObject *module, PyTypeObject *cls);
PyObject *PyDescr_NewMethod(PyTypeObject *tp, PyMethodDef *ml);
PyObject *PyDescr_NewClassMethod(PyTypeObject *tp, PyMethodDef *ml);
PyObject *PyDescr_NewMember(PyTypeObject *tp, PyMemberDef *m);
PyObject *PyDescr_NewGetSet(PyTypeObject *tp, PyGetSetDef *g);
PyObject *PyMethod_New(PyObject *func, PyObject *self);
PyObject *PyMethod_Function(PyObject *m);
PyObject *PyMethod_Self(PyObject *m);
PyObject *PyStaticMethod_New(PyObject *f);
PyObject *PyClassMethod_New(PyObject *f);
PyObject *PyCell_New(PyObject *v);
PyObject *PyCell_Get(PyObject *c);
int PyCell_Set(PyObject *c, PyObject *v);
#define PyCell_GET(c) (((PyCellObject *)(c))->ob_ref)
#define PyCell_SET(c, v) (((PyCellObject *)(c))->ob_ref = (v))
typedef struct { PyObject_HEAD PyObject *ob_ref; } PyCellObject;

/* Compiled piper function: native code + Python-level metadata. */
typedef PyObject *(*piper_native_fn)(PyObject *self_func, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);
typedef struct {
    PyObject_HEAD
    piper_native_fn fn;          /* native entry taking (func, args, nargs, kwnames) */
    PyObject *name;
    PyObject *qualname;
    PyObject *module_name;
    PyObject *globals;           /* module dict */
    PyObject *builtins;
    PyObject *defaults;          /* tuple or NULL */
    PyObject *kwdefaults;        /* dict or NULL */
    PyObject *closure;           /* tuple of cells or NULL */
    PyObject *annotations;
    PyObject *dict;
    PyObject *doc;
    PyObject *argnames;          /* tuple of str: positional+keyword names in order */
    int32_t posonly_count;
    int32_t arg_count;           /* positional (incl. posonly) */
    int32_t kwonly_count;
    uint8_t has_varargs;
    uint8_t has_varkw;
    uint8_t is_generator;
    uint8_t is_coroutine;
    uint8_t is_async_generator;
    vectorcallfunc vectorcall;
} PiperFunctionObject;
PyObject *piper_function_new(piper_native_fn fn, PyObject *name, PyObject *qualname, PyObject *globals, PyObject *argnames,
                             int posonly, int argcount, int kwonly, int flags, PyObject *defaults, PyObject *kwdefaults, PyObject *closure);
#define PIPER_FN_VARARGS 1
#define PIPER_FN_VARKW 2
#define PIPER_FN_GENERATOR 4
#define PIPER_FN_COROUTINE 8
#define PIPER_FN_ASYNC_GENERATOR 16
/* Bind call arguments to the function's parameter slots (fills `slots`
 * with nargs_total entries: args..., kwonly..., *args tuple, **kw dict). */
int piper_bind_args(PiperFunctionObject *f, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames, PyObject **slots);
PyObject *piper_closure_cell(PyObject *func, Py_ssize_t i);
PyObject *piper_function_globals(PyObject *func);
PyObject *piper_function_builtins(PyObject *func);
void piper_super_push(PyObject *cls, PyObject *self);
void piper_super_pop(void);

typedef struct {
    PyObject_HEAD
    PyObject *md_dict;
    void *md_def;
    void *md_state;
    PyObject *md_weaklist;
    PyObject *md_name;
} PyModuleObject;
PyObject *PyModule_New(const char *name);
PyObject *PyModule_NewObject(PyObject *name);
PyObject *PyModuleDef_Init(PyModuleDef *def);
PyObject *PyModule_Create2(PyModuleDef *def, int api_version);
void *PyModule_GetState(PyObject *module);
PyObject *PyState_FindModule(PyModuleDef *def);
int PyState_AddModule(PyObject *module, PyModuleDef *def);
int PyState_RemoveModule(PyModuleDef *def);
#define PyModule_Create(def) PyModule_Create2((def), PYTHON_API_VERSION)
#ifndef PYTHON_API_VERSION
#define PYTHON_API_VERSION 1013
#endif
PyObject *PyModule_GetDict(PyObject *m);
PyObject *PyModule_GetNameObject(PyObject *m);
const char *PyModule_GetName(PyObject *m);
int PyModule_AddObjectRef(PyObject *m, const char *name, PyObject *v);
int PyModule_AddObject(PyObject *m, const char *name, PyObject *v);
int PyModule_Add(PyObject *m, const char *name, PyObject *v);
int PyModule_AddIntConstant(PyObject *m, const char *name, long v);
int PyModule_AddStringConstant(PyObject *m, const char *name, const char *v);
int PyModule_AddFunctions(PyObject *m, PyMethodDef *defs);
int PyModule_AddType(PyObject *m, PyTypeObject *tp);
PyObject *PyImport_AddModule(const char *name);
PyObject *PyImport_AddModuleRef(const char *name);
PyObject *PyImport_GetModuleDict(void);
PyObject *PyImport_GetModule(PyObject *name);
PyObject *PyImport_ImportModule(const char *name);
PyObject *PyImport_Import(PyObject *name);
PyObject *PyImport_ImportModuleLevelObject(PyObject *name, PyObject *globals, PyObject *locals, PyObject *fromlist, int level);
PyObject *piper_import(PyObject *name, PyObject *globals, PyObject *fromlist, int level);
PyObject *piper_import_from(PyObject *module, PyObject *name);
int piper_import_star(PyObject *module, PyObject *globals);

/* ---- capsules -------------------------------------------------------- */
PyObject *PyCapsule_New(void *pointer, const char *name, PyCapsule_Destructor destructor);
int PyCapsule_IsValid(PyObject *capsule, const char *name);
void *PyCapsule_GetPointer(PyObject *capsule, const char *name);
PyCapsule_Destructor PyCapsule_GetDestructor(PyObject *capsule);
void *PyCapsule_GetContext(PyObject *capsule);
const char *PyCapsule_GetName(PyObject *capsule);
int PyCapsule_SetPointer(PyObject *capsule, void *pointer);
int PyCapsule_SetDestructor(PyObject *capsule, PyCapsule_Destructor destructor);
int PyCapsule_SetContext(PyObject *capsule, void *context);
int PyCapsule_SetName(PyObject *capsule, const char *name);
void *PyCapsule_Import(const char *name, int no_block);

/* ---- weak references ------------------------------------------------- */
PyObject *PyWeakref_NewRef(PyObject *object, PyObject *callback);
int PyWeakref_GetRef(PyObject *reference, PyObject **object);
PyObject *PyWeakref_GetObject(PyObject *reference);
void PyObject_ClearWeakRefs(PyObject *object);

/* ---- errors ---------------------------------------------------------- */
extern PyObject *PyExc_BaseException, *PyExc_BaseExceptionGroup, *PyExc_Exception, *PyExc_TypeError, *PyExc_ValueError,
    *PyExc_KeyError, *PyExc_IndexError, *PyExc_AttributeError, *PyExc_NameError, *PyExc_UnboundLocalError,
    *PyExc_ZeroDivisionError, *PyExc_StopIteration, *PyExc_StopAsyncIteration, *PyExc_RuntimeError,
    *PyExc_NotImplementedError, *PyExc_RecursionError, *PyExc_OverflowError, *PyExc_ArithmeticError,
    *PyExc_FloatingPointError, *PyExc_AssertionError, *PyExc_LookupError, *PyExc_ImportError,
    *PyExc_ModuleNotFoundError, *PyExc_OSError, *PyExc_FileNotFoundError, *PyExc_FileExistsError,
    *PyExc_PermissionError, *PyExc_IsADirectoryError, *PyExc_NotADirectoryError, *PyExc_TimeoutError,
    *PyExc_BlockingIOError, *PyExc_BrokenPipeError, *PyExc_ConnectionError, *PyExc_ConnectionAbortedError,
    *PyExc_ConnectionRefusedError, *PyExc_ConnectionResetError, *PyExc_InterruptedError, *PyExc_ChildProcessError,
    *PyExc_ProcessLookupError, *PyExc_EOFError, *PyExc_MemoryError, *PyExc_SystemError, *PyExc_SystemExit,
    *PyExc_KeyboardInterrupt, *PyExc_GeneratorExit, *PyExc_UnicodeError, *PyExc_UnicodeDecodeError,
    *PyExc_UnicodeEncodeError, *PyExc_UnicodeTranslateError, *PyExc_BufferError, *PyExc_SyntaxError,
    *PyExc_IndentationError, *PyExc_TabError, *PyExc_ReferenceError, *PyExc_Warning, *PyExc_UserWarning,
    *PyExc_DeprecationWarning, *PyExc_PendingDeprecationWarning, *PyExc_SyntaxWarning, *PyExc_RuntimeWarning,
    *PyExc_FutureWarning, *PyExc_ImportWarning, *PyExc_UnicodeWarning, *PyExc_BytesWarning, *PyExc_ResourceWarning,
    *PyExc_EncodingWarning, *PyExc_ExceptionGroup, *PyExc_PythonFinalizationError;
void PyErr_SetString(PyObject *type, const char *msg);
void PyErr_SetObject(PyObject *type, PyObject *value);
void PyErr_SetNone(PyObject *type);
PyObject *PyErr_Format(PyObject *type, const char *fmt, ...);
PyObject *PyErr_FormatV(PyObject *type, const char *fmt, va_list va);
PyObject *PyErr_Occurred(void);
void PyErr_Clear(void);
PyObject *PyErr_GetRaisedException(void);
void PyErr_SetRaisedException(PyObject *e);
void PyErr_Fetch(PyObject **type, PyObject **value, PyObject **tb);
void PyErr_Restore(PyObject *type, PyObject *value, PyObject *tb);
void PyErr_NormalizeException(PyObject **type, PyObject **value, PyObject **tb);
int PyErr_ExceptionMatches(PyObject *type);
int PyErr_GivenExceptionMatches(PyObject *given, PyObject *type);
PyObject *PyErr_NoMemory(void);
PyObject *PyErr_SetFromErrno(PyObject *type);
PyObject *PyErr_SetFromErrnoWithFilename(PyObject *type, const char *filename);
PyObject *PyErr_SetFromErrnoWithFilenameObject(PyObject *type, PyObject *filename);
PyObject *PyErr_SetFromErrnoWithFilenameObjects(PyObject *type, PyObject *f1, PyObject *f2);
void PyErr_BadInternalCall(void);
int PyErr_BadArgument(void);
void PyErr_WriteUnraisable(PyObject *obj);
void PyErr_Print(void);
void PyErr_PrintEx(int set_sys_last_vars);
void PyErr_Display(PyObject *unused, PyObject *value, PyObject *tb);
void PyErr_DisplayException(PyObject *e);
int PyErr_WarnEx(PyObject *category, const char *msg, Py_ssize_t stack_level);
int PyErr_WarnFormat(PyObject *category, Py_ssize_t stack_level, const char *fmt, ...);
int PyErr_ResourceWarning(PyObject *source, Py_ssize_t stack_level, const char *fmt, ...);
int PyErr_CheckSignals(void);
void PyErr_SetInterrupt(void);
PyObject *PyErr_NewException(const char *name, PyObject *base, PyObject *dict);
PyObject *PyErr_NewExceptionWithDoc(const char *name, const char *doc, PyObject *base, PyObject *dict);
PyObject *PyException_GetTraceback(PyObject *e);
int PyException_SetTraceback(PyObject *e, PyObject *tb);
PyObject *PyException_GetCause(PyObject *e);
void PyException_SetCause(PyObject *e, PyObject *cause);
PyObject *PyException_GetContext(PyObject *e);
void PyException_SetContext(PyObject *e, PyObject *ctx);
PyObject *PyException_GetArgs(PyObject *e);
void PyException_SetArgs(PyObject *e, PyObject *args);
PyObject *piper_exc_type(const char *name);
PyObject *piper_exception_new(PyObject *type, PyObject *args);
void piper_traceback_add(const char *filename, const char *funcname, int lineno);
int piper_exception_group_match(PyObject *exc, PyObject *type, PyObject **match, PyObject **rest);
PyObject *piper_exception_group_merge(PyObject *raised, PyObject *rest);
int _PyErr_ChainExceptions1(PyObject *exc);
void _PyErr_ChainExceptions(PyObject *t, PyObject *v, PyObject *tb);
int Py_EnterRecursiveCall(const char *where);
void Py_LeaveRecursiveCall(void);
int Py_ReprEnter(PyObject *o);
void Py_ReprLeave(PyObject *o);

/* ---- argument parsing ------------------------------------------------ */
int PyArg_ParseTuple(PyObject *args, const char *fmt, ...);
int PyArg_ParseTupleAndKeywords(PyObject *args, PyObject *kw, const char *fmt, char **kwlist, ...);
int PyArg_VaParse(PyObject *args, const char *fmt, va_list va);
int PyArg_VaParseTupleAndKeywords(PyObject *args, PyObject *kw, const char *fmt, char **kwlist, va_list va);
int PyArg_UnpackTuple(PyObject *args, const char *name, Py_ssize_t min, Py_ssize_t max, ...);
int PyArg_Parse(PyObject *arg, const char *fmt, ...);
int PyArg_ValidateKeywordArguments(PyObject *kw);
PyObject *Py_BuildValue(const char *fmt, ...);
PyObject *Py_VaBuildValue(const char *fmt, va_list va);
PyObject *_Py_BuildValue_SizeT(const char *fmt, ...);
int _PyArg_NoKeywords(const char *name, PyObject *kw);
int _PyArg_NoPositional(const char *name, PyObject *args);
int _PyArg_CheckPositional(const char *name, Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max);
/* piper helpers for builtins written in C */
int piper_args_range(const char *name, Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max);
int piper_no_kwargs(const char *name, PyObject *kwnames);
Py_ssize_t piper_kwarg_index(PyObject *kwnames, const char *name);

/* ---- runtime --------------------------------------------------------- */
void piper_initialize(void);
PyObject *piper_builtins(void);
PyObject *piper_sys_module(void);
int piper_main(int argc, char **argv, PyObject *(*module_init)(void));
void piper_set_argv(int argc, char **argv);
PyObject *piper_none(void);
PyObject *piper_true(void);
PyObject *piper_false(void);
PyObject *piper_not_implemented(void);
PyObject *piper_ellipsis(void);
int piper_print_exception(void);
void piper_fatal(const char *msg);
int Py_IsInitialized(void);
void Py_Initialize(void);
void Py_InitializeEx(int);
int Py_FinalizeEx(void);
void Py_Finalize(void);
void Py_Exit(int status);
void _Py_FatalErrorFunc(const char *func, const char *message);
int Py_AtExit(void (*func)(void));
const char *Py_GetVersion(void);
const char *Py_GetPlatform(void);
int Py_IsNone(PyObject *o);
int Py_IsTrue(PyObject *o);
int Py_IsFalse(PyObject *o);
int Py_Is(PyObject *a, PyObject *b);
PyObject *PySys_GetObject(const char *name);
int PySys_SetObject(const char *name, PyObject *v);
void PySys_WriteStdout(const char *fmt, ...);
void PySys_WriteStderr(const char *fmt, ...);
void PySys_FormatStdout(const char *fmt, ...);
void PySys_FormatStderr(const char *fmt, ...);
PyObject *PyEval_GetBuiltins(void);
PyObject *PyEval_GetGlobals(void);
PyObject *PyEval_GetLocals(void);
void PyEval_InitThreads(void);
void *PyEval_SaveThread(void);
void PyEval_RestoreThread(void *ts);
int PyGILState_Check(void);
int PyGILState_Ensure(void);
void PyGILState_Release(int s);
void *PyThreadState_Get(void);
void *PyThreadState_GetUnchecked(void);
void *PyEval_SaveThread(void);
void PyEval_RestoreThread(void *state);
#define Py_BEGIN_ALLOW_THREADS { void *_save = PyEval_SaveThread();
#define Py_END_ALLOW_THREADS PyEval_RestoreThread(_save); }
#define Py_BLOCK_THREADS PyEval_RestoreThread(_save);
#define Py_UNBLOCK_THREADS _save = PyEval_SaveThread();
PyObject *PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals);
PyObject *PyRun_StringFlags(const char *s, int start, PyObject *g, PyObject *l, void *flags);
PyObject *Py_CompileString(const char *s, const char *filename, int start);
PyObject *piper_builtin_import_hook(PyObject *name, PyObject *globals, PyObject *locals, PyObject *fromlist, int level);
int piper_run_main_module(PyObject *(*module_init)(void));

/* ---- helpers for compiled code --------------------------------------- */
PyObject *piper_load_global(PyObject *globals, PyObject *builtins, PyObject *name);
PyObject *piper_load_name(PyObject *locals, PyObject *globals, PyObject *builtins, PyObject *name);
int piper_store_global(PyObject *globals, PyObject *name, PyObject *v);
int piper_delete_global(PyObject *globals, PyObject *name);
PyObject *piper_unbound_local(PyObject *name);
PyObject *piper_unbound_free(PyObject *name);
int piper_unpack_sequence(PyObject *seq, Py_ssize_t n, PyObject **out);
int piper_unpack_ex(PyObject *seq, Py_ssize_t before, Py_ssize_t after, PyObject **out);
PyObject *piper_build_string(PyObject *const *parts, Py_ssize_t n);
PyObject *piper_format_value(PyObject *v, int conversion, PyObject *spec);
PyObject *piper_build_slice(PyObject *a, PyObject *b, PyObject *c);
PyObject *piper_compare(int op, PyObject *a, PyObject *b);
PyObject *piper_contains(PyObject *item, PyObject *container, int negate);
PyObject *piper_is(PyObject *a, PyObject *b, int negate);
int piper_exception_matches(PyObject *exc, PyObject *type);
PyObject *piper_get_awaitable(PyObject *o);
PyObject *piper_get_aiter(PyObject *o);
PyObject *piper_get_anext(PyObject *o);
PyObject *piper_make_class(PyObject *func_body, PyObject *name, PyObject *bases, PyObject *kwds, PyObject *globals);
PyObject *piper_build_class_body_ns(void);
PyObject *piper_import_name(PyObject *globals, PyObject *name, PyObject *fromlist, int level);
int piper_setup_annotations(PyObject *ns);
PyObject *piper_list_extend(PyObject *list, PyObject *iterable);
PyObject *piper_list_to_tuple(PyObject *list);
int piper_set_add(PyObject *set, PyObject *v);
int piper_set_update(PyObject *set, PyObject *iterable);
int piper_dict_update(PyObject *d, PyObject *other);
int piper_dict_merge_call(PyObject *d, PyObject *other, PyObject *func);
PyObject *piper_call(PyObject *callable, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);
PyObject *piper_call_ex(PyObject *callable, PyObject *args, PyObject *kwargs);
PyObject *piper_load_method(PyObject *obj, PyObject *name, PyObject **self_out);
PyObject *piper_gen_new(PyObject *func, PyObject *frame_state, int kind);
PyObject *piper_raise(PyObject *exc, PyObject *cause);
PyObject *piper_reraise(void);
void piper_exc_info_push(PyObject *exc);
PyObject *piper_exc_info_pop(void);
PyObject *piper_current_handled_exception(void);
int piper_with_enter(PyObject *mgr, PyObject **exit_out, PyObject **value_out);
int piper_with_exit(PyObject *exit, PyObject *exc);
int piper_assert_failed(PyObject *msg);
PyObject *piper_cell_new(PyObject *v);
PyObject *piper_cell_get(PyObject *cell, PyObject *name, int is_free);
void piper_cell_set(PyObject *cell, PyObject *v);
PyObject *piper_match_class(PyObject *subject, PyObject *cls, Py_ssize_t npos, PyObject *kwnames);
PyObject *piper_match_keys(PyObject *subject, PyObject *keys);
int piper_match_sequence_check(PyObject *subject);
int piper_match_mapping_check(PyObject *subject);
PyObject *piper_intern(const char *s);
PyObject *piper_str_const(const char *utf8, Py_ssize_t n);
PyObject *piper_bytes_const(const char *data, Py_ssize_t n);
PyObject *piper_int_const(const char *decimal);
PyObject *piper_float_const(double v);
PyObject *piper_complex_const(double re, double im);
PyObject *piper_tuple_const(PyObject *const *items, Py_ssize_t n);
PyObject *piper_bool(int v);
PyObject *piper_debug_true(void);

/* traceback / frames for error reporting in compiled code */
void piper_frame_push(const char *filename, const char *funcname, PyObject *globals);
void piper_frame_pop(void);
void piper_frame_set_line(int lineno);
PyObject *piper_frame_globals(void);

#define Py_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define Py_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define Py_ABS(x) ((x) < 0 ? -(x) : (x))
#define Py_ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define Py_UNUSED(x) x##_unused __attribute__((unused))
#define Py_UNREACHABLE() __builtin_unreachable()
#define Py_CHARMASK(c) ((unsigned char)((c) & 0xff))
#define PyDoc_STR(s) s
#define PyDoc_STRVAR(name, s) static const char name[] = s
#define PyDoc_VAR(name) static const char name[]
#define PyAPI_FUNC(t) t
#define PyAPI_DATA(t) extern t
#define PyMODINIT_FUNC PyObject *
#define Py_hash_t_defined 1

#ifdef __cplusplus
}
#endif
#endif
