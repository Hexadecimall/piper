/* PyArg_ParseTuple / Py_BuildValue (the commonly used subset) and helpers. */
#include "internal.h"

int piper_args_range(const char *name, Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max) {
    if (nargs < min) {
        if (min == max) PyErr_Format(PyExc_TypeError, "%s() takes exactly %zd argument%s (%zd given)", name, min, min == 1 ? "" : "s", nargs);
        else PyErr_Format(PyExc_TypeError, "%s() takes at least %zd argument%s (%zd given)", name, min, min == 1 ? "" : "s", nargs);
        return -1;
    }
    if (nargs > max) {
        if (min == max) PyErr_Format(PyExc_TypeError, "%s() takes exactly %zd argument%s (%zd given)", name, max, max == 1 ? "" : "s", nargs);
        else PyErr_Format(PyExc_TypeError, "%s() takes at most %zd argument%s (%zd given)", name, max, max == 1 ? "" : "s", nargs);
        return -1;
    }
    return 0;
}
int piper_no_kwargs(const char *name, PyObject *kwnames) {
    if (kwnames && PyTuple_GET_SIZE(kwnames)) { PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", name); return -1; }
    return 0;
}
Py_ssize_t piper_kwarg_index(PyObject *kwnames, const char *name) {
    if (!kwnames) return -1;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) if (PyUnicode_EqualToUTF8(PyTuple_GET_ITEM(kwnames, i), name)) return i;
    return -1;
}
int _PyArg_NoKeywords(const char *name, PyObject *kw) { if (kw && PyDict_Size(kw)) { PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", name); return 0; } return 1; }
int _PyArg_NoPositional(const char *name, PyObject *args) { if (args && PyTuple_GET_SIZE(args)) { PyErr_Format(PyExc_TypeError, "%s() takes no positional arguments", name); return 0; } return 1; }
int _PyArg_CheckPositional(const char *name, Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max) { return piper_args_range(name, nargs, min, max) == 0; }
int PyArg_ValidateKeywordArguments(PyObject *kw) {
    if (!PyDict_Check(kw)) { PyErr_SetString(PyExc_SystemError, "PyArg_ValidateKeywordArguments: kwargs must be a dict"); return 0; }
    Py_ssize_t pos = 0; PyObject *k;
    while (PyDict_Next(kw, &pos, &k, NULL)) if (!PyUnicode_Check(k)) { PyErr_SetString(PyExc_TypeError, "keywords must be strings"); return 0; }
    return 1;
}

/* Convert one object according to a single format unit. Returns chars consumed from fmt. */
static int convert_one(PyObject *arg, const char *fmt, va_list *va, const char *fname, int argno) {
    char c = fmt[0];
    int consumed = 1;
    switch (c) {
    case 'O': {
        if (fmt[1] == '!') { PyTypeObject *tp = va_arg(*va, PyTypeObject *); PyObject **p = va_arg(*va, PyObject **); consumed = 2; if (!PyObject_TypeCheck(arg, tp)) { PyErr_Format(PyExc_TypeError, "%s() argument %d must be %s, not %s", fname, argno, tp->tp_name, Py_TYPE(arg)->tp_name); return -1; } *p = arg; break; }
        if (fmt[1] == '&') { int (*conv)(PyObject *, void *) = va_arg(*va, int (*)(PyObject *, void *)); void *p = va_arg(*va, void *); consumed = 2; if (!conv(arg, p)) return -1; break; }
        PyObject **p = va_arg(*va, PyObject **); *p = arg; break;
    }
    case 'S': { PyObject **p = va_arg(*va, PyObject **); if (!PyBytes_Check(arg)) { PyErr_Format(PyExc_TypeError, "%s() argument %d must be bytes, not %s", fname, argno, Py_TYPE(arg)->tp_name); return -1; } *p = arg; break; }
    case 'U': { PyObject **p = va_arg(*va, PyObject **); if (!PyUnicode_Check(arg)) { PyErr_Format(PyExc_TypeError, "%s() argument %d must be str, not %s", fname, argno, Py_TYPE(arg)->tp_name); return -1; } *p = arg; break; }
    case 'i': { int *p = va_arg(*va, int *); long v = PyLong_AsLong(arg); if (v == -1 && PyErr_Occurred()) return -1; *p = (int)v; break; }
    case 'b': case 'B': case 'h': case 'H': { void *p = va_arg(*va, void *); long v = PyLong_AsLong(arg); if (v == -1 && PyErr_Occurred()) return -1; if (c == 'b' || c == 'B') *(unsigned char *)p = (unsigned char)v; else *(short *)p = (short)v; break; }
    case 'l': { long *p = va_arg(*va, long *); long v = PyLong_AsLong(arg); if (v == -1 && PyErr_Occurred()) return -1; *p = v; break; }
    case 'I': case 'k': { void *p = va_arg(*va, void *); unsigned long v = PyLong_AsUnsignedLongMask(arg); if (v == (unsigned long)-1 && PyErr_Occurred()) return -1; if (c == 'I') *(unsigned int *)p = (unsigned)v; else *(unsigned long *)p = v; break; }
    case 'L': { long long *p = va_arg(*va, long long *); long long v = PyLong_AsLongLong(arg); if (v == -1 && PyErr_Occurred()) return -1; *p = v; break; }
    case 'K': { unsigned long long *p = va_arg(*va, unsigned long long *); *p = PyLong_AsUnsignedLongLongMask(arg); if (PyErr_Occurred()) return -1; break; }
    case 'n': { Py_ssize_t *p = va_arg(*va, Py_ssize_t *); Py_ssize_t v = PyNumber_AsSsize_t(arg, PyExc_OverflowError); if (v == -1 && PyErr_Occurred()) return -1; *p = v; break; }
    case 'd': { double *p = va_arg(*va, double *); double v = PyFloat_AsDouble(arg); if (v == -1.0 && PyErr_Occurred()) return -1; *p = v; break; }
    case 'f': { float *p = va_arg(*va, float *); double v = PyFloat_AsDouble(arg); if (v == -1.0 && PyErr_Occurred()) return -1; *p = (float)v; break; }
    case 'p': { int *p = va_arg(*va, int *); int v = PyObject_IsTrue(arg); if (v < 0) return -1; *p = v; break; }
    case 'C': { int *p = va_arg(*va, int *); if (!PyUnicode_Check(arg) || PyUnicode_GET_LENGTH(arg) != 1) { PyErr_SetString(PyExc_TypeError, "expected a character"); return -1; } *p = (int)PyUnicode_READ_CHAR(arg, 0); break; }
    case 'c': { char *p = va_arg(*va, char *); if (!PyBytes_Check(arg) || Py_SIZE(arg) != 1) { PyErr_SetString(PyExc_TypeError, "expected a byte string of length 1"); return -1; } *p = PyBytes_AS_STRING(arg)[0]; break; }
    case 's': case 'z': case 'y': {
        int allow_none = c == 'z';
        if (fmt[1] == '*') {
            Py_buffer *view = va_arg(*va, Py_buffer *); consumed = 2;
            if (c != 'y' && PyUnicode_Check(arg)) { Py_ssize_t n; const char *s = PyUnicode_AsUTF8AndSize(arg, &n); if (!s) return -1; PyBuffer_FillInfo(view, arg, (void *)s, n, 1, 0); }
            else if (PyObject_GetBuffer(arg, view, PyBUF_SIMPLE) < 0) return -1;
            break;
        }
        if (fmt[1] == '#') {
            const char **p = va_arg(*va, const char **); Py_ssize_t *len = va_arg(*va, Py_ssize_t *); consumed = 2;
            if (allow_none && arg == Py_None) { *p = NULL; *len = 0; break; }
            if (c != 'y' && PyUnicode_Check(arg)) { *p = PyUnicode_AsUTF8AndSize(arg, len); if (!*p) return -1; }
            else if (PyBytes_Check(arg)) { *p = PyBytes_AS_STRING(arg); *len = Py_SIZE(arg); }
            else { PyErr_Format(PyExc_TypeError, "%s() argument %d must be %s, not %s", fname, argno, c == 'y' ? "bytes" : "str", Py_TYPE(arg)->tp_name); return -1; }
            break;
        }
        const char **p = va_arg(*va, const char **);
        if (allow_none && arg == Py_None) { *p = NULL; break; }
        if (c != 'y' && PyUnicode_Check(arg)) { Py_ssize_t n; *p = PyUnicode_AsUTF8AndSize(arg, &n); if (!*p) return -1; if ((Py_ssize_t)strlen(*p) != n) { PyErr_SetString(PyExc_ValueError, "embedded null character"); return -1; } }
        else if (c == 'y' && PyBytes_Check(arg)) { *p = PyBytes_AS_STRING(arg); if ((Py_ssize_t)strlen(*p) != Py_SIZE(arg)) { PyErr_SetString(PyExc_ValueError, "embedded null byte"); return -1; } }
        else { PyErr_Format(PyExc_TypeError, "%s() argument %d must be %s, not %s", fname, argno, c == 'y' ? "bytes" : "str", Py_TYPE(arg)->tp_name); return -1; }
        break;
    }
    case 'w': { Py_buffer *view = va_arg(*va, Py_buffer *); consumed = 2; if (PyObject_GetBuffer(arg, view, PyBUF_WRITABLE) < 0) return -1; break; }
    case 'e': { /* es / et: encoded string */ const char *enc = va_arg(*va, const char *); char **p = va_arg(*va, char **); consumed = 2; if (fmt[2] == '#') { Py_ssize_t *len = va_arg(*va, Py_ssize_t *); consumed = 3; PyObject *b = PyUnicode_AsEncodedString(arg, enc ? enc : "utf-8", "strict"); if (!b) return -1; *p = PyMem_Malloc((size_t)Py_SIZE(b) + 1); memcpy(*p, PyBytes_AS_STRING(b), (size_t)Py_SIZE(b) + 1); *len = Py_SIZE(b); Py_DECREF(b); } else { PyObject *b = PyUnicode_AsEncodedString(arg, enc ? enc : "utf-8", "strict"); if (!b) return -1; *p = PyMem_Malloc((size_t)Py_SIZE(b) + 1); memcpy(*p, PyBytes_AS_STRING(b), (size_t)Py_SIZE(b) + 1); Py_DECREF(b); } break; }
    default:
        PyErr_Format(PyExc_SystemError, "bad format char '%c' in PyArg_Parse", c);
        return -1;
    }
    return consumed;
}

static const char *fmt_name(const char *fmt) { const char *c = strchr(fmt, ':'); return c ? c + 1 : "function"; }

int PyArg_VaParseTupleAndKeywords(PyObject *args, PyObject *kw, const char *fmt, char **kwlist, va_list va) {
    va_list vac;
    va_copy(vac, va);
    const char *fname = fmt_name(fmt);
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t nkw = kw ? PyDict_Size(kw) : 0;
    int min = -1, max = 0, i = 0, posonly_end = -1, ok = 1;
    const char *p = fmt;
    Py_ssize_t used_kw = 0;
    while (*p && *p != ':' && *p != ';') {
        if (*p == '|') { min = max; p++; continue; }
        if (*p == '$') { posonly_end = max; p++; continue; }
        const char *name = kwlist ? kwlist[i] : NULL;
        PyObject *arg = NULL;
        if (i < nargs) {
            if (posonly_end >= 0 && i >= posonly_end && name && *name) { PyErr_Format(PyExc_TypeError, "%s() takes at most %d positional arguments (%zd given)", fname, posonly_end, nargs); ok = 0; break; }
            arg = PyTuple_GET_ITEM(args, i);
            if (kw && name && *name && PyDict_GetItemString(kw, name)) { PyErr_Format(PyExc_TypeError, "argument for %s() given by name ('%s') and position (%d)", fname, name, i + 1); ok = 0; break; }
        } else if (kw && name && *name) { arg = PyDict_GetItemString(kw, name); if (arg) used_kw++; }
        if (!arg) {
            if (min >= 0 && max >= min) {
                /* optional: skip its va args */
                int skip = convert_one(Py_None, p, &vac, fname, i + 1);
                if (skip < 0) { PyErr_Clear(); /* count units manually */ skip = 1; if (p[1] == '!' || p[1] == '&' || p[1] == '#' || p[1] == '*') skip = 2; if (p[0] == 'e' && p[2] == '#') skip = 3; }
                p += skip; max++; i++; continue;
            }
            if (name && *name) PyErr_Format(PyExc_TypeError, "%s() missing required argument '%s' (pos %d)", fname, name, i + 1);
            else PyErr_Format(PyExc_TypeError, "%s() takes at least %d positional arguments (%zd given)", fname, max + 1, nargs);
            ok = 0; break;
        }
        int consumed = convert_one(arg, p, &vac, fname, i + 1);
        if (consumed < 0) { ok = 0; break; }
        p += consumed; max++; i++;
    }
    va_end(vac);
    if (!ok) return 0;
    if (nargs > max) { PyErr_Format(PyExc_TypeError, "%s() takes at most %d argument%s (%zd given)", fname, max, max == 1 ? "" : "s", nargs); return 0; }
    if (nkw > used_kw) {
        Py_ssize_t pos = 0; PyObject *k;
        while (PyDict_Next(kw, &pos, &k, NULL)) {
            int found = 0;
            for (int j = 0; kwlist && kwlist[j]; j++) if (PyUnicode_EqualToUTF8(k, kwlist[j])) { found = 1; break; }
            if (!found) { PyErr_Format(PyExc_TypeError, "'%U' is an invalid keyword argument for %s()", k, fname); return 0; }
        }
    }
    return 1;
}
int PyArg_ParseTupleAndKeywords(PyObject *args, PyObject *kw, const char *fmt, char **kwlist, ...) {
    va_list va; va_start(va, kwlist);
    int r = PyArg_VaParseTupleAndKeywords(args, kw, fmt, kwlist, va);
    va_end(va);
    return r;
}
int PyArg_VaParse(PyObject *args, const char *fmt, va_list va) { return PyArg_VaParseTupleAndKeywords(args, NULL, fmt, NULL, va); }
int PyArg_ParseTuple(PyObject *args, const char *fmt, ...) { va_list va; va_start(va, fmt); int r = PyArg_VaParse(args, fmt, va); va_end(va); return r; }
int PyArg_Parse(PyObject *arg, const char *fmt, ...) { va_list va; va_start(va, fmt); PyObject *t = PyTuple_Pack(1, arg); int r = PyArg_VaParse(t, fmt, va); Py_DECREF(t); va_end(va); return r; }
int PyArg_UnpackTuple(PyObject *args, const char *name, Py_ssize_t min, Py_ssize_t max, ...) {
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (piper_args_range(name, n, min, max) < 0) return 0;
    va_list va; va_start(va, max);
    for (Py_ssize_t i = 0; i < max; i++) { PyObject **p = va_arg(va, PyObject **); *p = i < n ? PyTuple_GET_ITEM(args, i) : NULL; }
    va_end(va);
    return 1;
}

/* ---- Py_BuildValue --------------------------------------------------------- */

static PyObject *build_value(const char **pfmt, va_list *va);
static PyObject *build_sequence(const char **pfmt, va_list *va, char close, int kind) {
    PyObject *list = PyList_New(0);
    while (**pfmt && **pfmt != close) {
        if (**pfmt == ',' || **pfmt == ' ') { (*pfmt)++; continue; }
        PyObject *v = build_value(pfmt, va);
        if (!v) { Py_DECREF(list); return NULL; }
        PyList_Append(list, v);
        Py_DECREF(v);
    }
    if (**pfmt == close) (*pfmt)++;
    PyObject *r;
    if (kind == 0) r = PyList_AsTuple(list);
    else if (kind == 1) r = Py_NewRef(list);
    else {
        r = PyDict_New();
        for (Py_ssize_t i = 0; i + 1 < PyList_GET_SIZE(list); i += 2) PyDict_SetItem(r, PyList_GET_ITEM(list, i), PyList_GET_ITEM(list, i + 1));
    }
    Py_DECREF(list);
    return r;
}
static PyObject *build_value(const char **pfmt, va_list *va) {
    char c = *(*pfmt)++;
    switch (c) {
    case '(': return build_sequence(pfmt, va, ')', 0);
    case '[': return build_sequence(pfmt, va, ']', 1);
    case '{': return build_sequence(pfmt, va, '}', 2);
    case 'i': case 'b': case 'h': return PyLong_FromLong(va_arg(*va, int));
    case 'B': case 'H': case 'I': return PyLong_FromUnsignedLong(va_arg(*va, unsigned int));
    case 'l': return PyLong_FromLong(va_arg(*va, long));
    case 'k': return PyLong_FromUnsignedLong(va_arg(*va, unsigned long));
    case 'L': return PyLong_FromLongLong(va_arg(*va, long long));
    case 'K': return PyLong_FromUnsignedLongLong(va_arg(*va, unsigned long long));
    case 'n': return PyLong_FromSsize_t(va_arg(*va, Py_ssize_t));
    case 'd': case 'f': return PyFloat_FromDouble(va_arg(*va, double));
    case 'p': return PyBool_FromLong(va_arg(*va, int));
    case 'C': return PyUnicode_FromOrdinal(va_arg(*va, int));
    case 'c': { char ch = (char)va_arg(*va, int); return PyBytes_FromStringAndSize(&ch, 1); }
    case 's': case 'z': case 'U': case 'y': {
        const char *s = va_arg(*va, const char *);
        Py_ssize_t n = -1;
        if (**pfmt == '#') { (*pfmt)++; n = va_arg(*va, Py_ssize_t); }
        if (!s) Py_RETURN_NONE;
        if (n < 0) n = (Py_ssize_t)strlen(s);
        return c == 'y' ? PyBytes_FromStringAndSize(s, n) : PyUnicode_FromStringAndSize(s, n);
    }
    case 'O': case 'S': case 'N': {
        if (**pfmt == '&') { (*pfmt)++; PyObject *(*conv)(void *) = va_arg(*va, PyObject *(*)(void *)); void *p = va_arg(*va, void *); return conv(p); }
        PyObject *o = va_arg(*va, PyObject *);
        if (!o) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_SystemError, "NULL object passed to Py_BuildValue"); return NULL; }
        return c == 'N' ? o : Py_NewRef(o);
    }
    default:
        PyErr_Format(PyExc_SystemError, "bad format char '%c' in Py_BuildValue", c);
        return NULL;
    }
}
PyObject *Py_VaBuildValue(const char *fmt, va_list va) {
    va_list vac; va_copy(vac, va);
    const char *p = fmt;
    /* count top-level items */
    int depth = 0, items = 0;
    for (const char *q = fmt; *q; q++) { if (*q == '(' || *q == '[' || *q == '{') { if (depth == 0) items++; depth++; } else if (*q == ')' || *q == ']' || *q == '}') depth--; else if (depth == 0 && *q != ',' && *q != ' ' && *q != '#' && *q != '&') items++; }
    PyObject *r;
    if (items == 0) { va_end(vac); Py_RETURN_NONE; }
    if (items == 1) r = build_value(&p, &vac);
    else { const char *pp = fmt; r = build_sequence(&pp, &vac, 0, 0); }
    va_end(vac);
    return r;
}
PyObject *Py_BuildValue(const char *fmt, ...) { va_list va; va_start(va, fmt); PyObject *r = Py_VaBuildValue(fmt, va); va_end(va); return r; }
PyObject *_Py_BuildValue_SizeT(const char *fmt, ...) { va_list va; va_start(va, fmt); PyObject *r = Py_VaBuildValue(fmt, va); va_end(va); return r; }
