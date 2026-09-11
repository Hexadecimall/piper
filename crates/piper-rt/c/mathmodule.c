/* Core math module backed by the platform C math library. */
#include "internal.h"

static int as_double(PyObject *o, double *v) {
    *v = PyFloat_AsDouble(o);
    return *v == -1.0 && PyErr_Occurred() ? -1 : 0;
}

static PyObject *math_result(double v) {
    if (isnan(v) && errno == EDOM) { errno = 0; PyErr_SetString(PyExc_ValueError, "math domain error"); return NULL; }
    if (isinf(v) && errno == ERANGE) { errno = 0; PyErr_SetString(PyExc_OverflowError, "math range error"); return NULL; }
    errno = 0;
    return PyFloat_FromDouble(v);
}

#define UNARY(name, fn) static PyObject *m_##name(PyObject *m, PyObject *o) { \
    PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; errno = 0; return math_result(fn(x)); }
#define BINARY(name, fn) static PyObject *m_##name(PyObject *m, PyObject *const *a, Py_ssize_t n) { \
    PIPER_UNUSED(m); if (piper_args_range(#name, n, 2, 2) < 0) return NULL; double x, y; \
    if (as_double(a[0], &x) < 0 || as_double(a[1], &y) < 0) return NULL; errno = 0; return math_result(fn(x, y)); }

UNARY(sqrt, sqrt) UNARY(sin, sin) UNARY(cos, cos) UNARY(tan, tan)
UNARY(asin, asin) UNARY(acos, acos) UNARY(atan, atan) UNARY(sinh, sinh)
UNARY(cosh, cosh) UNARY(tanh, tanh) UNARY(asinh, asinh) UNARY(acosh, acosh)
UNARY(atanh, atanh) UNARY(exp, exp) UNARY(expm1, expm1) UNARY(log1p, log1p)
UNARY(log2, log2) UNARY(log10, log10) UNARY(fabs, fabs) UNARY(erf, erf)
UNARY(erfc, erfc) UNARY(tgamma, tgamma) UNARY(lgamma, lgamma)
BINARY(atan2, atan2) BINARY(pow, pow) BINARY(fmod, fmod) BINARY(remainder, remainder)
BINARY(copysign, copysign) BINARY(hypot, hypot)

static PyObject *m_floor(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyLong_FromDouble(floor(x)); }
static PyObject *m_ceil(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyLong_FromDouble(ceil(x)); }
static PyObject *m_trunc(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyLong_FromDouble(trunc(x)); }
static PyObject *m_isfinite(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyBool_FromLong(isfinite(x)); }
static PyObject *m_isinf(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyBool_FromLong(isinf(x)); }
static PyObject *m_isnan(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyBool_FromLong(isnan(x)); }
static PyObject *m_degrees(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyFloat_FromDouble(x * (180.0 / M_PI)); }
static PyObject *m_radians(PyObject *m, PyObject *o) { PIPER_UNUSED(m); double x; if (as_double(o, &x) < 0) return NULL; return PyFloat_FromDouble(x * (M_PI / 180.0)); }

static PyObject *m_log(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m); if (piper_args_range("log", n, 1, 2) < 0) return NULL;
    double x; if (as_double(a[0], &x) < 0) return NULL;
    errno = 0; double r = log(x);
    if (n == 2) { double b; if (as_double(a[1], &b) < 0) return NULL; r /= log(b); }
    return math_result(r);
}

static PyMethodDef math_methods[] = {
    { "sqrt", m_sqrt, METH_O, NULL }, { "sin", m_sin, METH_O, NULL }, { "cos", m_cos, METH_O, NULL }, { "tan", m_tan, METH_O, NULL },
    { "asin", m_asin, METH_O, NULL }, { "acos", m_acos, METH_O, NULL }, { "atan", m_atan, METH_O, NULL },
    { "sinh", m_sinh, METH_O, NULL }, { "cosh", m_cosh, METH_O, NULL }, { "tanh", m_tanh, METH_O, NULL },
    { "asinh", m_asinh, METH_O, NULL }, { "acosh", m_acosh, METH_O, NULL }, { "atanh", m_atanh, METH_O, NULL },
    { "exp", m_exp, METH_O, NULL }, { "expm1", m_expm1, METH_O, NULL }, { "log1p", m_log1p, METH_O, NULL },
    { "log2", m_log2, METH_O, NULL }, { "log10", m_log10, METH_O, NULL }, { "log", (PyCFunction)(void (*)(void))m_log, METH_FASTCALL, NULL },
    { "fabs", m_fabs, METH_O, NULL }, { "floor", m_floor, METH_O, NULL }, { "ceil", m_ceil, METH_O, NULL }, { "trunc", m_trunc, METH_O, NULL },
    { "erf", m_erf, METH_O, NULL }, { "erfc", m_erfc, METH_O, NULL }, { "gamma", m_tgamma, METH_O, NULL }, { "lgamma", m_lgamma, METH_O, NULL },
    { "isfinite", m_isfinite, METH_O, NULL }, { "isinf", m_isinf, METH_O, NULL }, { "isnan", m_isnan, METH_O, NULL },
    { "degrees", m_degrees, METH_O, NULL }, { "radians", m_radians, METH_O, NULL },
    { "atan2", (PyCFunction)(void (*)(void))m_atan2, METH_FASTCALL, NULL }, { "pow", (PyCFunction)(void (*)(void))m_pow, METH_FASTCALL, NULL },
    { "fmod", (PyCFunction)(void (*)(void))m_fmod, METH_FASTCALL, NULL }, { "remainder", (PyCFunction)(void (*)(void))m_remainder, METH_FASTCALL, NULL },
    { "copysign", (PyCFunction)(void (*)(void))m_copysign, METH_FASTCALL, NULL }, { "hypot", (PyCFunction)(void (*)(void))m_hypot, METH_FASTCALL, NULL },
    { NULL, NULL, 0, NULL },
};

void piper_init_math(void) {
    PyObject *m = piper_new_stdlib_module("math");
    if (!m) return;
    PyModule_AddFunctions(m, math_methods);
    PyObject *v = PyFloat_FromDouble(M_PI); PyModule_AddObject(m, "pi", v);
    v = PyFloat_FromDouble(M_E); PyModule_AddObject(m, "e", v);
    v = PyFloat_FromDouble(2.0 * M_PI); PyModule_AddObject(m, "tau", v);
    v = PyFloat_FromDouble(INFINITY); PyModule_AddObject(m, "inf", v);
    v = PyFloat_FromDouble(NAN); PyModule_AddObject(m, "nan", v);
}
