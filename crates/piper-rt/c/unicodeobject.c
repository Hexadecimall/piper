/* str: compact CPython-layout unicode objects and the str methods. */
#include "internal.h"
#include <wchar.h>

/* ---- construction ------------------------------------------------------ */

PyObject *PyUnicode_New(Py_ssize_t size, Py_UCS4 maxchar) {
    int kind;
    size_t charsize;
    int ascii = 0;
    if (maxchar < 128) { kind = PyUnicode_1BYTE_KIND; charsize = 1; ascii = 1; }
    else if (maxchar < 256) { kind = PyUnicode_1BYTE_KIND; charsize = 1; }
    else if (maxchar < 65536) { kind = PyUnicode_2BYTE_KIND; charsize = 2; }
    else { if (maxchar > 0x10ffff) { PyErr_SetString(PyExc_SystemError, "invalid maximum character passed to PyUnicode_New"); return NULL; } kind = PyUnicode_4BYTE_KIND; charsize = 4; }
    size_t struct_size = ascii ? sizeof(PyASCIIObject) : sizeof(PyCompactUnicodeObject);
    PyCompactUnicodeObject *o = PyObject_Malloc(struct_size + ((size_t)size + 1) * charsize);
    if (!o) return PyErr_NoMemory();
    PyObject_Init((PyObject *)o, &PyUnicode_Type);
    PyASCIIObject *a = &o->_base;
    a->length = size;
    a->hash = -1;
    a->state.interned = 0;
    a->state.kind = (unsigned)kind;
    a->state.compact = 1;
    a->state.ascii = (unsigned)ascii;
    a->state.statically_allocated = 0;
    if (!ascii) { o->utf8_length = 0; o->utf8 = NULL; }
    void *data = PyUnicode_DATA((PyObject *)o);
    if (charsize == 1) ((Py_UCS1 *)data)[size] = 0;
    else if (charsize == 2) ((Py_UCS2 *)data)[size] = 0;
    else ((Py_UCS4 *)data)[size] = 0;
    return (PyObject *)o;
}

static void unicode_dealloc(PyObject *o) {
    if (!PyUnicode_IS_COMPACT_ASCII(o) && PyUnicode_IS_COMPACT(o)) {
        PyCompactUnicodeObject *c = (PyCompactUnicodeObject *)o;
        if (c->utf8) PyMem_Free(c->utf8);
    } else if (!PyUnicode_IS_COMPACT(o)) {
        PyUnicodeObject *u = (PyUnicodeObject *)o;
        if (u->_base.utf8 && u->_base.utf8 != u->data.any) PyMem_Free(u->_base.utf8);
        PyMem_Free(u->data.any);
    }
    Py_TYPE(o)->tp_free(o);
}

/* Decode UTF-8 into a new str; invalid sequences raise UnicodeDecodeError
 * unless errors is "surrogateescape" or "replace". */
static PyObject *decode_utf8(const char *s, Py_ssize_t n, const char *errors, Py_ssize_t *consumed) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    /* First pass: length and maxchar. */
    Py_ssize_t len = 0;
    Py_UCS4 maxchar = 0;
    int replace = errors && !strcmp(errors, "replace");
    int escape = errors && !strcmp(errors, "surrogateescape");
    int ignore = errors && !strcmp(errors, "ignore");
    const unsigned char *q = p;
    while (q < end) {
        unsigned char c = *q;
        Py_UCS4 ch;
        int nb;
        if (c < 0x80) { ch = c; nb = 1; }
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) { nb = 2; ch = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { nb = 3; ch = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) { nb = 4; ch = c & 0x07; }
        else nb = 0;
        int ok = nb > 0 && q + nb <= end;
        if (ok) for (int i = 1; i < nb; i++) { unsigned char cc = q[i]; if ((cc & 0xC0) != 0x80) { ok = 0; break; } ch = (ch << 6) | (cc & 0x3F); }
        if (ok && nb == 3 && (ch < 0x800 || (ch >= 0xD800 && ch <= 0xDFFF))) ok = 0;
        if (ok && nb == 4 && (ch < 0x10000 || ch > 0x10FFFF)) ok = 0;
        if (!ok) {
            if (consumed && q + (nb ? nb : 1) > end && nb) { break; }
            if (escape) { ch = 0xDC00 + c; nb = 1; }
            else if (replace) { ch = 0xFFFD; nb = 1; }
            else if (ignore) { q++; continue; }
            else {
                PyErr_Format(PyExc_UnicodeDecodeError, "'utf-8' codec can't decode byte 0x%02x in position %zd: invalid start byte", c, (Py_ssize_t)(q - p));
                return NULL;
            }
        }
        if (ch > maxchar) maxchar = ch;
        len++;
        q += nb;
    }
    PyObject *r = PyUnicode_New(len, maxchar);
    if (!r) return NULL;
    int kind = PyUnicode_KIND(r);
    void *data = PyUnicode_DATA(r);
    Py_ssize_t i = 0;
    q = p;
    const unsigned char *stop = consumed ? q + (q - p) : end;
    (void)stop;
    Py_ssize_t written_end = len;
    while (q < end && i < written_end) {
        unsigned char c = *q;
        Py_UCS4 ch;
        int nb;
        if (c < 0x80) { ch = c; nb = 1; }
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) { nb = 2; ch = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { nb = 3; ch = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) { nb = 4; ch = c & 0x07; }
        else nb = 0;
        int ok = nb > 0 && q + nb <= end;
        if (ok) for (int k = 1; k < nb; k++) { unsigned char cc = q[k]; if ((cc & 0xC0) != 0x80) { ok = 0; break; } ch = (ch << 6) | (cc & 0x3F); }
        if (ok && nb == 3 && (ch < 0x800 || (ch >= 0xD800 && ch <= 0xDFFF))) ok = 0;
        if (ok && nb == 4 && (ch < 0x10000 || ch > 0x10FFFF)) ok = 0;
        if (!ok) { if (ignore) { q++; continue; } ch = escape ? 0xDC00 + c : 0xFFFD; nb = 1; }
        PyUnicode_WRITE(kind, data, i++, ch);
        q += nb;
    }
    if (consumed) *consumed = (Py_ssize_t)(q - p);
    return r;
}

PyObject *PyUnicode_DecodeUTF8Stateful(const char *s, Py_ssize_t n, const char *errors, Py_ssize_t *consumed) { return decode_utf8(s, n, errors, consumed); }
PyObject *PyUnicode_DecodeUTF8(const char *s, Py_ssize_t n, const char *errors) { return decode_utf8(s, n, errors, NULL); }

PyObject *PyUnicode_FromStringAndSize(const char *s, Py_ssize_t n) {
    if (n < 0) { PyErr_SetString(PyExc_SystemError, "Negative size passed to PyUnicode_FromStringAndSize"); return NULL; }
    if (!s) return PyUnicode_New(n, 0x7f);
    /* fast path: ASCII */
    Py_ssize_t i = 0;
    while (i < n && (unsigned char)s[i] < 0x80) i++;
    if (i == n) {
        PyObject *r = PyUnicode_New(n, 0x7f);
        if (!r) return NULL;
        memcpy(PyUnicode_DATA(r), s, (size_t)n);
        return r;
    }
    return decode_utf8(s, n, NULL, NULL);
}
PyObject *PyUnicode_FromString(const char *s) { return PyUnicode_FromStringAndSize(s, (Py_ssize_t)strlen(s)); }
PyObject *piper_str_const(const char *utf8, Py_ssize_t n) { return PyUnicode_FromStringAndSize(utf8, n); }
PyObject *PyUnicode_DecodeASCII(const char *s, Py_ssize_t n, const char *errors) {
    for (Py_ssize_t i = 0; i < n; i++) if ((unsigned char)s[i] >= 0x80) {
        if (errors && !strcmp(errors, "ignore")) { PyObject *r = PyUnicode_New(n, 0x7f); Py_ssize_t j = 0; for (Py_ssize_t k = 0; k < n; k++) if ((unsigned char)s[k] < 0x80) ((Py_UCS1 *)PyUnicode_DATA(r))[j++] = (Py_UCS1)s[k]; ((PyASCIIObject *)r)->length = j; return r; }
        PyErr_Format(PyExc_UnicodeDecodeError, "'ascii' codec can't decode byte 0x%02x in position %zd: ordinal not in range(128)", (unsigned char)s[i], i);
        return NULL;
    }
    return PyUnicode_FromStringAndSize(s, n);
}
PyObject *PyUnicode_DecodeLatin1(const char *s, Py_ssize_t n, const char *errors) {
    PIPER_UNUSED(errors);
    Py_UCS4 maxchar = 0;
    for (Py_ssize_t i = 0; i < n; i++) if ((unsigned char)s[i] > maxchar) maxchar = (unsigned char)s[i];
    PyObject *r = PyUnicode_New(n, maxchar);
    if (!r) return NULL;
    memcpy(PyUnicode_DATA(r), s, (size_t)n);
    return r;
}
PyObject *PyUnicode_DecodeFSDefaultAndSize(const char *s, Py_ssize_t n) { return decode_utf8(s, n, "surrogateescape", NULL); }
PyObject *PyUnicode_DecodeFSDefault(const char *s) { return PyUnicode_DecodeFSDefaultAndSize(s, (Py_ssize_t)strlen(s)); }

PyObject *PyUnicode_FromKindAndData(int kind, const void *buf, Py_ssize_t n) {
    Py_UCS4 maxchar = 0;
    for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, buf, i); if (c > maxchar) maxchar = c; }
    PyObject *r = PyUnicode_New(n, maxchar);
    if (!r) return NULL;
    int rk = PyUnicode_KIND(r);
    void *data = PyUnicode_DATA(r);
    if (rk == kind) memcpy(data, buf, (size_t)n * (size_t)kind);
    else for (Py_ssize_t i = 0; i < n; i++) PyUnicode_WRITE(rk, data, i, PyUnicode_READ(kind, buf, i));
    return r;
}
PyObject *piper_unicode_from_ucs4(const Py_UCS4 *buf, Py_ssize_t n) { return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, buf, n); }

PyObject *PyUnicode_FromOrdinal(int ord) {
    if (ord < 0 || ord > 0x10ffff) { PyErr_SetString(PyExc_ValueError, "chr() arg not in range(0x110000)"); return NULL; }
    Py_UCS4 c = (Py_UCS4)ord;
    return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &c, 1);
}

PyObject *PyUnicode_FromWideChar(const wchar_t *w, Py_ssize_t size) {
    if (size < 0) size = (Py_ssize_t)wcslen(w);
    PyObject *r = PyUnicode_New(size, 0x10ffff);
    if (!r) return NULL;
    Py_UCS4 *tmp = PyMem_Malloc((size_t)size * 4);
    for (Py_ssize_t i = 0; i < size; i++) tmp[i] = (Py_UCS4)w[i];
    Py_DECREF(r);
    r = piper_unicode_from_ucs4(tmp, size);
    PyMem_Free(tmp);
    return r;
}

/* ---- UTF-8 out ---------------------------------------------------------- */

static Py_ssize_t utf8_len_of(PyObject *u) {
    int kind = PyUnicode_KIND(u);
    void *data = PyUnicode_DATA(u);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u), total = 0;
    for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); total += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
    return total;
}

static void encode_utf8_into(PyObject *u, char *out) {
    int kind = PyUnicode_KIND(u);
    void *data = PyUnicode_DATA(u);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    unsigned char *o = (unsigned char *)out;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        if (c < 0x80) *o++ = (unsigned char)c;
        else if (c < 0x800) { *o++ = (unsigned char)(0xC0 | (c >> 6)); *o++ = (unsigned char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { *o++ = (unsigned char)(0xE0 | (c >> 12)); *o++ = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (unsigned char)(0x80 | (c & 0x3F)); }
        else { *o++ = (unsigned char)(0xF0 | (c >> 18)); *o++ = (unsigned char)(0x80 | ((c >> 12) & 0x3F)); *o++ = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (unsigned char)(0x80 | (c & 0x3F)); }
    }
    *o = 0;
}

const char *PyUnicode_AsUTF8AndSize(PyObject *u, Py_ssize_t *n) {
    if (!PyUnicode_Check(u)) { PyErr_BadArgument(); return NULL; }
    if (PyUnicode_IS_COMPACT_ASCII(u)) { if (n) *n = PyUnicode_GET_LENGTH(u); return (const char *)PyUnicode_DATA(u); }
    PyCompactUnicodeObject *c = (PyCompactUnicodeObject *)u;
    if (!c->utf8) {
        /* surrogates cannot be encoded */
        int kind = PyUnicode_KIND(u);
        void *data = PyUnicode_DATA(u);
        for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(u); i++) { Py_UCS4 ch = PyUnicode_READ(kind, data, i); if (ch >= 0xD800 && ch <= 0xDFFF) { PyErr_Format(PyExc_UnicodeEncodeError, "'utf-8' codec can't encode character '\\u%04x' in position %zd: surrogates not allowed", ch, i); return NULL; } }
        Py_ssize_t len = utf8_len_of(u);
        c->utf8 = PyMem_Malloc((size_t)len + 1);
        if (!c->utf8) { PyErr_NoMemory(); return NULL; }
        encode_utf8_into(u, c->utf8);
        c->utf8_length = len;
    }
    if (n) *n = c->utf8_length;
    return c->utf8;
}
const char *PyUnicode_AsUTF8(PyObject *u) { return PyUnicode_AsUTF8AndSize(u, NULL); }

PyObject *PyUnicode_AsUTF8String(PyObject *u) {
    Py_ssize_t n;
    const char *s = PyUnicode_AsUTF8AndSize(u, &n);
    if (!s) return NULL;
    return PyBytes_FromStringAndSize(s, n);
}
PyObject *PyUnicode_EncodeFSDefault(PyObject *u) {
    /* surrogateescape */
    int kind = PyUnicode_KIND(u);
    void *data = PyUnicode_DATA(u);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    PyObject *b = PyBytes_FromStringAndSize(NULL, n * 4);
    unsigned char *o = (unsigned char *)PyBytes_AS_STRING(b);
    Py_ssize_t j = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        if (c >= 0xDC80 && c <= 0xDCFF) o[j++] = (unsigned char)(c - 0xDC00);
        else if (c < 0x80) o[j++] = (unsigned char)c;
        else if (c < 0x800) { o[j++] = (unsigned char)(0xC0 | (c >> 6)); o[j++] = (unsigned char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o[j++] = (unsigned char)(0xE0 | (c >> 12)); o[j++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); o[j++] = (unsigned char)(0x80 | (c & 0x3F)); }
        else { o[j++] = (unsigned char)(0xF0 | (c >> 18)); o[j++] = (unsigned char)(0x80 | ((c >> 12) & 0x3F)); o[j++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F)); o[j++] = (unsigned char)(0x80 | (c & 0x3F)); }
    }
    _PyBytes_Resize(&b, j);
    return b;
}
PyObject *PyUnicode_AsASCIIString(PyObject *u) {
    if (!PyUnicode_IS_ASCII(u)) {
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
        for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(u); i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (c >= 128) { PyErr_Format(PyExc_UnicodeEncodeError, "'ascii' codec can't encode character '\\u%04x' in position %zd: ordinal not in range(128)", c, i); return NULL; } }
    }
    return PyBytes_FromStringAndSize((const char *)PyUnicode_DATA(u), PyUnicode_GET_LENGTH(u));
}
PyObject *PyUnicode_AsLatin1String(PyObject *u) {
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    PyObject *b = PyBytes_FromStringAndSize(NULL, n);
    for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (c >= 256) { Py_DECREF(b); PyErr_Format(PyExc_UnicodeEncodeError, "'latin-1' codec can't encode character '\\u%04x' in position %zd: ordinal not in range(256)", c, i); return NULL; } PyBytes_AS_STRING(b)[i] = (char)c; }
    return b;
}

static int codec_normalize(const char *enc, char *out, size_t cap) {
    size_t j = 0;
    for (const char *p = enc; *p && j + 1 < cap; p++) { char c = *p; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c == '-' || c == ' ') c = '_'; out[j++] = c; }
    out[j] = 0;
    return 0;
}

PyObject *PyUnicode_AsEncodedString(PyObject *u, const char *enc, const char *errors) {
    char e[64];
    codec_normalize(enc ? enc : "utf-8", e, sizeof e);
    if (!strcmp(e, "utf_8") || !strcmp(e, "utf8") || !strcmp(e, "u8")) {
        if (errors && !strcmp(errors, "surrogateescape")) return PyUnicode_EncodeFSDefault(u);
        return PyUnicode_AsUTF8String(u);
    }
    if (!strcmp(e, "ascii") || !strcmp(e, "us_ascii")) {
        if (errors && (!strcmp(errors, "ignore") || !strcmp(errors, "replace"))) {
            int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
            Py_ssize_t n = PyUnicode_GET_LENGTH(u);
            PyObject *b = PyBytes_FromStringAndSize(NULL, n);
            Py_ssize_t j = 0;
            for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (c < 128) PyBytes_AS_STRING(b)[j++] = (char)c; else if (errors[0] == 'r') PyBytes_AS_STRING(b)[j++] = '?'; }
            _PyBytes_Resize(&b, j);
            return b;
        }
        return PyUnicode_AsASCIIString(u);
    }
    if (!strcmp(e, "latin_1") || !strcmp(e, "latin1") || !strcmp(e, "iso_8859_1") || !strcmp(e, "iso8859_1") || !strcmp(e, "l1")) return PyUnicode_AsLatin1String(u);
    if (!strcmp(e, "utf_16") || !strcmp(e, "utf_16_le") || !strcmp(e, "utf_16_be") || !strcmp(e, "utf16")) {
        int be = !strcmp(e, "utf_16_be");
        int bom = !strcmp(e, "utf_16") || !strcmp(e, "utf16");
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
        Py_ssize_t n = PyUnicode_GET_LENGTH(u);
        PyObject *b = PyBytes_FromStringAndSize(NULL, n * 4 + 2);
        unsigned char *o = (unsigned char *)PyBytes_AS_STRING(b);
        Py_ssize_t j = 0;
        if (bom) { o[j++] = 0xFF; o[j++] = 0xFE; }
        for (Py_ssize_t i = 0; i < n; i++) {
            Py_UCS4 c = PyUnicode_READ(kind, data, i);
            Py_UCS4 units[2]; int nu = 1;
            if (c >= 0x10000) { c -= 0x10000; units[0] = 0xD800 | (c >> 10); units[1] = 0xDC00 | (c & 0x3FF); nu = 2; } else units[0] = c;
            for (int k = 0; k < nu; k++) { if (be) { o[j++] = (unsigned char)(units[k] >> 8); o[j++] = (unsigned char)units[k]; } else { o[j++] = (unsigned char)units[k]; o[j++] = (unsigned char)(units[k] >> 8); } }
        }
        _PyBytes_Resize(&b, j);
        return b;
    }
    if (!strcmp(e, "utf_32") || !strcmp(e, "utf_32_le") || !strcmp(e, "utf_32_be")) {
        int be = !strcmp(e, "utf_32_be");
        int bom = !strcmp(e, "utf_32");
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
        Py_ssize_t n = PyUnicode_GET_LENGTH(u);
        PyObject *b = PyBytes_FromStringAndSize(NULL, n * 4 + 4);
        unsigned char *o = (unsigned char *)PyBytes_AS_STRING(b);
        Py_ssize_t j = 0;
        if (bom) { o[j++] = 0xFF; o[j++] = 0xFE; o[j++] = 0; o[j++] = 0; }
        for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); for (int k = 0; k < 4; k++) o[j++] = be ? (unsigned char)(c >> (8 * (3 - k))) : (unsigned char)(c >> (8 * k)); }
        _PyBytes_Resize(&b, j);
        return b;
    }
    PyErr_Format(PyExc_LookupError, "unknown encoding: %s", enc);
    return NULL;
}

PyObject *PyUnicode_Decode(const char *s, Py_ssize_t n, const char *enc, const char *errors) {
    char e[64];
    codec_normalize(enc ? enc : "utf-8", e, sizeof e);
    if (!strcmp(e, "utf_8") || !strcmp(e, "utf8") || !strcmp(e, "u8") || !strcmp(e, "utf_8_sig")) {
        if (!strcmp(e, "utf_8_sig") && n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) { s += 3; n -= 3; }
        return decode_utf8(s, n, errors, NULL);
    }
    if (!strcmp(e, "ascii") || !strcmp(e, "us_ascii")) return PyUnicode_DecodeASCII(s, n, errors);
    if (!strcmp(e, "latin_1") || !strcmp(e, "latin1") || !strcmp(e, "iso_8859_1") || !strcmp(e, "iso8859_1") || !strcmp(e, "l1")) return PyUnicode_DecodeLatin1(s, n, errors);
    if (!strncmp(e, "utf_16", 6) || !strcmp(e, "utf16")) {
        int be = !strcmp(e, "utf_16_be");
        const unsigned char *p = (const unsigned char *)s;
        if ((!strcmp(e, "utf_16") || !strcmp(e, "utf16")) && n >= 2) { if (p[0] == 0xFE && p[1] == 0xFF) { be = 1; p += 2; n -= 2; } else if (p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; } }
        Py_UCS4 *buf = PyMem_Malloc((size_t)n / 2 * 4 + 4);
        Py_ssize_t len = 0;
        for (Py_ssize_t i = 0; i + 1 < n; i += 2) {
            Py_UCS4 c = be ? ((Py_UCS4)p[i] << 8 | p[i + 1]) : ((Py_UCS4)p[i + 1] << 8 | p[i]);
            if (c >= 0xD800 && c <= 0xDBFF && i + 3 < n) { Py_UCS4 c2 = be ? ((Py_UCS4)p[i + 2] << 8 | p[i + 3]) : ((Py_UCS4)p[i + 3] << 8 | p[i + 2]); if (c2 >= 0xDC00 && c2 <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2; } }
            buf[len++] = c;
        }
        PyObject *r = piper_unicode_from_ucs4(buf, len);
        PyMem_Free(buf);
        return r;
    }
    if (!strncmp(e, "utf_32", 6)) {
        int be = !strcmp(e, "utf_32_be");
        const unsigned char *p = (const unsigned char *)s;
        if (!strcmp(e, "utf_32") && n >= 4) { if (p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) { be = 1; p += 4; n -= 4; } else if (p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) { p += 4; n -= 4; } }
        Py_UCS4 *buf = PyMem_Malloc((size_t)n + 4);
        Py_ssize_t len = 0;
        for (Py_ssize_t i = 0; i + 3 < n; i += 4) buf[len++] = be ? ((Py_UCS4)p[i] << 24 | (Py_UCS4)p[i + 1] << 16 | (Py_UCS4)p[i + 2] << 8 | p[i + 3]) : ((Py_UCS4)p[i + 3] << 24 | (Py_UCS4)p[i + 2] << 16 | (Py_UCS4)p[i + 1] << 8 | p[i]);
        PyObject *r = piper_unicode_from_ucs4(buf, len);
        PyMem_Free(buf);
        return r;
    }
    PyErr_Format(PyExc_LookupError, "unknown encoding: %s", enc);
    return NULL;
}

PyObject *PyUnicode_FromEncodedObject(PyObject *o, const char *enc, const char *errors) {
    if (PyBytes_Check(o)) return PyUnicode_Decode(PyBytes_AS_STRING(o), PyBytes_GET_SIZE(o), enc, errors);
    Py_buffer view;
    if (PyObject_GetBuffer(o, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyObject *r = PyUnicode_Decode(view.buf, view.len, enc, errors);
    PyBuffer_Release(&view);
    return r;
}

PyObject *PyUnicode_FromObject(PyObject *o) {
    if (PyUnicode_CheckExact(o)) return Py_NewRef(o);
    if (PyUnicode_Check(o)) return PyUnicode_Substring(o, 0, PyUnicode_GET_LENGTH(o));
    PyErr_Format(PyExc_TypeError, "Can't convert '%s' object to str implicitly", Py_TYPE(o)->tp_name);
    return NULL;
}

Py_ssize_t PyUnicode_AsWideChar(PyObject *u, wchar_t *w, Py_ssize_t size) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (!w) return n + 1;
    Py_ssize_t m = n < size ? n : size;
    for (Py_ssize_t i = 0; i < m; i++) w[i] = (wchar_t)PyUnicode_READ_CHAR(u, i);
    if (m < size) w[m] = 0;
    return m;
}
wchar_t *PyUnicode_AsWideCharString(PyObject *u, Py_ssize_t *size) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    wchar_t *w = PyMem_Malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!w) { PyErr_NoMemory(); return NULL; }
    PyUnicode_AsWideChar(u, w, n + 1);
    if (size) *size = n;
    return w;
}
Py_UCS4 *PyUnicode_AsUCS4(PyObject *u, Py_UCS4 *buf, Py_ssize_t bufsize, int copy_null) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (bufsize < n + (copy_null ? 1 : 0)) { PyErr_SetString(PyExc_SystemError, "string is longer than the buffer"); return NULL; }
    for (Py_ssize_t i = 0; i < n; i++) buf[i] = PyUnicode_READ_CHAR(u, i);
    if (copy_null) buf[n] = 0;
    return buf;
}
Py_UCS4 *PyUnicode_AsUCS4Copy(PyObject *u) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    Py_UCS4 *buf = PyMem_Malloc(((size_t)n + 1) * 4);
    if (!buf) { PyErr_NoMemory(); return NULL; }
    return PyUnicode_AsUCS4(u, buf, n + 1, 1);
}

/* ---- basic accessors ------------------------------------------------------ */

Py_ssize_t PyUnicode_GetLength(PyObject *u) { if (!PyUnicode_Check(u)) { PyErr_BadArgument(); return -1; } return PyUnicode_GET_LENGTH(u); }
Py_UCS4 PyUnicode_ReadChar(PyObject *u, Py_ssize_t i) {
    if (i < 0 || i >= PyUnicode_GET_LENGTH(u)) { PyErr_SetString(PyExc_IndexError, "string index out of range"); return (Py_UCS4)-1; }
    return PyUnicode_READ_CHAR(u, i);
}
int PyUnicode_WriteChar(PyObject *u, Py_ssize_t i, Py_UCS4 c) {
    if (i < 0 || i >= PyUnicode_GET_LENGTH(u)) { PyErr_SetString(PyExc_IndexError, "string index out of range"); return -1; }
    PyUnicode_WRITE(PyUnicode_KIND(u), PyUnicode_DATA(u), i, c);
    return 0;
}
Py_UCS4 PyUnicode_MaxChar(PyObject *u) {
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_UCS4 m = 0;
    for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(u); i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (c > m) m = c; }
    return m;
}
void *PyUnicode_DATA_fn(PyObject *u) { return PyUnicode_DATA(u); }
int PyUnicode_KIND_fn(PyObject *u) { return PyUnicode_KIND(u); }

PyObject *PyUnicode_Substring(PyObject *u, Py_ssize_t start, Py_ssize_t end) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (start >= end) return PyUnicode_New(0, 0);
    if (start == 0 && end == n && PyUnicode_CheckExact(u)) return Py_NewRef(u);
    int kind = PyUnicode_KIND(u);
    return PyUnicode_FromKindAndData(kind, (char *)PyUnicode_DATA(u) + (size_t)start * (size_t)kind, end - start);
}

Py_ssize_t PyUnicode_CopyCharacters(PyObject *to, Py_ssize_t to_start, PyObject *from, Py_ssize_t from_start, Py_ssize_t n) {
    int tk = PyUnicode_KIND(to), fk = PyUnicode_KIND(from);
    void *td = PyUnicode_DATA(to), *fd = PyUnicode_DATA(from);
    Py_ssize_t avail = PyUnicode_GET_LENGTH(from) - from_start;
    if (n > avail) n = avail;
    if (tk == fk) memcpy((char *)td + (size_t)to_start * (size_t)tk, (char *)fd + (size_t)from_start * (size_t)fk, (size_t)n * (size_t)tk);
    else for (Py_ssize_t i = 0; i < n; i++) PyUnicode_WRITE(tk, td, to_start + i, PyUnicode_READ(fk, fd, from_start + i));
    return n;
}

Py_ssize_t PyUnicode_Fill(PyObject *u, Py_ssize_t start, Py_ssize_t length, Py_UCS4 c) {
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    for (Py_ssize_t i = 0; i < length; i++) PyUnicode_WRITE(kind, data, start + i, c);
    return length;
}

/* ---- hash / compare ------------------------------------------------------- */

Py_hash_t piper_unicode_hash(PyObject *u) {
    PyASCIIObject *a = (PyASCIIObject *)u;
    if (a->hash != -1) return a->hash;
    Py_hash_t h;
    if (PyUnicode_IS_ASCII(u)) h = piper_hash_bytes(PyUnicode_DATA(u), a->length);
    else {
        /* hash the UCS4 form so equal strings of different kinds agree */
        Py_ssize_t n = a->length;
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
        uint64_t x = 1469598103934665603ULL;
        if (n == 0) h = 0;
        else {
            for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); for (int k = 0; k < 4; k++) { x ^= (c >> (8 * k)) & 0xFF; x *= 1099511628211ULL; } }
            h = (Py_hash_t)x;
            if (h == -1) h = -2;
        }
    }
    /* ASCII strings hashed as bytes must agree with the UCS4 path for ASCII content: make both use bytes */
    if (!PyUnicode_IS_ASCII(u)) {
        int all_ascii = 1;
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
        for (Py_ssize_t i = 0; i < a->length; i++) if (PyUnicode_READ(kind, data, i) >= 128) { all_ascii = 0; break; }
        if (all_ascii) {
            char *tmp = PyMem_Malloc((size_t)a->length + 1);
            for (Py_ssize_t i = 0; i < a->length; i++) tmp[i] = (char)PyUnicode_READ(kind, data, i);
            h = piper_hash_bytes(tmp, a->length);
            PyMem_Free(tmp);
        }
    }
    a->hash = h;
    return h;
}
static Py_hash_t unicode_hash(PyObject *u) { return piper_unicode_hash(u); }

int piper_unicode_eq(PyObject *a, PyObject *b) {
    if (a == b) return 1;
    Py_ssize_t n = PyUnicode_GET_LENGTH(a);
    if (n != PyUnicode_GET_LENGTH(b)) return 0;
    PyASCIIObject *aa = (PyASCIIObject *)a, *bb = (PyASCIIObject *)b;
    if (aa->hash != -1 && bb->hash != -1 && aa->hash != bb->hash) return 0;
    int ka = PyUnicode_KIND(a), kb = PyUnicode_KIND(b);
    if (ka == kb) return memcmp(PyUnicode_DATA(a), PyUnicode_DATA(b), (size_t)n * (size_t)ka) == 0;
    void *da = PyUnicode_DATA(a), *db = PyUnicode_DATA(b);
    for (Py_ssize_t i = 0; i < n; i++) if (PyUnicode_READ(ka, da, i) != PyUnicode_READ(kb, db, i)) return 0;
    return 1;
}
int PyUnicode_Equal(PyObject *a, PyObject *b) { return piper_unicode_eq(a, b); }

int PyUnicode_Compare(PyObject *a, PyObject *b) {
    if (!PyUnicode_Check(a) || !PyUnicode_Check(b)) { PyErr_SetString(PyExc_TypeError, "Can't compare str with non-str"); return -1; }
    Py_ssize_t na = PyUnicode_GET_LENGTH(a), nb = PyUnicode_GET_LENGTH(b);
    int ka = PyUnicode_KIND(a), kb = PyUnicode_KIND(b);
    void *da = PyUnicode_DATA(a), *db = PyUnicode_DATA(b);
    Py_ssize_t n = na < nb ? na : nb;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 x = PyUnicode_READ(ka, da, i), y = PyUnicode_READ(kb, db, i);
        if (x != y) return x < y ? -1 : 1;
    }
    return na < nb ? -1 : na > nb;
}
int PyUnicode_CompareWithASCIIString(PyObject *a, const char *b) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(a);
    int kind = PyUnicode_KIND(a); void *data = PyUnicode_DATA(a);
    Py_ssize_t i = 0;
    for (; i < n && b[i]; i++) { Py_UCS4 x = PyUnicode_READ(kind, data, i), y = (unsigned char)b[i]; if (x != y) return x < y ? -1 : 1; }
    if (i < n) return 1;
    return b[i] ? -1 : 0;
}
int PyUnicode_EqualToUTF8AndSize(PyObject *a, const char *b, Py_ssize_t n) {
    if (PyUnicode_IS_ASCII(a)) return PyUnicode_GET_LENGTH(a) == n && memcmp(PyUnicode_DATA(a), b, (size_t)n) == 0;
    Py_ssize_t m;
    const char *s = PyUnicode_AsUTF8AndSize(a, &m);
    if (!s) { PyErr_Clear(); return 0; }
    return m == n && memcmp(s, b, (size_t)n) == 0;
}
int PyUnicode_EqualToUTF8(PyObject *a, const char *b) { return PyUnicode_EqualToUTF8AndSize(a, b, (Py_ssize_t)strlen(b)); }

PyObject *PyUnicode_RichCompare(PyObject *a, PyObject *b, int op) {
    if (!PyUnicode_Check(a) || !PyUnicode_Check(b)) Py_RETURN_NOTIMPLEMENTED;
    if (op == Py_EQ) Py_RETURN_BOOL(piper_unicode_eq(a, b));
    if (op == Py_NE) Py_RETURN_BOOL(!piper_unicode_eq(a, b));
    int c = PyUnicode_Compare(a, b);
    int r;
    switch (op) { case Py_LT: r = c < 0; break; case Py_LE: r = c <= 0; break; case Py_GT: r = c > 0; break; default: r = c >= 0; }
    Py_RETURN_BOOL(r);
}

/* ---- concat / repeat / find --------------------------------------------- */

PyObject *PyUnicode_Concat(PyObject *a, PyObject *b) {
    if (!PyUnicode_Check(a) || !PyUnicode_Check(b)) { PyErr_Format(PyExc_TypeError, "can only concatenate str (not \"%s\") to str", Py_TYPE(b)->tp_name); return NULL; }
    Py_ssize_t na = PyUnicode_GET_LENGTH(a), nb = PyUnicode_GET_LENGTH(b);
    if (nb == 0 && PyUnicode_CheckExact(a)) return Py_NewRef(a);
    if (na == 0 && PyUnicode_CheckExact(b)) return Py_NewRef(b);
    Py_UCS4 ma = PyUnicode_MAX_CHAR_VALUE(a), mb = PyUnicode_MAX_CHAR_VALUE(b);
    PyObject *r = PyUnicode_New(na + nb, ma > mb ? ma : mb);
    if (!r) return NULL;
    PyUnicode_CopyCharacters(r, 0, a, 0, na);
    PyUnicode_CopyCharacters(r, na, b, 0, nb);
    return r;
}
void PyUnicode_Append(PyObject **pleft, PyObject *right) {
    if (!*pleft) return;
    if (!right) { Py_CLEAR(*pleft); return; }
    PyObject *r = PyUnicode_Concat(*pleft, right);
    Py_DECREF(*pleft);
    *pleft = r;
}
void PyUnicode_AppendAndDel(PyObject **pleft, PyObject *right) { PyUnicode_Append(pleft, right); Py_XDECREF(right); }

static PyObject *unicode_concat(PyObject *a, PyObject *b) {
    if (!PyUnicode_Check(b)) { PyErr_Format(PyExc_TypeError, "can only concatenate str (not \"%s\") to str", Py_TYPE(b)->tp_name); return NULL; }
    return PyUnicode_Concat(a, b);
}

static PyObject *unicode_repeat(PyObject *a, Py_ssize_t n) {
    if (n < 0) n = 0;
    Py_ssize_t len = PyUnicode_GET_LENGTH(a);
    if (n == 1 && PyUnicode_CheckExact(a)) return Py_NewRef(a);
    if (len && n > PY_SSIZE_T_MAX / len) return PyErr_NoMemory();
    PyObject *r = PyUnicode_New(len * n, PyUnicode_MAX_CHAR_VALUE(a));
    if (!r) return NULL;
    int kind = PyUnicode_KIND(r);
    char *dst = PyUnicode_DATA(r);
    size_t chunk = (size_t)len * (size_t)kind;
    for (Py_ssize_t i = 0; i < n; i++) memcpy(dst + (size_t)i * chunk, PyUnicode_DATA(a), chunk);
    return r;
}

static Py_ssize_t adjust_indices(Py_ssize_t *start, Py_ssize_t *end, Py_ssize_t len) {
    if (*end > len) *end = len;
    else if (*end < 0) { *end += len; if (*end < 0) *end = 0; }
    if (*start < 0) { *start += len; if (*start < 0) *start = 0; }
    return *end - *start;
}

Py_ssize_t piper_unicode_find_ucs4(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end, int direction) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u), m = PyUnicode_GET_LENGTH(sub);
    adjust_indices(&start, &end, n);
    if (end - start < m) return -1;
    int ku = PyUnicode_KIND(u), ks = PyUnicode_KIND(sub);
    void *du = PyUnicode_DATA(u), *ds = PyUnicode_DATA(sub);
    if (m == 0) return direction > 0 ? start : end;
    if (direction > 0) {
        for (Py_ssize_t i = start; i + m <= end; i++) {
            Py_ssize_t j = 0;
            while (j < m && PyUnicode_READ(ku, du, i + j) == PyUnicode_READ(ks, ds, j)) j++;
            if (j == m) return i;
        }
    } else {
        for (Py_ssize_t i = end - m; i >= start; i--) {
            Py_ssize_t j = 0;
            while (j < m && PyUnicode_READ(ku, du, i + j) == PyUnicode_READ(ks, ds, j)) j++;
            if (j == m) return i;
        }
    }
    return -1;
}
Py_ssize_t PyUnicode_Find(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end, int direction) {
    if (!PyUnicode_Check(sub)) { PyErr_Format(PyExc_TypeError, "must be str, not %s", Py_TYPE(sub)->tp_name); return -2; }
    return piper_unicode_find_ucs4(u, sub, start, end, direction);
}
Py_ssize_t PyUnicode_FindChar(PyObject *u, Py_UCS4 c, Py_ssize_t start, Py_ssize_t end, int direction) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    adjust_indices(&start, &end, n);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    if (direction > 0) { for (Py_ssize_t i = start; i < end; i++) if (PyUnicode_READ(kind, data, i) == c) return i; }
    else { for (Py_ssize_t i = end - 1; i >= start; i--) if (PyUnicode_READ(kind, data, i) == c) return i; }
    return -1;
}
Py_ssize_t PyUnicode_Count(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u), m = PyUnicode_GET_LENGTH(sub);
    adjust_indices(&start, &end, n);
    if (m == 0) return end - start + 1 > 0 ? end - start + 1 : 0;
    Py_ssize_t count = 0, i = start;
    while (i + m <= end) {
        Py_ssize_t f = piper_unicode_find_ucs4(u, sub, i, end, 1);
        if (f < 0) break;
        count++;
        i = f + m;
    }
    return count;
}
Py_ssize_t PyUnicode_Tailmatch(PyObject *u, PyObject *sub, Py_ssize_t start, Py_ssize_t end, int direction) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u), m = PyUnicode_GET_LENGTH(sub);
    adjust_indices(&start, &end, n);
    if (end - start < m || start > n) return 0;
    Py_ssize_t at = direction > 0 ? end - m : start;
    int ku = PyUnicode_KIND(u), ks = PyUnicode_KIND(sub);
    void *du = PyUnicode_DATA(u), *ds = PyUnicode_DATA(sub);
    for (Py_ssize_t j = 0; j < m; j++) if (PyUnicode_READ(ku, du, at + j) != PyUnicode_READ(ks, ds, j)) return 0;
    return 1;
}
int PyUnicode_Contains(PyObject *u, PyObject *sub) {
    if (!PyUnicode_Check(sub)) { PyErr_Format(PyExc_TypeError, "'in <string>' requires string as left operand, not %s", Py_TYPE(sub)->tp_name); return -1; }
    return piper_unicode_find_ucs4(u, sub, 0, PY_SSIZE_T_MAX, 1) >= 0;
}

/* ---- join / split / replace ---------------------------------------------- */

PyObject *_PyUnicode_JoinArray(PyObject *sep, PyObject *const *items, Py_ssize_t n) {
    if (n == 0) return PyUnicode_New(0, 0);
    if (n == 1 && PyUnicode_CheckExact(items[0])) return Py_NewRef(items[0]);
    Py_ssize_t seplen = sep ? PyUnicode_GET_LENGTH(sep) : 0;
    Py_UCS4 maxchar = sep && n > 1 ? PyUnicode_MAX_CHAR_VALUE(sep) : 0;
    Py_ssize_t total = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (!PyUnicode_Check(items[i])) { PyErr_Format(PyExc_TypeError, "sequence item %zd: expected str instance, %s found", i, Py_TYPE(items[i])->tp_name); return NULL; }
        total += PyUnicode_GET_LENGTH(items[i]);
        Py_UCS4 m = PyUnicode_MAX_CHAR_VALUE(items[i]);
        if (m > maxchar) maxchar = m;
    }
    total += seplen * (n - 1);
    PyObject *r = PyUnicode_New(total, maxchar);
    if (!r) return NULL;
    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (i && seplen) { PyUnicode_CopyCharacters(r, pos, sep, 0, seplen); pos += seplen; }
        Py_ssize_t l = PyUnicode_GET_LENGTH(items[i]);
        PyUnicode_CopyCharacters(r, pos, items[i], 0, l);
        pos += l;
    }
    return r;
}

PyObject *PyUnicode_Join(PyObject *sep, PyObject *seq) {
    PyObject *fast = PySequence_Fast(seq, "can only join an iterable");
    if (!fast) return NULL;
    PyObject *r = _PyUnicode_JoinArray(sep, PySequence_Fast_ITEMS(fast), PySequence_Fast_GET_SIZE(fast));
    Py_DECREF(fast);
    return r;
}

static int is_space(Py_UCS4 c) {
    return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
static int is_linebreak(Py_UCS4 c) { return c == '\n' || c == '\r' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e || c == 0x85 || c == 0x2028 || c == 0x2029; }

PyObject *PyUnicode_Split(PyObject *u, PyObject *sep, Py_ssize_t maxsplit) {
    PyObject *list = PyList_New(0);
    if (!list) return NULL;
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    if (maxsplit < 0) maxsplit = PY_SSIZE_T_MAX;
    if (!sep || sep == Py_None) {
        Py_ssize_t i = 0;
        while (i < n) {
            while (i < n && is_space(PyUnicode_READ(kind, data, i))) i++;
            if (i >= n) break;
            Py_ssize_t j = i;
            if (maxsplit == 0) j = n;
            else { while (j < n && !is_space(PyUnicode_READ(kind, data, j))) j++; }
            if (maxsplit == 0) { /* rest, right-stripped */ Py_ssize_t e = n; while (e > i && is_space(PyUnicode_READ(kind, data, e - 1))) e--; j = e; }
            PyObject *s = PyUnicode_Substring(u, i, j);
            if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
            Py_DECREF(s);
            maxsplit--;
            i = j;
        }
        return list;
    }
    if (!PyUnicode_Check(sep)) { Py_DECREF(list); PyErr_Format(PyExc_TypeError, "must be str or None, not %s", Py_TYPE(sep)->tp_name); return NULL; }
    Py_ssize_t m = PyUnicode_GET_LENGTH(sep);
    if (m == 0) { Py_DECREF(list); PyErr_SetString(PyExc_ValueError, "empty separator"); return NULL; }
    Py_ssize_t i = 0;
    while (maxsplit-- > 0) {
        Py_ssize_t f = piper_unicode_find_ucs4(u, sep, i, n, 1);
        if (f < 0) break;
        PyObject *s = PyUnicode_Substring(u, i, f);
        if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
        Py_DECREF(s);
        i = f + m;
    }
    PyObject *s = PyUnicode_Substring(u, i, n);
    if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
    Py_DECREF(s);
    return list;
}

PyObject *PyUnicode_RSplit(PyObject *u, PyObject *sep, Py_ssize_t maxsplit) {
    PyObject *list = PyList_New(0);
    if (!list) return NULL;
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    if (maxsplit < 0) maxsplit = PY_SSIZE_T_MAX;
    if (!sep || sep == Py_None) {
        Py_ssize_t i = n;
        while (i > 0) {
            while (i > 0 && is_space(PyUnicode_READ(kind, data, i - 1))) i--;
            if (i <= 0) break;
            Py_ssize_t j = i;
            if (maxsplit == 0) { Py_ssize_t b = 0; while (b < i && is_space(PyUnicode_READ(kind, data, b))) b++; j = b; }
            else { while (j > 0 && !is_space(PyUnicode_READ(kind, data, j - 1))) j--; }
            PyObject *s = PyUnicode_Substring(u, j, i);
            if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
            Py_DECREF(s);
            maxsplit--;
            i = j;
        }
        PyList_Reverse(list);
        return list;
    }
    if (!PyUnicode_Check(sep)) { Py_DECREF(list); PyErr_Format(PyExc_TypeError, "must be str or None, not %s", Py_TYPE(sep)->tp_name); return NULL; }
    Py_ssize_t m = PyUnicode_GET_LENGTH(sep);
    if (m == 0) { Py_DECREF(list); PyErr_SetString(PyExc_ValueError, "empty separator"); return NULL; }
    Py_ssize_t end = n;
    while (maxsplit-- > 0) {
        Py_ssize_t f = piper_unicode_find_ucs4(u, sep, 0, end, -1);
        if (f < 0) break;
        PyObject *s = PyUnicode_Substring(u, f + m, end);
        if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
        Py_DECREF(s);
        end = f;
    }
    PyObject *s = PyUnicode_Substring(u, 0, end);
    if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
    Py_DECREF(s);
    PyList_Reverse(list);
    return list;
}

PyObject *PyUnicode_Splitlines(PyObject *u, int keepends) {
    PyObject *list = PyList_New(0);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_ssize_t i = 0;
    while (i < n) {
        Py_ssize_t j = i;
        while (j < n && !is_linebreak(PyUnicode_READ(kind, data, j))) j++;
        Py_ssize_t eol = j;
        if (j < n) { if (PyUnicode_READ(kind, data, j) == '\r' && j + 1 < n && PyUnicode_READ(kind, data, j + 1) == '\n') j += 2; else j++; if (keepends) eol = j; }
        PyObject *s = PyUnicode_Substring(u, i, eol);
        if (!s || PyList_Append(list, s) < 0) { Py_XDECREF(s); Py_DECREF(list); return NULL; }
        Py_DECREF(s);
        i = j;
    }
    return list;
}

PyObject *PyUnicode_Replace(PyObject *u, PyObject *a, PyObject *b, Py_ssize_t maxcount) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u), m = PyUnicode_GET_LENGTH(a);
    if (maxcount < 0) maxcount = PY_SSIZE_T_MAX;
    PyObject *parts = PyList_New(0);
    Py_ssize_t i = 0;
    if (m == 0) {
        /* insert b between every character */
        Py_ssize_t count = 0;
        while (i <= n && count < maxcount) {
            PyList_Append(parts, b);
            count++;
            if (i < n) { PyObject *c = PyUnicode_Substring(u, i, i + 1); PyList_Append(parts, c); Py_DECREF(c); }
            i++;
        }
        if (i <= n) { PyObject *rest = PyUnicode_Substring(u, i, n); PyList_Append(parts, rest); Py_DECREF(rest); }
    } else {
        while (maxcount-- > 0) {
            Py_ssize_t f = piper_unicode_find_ucs4(u, a, i, n, 1);
            if (f < 0) break;
            PyObject *s = PyUnicode_Substring(u, i, f);
            PyList_Append(parts, s); Py_DECREF(s);
            PyList_Append(parts, b);
            i = f + m;
        }
        PyObject *s = PyUnicode_Substring(u, i, n);
        PyList_Append(parts, s); Py_DECREF(s);
    }
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty);
    Py_DECREF(parts);
    return r;
}

PyObject *PyUnicode_Partition(PyObject *u, PyObject *sep) {
    if (!PyUnicode_Check(sep)) { PyErr_Format(PyExc_TypeError, "must be str, not %s", Py_TYPE(sep)->tp_name); return NULL; }
    if (PyUnicode_GET_LENGTH(sep) == 0) { PyErr_SetString(PyExc_ValueError, "empty separator"); return NULL; }
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    Py_ssize_t f = piper_unicode_find_ucs4(u, sep, 0, n, 1);
    PyObject *a, *b, *c;
    if (f < 0) { a = Py_NewRef(u); b = PyUnicode_New(0, 0); c = PyUnicode_New(0, 0); }
    else { a = PyUnicode_Substring(u, 0, f); b = Py_NewRef(sep); c = PyUnicode_Substring(u, f + PyUnicode_GET_LENGTH(sep), n); }
    PyObject *t = PyTuple_Pack(3, a, b, c);
    Py_DECREF(a); Py_DECREF(b); Py_DECREF(c);
    return t;
}
PyObject *PyUnicode_RPartition(PyObject *u, PyObject *sep) {
    if (!PyUnicode_Check(sep)) { PyErr_Format(PyExc_TypeError, "must be str, not %s", Py_TYPE(sep)->tp_name); return NULL; }
    if (PyUnicode_GET_LENGTH(sep) == 0) { PyErr_SetString(PyExc_ValueError, "empty separator"); return NULL; }
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    Py_ssize_t f = piper_unicode_find_ucs4(u, sep, 0, n, -1);
    PyObject *a, *b, *c;
    if (f < 0) { a = PyUnicode_New(0, 0); b = PyUnicode_New(0, 0); c = Py_NewRef(u); }
    else { a = PyUnicode_Substring(u, 0, f); b = Py_NewRef(sep); c = PyUnicode_Substring(u, f + PyUnicode_GET_LENGTH(sep), n); }
    PyObject *t = PyTuple_Pack(3, a, b, c);
    Py_DECREF(a); Py_DECREF(b); Py_DECREF(c);
    return t;
}

/* ---- repr ------------------------------------------------------------------ */

extern int piper_unicode_isprintable(Py_UCS4 c);

PyObject *piper_unicode_repr(PyObject *u) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_ssize_t squotes = 0, dquotes = 0, out_len = 2;
    Py_UCS4 maxchar = 127;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        if (c == '\'') squotes++;
        else if (c == '"') dquotes++;
        if (c == '\\' || c == '\t' || c == '\r' || c == '\n') out_len += 2;
        else if (c < ' ' || c == 0x7f) out_len += 4;
        else if (c < 0x7f) out_len += 1;
        else if (piper_unicode_isprintable(c)) { out_len += 1; if (c > maxchar) maxchar = c; }
        else out_len += c < 0x100 ? 4 : c < 0x10000 ? 6 : 10;
    }
    Py_UCS4 quote = '\'';
    if (squotes && !dquotes) quote = '"';
    else if (squotes) out_len += squotes;
    PyObject *r = PyUnicode_New(out_len, maxchar);
    if (!r) return NULL;
    int rk = PyUnicode_KIND(r); void *rd = PyUnicode_DATA(r);
    Py_ssize_t j = 0;
    PyUnicode_WRITE(rk, rd, j++, quote);
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        if (c == quote || c == '\\') { PyUnicode_WRITE(rk, rd, j++, '\\'); PyUnicode_WRITE(rk, rd, j++, c); }
        else if (c == '\t') { PyUnicode_WRITE(rk, rd, j++, '\\'); PyUnicode_WRITE(rk, rd, j++, 't'); }
        else if (c == '\n') { PyUnicode_WRITE(rk, rd, j++, '\\'); PyUnicode_WRITE(rk, rd, j++, 'n'); }
        else if (c == '\r') { PyUnicode_WRITE(rk, rd, j++, '\\'); PyUnicode_WRITE(rk, rd, j++, 'r'); }
        else if (c < ' ' || c == 0x7f || (c >= 0x80 && !piper_unicode_isprintable(c))) {
            char buf[16];
            int len = c < 0x100 ? snprintf(buf, sizeof buf, "\\x%02x", c) : c < 0x10000 ? snprintf(buf, sizeof buf, "\\u%04x", c) : snprintf(buf, sizeof buf, "\\U%08x", c);
            for (int k = 0; k < len; k++) PyUnicode_WRITE(rk, rd, j++, (Py_UCS4)buf[k]);
        } else PyUnicode_WRITE(rk, rd, j++, c);
    }
    PyUnicode_WRITE(rk, rd, j++, quote);
    ((PyASCIIObject *)r)->length = j;
    return r;
}
static PyObject *unicode_repr(PyObject *u) { return piper_unicode_repr(u); }
static PyObject *unicode_str(PyObject *u) { return PyUnicode_CheckExact(u) ? Py_NewRef(u) : PyUnicode_Substring(u, 0, PyUnicode_GET_LENGTH(u)); }

/* ---- sequence / mapping slots ---------------------------------------------- */

static Py_ssize_t unicode_length(PyObject *u) { return PyUnicode_GET_LENGTH(u); }
static PyObject *unicode_getitem_index(PyObject *u, Py_ssize_t i) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (i < 0) i += n;
    if (i < 0 || i >= n) { PyErr_SetString(PyExc_IndexError, "string index out of range"); return NULL; }
    Py_UCS4 c = PyUnicode_READ_CHAR(u, i);
    return PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &c, 1);
}
static PyObject *unicode_subscript(PyObject *u, PyObject *item) {
    if (PyIndex_Check(item)) {
        Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred()) return NULL;
        return unicode_getitem_index(u, i);
    }
    if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, len;
        if (PySlice_GetIndicesEx(item, PyUnicode_GET_LENGTH(u), &start, &stop, &step, &len) < 0) return NULL;
        if (step == 1) return PyUnicode_Substring(u, start, stop);
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
        Py_UCS4 *buf = PyMem_Malloc(((size_t)len + 1) * 4);
        for (Py_ssize_t i = 0, cur = start; i < len; i++, cur += step) buf[i] = PyUnicode_READ(kind, data, cur);
        PyObject *r = piper_unicode_from_ucs4(buf, len);
        PyMem_Free(buf);
        return r;
    }
    PyErr_Format(PyExc_TypeError, "string indices must be integers, not '%s'", Py_TYPE(item)->tp_name);
    return NULL;
}
static int unicode_contains(PyObject *u, PyObject *sub) { return PyUnicode_Contains(u, sub); }

static PySequenceMethods unicode_as_sequence = { .sq_length = unicode_length, .sq_concat = unicode_concat, .sq_repeat = unicode_repeat, .sq_item = unicode_getitem_index, .sq_contains = unicode_contains };
static PyMappingMethods unicode_as_mapping = { .mp_length = unicode_length, .mp_subscript = unicode_subscript };

/* ---- iterator ----------------------------------------------------------------- */

typedef struct { PyObject_HEAD PyObject *seq; Py_ssize_t index; } unicodeiterobject;
static void unicodeiter_dealloc(PyObject *o) { Py_XDECREF(((unicodeiterobject *)o)->seq); PyObject_Free(o); }
static PyObject *unicodeiter_next(PyObject *o) {
    unicodeiterobject *it = (unicodeiterobject *)o;
    if (!it->seq) return NULL;
    if (it->index < PyUnicode_GET_LENGTH(it->seq)) return unicode_getitem_index(it->seq, it->index++);
    Py_CLEAR(it->seq);
    return NULL;
}
static PyObject *unicodeiter_len(PyObject *o, PyObject *u) { PIPER_UNUSED(u); unicodeiterobject *it = (unicodeiterobject *)o; return PyLong_FromSsize_t(it->seq ? PyUnicode_GET_LENGTH(it->seq) - it->index : 0); }
static PyMethodDef unicodeiter_methods[] = { { "__length_hint__", unicodeiter_len, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
PyTypeObject PyUnicodeIter_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "str_ascii_iterator", .tp_basicsize = sizeof(unicodeiterobject), .tp_dealloc = unicodeiter_dealloc,
    .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = unicodeiter_next, .tp_methods = unicodeiter_methods,
};
static PyObject *unicode_iter(PyObject *u) {
    unicodeiterobject *it = PyObject_New(unicodeiterobject, &PyUnicodeIter_Type);
    if (!it) return NULL;
    it->seq = Py_NewRef(u);
    it->index = 0;
    return (PyObject *)it;
}

/* ---- methods ------------------------------------------------------------------ */

extern Py_UCS4 piper_unicode_tolower(Py_UCS4 c);
extern Py_UCS4 piper_unicode_toupper(Py_UCS4 c);
extern int piper_unicode_isalpha(Py_UCS4 c);
extern int piper_unicode_isdecimal(Py_UCS4 c);
extern int piper_unicode_isdigit(Py_UCS4 c);
extern int piper_unicode_isnumeric(Py_UCS4 c);
extern int piper_unicode_islower(Py_UCS4 c);
extern int piper_unicode_isupper(Py_UCS4 c);
extern int piper_unicode_istitle_char(Py_UCS4 c);
extern int piper_unicode_todecimal(Py_UCS4 c);

static PyObject *map_chars(PyObject *u, Py_UCS4 (*f)(Py_UCS4)) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_UCS4 *buf = PyMem_Malloc(((size_t)n + 1) * 4);
    for (Py_ssize_t i = 0; i < n; i++) buf[i] = f(PyUnicode_READ(kind, data, i));
    PyObject *r = piper_unicode_from_ucs4(buf, n);
    PyMem_Free(buf);
    return r;
}
static PyObject *str_upper(PyObject *u, PyObject *x) { PIPER_UNUSED(x); return map_chars(u, piper_unicode_toupper); }
static PyObject *str_lower(PyObject *u, PyObject *x) { PIPER_UNUSED(x); return map_chars(u, piper_unicode_tolower); }
static PyObject *str_casefold(PyObject *u, PyObject *x) { PIPER_UNUSED(x); return map_chars(u, piper_unicode_tolower); }
static Py_UCS4 swapcase_char(Py_UCS4 c) { if (piper_unicode_isupper(c)) return piper_unicode_tolower(c); if (piper_unicode_islower(c)) return piper_unicode_toupper(c); return c; }
static PyObject *str_swapcase(PyObject *u, PyObject *x) { PIPER_UNUSED(x); return map_chars(u, swapcase_char); }
static PyObject *str_capitalize(PyObject *u, PyObject *x) {
    PIPER_UNUSED(x);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_UCS4 *buf = PyMem_Malloc(((size_t)n + 1) * 4);
    for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); buf[i] = i == 0 ? piper_unicode_toupper(c) : piper_unicode_tolower(c); }
    PyObject *r = piper_unicode_from_ucs4(buf, n);
    PyMem_Free(buf);
    return r;
}
static PyObject *str_title(PyObject *u, PyObject *x) {
    PIPER_UNUSED(x);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_UCS4 *buf = PyMem_Malloc(((size_t)n + 1) * 4);
    int prev_cased = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        int cased = piper_unicode_isupper(c) || piper_unicode_islower(c) || piper_unicode_istitle_char(c);
        buf[i] = prev_cased ? piper_unicode_tolower(c) : piper_unicode_toupper(c);
        prev_cased = cased;
    }
    PyObject *r = piper_unicode_from_ucs4(buf, n);
    PyMem_Free(buf);
    return r;
}

static int parse_sub_range(const char *name, PyObject *const *args, Py_ssize_t nargs, PyObject **sub, Py_ssize_t *start, Py_ssize_t *end) {
    if (nargs < 1 || nargs > 3) { PyErr_Format(PyExc_TypeError, "%s() takes at least 1 argument (%zd given)", name, nargs); return -1; }
    *sub = args[0];
    *start = 0; *end = PY_SSIZE_T_MAX;
    if (nargs >= 2 && args[1] != Py_None) { *start = PyNumber_AsSsize_t(args[1], NULL); if (*start == -1 && PyErr_Occurred()) return -1; }
    if (nargs >= 3 && args[2] != Py_None) { *end = PyNumber_AsSsize_t(args[2], NULL); if (*end == -1 && PyErr_Occurred()) return -1; }
    return 0;
}

static PyObject *str_find_impl(PyObject *u, PyObject *const *args, Py_ssize_t nargs, int dir, int raise, const char *name) {
    PyObject *sub; Py_ssize_t start, end;
    if (parse_sub_range(name, args, nargs, &sub, &start, &end) < 0) return NULL;
    if (!PyUnicode_Check(sub)) { PyErr_Format(PyExc_TypeError, "must be str, not %s", Py_TYPE(sub)->tp_name); return NULL; }
    Py_ssize_t r = piper_unicode_find_ucs4(u, sub, start, end, dir);
    if (r < 0 && raise) { PyErr_SetString(PyExc_ValueError, "substring not found"); return NULL; }
    return PyLong_FromSsize_t(r);
}
static PyObject *str_find(PyObject *u, PyObject *const *a, Py_ssize_t n) { return str_find_impl(u, a, n, 1, 0, "find"); }
static PyObject *str_rfind(PyObject *u, PyObject *const *a, Py_ssize_t n) { return str_find_impl(u, a, n, -1, 0, "rfind"); }
static PyObject *str_index(PyObject *u, PyObject *const *a, Py_ssize_t n) { return str_find_impl(u, a, n, 1, 1, "index"); }
static PyObject *str_rindex(PyObject *u, PyObject *const *a, Py_ssize_t n) { return str_find_impl(u, a, n, -1, 1, "rindex"); }
static PyObject *str_count(PyObject *u, PyObject *const *args, Py_ssize_t nargs) {
    PyObject *sub; Py_ssize_t start, end;
    if (parse_sub_range("count", args, nargs, &sub, &start, &end) < 0) return NULL;
    if (!PyUnicode_Check(sub)) { PyErr_Format(PyExc_TypeError, "must be str, not %s", Py_TYPE(sub)->tp_name); return NULL; }
    return PyLong_FromSsize_t(PyUnicode_Count(u, sub, start, end));
}
static PyObject *str_tailmatch(PyObject *u, PyObject *const *args, Py_ssize_t nargs, int dir, const char *name) {
    PyObject *sub; Py_ssize_t start, end;
    if (parse_sub_range(name, args, nargs, &sub, &start, &end) < 0) return NULL;
    if (PyTuple_Check(sub)) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(sub); i++) {
            PyObject *s = PyTuple_GET_ITEM(sub, i);
            if (!PyUnicode_Check(s)) { PyErr_Format(PyExc_TypeError, "tuple for %s must only contain str, not %s", name, Py_TYPE(s)->tp_name); return NULL; }
            if (PyUnicode_Tailmatch(u, s, start, end, dir)) Py_RETURN_TRUE;
        }
        Py_RETURN_FALSE;
    }
    if (!PyUnicode_Check(sub)) { PyErr_Format(PyExc_TypeError, "%s first arg must be str or a tuple of str, not %s", name, Py_TYPE(sub)->tp_name); return NULL; }
    Py_RETURN_BOOL(PyUnicode_Tailmatch(u, sub, start, end, dir));
}
static PyObject *str_startswith(PyObject *u, PyObject *const *a, Py_ssize_t n) { return str_tailmatch(u, a, n, -1, "startswith"); }
static PyObject *str_endswith(PyObject *u, PyObject *const *a, Py_ssize_t n) { return str_tailmatch(u, a, n, 1, "endswith"); }

static PyObject *do_strip(PyObject *u, PyObject *chars, int left, int right) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_ssize_t i = 0, j = n;
    if (!chars || chars == Py_None) {
        if (left) while (i < n && is_space(PyUnicode_READ(kind, data, i))) i++;
        if (right) while (j > i && is_space(PyUnicode_READ(kind, data, j - 1))) j--;
    } else {
        if (!PyUnicode_Check(chars)) { PyErr_Format(PyExc_TypeError, "strip arg must be None or str"); return NULL; }
        Py_ssize_t m = PyUnicode_GET_LENGTH(chars);
        int ck = PyUnicode_KIND(chars); void *cd = PyUnicode_DATA(chars);
#define IN_CHARS(c) ({ int _f = 0; for (Py_ssize_t _k = 0; _k < m; _k++) if (PyUnicode_READ(ck, cd, _k) == (c)) { _f = 1; break; } _f; })
        if (left) while (i < n && IN_CHARS(PyUnicode_READ(kind, data, i))) i++;
        if (right) while (j > i && IN_CHARS(PyUnicode_READ(kind, data, j - 1))) j--;
#undef IN_CHARS
    }
    return PyUnicode_Substring(u, i, j);
}
static PyObject *str_strip(PyObject *u, PyObject *const *a, Py_ssize_t n) { return do_strip(u, n ? a[0] : NULL, 1, 1); }
static PyObject *str_lstrip(PyObject *u, PyObject *const *a, Py_ssize_t n) { return do_strip(u, n ? a[0] : NULL, 1, 0); }
static PyObject *str_rstrip(PyObject *u, PyObject *const *a, Py_ssize_t n) { return do_strip(u, n ? a[0] : NULL, 0, 1); }

static PyObject *str_split_impl(PyObject *u, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames, int rsplit) {
    PyObject *sep = NULL;
    Py_ssize_t maxsplit = -1;
    if (nargs >= 1) sep = args[0];
    if (nargs >= 2) { maxsplit = PyNumber_AsSsize_t(args[1], NULL); if (maxsplit == -1 && PyErr_Occurred()) return NULL; }
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kwnames, i);
        if (PyUnicode_EqualToUTF8(k, "sep")) sep = args[nargs + i];
        else if (PyUnicode_EqualToUTF8(k, "maxsplit")) { maxsplit = PyNumber_AsSsize_t(args[nargs + i], NULL); if (maxsplit == -1 && PyErr_Occurred()) return NULL; }
        else { PyErr_Format(PyExc_TypeError, "split() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    return rsplit ? PyUnicode_RSplit(u, sep, maxsplit) : PyUnicode_Split(u, sep, maxsplit);
}
static PyObject *str_split(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) { return str_split_impl(u, a, n, kw, 0); }
static PyObject *str_rsplit(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) { return str_split_impl(u, a, n, kw, 1); }
static PyObject *str_splitlines(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    int keepends = 0;
    if (n >= 1) { keepends = PyObject_IsTrue(a[0]); if (keepends < 0) return NULL; }
    if (kw && PyTuple_GET_SIZE(kw) == 1) { keepends = PyObject_IsTrue(a[n]); if (keepends < 0) return NULL; }
    return PyUnicode_Splitlines(u, keepends);
}
static PyObject *str_join(PyObject *u, PyObject *seq) { return PyUnicode_Join(u, seq); }
static PyObject *str_replace(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    if (n < 2 || n > 3) { PyErr_Format(PyExc_TypeError, "replace expected at least 2 arguments, got %zd", n); return NULL; }
    if (!PyUnicode_Check(a[0]) || !PyUnicode_Check(a[1])) { PyErr_SetString(PyExc_TypeError, "replace() arguments must be str"); return NULL; }
    Py_ssize_t count = -1;
    if (n == 3) { count = PyNumber_AsSsize_t(a[2], NULL); if (count == -1 && PyErr_Occurred()) return NULL; }
    if (kw && PyTuple_GET_SIZE(kw)) { count = PyNumber_AsSsize_t(a[n], NULL); if (count == -1 && PyErr_Occurred()) return NULL; }
    return PyUnicode_Replace(u, a[0], a[1], count);
}
static PyObject *str_partition(PyObject *u, PyObject *sep) { return PyUnicode_Partition(u, sep); }
static PyObject *str_rpartition(PyObject *u, PyObject *sep) { return PyUnicode_RPartition(u, sep); }

static PyObject *pad(PyObject *u, Py_ssize_t left, Py_ssize_t right, Py_UCS4 fill) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (left <= 0 && right <= 0) return PyUnicode_CheckExact(u) ? Py_NewRef(u) : PyUnicode_Substring(u, 0, n);
    if (left < 0) left = 0;
    if (right < 0) right = 0;
    Py_UCS4 maxchar = PyUnicode_MAX_CHAR_VALUE(u);
    if (fill > maxchar) maxchar = fill;
    PyObject *r = PyUnicode_New(left + n + right, maxchar);
    if (!r) return NULL;
    PyUnicode_Fill(r, 0, left, fill);
    PyUnicode_CopyCharacters(r, left, u, 0, n);
    PyUnicode_Fill(r, left + n, right, fill);
    return r;
}
static int parse_width_fill(const char *name, PyObject *const *a, Py_ssize_t n, Py_ssize_t *width, Py_UCS4 *fill) {
    if (n < 1 || n > 2) { PyErr_Format(PyExc_TypeError, "%s() takes at least 1 argument (%zd given)", name, n); return -1; }
    *width = PyNumber_AsSsize_t(a[0], PyExc_OverflowError);
    if (*width == -1 && PyErr_Occurred()) return -1;
    *fill = ' ';
    if (n == 2) {
        if (!PyUnicode_Check(a[1]) || PyUnicode_GET_LENGTH(a[1]) != 1) { PyErr_SetString(PyExc_TypeError, "The fill character must be exactly one character long"); return -1; }
        *fill = PyUnicode_READ_CHAR(a[1], 0);
    }
    return 0;
}
static PyObject *str_center(PyObject *u, PyObject *const *a, Py_ssize_t n) {
    Py_ssize_t w; Py_UCS4 f;
    if (parse_width_fill("center", a, n, &w, &f) < 0) return NULL;
    Py_ssize_t len = PyUnicode_GET_LENGTH(u);
    Py_ssize_t marg = w - len;
    if (marg <= 0) return pad(u, 0, 0, f);
    Py_ssize_t left = marg / 2 + (marg & w & 1);
    return pad(u, left, marg - left, f);
}
static PyObject *str_ljust(PyObject *u, PyObject *const *a, Py_ssize_t n) { Py_ssize_t w; Py_UCS4 f; if (parse_width_fill("ljust", a, n, &w, &f) < 0) return NULL; return pad(u, 0, w - PyUnicode_GET_LENGTH(u), f); }
static PyObject *str_rjust(PyObject *u, PyObject *const *a, Py_ssize_t n) { Py_ssize_t w; Py_UCS4 f; if (parse_width_fill("rjust", a, n, &w, &f) < 0) return NULL; return pad(u, w - PyUnicode_GET_LENGTH(u), 0, f); }
static PyObject *str_zfill(PyObject *u, PyObject *arg) {
    Py_ssize_t w = PyNumber_AsSsize_t(arg, PyExc_OverflowError);
    if (w == -1 && PyErr_Occurred()) return NULL;
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (n >= w) return pad(u, 0, 0, '0');
    PyObject *r = pad(u, w - n, 0, '0');
    if (!r) return NULL;
    if (n > 0) {
        Py_UCS4 c = PyUnicode_READ_CHAR(r, w - n);
        if (c == '+' || c == '-') { PyUnicode_WriteChar(r, 0, c); PyUnicode_WriteChar(r, w - n, '0'); }
    }
    return r;
}
static PyObject *str_expandtabs(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    Py_ssize_t tabsize = 8;
    if (n >= 1) { tabsize = PyNumber_AsSsize_t(a[0], NULL); if (tabsize == -1 && PyErr_Occurred()) return NULL; }
    if (kw && PyTuple_GET_SIZE(kw)) { tabsize = PyNumber_AsSsize_t(a[n], NULL); if (tabsize == -1 && PyErr_Occurred()) return NULL; }
    Py_ssize_t len = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    Py_ssize_t cap = len * (tabsize > 1 ? tabsize : 1) + 1;
    Py_UCS4 *buf = PyMem_Malloc((size_t)cap * 4);
    Py_ssize_t j = 0, col = 0;
    for (Py_ssize_t i = 0; i < len; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        if (c == '\t') { if (tabsize > 0) { Py_ssize_t incr = tabsize - (col % tabsize); col += incr; while (incr--) buf[j++] = ' '; } }
        else { buf[j++] = c; col++; if (c == '\n' || c == '\r') col = 0; }
    }
    PyObject *r = piper_unicode_from_ucs4(buf, j);
    PyMem_Free(buf);
    return r;
}

#define STR_PREDICATE(name, cond, empty) \
    static PyObject *str_##name(PyObject *u, PyObject *x) { \
        PIPER_UNUSED(x); \
        Py_ssize_t n = PyUnicode_GET_LENGTH(u); \
        if (n == 0) { if (empty) Py_RETURN_TRUE; else Py_RETURN_FALSE; } \
        int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u); \
        for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (!(cond)) Py_RETURN_FALSE; } \
        Py_RETURN_TRUE; \
    }
STR_PREDICATE(isspace, is_space(c), 0)
STR_PREDICATE(isalpha, piper_unicode_isalpha(c), 0)
STR_PREDICATE(isdecimal, piper_unicode_isdecimal(c), 0)
STR_PREDICATE(isdigit, piper_unicode_isdigit(c), 0)
STR_PREDICATE(isnumeric, piper_unicode_isnumeric(c), 0)
STR_PREDICATE(isalnum, piper_unicode_isalpha(c) || piper_unicode_isdecimal(c) || piper_unicode_isdigit(c) || piper_unicode_isnumeric(c), 0)
STR_PREDICATE(isascii, c < 128, 1)
STR_PREDICATE(isprintable, piper_unicode_isprintable(c), 1)
static PyObject *str_islower(PyObject *u, PyObject *x) {
    PIPER_UNUSED(x);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u); int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u); int cased = 0;
    for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (piper_unicode_isupper(c) || piper_unicode_istitle_char(c)) Py_RETURN_FALSE; if (piper_unicode_islower(c)) cased = 1; }
    Py_RETURN_BOOL(cased);
}
static PyObject *str_isupper(PyObject *u, PyObject *x) {
    PIPER_UNUSED(x);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u); int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u); int cased = 0;
    for (Py_ssize_t i = 0; i < n; i++) { Py_UCS4 c = PyUnicode_READ(kind, data, i); if (piper_unicode_islower(c) || piper_unicode_istitle_char(c)) Py_RETURN_FALSE; if (piper_unicode_isupper(c)) cased = 1; }
    Py_RETURN_BOOL(cased);
}
static PyObject *str_istitle(PyObject *u, PyObject *x) {
    PIPER_UNUSED(x);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u); int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    int cased = 0, prev_cased = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        if (piper_unicode_isupper(c) || piper_unicode_istitle_char(c)) { if (prev_cased) Py_RETURN_FALSE; prev_cased = 1; cased = 1; }
        else if (piper_unicode_islower(c)) { if (!prev_cased) Py_RETURN_FALSE; prev_cased = 1; cased = 1; }
        else prev_cased = 0;
    }
    Py_RETURN_BOOL(cased);
}
static PyObject *str_isidentifier(PyObject *u, PyObject *x) { PIPER_UNUSED(x); Py_RETURN_BOOL(PyUnicode_IsIdentifier(u)); }

static PyObject *str_encode(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) {
    const char *enc = "utf-8", *errors = "strict";
    if (n >= 1) { if (!PyUnicode_Check(a[0])) { PyErr_SetString(PyExc_TypeError, "encode() argument 'encoding' must be str"); return NULL; } enc = PyUnicode_AsUTF8(a[0]); }
    if (n >= 2) { if (!PyUnicode_Check(a[1])) { PyErr_SetString(PyExc_TypeError, "encode() argument 'errors' must be str"); return NULL; } errors = PyUnicode_AsUTF8(a[1]); }
    Py_ssize_t nkw = kw ? PyTuple_GET_SIZE(kw) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *k = PyTuple_GET_ITEM(kw, i);
        if (PyUnicode_EqualToUTF8(k, "encoding")) enc = PyUnicode_AsUTF8(a[n + i]);
        else if (PyUnicode_EqualToUTF8(k, "errors")) errors = PyUnicode_AsUTF8(a[n + i]);
        else { PyErr_Format(PyExc_TypeError, "encode() got an unexpected keyword argument '%U'", k); return NULL; }
    }
    return PyUnicode_AsEncodedString(u, enc, errors);
}

static PyObject *str_format_map(PyObject *u, PyObject *mapping) {
    PyObject *args[2] = { u, mapping };
    extern PyObject *piper_str_format_with_mapping(PyObject *self, PyObject *mapping);
    PIPER_UNUSED(args);
    return piper_str_format_with_mapping(u, mapping);
}
static PyObject *str_format(PyObject *u, PyObject *const *a, Py_ssize_t n, PyObject *kw) { return piper_str_format_method(u, a, n, kw); }
static PyObject *str_dunder_format(PyObject *u, PyObject *const *a, Py_ssize_t n) {
    if (piper_args_range("__format__", n, 1, 1) < 0) return NULL;
    if (!PyUnicode_Check(a[0])) { PyErr_SetString(PyExc_TypeError, "__format__() argument must be str"); return NULL; }
    return piper_format_spec_apply(u, a[0]);
}
static PyObject *str_mod(PyObject *u, PyObject *args) { if (!PyUnicode_Check(u)) Py_RETURN_NOTIMPLEMENTED; return PyUnicode_Format(u, args); }
static PyObject *str_getnewargs(PyObject *u, PyObject *x) { PIPER_UNUSED(x); PyObject *c = PyUnicode_Substring(u, 0, PyUnicode_GET_LENGTH(u)); PyObject *t = PyTuple_Pack(1, c); Py_DECREF(c); return t; }
static PyObject *str_sizeof(PyObject *u, PyObject *x) { PIPER_UNUSED(x); return PyLong_FromSsize_t((Py_ssize_t)sizeof(PyCompactUnicodeObject) + (PyUnicode_GET_LENGTH(u) + 1) * PyUnicode_KIND(u)); }

static PyObject *str_translate(PyObject *u, PyObject *table) { return PyUnicode_Translate(u, table, NULL); }
static PyObject *str_maketrans(PyObject *cls, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(cls);
    PyObject *d = PyDict_New();
    if (n == 1) {
        if (!PyDict_Check(a[0])) { PyErr_SetString(PyExc_TypeError, "if you give only one argument to maketrans it must be a dict"); Py_DECREF(d); return NULL; }
        Py_ssize_t pos = 0; PyObject *k, *v;
        while (PyDict_Next(a[0], &pos, &k, &v)) {
            PyObject *key = k;
            if (PyUnicode_Check(k)) { if (PyUnicode_GET_LENGTH(k) != 1) { PyErr_SetString(PyExc_ValueError, "string keys in translate table must be of length 1"); Py_DECREF(d); return NULL; } key = PyLong_FromLong((long)PyUnicode_READ_CHAR(k, 0)); } else Py_INCREF(key);
            PyDict_SetItem(d, key, v);
            Py_DECREF(key);
        }
        return d;
    }
    if (n < 2 || n > 3 || !PyUnicode_Check(a[0]) || !PyUnicode_Check(a[1]) || PyUnicode_GET_LENGTH(a[0]) != PyUnicode_GET_LENGTH(a[1])) { PyErr_SetString(PyExc_ValueError, "the first two maketrans arguments must have equal length"); Py_DECREF(d); return NULL; }
    for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(a[0]); i++) {
        PyObject *k = PyLong_FromLong((long)PyUnicode_READ_CHAR(a[0], i)), *v = PyLong_FromLong((long)PyUnicode_READ_CHAR(a[1], i));
        PyDict_SetItem(d, k, v); Py_DECREF(k); Py_DECREF(v);
    }
    if (n == 3) for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(a[2]); i++) { PyObject *k = PyLong_FromLong((long)PyUnicode_READ_CHAR(a[2], i)); PyDict_SetItem(d, k, Py_None); Py_DECREF(k); }
    return d;
}
PyObject *PyUnicode_Translate(PyObject *u, PyObject *table, const char *errors) {
    PIPER_UNUSED(errors);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    int kind = PyUnicode_KIND(u); void *data = PyUnicode_DATA(u);
    PyObject *parts = PyList_New(0);
    for (Py_ssize_t i = 0; i < n; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, data, i);
        PyObject *k = PyLong_FromLong((long)c);
        PyObject *v = PyObject_GetItem(table, k);
        Py_DECREF(k);
        if (!v) {
            if (PyErr_ExceptionMatches(PyExc_LookupError)) { PyErr_Clear(); PyObject *s = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &c, 1); PyList_Append(parts, s); Py_DECREF(s); continue; }
            Py_DECREF(parts); return NULL;
        }
        if (v == Py_None) { Py_DECREF(v); continue; }
        if (PyLong_Check(v)) { long o = PyLong_AsLong(v); Py_DECREF(v); if (o < 0 || o > 0x10ffff) { PyErr_SetString(PyExc_ValueError, "character mapping must be in range(0x110000)"); Py_DECREF(parts); return NULL; } Py_UCS4 oc = (Py_UCS4)o; PyObject *s = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &oc, 1); PyList_Append(parts, s); Py_DECREF(s); }
        else if (PyUnicode_Check(v)) { PyList_Append(parts, v); Py_DECREF(v); }
        else { Py_DECREF(v); Py_DECREF(parts); PyErr_SetString(PyExc_TypeError, "character mapping must return integer, None or str"); return NULL; }
    }
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts);
    return r;
}
static PyObject *str_removeprefix(PyObject *u, PyObject *p) {
    if (!PyUnicode_Check(p)) { PyErr_Format(PyExc_TypeError, "removeprefix() argument must be str, not %s", Py_TYPE(p)->tp_name); return NULL; }
    if (PyUnicode_Tailmatch(u, p, 0, PY_SSIZE_T_MAX, -1)) return PyUnicode_Substring(u, PyUnicode_GET_LENGTH(p), PyUnicode_GET_LENGTH(u));
    return unicode_str(u);
}
static PyObject *str_removesuffix(PyObject *u, PyObject *p) {
    if (!PyUnicode_Check(p)) { PyErr_Format(PyExc_TypeError, "removesuffix() argument must be str, not %s", Py_TYPE(p)->tp_name); return NULL; }
    if (PyUnicode_GET_LENGTH(p) && PyUnicode_Tailmatch(u, p, 0, PY_SSIZE_T_MAX, 1)) return PyUnicode_Substring(u, 0, PyUnicode_GET_LENGTH(u) - PyUnicode_GET_LENGTH(p));
    return unicode_str(u);
}

int PyUnicode_IsIdentifier(PyObject *u) {
    extern int piper_unicode_is_xid_start(Py_UCS4 c);
    extern int piper_unicode_is_xid_continue(Py_UCS4 c);
    Py_ssize_t n = PyUnicode_GET_LENGTH(u);
    if (n == 0) return 0;
    Py_UCS4 c = PyUnicode_READ_CHAR(u, 0);
    if (c != '_' && !piper_unicode_is_xid_start(c)) return 0;
    for (Py_ssize_t i = 1; i < n; i++) if (!piper_unicode_is_xid_continue(PyUnicode_READ_CHAR(u, i))) return 0;
    return 1;
}

static PyMethodDef unicode_methods[] = {
    { "upper", str_upper, METH_NOARGS, NULL }, { "lower", str_lower, METH_NOARGS, NULL }, { "casefold", str_casefold, METH_NOARGS, NULL },
    { "swapcase", str_swapcase, METH_NOARGS, NULL }, { "capitalize", str_capitalize, METH_NOARGS, NULL }, { "title", str_title, METH_NOARGS, NULL },
    { "find", (PyCFunction)(void (*)(void))str_find, METH_FASTCALL, NULL }, { "rfind", (PyCFunction)(void (*)(void))str_rfind, METH_FASTCALL, NULL },
    { "index", (PyCFunction)(void (*)(void))str_index, METH_FASTCALL, NULL }, { "rindex", (PyCFunction)(void (*)(void))str_rindex, METH_FASTCALL, NULL },
    { "count", (PyCFunction)(void (*)(void))str_count, METH_FASTCALL, NULL },
    { "startswith", (PyCFunction)(void (*)(void))str_startswith, METH_FASTCALL, NULL }, { "endswith", (PyCFunction)(void (*)(void))str_endswith, METH_FASTCALL, NULL },
    { "strip", (PyCFunction)(void (*)(void))str_strip, METH_FASTCALL, NULL }, { "lstrip", (PyCFunction)(void (*)(void))str_lstrip, METH_FASTCALL, NULL }, { "rstrip", (PyCFunction)(void (*)(void))str_rstrip, METH_FASTCALL, NULL },
    { "split", (PyCFunction)(void (*)(void))str_split, METH_FASTCALL | METH_KEYWORDS, NULL }, { "rsplit", (PyCFunction)(void (*)(void))str_rsplit, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "splitlines", (PyCFunction)(void (*)(void))str_splitlines, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "join", str_join, METH_O, NULL }, { "replace", (PyCFunction)(void (*)(void))str_replace, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "partition", str_partition, METH_O, NULL }, { "rpartition", str_rpartition, METH_O, NULL },
    { "center", (PyCFunction)(void (*)(void))str_center, METH_FASTCALL, NULL }, { "ljust", (PyCFunction)(void (*)(void))str_ljust, METH_FASTCALL, NULL }, { "rjust", (PyCFunction)(void (*)(void))str_rjust, METH_FASTCALL, NULL },
    { "zfill", str_zfill, METH_O, NULL }, { "expandtabs", (PyCFunction)(void (*)(void))str_expandtabs, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "isspace", str_isspace, METH_NOARGS, NULL }, { "isalpha", str_isalpha, METH_NOARGS, NULL }, { "isdecimal", str_isdecimal, METH_NOARGS, NULL },
    { "isdigit", str_isdigit, METH_NOARGS, NULL }, { "isnumeric", str_isnumeric, METH_NOARGS, NULL }, { "isalnum", str_isalnum, METH_NOARGS, NULL },
    { "isascii", str_isascii, METH_NOARGS, NULL }, { "isprintable", str_isprintable, METH_NOARGS, NULL }, { "islower", str_islower, METH_NOARGS, NULL },
    { "isupper", str_isupper, METH_NOARGS, NULL }, { "istitle", str_istitle, METH_NOARGS, NULL }, { "isidentifier", str_isidentifier, METH_NOARGS, NULL },
    { "encode", (PyCFunction)(void (*)(void))str_encode, METH_FASTCALL | METH_KEYWORDS, NULL },
    { "format", (PyCFunction)(void (*)(void))str_format, METH_FASTCALL | METH_KEYWORDS, NULL }, { "format_map", str_format_map, METH_O, NULL },
    { "__format__", (PyCFunction)(void (*)(void))str_dunder_format, METH_FASTCALL, NULL },
    { "translate", str_translate, METH_O, NULL }, { "maketrans", (PyCFunction)(void (*)(void))str_maketrans, METH_FASTCALL | METH_STATIC, NULL },
    { "removeprefix", str_removeprefix, METH_O, NULL }, { "removesuffix", str_removesuffix, METH_O, NULL },
    { "__getnewargs__", str_getnewargs, METH_NOARGS, NULL }, { "__sizeof__", str_sizeof, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

static PyNumberMethods unicode_as_number = { .nb_remainder = str_mod };

static PyObject *unicode_new(PyTypeObject *tp, PyObject *args, PyObject *kwds) {
    PyObject *x = NULL, *encoding = NULL, *errors = NULL;
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n > 3) { PyErr_Format(PyExc_TypeError, "str() takes at most 3 arguments (%zd given)", n); return NULL; }
    if (n >= 1) x = PyTuple_GET_ITEM(args, 0);
    if (n >= 2) encoding = PyTuple_GET_ITEM(args, 1);
    if (n >= 3) errors = PyTuple_GET_ITEM(args, 2);
    if (kwds) {
        PyObject *v;
        if ((v = PyDict_GetItemString(kwds, "object"))) x = v;
        if ((v = PyDict_GetItemString(kwds, "encoding"))) encoding = v;
        if ((v = PyDict_GetItemString(kwds, "errors"))) errors = v;
    }
    PyObject *r;
    if (!x) r = PyUnicode_New(0, 0);
    else if (!encoding && !errors) r = PyObject_Str(x);
    else {
        if (PyUnicode_Check(x)) { PyErr_SetString(PyExc_TypeError, "decoding str is not supported"); return NULL; }
        r = PyUnicode_FromEncodedObject(x, encoding ? PyUnicode_AsUTF8(encoding) : "utf-8", errors ? PyUnicode_AsUTF8(errors) : "strict");
    }
    if (!r || tp == &PyUnicode_Type) return r;
    /* subclass: non-compact object with a copied buffer */
    Py_ssize_t len = PyUnicode_GET_LENGTH(r);
    int kind = PyUnicode_KIND(r);
    PyUnicodeObject *sub = (PyUnicodeObject *)tp->tp_alloc(tp, 0);
    if (!sub) { Py_DECREF(r); return NULL; }
    PyASCIIObject *a = &sub->_base._base;
    a->length = len; a->hash = -1;
    a->state.kind = (unsigned)kind; a->state.compact = 0; a->state.ascii = PyUnicode_IS_ASCII(r); a->state.interned = 0; a->state.statically_allocated = 0;
    sub->_base.utf8 = NULL; sub->_base.utf8_length = 0;
    sub->data.any = PyMem_Malloc(((size_t)len + 1) * (size_t)kind);
    memcpy(sub->data.any, PyUnicode_DATA(r), ((size_t)len + 1) * (size_t)kind);
    Py_DECREF(r);
    return (PyObject *)sub;
}

PyTypeObject PyUnicode_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "str",
    .tp_basicsize = sizeof(PyUnicodeObject),
    .tp_dealloc = unicode_dealloc,
    .tp_repr = unicode_repr,
    .tp_as_number = &unicode_as_number,
    .tp_as_sequence = &unicode_as_sequence,
    .tp_as_mapping = &unicode_as_mapping,
    .tp_hash = unicode_hash,
    .tp_str = unicode_str,
    .tp_getattro = PyObject_GenericGetAttr,
    .tp_flags = Py_TPFLAGS_BASETYPE | Py_TPFLAGS_UNICODE_SUBCLASS | Py_TPFLAGS_MATCH_SELF,
    .tp_richcompare = PyUnicode_RichCompare,
    .tp_iter = unicode_iter,
    .tp_methods = unicode_methods,
    .tp_alloc = PyType_GenericAlloc,
    .tp_new = unicode_new,
    .tp_free = PyObject_Free,
};

/* ---- interning ------------------------------------------------------------- */

static PyObject *interned = NULL;
PyObject *PyUnicode_InternFromString(const char *s) {
    PyObject *u = PyUnicode_FromString(s);
    if (!u) return NULL;
    PyUnicode_InternInPlace(&u);
    return u;
}
void PyUnicode_InternInPlace(PyObject **p) {
    PyObject *s = *p;
    if (!s || !PyUnicode_CheckExact(s) || ((PyASCIIObject *)s)->state.interned) return;
    if (!interned) { interned = PyDict_New(); if (!interned) { PyErr_Clear(); return; } }
    PyObject *t = PyDict_SetDefault(interned, s, s);
    if (!t) { PyErr_Clear(); return; }
    if (t != s) { Py_INCREF(t); Py_SETREF(*p, t); return; }
    ((PyASCIIObject *)s)->state.interned = 2;
    s->ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT;
}
void PyUnicode_InternImmortal(PyObject **p) { PyUnicode_InternInPlace(p); }
PyObject *piper_intern(const char *s) {
    if (!interned) { interned = PyDict_New(); if (!interned) return NULL; }
    PyObject *u = PyUnicode_FromString(s);
    if (!u) return NULL;
    PyObject *t = PyDict_GetItemWithError(interned, u);
    if (t) { Py_DECREF(u); return t; }
    if (PyErr_Occurred()) { Py_DECREF(u); return NULL; }
    PyDict_SetItem(interned, u, u);
    ((PyASCIIObject *)u)->state.interned = 2;
    u->ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT;
    Py_DECREF(u);
    return u;
}

/* ---- PyUnicode_FromFormat ---------------------------------------------------- */

PyObject *PyUnicode_FromFormatV(const char *fmt, va_list va) {
    PyObject *parts = PyList_New(0);
    if (!parts) return NULL;
    const char *p = fmt;
    char buf[64];
    while (*p) {
        if (*p != '%') {
            const char *q = p;
            while (*q && *q != '%') q++;
            PyObject *s = PyUnicode_FromStringAndSize(p, q - p);
            if (!s) goto fail;
            PyList_Append(parts, s); Py_DECREF(s);
            p = q;
            continue;
        }
        p++;
        /* flags/width/precision */
        int longflag = 0, sizeflag = 0, longlongflag = 0;
        int prec = -1, width = -1;
        int zeropad = 0;
        if (*p == '0') { zeropad = 1; p++; }
        while (*p >= '0' && *p <= '9') { if (width < 0) width = 0; width = width * 10 + (*p - '0'); p++; }
        if (*p == '.') { p++; prec = 0; while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; } }
        if (*p == 'l') { p++; if (*p == 'l') { longlongflag = 1; p++; } else longflag = 1; }
        else if (*p == 'z') { sizeflag = 1; p++; }
        PyObject *s = NULL;
        switch (*p) {
        case '%': s = PyUnicode_FromString("%"); break;
        case 'c': { int c = va_arg(va, int); s = PyUnicode_FromOrdinal(c); break; }
        case 'd': case 'i': {
            char f[16]; snprintf(f, sizeof f, "%%%s%s%s%c", zeropad ? "0" : "", width >= 0 ? "*" : "", longlongflag ? "lld" : longflag ? "ld" : sizeflag ? "zd" : "d", 0);
            if (longlongflag) { long long v = va_arg(va, long long); if (width >= 0) snprintf(buf, sizeof buf, f, width, v); else snprintf(buf, sizeof buf, f, v); }
            else if (longflag) { long v = va_arg(va, long); if (width >= 0) snprintf(buf, sizeof buf, f, width, v); else snprintf(buf, sizeof buf, f, v); }
            else if (sizeflag) { Py_ssize_t v = va_arg(va, Py_ssize_t); if (width >= 0) snprintf(buf, sizeof buf, f, width, v); else snprintf(buf, sizeof buf, f, v); }
            else { int v = va_arg(va, int); if (width >= 0) snprintf(buf, sizeof buf, f, width, v); else snprintf(buf, sizeof buf, f, v); }
            s = PyUnicode_FromString(buf); break;
        }
        case 'u': {
            if (longlongflag) snprintf(buf, sizeof buf, "%llu", va_arg(va, unsigned long long));
            else if (longflag) snprintf(buf, sizeof buf, "%lu", va_arg(va, unsigned long));
            else if (sizeflag) snprintf(buf, sizeof buf, "%zu", va_arg(va, size_t));
            else snprintf(buf, sizeof buf, "%u", va_arg(va, unsigned));
            s = PyUnicode_FromString(buf); break;
        }
        case 'x': {
            if (longlongflag) snprintf(buf, sizeof buf, "%llx", va_arg(va, unsigned long long));
            else if (longflag) snprintf(buf, sizeof buf, "%lx", va_arg(va, unsigned long));
            else if (sizeflag) snprintf(buf, sizeof buf, "%zx", va_arg(va, size_t));
            else snprintf(buf, sizeof buf, "%x", va_arg(va, unsigned));
            s = PyUnicode_FromString(buf); break;
        }
        case 'p': snprintf(buf, sizeof buf, "%p", va_arg(va, void *)); s = PyUnicode_FromString(buf); break;
        case 's': { const char *str = va_arg(va, const char *); if (!str) str = "(null)"; Py_ssize_t n = (Py_ssize_t)strlen(str); if (prec >= 0 && n > prec) n = prec; s = PyUnicode_DecodeUTF8(str, n, "replace"); break; }
        case 'U': { PyObject *o = va_arg(va, PyObject *); s = o ? PyObject_Str(o) : PyUnicode_FromString("(null)"); break; }
        case 'V': { PyObject *o = va_arg(va, PyObject *); const char *str = va_arg(va, const char *); s = o ? PyObject_Str(o) : PyUnicode_FromString(str ? str : "(null)"); break; }
        case 'S': { PyObject *o = va_arg(va, PyObject *); s = PyObject_Str(o); break; }
        case 'R': { PyObject *o = va_arg(va, PyObject *); s = PyObject_Repr(o); break; }
        case 'A': { PyObject *o = va_arg(va, PyObject *); s = PyObject_ASCII(o); break; }
        case 'T': { PyObject *o = va_arg(va, PyObject *); s = PyUnicode_FromString(Py_TYPE(o)->tp_name); break; }
        case 'N': { PyTypeObject *t = va_arg(va, PyTypeObject *); s = PyUnicode_FromString(t->tp_name); break; }
        default: PyErr_Format(PyExc_SystemError, "invalid format string: %s", fmt); goto fail;
        }
        if (!s) goto fail;
        PyList_Append(parts, s);
        Py_DECREF(s);
        p++;
    }
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty);
    Py_DECREF(parts);
    return r;
fail:
    Py_DECREF(parts);
    return NULL;
}

PyObject *PyUnicode_FromFormat(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    PyObject *r = PyUnicode_FromFormatV(fmt, va);
    va_end(va);
    return r;
}

int PyUnicode_FSConverter(PyObject *o, void *result) {
    PyObject *b;
    if (PyBytes_Check(o)) b = Py_NewRef(o);
    else if (PyUnicode_Check(o)) b = PyUnicode_EncodeFSDefault(o);
    else { PyErr_Format(PyExc_TypeError, "expected str, bytes or os.PathLike object, not %s", Py_TYPE(o)->tp_name); return 0; }
    if (!b) return 0;
    *(PyObject **)result = b;
    return 1;
}
int PyUnicode_FSDecoder(PyObject *o, void *result) {
    PyObject *u;
    if (PyUnicode_Check(o)) u = Py_NewRef(o);
    else if (PyBytes_Check(o)) u = PyUnicode_DecodeFSDefaultAndSize(PyBytes_AS_STRING(o), PyBytes_GET_SIZE(o));
    else { PyErr_Format(PyExc_TypeError, "expected str, bytes or os.PathLike object, not %s", Py_TYPE(o)->tp_name); return 0; }
    if (!u) return 0;
    *(PyObject **)result = u;
    return 1;
}
PyObject *PyUnicode_BuildEncodingMap(PyObject *s) { PIPER_UNUSED(s); Py_RETURN_NONE; }
