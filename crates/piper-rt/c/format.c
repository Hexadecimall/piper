/* String formatting: format specs (PEP 3101), str.format, and printf-style %. */
#include "internal.h"

int PyOS_vsnprintf(char *str, size_t size, const char *format, va_list va) {
    if (!str || !format || size > (size_t)INT_MAX) return -1;
    int written = vsnprintf(str, size, format, va);
    if (size) str[size - 1] = '\0';
    return written;
}

int PyOS_snprintf(char *str, size_t size, const char *format, ...) {
    va_list va;
    va_start(va, format);
    int written = PyOS_vsnprintf(str, size, format, va);
    va_end(va);
    return written;
}

typedef struct {
    Py_UCS4 fill;
    char align;      /* '<' '>' '=' '^' or 0 */
    char sign;       /* '+' '-' ' ' or 0 */
    int alt;         /* '#' */
    int zero;        /* '0' */
    Py_ssize_t width;
    char grouping;   /* ',' '_' or 0 */
    int has_precision;
    Py_ssize_t precision;
    char type;       /* conversion type or 0 */
} FormatSpec;

static int parse_spec(PyObject *spec, FormatSpec *fs) {
    memset(fs, 0, sizeof *fs);
    fs->fill = ' ';
    fs->width = -1;
    fs->precision = -1;
    Py_ssize_t n = PyUnicode_GET_LENGTH(spec), i = 0;
    int kind = PyUnicode_KIND(spec); void *data = PyUnicode_DATA(spec);
#define AT(k) PyUnicode_READ(kind, data, k)
    if (n >= 2 && strchr("<>=^", (int)AT(1)) && AT(1) < 128) { fs->fill = AT(0); fs->align = (char)AT(1); i = 2; }
    else if (n >= 1 && AT(0) < 128 && strchr("<>=^", (int)AT(0))) { fs->align = (char)AT(0); i = 1; }
    if (i < n && (AT(i) == '+' || AT(i) == '-' || AT(i) == ' ')) fs->sign = (char)AT(i++);
    if (i < n && AT(i) == 'z') i++;
    if (i < n && AT(i) == '#') { fs->alt = 1; i++; }
    if (i < n && AT(i) == '0') { fs->zero = 1; if (!fs->align) { fs->fill = '0'; fs->align = '='; } i++; }
    while (i < n && AT(i) >= '0' && AT(i) <= '9') { if (fs->width < 0) fs->width = 0; fs->width = fs->width * 10 + (Py_ssize_t)(AT(i) - '0'); i++; }
    if (i < n && (AT(i) == ',' || AT(i) == '_')) fs->grouping = (char)AT(i++);
    if (i < n && AT(i) == '.') {
        i++;
        fs->has_precision = 1; fs->precision = 0;
        if (i >= n || AT(i) < '0' || AT(i) > '9') { PyErr_SetString(PyExc_ValueError, "Format specifier missing precision"); return -1; }
        while (i < n && AT(i) >= '0' && AT(i) <= '9') { fs->precision = fs->precision * 10 + (Py_ssize_t)(AT(i) - '0'); i++; }
    }
    if (i < n) { fs->type = (char)AT(i++); }
    if (i < n) { PyErr_SetString(PyExc_ValueError, "Invalid format specifier"); return -1; }
#undef AT
    return 0;
}

/* Pad `body` (a str) to width with fill/align; default align given. */
static PyObject *apply_padding(PyObject *body, FormatSpec *fs, char default_align, Py_ssize_t sign_prefix_len) {
    Py_ssize_t len = PyUnicode_GET_LENGTH(body);
    if (fs->width <= len) return Py_NewRef(body);
    char align = fs->align ? fs->align : default_align;
    Py_ssize_t pad = fs->width - len, left = 0, right = 0;
    if (align == '<') right = pad;
    else if (align == '>') left = pad;
    else if (align == '^') { left = pad / 2; right = pad - left; }
    else { /* '=' : after sign */
        Py_UCS4 maxchar = PyUnicode_MAX_CHAR_VALUE(body); if (fs->fill > maxchar) maxchar = fs->fill;
        PyObject *r = PyUnicode_New(fs->width, maxchar);
        PyUnicode_CopyCharacters(r, 0, body, 0, sign_prefix_len);
        PyUnicode_Fill(r, sign_prefix_len, pad, fs->fill);
        PyUnicode_CopyCharacters(r, sign_prefix_len + pad, body, sign_prefix_len, len - sign_prefix_len);
        return r;
    }
    Py_UCS4 maxchar = PyUnicode_MAX_CHAR_VALUE(body); if (fs->fill > maxchar) maxchar = fs->fill;
    PyObject *r = PyUnicode_New(fs->width, maxchar);
    PyUnicode_Fill(r, 0, left, fs->fill);
    PyUnicode_CopyCharacters(r, left, body, 0, len);
    PyUnicode_Fill(r, left + len, right, fs->fill);
    return r;
}

static void insert_grouping(char *digits, char sep, int group) {
    /* digits: NUL-terminated integer digits (no sign). Groups from the right. */
    size_t n = strlen(digits);
    if (n <= (size_t)group) return;
    size_t ngroups = (n - 1) / (size_t)group;
    char *out = PyMem_Malloc(n + ngroups + 1);
    size_t j = 0, first = n - ngroups * (size_t)group;
    memcpy(out, digits, first); j = first;
    for (size_t i = first; i < n; i += (size_t)group) { out[j++] = sep; memcpy(out + j, digits + i, (size_t)group); j += (size_t)group; }
    out[j] = 0;
    strcpy(digits, out);
    PyMem_Free(out);
}

static PyObject *format_int(PyObject *v, FormatSpec *fs) {
    char t = fs->type ? fs->type : 'd';
    if (strchr("eEfFgG%", t)) { double d = PyLong_AsDouble(v); if (d == -1.0 && PyErr_Occurred()) return NULL; PyObject *f = PyFloat_FromDouble(d); extern PyObject *piper_format_float(PyObject *, FormatSpec *); PyObject *r = piper_format_float(f, fs); Py_DECREF(f); return r; }
    if (fs->has_precision) { PyErr_SetString(PyExc_ValueError, "Precision not allowed in integer format specifier"); return NULL; }
    if (t == 'c') {
        if (fs->sign) { PyErr_SetString(PyExc_ValueError, "Sign not allowed with integer format specifier 'c'"); return NULL; }
        long o = PyLong_AsLong(v);
        if (o == -1 && PyErr_Occurred()) return NULL;
        PyObject *ch = PyUnicode_FromOrdinal((int)o);
        if (!ch) return NULL;
        PyObject *r = apply_padding(ch, fs, '<', 0);
        Py_DECREF(ch);
        return r;
    }
    int base; const char *prefix = "";
    switch (t) { case 'd': case 'n': base = 10; break; case 'b': base = 2; prefix = "0b"; break; case 'o': base = 8; prefix = "0o"; break; case 'x': base = 16; prefix = "0x"; break; case 'X': base = 16; prefix = "0X"; break;
    default: PyErr_Format(PyExc_ValueError, "Unknown format code '%c' for object of type 'int'", t); return NULL; }
    PyObject *s = _PyLong_Format(v, base);
    if (!s) return NULL;
    const char *str = PyUnicode_AsUTF8(s);
    int neg = str[0] == '-';
    const char *digits = str + neg + (base != 10 ? 2 : 0);
    char *buf = PyMem_Malloc(strlen(digits) * 2 + 8);
    strcpy(buf, digits);
    if (t == 'X') for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    if (fs->grouping) insert_grouping(buf, fs->grouping, base == 10 ? 3 : 4);
    char signch = neg ? '-' : fs->sign == '+' ? '+' : fs->sign == ' ' ? ' ' : 0;
    size_t plen = (signch ? 1 : 0) + (fs->alt ? strlen(prefix) : 0);
    char *full = PyMem_Malloc(strlen(buf) + plen + 1);
    size_t k = 0;
    if (signch) full[k++] = signch;
    if (fs->alt) { strcpy(full + k, prefix); k += strlen(prefix); }
    strcpy(full + k, buf);
    Py_DECREF(s);
    PyObject *body = PyUnicode_FromString(full);
    PyMem_Free(buf);
    PyObject *r;
    if (fs->zero && fs->align == '=' && fs->grouping && fs->width > 0) {
        /* zero padding with grouping: pad the digit part with grouped zeros */
        Py_ssize_t need = fs->width - (Py_ssize_t)plen;
        char *digs = PyMem_Malloc((size_t)need + strlen(full) + 8);
        strcpy(digs, full + plen);
        while ((Py_ssize_t)strlen(digs) < need) { memmove(digs + 1, digs, strlen(digs) + 1); digs[0] = '0'; if ((Py_ssize_t)strlen(digs) < need) { /* re-group */ char *raw = PyMem_Malloc(strlen(digs) + 1); size_t q = 0; for (char *p = digs; *p; p++) if (*p != fs->grouping) raw[q++] = *p; raw[q] = 0; strcpy(digs, raw); PyMem_Free(raw); insert_grouping(digs, fs->grouping, base == 10 ? 3 : 4); } }
        char *all = PyMem_Malloc(plen + strlen(digs) + 1);
        memcpy(all, full, plen); strcpy(all + plen, digs);
        Py_DECREF(body);
        body = PyUnicode_FromString(all);
        PyMem_Free(all); PyMem_Free(digs);
        r = Py_NewRef(body);
    } else r = apply_padding(body, fs, '>', (Py_ssize_t)plen);
    PyMem_Free(full);
    Py_DECREF(body);
    return r;
}

PyObject *piper_format_float(PyObject *v, FormatSpec *fs) {
    double d = PyFloat_AS_DOUBLE(v);
    char t = fs->type;
    char buf[512];
    int default_prec = 6;
    Py_ssize_t prec = fs->has_precision ? fs->precision : default_prec;
    if (isnan(d) || isinf(d)) {
        const char *s = isnan(d) ? "nan" : "inf";
        int upper = t == 'E' || t == 'F' || t == 'G';
        snprintf(buf, sizeof buf, "%s%s", d < 0 ? "-" : fs->sign == '+' ? "+" : fs->sign == ' ' ? " " : "", upper ? (isnan(d) ? "NAN" : "INF") : s);
    } else {
        char f[16];
        int neg = signbit(d);
        double a = fabs(d);
        if (t == 0) {
            if (fs->has_precision) { snprintf(f, sizeof f, "%%#.%zdg", prec ? prec : 1); snprintf(buf, sizeof buf, f, a); /* strip trailing zeros like 'g' but keep at least one digit after point */ char *dot = strchr(buf, '.'); if (dot && !strchr(buf, 'e')) { char *e = buf + strlen(buf) - 1; while (e > dot + 1 && *e == '0') *e-- = 0; } }
            else { piper_float_repr(a, buf, sizeof buf); }
        } else if (t == '%') { snprintf(f, sizeof f, "%%.%zdf%%%%", prec); snprintf(buf, sizeof buf, f, a * 100.0); }
        else if (t == 'n') { snprintf(f, sizeof f, "%%.%zdg", prec); snprintf(buf, sizeof buf, f, a); }
        else if (strchr("eEfFgG", t)) { snprintf(f, sizeof f, "%%%s.%zd%c", fs->alt ? "#" : "", prec, t); snprintf(buf, sizeof buf, f, a); if ((t == 'g' || t == 'G') && fs->alt == 0) {} }
        else { PyErr_Format(PyExc_ValueError, "Unknown format code '%c' for object of type 'float'", t); return NULL; }
        if (fs->alt && !strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') && t != '%') strcat(buf, ".");
        /* grouping on integer part */
        if (fs->grouping) {
            char *dot = strpbrk(buf, ".eE%");
            size_t ilen = dot ? (size_t)(dot - buf) : strlen(buf);
            char ipart[256]; memcpy(ipart, buf, ilen); ipart[ilen] = 0;
            char rest[256]; strcpy(rest, buf + ilen);
            insert_grouping(ipart, fs->grouping, 3);
            snprintf(buf, sizeof buf, "%s%s", ipart, rest);
        }
        char signch = neg ? '-' : fs->sign == '+' ? '+' : fs->sign == ' ' ? ' ' : 0;
        if (signch) { memmove(buf + 1, buf, strlen(buf) + 1); buf[0] = signch; }
    }
    PyObject *body = PyUnicode_FromString(buf);
    Py_ssize_t plen = (buf[0] == '-' || buf[0] == '+' || buf[0] == ' ') ? 1 : 0;
    PyObject *r = apply_padding(body, fs, '>', plen);
    Py_DECREF(body);
    return r;
}

static PyObject *format_str(PyObject *v, FormatSpec *fs) {
    if (fs->type && fs->type != 's') { PyErr_Format(PyExc_ValueError, "Unknown format code '%c' for object of type 'str'", fs->type); return NULL; }
    if (fs->sign) { PyErr_SetString(PyExc_ValueError, "Sign not allowed in string format specifier"); return NULL; }
    if (fs->align == '=') { PyErr_SetString(PyExc_ValueError, "'=' alignment not allowed in string format specifier"); return NULL; }
    PyObject *body = v;
    if (fs->has_precision && fs->precision < PyUnicode_GET_LENGTH(v)) body = PyUnicode_Substring(v, 0, fs->precision); else Py_INCREF(body);
    PyObject *r = apply_padding(body, fs, '<', 0);
    Py_DECREF(body);
    return r;
}

PyObject *piper_format_spec_apply(PyObject *value, PyObject *spec) {
    FormatSpec fs;
    if (parse_spec(spec, &fs) < 0) return NULL;
    if (PyUnicode_Check(value)) return format_str(value, &fs);
    if (PyLong_Check(value)) { if (PyBool_Check(value) && !fs.type && PyUnicode_GET_LENGTH(spec) == 0) return PyObject_Str(value); return format_int(value, &fs); }
    if (PyFloat_Check(value)) return piper_format_float(value, &fs);
    PyErr_Format(PyExc_TypeError, "unsupported format string passed to %s.__format__", Py_TYPE(value)->tp_name);
    return NULL;
}

/* ---- str.format --------------------------------------------------------------- */

typedef struct { PyObject *const *args; Py_ssize_t nargs; PyObject *kwnames; PyObject *mapping; Py_ssize_t auto_idx; int manual; } FormatArgs;

static PyObject *lookup_field(FormatArgs *fa, PyObject *name) {
    /* name: "" (auto), digits, or identifier; then .attr / [key] chains */
    Py_ssize_t n = PyUnicode_GET_LENGTH(name), i = 0;
    while (i < n && PyUnicode_READ_CHAR(name, i) != '.' && PyUnicode_READ_CHAR(name, i) != '[') i++;
    PyObject *first = PyUnicode_Substring(name, 0, i);
    PyObject *obj = NULL;
    if (PyUnicode_GET_LENGTH(first) == 0) {
        if (fa->manual == 1) { Py_DECREF(first); PyErr_SetString(PyExc_ValueError, "cannot switch from manual field specification to automatic field numbering"); return NULL; }
        fa->manual = 2;
        if (fa->auto_idx >= fa->nargs) { Py_DECREF(first); PyErr_Format(PyExc_IndexError, "Replacement index %zd out of range for positional args tuple", fa->auto_idx); return NULL; }
        obj = Py_NewRef(fa->args[fa->auto_idx++]);
    } else {
        int all_digits = 1;
        for (Py_ssize_t k = 0; k < PyUnicode_GET_LENGTH(first); k++) if (PyUnicode_READ_CHAR(first, k) < '0' || PyUnicode_READ_CHAR(first, k) > '9') { all_digits = 0; break; }
        if (all_digits) {
            if (fa->manual == 2) { Py_DECREF(first); PyErr_SetString(PyExc_ValueError, "cannot switch from automatic field numbering to manual field specification"); return NULL; }
            fa->manual = 1;
            Py_ssize_t idx = strtoll(PyUnicode_AsUTF8(first), NULL, 10);
            if (idx >= fa->nargs) { Py_DECREF(first); PyErr_Format(PyExc_IndexError, "Replacement index %zd out of range for positional args tuple", idx); return NULL; }
            obj = Py_NewRef(fa->args[idx]);
        } else {
            if (fa->mapping) { obj = PyObject_GetItem(fa->mapping, first); }
            else {
                Py_ssize_t ki = -1;
                if (fa->kwnames) for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(fa->kwnames); k++) if (piper_unicode_eq(PyTuple_GET_ITEM(fa->kwnames, k), first)) { ki = k; break; }
                if (ki < 0) { PyErr_SetObject(PyExc_KeyError, first); }
                else obj = Py_NewRef(fa->args[fa->nargs + ki]);
            }
        }
    }
    Py_DECREF(first);
    if (!obj) return NULL;
    while (i < n) {
        Py_UCS4 c = PyUnicode_READ_CHAR(name, i);
        if (c == '.') {
            Py_ssize_t j = i + 1;
            while (j < n && PyUnicode_READ_CHAR(name, j) != '.' && PyUnicode_READ_CHAR(name, j) != '[') j++;
            if (j == i + 1) { Py_DECREF(obj); PyErr_SetString(PyExc_ValueError, "Empty attribute in format string"); return NULL; }
            PyObject *attr = PyUnicode_Substring(name, i + 1, j);
            PyObject *next = PyObject_GetAttr(obj, attr);
            Py_DECREF(attr); Py_DECREF(obj);
            if (!next) return NULL;
            obj = next; i = j;
        } else if (c == '[') {
            Py_ssize_t j = i + 1;
            while (j < n && PyUnicode_READ_CHAR(name, j) != ']') j++;
            if (j >= n) { Py_DECREF(obj); PyErr_SetString(PyExc_ValueError, "Missing ']' in format string"); return NULL; }
            PyObject *key = PyUnicode_Substring(name, i + 1, j);
            int digits = PyUnicode_GET_LENGTH(key) > 0;
            for (Py_ssize_t k = 0; k < PyUnicode_GET_LENGTH(key); k++) if (PyUnicode_READ_CHAR(key, k) < '0' || PyUnicode_READ_CHAR(key, k) > '9') { digits = 0; break; }
            PyObject *k2 = digits ? PyLong_FromString(PyUnicode_AsUTF8(key), NULL, 10) : Py_NewRef(key);
            Py_DECREF(key);
            PyObject *next = PyObject_GetItem(obj, k2);
            Py_DECREF(k2); Py_DECREF(obj);
            if (!next) return NULL;
            obj = next; i = j + 1;
        } else { Py_DECREF(obj); PyErr_SetString(PyExc_ValueError, "Only '.' or '[' may follow ']' in format field specifier"); return NULL; }
    }
    return obj;
}

static PyObject *do_format(PyObject *tmpl, FormatArgs *fa, int depth);

static PyObject *render_field(PyObject *field, FormatArgs *fa, int depth) {
    /* field text without braces: name[!conv][:spec] */
    Py_ssize_t n = PyUnicode_GET_LENGTH(field), i = 0, brackets = 0;
    while (i < n) { Py_UCS4 c = PyUnicode_READ_CHAR(field, i); if (c == '[') brackets++; else if (c == ']') brackets--; else if (brackets == 0 && (c == '!' || c == ':')) break; i++; }
    PyObject *name = PyUnicode_Substring(field, 0, i);
    int conv = 0;
    PyObject *spec = NULL;
    if (i < n && PyUnicode_READ_CHAR(field, i) == '!') {
        if (i + 1 >= n) { Py_DECREF(name); PyErr_SetString(PyExc_ValueError, "end of string while looking for conversion specifier"); return NULL; }
        conv = (int)PyUnicode_READ_CHAR(field, i + 1);
        i += 2;
        if (i < n && PyUnicode_READ_CHAR(field, i) != ':') { Py_DECREF(name); PyErr_SetString(PyExc_ValueError, "expected ':' after conversion specifier"); return NULL; }
    }
    if (i < n && PyUnicode_READ_CHAR(field, i) == ':') spec = PyUnicode_Substring(field, i + 1, n);
    else spec = PyUnicode_New(0, 0);
    PyObject *obj = lookup_field(fa, name);
    Py_DECREF(name);
    if (!obj) { Py_DECREF(spec); return NULL; }
    if (PyUnicode_GET_LENGTH(spec) && piper_unicode_find_ucs4(spec, piper_intern("{"), 0, PY_SSIZE_T_MAX, 1) >= 0) {
        PyObject *expanded = do_format(spec, fa, depth + 1);
        Py_DECREF(spec);
        if (!expanded) { Py_DECREF(obj); return NULL; }
        spec = expanded;
    }
    PyObject *r;
    if (conv) {
        if (conv != 's' && conv != 'r' && conv != 'a') { Py_DECREF(obj); Py_DECREF(spec); PyErr_Format(PyExc_ValueError, "Unknown conversion specifier %c", conv); return NULL; }
        r = piper_format_value(obj, conv, spec);
    } else r = PyObject_Format(obj, spec);
    Py_DECREF(obj); Py_DECREF(spec);
    return r;
}

static PyObject *do_format(PyObject *tmpl, FormatArgs *fa, int depth) {
    if (depth > 2) { PyErr_SetString(PyExc_ValueError, "Max string recursion exceeded"); return NULL; }
    Py_ssize_t n = PyUnicode_GET_LENGTH(tmpl), i = 0;
    PyObject *parts = PyList_New(0);
    Py_ssize_t lit_start = 0;
    while (i < n) {
        Py_UCS4 c = PyUnicode_READ_CHAR(tmpl, i);
        if (c == '{') {
            if (i + 1 < n && PyUnicode_READ_CHAR(tmpl, i + 1) == '{') { PyObject *s = PyUnicode_Substring(tmpl, lit_start, i + 1); PyList_Append(parts, s); Py_DECREF(s); i += 2; lit_start = i; continue; }
            PyObject *s = PyUnicode_Substring(tmpl, lit_start, i); PyList_Append(parts, s); Py_DECREF(s);
            Py_ssize_t j = i + 1; int nest = 1;
            while (j < n) { Py_UCS4 d = PyUnicode_READ_CHAR(tmpl, j); if (d == '{') nest++; else if (d == '}') { if (--nest == 0) break; } j++; }
            if (j >= n) { Py_DECREF(parts); PyErr_SetString(PyExc_ValueError, "Single '{' encountered in format string"); return NULL; }
            PyObject *field = PyUnicode_Substring(tmpl, i + 1, j);
            PyObject *r = render_field(field, fa, depth);
            Py_DECREF(field);
            if (!r) { Py_DECREF(parts); return NULL; }
            PyList_Append(parts, r); Py_DECREF(r);
            i = j + 1; lit_start = i;
        } else if (c == '}') {
            if (i + 1 < n && PyUnicode_READ_CHAR(tmpl, i + 1) == '}') { PyObject *s = PyUnicode_Substring(tmpl, lit_start, i + 1); PyList_Append(parts, s); Py_DECREF(s); i += 2; lit_start = i; continue; }
            Py_DECREF(parts); PyErr_SetString(PyExc_ValueError, "Single '}' encountered in format string"); return NULL;
        } else i++;
    }
    PyObject *s = PyUnicode_Substring(tmpl, lit_start, n); PyList_Append(parts, s); Py_DECREF(s);
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts);
    return r;
}

PyObject *piper_str_format_method(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    FormatArgs fa = { args, nargs, kwnames, NULL, 0, 0 };
    return do_format(self, &fa, 0);
}
PyObject *piper_str_format_with_mapping(PyObject *self, PyObject *mapping) {
    FormatArgs fa = { NULL, 0, NULL, mapping, 0, 0 };
    return do_format(self, &fa, 0);
}

/* ---- printf-style % ----------------------------------------------------------- */

PyObject *PyUnicode_Format(PyObject *fmt, PyObject *args) {
    Py_ssize_t n = PyUnicode_GET_LENGTH(fmt), i = 0, argidx = 0;
    PyObject *dict = NULL;
    PyObject *tuple;
    if (PyTuple_Check(args)) tuple = Py_NewRef(args);
    else { tuple = PyTuple_Pack(1, args); if (Py_TYPE(args)->tp_as_mapping && Py_TYPE(args)->tp_as_mapping->mp_subscript && !PyUnicode_Check(args) && !PyTuple_Check(args)) dict = args; }
    Py_ssize_t nargs = PyTuple_GET_SIZE(tuple);
    PyObject *parts = PyList_New(0);
    Py_ssize_t lit_start = 0;
    int used_dict = 0;
    while (i < n) {
        if (PyUnicode_READ_CHAR(fmt, i) != '%') { i++; continue; }
        PyObject *s = PyUnicode_Substring(fmt, lit_start, i); PyList_Append(parts, s); Py_DECREF(s);
        i++;
        if (i >= n) { PyErr_SetString(PyExc_ValueError, "incomplete format"); goto fail; }
        PyObject *keyed = NULL;
        if (PyUnicode_READ_CHAR(fmt, i) == '(') {
            if (!dict) { PyErr_SetString(PyExc_TypeError, "format requires a mapping"); goto fail; }
            Py_ssize_t j = i + 1; int depth = 1;
            while (j < n) { Py_UCS4 c = PyUnicode_READ_CHAR(fmt, j); if (c == '(') depth++; else if (c == ')') { if (--depth == 0) break; } j++; }
            if (j >= n) { PyErr_SetString(PyExc_ValueError, "incomplete format key"); goto fail; }
            PyObject *key = PyUnicode_Substring(fmt, i + 1, j);
            keyed = PyObject_GetItem(dict, key);
            Py_DECREF(key);
            if (!keyed) goto fail;
            used_dict = 1;
            i = j + 1;
        }
        FormatSpec fs; memset(&fs, 0, sizeof fs); fs.fill = ' '; fs.width = -1; fs.precision = -1;
        int left = 0;
        while (i < n) { Py_UCS4 c = PyUnicode_READ_CHAR(fmt, i); if (c == '-') left = 1; else if (c == '+') fs.sign = '+'; else if (c == ' ') { if (!fs.sign) fs.sign = ' '; } else if (c == '#') fs.alt = 1; else if (c == '0') fs.zero = 1; else break; i++; }
        if (i < n && PyUnicode_READ_CHAR(fmt, i) == '*') { if (argidx >= nargs) { PyErr_SetString(PyExc_TypeError, "not enough arguments for format string"); goto fail; } fs.width = PyLong_AsSsize_t(PyTuple_GET_ITEM(tuple, argidx++)); if (fs.width == -1 && PyErr_Occurred()) goto fail; if (fs.width < 0) { left = 1; fs.width = -fs.width; } i++; }
        else while (i < n && PyUnicode_READ_CHAR(fmt, i) >= '0' && PyUnicode_READ_CHAR(fmt, i) <= '9') { if (fs.width < 0) fs.width = 0; fs.width = fs.width * 10 + (Py_ssize_t)(PyUnicode_READ_CHAR(fmt, i) - '0'); i++; }
        if (i < n && PyUnicode_READ_CHAR(fmt, i) == '.') {
            i++; fs.has_precision = 1; fs.precision = 0;
            if (i < n && PyUnicode_READ_CHAR(fmt, i) == '*') { if (argidx >= nargs) { PyErr_SetString(PyExc_TypeError, "not enough arguments for format string"); goto fail; } fs.precision = PyLong_AsSsize_t(PyTuple_GET_ITEM(tuple, argidx++)); i++; }
            else while (i < n && PyUnicode_READ_CHAR(fmt, i) >= '0' && PyUnicode_READ_CHAR(fmt, i) <= '9') { fs.precision = fs.precision * 10 + (Py_ssize_t)(PyUnicode_READ_CHAR(fmt, i) - '0'); i++; }
        }
        while (i < n && (PyUnicode_READ_CHAR(fmt, i) == 'l' || PyUnicode_READ_CHAR(fmt, i) == 'h' || PyUnicode_READ_CHAR(fmt, i) == 'L')) i++;
        if (i >= n) { PyErr_SetString(PyExc_ValueError, "incomplete format"); goto fail; }
        Py_UCS4 t = PyUnicode_READ_CHAR(fmt, i++);
        if (t == '%') { PyObject *pct = PyUnicode_FromString("%"); PyList_Append(parts, pct); Py_DECREF(pct); lit_start = i; Py_XDECREF(keyed); continue; }
        PyObject *arg = keyed;
        if (!arg) {
            if (dict && !keyed) { arg = Py_NewRef(dict); }
            else { if (argidx >= nargs) { PyErr_SetString(PyExc_TypeError, "not enough arguments for format string"); goto fail; } arg = Py_NewRef(PyTuple_GET_ITEM(tuple, argidx++)); }
        }
        PyObject *body = NULL;
        fs.align = left ? '<' : (fs.zero ? '=' : '>');
        if (fs.zero && !left) fs.fill = '0';
        switch (t) {
        case 's': body = PyObject_Str(arg); break;
        case 'r': body = PyObject_Repr(arg); break;
        case 'a': body = PyObject_ASCII(arg); break;
        case 'c': if (PyUnicode_Check(arg)) { if (PyUnicode_GET_LENGTH(arg) != 1) { PyErr_SetString(PyExc_TypeError, "%c requires an int or a unicode character, not a string of length 2 or more"); Py_DECREF(arg); goto fail; } body = Py_NewRef(arg); } else { long o = PyLong_AsLong(arg); if (o == -1 && PyErr_Occurred()) { Py_DECREF(arg); goto fail; } body = PyUnicode_FromOrdinal((int)o); } break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            PyObject *iv;
            if (PyLong_Check(arg)) iv = Py_NewRef(arg);
            else if (PyFloat_Check(arg) && (t == 'd' || t == 'i' || t == 'u')) iv = PyNumber_Long(arg);
            else if (PyIndex_Check(arg)) iv = PyNumber_Index(arg);
            else { PyErr_Format(PyExc_TypeError, "%%%c format: %s is required, not %s", (char)t, (t == 'd' || t == 'i' || t == 'u') ? "a real number" : "an integer", Py_TYPE(arg)->tp_name); Py_DECREF(arg); goto fail; }
            if (!iv) { Py_DECREF(arg); goto fail; }
            FormatSpec ifs = fs; ifs.type = (t == 'd' || t == 'i' || t == 'u') ? 'd' : (char)t; ifs.has_precision = 0;
            Py_ssize_t saved_w = ifs.width; ifs.width = -1;
            body = format_int(iv, &ifs);
            Py_DECREF(iv);
            if (body && fs.has_precision) { /* precision = min digits */ Py_ssize_t len = PyUnicode_GET_LENGTH(body); int neg = PyUnicode_READ_CHAR(body, 0) == '-' || PyUnicode_READ_CHAR(body, 0) == '+' || PyUnicode_READ_CHAR(body, 0) == ' '; Py_ssize_t digits = len - neg - ((fs.alt && t != 'd' && t != 'i' && t != 'u') ? 2 : 0); if (digits < fs.precision) { FormatSpec z = fs; z.fill = '0'; z.align = '='; z.width = len + (fs.precision - digits); PyObject *padded = apply_padding(body, &z, '=', neg + ((fs.alt && t != 'd' && t != 'i' && t != 'u') ? 2 : 0)); Py_DECREF(body); body = padded; } }
            fs.width = saved_w;
            if (body && fs.zero && !left && fs.width > PyUnicode_GET_LENGTH(body)) { Py_ssize_t plen = (PyUnicode_READ_CHAR(body, 0) == '-' || PyUnicode_READ_CHAR(body, 0) == '+' || PyUnicode_READ_CHAR(body, 0) == ' ') + ((fs.alt && t != 'd' && t != 'i' && t != 'u') ? 2 : 0); PyObject *padded = apply_padding(body, &fs, '=', plen); Py_DECREF(body); body = padded; }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double d = PyFloat_AsDouble(arg);
            if (d == -1.0 && PyErr_Occurred()) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "must be real number, not %s", Py_TYPE(arg)->tp_name); Py_DECREF(arg); goto fail; }
            PyObject *fv = PyFloat_FromDouble(d);
            FormatSpec ffs = fs; ffs.type = (char)t; ffs.width = -1;
            body = piper_format_float(fv, &ffs);
            Py_DECREF(fv);
            if (body && fs.zero && !left && fs.width > PyUnicode_GET_LENGTH(body)) { Py_ssize_t plen = (PyUnicode_READ_CHAR(body, 0) == '-' || PyUnicode_READ_CHAR(body, 0) == '+' || PyUnicode_READ_CHAR(body, 0) == ' '); PyObject *padded = apply_padding(body, &fs, '=', plen); Py_DECREF(body); body = padded; }
            break;
        }
        default:
            PyErr_Format(PyExc_ValueError, "unsupported format character '%c' (0x%x) at index %zd", (char)t, t, i - 1);
            Py_DECREF(arg); goto fail;
        }
        Py_DECREF(arg);
        if (!body) goto fail;
        if ((t == 's' || t == 'r' || t == 'a') && fs.has_precision && fs.precision < PyUnicode_GET_LENGTH(body)) { PyObject *cut = PyUnicode_Substring(body, 0, fs.precision); Py_DECREF(body); body = cut; }
        if (fs.width > PyUnicode_GET_LENGTH(body) && !(fs.zero && !left && strchr("diuxXoeEfFgG", (int)t))) { FormatSpec p = fs; p.fill = ' '; p.align = left ? '<' : '>'; PyObject *padded = apply_padding(body, &p, '>', 0); Py_DECREF(body); body = padded; }
        PyList_Append(parts, body); Py_DECREF(body);
        lit_start = i;
    }
    if (argidx < nargs && !dict) { PyErr_SetString(PyExc_TypeError, "not all arguments converted during string formatting"); goto fail; }
    PIPER_UNUSED(used_dict);
    { PyObject *s = PyUnicode_Substring(fmt, lit_start, n); PyList_Append(parts, s); Py_DECREF(s); }
    PyObject *empty = PyUnicode_New(0, 0);
    PyObject *r = _PyUnicode_JoinArray(empty, PyList_ITEMS(parts), PyList_GET_SIZE(parts));
    Py_DECREF(empty); Py_DECREF(parts); Py_DECREF(tuple);
    return r;
fail:
    Py_DECREF(parts); Py_DECREF(tuple);
    return NULL;
}
