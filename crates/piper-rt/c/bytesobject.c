/* bytes and bytearray. */
#include "internal.h"

PyObject *PyBytes_FromStringAndSize(const char *s, Py_ssize_t n) {
    if (n < 0) { PyErr_SetString(PyExc_SystemError, "Negative size passed to PyBytes_FromStringAndSize"); return NULL; }
    PyBytesObject *b = PyObject_Malloc(offsetof(PyBytesObject, ob_sval) + (size_t)n + 1);
    if (!b) return PyErr_NoMemory();
    PyObject_InitVar((PyVarObject *)b, &PyBytes_Type, n);
    b->ob_hash = -1;
    if (s) memcpy(b->ob_sval, s, (size_t)n);
    b->ob_sval[n] = 0;
    return (PyObject *)b;
}
PyObject *PyBytes_FromString(const char *s) { return PyBytes_FromStringAndSize(s, (Py_ssize_t)strlen(s)); }
PyObject *piper_bytes_const(const char *data, Py_ssize_t n) { return PyBytes_FromStringAndSize(data, n); }
char *PyBytes_AsString(PyObject *o) { if (!PyBytes_Check(o)) { PyErr_Format(PyExc_TypeError, "expected bytes, %s found", Py_TYPE(o)->tp_name); return NULL; } return PyBytes_AS_STRING(o); }
Py_ssize_t PyBytes_Size(PyObject *o) { if (!PyBytes_Check(o)) { PyErr_Format(PyExc_TypeError, "expected bytes, %s found", Py_TYPE(o)->tp_name); return -1; } return Py_SIZE(o); }
int PyBytes_AsStringAndSize(PyObject *o, char **s, Py_ssize_t *n) {
    if (!PyBytes_Check(o)) { PyErr_Format(PyExc_TypeError, "expected bytes, %s found", Py_TYPE(o)->tp_name); return -1; }
    *s = PyBytes_AS_STRING(o);
    if (n) *n = Py_SIZE(o);
    else if ((Py_ssize_t)strlen(*s) != Py_SIZE(o)) { PyErr_SetString(PyExc_ValueError, "embedded null byte"); return -1; }
    return 0;
}
int _PyBytes_Resize(PyObject **o, Py_ssize_t n) {
    PyBytesObject *b = (PyBytesObject *)*o;
    if (Py_REFCNT(b) != 1 && !_Py_IsImmortal(*o)) { PyObject *nb = PyBytes_FromStringAndSize(b->ob_sval, n < Py_SIZE(b) ? n : Py_SIZE(b)); Py_DECREF(*o); *o = nb; return nb ? 0 : -1; }
    PyBytesObject *nb = PyObject_Realloc(b, offsetof(PyBytesObject, ob_sval) + (size_t)n + 1);
    if (!nb) { PyErr_NoMemory(); return -1; }
    Py_SET_SIZE(nb, n);
    nb->ob_sval[n] = 0;
    nb->ob_hash = -1;
    *o = (PyObject *)nb;
    return 0;
}
void PyBytes_Concat(PyObject **a, PyObject *b) {
    if (!*a) return;
    if (!b) { Py_CLEAR(*a); return; }
    PyObject *r = PyNumber_Add(*a, b);
    Py_DECREF(*a);
    *a = r;
}
void PyBytes_ConcatAndDel(PyObject **a, PyObject *b) { PyBytes_Concat(a, b); Py_XDECREF(b); }

PyObject *PyBytes_FromFormat(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, va);
    va_end(va);
    return PyBytes_FromString(buf);
}

PyObject *PyBytes_FromObject(PyObject *o) {
    if (PyBytes_CheckExact(o)) return Py_NewRef(o);
    if (PyObject_CheckBuffer(o)) {
        Py_buffer v;
        if (PyObject_GetBuffer(o, &v, PyBUF_SIMPLE) < 0) return NULL;
        PyObject *r = PyBytes_FromStringAndSize(v.buf, v.len);
        PyBuffer_Release(&v);
        return r;
    }
    if (PyUnicode_Check(o)) { PyErr_SetString(PyExc_TypeError, "cannot convert 'str' object to bytes"); return NULL; }
    PyObject *it = PyObject_GetIter(o);
    if (!it) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "cannot convert '%s' object to bytes", Py_TYPE(o)->tp_name); return NULL; }
    PyObject *l = piper_list_from_iterable(it);
    Py_DECREF(it);
    if (!l) return NULL;
    Py_ssize_t n = PyList_GET_SIZE(l);
    PyObject *r = PyBytes_FromStringAndSize(NULL, n);
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_ssize_t v = PyNumber_AsSsize_t(PyList_GET_ITEM(l, i), NULL);
        if (v == -1 && PyErr_Occurred()) { Py_DECREF(l); Py_DECREF(r); return NULL; }
        if (v < 0 || v > 255) { Py_DECREF(l); Py_DECREF(r); PyErr_SetString(PyExc_ValueError, "bytes must be in range(0, 256)"); return NULL; }
        PyBytes_AS_STRING(r)[i] = (char)v;
    }
    Py_DECREF(l);
    return r;
}

PyObject *PyBytes_Repr(PyObject *o, int smartquotes) {
    const unsigned char *s = (const unsigned char *)PyBytes_AS_STRING(o);
    Py_ssize_t n = Py_SIZE(o);
    int sq = 0, dq = 0;
    for (Py_ssize_t i = 0; i < n; i++) { if (s[i] == '\'') sq = 1; else if (s[i] == '"') dq = 1; }
    char quote = (smartquotes && sq && !dq) ? '"' : '\'';
    char *buf = PyMem_Malloc((size_t)n * 4 + 4);
    Py_ssize_t j = 0;
    buf[j++] = 'b'; buf[j++] = quote;
    for (Py_ssize_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == quote || c == '\\') { buf[j++] = '\\'; buf[j++] = (char)c; }
        else if (c == '\t') { buf[j++] = '\\'; buf[j++] = 't'; }
        else if (c == '\n') { buf[j++] = '\\'; buf[j++] = 'n'; }
        else if (c == '\r') { buf[j++] = '\\'; buf[j++] = 'r'; }
        else if (c < ' ' || c >= 0x7f) { j += snprintf(buf + j, 5, "\\x%02x", c); }
        else buf[j++] = (char)c;
    }
    buf[j++] = quote;
    PyObject *r = PyUnicode_FromStringAndSize(buf, j);
    PyMem_Free(buf);
    return r;
}
static PyObject *bytes_repr(PyObject *o) { return PyBytes_Repr(o, 1); }

static Py_hash_t bytes_hash(PyObject *o) {
    PyBytesObject *b = (PyBytesObject *)o;
    if (b->ob_hash == -1) b->ob_hash = piper_hash_bytes(b->ob_sval, Py_SIZE(b));
    return b->ob_hash;
}

static PyObject *bytes_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PyBytes_Check(a) || !PyBytes_Check(b)) Py_RETURN_NOTIMPLEMENTED;
    Py_ssize_t na = Py_SIZE(a), nb = Py_SIZE(b);
    int c = memcmp(PyBytes_AS_STRING(a), PyBytes_AS_STRING(b), (size_t)(na < nb ? na : nb));
    if (c == 0) c = na < nb ? -1 : na > nb;
    int r;
    switch (op) { case Py_LT: r = c < 0; break; case Py_LE: r = c <= 0; break; case Py_EQ: r = c == 0; break; case Py_NE: r = c != 0; break; case Py_GT: r = c > 0; break; default: r = c >= 0; }
    Py_RETURN_BOOL(r);
}

static Py_ssize_t bytes_length(PyObject *o) { return Py_SIZE(o); }
static PyObject *bytes_item(PyObject *o, Py_ssize_t i) {
    if (i < 0) i += Py_SIZE(o);
    if (i < 0 || i >= Py_SIZE(o)) { PyErr_SetString(PyExc_IndexError, "index out of range"); return NULL; }
    return PyLong_FromLong((unsigned char)PyBytes_AS_STRING(o)[i]);
}
static PyObject *bytes_subscript(PyObject *o, PyObject *item) {
    if (PyIndex_Check(item)) { Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError); if (i == -1 && PyErr_Occurred()) return NULL; return bytes_item(o, i); }
    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(item, Py_SIZE(o), &start, &stop, &step, &len) < 0) return NULL;
        if (step == 1) return PyBytes_FromStringAndSize(PyBytes_AS_STRING(o) + start, len);
        PyObject *r = PyBytes_FromStringAndSize(NULL, len);
        for (Py_ssize_t i = 0, cur = start; i < len; i++, cur += step) PyBytes_AS_STRING(r)[i] = PyBytes_AS_STRING(o)[cur];
        return r;
    }
    PyErr_Format(PyExc_TypeError, "byte indices must be integers or slices, not %s", Py_TYPE(item)->tp_name);
    return NULL;
}
static PyObject *bytes_concat(PyObject *a, PyObject *b) {
    Py_buffer va, vb;
    if (PyObject_GetBuffer(a, &va, PyBUF_SIMPLE) < 0) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "can't concat %s to %s", Py_TYPE(b)->tp_name, Py_TYPE(a)->tp_name); return NULL; }
    if (PyObject_GetBuffer(b, &vb, PyBUF_SIMPLE) < 0) { PyBuffer_Release(&va); PyErr_Clear(); PyErr_Format(PyExc_TypeError, "can't concat %s to %s", Py_TYPE(b)->tp_name, Py_TYPE(a)->tp_name); return NULL; }
    PyObject *r = PyBytes_FromStringAndSize(NULL, va.len + vb.len);
    if (r) { memcpy(PyBytes_AS_STRING(r), va.buf, (size_t)va.len); memcpy(PyBytes_AS_STRING(r) + va.len, vb.buf, (size_t)vb.len); }
    PyBuffer_Release(&va); PyBuffer_Release(&vb);
    return r;
}
static PyObject *bytes_repeat(PyObject *a, Py_ssize_t n) {
    if (n < 0) n = 0;
    Py_ssize_t len = Py_SIZE(a);
    PyObject *r = PyBytes_FromStringAndSize(NULL, len * n);
    if (!r) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) memcpy(PyBytes_AS_STRING(r) + i * len, PyBytes_AS_STRING(a), (size_t)len);
    return r;
}
static Py_ssize_t bytes_find_raw(const char *s, Py_ssize_t n, const char *sub, Py_ssize_t m, Py_ssize_t start, int dir) {
    if (m == 0) return start <= n ? start : -1;
    if (dir > 0) { for (Py_ssize_t i = start; i + m <= n; i++) if (!memcmp(s + i, sub, (size_t)m)) return i; }
    else { for (Py_ssize_t i = n - m; i >= start; i--) if (!memcmp(s + i, sub, (size_t)m)) return i; }
    return -1;
}
static int bytes_contains(PyObject *o, PyObject *arg) {
    if (PyIndex_Check(arg)) {
        Py_ssize_t v = PyNumber_AsSsize_t(arg, NULL);
        if (v == -1 && PyErr_Occurred()) return -1;
        if (v < 0 || v > 255) { PyErr_SetString(PyExc_ValueError, "byte must be in range(0, 256)"); return -1; }
        return memchr(PyBytes_AS_STRING(o), (int)v, (size_t)Py_SIZE(o)) != NULL;
    }
    Py_buffer v;
    if (PyObject_GetBuffer(arg, &v, PyBUF_SIMPLE) < 0) return -1;
    int r = bytes_find_raw(PyBytes_AS_STRING(o), Py_SIZE(o), v.buf, v.len, 0, 1) >= 0;
    PyBuffer_Release(&v);
    return r;
}
static int bytes_getbuffer(PyObject *o, Py_buffer *view, int flags) { return PyBuffer_FillInfo(view, o, PyBytes_AS_STRING(o), Py_SIZE(o), 1, flags); }

static PyObject *bytes_decode(PyObject *o, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    const char *enc = "utf-8", *errors = "strict";
    if (n >= 1) { if (!PyUnicode_Check(a[0])) { PyErr_SetString(PyExc_TypeError, "decode() argument 'encoding' must be str"); return NULL; } enc = PyUnicode_AsUTF8(a[0]); }
    if (n >= 2) { if (!PyUnicode_Check(a[1])) { PyErr_SetString(PyExc_TypeError, "decode() argument 'errors' must be str"); return NULL; } errors = PyUnicode_AsUTF8(a[1]); }
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kw, i);
        if (PyUnicode_EqualToUTF8(k, "encoding")) enc = PyUnicode_AsUTF8(a[n + i]);
        else if (PyUnicode_EqualToUTF8(k, "errors")) errors = PyUnicode_AsUTF8(a[n + i]);
        else { PyErr_Format(PyExc_TypeError, "decode() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    return PyUnicode_Decode(PyBytes_AS_STRING(o), Py_SIZE(o), enc, errors);
}
static PyObject *bytes_hex(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(a); PIPER_UNUSED(n);
    Py_ssize_t len = Py_SIZE(o);
    PyObject *r = PyUnicode_New(len * 2, 0x7f);
    char *d = PyUnicode_DATA(r);
    for (Py_ssize_t i = 0; i < len; i++) { unsigned char c = (unsigned char)PyBytes_AS_STRING(o)[i]; d[2 * i] = "0123456789abcdef"[c >> 4]; d[2 * i + 1] = "0123456789abcdef"[c & 15]; }
    return r;
}
static PyObject *bytes_fromhex(PyObject *cls, PyObject *s) {
    if (!PyUnicode_Check(s)) { PyErr_SetString(PyExc_TypeError, "fromhex() argument must be str"); return NULL; }
    const char *p = PyUnicode_AsUTF8(s);
    PyObject *r = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)strlen(p) / 2 + 1);
    Py_ssize_t j = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int hi = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
        int lo = p[1] >= '0' && p[1] <= '9' ? p[1] - '0' : (p[1] | 32) >= 'a' && (p[1] | 32) <= 'f' ? (p[1] | 32) - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) { Py_DECREF(r); PyErr_SetString(PyExc_ValueError, "non-hexadecimal number found in fromhex() arg"); return NULL; }
        PyBytes_AS_STRING(r)[j++] = (char)(hi * 16 + lo);
        p += 2;
    }
    _PyBytes_Resize(&r, j);
    if ((PyTypeObject *)cls != &PyBytes_Type) { PyObject *t = PyObject_CallOneArg(cls, r); Py_DECREF(r); r = t; }
    return r;
}
static PyObject *bytes_find_impl(PyObject *o, PyObject *const *a, Py_ssize_t n, int dir, int raise) {
    if (n < 1) { PyErr_SetString(PyExc_TypeError, "find() takes at least 1 argument"); return NULL; }
    Py_ssize_t start = 0, end = Py_SIZE(o);
    if (n >= 2 && a[1] != Py_None) { start = PyNumber_AsSsize_t(a[1], NULL); if (start == -1 && PyErr_Occurred()) return NULL; if (start < 0) { start += Py_SIZE(o); if (start < 0) start = 0; } }
    if (n >= 3 && a[2] != Py_None) { end = PyNumber_AsSsize_t(a[2], NULL); if (end == -1 && PyErr_Occurred()) return NULL; if (end < 0) { end += Py_SIZE(o); if (end < 0) end = 0; } if (end > Py_SIZE(o)) end = Py_SIZE(o); }
    Py_ssize_t r;
    if (PyIndex_Check(a[0])) {
        Py_ssize_t v = PyNumber_AsSsize_t(a[0], NULL);
        if (v == -1 && PyErr_Occurred()) return NULL;
        char c = (char)v;
        r = bytes_find_raw(PyBytes_AS_STRING(o), end, &c, 1, start, dir);
    } else {
        Py_buffer v;
        if (PyObject_GetBuffer(a[0], &v, PyBUF_SIMPLE) < 0) return NULL;
        r = bytes_find_raw(PyBytes_AS_STRING(o), end, v.buf, v.len, start, dir);
        PyBuffer_Release(&v);
    }
    if (r < 0 && raise) { PyErr_SetString(PyExc_ValueError, "subsection not found"); return NULL; }
    return PyLong_FromSsize_t(r);
}
static PyObject *bytes_find(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_find_impl(o, a, n, 1, 0); }
static PyObject *bytes_rfind(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_find_impl(o, a, n, -1, 0); }
static PyObject *bytes_index(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_find_impl(o, a, n, 1, 1); }
static PyObject *bytes_tailmatch(PyObject *o, PyObject *const *a, Py_ssize_t n, int dir) {
    if (n < 1) { PyErr_SetString(PyExc_TypeError, "takes at least 1 argument"); return NULL; }
    PyObject *subs = a[0];
    Py_ssize_t len = Py_SIZE(o);
    PyObject *tuple = PyTuple_Check(subs) ? subs : NULL;
    Py_ssize_t count = tuple ? PyTuple_GET_SIZE(tuple) : 1;
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *sub = tuple ? PyTuple_GET_ITEM(tuple, i) : subs;
        Py_buffer v;
        if (PyObject_GetBuffer(sub, &v, PyBUF_SIMPLE) < 0) return NULL;
        int ok = v.len <= len && !memcmp(dir > 0 ? PyBytes_AS_STRING(o) + len - v.len : PyBytes_AS_STRING(o), v.buf, (size_t)v.len);
        PyBuffer_Release(&v);
        if (ok) Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}
static PyObject *bytes_startswith(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_tailmatch(o, a, n, -1); }
static PyObject *bytes_endswith(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_tailmatch(o, a, n, 1); }
static PyObject *bytes_join(PyObject *o, PyObject *iterable) { return PyBytes_Join(o, iterable); }
PyObject *PyBytes_Join(PyObject *sep, PyObject *iterable) {
    PyObject *fast = PySequence_Fast(iterable, "can only join an iterable");
    if (!fast) return NULL;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast), total = 0, seplen = Py_SIZE(sep);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *it = PySequence_Fast_GET_ITEM(fast, i);
        if (!PyObject_CheckBuffer(it)) { Py_DECREF(fast); PyErr_Format(PyExc_TypeError, "sequence item %zd: expected a bytes-like object, %s found", i, Py_TYPE(it)->tp_name); return NULL; }
        Py_buffer v; PyObject_GetBuffer(it, &v, PyBUF_SIMPLE); total += v.len; PyBuffer_Release(&v);
    }
    if (n) total += seplen * (n - 1);
    PyObject *r = PyBytes_FromStringAndSize(NULL, total);
    char *d = PyBytes_AS_STRING(r);
    for (Py_ssize_t i = 0; i < n; i++) {
        if (i) { memcpy(d, PyBytes_AS_STRING(sep), (size_t)seplen); d += seplen; }
        Py_buffer v; PyObject_GetBuffer(PySequence_Fast_GET_ITEM(fast, i), &v, PyBUF_SIMPLE); memcpy(d, v.buf, (size_t)v.len); d += v.len; PyBuffer_Release(&v);
    }
    Py_DECREF(fast);
    return r;
}
static PyObject *bytes_strip_impl(PyObject *o, PyObject *const *a, Py_ssize_t n, int left, int right) {
    const char *s = PyBytes_AS_STRING(o);
    Py_ssize_t len = Py_SIZE(o), i = 0, j = len;
    const char *chars = " \t\n\r\x0b\x0c"; Py_ssize_t nchars = 6;
    Py_buffer v = { 0 };
    if (n >= 1 && a[0] != Py_None) { if (PyObject_GetBuffer(a[0], &v, PyBUF_SIMPLE) < 0) return NULL; chars = v.buf; nchars = v.len; }
    if (left) while (i < len && memchr(chars, s[i], (size_t)nchars)) i++;
    if (right) while (j > i && memchr(chars, s[j - 1], (size_t)nchars)) j--;
    if (v.obj) PyBuffer_Release(&v);
    return PyBytes_FromStringAndSize(s + i, j - i);
}
static PyObject *bytes_strip(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_strip_impl(o, a, n, 1, 1); }
static PyObject *bytes_lstrip(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_strip_impl(o, a, n, 1, 0); }
static PyObject *bytes_rstrip(PyObject *o, PyObject *const *a, Py_ssize_t n) { return bytes_strip_impl(o, a, n, 0, 1); }
static PyObject *bytes_split(PyObject *o, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PIPER_UNUSED(kw);
    PyObject *list = PyList_New(0);
    const char *s = PyBytes_AS_STRING(o);
    Py_ssize_t len = Py_SIZE(o), maxsplit = -1;
    if (n >= 2) { maxsplit = PyNumber_AsSsize_t(a[1], NULL); if (maxsplit == -1 && PyErr_Occurred()) return NULL; }
    if (maxsplit < 0) maxsplit = PY_SSIZE_T_MAX;
    if (n == 0 || a[0] == Py_None) {
        Py_ssize_t i = 0;
        while (i < len) {
            while (i < len && strchr(" \t\n\r\x0b\x0c", s[i]) && s[i]) i++;
            if (i >= len) break;
            Py_ssize_t j = i;
            if (maxsplit == 0) { j = len; while (j > i && strchr(" \t\n\r\x0b\x0c", s[j - 1]) && s[j - 1]) j--; }
            else while (j < len && !(strchr(" \t\n\r\x0b\x0c", s[j]) && s[j])) j++;
            PyObject *p = PyBytes_FromStringAndSize(s + i, j - i); PyList_Append(list, p); Py_DECREF(p);
            maxsplit--; i = j;
        }
        return list;
    }
    Py_buffer v;
    if (PyObject_GetBuffer(a[0], &v, PyBUF_SIMPLE) < 0) { Py_DECREF(list); return NULL; }
    if (v.len == 0) { PyBuffer_Release(&v); Py_DECREF(list); PyErr_SetString(PyExc_ValueError, "empty separator"); return NULL; }
    Py_ssize_t i = 0;
    while (maxsplit-- > 0) {
        Py_ssize_t f = bytes_find_raw(s, len, v.buf, v.len, i, 1);
        if (f < 0) break;
        PyObject *p = PyBytes_FromStringAndSize(s + i, f - i); PyList_Append(list, p); Py_DECREF(p);
        i = f + v.len;
    }
    PyObject *p = PyBytes_FromStringAndSize(s + i, len - i); PyList_Append(list, p); Py_DECREF(p);
    PyBuffer_Release(&v);
    return list;
}
static PyObject *bytes_replace(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (n < 2) { PyErr_SetString(PyExc_TypeError, "replace expected at least 2 arguments"); return NULL; }
    Py_buffer va, vb;
    if (PyObject_GetBuffer(a[0], &va, PyBUF_SIMPLE) < 0) return NULL;
    if (PyObject_GetBuffer(a[1], &vb, PyBUF_SIMPLE) < 0) { PyBuffer_Release(&va); return NULL; }
    Py_ssize_t count = n >= 3 ? PyNumber_AsSsize_t(a[2], NULL) : -1;
    if (count < 0) count = PY_SSIZE_T_MAX;
    const char *s = PyBytes_AS_STRING(o);
    Py_ssize_t len = Py_SIZE(o);
    PyObject *parts = PyList_New(0);
    Py_ssize_t i = 0;
    if (va.len == 0) {
        while (i <= len && count-- > 0) { PyObject *p = PyBytes_FromStringAndSize(vb.buf, vb.len); PyList_Append(parts, p); Py_DECREF(p); if (i < len) { p = PyBytes_FromStringAndSize(s + i, 1); PyList_Append(parts, p); Py_DECREF(p); } i++; }
        if (i <= len) { PyObject *p = PyBytes_FromStringAndSize(s + i, len - i); PyList_Append(parts, p); Py_DECREF(p); }
    } else {
        while (count-- > 0) {
            Py_ssize_t f = bytes_find_raw(s, len, va.buf, va.len, i, 1);
            if (f < 0) break;
            PyObject *p = PyBytes_FromStringAndSize(s + i, f - i); PyList_Append(parts, p); Py_DECREF(p);
            p = PyBytes_FromStringAndSize(vb.buf, vb.len); PyList_Append(parts, p); Py_DECREF(p);
            i = f + va.len;
        }
        PyObject *p = PyBytes_FromStringAndSize(s + i, len - i); PyList_Append(parts, p); Py_DECREF(p);
    }
    PyBuffer_Release(&va); PyBuffer_Release(&vb);
    PyObject *empty = PyBytes_FromStringAndSize("", 0);
    PyObject *r = PyBytes_Join(empty, parts);
    Py_DECREF(empty); Py_DECREF(parts);
    return r;
}
static PyObject *bytes_count(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (n < 1) { PyErr_SetString(PyExc_TypeError, "count() takes at least 1 argument"); return NULL; }
    Py_buffer v;
    if (PyObject_GetBuffer(a[0], &v, PyBUF_SIMPLE) < 0) return NULL;
    Py_ssize_t c = 0, i = 0, len = Py_SIZE(o);
    if (v.len == 0) c = len + 1;
    else while ((i = bytes_find_raw(PyBytes_AS_STRING(o), len, v.buf, v.len, i, 1)) >= 0) { c++; i += v.len; }
    PyBuffer_Release(&v);
    return PyLong_FromSsize_t(c);
}
static PyObject *bytes_upper(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyObject *r = PyBytes_FromStringAndSize(PyBytes_AS_STRING(o), Py_SIZE(o)); for (Py_ssize_t i = 0; i < Py_SIZE(r); i++) { char *c = &PyBytes_AS_STRING(r)[i]; if (*c >= 'a' && *c <= 'z') *c -= 32; } return r; }
static PyObject *bytes_lower(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyObject *r = PyBytes_FromStringAndSize(PyBytes_AS_STRING(o), Py_SIZE(o)); for (Py_ssize_t i = 0; i < Py_SIZE(r); i++) { char *c = &PyBytes_AS_STRING(r)[i]; if (*c >= 'A' && *c <= 'Z') *c += 32; } return r; }
static PyObject *bytes_isdigit(PyObject *o, PyObject *u) { PIPER_UNUSED(u); if (!Py_SIZE(o)) Py_RETURN_FALSE; for (Py_ssize_t i = 0; i < Py_SIZE(o); i++) { char c = PyBytes_AS_STRING(o)[i]; if (c < '0' || c > '9') Py_RETURN_FALSE; } Py_RETURN_TRUE; }
static PyObject *bytes_isalpha(PyObject *o, PyObject *u) { PIPER_UNUSED(u); if (!Py_SIZE(o)) Py_RETURN_FALSE; for (Py_ssize_t i = 0; i < Py_SIZE(o); i++) { char c = (char)(PyBytes_AS_STRING(o)[i] | 32); if (c < 'a' || c > 'z') Py_RETURN_FALSE; } Py_RETURN_TRUE; }
static PyObject *bytes_isspace(PyObject *o, PyObject *u) { PIPER_UNUSED(u); if (!Py_SIZE(o)) Py_RETURN_FALSE; for (Py_ssize_t i = 0; i < Py_SIZE(o); i++) { char c = PyBytes_AS_STRING(o)[i]; if (!(c == ' ' || (c >= 9 && c <= 13))) Py_RETURN_FALSE; } Py_RETURN_TRUE; }
static PyObject *bytes_getnewargs(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyObject *b = PyBytes_FromStringAndSize(PyBytes_AS_STRING(o), Py_SIZE(o)); PyObject *t = PyTuple_Pack(1, b); Py_DECREF(b); return t; }
static PyObject *bytes_mod(PyObject *o, PyObject *args) {
    if (!PyBytes_Check(o)) Py_RETURN_NOTIMPLEMENTED;
    PyObject *s = PyUnicode_DecodeLatin1(PyBytes_AS_STRING(o), Py_SIZE(o), NULL);
    if (!s) return NULL;
    PyObject *r = PyUnicode_Format(s, args);
    Py_DECREF(s);
    if (!r) return NULL;
    PyObject *b = PyUnicode_AsLatin1String(r);
    Py_DECREF(r);
    return b;
}

static PyMethodDef bytes_methods[] = {
    { "decode", (PyCFunction)(void (*)(void))bytes_decode, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "hex", (PyCFunction)(void (*)(void))bytes_hex, METH_FASTCALL, NULL },
    { "fromhex", bytes_fromhex, METH_O | METH_CLASS, NULL },
    { "find", (PyCFunction)(void (*)(void))bytes_find, METH_FASTCALL, NULL },
    { "rfind", (PyCFunction)(void (*)(void))bytes_rfind, METH_FASTCALL, NULL },
    { "index", (PyCFunction)(void (*)(void))bytes_index, METH_FASTCALL, NULL },
    { "startswith", (PyCFunction)(void (*)(void))bytes_startswith, METH_FASTCALL, NULL },
    { "endswith", (PyCFunction)(void (*)(void))bytes_endswith, METH_FASTCALL, NULL },
    { "join", bytes_join, METH_O, NULL },
    { "strip", (PyCFunction)(void (*)(void))bytes_strip, METH_FASTCALL, NULL },
    { "lstrip", (PyCFunction)(void (*)(void))bytes_lstrip, METH_FASTCALL, NULL },
    { "rstrip", (PyCFunction)(void (*)(void))bytes_rstrip, METH_FASTCALL, NULL },
    { "split", (PyCFunction)(void (*)(void))bytes_split, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "replace", (PyCFunction)(void (*)(void))bytes_replace, METH_FASTCALL, NULL },
    { "count", (PyCFunction)(void (*)(void))bytes_count, METH_FASTCALL, NULL },
    { "upper", bytes_upper, METH_NOARGS, NULL }, { "lower", bytes_lower, METH_NOARGS, NULL },
    { "isdigit", bytes_isdigit, METH_NOARGS, NULL }, { "isalpha", bytes_isalpha, METH_NOARGS, NULL }, { "isspace", bytes_isspace, METH_NOARGS, NULL },
    { "__getnewargs__", bytes_getnewargs, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyObject *bytes_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    PyObject *r;
    if (n == 0) r = PyBytes_FromStringAndSize("", 0);
    else {
        PyObject *x = PyTuple_GET_ITEM(args, 0);
        PyObject *enc = n >= 2 ? PyTuple_GET_ITEM(args, 1) : (kwds ? PyDict_GetItemString(kwds, "encoding") : NULL);
        PyObject *errors = n >= 3 ? PyTuple_GET_ITEM(args, 2) : (kwds ? PyDict_GetItemString(kwds, "errors") : NULL);
        if (PyUnicode_Check(x)) {
            if (!enc) { PyErr_SetString(PyExc_TypeError, "string argument without an encoding"); return NULL; }
            r = PyUnicode_AsEncodedString(x, PyUnicode_AsUTF8(enc), errors ? PyUnicode_AsUTF8(errors) : "strict");
        } else if (enc) { PyErr_SetString(PyExc_TypeError, "encoding without a string argument"); return NULL; }
        else if (PyIndex_Check(x)) {
            Py_ssize_t size = PyNumber_AsSsize_t(x, PyExc_OverflowError);
            if (size == -1 && PyErr_Occurred()) return NULL;
            if (size < 0) { PyErr_SetString(PyExc_ValueError, "negative count"); return NULL; }
            r = PyBytes_FromStringAndSize(NULL, size);
            if (r) memset(PyBytes_AS_STRING(r), 0, (size_t)size);
        } else r = PyObject_Bytes(x);
    }
    if (!r || tp == &PyBytes_Type) return r;
    PyObject *sub = tp->tp_alloc(tp, Py_SIZE(r));
    if (sub) { memcpy(PyBytes_AS_STRING(sub), PyBytes_AS_STRING(r), (size_t)Py_SIZE(r) + 1); ((PyBytesObject *)sub)->ob_hash = -1; }
    Py_DECREF(r);
    return sub;
}

static PySequenceMethods bytes_as_sequence = { .sq_length = bytes_length, .sq_concat = bytes_concat, .sq_repeat = bytes_repeat, .sq_item = bytes_item, .sq_contains = bytes_contains };
static PyMappingMethods bytes_as_mapping = { .mp_length = bytes_length, .mp_subscript = bytes_subscript };
static PyBufferProcs bytes_as_buffer = { .bf_getbuffer = bytes_getbuffer };
static PyNumberMethods bytes_as_number = { .nb_remainder = bytes_mod };
static void bytes_dealloc(PyObject *o) { Py_TYPE(o)->tp_free(o); }

typedef struct { PyObject_HEAD PyObject *seq; Py_ssize_t index; } bytesiterobject;
static void bytesiter_dealloc(PyObject *o) { Py_XDECREF(((bytesiterobject *)o)->seq); PyObject_Free(o); }
static PyObject *bytesiter_next(PyObject *o) {
    bytesiterobject *it = (bytesiterobject *)o;
    if (!it->seq) return NULL;
    if (it->index < Py_SIZE(it->seq)) return PyLong_FromLong((unsigned char)PyBytes_AS_STRING(it->seq)[it->index++]);
    Py_CLEAR(it->seq);
    return NULL;
}
PyTypeObject PyBytesIter_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "bytes_iterator", .tp_basicsize = sizeof(bytesiterobject), .tp_dealloc = bytesiter_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = bytesiter_next };
static PyObject *bytes_iter(PyObject *o) { bytesiterobject *it = PyObject_New(bytesiterobject, &PyBytesIter_Type); if (!it) return NULL; it->seq = Py_NewRef(o); it->index = 0; return (PyObject *)it; }

PyTypeObject PyBytes_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "bytes",
    .tp_basicsize = offsetof(PyBytesObject, ob_sval),
    .tp_itemsize = 1,
    .tp_dealloc = bytes_dealloc,
    .tp_repr = bytes_repr,
    .tp_as_number = &bytes_as_number,
    .tp_as_sequence = &bytes_as_sequence,
    .tp_as_mapping = &bytes_as_mapping,
    .tp_hash = bytes_hash,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_as_buffer = &bytes_as_buffer,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_BYTES_SUBCLASS | Py_TPFLAGS_MATCH_SELF,
    .tp_richcompare = bytes_richcompare,
    .tp_iter = bytes_iter,
    .tp_methods = bytes_methods,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = bytes_new,
    .tp_free = PyObject_Free,
};

/* ---- bytearray (minimal, growable) ------------------------------------------ */

typedef struct { PyObject_VAR_HEAD Py_ssize_t ob_alloc; char *ob_bytes; char *ob_start; Py_ssize_t ob_exports; } PyByteArrayObject;

PyObject *piper_bytearray_new(const char *s, Py_ssize_t n) {
    PyByteArrayObject *b = PyObject_New(PyByteArrayObject, &PyByteArray_Type);
    if (!b) return NULL;
    Py_SET_SIZE(b, n);
    b->ob_alloc = n + 1;
    b->ob_bytes = b->ob_start = PyMem_Malloc((size_t)n + 1);
    if (s) memcpy(b->ob_bytes, s, (size_t)n);
    b->ob_bytes[n] = 0;
    b->ob_exports = 0;
    return (PyObject *)b;
}
static void bytearray_dealloc(PyObject *o) { PyMem_Free(((PyByteArrayObject *)o)->ob_bytes); PyObject_Free(o); }
static int bytearray_resize(PyByteArrayObject *b, Py_ssize_t n) {
    if (n + 1 > b->ob_alloc) { Py_ssize_t alloc = n + (n >> 3) + 8; char *nb = PyMem_Realloc(b->ob_bytes, (size_t)alloc); if (!nb) { PyErr_NoMemory(); return -1; } b->ob_bytes = b->ob_start = nb; b->ob_alloc = alloc; }
    Py_SET_SIZE(b, n);
    b->ob_bytes[n] = 0;
    return 0;
}
static PyObject *bytearray_repr(PyObject *o) {
    PyObject *b = PyBytes_FromStringAndSize(((PyByteArrayObject *)o)->ob_bytes, Py_SIZE(o));
    PyObject *r = PyBytes_Repr(b, 1);
    Py_DECREF(b);
    PyObject *res = PyUnicode_FromFormat("bytearray(%U)", r);
    Py_DECREF(r);
    return res;
}
static Py_ssize_t bytearray_length(PyObject *o) { return Py_SIZE(o); }
static PyObject *bytearray_item(PyObject *o, Py_ssize_t i) { if (i < 0) i += Py_SIZE(o); if (i < 0 || i >= Py_SIZE(o)) { PyErr_SetString(PyExc_IndexError, "bytearray index out of range"); return NULL; } return PyLong_FromLong((unsigned char)((PyByteArrayObject *)o)->ob_bytes[i]); }
static int bytearray_ass_item(PyObject *o, Py_ssize_t i, PyObject *v) {
    PyByteArrayObject *b = (PyByteArrayObject *)o;
    if (i < 0) i += Py_SIZE(o);
    if (i < 0 || i >= Py_SIZE(o)) { PyErr_SetString(PyExc_IndexError, "bytearray index out of range"); return -1; }
    if (!v) { memmove(b->ob_bytes + i, b->ob_bytes + i + 1, (size_t)(Py_SIZE(o) - i - 1)); return bytearray_resize(b, Py_SIZE(o) - 1); }
    Py_ssize_t x = PyNumber_AsSsize_t(v, NULL);
    if (x == -1 && PyErr_Occurred()) return -1;
    if (x < 0 || x > 255) { PyErr_SetString(PyExc_ValueError, "byte must be in range(0, 256)"); return -1; }
    b->ob_bytes[i] = (char)x;
    return 0;
}
static PyObject *bytearray_subscript(PyObject *o, PyObject *item) {
    if (PyIndex_Check(item)) { Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError); if (i == -1 && PyErr_Occurred()) return NULL; return bytearray_item(o, i); }
    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(item, Py_SIZE(o), &start, &stop, &step, &len) < 0) return NULL;
        PyObject *r = piper_bytearray_new(NULL, len);
        for (Py_ssize_t i = 0, cur = start; i < len; i++, cur += step) ((PyByteArrayObject *)r)->ob_bytes[i] = ((PyByteArrayObject *)o)->ob_bytes[cur];
        return r;
    }
    PyErr_SetString(PyExc_TypeError, "bytearray indices must be integers or slices");
    return NULL;
}
static int bytearray_getbuffer(PyObject *o, Py_buffer *view, int flags) { PyByteArrayObject *b = (PyByteArrayObject *)o; int r = PyBuffer_FillInfo(view, o, b->ob_bytes, Py_SIZE(o), 0, flags); if (r == 0) b->ob_exports++; return r; }
static void bytearray_releasebuffer(PyObject *o, Py_buffer *view) { PIPER_UNUSED(view); ((PyByteArrayObject *)o)->ob_exports--; }
static PyObject *bytearray_append(PyObject *o, PyObject *v) {
    Py_ssize_t x = PyNumber_AsSsize_t(v, NULL);
    if (x == -1 && PyErr_Occurred()) return NULL;
    if (x < 0 || x > 255) { PyErr_SetString(PyExc_ValueError, "byte must be in range(0, 256)"); return NULL; }
    PyByteArrayObject *b = (PyByteArrayObject *)o;
    Py_ssize_t n = Py_SIZE(o);
    if (bytearray_resize(b, n + 1) < 0) return NULL;
    b->ob_bytes[n] = (char)x;
    Py_RETURN_NONE;
}
static PyObject *bytearray_extend(PyObject *o, PyObject *v) {
    PyObject *bytes = PyObject_Bytes(v);
    if (!bytes) return NULL;
    PyByteArrayObject *b = (PyByteArrayObject *)o;
    Py_ssize_t n = Py_SIZE(o), m = Py_SIZE(bytes);
    if (bytearray_resize(b, n + m) < 0) { Py_DECREF(bytes); return NULL; }
    memcpy(b->ob_bytes + n, PyBytes_AS_STRING(bytes), (size_t)m);
    Py_DECREF(bytes);
    Py_RETURN_NONE;
}
static PyObject *bytearray_decode(PyObject *o, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    PyObject *b = PyBytes_FromStringAndSize(((PyByteArrayObject *)o)->ob_bytes, Py_SIZE(o));
    PyObject *r = bytes_decode(b, a, n, kw);
    Py_DECREF(b);
    return r;
}
static PyObject *bytearray_find(PyObject *o, PyObject *const *a, Py_ssize_t n) {
    if (n < 1) { PyErr_SetString(PyExc_TypeError, "find() takes at least 1 argument"); return NULL; }
    Py_ssize_t start = n >= 2 ? PyNumber_AsSsize_t(a[1], PyExc_OverflowError) : 0;
    Py_ssize_t end = n >= 3 ? PyNumber_AsSsize_t(a[2], PyExc_OverflowError) : Py_SIZE(o);
    if (PyErr_Occurred()) return NULL;
    if (start < 0) { start += Py_SIZE(o); if (start < 0) start = 0; }
    if (end < 0) end += Py_SIZE(o);
    if (end > Py_SIZE(o)) end = Py_SIZE(o);
    if (end < 0) end = 0;
    char one;
    const char *needle;
    Py_ssize_t needle_len;
    Py_buffer view;
    int has_view = 0;
    if (PyIndex_Check(a[0])) {
        Py_ssize_t value = PyNumber_AsSsize_t(a[0], NULL);
        if (value == -1 && PyErr_Occurred()) return NULL;
        if (value < 0 || value > 255) { PyErr_SetString(PyExc_ValueError, "byte must be in range(0, 256)"); return NULL; }
        one = (char)value; needle = &one; needle_len = 1;
    } else {
        if (PyObject_GetBuffer(a[0], &view, PyBUF_SIMPLE) < 0) return NULL;
        needle = view.buf; needle_len = view.len; has_view = 1;
    }
    Py_ssize_t result = bytes_find_raw(((PyByteArrayObject *)o)->ob_bytes, end, needle, needle_len, start, 1);
    if (has_view) PyBuffer_Release(&view);
    return PyLong_FromSsize_t(result);
}
static PyObject *bytearray_iconcat(PyObject *o, PyObject *v) { PyObject *r = bytearray_extend(o, v); if (!r) return NULL; Py_DECREF(r); return Py_NewRef(o); }
static PyObject *bytearray_concat(PyObject *a, PyObject *b) {
    Py_buffer va, vb;
    if (PyObject_GetBuffer(a, &va, PyBUF_SIMPLE) < 0) return NULL;
    if (PyObject_GetBuffer(b, &vb, PyBUF_SIMPLE) < 0) { PyBuffer_Release(&va); return NULL; }
    PyObject *r = piper_bytearray_new(NULL, va.len + vb.len);
    memcpy(((PyByteArrayObject *)r)->ob_bytes, va.buf, (size_t)va.len);
    memcpy(((PyByteArrayObject *)r)->ob_bytes + va.len, vb.buf, (size_t)vb.len);
    PyBuffer_Release(&va); PyBuffer_Release(&vb);
    return r;
}
static PyObject *bytearray_richcompare(PyObject *a, PyObject *b, int op) {
    if (!PyObject_CheckBuffer(a) || !PyObject_CheckBuffer(b)) Py_RETURN_NOTIMPLEMENTED;
    Py_buffer va, vb;
    if (PyObject_GetBuffer(a, &va, PyBUF_SIMPLE) < 0 || PyObject_GetBuffer(b, &vb, PyBUF_SIMPLE) < 0) { PyErr_Clear(); Py_RETURN_NOTIMPLEMENTED; }
    int c = memcmp(va.buf, vb.buf, (size_t)(va.len < vb.len ? va.len : vb.len));
    if (c == 0) c = va.len < vb.len ? -1 : va.len > vb.len;
    PyBuffer_Release(&va); PyBuffer_Release(&vb);
    int r;
    switch (op) { case Py_LT: r = c < 0; break; case Py_LE: r = c <= 0; break; case Py_EQ: r = c == 0; break; case Py_NE: r = c != 0; break; case Py_GT: r = c > 0; break; default: r = c >= 0; }
    Py_RETURN_BOOL(r);
}
static PyObject *bytearray_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PyObject *b = bytes_new(&PyBytes_Type, args, kwds);
    if (!b) return NULL;
    PyObject *r = piper_bytearray_new(PyBytes_AS_STRING(b), Py_SIZE(b));
    Py_DECREF(b);
    if (r && tp != &PyByteArray_Type) { Py_SET_TYPE(r, tp); Py_INCREF(tp); }
    return r;
}
static PyObject *bytearray_iter(PyObject *o) { PyObject *b = PyBytes_FromStringAndSize(((PyByteArrayObject *)o)->ob_bytes, Py_SIZE(o)); PyObject *it = bytes_iter(b); Py_DECREF(b); return it; }
static PyMethodDef bytearray_methods[] = {
    { "append", bytearray_append, METH_O, NULL }, { "extend", bytearray_extend, METH_O, NULL },
    { "find", (PyCFunction)(void (*)(void))bytearray_find, METH_FASTCALL, NULL },
    { "decode", (PyCFunction)(void (*)(void))bytearray_decode, METH_FASTCALL | METH_KEYWORDS, NULL },
    { NULL, NULL, 0, NULL },
};
static PySequenceMethods bytearray_as_sequence = { .sq_length = bytearray_length, .sq_concat = bytearray_concat, .sq_item = bytearray_item, .sq_ass_item = bytearray_ass_item, .sq_inplace_concat = bytearray_iconcat };
static PyMappingMethods bytearray_as_mapping = { .mp_length = bytearray_length, .mp_subscript = bytearray_subscript };
static PyBufferProcs bytearray_as_buffer = { .bf_getbuffer = bytearray_getbuffer, .bf_releasebuffer = bytearray_releasebuffer };
PyTypeObject PyByteArray_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "bytearray", .tp_basicsize = sizeof(PyByteArrayObject), .tp_dealloc = bytearray_dealloc, .tp_repr = bytearray_repr,
    .tp_as_sequence = &bytearray_as_sequence, .tp_as_mapping = &bytearray_as_mapping, .tp_hash = PyObject_HashNotImplemented,
    .tp_getattro = PyObject_GenericGetAttr, .tp_as_buffer = &bytearray_as_buffer, .tp_flags = Py_TPFLAGS_BASETYPE,
    .tp_richcompare = bytearray_richcompare, .tp_iter = bytearray_iter, .tp_methods = bytearray_methods,
    .tp_alloc = PyType_GenericAlloc, .tp_new = bytearray_new, .tp_free = PyObject_Free,
};

/* ---- memoryview (thin) --------------------------------------------------------- */

typedef struct { PyObject_HEAD Py_buffer view; } PyMemoryViewObject;
PyObject *piper_memoryview_new(PyObject *o) {
    PyMemoryViewObject *m = PyObject_New(PyMemoryViewObject, &PyMemoryView_Type);
    if (!m) return NULL;
    if (PyObject_GetBuffer(o, &m->view, PyBUF_FULL_RO) < 0) { m->view.obj = NULL; Py_DECREF(m); return NULL; }
    return (PyObject *)m;
}
static void memoryview_dealloc(PyObject *o) { PyMemoryViewObject *m = (PyMemoryViewObject *)o; if (m->view.obj) PyBuffer_Release(&m->view); PyObject_Free(o); }
static int memoryview_getbuffer(PyObject *o, Py_buffer *view, int flags) { PyMemoryViewObject *m = (PyMemoryViewObject *)o; return PyBuffer_FillInfo(view, o, m->view.buf, m->view.len, m->view.readonly, flags); }
static Py_ssize_t memoryview_length(PyObject *o) { return ((PyMemoryViewObject *)o)->view.len; }
static PyObject *memoryview_item(PyObject *o, Py_ssize_t i) { PyMemoryViewObject *m = (PyMemoryViewObject *)o; if (i < 0) i += m->view.len; if (i < 0 || i >= m->view.len) { PyErr_SetString(PyExc_IndexError, "index out of bounds"); return NULL; } return PyLong_FromLong(((unsigned char *)m->view.buf)[i]); }
static PyObject *memoryview_tobytes(PyObject *o, PyObject *u) { PIPER_UNUSED(u); PyMemoryViewObject *m = (PyMemoryViewObject *)o; return PyBytes_FromStringAndSize(m->view.buf, m->view.len); }
static PyObject *memoryview_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) { PIPER_UNUSED(tp); PIPER_UNUSED(kwds); if (PyTuple_GET_SIZE(args) != 1) { PyErr_SetString(PyExc_TypeError, "memoryview() takes exactly one argument"); return NULL; } return piper_memoryview_new(PyTuple_GET_ITEM(args, 0)); }
static PyMethodDef memoryview_methods[] = { { "tobytes", memoryview_tobytes, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PySequenceMethods memoryview_as_sequence = { .sq_length = memoryview_length, .sq_item = memoryview_item };
static PyBufferProcs memoryview_as_buffer = { .bf_getbuffer = memoryview_getbuffer };
PyTypeObject PyMemoryView_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "memoryview", .tp_basicsize = sizeof(PyMemoryViewObject), .tp_dealloc = memoryview_dealloc,
    .tp_as_sequence = &memoryview_as_sequence, .tp_getattro = PyObject_GenericGetAttr, .tp_as_buffer = &memoryview_as_buffer,
    .tp_methods = memoryview_methods, .tp_alloc = PyType_GenericAlloc, .tp_new = memoryview_new, .tp_free = PyObject_Free,
};
