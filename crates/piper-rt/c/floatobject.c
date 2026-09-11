/* float, with Python's repr and arithmetic semantics. */
#include "internal.h"
#include <float.h>

PyObject *PyFloat_FromDouble(double v) {
    PyFloatObject *f = PyObject_Malloc(sizeof(PyFloatObject));
    if (!f) return PyErr_NoMemory();
    PyObject_Init((PyObject *)f, &PyFloat_Type);
    f->ob_fval = v;
    return (PyObject *)f;
}
PyObject *piper_float_const(double v) { return PyFloat_FromDouble(v); }

double PyFloat_AsDouble(PyObject *o) {
    if (PyFloat_Check(o)) return PyFloat_AS_DOUBLE(o);
    PyNumberMethods *m = Py_TYPE(o)->tp_as_number;
    if (m && m->nb_float) {
        PyObject *f = m->nb_float(o);
        if (!f) return -1.0;
        if (!PyFloat_Check(f)) { PyErr_Format(PyExc_TypeError, "%s.__float__ returned non-float (type %s)", Py_TYPE(o)->tp_name, Py_TYPE(f)->tp_name); Py_DECREF(f); return -1.0; }
        double d = PyFloat_AS_DOUBLE(f);
        Py_DECREF(f);
        return d;
    }
    if (m && m->nb_index) { PyObject *i = PyNumber_Index(o); if (!i) return -1.0; double d = PyLong_AsDouble(i); Py_DECREF(i); return d; }
    PyErr_Format(PyExc_TypeError, "must be real number, not %s", Py_TYPE(o)->tp_name);
    return -1.0;
}

/* ---- repr ------------------------------------------------------------- */

/* Shortest round-trip decimal digits: try increasing precision until the
 * value reads back exactly. */
static int shortest_digits(double v, char *digits, int *decpt, int *neg) {
    char buf[64];
    *neg = signbit(v) ? 1 : 0;
    double a = fabs(v);
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        if (strtod(buf, NULL) == a) {
            /* buf like "d.ddddde+XX" */
            int n = 0;
            const char *s = buf;
            for (; *s && *s != 'e'; s++) if (*s != '.') digits[n++] = *s;
            digits[n] = 0;
            int e = atoi(s + 1);
            /* strip trailing zeros */
            while (n > 1 && digits[n - 1] == '0') digits[--n] = 0;
            *decpt = e + 1;
            return n;
        }
    }
    return 0;
}

int piper_float_repr(double v, char *out, size_t cap) {
    if (isnan(v)) { snprintf(out, cap, "nan"); return 0; }
    if (isinf(v)) { snprintf(out, cap, v > 0 ? "inf" : "-inf"); return 0; }
    char digits[32];
    int decpt, neg;
    int n;
    if (v == 0.0) { n = 1; digits[0] = '0'; digits[1] = 0; decpt = 1; neg = signbit(v) ? 1 : 0; }
    else n = shortest_digits(v, digits, &decpt, &neg);
    size_t pos = 0;
    if (neg) out[pos++] = '-';
    int exp10 = decpt - 1;
    if (exp10 < -4 || exp10 >= 16) {
        out[pos++] = digits[0];
        if (n > 1) { out[pos++] = '.'; memcpy(out + pos, digits + 1, (size_t)n - 1); pos += (size_t)n - 1; }
        pos += (size_t)snprintf(out + pos, cap - pos, "e%c%02d", exp10 < 0 ? '-' : '+', abs(exp10));
    } else if (decpt <= 0) {
        out[pos++] = '0'; out[pos++] = '.';
        for (int i = 0; i < -decpt; i++) out[pos++] = '0';
        memcpy(out + pos, digits, (size_t)n); pos += (size_t)n;
    } else if (decpt >= n) {
        memcpy(out + pos, digits, (size_t)n); pos += (size_t)n;
        for (int i = 0; i < decpt - n; i++) out[pos++] = '0';
        out[pos++] = '.'; out[pos++] = '0';
    } else {
        memcpy(out + pos, digits, (size_t)decpt); pos += (size_t)decpt;
        out[pos++] = '.';
        memcpy(out + pos, digits + decpt, (size_t)(n - decpt)); pos += (size_t)(n - decpt);
    }
    out[pos] = 0;
    return 0;
}

static PyObject *float_repr(PyObject *o) {
    char buf[64];
    piper_float_repr(PyFloat_AS_DOUBLE(o), buf, sizeof buf);
    return PyUnicode_FromString(buf);
}

char *PyOS_double_to_string(double v, char fmt, int precision, int flags, int *type) {
    char buf[512];
    if (fmt == 'r') piper_float_repr(v, buf, sizeof buf);
    else {
        char f[8];
        snprintf(f, sizeof f, "%%.%d%c", precision, fmt);
        snprintf(buf, sizeof buf, f, v);
        if ((flags & Py_DTSF_ADD_DOT_0) && !strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n')) strcat(buf, ".0");
    }
    if ((flags & Py_DTSF_SIGN) && buf[0] != '-') { memmove(buf + 1, buf, strlen(buf) + 1); buf[0] = '+'; }
    if (type) *type = isnan(v) ? 2 : isinf(v) ? 1 : 0;
    char *r = PyMem_Malloc(strlen(buf) + 1);
    strcpy(r, buf);
    return r;
}

double PyOS_string_to_double(const char *s, char **endptr, PyObject *overflow_exc) {
    errno = 0;
    char *end;
    double d = strtod(s, &end);
    if (end == s) { PyErr_Format(PyExc_ValueError, "could not convert string to float: '%s'", s); if (endptr) *endptr = (char *)s; return -1.0; }
    if (endptr) *endptr = end;
    else if (*end) { PyErr_Format(PyExc_ValueError, "could not convert string to float: '%s'", s); return -1.0; }
    if (errno == ERANGE && overflow_exc && fabs(d) >= 1.0) { PyErr_Format(overflow_exc, "value too large to convert to float: '%s'", s); return -1.0; }
    return d;
}

PyObject *PyFloat_FromString(PyObject *u) {
    Py_ssize_t n;
    const char *s = PyUnicode_AsUTF8AndSize(u, &n);
    if (!s) return NULL;
    /* strip whitespace, drop underscores between digits */
    char *buf = PyMem_Malloc((size_t)n + 1);
    Py_ssize_t j = 0;
    Py_ssize_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) i++;
    Py_ssize_t end = n;
    while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r' || s[end - 1] == '\f' || s[end - 1] == '\v')) end--;
    int bad = 0;
    for (; i < end; i++) {
        if (s[i] == '_') { if (j == 0 || !((buf[j - 1] >= '0' && buf[j - 1] <= '9')) || i + 1 >= end || !(s[i + 1] >= '0' && s[i + 1] <= '9')) { bad = 1; break; } continue; }
        buf[j++] = s[i];
    }
    buf[j] = 0;
    double d = 0;
    if (!bad && j > 0) {
        /* accept inf/nan spellings case-insensitively like Python */
        char *lower = buf;
        for (char *p = lower; *p; p++) *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        const char *t = lower;
        int sign = 1;
        if (*t == '+' || *t == '-') { sign = *t == '-' ? -1 : 1; t++; }
        if (!strcmp(t, "inf") || !strcmp(t, "infinity")) d = sign * INFINITY;
        else if (!strcmp(t, "nan")) d = NAN;
        else if (t[0] == '0' && (t[1] == 'x' || t[1] == 'b' || t[1] == 'o')) bad = 1;
        else {
            errno = 0;
            char *e;
            d = strtod(lower, &e);
            if (e == lower || *e) bad = 1;
        }
    } else bad = 1;
    PyMem_Free(buf);
    if (bad) { PyObject *r = PyObject_Repr(u); PyErr_Format(PyExc_ValueError, "could not convert string to float: %U", r); Py_XDECREF(r); return NULL; }
    return PyFloat_FromDouble(d);
}

/* ---- arithmetic ---------------------------------------------------------- */

static int convert_to_double(PyObject *o, double *d) {
    if (PyFloat_Check(o)) { *d = PyFloat_AS_DOUBLE(o); return 0; }
    if (PyLong_Check(o)) { *d = PyLong_AsDouble(o); return (*d == -1.0 && PyErr_Occurred()) ? -1 : 0; }
    return 1; /* NotImplemented */
}

#define CONVERT(v, w, a, b) \
    { int r1 = convert_to_double(v, &a); if (r1 < 0) return NULL; if (r1 > 0) Py_RETURN_NOTIMPLEMENTED; \
      int r2 = convert_to_double(w, &b); if (r2 < 0) return NULL; if (r2 > 0) Py_RETURN_NOTIMPLEMENTED; }

static PyObject *float_add(PyObject *v, PyObject *w) { double a, b; CONVERT(v, w, a, b); return PyFloat_FromDouble(a + b); }
static PyObject *float_sub(PyObject *v, PyObject *w) { double a, b; CONVERT(v, w, a, b); return PyFloat_FromDouble(a - b); }
static PyObject *float_mul(PyObject *v, PyObject *w) { double a, b; CONVERT(v, w, a, b); return PyFloat_FromDouble(a * b); }
static PyObject *float_div(PyObject *v, PyObject *w) {
    double a, b; CONVERT(v, w, a, b);
    if (b == 0.0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return NULL; }
    return PyFloat_FromDouble(a / b);
}

static void float_divmod_impl(double vx, double wx, double *floordiv, double *mod) {
    double m = fmod(vx, wx);
    double div = (vx - m) / wx;
    if (m) { if ((wx < 0) != (m < 0)) { m += wx; div -= 1.0; } }
    else m = copysign(0.0, wx);
    if (div) { *floordiv = floor(div); if (div - *floordiv > 0.5) *floordiv += 1.0; }
    else *floordiv = copysign(0.0, vx / wx);
    *mod = m;
}

static PyObject *float_mod(PyObject *v, PyObject *w) {
    double a, b; CONVERT(v, w, a, b);
    if (b == 0.0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return NULL; }
    double fd, m;
    float_divmod_impl(a, b, &fd, &m);
    return PyFloat_FromDouble(m);
}
static PyObject *float_floordiv(PyObject *v, PyObject *w) {
    double a, b; CONVERT(v, w, a, b);
    if (b == 0.0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return NULL; }
    double fd, m;
    float_divmod_impl(a, b, &fd, &m);
    return PyFloat_FromDouble(fd);
}
static PyObject *float_divmod(PyObject *v, PyObject *w) {
    double a, b; CONVERT(v, w, a, b);
    if (b == 0.0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return NULL; }
    double fd, m;
    float_divmod_impl(a, b, &fd, &m);
    PyObject *x = PyFloat_FromDouble(fd), *y = PyFloat_FromDouble(m);
    PyObject *t = PyTuple_Pack(2, x, y);
    Py_DECREF(x); Py_DECREF(y);
    return t;
}

static PyObject *float_pow(PyObject *v, PyObject *w, PyObject *z) {
    if (z != Py_None) { PyErr_SetString(PyExc_TypeError, "pow() 3rd argument not allowed unless all arguments are integers"); return NULL; }
    double a, b; CONVERT(v, w, a, b);
    if (b == 0) return PyFloat_FromDouble(1.0);
    if (isnan(a)) return PyFloat_FromDouble(b == 0 ? 1.0 : a);
    if (isnan(b)) return PyFloat_FromDouble(a == 1.0 ? 1.0 : b);
    if (a == 0.0 && b < 0.0) { PyErr_SetString(PyExc_ZeroDivisionError, "zero to a negative power"); return NULL; }
    if (a < 0.0 && b != floor(b)) {
        extern PyObject *piper_complex_pow(double ar, double ai, double br, double bi);
        return piper_complex_pow(a, 0.0, b, 0.0);
    }
    errno = 0;
    double r = pow(a, b);
    if (isinf(r) && !isinf(a) && !isinf(b)) { PyErr_SetString(PyExc_OverflowError, "(34, 'Numerical result out of range')"); return NULL; }
    return PyFloat_FromDouble(r);
}

static PyObject *float_neg(PyObject *v) { return PyFloat_FromDouble(-PyFloat_AS_DOUBLE(v)); }
static PyObject *float_pos(PyObject *v) { return PyFloat_CheckExact(v) ? Py_NewRef(v) : PyFloat_FromDouble(PyFloat_AS_DOUBLE(v)); }
static PyObject *float_abs(PyObject *v) { return PyFloat_FromDouble(fabs(PyFloat_AS_DOUBLE(v))); }
static int float_bool(PyObject *v) { return PyFloat_AS_DOUBLE(v) != 0.0; }
static PyObject *float_int(PyObject *v) { return PyLong_FromDouble(PyFloat_AS_DOUBLE(v)); }
static PyObject *float_float(PyObject *v) { return float_pos(v); }

static PyObject *float_richcompare(PyObject *v, PyObject *w, int op) {
    double i = PyFloat_AS_DOUBLE(v), j;
    if (PyFloat_Check(w)) j = PyFloat_AS_DOUBLE(w);
    else if (PyLong_Check(w)) {
        if (isinf(i) || isnan(i)) j = 0.0;
        else {
            /* exact comparison against big ints */
            int overflow;
            j = piper_int_to_double(w, &overflow);
            if (overflow) {
                int sign = _PyLong_Sign(w);
                j = sign > 0 ? INFINITY : -INFINITY;
                /* finite float vs huge int */
            } else if (fabs(i) >= 9007199254740992.0 && !overflow) {
                /* compare as ints when both are integral and large */
                if (i == floor(i)) {
                    PyObject *iv = PyLong_FromDouble(i);
                    if (!iv) return NULL;
                    PyObject *r = PyObject_RichCompare(iv, w, op);
                    Py_DECREF(iv);
                    return r;
                }
            }
        }
        if (isinf(i) || isnan(i)) { /* fall through with j = 0 */ }
    } else Py_RETURN_NOTIMPLEMENTED;
    int r;
    switch (op) { case Py_EQ: r = i == j; break; case Py_NE: r = i != j; break; case Py_LE: r = i <= j; break; case Py_GE: r = i >= j; break; case Py_LT: r = i < j; break; default: r = i > j; }
    Py_RETURN_BOOL(r);
}

static Py_hash_t float_hash(PyObject *v) { return _Py_HashDouble(v, PyFloat_AS_DOUBLE(v)); }

static PyObject *float_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "float() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "float expected at most 1 argument, got %zd", n); return NULL; }
    PyObject *r = n == 0 ? PyFloat_FromDouble(0.0) : PyNumber_Float(PyTuple_GET_ITEM(args, 0));
    if (!r || tp == &PyFloat_Type) return r;
    PyObject *sub = tp->tp_alloc(tp, 0);
    if (sub) ((PyFloatObject *)sub)->ob_fval = PyFloat_AS_DOUBLE(r);
    Py_DECREF(r);
    return sub;
}

static PyObject *float_is_integer(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); double d = PyFloat_AS_DOUBLE(self); Py_RETURN_BOOL(isfinite(d) && d == floor(d)); }
static PyObject *float_conjugate(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return float_pos(self); }
static PyObject *float_trunc(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return PyLong_FromDouble(PyFloat_AS_DOUBLE(self)); }
static PyObject *float_floor(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return PyLong_FromDouble(floor(PyFloat_AS_DOUBLE(self))); }
static PyObject *float_ceil(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return PyLong_FromDouble(ceil(PyFloat_AS_DOUBLE(self))); }
static PyObject *float_round(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    double x = PyFloat_AS_DOUBLE(self);
    if (nargs == 0 || args[0] == Py_None) {
        double r = round(x);
        if (fabs(x - r) == 0.5) r = 2.0 * round(x / 2.0);
        return PyLong_FromDouble(r);
    }
    Py_ssize_t nd = PyNumber_AsSsize_t(args[0], NULL);
    if (nd == -1 && PyErr_Occurred()) return NULL;
    if (!isfinite(x)) return PyFloat_FromDouble(x);
    if (nd > 22) return PyFloat_FromDouble(x);
    if (nd < -308) return PyFloat_FromDouble(0.0 * x);
    /* Correctly rounded via string conversion. */
    char buf[512];
    if (nd >= 0) {
        snprintf(buf, sizeof buf, "%.*f", (int)nd, x);
        return PyFloat_FromDouble(strtod(buf, NULL));
    }
    double p = pow(10.0, (double)-nd);
    double y = x / p;
    double r = round(y);
    if (fabs(y - r) == 0.5) r = 2.0 * round(y / 2.0);
    return PyFloat_FromDouble(r * p);
}
static PyObject *float_as_integer_ratio(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    double x = PyFloat_AS_DOUBLE(self);
    if (isinf(x)) { PyErr_SetString(PyExc_OverflowError, "cannot convert Infinity to integer ratio"); return NULL; }
    if (isnan(x)) { PyErr_SetString(PyExc_ValueError, "cannot convert NaN to integer ratio"); return NULL; }
    int e;
    double m = frexp(x, &e);
    for (int i = 0; i < 300 && m != floor(m); i++) { m *= 2.0; e--; }
    PyObject *num = PyLong_FromDouble(m), *den = PyLong_FromLong(1);
    PyObject *sh = PyLong_FromLong(abs(e));
    if (e > 0) { PyObject *t = PyNumber_Lshift(num, sh); Py_DECREF(num); num = t; }
    else { PyObject *t = PyNumber_Lshift(den, sh); Py_DECREF(den); den = t; }
    Py_DECREF(sh);
    PyObject *r = PyTuple_Pack(2, num, den);
    Py_DECREF(num); Py_DECREF(den);
    return r;
}
static PyObject *float_hex(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    double x = PyFloat_AS_DOUBLE(self);
    if (isnan(x) || isinf(x)) return float_repr(self);
    if (x == 0.0) return PyUnicode_FromString(signbit(x) ? "-0x0.0p+0" : "0x0.0p+0");
    int e;
    double m = frexp(fabs(x), &e);
    m *= 2.0; e--;
    char buf[64];
    int pos = snprintf(buf, sizeof buf, "%s0x%d.", signbit(x) ? "-" : "", (int)m);
    m -= (int)m;
    for (int i = 0; i < 13; i++) { m *= 16.0; int d = (int)m; buf[pos++] = "0123456789abcdef"[d]; m -= d; }
    snprintf(buf + pos, sizeof buf - (size_t)pos, "p%+d", e);
    return PyUnicode_FromString(buf);
}
static PyObject *float_fromhex(PyObject *cls, PyObject *arg) {
    if (!PyUnicode_Check(arg)) { PyErr_SetString(PyExc_TypeError, "must be str"); return NULL; }
    const char *s = PyUnicode_AsUTF8(arg);
    while (*s == ' ') s++;
    char *end;
    double d = strtod(s, &end);
    while (*end == ' ') end++;
    if (*end) { PyErr_SetString(PyExc_ValueError, "invalid hexadecimal floating-point string"); return NULL; }
    PyObject *r = PyFloat_FromDouble(d);
    if ((PyTypeObject *)cls != &PyFloat_Type) { PyObject *t = PyObject_CallOneArg(cls, r); Py_DECREF(r); r = t; }
    return r;
}
static PyObject *float_getnewargs(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); PyObject *f = float_pos(self); PyObject *t = PyTuple_Pack(1, f); Py_DECREF(f); return t; }
static PyObject *float_format(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (piper_args_range("__format__", nargs, 1, 1) < 0) return NULL;
    if (!PyUnicode_Check(args[0])) { PyErr_SetString(PyExc_TypeError, "__format__() argument must be str"); return NULL; }
    return piper_format_spec_apply(self, args[0]);
}
static PyObject *float_getformat(PyObject *cls, PyObject *arg) { PIPER_UNUSED(cls); PIPER_UNUSED(arg); return PyUnicode_FromString("IEEE, little-endian"); }
static PyObject *float_real(PyObject *self, void *c) { PIPER_UNUSED(c); return float_pos(self); }
static PyObject *float_imag(PyObject *self, void *c) { PIPER_UNUSED(self); PIPER_UNUSED(c); return PyFloat_FromDouble(0.0); }

static PyMethodDef float_methods[] = {
    { "is_integer", float_is_integer, METH_NOARGS, NULL },
    { "conjugate", float_conjugate, METH_NOARGS, NULL },
    { "__trunc__", float_trunc, METH_NOARGS, NULL },
    { "__floor__", float_floor, METH_NOARGS, NULL },
    { "__ceil__", float_ceil, METH_NOARGS, NULL },
    { "__round__", (PyCFunction)(void (*)(void))float_round, METH_FASTCALL, NULL },
    { "as_integer_ratio", float_as_integer_ratio, METH_NOARGS, NULL },
    { "hex", float_hex, METH_NOARGS, NULL },
    { "fromhex", float_fromhex, METH_O | METH_CLASS, NULL },
    { "__getnewargs__", float_getnewargs, METH_NOARGS, NULL },
    { "__format__", (PyCFunction)(void (*)(void))float_format, METH_FASTCALL, NULL },
    { "__getformat__", float_getformat, METH_O | METH_CLASS, NULL },
    { NULL, NULL, 0, NULL },
};
static PyGetSetDef float_getsets[] = {
    { "real", float_real, NULL, NULL, NULL },
    { "imag", float_imag, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};

static PyNumberMethods float_as_number = {
    .nb_add = float_add, .nb_subtract = float_sub, .nb_multiply = float_mul, .nb_remainder = float_mod, .nb_divmod = float_divmod,
    .nb_power = float_pow, .nb_negative = float_neg, .nb_positive = float_pos, .nb_absolute = float_abs, .nb_bool = float_bool,
    .nb_int = float_int, .nb_float = float_float, .nb_floor_divide = float_floordiv, .nb_true_divide = float_div,
};

static void float_dealloc(PyObject *o) { Py_TYPE(o)->tp_free(o); }

PyTypeObject PyFloat_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "float",
    .tp_basicsize = sizeof(PyFloatObject),
    .tp_dealloc = float_dealloc,
    .tp_repr = float_repr,
    .tp_as_number = &float_as_number,
    .tp_hash = float_hash,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_MATCH_SELF,
    .tp_richcompare = float_richcompare,
    .tp_methods = float_methods,
    .tp_getset = float_getsets,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = float_new,
    .tp_free = PyObject_Free,
};

double PyFloat_GetMax(void) { return DBL_MAX; }
double PyFloat_GetMin(void) { return DBL_MIN; }
PyObject *PyFloat_GetInfo(void) {
    PyObject *d = PyDict_New();
    PyObject *v;
    v = PyFloat_FromDouble(DBL_MAX); PyDict_SetItemString(d, "max", v); Py_DECREF(v);
    v = PyLong_FromLong(DBL_MAX_EXP); PyDict_SetItemString(d, "max_exp", v); Py_DECREF(v);
    v = PyLong_FromLong(DBL_MAX_10_EXP); PyDict_SetItemString(d, "max_10_exp", v); Py_DECREF(v);
    v = PyFloat_FromDouble(DBL_MIN); PyDict_SetItemString(d, "min", v); Py_DECREF(v);
    v = PyLong_FromLong(DBL_MIN_EXP); PyDict_SetItemString(d, "min_exp", v); Py_DECREF(v);
    v = PyLong_FromLong(DBL_MIN_10_EXP); PyDict_SetItemString(d, "min_10_exp", v); Py_DECREF(v);
    v = PyLong_FromLong(DBL_DIG); PyDict_SetItemString(d, "dig", v); Py_DECREF(v);
    v = PyLong_FromLong(DBL_MANT_DIG); PyDict_SetItemString(d, "mant_dig", v); Py_DECREF(v);
    v = PyFloat_FromDouble(DBL_EPSILON); PyDict_SetItemString(d, "epsilon", v); Py_DECREF(v);
    v = PyLong_FromLong(FLT_RADIX); PyDict_SetItemString(d, "radix", v); Py_DECREF(v);
    v = PyLong_FromLong(FLT_ROUNDS); PyDict_SetItemString(d, "rounds", v); Py_DECREF(v);
    return d;
}

/* ---- complex --------------------------------------------------------------- */

typedef struct { PyObject_HEAD double real, imag; } PyComplexObject;

PyObject *piper_complex_new(double re, double im) {
    PyComplexObject *c = PyObject_Malloc(sizeof(PyComplexObject));
    if (!c) return PyErr_NoMemory();
    PyObject_Init((PyObject *)c, &PyComplex_Type);
    c->real = re; c->imag = im;
    return (PyObject *)c;
}
PyObject *piper_complex_const(double re, double im) { return piper_complex_new(re, im); }

static int to_complex(PyObject *o, double *re, double *im) {
    if (Py_IS_TYPE(o, &PyComplex_Type) || PyObject_TypeCheck(o, &PyComplex_Type)) { *re = ((PyComplexObject *)o)->real; *im = ((PyComplexObject *)o)->imag; return 0; }
    if (PyFloat_Check(o)) { *re = PyFloat_AS_DOUBLE(o); *im = 0; return 0; }
    if (PyLong_Check(o)) { *re = PyLong_AsDouble(o); *im = 0; return (*re == -1.0 && PyErr_Occurred()) ? -1 : 0; }
    return 1;
}
#define CCONVERT(v, w) double ar, ai, br, bi; { int r1 = to_complex(v, &ar, &ai); if (r1 < 0) return NULL; if (r1) Py_RETURN_NOTIMPLEMENTED; int r2 = to_complex(w, &br, &bi); if (r2 < 0) return NULL; if (r2) Py_RETURN_NOTIMPLEMENTED; }

static PyObject *complex_add(PyObject *v, PyObject *w) { CCONVERT(v, w); return piper_complex_new(ar + br, ai + bi); }
static PyObject *complex_sub(PyObject *v, PyObject *w) { CCONVERT(v, w); return piper_complex_new(ar - br, ai - bi); }
static PyObject *complex_mul(PyObject *v, PyObject *w) { CCONVERT(v, w); return piper_complex_new(ar * br - ai * bi, ar * bi + ai * br); }
static PyObject *complex_div(PyObject *v, PyObject *w) {
    CCONVERT(v, w);
    if (br == 0 && bi == 0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return NULL; }
    double d = br * br + bi * bi;
    return piper_complex_new((ar * br + ai * bi) / d, (ai * br - ar * bi) / d);
}
PyObject *piper_complex_pow(double ar, double ai, double br, double bi) {
    if (br == 0 && bi == 0) return piper_complex_new(1.0, 0.0);
    if (ar == 0 && ai == 0) { if (bi != 0 || br < 0) { PyErr_SetString(PyExc_ZeroDivisionError, "zero to a negative or complex power"); return NULL; } return piper_complex_new(0, 0); }
    double vabs = hypot(ar, ai), len = pow(vabs, br), at = atan2(ai, ar), phase = at * br;
    if (bi != 0) { len /= exp(at * bi); phase += bi * log(vabs); }
    return piper_complex_new(len * cos(phase), len * sin(phase));
}
static PyObject *complex_pow(PyObject *v, PyObject *w, PyObject *z) {
    if (z != Py_None) { PyErr_SetString(PyExc_ValueError, "complex modulo"); return NULL; }
    CCONVERT(v, w);
    return piper_complex_pow(ar, ai, br, bi);
}
static PyObject *complex_neg(PyObject *v) { return piper_complex_new(-((PyComplexObject *)v)->real, -((PyComplexObject *)v)->imag); }
static PyObject *complex_pos(PyObject *v) { return piper_complex_new(((PyComplexObject *)v)->real, ((PyComplexObject *)v)->imag); }
static PyObject *complex_abs(PyObject *v) { return PyFloat_FromDouble(hypot(((PyComplexObject *)v)->real, ((PyComplexObject *)v)->imag)); }
static int complex_bool(PyObject *v) { return ((PyComplexObject *)v)->real != 0 || ((PyComplexObject *)v)->imag != 0; }
static PyObject *complex_repr(PyObject *v) {
    PyComplexObject *c = (PyComplexObject *)v;
    char re[64], im[64];
    if (c->real == 0.0 && copysign(1.0, c->real) == 1.0) {
        piper_float_repr(c->imag, im, sizeof im);
        size_t n = strlen(im); if (n > 2 && !strcmp(im + n - 2, ".0")) im[n - 2] = 0;
        return PyUnicode_FromFormat("%sj", im);
    }
    piper_float_repr(c->real, re, sizeof re);
    piper_float_repr(c->imag, im, sizeof im);
    size_t n = strlen(re); if (n > 2 && !strcmp(re + n - 2, ".0")) re[n - 2] = 0;
    n = strlen(im); if (n > 2 && !strcmp(im + n - 2, ".0")) im[n - 2] = 0;
    return PyUnicode_FromFormat("(%s%s%sj)", re, (im[0] == '-' || im[0] == 'n' || im[0] == 'i') && im[0] == '-' ? "" : "+", im);
}
static PyObject *complex_richcompare(PyObject *v, PyObject *w, int op) {
    if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
    double br, bi;
    int r = to_complex(w, &br, &bi);
    if (r < 0) return NULL;
    if (r) Py_RETURN_NOTIMPLEMENTED;
    PyComplexObject *c = (PyComplexObject *)v;
    int eq = c->real == br && c->imag == bi;
    Py_RETURN_BOOL(op == Py_EQ ? eq : !eq);
}
static Py_hash_t complex_hash(PyObject *v) {
    PyComplexObject *c = (PyComplexObject *)v;
    Py_uhash_t h = (Py_uhash_t)_Py_HashDouble(v, c->real) + _PyHASH_IMAG * (Py_uhash_t)_Py_HashDouble(v, c->imag);
    Py_hash_t r = (Py_hash_t)h;
    return r == -1 ? -2 : r;
}
static PyObject *complex_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(kwds);
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    double re = 0, im = 0;
    if (n >= 1) {
        PyObject *a = PyTuple_GET_ITEM(args, 0);
        if (PyUnicode_Check(a)) {
            if (n > 1) { PyErr_SetString(PyExc_TypeError, "complex() can't take second arg if first is a string"); return NULL; }
            const char *s = PyUnicode_AsUTF8(a);
            while (*s == ' ') s++;
            char buf[128];
            strncpy(buf, s, sizeof buf - 1); buf[sizeof buf - 1] = 0;
            size_t len = strlen(buf);
            while (len && buf[len - 1] == ' ') buf[--len] = 0;
            char *p = buf;
            if (*p == '(' && buf[len - 1] == ')') { p++; buf[len - 1] = 0; }
            char *end;
            double x = strtod(p, &end);
            if (end == p) { if (*p == 'j' || ((*p == '+' || *p == '-') && p[1] == 'j')) { x = 0; } else goto bad; }
            if (*end == 'j' && end[1] == 0) { re = 0; im = (end == p) ? (*p == '-' ? -1 : 1) : x; }
            else if (*end == 0) re = x;
            else if (*end == '+' || *end == '-') {
                char *e2;
                double y = strtod(end, &e2);
                if (e2 == end + 1 && *e2 == 'j') { y = *end == '-' ? -1 : 1; e2++; }
                else if (*e2 != 'j') goto bad;
                if (e2[0] == 'j' && e2[1] == 0) { re = x; im = y; } else if (e2[0] == 0 && e2[-1] == 'j') { re = x; im = y; } else goto bad;
            } else goto bad;
        } else {
            double r1, i1;
            int r = to_complex(a, &r1, &i1);
            if (r < 0) return NULL;
            if (r) { PyObject *f = PyNumber_Float(a); if (!f) return NULL; r1 = PyFloat_AS_DOUBLE(f); i1 = 0; Py_DECREF(f); }
            re = r1; im = i1;
        }
    }
    if (n >= 2) {
        double r2, i2;
        int r = to_complex(PyTuple_GET_ITEM(args, 1), &r2, &i2);
        if (r < 0) return NULL;
        if (r) { PyErr_SetString(PyExc_TypeError, "complex() second argument must be a number"); return NULL; }
        re -= i2; im += r2;
    }
    PyObject *c = piper_complex_new(re, im);
    if (tp != &PyComplex_Type && c) { PyObject *sub = tp->tp_alloc(tp, 0); ((PyComplexObject *)sub)->real = re; ((PyComplexObject *)sub)->imag = im; Py_DECREF(c); return sub; }
    return c;
bad:
    PyErr_SetString(PyExc_ValueError, "complex() arg is a malformed string");
    return NULL;
}
static PyObject *complex_real(PyObject *self, void *c) { PIPER_UNUSED(c); return PyFloat_FromDouble(((PyComplexObject *)self)->real); }
static PyObject *complex_imag(PyObject *self, void *c) { PIPER_UNUSED(c); return PyFloat_FromDouble(((PyComplexObject *)self)->imag); }
static PyObject *complex_conjugate(PyObject *self, PyObject *u) { PIPER_UNUSED(u); return piper_complex_new(((PyComplexObject *)self)->real, -((PyComplexObject *)self)->imag); }
static PyGetSetDef complex_getsets[] = { { "real", complex_real, NULL, NULL, NULL }, { "imag", complex_imag, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyMethodDef complex_methods[] = { { "conjugate", complex_conjugate, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyNumberMethods complex_as_number = {
    .nb_add = complex_add, .nb_subtract = complex_sub, .nb_multiply = complex_mul, .nb_power = complex_pow, .nb_negative = complex_neg,
    .nb_positive = complex_pos, .nb_absolute = complex_abs, .nb_bool = complex_bool, .nb_true_divide = complex_div,
};
PyTypeObject PyComplex_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "complex", .tp_basicsize = sizeof(PyComplexObject), .tp_dealloc = float_dealloc, .tp_repr = complex_repr,
    .tp_as_number = &complex_as_number, .tp_hash = complex_hash, .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE, .tp_richcompare = complex_richcompare, .tp_methods = complex_methods, .tp_getset = complex_getsets,
    .tp_alloc = PyType_GenericAlloc, .tp_new = complex_new, .tp_free = PyObject_Free,
};
