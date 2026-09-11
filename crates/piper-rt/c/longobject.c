/* Arbitrary precision integers with CPython 3.14's tagged layout, plus bool. */
#include "internal.h"

/* ---- allocation and normalization ------------------------------------- */

PyLongObject piper_small_ints[PIPER_NSMALLNEGINTS + PIPER_NSMALLPOSINTS];

static inline void set_tag(PyLongObject *v, int sign, Py_ssize_t ndigits) {
    /* sign: +1 -> 0, 0 -> 1, -1 -> 2 */
    uintptr_t s = sign > 0 ? 0 : sign == 0 ? 1 : 2;
    v->long_value.lv_tag = ((uintptr_t)ndigits << _PyLong_NON_SIZE_BITS) | s;
}
void piper_long_set_sign_and_size(PyLongObject *v, int sign, Py_ssize_t ndigits) { set_tag(v, sign, ndigits); }

PyLongObject *piper_long_alloc(Py_ssize_t ndigits) {
    Py_ssize_t n = ndigits ? ndigits : 1;
    PyLongObject *v = PyObject_Malloc(offsetof(PyLongObject, long_value.ob_digit) + (size_t)n * sizeof(digit));
    if (!v) { PyErr_NoMemory(); return NULL; }
    PyObject_Init((PyObject *)v, &PyLong_Type);
    set_tag(v, 1, ndigits);
    v->long_value.ob_digit[0] = 0;
    return v;
}

static PyObject *small_int(sdigit v) { return (PyObject *)&piper_small_ints[v + PIPER_NSMALLNEGINTS]; }
static inline int is_small(stwodigits v) { return v >= -PIPER_NSMALLNEGINTS && v < PIPER_NSMALLPOSINTS - PIPER_NSMALLNEGINTS + 0 && v < 257; }

/* Strip leading zero digits and fix the sign tag; may return a small int. */
PyObject *piper_long_normalize(PyLongObject *v) {
    Py_ssize_t n = piper_long_ndigits(v);
    int sign = piper_long_is_neg(v) ? -1 : 1;
    while (n > 0 && v->long_value.ob_digit[n - 1] == 0) n--;
    if (n == 0) { set_tag(v, 0, 0); v->long_value.ob_digit[0] = 0; }
    else set_tag(v, sign, n);
    if (n <= 1) {
        stwodigits val = n == 0 ? 0 : (stwodigits)sign * (stwodigits)v->long_value.ob_digit[0];
        if (val >= -PIPER_NSMALLNEGINTS && val < PIPER_NSMALLPOSINTS - PIPER_NSMALLNEGINTS) {
            Py_DECREF(v);
            return small_int((sdigit)val);
        }
    }
    return (PyObject *)v;
}

/* ---- construction ------------------------------------------------------- */

static PyObject *long_from_u64(uint64_t u, int sign) {
    if (sign == 0 || u == 0) return small_int(0);
    if (u < PIPER_NSMALLPOSINTS - PIPER_NSMALLNEGINTS && sign > 0) return small_int((sdigit)u);
    if (sign < 0 && u <= PIPER_NSMALLNEGINTS) return small_int(-(sdigit)u);
    Py_ssize_t n = 0;
    uint64_t t = u;
    while (t) { n++; t >>= PyLong_SHIFT; }
    PyLongObject *v = piper_long_alloc(n);
    if (!v) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) { v->long_value.ob_digit[i] = (digit)(u & PyLong_MASK); u >>= PyLong_SHIFT; }
    set_tag(v, sign, n);
    return (PyObject *)v;
}

PyObject *PyLong_FromLongLong(long long v) {
    if (v >= 0) return long_from_u64((uint64_t)v, 1);
    return long_from_u64((uint64_t)0 - (uint64_t)v, -1);
}
PyObject *PyLong_FromLong(long v) { return PyLong_FromLongLong(v); }
PyObject *PyLong_FromSsize_t(Py_ssize_t v) { return PyLong_FromLongLong(v); }
PyObject *PyLong_FromUnsignedLongLong(unsigned long long v) { return long_from_u64(v, 1); }
PyObject *PyLong_FromUnsignedLong(unsigned long v) { return long_from_u64(v, 1); }
PyObject *PyLong_FromSize_t(size_t v) { return long_from_u64(v, 1); }
PyObject *PyLong_FromBool(int v) { return small_int(v ? 1 : 0); }
PyObject *PyLong_FromVoidPtr(void *p) { return long_from_u64((uint64_t)(uintptr_t)p, 1); }
PyObject *piper_int_from_i64(int64_t v) { return PyLong_FromLongLong(v); }

PyObject *PyLong_FromDouble(double d) {
    if (isnan(d)) { PyErr_SetString(PyExc_ValueError, "cannot convert float NaN to integer"); return NULL; }
    if (isinf(d)) { PyErr_SetString(PyExc_OverflowError, "cannot convert float infinity to integer"); return NULL; }
    if (d > -9.2e18 && d < 9.2e18) return PyLong_FromLongLong((long long)d);
    int sign = 1;
    if (d < 0) { sign = -1; d = -d; }
    int e;
    double frac = frexp(d, &e);
    Py_ssize_t n = (e - 1) / PyLong_SHIFT + 1;
    PyLongObject *v = piper_long_alloc(n);
    if (!v) return NULL;
    frac = ldexp(frac, (e - 1) % PyLong_SHIFT + 1);
    for (Py_ssize_t i = n; --i >= 0;) {
        digit bits = (digit)frac;
        v->long_value.ob_digit[i] = bits;
        frac -= (double)bits;
        frac = ldexp(frac, PyLong_SHIFT);
    }
    set_tag(v, sign, n);
    return piper_long_normalize(v);
}

/* ---- conversion out ------------------------------------------------------ */

/* Magnitude as u64; returns -1 on overflow (no exception). */
static int long_as_u64_mag(PyLongObject *v, uint64_t *out) {
    Py_ssize_t n = piper_long_ndigits(v);
    uint64_t r = 0;
    for (Py_ssize_t i = n; --i >= 0;) {
        if (r >> (64 - PyLong_SHIFT)) return -1;
        r = (r << PyLong_SHIFT) | v->long_value.ob_digit[i];
    }
    *out = r;
    return 0;
}

long long PyLong_AsLongLongAndOverflow(PyObject *o, int *overflow) {
    *overflow = 0;
    PyObject *v = o;
    if (!PyLong_Check(o)) {
        if (!PyIndex_Check(o)) { PyErr_Format(PyExc_TypeError, "'%s' object cannot be interpreted as an integer", Py_TYPE(o)->tp_name); return -1; }
        v = PyNumber_Index(o);
        if (!v) return -1;
    } else Py_INCREF(v);
    PyLongObject *l = (PyLongObject *)v;
    long long res;
    if (piper_long_is_compact(l)) res = (long long)piper_long_compact_value(l);
    else {
        uint64_t mag;
        int neg = piper_long_is_neg(l);
        if (long_as_u64_mag(l, &mag) < 0 || (!neg && mag > (uint64_t)LLONG_MAX) || (neg && mag > (uint64_t)LLONG_MAX + 1)) {
            *overflow = neg ? -1 : 1;
            Py_DECREF(v);
            return -1;
        }
        res = neg ? (long long)(0 - mag) : (long long)mag;
    }
    Py_DECREF(v);
    return res;
}

long long PyLong_AsLongLong(PyObject *o) {
    int overflow;
    long long r = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow) { PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C long"); return -1; }
    return r;
}
long PyLong_AsLongAndOverflow(PyObject *o, int *overflow) { return (long)PyLong_AsLongLongAndOverflow(o, overflow); }
long PyLong_AsLong(PyObject *o) { return (long)PyLong_AsLongLong(o); }
Py_ssize_t PyLong_AsSsize_t(PyObject *o) {
    if (!PyLong_Check(o)) { PyErr_SetString(PyExc_TypeError, "an integer is required"); return -1; }
    int overflow;
    long long r = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow) { PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C ssize_t"); return -1; }
    return (Py_ssize_t)r;
}
int PyLong_AsInt(PyObject *o) {
    long long r = PyLong_AsLongLong(o);
    if (r == -1 && PyErr_Occurred()) return -1;
    if (r > INT_MAX || r < INT_MIN) { PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C int"); return -1; }
    return (int)r;
}
int piper_int_fits_i64(PyObject *o, int64_t *out) {
    int overflow;
    long long r = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow || (r == -1 && PyErr_Occurred())) return 0;
    *out = r;
    return 1;
}

unsigned long long PyLong_AsUnsignedLongLong(PyObject *o) {
    if (!PyLong_Check(o)) { PyErr_SetString(PyExc_TypeError, "an integer is required"); return (unsigned long long)-1; }
    PyLongObject *l = (PyLongObject *)o;
    if (piper_long_is_neg(l)) { PyErr_SetString(PyExc_OverflowError, "can't convert negative value to unsigned int"); return (unsigned long long)-1; }
    uint64_t mag;
    if (long_as_u64_mag(l, &mag) < 0) { PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C unsigned long"); return (unsigned long long)-1; }
    return mag;
}
unsigned long PyLong_AsUnsignedLong(PyObject *o) { return (unsigned long)PyLong_AsUnsignedLongLong(o); }
size_t PyLong_AsSize_t(PyObject *o) { return (size_t)PyLong_AsUnsignedLongLong(o); }

unsigned long long PyLong_AsUnsignedLongLongMask(PyObject *o) {
    PyObject *v = PyNumber_Index(o);
    if (!v) return (unsigned long long)-1;
    PyLongObject *l = (PyLongObject *)v;
    uint64_t r = 0;
    Py_ssize_t n = piper_long_ndigits(l);
    for (Py_ssize_t i = n; --i >= 0;) r = (r << PyLong_SHIFT) | l->long_value.ob_digit[i];
    if (piper_long_is_neg(l)) r = 0 - r;
    Py_DECREF(v);
    return r;
}
unsigned long PyLong_AsUnsignedLongMask(PyObject *o) { return (unsigned long)PyLong_AsUnsignedLongLongMask(o); }
void *PyLong_AsVoidPtr(PyObject *o) { return (void *)(uintptr_t)PyLong_AsUnsignedLongLongMask(o); }

double piper_int_to_double(PyObject *o, int *overflow) {
    PyLongObject *l = (PyLongObject *)o;
    *overflow = 0;
    if (piper_long_is_compact(l)) return (double)piper_long_compact_value(l);
    Py_ssize_t n = piper_long_ndigits(l);
    /* Use the top 3 digits with correct rounding via long double when available. */
    double r = 0.0;
    for (Py_ssize_t i = n; --i >= 0;) r = r * (double)PyLong_BASE + (double)l->long_value.ob_digit[i];
    if (isinf(r)) { *overflow = 1; return -1.0; }
    return piper_long_is_neg(l) ? -r : r;
}

double PyLong_AsDouble(PyObject *o) {
    if (!PyLong_Check(o)) { PyErr_SetString(PyExc_TypeError, "an integer is required"); return -1.0; }
    int overflow;
    double d = piper_int_to_double(o, &overflow);
    if (overflow) { PyErr_SetString(PyExc_OverflowError, "int too large to convert to float"); return -1.0; }
    return d;
}

int _PyLong_Sign(PyObject *o) { return piper_long_sign((PyLongObject *)o); }
int PyLong_GetSign(PyObject *o, int *sign) { if (!PyLong_Check(o)) { PyErr_SetString(PyExc_TypeError, "expect int"); return -1; } *sign = _PyLong_Sign(o); return 0; }
int PyLong_IsPositive(PyObject *o) { return _PyLong_Sign(o) > 0; }
int PyLong_IsNegative(PyObject *o) { return _PyLong_Sign(o) < 0; }
int PyLong_IsZero(PyObject *o) { return _PyLong_Sign(o) == 0; }

int64_t _PyLong_NumBits(PyObject *o) {
    PyLongObject *l = (PyLongObject *)o;
    Py_ssize_t n = piper_long_ndigits(l);
    if (n == 0) return 0;
    digit top = l->long_value.ob_digit[n - 1];
    int64_t bits = (int64_t)(n - 1) * PyLong_SHIFT;
    while (top) { bits++; top >>= 1; }
    return bits;
}

/* ---- digit arithmetic ------------------------------------------------------ */

static PyLongObject *x_add(PyLongObject *a, PyLongObject *b) {
    Py_ssize_t na = piper_long_ndigits(a), nb = piper_long_ndigits(b);
    if (na < nb) { PyLongObject *t = a; a = b; b = t; Py_ssize_t tn = na; na = nb; nb = tn; }
    PyLongObject *z = piper_long_alloc(na + 1);
    if (!z) return NULL;
    digit carry = 0;
    Py_ssize_t i;
    for (i = 0; i < nb; i++) { carry += a->long_value.ob_digit[i] + b->long_value.ob_digit[i]; z->long_value.ob_digit[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; }
    for (; i < na; i++) { carry += a->long_value.ob_digit[i]; z->long_value.ob_digit[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; }
    z->long_value.ob_digit[i] = carry;
    return z;
}

/* |a| - |b|, sign set by magnitude comparison */
static PyLongObject *x_sub(PyLongObject *a, PyLongObject *b) {
    Py_ssize_t na = piper_long_ndigits(a), nb = piper_long_ndigits(b);
    int sign = 1;
    if (na < nb) { sign = -1; PyLongObject *t = a; a = b; b = t; Py_ssize_t tn = na; na = nb; nb = tn; }
    else if (na == nb) {
        Py_ssize_t i = na;
        while (--i >= 0 && a->long_value.ob_digit[i] == b->long_value.ob_digit[i]) {}
        if (i < 0) return (PyLongObject *)Py_NewRef(small_int(0));
        if (a->long_value.ob_digit[i] < b->long_value.ob_digit[i]) { sign = -1; PyLongObject *t = a; a = b; b = t; }
        na = nb = i + 1;
    }
    PyLongObject *z = piper_long_alloc(na);
    if (!z) return NULL;
    digit borrow = 0;
    Py_ssize_t i;
    for (i = 0; i < nb; i++) { borrow = a->long_value.ob_digit[i] - b->long_value.ob_digit[i] - borrow; z->long_value.ob_digit[i] = borrow & PyLong_MASK; borrow >>= PyLong_SHIFT; borrow &= 1; }
    for (; i < na; i++) { borrow = a->long_value.ob_digit[i] - borrow; z->long_value.ob_digit[i] = borrow & PyLong_MASK; borrow >>= PyLong_SHIFT; borrow &= 1; }
    set_tag(z, sign, na);
    return z;
}

static PyObject *long_add(PyLongObject *a, PyLongObject *b) {
    if (piper_long_is_compact(a) && piper_long_is_compact(b)) return PyLong_FromLongLong((long long)(piper_long_compact_value(a) + piper_long_compact_value(b)));
    PyLongObject *z;
    if (piper_long_is_neg(a)) {
        if (piper_long_is_neg(b)) { z = x_add(a, b); if (z) set_tag(z, -1, piper_long_ndigits(z)); }
        else { z = x_sub(b, a); }
    } else {
        if (piper_long_is_neg(b)) z = x_sub(a, b);
        else z = x_add(a, b);
    }
    if (!z) return NULL;
    if (z == &piper_small_ints[PIPER_NSMALLNEGINTS]) return (PyObject *)z;
    return piper_long_normalize(z);
}

static PyObject *long_sub(PyLongObject *a, PyLongObject *b) {
    if (piper_long_is_compact(a) && piper_long_is_compact(b)) return PyLong_FromLongLong((long long)(piper_long_compact_value(a) - piper_long_compact_value(b)));
    PyLongObject *z;
    if (piper_long_is_neg(a)) {
        if (piper_long_is_neg(b)) z = x_sub(b, a);
        else { z = x_add(a, b); if (z) set_tag(z, -1, piper_long_ndigits(z)); }
    } else {
        if (piper_long_is_neg(b)) z = x_add(a, b);
        else z = x_sub(a, b);
    }
    if (!z) return NULL;
    if (z == &piper_small_ints[PIPER_NSMALLNEGINTS]) return (PyObject *)z;
    return piper_long_normalize(z);
}

static PyLongObject *x_mul(PyLongObject *a, PyLongObject *b) {
    Py_ssize_t na = piper_long_ndigits(a), nb = piper_long_ndigits(b);
    PyLongObject *z = piper_long_alloc(na + nb);
    if (!z) return NULL;
    memset(z->long_value.ob_digit, 0, (size_t)(na + nb) * sizeof(digit));
    for (Py_ssize_t i = 0; i < na; i++) {
        twodigits carry = 0;
        twodigits f = a->long_value.ob_digit[i];
        digit *pz = z->long_value.ob_digit + i;
        for (Py_ssize_t j = 0; j < nb; j++) {
            carry += *pz + b->long_value.ob_digit[j] * f;
            *pz++ = (digit)(carry & PyLong_MASK);
            carry >>= PyLong_SHIFT;
        }
        if (carry) *pz += (digit)(carry & PyLong_MASK);
    }
    return z;
}

static PyObject *long_mul(PyLongObject *a, PyLongObject *b) {
    if (piper_long_is_compact(a) && piper_long_is_compact(b)) {
        stwodigits v = piper_long_compact_value(a) * piper_long_compact_value(b);
        return PyLong_FromLongLong((long long)v);
    }
    PyLongObject *z = x_mul(a, b);
    if (!z) return NULL;
    set_tag(z, piper_long_is_neg(a) != piper_long_is_neg(b) ? -1 : 1, piper_long_ndigits(z));
    return piper_long_normalize(z);
}

/* Divide |a| by single digit n; returns quotient, remainder in *rem. */
static PyLongObject *divrem1(PyLongObject *a, digit n, digit *rem) {
    Py_ssize_t size = piper_long_ndigits(a);
    PyLongObject *z = piper_long_alloc(size);
    if (!z) return NULL;
    twodigits r = 0;
    for (Py_ssize_t i = size; --i >= 0;) {
        r = (r << PyLong_SHIFT) | a->long_value.ob_digit[i];
        z->long_value.ob_digit[i] = (digit)(r / n);
        r %= n;
    }
    *rem = (digit)r;
    return z;
}

/* Knuth algorithm D on magnitudes; a has >= 2 digits more or equal, |b| >= 2 digits. */
static PyLongObject *x_divrem(PyLongObject *v1, PyLongObject *w1, PyLongObject **prem) {
    Py_ssize_t size_v = piper_long_ndigits(v1), size_w = piper_long_ndigits(w1);
    PyLongObject *v = piper_long_alloc(size_v + 1), *w = piper_long_alloc(size_w);
    if (!v || !w) { Py_XDECREF(v); Py_XDECREF(w); return NULL; }
    int d = 0;
    digit wtop = w1->long_value.ob_digit[size_w - 1];
    while (wtop < (PyLong_BASE >> 1)) { wtop <<= 1; d++; }
    /* normalize: shift both left by d bits */
    digit carry = 0;
    for (Py_ssize_t i = 0; i < size_w; i++) { twodigits acc = ((twodigits)w1->long_value.ob_digit[i] << d) | carry; w->long_value.ob_digit[i] = (digit)(acc & PyLong_MASK); carry = (digit)(acc >> PyLong_SHIFT); }
    carry = 0;
    for (Py_ssize_t i = 0; i < size_v; i++) { twodigits acc = ((twodigits)v1->long_value.ob_digit[i] << d) | carry; v->long_value.ob_digit[i] = (digit)(acc & PyLong_MASK); carry = (digit)(acc >> PyLong_SHIFT); }
    v->long_value.ob_digit[size_v] = carry;
    size_v++;
    Py_ssize_t k = size_v - size_w;
    PyLongObject *a = piper_long_alloc(k);
    if (!a) { Py_DECREF(v); Py_DECREF(w); return NULL; }
    digit *v0 = v->long_value.ob_digit, *w0 = w->long_value.ob_digit;
    digit wm1 = w0[size_w - 1], wm2 = w0[size_w - 2];
    for (digit *vk = v0 + k, *ak = a->long_value.ob_digit + k; vk-- > v0;) {
        digit vtop = vk[size_w];
        twodigits vv = ((twodigits)vtop << PyLong_SHIFT) | vk[size_w - 1];
        digit q = (digit)(vv / wm1);
        digit r = (digit)(vv - (twodigits)wm1 * q);
        while ((twodigits)wm2 * q > (((twodigits)r << PyLong_SHIFT) | vk[size_w - 2])) {
            --q;
            r += wm1;
            if (r >= PyLong_BASE) break;
        }
        sdigit zhi = 0;
        for (Py_ssize_t i = 0; i < size_w; ++i) {
            stwodigits z = (sdigit)vk[i] + zhi - (stwodigits)q * (stwodigits)w0[i];
            vk[i] = (digit)z & PyLong_MASK;
            zhi = (sdigit)(z >> PyLong_SHIFT);
        }
        if ((sdigit)vtop + zhi < 0) {
            carry = 0;
            for (Py_ssize_t i = 0; i < size_w; ++i) { carry += vk[i] + w0[i]; vk[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; }
            --q;
        }
        *--ak = q;
    }
    /* unshift remainder */
    carry = 0;
    for (Py_ssize_t i = size_w; --i >= 0;) { twodigits acc = ((twodigits)carry << PyLong_SHIFT) | v0[i]; v0[i] = (digit)(acc >> d); carry = (digit)(acc & (((twodigits)1 << d) - 1)); }
    set_tag(v, 1, size_w);
    Py_DECREF(w);
    *prem = v;
    return a;
}

/* Magnitude division. Sets *pdiv, *prem (positive magnitudes, normalized later). */
static int long_divrem_mag(PyLongObject *a, PyLongObject *b, PyLongObject **pdiv, PyLongObject **prem) {
    Py_ssize_t na = piper_long_ndigits(a), nb = piper_long_ndigits(b);
    if (nb == 0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return -1; }
    if (na < nb || (na == nb && a->long_value.ob_digit[na - 1] < b->long_value.ob_digit[nb - 1])) {
        *pdiv = (PyLongObject *)Py_NewRef(small_int(0));
        PyLongObject *r = piper_long_alloc(na);
        if (!r) return -1;
        memcpy(r->long_value.ob_digit, a->long_value.ob_digit, (size_t)na * sizeof(digit));
        set_tag(r, na ? 1 : 0, na);
        *prem = r;
        return 0;
    }
    if (nb == 1) {
        digit rem;
        PyLongObject *z = divrem1(a, b->long_value.ob_digit[0], &rem);
        if (!z) return -1;
        *pdiv = z;
        PyLongObject *r = piper_long_alloc(1);
        r->long_value.ob_digit[0] = rem;
        set_tag(r, rem ? 1 : 0, rem ? 1 : 0);
        *prem = r;
        return 0;
    }
    PyLongObject *rem;
    PyLongObject *z = x_divrem(a, b, &rem);
    if (!z) return -1;
    *pdiv = z;
    *prem = rem;
    return 0;
}

/* Floor division and modulo with Python semantics. Either output may be NULL. */
static int l_divmod(PyLongObject *a, PyLongObject *b, PyObject **pdiv, PyObject **pmod) {
    if (piper_long_is_compact(a) && piper_long_is_compact(b)) {
        stwodigits x = piper_long_compact_value(a), y = piper_long_compact_value(b);
        if (y == 0) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return -1; }
        stwodigits q = x / y, r = x % y;
        if (r != 0 && ((r < 0) != (y < 0))) { r += y; q -= 1; }
        if (pdiv) *pdiv = PyLong_FromLongLong((long long)q);
        if (pmod) *pmod = PyLong_FromLongLong((long long)r);
        return 0;
    }
    PyLongObject *div, *mod;
    if (long_divrem_mag(a, b, &div, &mod) < 0) return -1;
    int sa = piper_long_is_neg(a) ? -1 : 1, sb = piper_long_is_neg(b) ? -1 : 1;
    /* quotient sign */
    if (piper_long_ndigits(div)) {
        if (div == &piper_small_ints[PIPER_NSMALLNEGINTS]) {} else set_tag(div, sa * sb, piper_long_ndigits(div));
    }
    if (piper_long_ndigits(mod)) set_tag(mod, sa, piper_long_ndigits(mod));
    PyObject *qd = piper_long_normalize(div);
    PyObject *md = piper_long_normalize(mod);
    if (!qd || !md) { Py_XDECREF(qd); Py_XDECREF(md); return -1; }
    /* Python floor semantics: if mod != 0 and sign(mod) != sign(b): mod += b, div -= 1 */
    if (!piper_long_is_zero((PyLongObject *)md) && (piper_long_is_neg((PyLongObject *)md) != (sb < 0))) {
        PyObject *m2 = long_add((PyLongObject *)md, b);
        Py_DECREF(md);
        md = m2;
        PyObject *one = small_int(1);
        PyObject *q2 = long_sub((PyLongObject *)qd, (PyLongObject *)one);
        Py_DECREF(qd);
        qd = q2;
        if (!md || !qd) { Py_XDECREF(qd); Py_XDECREF(md); return -1; }
    }
    if (pdiv) *pdiv = qd; else Py_DECREF(qd);
    if (pmod) *pmod = md; else Py_DECREF(md);
    return 0;
}

#define CHECK_BINOP(v, w) if (!PyLong_Check(v) || !PyLong_Check(w)) Py_RETURN_NOTIMPLEMENTED

static PyObject *long_nb_add(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); return long_add((PyLongObject *)a, (PyLongObject *)b); }
static PyObject *long_nb_sub(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); return long_sub((PyLongObject *)a, (PyLongObject *)b); }
static PyObject *long_nb_mul(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); return long_mul((PyLongObject *)a, (PyLongObject *)b); }
static PyObject *long_nb_floordiv(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); PyObject *q; if (l_divmod((PyLongObject *)a, (PyLongObject *)b, &q, NULL) < 0) return NULL; return q; }
static PyObject *long_nb_mod(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); PyObject *m; if (l_divmod((PyLongObject *)a, (PyLongObject *)b, NULL, &m) < 0) return NULL; return m; }
static PyObject *long_nb_divmod(PyObject *a, PyObject *b) {
    CHECK_BINOP(a, b);
    PyObject *q, *m;
    if (l_divmod((PyLongObject *)a, (PyLongObject *)b, &q, &m) < 0) return NULL;
    PyObject *t = PyTuple_Pack(2, q, m);
    Py_DECREF(q); Py_DECREF(m);
    return t;
}

static PyObject *long_nb_truediv(PyObject *a, PyObject *b) {
    CHECK_BINOP(a, b);
    if (piper_long_is_zero((PyLongObject *)b)) { PyErr_SetString(PyExc_ZeroDivisionError, "division by zero"); return NULL; }
    int oa, ob;
    double da = piper_int_to_double(a, &oa), db = piper_int_to_double(b, &ob);
    if (!oa && !ob && fabs(da) < 9007199254740992.0 && fabs(db) < 9007199254740992.0) return PyFloat_FromDouble(da / db);
    /* Big operands: scale to keep precision. */
    int64_t bits_a = _PyLong_NumBits(a), bits_b = _PyLong_NumBits(b);
    int64_t shift = bits_b - bits_a + 55;
    PyObject *num, *den;
    PyObject *sh;
    if (shift > 0) { sh = PyLong_FromLongLong(shift); num = PyNumber_Lshift(a, sh); Py_DECREF(sh); den = Py_NewRef(b); }
    else { sh = PyLong_FromLongLong(-shift); den = PyNumber_Lshift(b, sh); Py_DECREF(sh); num = Py_NewRef(a); }
    if (!num || !den) { Py_XDECREF(num); Py_XDECREF(den); return NULL; }
    PyObject *q, *r;
    int neg = piper_long_is_neg((PyLongObject *)num) != piper_long_is_neg((PyLongObject *)den);
    PyObject *an = PyNumber_Absolute(num), *ad = PyNumber_Absolute(den);
    Py_DECREF(num); Py_DECREF(den);
    if (l_divmod((PyLongObject *)an, (PyLongObject *)ad, &q, &r) < 0) { Py_DECREF(an); Py_DECREF(ad); return NULL; }
    int sticky = !piper_long_is_zero((PyLongObject *)r);
    Py_DECREF(r); Py_DECREF(an); Py_DECREF(ad);
    int o;
    double dq = piper_int_to_double(q, &o);
    Py_DECREF(q);
    if (sticky) dq += 0.5 * (dq >= 0 ? 1 : -1) * 0.0; /* sticky bit approximated */
    double res = ldexp(dq, (int)-shift);
    if (isinf(res)) { PyErr_SetString(PyExc_OverflowError, "integer division result too large for a float"); return NULL; }
    return PyFloat_FromDouble(neg ? -res : res);
}

static PyObject *long_nb_neg(PyObject *a) {
    PyLongObject *l = (PyLongObject *)a;
    if (piper_long_is_compact(l)) return PyLong_FromLongLong(-(long long)piper_long_compact_value(l));
    Py_ssize_t n = piper_long_ndigits(l);
    PyLongObject *z = piper_long_alloc(n);
    if (!z) return NULL;
    memcpy(z->long_value.ob_digit, l->long_value.ob_digit, (size_t)n * sizeof(digit));
    set_tag(z, piper_long_is_neg(l) ? 1 : -1, n);
    return (PyObject *)z;
}
static PyObject *long_nb_pos(PyObject *a) { if (PyLong_CheckExact(a)) return Py_NewRef(a); return PyLong_FromLongLong(PyLong_AsLongLong(a)); }
static PyObject *long_nb_abs(PyObject *a) { return piper_long_is_neg((PyLongObject *)a) ? long_nb_neg(a) : long_nb_pos(a); }
static int long_nb_bool(PyObject *a) { return !piper_long_is_zero((PyLongObject *)a); }
static PyObject *long_nb_int(PyObject *a) { return long_nb_pos(a); }
static PyObject *long_nb_float(PyObject *a) { double d = PyLong_AsDouble(a); if (d == -1.0 && PyErr_Occurred()) return NULL; return PyFloat_FromDouble(d); }

static PyObject *long_nb_invert(PyObject *a) {
    /* ~x == -(x+1) */
    PyObject *one = small_int(1);
    PyObject *t = long_add((PyLongObject *)a, (PyLongObject *)one);
    if (!t) return NULL;
    PyObject *r = long_nb_neg(t);
    Py_DECREF(t);
    return r;
}

static PyObject *long_lshift_n(PyLongObject *a, Py_ssize_t shift) {
    if (piper_long_is_zero(a)) return small_int(0);
    Py_ssize_t wordshift = shift / PyLong_SHIFT;
    int remshift = (int)(shift % PyLong_SHIFT);
    Py_ssize_t na = piper_long_ndigits(a);
    Py_ssize_t nz = na + wordshift + (remshift ? 1 : 0);
    PyLongObject *z = piper_long_alloc(nz);
    if (!z) return NULL;
    memset(z->long_value.ob_digit, 0, (size_t)nz * sizeof(digit));
    twodigits acc = 0;
    Py_ssize_t j = wordshift;
    for (Py_ssize_t i = 0; i < na; i++, j++) {
        acc |= (twodigits)a->long_value.ob_digit[i] << remshift;
        z->long_value.ob_digit[j] = (digit)(acc & PyLong_MASK);
        acc >>= PyLong_SHIFT;
    }
    if (remshift) z->long_value.ob_digit[j] = (digit)acc;
    set_tag(z, piper_long_is_neg(a) ? -1 : 1, nz);
    return piper_long_normalize(z);
}

static PyObject *long_rshift_n(PyLongObject *a, Py_ssize_t shift) {
    if (piper_long_is_neg(a)) {
        /* -(( -a - 1) >> shift) - 1 */
        PyObject *inv = long_nb_invert((PyObject *)a);
        if (!inv) return NULL;
        PyObject *sh = long_rshift_n((PyLongObject *)inv, shift);
        Py_DECREF(inv);
        if (!sh) return NULL;
        PyObject *r = long_nb_invert(sh);
        Py_DECREF(sh);
        return r;
    }
    Py_ssize_t wordshift = shift / PyLong_SHIFT;
    int remshift = (int)(shift % PyLong_SHIFT);
    Py_ssize_t na = piper_long_ndigits(a);
    Py_ssize_t nz = na - wordshift;
    if (nz <= 0) return small_int(0);
    PyLongObject *z = piper_long_alloc(nz);
    if (!z) return NULL;
    for (Py_ssize_t i = 0; i < nz; i++) {
        twodigits acc = a->long_value.ob_digit[i + wordshift] >> remshift;
        if (i + wordshift + 1 < na) acc |= ((twodigits)a->long_value.ob_digit[i + wordshift + 1] << (PyLong_SHIFT - remshift)) & PyLong_MASK;
        z->long_value.ob_digit[i] = (digit)(acc & PyLong_MASK);
    }
    set_tag(z, 1, nz);
    return piper_long_normalize(z);
}

static PyObject *long_nb_lshift(PyObject *a, PyObject *b) {
    CHECK_BINOP(a, b);
    if (piper_long_is_neg((PyLongObject *)b)) { PyErr_SetString(PyExc_ValueError, "negative shift count"); return NULL; }
    Py_ssize_t shift = PyLong_AsSsize_t(b);
    if (shift == -1 && PyErr_Occurred()) { PyErr_Clear(); PyErr_SetString(PyExc_OverflowError, "too many digits in integer"); return NULL; }
    return long_lshift_n((PyLongObject *)a, shift);
}

static PyObject *long_nb_rshift(PyObject *a, PyObject *b) {
    CHECK_BINOP(a, b);
    if (piper_long_is_neg((PyLongObject *)b)) { PyErr_SetString(PyExc_ValueError, "negative shift count"); return NULL; }
    Py_ssize_t shift = PyLong_AsSsize_t(b);
    if (shift == -1 && PyErr_Occurred()) { PyErr_Clear(); return small_int(piper_long_is_neg((PyLongObject *)a) ? -1 : 0); }
    return long_rshift_n((PyLongObject *)a, shift);
}

/* Two's complement bitwise ops via digit-wise ops on complemented magnitudes. */
static PyObject *long_bitwise(PyLongObject *a, char op, PyLongObject *b) {
    Py_ssize_t na = piper_long_ndigits(a), nb = piper_long_ndigits(b);
    int nega = piper_long_is_neg(a), negb = piper_long_is_neg(b);
    /* Build two's complement digit arrays of length max+1 */
    Py_ssize_t n = (na > nb ? na : nb) + 1;
    digit *da = PyMem_Calloc((size_t)n, sizeof(digit)), *db = PyMem_Calloc((size_t)n, sizeof(digit));
    if (!da || !db) { PyMem_Free(da); PyMem_Free(db); return PyErr_NoMemory(); }
    memcpy(da, a->long_value.ob_digit, (size_t)na * sizeof(digit));
    memcpy(db, b->long_value.ob_digit, (size_t)nb * sizeof(digit));
    if (nega) { digit carry = 1; for (Py_ssize_t i = 0; i < n; i++) { carry += (da[i] ^ PyLong_MASK); da[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; } }
    if (negb) { digit carry = 1; for (Py_ssize_t i = 0; i < n; i++) { carry += (db[i] ^ PyLong_MASK); db[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; } }
    int negz;
    switch (op) { case '&': negz = nega & negb; break; case '|': negz = nega | negb; break; default: negz = nega ^ negb; }
    PyLongObject *z = piper_long_alloc(n);
    if (!z) { PyMem_Free(da); PyMem_Free(db); return NULL; }
    for (Py_ssize_t i = 0; i < n; i++) {
        switch (op) { case '&': z->long_value.ob_digit[i] = da[i] & db[i]; break; case '|': z->long_value.ob_digit[i] = da[i] | db[i]; break; default: z->long_value.ob_digit[i] = da[i] ^ db[i]; }
    }
    PyMem_Free(da); PyMem_Free(db);
    if (negz) { digit carry = 1; for (Py_ssize_t i = 0; i < n; i++) { carry += (z->long_value.ob_digit[i] ^ PyLong_MASK); z->long_value.ob_digit[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; } }
    set_tag(z, negz ? -1 : 1, n);
    return piper_long_normalize(z);
}
static PyObject *long_nb_and(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); return long_bitwise((PyLongObject *)a, '&', (PyLongObject *)b); }
static PyObject *long_nb_or(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); return long_bitwise((PyLongObject *)a, '|', (PyLongObject *)b); }
static PyObject *long_nb_xor(PyObject *a, PyObject *b) { CHECK_BINOP(a, b); return long_bitwise((PyLongObject *)a, '^', (PyLongObject *)b); }

static PyObject *long_nb_pow(PyObject *v, PyObject *w, PyObject *z) {
    CHECK_BINOP(v, w);
    if (z != Py_None && !PyLong_Check(z)) Py_RETURN_NOTIMPLEMENTED;
    PyLongObject *a = (PyLongObject *)v, *b = (PyLongObject *)w;
    if (piper_long_is_neg(b)) {
        if (z != Py_None) {
            /* modular inverse */
            PyObject *inv = NULL;
            PyObject *m = z, *x = v;
            PyObject *g0 = Py_NewRef(x), *g1 = Py_NewRef(m), *c0 = small_int(1), *c1 = small_int(0);
            Py_INCREF(c0); Py_INCREF(c1);
            while (!piper_long_is_zero((PyLongObject *)g1)) {
                PyObject *q, *r;
                if (l_divmod((PyLongObject *)g0, (PyLongObject *)g1, &q, &r) < 0) goto inv_fail;
                Py_DECREF(g0); g0 = g1; g1 = r;
                PyObject *t = long_mul((PyLongObject *)q, (PyLongObject *)c1);
                Py_DECREF(q);
                PyObject *nc = long_sub((PyLongObject *)c0, (PyLongObject *)t);
                Py_DECREF(t);
                Py_DECREF(c0); c0 = c1; c1 = nc;
            }
            if (!(piper_long_is_compact((PyLongObject *)g0) && piper_long_compact_value((PyLongObject *)g0) == 1)) { PyErr_SetString(PyExc_ValueError, "base is not invertible for the given modulus"); goto inv_fail; }
            if (l_divmod((PyLongObject *)c0, (PyLongObject *)m, NULL, &inv) < 0) goto inv_fail;
            Py_DECREF(g0); Py_DECREF(g1); Py_DECREF(c0); Py_DECREF(c1);
            PyObject *nb = long_nb_neg(w);
            PyObject *r = long_nb_pow(inv, nb, z);
            Py_DECREF(inv); Py_DECREF(nb);
            return r;
        inv_fail:
            Py_XDECREF(g0); Py_XDECREF(g1); Py_XDECREF(c0); Py_XDECREF(c1);
            return NULL;
        }
        PyObject *fa = long_nb_float(v), *fb = long_nb_float(w);
        if (!fa || !fb) { Py_XDECREF(fa); Py_XDECREF(fb); return NULL; }
        PyObject *r = PyNumber_Power(fa, fb, Py_None);
        Py_DECREF(fa); Py_DECREF(fb);
        return r;
    }
    if (z != Py_None && piper_long_is_zero((PyLongObject *)z)) { PyErr_SetString(PyExc_ValueError, "pow() 3rd argument cannot be 0"); return NULL; }
    PyObject *result = Py_NewRef(small_int(1));
    PyObject *base = Py_NewRef(v);
    if (z != Py_None) { PyObject *t; if (l_divmod((PyLongObject *)base, (PyLongObject *)z, NULL, &t) < 0) { Py_DECREF(base); Py_DECREF(result); return NULL; } Py_DECREF(base); base = t; }
    /* binary exponentiation over the digits of b */
    Py_ssize_t nb = piper_long_ndigits(b);
    for (Py_ssize_t i = 0; i < nb; i++) {
        digit d = b->long_value.ob_digit[i];
        for (int bit = 0; bit < PyLong_SHIFT; bit++) {
            if (d & 1) {
                PyObject *t = long_mul((PyLongObject *)result, (PyLongObject *)base);
                Py_DECREF(result);
                if (!t) { Py_DECREF(base); return NULL; }
                result = t;
                if (z != Py_None) { PyObject *m; if (l_divmod((PyLongObject *)result, (PyLongObject *)z, NULL, &m) < 0) { Py_DECREF(base); Py_DECREF(result); return NULL; } Py_DECREF(result); result = m; }
            }
            d >>= 1;
            if (d == 0 && i == nb - 1) break;
            PyObject *t = long_mul((PyLongObject *)base, (PyLongObject *)base);
            Py_DECREF(base);
            if (!t) { Py_DECREF(result); return NULL; }
            base = t;
            if (z != Py_None) { PyObject *m; if (l_divmod((PyLongObject *)base, (PyLongObject *)z, NULL, &m) < 0) { Py_DECREF(base); Py_DECREF(result); return NULL; } Py_DECREF(base); base = m; }
        }
    }
    Py_DECREF(base);
    if (z != Py_None) {
        /* Python: result has the sign of the modulus */
        PyObject *m;
        if (l_divmod((PyLongObject *)result, (PyLongObject *)z, NULL, &m) < 0) { Py_DECREF(result); return NULL; }
        Py_DECREF(result);
        result = m;
    }
    return result;
}

static PyObject *long_nb_index(PyObject *a) { return long_nb_pos(a); }

/* ---- comparison / hash ------------------------------------------------------- */

int piper_long_compare(PyLongObject *a, PyLongObject *b) {
    if (piper_long_is_compact(a) && piper_long_is_compact(b)) {
        stwodigits x = piper_long_compact_value(a), y = piper_long_compact_value(b);
        return x < y ? -1 : x > y;
    }
    int sa = piper_long_sign(a), sb = piper_long_sign(b);
    if (sa != sb) return sa < sb ? -1 : 1;
    Py_ssize_t na = piper_long_ndigits(a), nb = piper_long_ndigits(b);
    int mag;
    if (na != nb) mag = na < nb ? -1 : 1;
    else {
        Py_ssize_t i = na;
        while (--i >= 0 && a->long_value.ob_digit[i] == b->long_value.ob_digit[i]) {}
        mag = i < 0 ? 0 : (a->long_value.ob_digit[i] < b->long_value.ob_digit[i] ? -1 : 1);
    }
    return sa < 0 ? -mag : mag;
}

static PyObject *long_richcompare(PyObject *a, PyObject *b, int op) {
    CHECK_BINOP(a, b);
    int c = piper_long_compare((PyLongObject *)a, (PyLongObject *)b);
    int r;
    switch (op) { case Py_LT: r = c < 0; break; case Py_LE: r = c <= 0; break; case Py_EQ: r = c == 0; break; case Py_NE: r = c != 0; break; case Py_GT: r = c > 0; break; default: r = c >= 0; }
    Py_RETURN_BOOL(r);
}

static Py_hash_t long_hash(PyObject *o) {
    PyLongObject *v = (PyLongObject *)o;
    if (piper_long_is_compact(v)) {
        Py_hash_t x = (Py_hash_t)piper_long_compact_value(v);
        return x == -1 ? -2 : x;
    }
    Py_uhash_t x = 0;
    Py_ssize_t n = piper_long_ndigits(v);
    int sign = piper_long_is_neg(v) ? -1 : 1;
    for (Py_ssize_t i = n; --i >= 0;) {
        x = ((x << PyLong_SHIFT) & _PyHASH_MODULUS) | (x >> (_PyHASH_BITS - PyLong_SHIFT));
        x += v->long_value.ob_digit[i];
        if (x >= _PyHASH_MODULUS) x -= _PyHASH_MODULUS;
    }
    x = x * (Py_uhash_t)sign;
    if (x == (Py_uhash_t)-1) x = (Py_uhash_t)-2;
    return (Py_hash_t)x;
}

/* ---- string conversion ---------------------------------------------------- */

PyObject *piper_long_to_decimal_string(PyObject *o) {
    PyLongObject *v = (PyLongObject *)o;
    if (piper_long_is_compact(v)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", (long long)piper_long_compact_value(v));
        return PyUnicode_FromString(buf);
    }
    /* Repeated division by 10^9. */
    Py_ssize_t n = piper_long_ndigits(v);
    digit *tmp = PyMem_Malloc((size_t)n * sizeof(digit));
    if (!tmp) return PyErr_NoMemory();
    memcpy(tmp, v->long_value.ob_digit, (size_t)n * sizeof(digit));
    size_t cap = (size_t)n * 10 + 2;
    char *out = PyMem_Malloc(cap);
    size_t pos = cap;
    out[--pos] = 0;
    Py_ssize_t size = n;
    while (size > 0) {
        twodigits rem = 0;
        for (Py_ssize_t i = size; --i >= 0;) { rem = (rem << PyLong_SHIFT) | tmp[i]; tmp[i] = (digit)(rem / 1000000000u); rem %= 1000000000u; }
        while (size > 0 && tmp[size - 1] == 0) size--;
        uint32_t chunk = (uint32_t)rem;
        for (int k = 0; k < 9; k++) { out[--pos] = (char)('0' + chunk % 10); chunk /= 10; if (size == 0 && chunk == 0) break; }
    }
    while (out[pos] == '0' && out[pos + 1]) pos++;
    if (piper_long_is_neg(v)) out[--pos] = '-';
    PyObject *r = PyUnicode_FromString(out + pos);
    PyMem_Free(out); PyMem_Free(tmp);
    return r;
}

PyObject *_PyLong_Format(PyObject *o, int base) {
    if (base == 10) return piper_long_to_decimal_string(o);
    PyLongObject *v = (PyLongObject *)o;
    int bits = base == 2 ? 1 : base == 8 ? 3 : 4;
    const char *prefix = base == 2 ? "0b" : base == 8 ? "0o" : "0x";
    Py_ssize_t n = piper_long_ndigits(v);
    size_t cap = (size_t)n * PyLong_SHIFT / (size_t)bits + 8;
    char *out = PyMem_Malloc(cap);
    size_t pos = cap;
    out[--pos] = 0;
    if (n == 0) out[--pos] = '0';
    else {
        twodigits acc = 0;
        int accbits = 0;
        for (Py_ssize_t i = 0; i < n; i++) {
            acc |= (twodigits)v->long_value.ob_digit[i] << accbits;
            accbits += PyLong_SHIFT;
            while (accbits >= bits || (i == n - 1 && acc)) {
                int d = (int)(acc & ((1u << bits) - 1));
                out[--pos] = "0123456789abcdef"[d];
                acc >>= bits;
                accbits -= bits;
                if (i == n - 1 && acc == 0) break;
            }
        }
        while (out[pos] == '0' && out[pos + 1]) pos++;
    }
    pos -= 2;
    out[pos] = prefix[0]; out[pos + 1] = prefix[1];
    if (piper_long_is_neg(v)) out[--pos] = '-';
    PyObject *r = PyUnicode_FromString(out + pos);
    PyMem_Free(out);
    return r;
}

static PyObject *long_repr(PyObject *o) { return piper_long_to_decimal_string(o); }

/* Parse digits (no sign, underscores allowed between digits). */
PyObject *piper_long_from_digits_str(const char *s, Py_ssize_t n, int base, int *ok) {
    *ok = 0;
    if (n == 0) return NULL;
    PyObject *result = Py_NewRef(small_int(0));
    PyObject *pbase = PyLong_FromLong(base);
    int prev_us = 1;
    for (Py_ssize_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '_') { if (prev_us) { Py_DECREF(result); Py_DECREF(pbase); return NULL; } prev_us = 1; continue; }
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else { Py_DECREF(result); Py_DECREF(pbase); return NULL; }
        if (d >= base) { Py_DECREF(result); Py_DECREF(pbase); return NULL; }
        prev_us = 0;
        PyObject *t = long_mul((PyLongObject *)result, (PyLongObject *)pbase);
        Py_DECREF(result);
        PyObject *pd = small_int((sdigit)d);
        result = long_add((PyLongObject *)t, (PyLongObject *)pd);
        Py_DECREF(t);
        if (!result) { Py_DECREF(pbase); return NULL; }
    }
    Py_DECREF(pbase);
    if (prev_us) { Py_DECREF(result); return NULL; }
    *ok = 1;
    return result;
}

PyObject *piper_int_from_decimal(const char *digits) { int ok; PyObject *r = piper_long_from_digits_str(digits + (digits[0] == '-'), (Py_ssize_t)strlen(digits) - (digits[0] == '-'), 10, &ok); if (r && digits[0] == '-') { PyObject *n = long_nb_neg(r); Py_DECREF(r); r = n; } return r; }
PyObject *piper_int_const(const char *decimal) { return piper_int_from_decimal(decimal); }

static PyObject *long_from_text(const char *s, Py_ssize_t n, int base, PyObject *orig) {
    const char *start = s, *end = s + n;
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r' || *start == '\f' || *start == '\v')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\f' || end[-1] == '\v')) end--;
    int neg = 0;
    if (start < end && (*start == '+' || *start == '-')) { neg = *start == '-'; start++; }
    int b = base;
    if (end - start >= 2 && start[0] == '0') {
        char p = (char)(start[1] | 0x20);
        int pb = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
        if (pb && (base == 0 || base == pb)) { b = pb; start += 2; if (start < end && *start == '_') start++; }
    }
    if (b == 0) {
        b = 10;
        /* leading zeros are not allowed in base 0 unless the value is 0 */
        if (end - start > 1 && start[0] == '0') {
            int allzero = 1;
            for (const char *p = start; p < end; p++) if (*p != '0' && *p != '_') { allzero = 0; break; }
            if (!allzero) goto bad;
        }
    }
    int ok;
    PyObject *r = piper_long_from_digits_str(start, end - start, b, &ok);
    if (!ok) goto bad;
    if (neg) { PyObject *t = long_nb_neg(r); Py_DECREF(r); r = t; }
    return r;
bad:
    PyErr_Clear();
    if (orig) { PyObject *rp = PyObject_Repr(orig); PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: %U", base, rp); Py_XDECREF(rp); }
    else { PyObject *s2 = PyUnicode_FromStringAndSize(s, n); PyObject *rp = s2 ? PyObject_Repr(s2) : NULL; PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: %U", base, rp); Py_XDECREF(s2); Py_XDECREF(rp); }
    return NULL;
}

PyObject *PyLong_FromString(const char *s, char **pend, int base) {
    if (base != 0 && (base < 2 || base > 36)) { PyErr_SetString(PyExc_ValueError, "int() base must be >= 2 and <= 36, or 0"); return NULL; }
    PyObject *r = long_from_text(s, (Py_ssize_t)strlen(s), base, NULL);
    if (pend) *pend = (char *)s + strlen(s);
    return r;
}

PyObject *PyLong_FromUnicodeObject(PyObject *u, int base) {
    if (base != 0 && (base < 2 || base > 36)) { PyErr_SetString(PyExc_ValueError, "int() base must be >= 2 and <= 36, or 0"); return NULL; }
    Py_ssize_t n;
    const char *s = PyUnicode_AsUTF8AndSize(u, &n);
    if (!s) return NULL;
    /* Non-ASCII digits: translate via unicodedata later; require ASCII here. */
    return long_from_text(s, n, base, u);
}

/* ---- int type ------------------------------------------------------------- */

static PyObject *long_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    PyObject *x = NULL, *obase = NULL;
    if (nargs > 2) { PyErr_Format(PyExc_TypeError, "int() takes at most 2 arguments (%zd given)", nargs); return NULL; }
    if (nargs >= 1) x = PyTuple_GET_ITEM(args, 0);
    if (nargs == 2) obase = PyTuple_GET_ITEM(args, 1);
    if (kwds && PyDict_Size(kwds)) {
        PyObject *b = PyDict_GetItemString(kwds, "base");
        if (b && !obase) obase = b;
        if (PyDict_Size(kwds) > (b ? 1 : 0)) { PyErr_SetString(PyExc_TypeError, "'x' is an invalid keyword argument for int()"); return NULL; }
    }
    PyObject *r;
    if (!x) { if (obase) { PyErr_SetString(PyExc_TypeError, "int() missing string argument"); return NULL; } r = Py_NewRef(small_int(0)); }
    else if (!obase) r = PyNumber_Long(x);
    else {
        Py_ssize_t base = PyNumber_AsSsize_t(obase, NULL);
        if (base == -1 && PyErr_Occurred()) return NULL;
        if (base != 0 && (base < 2 || base > 36)) { PyErr_SetString(PyExc_ValueError, "int() base must be >= 2 and <= 36, or 0"); return NULL; }
        if (PyUnicode_Check(x)) r = PyLong_FromUnicodeObject(x, (int)base);
        else if (PyBytes_Check(x)) { PyObject *s = PyUnicode_FromStringAndSize(PyBytes_AS_STRING(x), PyBytes_GET_SIZE(x)); r = s ? PyLong_FromUnicodeObject(s, (int)base) : NULL; Py_XDECREF(s); }
        else { PyErr_SetString(PyExc_TypeError, "int() can't convert non-string with explicit base"); return NULL; }
    }
    if (!r || tp == &PyLong_Type) return r;
    /* subclass: copy into an instance of tp */
    Py_ssize_t n = piper_long_ndigits((PyLongObject *)r);
    PyLongObject *z = (PyLongObject *)tp->tp_alloc(tp, n ? n : 1);
    if (!z) { Py_DECREF(r); return NULL; }
    z->long_value.lv_tag = ((PyLongObject *)r)->long_value.lv_tag;
    memcpy(z->long_value.ob_digit, ((PyLongObject *)r)->long_value.ob_digit, (size_t)(n ? n : 1) * sizeof(digit));
    Py_DECREF(r);
    return (PyObject *)z;
}

static PyObject *long_bit_length(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return PyLong_FromLongLong(_PyLong_NumBits(self)); }
PyObject *piper_long_bit_length(PyObject *v) { return long_bit_length(v, NULL); }
static PyObject *long_bit_count(PyObject *self, PyObject *unused) {
    PIPER_UNUSED(unused);
    PyLongObject *v = (PyLongObject *)self;
    int64_t c = 0;
    for (Py_ssize_t i = 0; i < piper_long_ndigits(v); i++) c += __builtin_popcount(v->long_value.ob_digit[i]);
    return PyLong_FromLongLong(c);
}
static PyObject *long_conjugate(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return long_nb_pos(self); }
static PyObject *long_to_bytes(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    Py_ssize_t length = 1;
    const char *order = "big";
    int is_signed = 0;
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    if (nargs >= 1) { length = PyNumber_AsSsize_t(args[0], PyExc_OverflowError); if (length == -1 && PyErr_Occurred()) return NULL; }
    if (nargs >= 2) { if (!PyUnicode_Check(args[1])) { PyErr_SetString(PyExc_TypeError, "to_bytes() argument 'byteorder' must be str"); return NULL; } order = PyUnicode_AsUTF8(args[1]); }
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kwnames, i);
        PyObject *v = args[nargs + i];
        if (PyUnicode_EqualToUTF8(k, "length")) { length = PyNumber_AsSsize_t(v, PyExc_OverflowError); if (length == -1 && PyErr_Occurred()) return NULL; }
        else if (PyUnicode_EqualToUTF8(k, "byteorder")) order = PyUnicode_AsUTF8(v);
        else if (PyUnicode_EqualToUTF8(k, "signed")) { is_signed = PyObject_IsTrue(v); if (is_signed < 0) return NULL; }
        else { PyErr_Format(PyExc_TypeError, "to_bytes() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    if (length < 0) { PyErr_SetString(PyExc_ValueError, "length argument must be non-negative"); return NULL; }
    int little = !strcmp(order, "little");
    if (!little && strcmp(order, "big")) { PyErr_SetString(PyExc_ValueError, "byteorder must be either 'little' or 'big'"); return NULL; }
    PyObject *b = PyBytes_FromStringAndSize(NULL, length);
    if (!b) return NULL;
    if (_PyLong_AsByteArray((PyLongObject *)self, (unsigned char *)PyBytes_AS_STRING(b), (size_t)length, little, is_signed, 1) < 0) { Py_DECREF(b); return NULL; }
    return b;
}
static PyObject *long_from_bytes(PyObject *cls, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    PyObject *bytes = NULL;
    const char *order = "big";
    int is_signed = 0;
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    if (nargs >= 1) bytes = args[0];
    if (nargs >= 2) order = PyUnicode_AsUTF8(args[1]);
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kwnames, i);
        PyObject *v = args[nargs + i];
        if (PyUnicode_EqualToUTF8(k, "bytes")) bytes = v;
        else if (PyUnicode_EqualToUTF8(k, "byteorder")) order = PyUnicode_AsUTF8(v);
        else if (PyUnicode_EqualToUTF8(k, "signed")) { is_signed = PyObject_IsTrue(v); if (is_signed < 0) return NULL; }
        else { PyErr_Format(PyExc_TypeError, "from_bytes() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    if (!bytes) { PyErr_SetString(PyExc_TypeError, "from_bytes() missing required argument 'bytes'"); return NULL; }
    int little = !strcmp(order, "little");
    if (!little && strcmp(order, "big")) { PyErr_SetString(PyExc_ValueError, "byteorder must be either 'little' or 'big'"); return NULL; }
    PyObject *b = PyObject_Bytes(bytes);
    if (!b) return NULL;
    PyObject *r = _PyLong_FromByteArray((unsigned char *)PyBytes_AS_STRING(b), (size_t)PyBytes_GET_SIZE(b), little, is_signed);
    Py_DECREF(b);
    if (r && (PyTypeObject *)cls != &PyLong_Type) { PyObject *t = PyObject_CallOneArg(cls, r); Py_DECREF(r); r = t; }
    return r;
}
static PyObject *long_as_integer_ratio(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); PyObject *n = long_nb_pos(self); PyObject *t = PyTuple_Pack(2, n, small_int(1)); Py_DECREF(n); return t; }
static PyObject *long_is_integer(PyObject *self, PyObject *unused) { PIPER_UNUSED(self); PIPER_UNUSED(unused); Py_RETURN_TRUE; }
static PyObject *long_getnewargs(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); PyObject *n = long_nb_pos(self); PyObject *t = PyTuple_Pack(1, n); Py_DECREF(n); return t; }
static PyObject *long_format(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (piper_args_range("__format__", nargs, 1, 1) < 0) return NULL;
    if (!PyUnicode_Check(args[0])) { PyErr_SetString(PyExc_TypeError, "__format__() argument must be str"); return NULL; }
    return piper_format_spec_apply(self, args[0]);
}
static PyObject *long_round(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs == 0 || args[0] == Py_None) return long_nb_pos(self);
    PyObject *nd = PyNumber_Index(args[0]);
    if (!nd) return NULL;
    if (!piper_long_is_neg((PyLongObject *)nd)) { Py_DECREF(nd); return long_nb_pos(self); }
    /* round to 10**(-ndigits) with round-half-even */
    PyObject *neg = long_nb_neg(nd);
    Py_DECREF(nd);
    PyObject *ten = PyLong_FromLong(10);
    PyObject *pow10 = long_nb_pow(ten, neg, Py_None);
    Py_DECREF(ten); Py_DECREF(neg);
    if (!pow10) return NULL;
    PyObject *q, *r;
    if (l_divmod((PyLongObject *)self, (PyLongObject *)pow10, &q, &r) < 0) { Py_DECREF(pow10); return NULL; }
    PyObject *twice = long_lshift_n((PyLongObject *)r, 1);
    int c = piper_long_compare((PyLongObject *)twice, (PyLongObject *)pow10);
    Py_DECREF(twice); Py_DECREF(r);
    int odd = piper_long_ndigits((PyLongObject *)q) && (((PyLongObject *)q)->long_value.ob_digit[0] & 1);
    if (c > 0 || (c == 0 && odd)) { PyObject *one = small_int(1); PyObject *t = long_add((PyLongObject *)q, (PyLongObject *)one); Py_DECREF(q); q = t; }
    PyObject *res = long_mul((PyLongObject *)q, (PyLongObject *)pow10);
    Py_DECREF(q); Py_DECREF(pow10);
    return res;
}
static PyObject *long_trunc(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return long_nb_pos(self); }
static PyObject *long_sizeof(PyObject *self, PyObject *unused) { PIPER_UNUSED(unused); return PyLong_FromSsize_t((Py_ssize_t)offsetof(PyLongObject, long_value.ob_digit) + Py_MAX(piper_long_ndigits((PyLongObject *)self), 1) * (Py_ssize_t)sizeof(digit)); }

static PyObject *long_real(PyObject *self, void *c) { PIPER_UNUSED(c); return long_nb_pos(self); }
static PyObject *long_imag(PyObject *self, void *c) { PIPER_UNUSED(self); PIPER_UNUSED(c); return Py_NewRef(small_int(0)); }
static PyObject *long_denominator(PyObject *self, void *c) { PIPER_UNUSED(self); PIPER_UNUSED(c); return Py_NewRef(small_int(1)); }

static PyMethodDef long_methods[] = {
    { "bit_length", long_bit_length, METH_NOARGS, NULL },
    { "bit_count", long_bit_count, METH_NOARGS, NULL },
    { "conjugate", long_conjugate, METH_NOARGS, NULL },
    { "to_bytes", (PyCFunction)(void (*)(void))long_to_bytes, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "from_bytes", (PyCFunction)(void (*)(void))long_from_bytes, METH_FASTCALL | METH_KEYWORDS | METH_CLASS, NULL },
    { "as_integer_ratio", long_as_integer_ratio, METH_NOARGS, NULL },
    { "is_integer", long_is_integer, METH_NOARGS, NULL },
    { "__getnewargs__", long_getnewargs, METH_NOARGS, NULL },
    { "__format__", (PyCFunction)(void (*)(void))long_format, METH_FASTCALL, NULL },
    { "__round__", (PyCFunction)(void (*)(void))long_round, METH_FASTCALL, NULL },
    { "__trunc__", long_trunc, METH_NOARGS, NULL },
    { "__floor__", long_trunc, METH_NOARGS, NULL },
    { "__ceil__", long_trunc, METH_NOARGS, NULL },
    { "__sizeof__", long_sizeof, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyGetSetDef long_getsets[] = {
    { "real", long_real, NULL, NULL, NULL },
    { "imag", long_imag, NULL, NULL, NULL },
    { "numerator", long_real, NULL, NULL, NULL },
    { "denominator", long_denominator, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL },
};

static PyNumberMethods long_as_number = {
    .nb_add = long_nb_add, .nb_subtract = long_nb_sub, .nb_multiply = long_nb_mul, .nb_remainder = long_nb_mod,
    .nb_divmod = long_nb_divmod, .nb_power = long_nb_pow, .nb_negative = long_nb_neg, .nb_positive = long_nb_pos,
    .nb_absolute = long_nb_abs, .nb_bool = long_nb_bool, .nb_invert = long_nb_invert, .nb_lshift = long_nb_lshift,
    .nb_rshift = long_nb_rshift, .nb_and = long_nb_and, .nb_xor = long_nb_xor, .nb_or = long_nb_or, .nb_int = long_nb_int,
    .nb_float = long_nb_float, .nb_floor_divide = long_nb_floordiv, .nb_true_divide = long_nb_truediv, .nb_index = long_nb_index,
};

static void long_dealloc(PyObject *o) { Py_TYPE(o)->tp_free(o); }

PyTypeObject PyLong_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "int",
    .tp_basicsize = offsetof(PyLongObject, long_value.ob_digit),
    .tp_itemsize = sizeof(digit),
    .tp_dealloc = long_dealloc,
    .tp_repr = long_repr,
    .tp_as_number = &long_as_number,
    .tp_hash = long_hash,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_LONG_SUBCLASS | Py_TPFLAGS_MATCH_SELF,
    .tp_doc = "int([x]) -> integer\nint(x, base=10) -> integer",
    .tp_richcompare = long_richcompare,
    .tp_methods = long_methods,
    .tp_getset = long_getsets,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = long_new,
    .tp_free = PyObject_Free,
};

/* ---- byte array conversions ---------------------------------------------- */

PyObject *_PyLong_FromByteArray(const unsigned char *bytes, size_t n, int little_endian, int is_signed) {
    if (n == 0) return small_int(0);
    int neg = 0;
    if (is_signed) { unsigned char msb = little_endian ? bytes[n - 1] : bytes[0]; neg = (msb & 0x80) != 0; }
    Py_ssize_t ndigits = (Py_ssize_t)((n * 8 + PyLong_SHIFT - 1) / PyLong_SHIFT);
    PyLongObject *v = piper_long_alloc(ndigits);
    if (!v) return NULL;
    memset(v->long_value.ob_digit, 0, (size_t)ndigits * sizeof(digit));
    twodigits acc = 0;
    int accbits = 0;
    Py_ssize_t idx = 0;
    digit carry = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = little_endian ? bytes[i] : bytes[n - 1 - i];
        if (neg) { b = (unsigned char)(b ^ 0xFF); }
        acc |= (twodigits)b << accbits;
        accbits += 8;
        if (accbits >= PyLong_SHIFT) { v->long_value.ob_digit[idx++] = (digit)(acc & PyLong_MASK); acc >>= PyLong_SHIFT; accbits -= PyLong_SHIFT; }
    }
    if (accbits) v->long_value.ob_digit[idx++] = (digit)acc;
    if (neg) {
        for (Py_ssize_t i = 0; i < ndigits; i++) { carry += v->long_value.ob_digit[i]; v->long_value.ob_digit[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; }
    }
    set_tag(v, neg ? -1 : 1, ndigits);
    return piper_long_normalize(v);
}

int _PyLong_AsByteArray(PyLongObject *v, unsigned char *bytes, size_t n, int little_endian, int is_signed, int with_exceptions) {
    int neg = piper_long_is_neg(v);
    if (neg && !is_signed) { if (with_exceptions) PyErr_SetString(PyExc_OverflowError, "can't convert negative int to unsigned"); return -1; }
    Py_ssize_t nd = piper_long_ndigits(v);
    /* two's complement digits */
    digit *tmp = PyMem_Calloc((size_t)nd + 1, sizeof(digit));
    memcpy(tmp, v->long_value.ob_digit, (size_t)nd * sizeof(digit));
    if (neg) { digit carry = 1; for (Py_ssize_t i = 0; i <= nd; i++) { carry += tmp[i] ^ PyLong_MASK; tmp[i] = carry & PyLong_MASK; carry >>= PyLong_SHIFT; } }
    twodigits acc = 0;
    int accbits = 0;
    Py_ssize_t idx = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (accbits < 8) {
            if (idx <= nd) { acc |= (twodigits)tmp[idx++] << accbits; accbits += PyLong_SHIFT; }
            else { acc |= (twodigits)(neg ? 0xFF : 0) << accbits; accbits += 8; }
        }
        unsigned char b = (unsigned char)(acc & 0xFF);
        acc >>= 8; accbits -= 8;
        if (little_endian) bytes[i] = b; else bytes[n - 1 - i] = b;
    }
    /* overflow check: remaining bits must all be sign bits */
    int overflow = 0;
    for (; idx <= nd; idx++) { if (tmp[idx] != (neg ? PyLong_MASK : 0)) overflow = 1; }
    while (accbits > 0) { if ((acc & 0xFF) != (twodigits)(neg ? 0xFF : 0)) overflow = 1; acc >>= 8; accbits -= 8; }
    if (!overflow && n > 0 && is_signed) {
        unsigned char msb = little_endian ? bytes[n - 1] : bytes[0];
        if (((msb & 0x80) != 0) != (neg != 0)) overflow = 1;
    }
    if (!overflow && n == 0 && !piper_long_is_zero(v)) overflow = 1;
    PyMem_Free(tmp);
    if (overflow) { if (with_exceptions) PyErr_SetString(PyExc_OverflowError, "int too big to convert"); return -1; }
    return 0;
}

PyObject *PyLong_GetInfo(void) {
    PyObject *d = PyDict_New();
    PyObject *v = PyLong_FromLong(PyLong_SHIFT);
    PyDict_SetItemString(d, "bits_per_digit", v); Py_DECREF(v);
    v = PyLong_FromLong(sizeof(digit));
    PyDict_SetItemString(d, "sizeof_digit", v); Py_DECREF(v);
    return d;
}

/* ---- bool ------------------------------------------------------------------ */

PyLongObject _Py_TrueStruct = { PyObject_HEAD_INIT(&PyBool_Type) { (1u << _PyLong_NON_SIZE_BITS) | 0, { 1 } } };
PyLongObject _Py_FalseStruct = { PyObject_HEAD_INIT(&PyBool_Type) { 1, { 0 } } };

PyObject *PyBool_FromLong(long v) { return v ? Py_True : Py_False; }

static PyObject *bool_repr(PyObject *o) { return PyUnicode_FromString(o == Py_True ? "True" : "False"); }
static PyObject *bool_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PIPER_UNUSED(tp);
    if (kwds && PyDict_Size(kwds)) { PyErr_SetString(PyExc_TypeError, "bool() takes no keyword arguments"); return NULL; }
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 1) { PyErr_Format(PyExc_TypeError, "bool expected at most 1 argument, got %zd", n); return NULL; }
    if (n == 0) Py_RETURN_FALSE;
    int r = PyObject_IsTrue(PyTuple_GET_ITEM(args, 0));
    if (r < 0) return NULL;
    Py_RETURN_BOOL(r);
}
static PyObject *bool_and(PyObject *a, PyObject *b) { if (!PyBool_Check(a) || !PyBool_Check(b)) return long_nb_and(a, b); Py_RETURN_BOOL(a == Py_True && b == Py_True); }
static PyObject *bool_or(PyObject *a, PyObject *b) { if (!PyBool_Check(a) || !PyBool_Check(b)) return long_nb_or(a, b); Py_RETURN_BOOL(a == Py_True || b == Py_True); }
static PyObject *bool_xor(PyObject *a, PyObject *b) { if (!PyBool_Check(a) || !PyBool_Check(b)) return long_nb_xor(a, b); Py_RETURN_BOOL((a == Py_True) != (b == Py_True)); }
static PyObject *bool_invert(PyObject *a) { return long_nb_invert(a); }

static PyNumberMethods bool_as_number = { .nb_and = bool_and, .nb_or = bool_or, .nb_xor = bool_xor, .nb_invert = bool_invert };

PyTypeObject PyBool_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "bool",
    .tp_basicsize = offsetof(PyLongObject, long_value.ob_digit),
    .tp_itemsize = sizeof(digit),
    .tp_repr = bool_repr,
    .tp_as_number = &bool_as_number,
    .tp_flags = Py_TPFLAGS_LONG_SUBCLASS,
    .tp_base = &PyLong_Type,
    .tp_new = bool_new,
};

void piper_init_int(void) {
    for (int i = 0; i < PIPER_NSMALLNEGINTS + PIPER_NSMALLPOSINTS; i++) {
        PyLongObject *v = &piper_small_ints[i];
        int val = i - PIPER_NSMALLNEGINTS;
        v->ob_base.ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT;
        v->ob_base.ob_type = &PyLong_Type;
        v->long_value.ob_digit[0] = (digit)(val < 0 ? -val : val);
        set_tag(v, val > 0 ? 1 : val == 0 ? 0 : -1, val == 0 ? 0 : 1);
    }
}
