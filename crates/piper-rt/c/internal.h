/* Private helpers shared by the runtime sources. */
#ifndef PIPER_INTERNAL_H
#define PIPER_INTERNAL_H

#include "piper/object.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <math.h>

/* Modular hashing parameters (CPython's) */
#define _PyHASH_BITS 61
#define _PyHASH_MODULUS (((size_t)1 << _PyHASH_BITS) - 1)
#define _PyHASH_INF 314159
#define _PyHASH_IMAG 1000003UL
#define _PyHASH_MULTIPLIER 1000003UL

/* Small ints */
#define PIPER_NSMALLNEGINTS 5
#define PIPER_NSMALLPOSINTS 257
extern PyLongObject piper_small_ints[PIPER_NSMALLNEGINTS + PIPER_NSMALLPOSINTS];

/* long internals */
static inline Py_ssize_t piper_long_ndigits(const PyLongObject *v) { return (Py_ssize_t)(v->long_value.lv_tag >> _PyLong_NON_SIZE_BITS); }
static inline int piper_long_sign(const PyLongObject *v) { return 1 - (int)(v->long_value.lv_tag & _PyLong_SIGN_MASK); } /* +1, 0, -1 */
static inline int piper_long_is_zero(const PyLongObject *v) { return (v->long_value.lv_tag & _PyLong_SIGN_MASK) == 1; }
static inline int piper_long_is_neg(const PyLongObject *v) { return (v->long_value.lv_tag & _PyLong_SIGN_MASK) == 2; }
static inline int piper_long_is_compact(const PyLongObject *v) { return v->long_value.lv_tag < (2 << _PyLong_NON_SIZE_BITS); }
static inline stwodigits piper_long_compact_value(const PyLongObject *v) {
    Py_ssize_t sign = 1 - (Py_ssize_t)(v->long_value.lv_tag & _PyLong_SIGN_MASK);
    return (stwodigits)(sign * (stwodigits)v->long_value.ob_digit[0]);
}
PyLongObject *piper_long_alloc(Py_ssize_t ndigits);
PyObject *piper_long_normalize(PyLongObject *v);
void piper_long_set_sign_and_size(PyLongObject *v, int sign, Py_ssize_t ndigits);
int piper_long_compare(PyLongObject *a, PyLongObject *b);
PyObject *piper_long_from_digits_str(const char *s, Py_ssize_t n, int base, int *ok);
PyObject *piper_long_to_decimal_string(PyObject *v);
PyObject *piper_long_bit_length(PyObject *v);

/* str internals */
PyObject *piper_unicode_repr(PyObject *u);
Py_hash_t piper_unicode_hash(PyObject *u);
PyObject *piper_unicode_from_ucs4(const Py_UCS4 *buf, Py_ssize_t n);
Py_ssize_t piper_unicode_find_ucs4(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end, int direction);
PyObject *piper_str_format_method(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);
PyObject *piper_format_spec_apply(PyObject *value, PyObject *spec);
int piper_unicode_eq(PyObject *a, PyObject *b);

/* dict internals */
PyObject *piper_dict_new_presized(Py_ssize_t n);
int piper_dict_next(PyObject *d, Py_ssize_t *pos, PyObject **k, PyObject **v, Py_hash_t *hash);
PyObject *piper_dict_keys_view(PyObject *d, int kind);
Py_ssize_t piper_dict_version(PyObject *d);

/* misc */
Py_hash_t piper_hash_bytes(const void *p, Py_ssize_t n);
PyObject *piper_repr_join(const char *open, PyObject *const *items, Py_ssize_t n, const char *close, int trailing_comma_single);
int piper_index_ssize(PyObject *o, Py_ssize_t *out, PyObject *exc_type);
PyObject *piper_seq_iter_new(PyObject *seq);
PyObject *piper_tuple_from_iterable(PyObject *it);
PyObject *piper_list_from_iterable(PyObject *it);
int piper_list_sort_impl(PyObject *l, PyObject *key, int reverse);
PyObject *piper_bound_method_new(PyObject *func, PyObject *self);
void piper_init_types(void);
void piper_init_types_core(void);
void piper_init_exceptions(void);
void piper_init_exceptions_core(void);
void piper_init_builtins(void);
void piper_init_builtins_core(void);
void piper_init_sys(int argc, char **argv);
void piper_init_sys_core(int argc, char **argv);
void piper_init_str(void);
void piper_init_int(void);
void piper_init_math(void);
void piper_init_time(void);
void piper_init_errno(void);
void piper_init_templatelib(void);
PyObject *piper_interpolation_new(PyObject *value, PyObject *expression, int conversion, PyObject *format_spec);
PyObject *piper_template_new(PyObject *const *parts, Py_ssize_t count);
PyObject *piper_type_lookup(PyTypeObject *tp, PyObject *name);
PyObject *piper_type_call(PyObject *tp, PyObject *args, PyObject *kwds);
PyObject *piper_object_new_default(PyTypeObject *tp, PyObject *args, PyObject *kwds);
int piper_object_init_default(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *piper_generic_repr(PyObject *o);
PyObject *piper_vectorcall_to_call(PyObject *f, PyObject *const *args, size_t nargsf, PyObject *kwnames);
PyObject *piper_exception_str(PyObject *e);
PyObject *piper_exc_type_name(PyObject *e);
void piper_print_traceback_lines(PyObject *tb);
PyObject *piper_traceback_new(void);
int piper_write_stdout(const char *s, Py_ssize_t n);
int piper_write_stderr(const char *s, Py_ssize_t n);
PyObject *piper_sys_stdout(void);
PyObject *piper_sys_stderr(void);
int piper_file_write(PyObject *file, PyObject *s);
int piper_file_flush(PyObject *file);
PyObject *piper_open_impl(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames);
void piper_register_module(const char *name, PyObject *m);
PyObject *piper_modules_dict(void);
PyObject *piper_new_stdlib_module(const char *name);
void piper_init_types_module(void);
void piper_init_abc(void);
void piper_init_atexit(void);
void piper_init_weakref(void);
void piper_init_platform(void);
PyObject *piper_module_from_def(PyModuleDef *def, const char *requested_name);
PyObject *piper_property_new(PyObject *fget, PyObject *fset, PyObject *fdel, PyObject *doc);
PyObject *piper_super_new(PyTypeObject *type, PyObject *obj);
PyObject *piper_generator_new(PyObject *func, void *resume, Py_ssize_t slot_count, int kind);
PyObject *piper_generator_function(PyObject *generator);
PyObject **piper_generator_slot(PyObject *generator, Py_ssize_t index);
int piper_generator_state(PyObject *generator);
void piper_generator_set_state(PyObject *generator, int state);
PyObject *piper_generator_finish(PyObject *generator, PyObject *value);
void piper_generator_abort(PyObject *generator);
PyObject *piper_yield_from_next(PyObject *iterator, PyObject *sent, int started);
PyObject *piper_await_iter(PyObject *awaitable);
PyObject *piper_async_iter(PyObject *iterable);
PyObject *piper_async_next(PyObject *iterator);
int piper_async_iteration_done(void);
PyObject *piper_async_with_enter(PyObject *manager, PyObject **exit);
PyObject *piper_async_with_exit(PyObject *exit, PyObject *exc);
PyObject *piper_type_alias_new(PyObject *name, PyObject *thunk, PyObject *type_params);
PyObject *piper_type_param_new(PyObject *name, int kind);
void piper_init_typealias(void);
void piper_set_static_importer(void *importer);
PyObject *piper_static_import_module(void *initializer, const char *fullname);
PyObject *piper_complex_new(double re, double im);
PyObject *piper_bytearray_new(const char *s, Py_ssize_t n);
PyObject *piper_memoryview_new(PyObject *o);

/* small helpers */
#define PIPER_UNUSED(x) (void)(x)
static inline int piper_is_str_kw(PyObject *o) { return PyUnicode_Check(o); }

#endif
