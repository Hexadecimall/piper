/* Initialization, the sys module, file objects for stdio, imports, class
 * creation, unpacking helpers, and the program entry point. */
#include "internal.h"
#include <unistd.h>
#include <fcntl.h>

int PyByteArray_Check_impl(PyObject *o);
int PyMemoryView_Check_impl(PyObject *o);

static int initialized = 0;
static PyObject *sys_module = NULL;
static PyObject *main_module = NULL;

/* ---- frames (bookkeeping for tracebacks and globals()) ------------------- */

typedef struct Frame { const char *filename; const char *funcname; PyObject *globals; int lineno; } Frame;
/* The frame stack grows on the heap: thread-local storage holds only a
 * pointer, so the static TLS block stays small. */
static __thread Frame *frames = NULL;
static __thread int frame_depth = 0;
static __thread int frame_cap = 0;

void piper_frame_push(const char *filename, const char *funcname, PyObject *globals) {
    if (frame_depth == frame_cap) {
        int cap = frame_cap ? frame_cap * 2 : 64;
        Frame *f = PyMem_RawRealloc(frames, (size_t)cap * sizeof(Frame));
        if (f) { frames = f; frame_cap = cap; }
    }
    if (frame_depth < frame_cap) {
        frames[frame_depth].filename = filename;
        frames[frame_depth].funcname = funcname;
        frames[frame_depth].globals = globals;
        frames[frame_depth].lineno = 0;
    }
    frame_depth++;
}
void piper_frame_pop(void) { if (frame_depth > 0) frame_depth--; }
void piper_frame_set_line(int lineno) { if (frame_depth > 0 && frame_depth <= frame_cap) frames[frame_depth - 1].lineno = lineno; }
PyObject *piper_frame_globals(void) { return frame_depth > 0 && frame_depth <= frame_cap ? frames[frame_depth - 1].globals : (main_module ? PyModule_GetDict(main_module) : NULL); }
PyObject *PyEval_GetGlobals(void) { return piper_frame_globals(); }
PyObject *PyEval_GetLocals(void) { return piper_frame_globals(); }

typedef struct { PyObject_HEAD PyObject *filename, *funcname, *globals; int lineno; } frameobject;
static void frame_dealloc(PyObject *o) { frameobject *f = (frameobject *)o; Py_XDECREF(f->filename); Py_XDECREF(f->funcname); Py_XDECREF(f->globals); PyObject_Free(o); }
static PyObject *frame_code_get(PyObject *o, void *c) {
    PIPER_UNUSED(c);
    frameobject *f = (frameobject *)o;
    PyObject *d = PyDict_New();
    PyDict_SetItemString(d, "co_filename", f->filename);
    PyDict_SetItemString(d, "co_name", f->funcname);
    PyDict_SetItemString(d, "co_qualname", f->funcname);
    PyObject *one = PyLong_FromLong(f->lineno); PyDict_SetItemString(d, "co_firstlineno", one); Py_DECREF(one);
    PyObject *bases = PyTuple_Pack(1, (PyObject *)&PyBaseObject_Type);
    PyObject *t = PyType_New3("code", bases, d);
    Py_DECREF(bases); Py_DECREF(d);
    return t;
}
static PyObject *frame_lineno_get(PyObject *o, void *c) { PIPER_UNUSED(c); return PyLong_FromLong(((frameobject *)o)->lineno); }
static PyObject *frame_globals_get(PyObject *o, void *c) { PIPER_UNUSED(c); frameobject *f = (frameobject *)o; return Py_NewRef(f->globals ? f->globals : Py_None); }
static PyObject *frame_back_get(PyObject *o, void *c) { PIPER_UNUSED(o); PIPER_UNUSED(c); Py_RETURN_NONE; }
static PyGetSetDef frame_getsets[] = { { "f_code", frame_code_get, NULL, NULL, NULL }, { "f_lineno", frame_lineno_get, NULL, NULL, NULL }, { "f_globals", frame_globals_get, NULL, NULL, NULL }, { "f_locals", frame_globals_get, NULL, NULL, NULL }, { "f_back", frame_back_get, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
PyTypeObject PyFrame_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "frame", .tp_basicsize = sizeof(frameobject), .tp_dealloc = frame_dealloc, .tp_getattro = PyObject_GenericGetAttr, .tp_getset = frame_getsets };
PyObject *piper_fake_frame_new(PyObject *filename, PyObject *funcname, int lineno) {
    frameobject *f = PyObject_New(frameobject, &PyFrame_Type);
    if (!f) return NULL;
    f->filename = Py_NewRef(filename); f->funcname = Py_NewRef(funcname); f->globals = Py_XNewRef(piper_frame_globals()); f->lineno = lineno;
    return (PyObject *)f;
}

/* ---- stdio file objects ---------------------------------------------------- */

typedef struct { PyObject_HEAD int fd; FILE *fp; int closed; int is_text; PyObject *name; PyObject *mode; PyObject *encoding; char *pending; Py_ssize_t pending_len; int owns_fp; } fileobject;
static PyTypeObject PyPiperFile_Type;

static PyObject *file_write(PyObject *self, PyObject *s) {
    fileobject *f = (fileobject *)self;
    if (f->closed) { PyErr_SetString(PyExc_ValueError, "I/O operation on closed file."); return NULL; }
    const char *data; Py_ssize_t n;
    if (f->is_text) { if (!PyUnicode_Check(s)) { PyErr_Format(PyExc_TypeError, "write() argument must be str, not %s", Py_TYPE(s)->tp_name); return NULL; } data = PyUnicode_AsUTF8AndSize(s, &n); if (!data) return NULL; }
    else { if (PyUnicode_Check(s)) { PyErr_SetString(PyExc_TypeError, "a bytes-like object is required, not 'str'"); return NULL; } Py_buffer v; if (PyObject_GetBuffer(s, &v, PyBUF_SIMPLE) < 0) return NULL; size_t w = fwrite(v.buf, 1, (size_t)v.len, f->fp); PyBuffer_Release(&v); return PyLong_FromSize_t(w); }
    if (fwrite(data, 1, (size_t)n, f->fp) != (size_t)n) { PyErr_SetFromErrno(PyExc_OSError); return NULL; }
    if (f->fd == 2 || (f->fd == 1 && isatty(1))) fflush(f->fp);
    return PyLong_FromSsize_t(f->is_text ? PyUnicode_GET_LENGTH(s) : n);
}
static PyObject *file_flush(PyObject *self, PyObject *u) { PIPER_UNUSED(u); fileobject *f = (fileobject *)self; if (f->fp && !f->closed) fflush(f->fp); Py_RETURN_NONE; }
static PyObject *file_read(PyObject *self, PyObject *const *a, Py_ssize_t n) {
    fileobject *f = (fileobject *)self;
    if (f->closed) { PyErr_SetString(PyExc_ValueError, "I/O operation on closed file."); return NULL; }
    Py_ssize_t size = -1;
    if (n >= 1 && a[0] != Py_None) { size = PyNumber_AsSsize_t(a[0], PyExc_OverflowError); if (size == -1 && PyErr_Occurred()) return NULL; }
    size_t cap = size >= 0 ? (size_t)size : 65536, len = 0;
    char *buf = PyMem_Malloc(cap + 1);
    for (;;) {
        if (size >= 0 && len >= (size_t)size) break;
        if (len == cap) { cap *= 2; buf = PyMem_Realloc(buf, cap + 1); }
        size_t want = size >= 0 ? (size_t)size - len : cap - len;
        size_t got = fread(buf + len, 1, want, f->fp);
        len += got;
        if (got < want) break;
    }
    PyObject *r = f->is_text ? PyUnicode_DecodeUTF8(buf, (Py_ssize_t)len, "replace") : PyBytes_FromStringAndSize(buf, (Py_ssize_t)len);
    PyMem_Free(buf);
    return r;
}
static PyObject *file_readline(PyObject *self, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(a); PIPER_UNUSED(n);
    fileobject *f = (fileobject *)self;
    if (f->closed) { PyErr_SetString(PyExc_ValueError, "I/O operation on closed file."); return NULL; }
    size_t cap = 256, len = 0;
    char *buf = PyMem_Malloc(cap);
    int c;
    while ((c = fgetc(f->fp)) != EOF) { if (len + 1 >= cap) { cap *= 2; buf = PyMem_Realloc(buf, cap); } buf[len++] = (char)c; if (c == '\n') break; }
    PyObject *r = f->is_text ? PyUnicode_DecodeUTF8(buf, (Py_ssize_t)len, "replace") : PyBytes_FromStringAndSize(buf, (Py_ssize_t)len);
    PyMem_Free(buf);
    return r;
}
static PyObject *file_readlines(PyObject *self, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(a); PIPER_UNUSED(n);
    PyObject *l = PyList_New(0);
    for (;;) { PyObject *line = file_readline(self, NULL, 0); if (!line) { Py_DECREF(l); return NULL; } if (PyObject_Size(line) == 0) { Py_DECREF(line); break; } PyList_Append(l, line); Py_DECREF(line); }
    return l;
}
static PyObject *file_close(PyObject *self, PyObject *u) { PIPER_UNUSED(u); fileobject *f = (fileobject *)self; if (!f->closed) { if (f->fp) { fflush(f->fp); if (f->owns_fp) fclose(f->fp); } f->closed = 1; } Py_RETURN_NONE; }
static PyObject *file_enter(PyObject *self, PyObject *u) { PIPER_UNUSED(u); return Py_NewRef(self); }
static PyObject *file_exit(PyObject *self, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(a); PIPER_UNUSED(n); return file_close(self, NULL); }
static PyObject *file_fileno(PyObject *self, PyObject *u) { PIPER_UNUSED(u); return PyLong_FromLong(((fileobject *)self)->fd); }
static PyObject *file_isatty(PyObject *self, PyObject *u) { PIPER_UNUSED(u); Py_RETURN_BOOL(isatty(((fileobject *)self)->fd)); }
static PyObject *file_readable(PyObject *self, PyObject *u) { PIPER_UNUSED(u); fileobject *f = (fileobject *)self; const char *m = f->mode ? PyUnicode_AsUTF8(f->mode) : "r"; Py_RETURN_BOOL(strchr(m, 'r') || strchr(m, '+')); }
static PyObject *file_writable(PyObject *self, PyObject *u) { PIPER_UNUSED(u); fileobject *f = (fileobject *)self; const char *m = f->mode ? PyUnicode_AsUTF8(f->mode) : "r"; Py_RETURN_BOOL(strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+') || strchr(m, 'x')); }
static PyObject *file_seekable(PyObject *self, PyObject *u) { PIPER_UNUSED(self); PIPER_UNUSED(u); Py_RETURN_TRUE; }
static PyObject *file_seek(PyObject *self, PyObject *const *a, Py_ssize_t n) { fileobject *f = (fileobject *)self; if (n < 1) { PyErr_SetString(PyExc_TypeError, "seek() missing offset"); return NULL; } long off = PyLong_AsLong(a[0]); int whence = n >= 2 ? (int)PyLong_AsLong(a[1]) : 0; if (PyErr_Occurred()) return NULL; if (fseek(f->fp, off, whence) < 0) { PyErr_SetFromErrno(PyExc_OSError); return NULL; } return PyLong_FromLong(ftell(f->fp)); }
static PyObject *file_tell(PyObject *self, PyObject *u) { PIPER_UNUSED(u); return PyLong_FromLong(ftell(((fileobject *)self)->fp)); }
static PyObject *file_writelines(PyObject *self, PyObject *lines) { PyObject *it = PyObject_GetIter(lines); if (!it) return NULL; for (;;) { PyObject *x = PyIter_Next(it); if (!x) break; PyObject *r = file_write(self, x); Py_DECREF(x); if (!r) { Py_DECREF(it); return NULL; } Py_DECREF(r); } Py_DECREF(it); if (PyErr_Occurred()) return NULL; Py_RETURN_NONE; }
static PyObject *file_iternext(PyObject *self) { PyObject *line = file_readline(self, NULL, 0); if (!line) return NULL; if (PyObject_Size(line) == 0) { Py_DECREF(line); return NULL; } return line; }
static PyObject *file_repr(PyObject *self) { fileobject *f = (fileobject *)self; return PyUnicode_FromFormat("<_io.%s name=%R mode=%R encoding='utf-8'>", f->is_text ? "TextIOWrapper" : "BufferedReader", f->name ? f->name : Py_None, f->mode ? f->mode : Py_None); }
static PyObject *file_closed_get(PyObject *self, void *c) { PIPER_UNUSED(c); Py_RETURN_BOOL(((fileobject *)self)->closed); }
static PyObject *file_encoding_get(PyObject *self, void *c) { PIPER_UNUSED(c); PIPER_UNUSED(self); return PyUnicode_FromString("utf-8"); }
static PyObject *file_errors_get(PyObject *self, void *c) { PIPER_UNUSED(c); PIPER_UNUSED(self); return PyUnicode_FromString("strict"); }
static PyObject *file_buffer_get(PyObject *self, void *c) {
    PIPER_UNUSED(c);
    fileobject *f = (fileobject *)self;
    fileobject *b = PyObject_New(fileobject, &PyPiperFile_Type);
    if (!b) return NULL;
    b->fd = f->fd; b->fp = f->fp; b->closed = 0; b->is_text = 0; b->name = Py_XNewRef(f->name); b->mode = PyUnicode_FromString("rb"); b->encoding = NULL; b->pending = NULL; b->pending_len = 0; b->owns_fp = 0;
    return (PyObject *)b;
}
static void file_dealloc(PyObject *self) { fileobject *f = (fileobject *)self; if (!f->closed && f->fp && f->owns_fp) fclose(f->fp); Py_XDECREF(f->name); Py_XDECREF(f->mode); Py_XDECREF(f->encoding); PyMem_Free(f->pending); PyObject_Free(self); }
static PyMethodDef file_methods[] = {
    { "write", file_write, METH_O, NULL }, { "flush", file_flush, METH_NOARGS, NULL }, { "read", (PyCFunction)(void (*)(void))file_read, METH_FASTCALL, NULL },
    { "readline", (PyCFunction)(void (*)(void))file_readline, METH_FASTCALL, NULL }, { "readlines", (PyCFunction)(void (*)(void))file_readlines, METH_FASTCALL, NULL },
    { "close", file_close, METH_NOARGS, NULL }, { "__enter__", file_enter, METH_NOARGS, NULL }, { "__exit__", (PyCFunction)(void (*)(void))file_exit, METH_FASTCALL, NULL },
    { "fileno", file_fileno, METH_NOARGS, NULL }, { "isatty", file_isatty, METH_NOARGS, NULL }, { "readable", file_readable, METH_NOARGS, NULL },
    { "writable", file_writable, METH_NOARGS, NULL }, { "seekable", file_seekable, METH_NOARGS, NULL }, { "seek", (PyCFunction)(void (*)(void))file_seek, METH_FASTCALL, NULL },
    { "tell", file_tell, METH_NOARGS, NULL }, { "writelines", file_writelines, METH_O, NULL },
    { NULL, NULL, 0, NULL },
};
static PyMemberDef file_members[] = { { "name", _Py_T_OBJECT, offsetof(fileobject, name), Py_READONLY, NULL }, { "mode", _Py_T_OBJECT, offsetof(fileobject, mode), Py_READONLY, NULL }, { NULL, 0, 0, 0, NULL } };
static PyGetSetDef file_getsets[] = { { "closed", file_closed_get, NULL, NULL, NULL }, { "encoding", file_encoding_get, NULL, NULL, NULL }, { "errors", file_errors_get, NULL, NULL, NULL }, { "buffer", file_buffer_get, NULL, NULL, NULL }, { NULL, NULL, NULL, NULL, NULL } };
static PyTypeObject PyPiperFile_Type = { PyVarObject_HEAD_INIT(&PyType_Type, 0) .tp_name = "_io.TextIOWrapper", .tp_basicsize = sizeof(fileobject), .tp_dealloc = file_dealloc, .tp_repr = file_repr, .tp_getattro = PyObject_GenericGetAttr, .tp_iter = PyObject_SelfIter, .tp_iternext = file_iternext, .tp_methods = file_methods, .tp_members = file_members, .tp_getset = file_getsets };

static PyObject *make_file(FILE *fp, int fd, const char *name, const char *mode, int owns) {
    fileobject *f = PyObject_New(fileobject, &PyPiperFile_Type);
    if (!f) return NULL;
    f->fd = fd; f->fp = fp; f->closed = 0; f->is_text = strchr(mode, 'b') == NULL; f->name = PyUnicode_FromString(name); f->mode = PyUnicode_FromString(mode); f->encoding = NULL; f->pending = NULL; f->pending_len = 0; f->owns_fp = owns;
    return (PyObject *)f;
}

PyObject *piper_open_impl(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
    PyObject *file = nargs >= 1 ? args[0] : NULL;
    const char *mode = nargs >= 2 ? PyUnicode_AsUTF8(args[1]) : "r";
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    for (Py_ssize_t i = 0; i < nkw; i++) { PyObject *k = PyTuple_GET_ITEM(kwnames, i); if (PyUnicode_EqualToUTF8(k, "file")) file = args[nargs + i]; else if (PyUnicode_EqualToUTF8(k, "mode")) mode = PyUnicode_AsUTF8(args[nargs + i]); }
    if (!file) { PyErr_SetString(PyExc_TypeError, "open() missing required argument 'file' (pos 1)"); return NULL; }
    if (!mode) return NULL;
    PyObject *path = NULL;
    if (PyUnicode_Check(file)) path = PyUnicode_EncodeFSDefault(file);
    else if (PyBytes_Check(file)) path = Py_NewRef(file);
    else if (PyLong_Check(file)) {
        int fd = (int)PyLong_AsLong(file);
        char cmode[8]; snprintf(cmode, sizeof cmode, "%s", strchr(mode, 'w') ? "w" : strchr(mode, 'a') ? "a" : "r");
        FILE *fp = fdopen(fd, cmode);
        if (!fp) return PyErr_SetFromErrno(PyExc_OSError);
        return make_file(fp, fd, "<fd>", mode, 1);
    } else { PyObject *fs = PyObject_CallMethod(file, "__fspath__", NULL); if (!fs) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "expected str, bytes or os.PathLike object, not %s", Py_TYPE(file)->tp_name); return NULL; } path = PyUnicode_Check(fs) ? PyUnicode_EncodeFSDefault(fs) : Py_NewRef(fs); Py_DECREF(fs); }
    if (!path) return NULL;
    char cmode[8];
    const char *base = strchr(mode, 'w') ? "w" : strchr(mode, 'a') ? "a" : strchr(mode, 'x') ? "wx" : "r";
    snprintf(cmode, sizeof cmode, "%s%s", base, strchr(mode, '+') ? "+" : "");
    if (strchr(mode, 'x')) { int fd = open(PyBytes_AS_STRING(path), O_WRONLY | O_CREAT | O_EXCL, 0666); if (fd < 0) { PyObject *r = PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, file); Py_DECREF(path); return r; } FILE *fp = fdopen(fd, "w"); PyObject *f = make_file(fp, fd, PyUnicode_Check(file) ? PyUnicode_AsUTF8(file) : PyBytes_AS_STRING(path), mode, 1); Py_DECREF(path); return f; }
    FILE *fp = fopen(PyBytes_AS_STRING(path), cmode);
    if (!fp) { PyObject *r = PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, file); Py_DECREF(path); return r; }
    PyObject *f = make_file(fp, fileno(fp), PyUnicode_Check(file) ? PyUnicode_AsUTF8(file) : PyBytes_AS_STRING(path), mode, 1);
    Py_DECREF(path);
    return f;
}

PyObject *piper_sys_stdout(void) { return PySys_GetObject("stdout"); }
PyObject *piper_sys_stderr(void) { return PySys_GetObject("stderr"); }
int piper_file_write(PyObject *file, PyObject *s) {
    if (Py_IS_TYPE(file, &PyPiperFile_Type)) { PyObject *r = file_write(file, s); if (!r) return -1; Py_DECREF(r); return 0; }
    PyObject *r = PyObject_CallMethod(file, "write", "O", s);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
int piper_file_flush(PyObject *file) {
    if (Py_IS_TYPE(file, &PyPiperFile_Type)) { file_flush(file, NULL); return 0; }
    PyObject *r = PyObject_CallMethod(file, "flush", NULL);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
int piper_write_stdout(const char *s, Py_ssize_t n) { return (Py_ssize_t)fwrite(s, 1, (size_t)n, stdout) == n ? 0 : -1; }
int piper_write_stderr(const char *s, Py_ssize_t n) { return (Py_ssize_t)fwrite(s, 1, (size_t)n, stderr) == n ? 0 : -1; }
void PySys_WriteStdout(const char *fmt, ...) { va_list va; va_start(va, fmt); vfprintf(stdout, fmt, va); va_end(va); }
void PySys_WriteStderr(const char *fmt, ...) { va_list va; va_start(va, fmt); vfprintf(stderr, fmt, va); va_end(va); }
void PySys_FormatStdout(const char *fmt, ...) { va_list va; va_start(va, fmt); PyObject *s = PyUnicode_FromFormatV(fmt, va); va_end(va); if (s) { fputs(PyUnicode_AsUTF8(s), stdout); Py_DECREF(s); } else PyErr_Clear(); }
void PySys_FormatStderr(const char *fmt, ...) { va_list va; va_start(va, fmt); PyObject *s = PyUnicode_FromFormatV(fmt, va); va_end(va); if (s) { fputs(PyUnicode_AsUTF8(s), stderr); Py_DECREF(s); } else PyErr_Clear(); }

/* ---- sys -------------------------------------------------------------------- */

PyObject *PySys_GetObject(const char *name) { if (!sys_module) return NULL; return PyDict_GetItemString(PyModule_GetDict(sys_module), name); }
int PySys_SetObject(const char *name, PyObject *v) { if (!sys_module) return -1; return v ? PyDict_SetItemString(PyModule_GetDict(sys_module), name, v) : PyDict_DelItemString(PyModule_GetDict(sys_module), name); }
PyObject *piper_sys_module(void) { return sys_module; }

static PyObject *sys_exit(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (piper_args_range("exit", n, 0, 1) < 0) return NULL; PyErr_SetObject(PyExc_SystemExit, n ? a[0] : Py_None); return NULL; }
static PyObject *sys_exc_info(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); PyObject *e = piper_current_handled_exception(); if (!e) return PyTuple_Pack(3, Py_None, Py_None, Py_None); PyObject *tb = PyException_GetTraceback(e); PyObject *t = PyTuple_Pack(3, (PyObject *)Py_TYPE(e), e, tb ? tb : Py_None); Py_XDECREF(tb); return t; }
static PyObject *sys_exception(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); PyObject *e = piper_current_handled_exception(); return Py_NewRef(e ? e : Py_None); }
static PyObject *sys_getrecursionlimit(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyLong_FromLong(1000); }
static PyObject *sys_setrecursionlimit(PyObject *m, PyObject *o) { PIPER_UNUSED(m); PIPER_UNUSED(o); Py_RETURN_NONE; }
static PyObject *sys_getsizeof(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (n < 1) { PyErr_SetString(PyExc_TypeError, "getsizeof() missing argument"); return NULL; } PyObject *r = PyObject_CallMethod(a[0], "__sizeof__", NULL); if (!r && n == 2) { PyErr_Clear(); return Py_NewRef(a[1]); } return r; }
static PyObject *sys_intern(PyObject *m, PyObject *s) { PIPER_UNUSED(m); if (!PyUnicode_CheckExact(s)) { PyErr_SetString(PyExc_TypeError, "intern() argument must be str"); return NULL; } PyObject *r = Py_NewRef(s); PyUnicode_InternInPlace(&r); return r; }
static PyObject *sys_getrefcount(PyObject *m, PyObject *o) { PIPER_UNUSED(m); return PyLong_FromSsize_t(Py_REFCNT(o)); }
static PyObject *sys_getdefaultencoding(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyUnicode_FromString("utf-8"); }
static PyObject *sys_getfilesystemencoding(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyUnicode_FromString("utf-8"); }
static PyObject *sys_getfilesystemencodeerrors(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyUnicode_FromString("surrogateescape"); }
static PyObject *sys_excepthook_impl(PyObject *m, PyObject *const *a, Py_ssize_t n) { PIPER_UNUSED(m); if (n != 3) { PyErr_SetString(PyExc_TypeError, "excepthook expected 3 arguments"); return NULL; } if (PyExceptionInstance_Check(a[1])) PyErr_DisplayException(a[1]); Py_RETURN_NONE; }
static PyObject *sys_is_finalizing(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); Py_RETURN_FALSE; }
static PyObject *sys_getframe(PyObject *m, PyObject *const *a, Py_ssize_t n) {
    PIPER_UNUSED(m);
    Py_ssize_t depth = n ? PyLong_AsSsize_t(a[0]) : 0;
    if (depth == -1 && PyErr_Occurred()) return NULL;
    int idx = frame_depth - 1 - (int)depth;
    if (idx < 0 || idx >= frame_cap) { PyErr_SetString(PyExc_ValueError, "call stack is not deep enough"); return NULL; }
    PyObject *fn = PyUnicode_FromString(frames[idx].filename ? frames[idx].filename : "<unknown>"), *func = PyUnicode_FromString(frames[idx].funcname ? frames[idx].funcname : "<module>");
    frameobject *f = (frameobject *)piper_fake_frame_new(fn, func, frames[idx].lineno);
    Py_DECREF(fn); Py_DECREF(func);
    if (f) { Py_XSETREF(f->globals, Py_XNewRef(frames[idx].globals)); }
    return (PyObject *)f;
}
static PyMethodDef sys_methods[] = {
    { "exit", (PyCFunction)(void (*)(void))sys_exit, METH_FASTCALL, NULL }, { "exc_info", sys_exc_info, METH_NOARGS, NULL }, { "exception", sys_exception, METH_NOARGS, NULL },
    { "getrecursionlimit", sys_getrecursionlimit, METH_NOARGS, NULL }, { "setrecursionlimit", sys_setrecursionlimit, METH_O, NULL },
    { "getsizeof", (PyCFunction)(void (*)(void))sys_getsizeof, METH_FASTCALL, NULL }, { "intern", sys_intern, METH_O, NULL }, { "getrefcount", sys_getrefcount, METH_O, NULL },
    { "getdefaultencoding", sys_getdefaultencoding, METH_NOARGS, NULL }, { "getfilesystemencoding", sys_getfilesystemencoding, METH_NOARGS, NULL },
    { "getfilesystemencodeerrors", sys_getfilesystemencodeerrors, METH_NOARGS, NULL },
    { "excepthook", (PyCFunction)(void (*)(void))sys_excepthook_impl, METH_FASTCALL, NULL }, { "is_finalizing", sys_is_finalizing, METH_NOARGS, NULL },
    { "_getframe", (PyCFunction)(void (*)(void))sys_getframe, METH_FASTCALL, NULL },
    { NULL, NULL, 0, NULL },
};

static void append_search_paths(PyObject *path, const char *value) {
    if (!value) return;
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const char *start = value;
    for (const char *p = value;; p++) {
        if (*p == separator || *p == '\0') {
            PyObject *entry = PyUnicode_FromStringAndSize(start, p - start);
            if (entry) { PyList_Append(path, entry); Py_DECREF(entry); }
            if (!*p) break;
            start = p + 1;
        }
    }
}

static void init_search_path(PyObject *path, const char *executable) {
    if (executable && *executable) {
        const char *slash = strrchr(executable, '/');
#ifdef _WIN32
        const char *backslash = strrchr(executable, '\\');
        if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
        PyObject *dir = slash ? PyUnicode_FromStringAndSize(executable, slash - executable) : PyUnicode_FromString("");
        if (dir) { PyList_Append(path, dir); Py_DECREF(dir); }
    } else {
        PyObject *empty = PyUnicode_FromString("");
        if (empty) { PyList_Append(path, empty); Py_DECREF(empty); }
    }
    append_search_paths(path, getenv("PIPERPATH"));
    append_search_paths(path, getenv("PYTHONPATH"));
}

void piper_init_sys_core(int argc, char **argv) {
    if (sys_module) return;
    sys_module = piper_new_stdlib_module("sys");
    PyObject *d = PyModule_GetDict(sys_module);
    PyObject *argv_list = PyList_New(0);
    for (int i = 0; i < argc; i++) { PyObject *s = PyUnicode_DecodeFSDefault(argv[i]); PyList_Append(argv_list, s); Py_DECREF(s); }
    PyDict_SetItemString(d, "argv", argv_list); Py_DECREF(argv_list);
    PyObject *orig = PyList_GetSlice(argv_list, 0, PyList_GET_SIZE(argv_list)); PyDict_SetItemString(d, "orig_argv", orig); Py_DECREF(orig);
    PyObject *so = make_file(stdout, 1, "<stdout>", "w", 0), *se = make_file(stderr, 2, "<stderr>", "w", 0), *si = make_file(stdin, 0, "<stdin>", "r", 0);
    PyDict_SetItemString(d, "stdout", so); PyDict_SetItemString(d, "__stdout__", so); Py_DECREF(so);
    PyDict_SetItemString(d, "stderr", se); PyDict_SetItemString(d, "__stderr__", se); Py_DECREF(se);
    PyDict_SetItemString(d, "stdin", si); PyDict_SetItemString(d, "__stdin__", si); Py_DECREF(si);
    PyDict_SetItemString(d, "modules", piper_modules_dict());
}

void piper_init_sys(int argc, char **argv) {
    piper_init_sys_core(argc, argv);
    PyObject *d = PyModule_GetDict(sys_module);
    PyModule_AddFunctions(sys_module, sys_methods);
    PyObject *v;
    v = PyUnicode_FromString("3.14.7 (piper)"); PyDict_SetItemString(d, "version", v); Py_DECREF(v);
    v = PyLong_FromLong(0x030E07F0); PyDict_SetItemString(d, "hexversion", v); Py_DECREF(v);
    PyObject *vi = PyTuple_New(5);
    PyTuple_SET_ITEM(vi, 0, PyLong_FromLong(3)); PyTuple_SET_ITEM(vi, 1, PyLong_FromLong(14)); PyTuple_SET_ITEM(vi, 2, PyLong_FromLong(7)); PyTuple_SET_ITEM(vi, 3, PyUnicode_FromString("final")); PyTuple_SET_ITEM(vi, 4, PyLong_FromLong(0));
    PyDict_SetItemString(d, "version_info", vi); Py_DECREF(vi);
    PyObject *implementation_values = PyDict_New();
    if (implementation_values) {
        PyObject *name = PyUnicode_FromString("piper");
        PyObject *cache_tag = PyUnicode_FromString("piper-314");
        PyObject *multiarch = PyUnicode_FromString(
#if defined(__APPLE__)
            "darwin"
#elif defined(_WIN32)
            "windows"
#else
            "linux"
#endif
        );
        PyDict_SetItemString(implementation_values, "name", name); Py_DECREF(name);
        PyDict_SetItemString(implementation_values, "cache_tag", cache_tag); Py_DECREF(cache_tag);
        PyDict_SetItemString(implementation_values, "version", PyDict_GetItemString(d, "version_info"));
        PyDict_SetItemString(implementation_values, "hexversion", PyDict_GetItemString(d, "hexversion"));
        PyDict_SetItemString(implementation_values, "_multiarch", multiarch); Py_DECREF(multiarch);
        PyDict_SetItemString(implementation_values, "supports_isolated_interpreters", Py_False);
        PyObject *empty = PyTuple_New(0);
        PyObject *implementation = empty ? PyObject_Call((PyObject *)&PySimpleNamespace_Type, empty, implementation_values) : NULL;
        Py_XDECREF(empty); Py_DECREF(implementation_values);
        if (implementation) { PyDict_SetItemString(d, "implementation", implementation); Py_DECREF(implementation); }
    }
    v = PyUnicode_FromString(
#if defined(__APPLE__)
        "darwin"
#elif defined(_WIN32)
        "win32"
#else
        "linux"
#endif
    ); PyDict_SetItemString(d, "platform", v); Py_DECREF(v);
    v = PyUnicode_FromString("piper"); PyDict_SetItemString(d, "implementation_name", v); Py_DECREF(v);
    v = PyUnicode_FromString(sizeof(void *) == 8 ? "little" : "little"); PyDict_SetItemString(d, "byteorder", v); Py_DECREF(v);
    v = PyLong_FromSsize_t(PY_SSIZE_T_MAX); PyDict_SetItemString(d, "maxsize", v); Py_DECREF(v);
    v = PyLong_FromLong(0x10ffff); PyDict_SetItemString(d, "maxunicode", v); Py_DECREF(v);
    v = PyList_New(0); init_search_path(v, argc > 0 ? argv[0] : NULL); PyDict_SetItemString(d, "path", v); Py_DECREF(v);
    v = PyTuple_New(3);
    PyTuple_SET_ITEM(v, 0, PyUnicode_FromString("builtins"));
    PyTuple_SET_ITEM(v, 1, PyUnicode_FromString("sys"));
    PyTuple_SET_ITEM(v, 2, PyUnicode_FromString(
#ifdef _WIN32
        "nt"
#else
        "posix"
#endif
    ));
    PyDict_SetItemString(d, "builtin_module_names", v); Py_DECREF(v);
    v = PyUnicode_FromString(argc > 0 ? argv[0] : ""); PyDict_SetItemString(d, "executable", v); Py_DECREF(v);
    v = PyUnicode_FromString(""); PyDict_SetItemString(d, "prefix", v); PyDict_SetItemString(d, "exec_prefix", v); PyDict_SetItemString(d, "base_prefix", v); PyDict_SetItemString(d, "base_exec_prefix", v); Py_DECREF(v);
    PyObject *fi = PyFloat_GetInfo(); PyDict_SetItemString(d, "float_info", fi); Py_DECREF(fi);
    PyObject *ii = PyLong_GetInfo(); PyDict_SetItemString(d, "int_info", ii); Py_DECREF(ii);
    v = PyUnicode_FromString("short"); PyDict_SetItemString(d, "float_repr_style", v); Py_DECREF(v);
    PyDict_SetItemString(d, "__excepthook__", PyDict_GetItemString(d, "excepthook"));
    PyDict_SetItemString(d, "flags", Py_None);
    v = PyList_New(0); PyDict_SetItemString(d, "warnoptions", v); Py_DECREF(v);
    v = PyDict_New(); PyDict_SetItemString(d, "_xoptions", v); Py_DECREF(v);
    v = PyList_New(0); PyDict_SetItemString(d, "meta_path", v); Py_DECREF(v);
    v = PyList_New(0); PyDict_SetItemString(d, "path_hooks", v); Py_DECREF(v);
    v = PyDict_New(); PyDict_SetItemString(d, "path_importer_cache", v); Py_DECREF(v);
}

void piper_set_argv(int argc, char **argv) { if (sys_module) { PyObject *l = PyList_New(0); for (int i = 0; i < argc; i++) { PyObject *s = PyUnicode_DecodeFSDefault(argv[i]); PyList_Append(l, s); Py_DECREF(s); } PySys_SetObject("argv", l); Py_DECREF(l); } }

/* ---- initialization ----------------------------------------------------------- */

static void ready_type(PyTypeObject *t) {
    if (!t->tp_alloc) t->tp_alloc = PyType_GenericAlloc;
    if (!t->tp_free) t->tp_free = PyObject_Free;
    if (!t->tp_getattro && !t->tp_getattr) t->tp_getattro = PyObject_GenericGetAttr;
    PyType_Ready(t);
}

void piper_init_types_core(void) {
    static int ready;
    if (ready) return;
    ready = 1;
    PyTypeObject *types[] = { &PyBaseObject_Type, &PyType_Type, &_PyNone_Type,
        &_PyNotImplemented_Type, &PyEllipsis_Type, &PyLong_Type, &PyBool_Type,
        &PyUnicode_Type, &PyTuple_Type, &PyList_Type, &PyDict_Type,
        &PyModule_Type, &PyModuleDef_Type, &PyTraceBack_Type, &PyFrame_Type,
        &PyPiperFile_Type };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) ready_type(types[i]);
}

void piper_init_types(void) {
    piper_init_types_core();
    PyTypeObject *types[] = {
        &PyBaseObject_Type, &PyType_Type, &_PyNone_Type, &_PyNotImplemented_Type, &PyEllipsis_Type, &PyLong_Type, &PyBool_Type, &PyFloat_Type, &PyComplex_Type,
        &PyUnicode_Type, &PyUnicodeIter_Type, &PyBytes_Type, &PyBytesIter_Type, &PyByteArray_Type, &PyMemoryView_Type, &PyTuple_Type, &PyTupleIter_Type, &PyList_Type, &PyListIter_Type,
        &PyDict_Type, &PyDictKeys_Type, &PyDictValues_Type, &PyDictItems_Type, &PyDictIterKey_Type, &PyDictIterValue_Type, &PyDictIterItem_Type, &PySet_Type, &PyFrozenSet_Type, &PySetIter_Type,
        &PySlice_Type, &PyRange_Type, &PyRangeIter_Type, &PySeqIter_Type, &PyCallIter_Type, &PyEnum_Type, &PyZip_Type, &PyMap_Type, &PyFilter_Type, &PyReversed_Type,
        &PyCFunction_Type, &PyMethodDescr_Type, &PyMemberDescr_Type, &PyGetSetDescr_Type, &PyWrapperDescr_Type, &PyMethod_Type, &PyStaticMethod_Type, &PyClassMethod_Type,
        &PyProperty_Type, &PySuper_Type, &PyFunction_Type, &PyCell_Type, &PyCode_Type, &PyModule_Type, &PyModuleDef_Type, &PyCapsule_Type, &_PyWeakref_RefType, &PyTemplate_Type, &PyInterpolation_Type, &PyGen_Type, &PyCoro_Type, &PyAsyncGen_Type, &PyAsyncGenASend_Type, &PyTypeAlias_Type, &PyTypeVar_Type, &PyParamSpec_Type, &PyTypeVarTuple_Type, &PyTraceBack_Type, &PyFrame_Type, &PyPiperFile_Type, &PyDictProxy_Type, &PyClassMethodDescr_Type, &PyMethodWrapper_Type, &PySimpleNamespace_Type,
    };
    extern PyTypeObject PyGenericAlias_Type, PyUnion_Type;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        PyTypeObject *t = types[i];
        ready_type(t);
    }
    PyType_Ready(&PyGenericAlias_Type);
    PyType_Ready(&PyUnion_Type);
    extern void piper_type_add_slot_wrappers(PyTypeObject *tp);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) piper_type_add_slot_wrappers(types[i]);
}

static void piper_initialize_base(void) {
    if (initialized) return;
    initialized = 1;
    piper_init_int();
    piper_init_exceptions_core();
    piper_init_types_core();
    extern void piper_type_add_slot_wrappers(PyTypeObject *tp);
    const char *exc_names[] = { "BaseException", "Exception", NULL };
    for (int i = 0; exc_names[i]; i++) piper_type_add_slot_wrappers((PyTypeObject *)piper_exc_type(exc_names[i]));
    piper_init_builtins_core();
    piper_init_sys_core(0, NULL);
    main_module = piper_new_stdlib_module("__main__");
    PyDict_SetItemString(PyModule_GetDict(main_module), "__builtins__", PyDict_GetItemString(piper_modules_dict(), "builtins"));
}

void piper_initialize(void) {
    static int full_initialized;
    piper_initialize_base();
    if (full_initialized) return;
    full_initialized = 1;
    piper_init_exceptions();
    piper_init_types();
    piper_init_builtins();
    piper_init_sys(0, NULL);
    piper_init_math();
    piper_init_time();
    piper_init_errno();
    piper_init_templatelib();
    piper_init_typealias();
    piper_init_types_module();
    piper_init_abc();
    piper_init_atexit();
    piper_init_weakref();
    piper_init_platform();
}
int Py_IsInitialized(void) { return initialized; }
void Py_Initialize(void) { piper_initialize(); }
void Py_InitializeEx(int i) { PIPER_UNUSED(i); piper_initialize(); }
int Py_FinalizeEx(void) { fflush(stdout); fflush(stderr); return 0; }
void Py_Finalize(void) { Py_FinalizeEx(); }
static void (*atexit_funcs[32])(void);
static int natexit = 0;
int Py_AtExit(void (*func)(void)) { if (natexit >= 32) return -1; atexit_funcs[natexit++] = func; return 0; }
void Py_Exit(int status) { Py_FinalizeEx(); exit(status); }
void _Py_FatalErrorFunc(const char *func, const char *message) {
    fprintf(stderr, "Fatal Python error: %s: %s\n", func ? func : "piper", message ? message : "fatal error");
    fflush(stderr);
    abort();
}
const char *Py_GetVersion(void) { return "3.14.7 (piper)"; }
const char *Py_GetPlatform(void) {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#else
    return "linux";
#endif
}
void piper_fatal(const char *msg) { fprintf(stderr, "Fatal Python error: %s\n", msg); abort(); }

/* GIL: single-threaded for now; the API exists so extensions link. */
void PyEval_InitThreads(void) {}
void *PyEval_SaveThread(void) { return (void *)1; }
void PyEval_RestoreThread(void *ts) { PIPER_UNUSED(ts); }
int PyGILState_Check(void) { return 1; }
int PyGILState_Ensure(void) { return 0; }
void PyGILState_Release(int s) { PIPER_UNUSED(s); }
static int fake_tstate;
void *PyThreadState_Get(void) { return &fake_tstate; }
void *PyThreadState_GetUnchecked(void) { return &fake_tstate; }
PyObject *PyEval_EvalCode(PyObject *co, PyObject *g, PyObject *l) { PIPER_UNUSED(co); PIPER_UNUSED(g); PIPER_UNUSED(l); PyErr_SetString(PyExc_NotImplementedError, "eval of code objects is not available yet"); return NULL; }
PyObject *PyRun_StringFlags(const char *s, int start, PyObject *g, PyObject *l, void *flags) { PIPER_UNUSED(s); PIPER_UNUSED(start); PIPER_UNUSED(g); PIPER_UNUSED(l); PIPER_UNUSED(flags); PyErr_SetString(PyExc_NotImplementedError, "exec of source is not available yet"); return NULL; }
PyObject *Py_CompileString(const char *s, const char *f, int start) { PIPER_UNUSED(s); PIPER_UNUSED(f); PIPER_UNUSED(start); PyErr_SetString(PyExc_NotImplementedError, "compile() is not available yet"); return NULL; }

/* ---- program entry -------------------------------------------------------------- */

void piper_handle_system_exit(PyObject *e) {
    PyObject *code = PyObject_GetAttrString(e, "code");
    if (!code) { PyErr_Clear(); Py_Exit(0); }
    int status = 0;
    if (code == Py_None) status = 0;
    else if (PyLong_Check(code)) status = (int)PyLong_AsLong(code);
    else { PyObject *s = PyObject_Str(code); if (s) { fprintf(stderr, "%s\n", PyUnicode_AsUTF8(s)); Py_DECREF(s); } else PyErr_Clear(); status = 1; }
    Py_DECREF(code);
    Py_Exit(status);
}

int piper_run_main_module(PyObject *(*module_init)(void)) {
    PyObject *r = module_init();
    if (r) { Py_DECREF(r); return 0; }
    if (PyErr_ExceptionMatches(PyExc_SystemExit)) { PyObject *e = PyErr_GetRaisedException(); piper_handle_system_exit(e); }
    PyErr_Print();
    return 1;
}

int piper_main(int argc, char **argv, PyObject *(*module_init)(void)) {
    piper_initialize();
    piper_set_argv(argc, argv);
    int status = piper_run_main_module(module_init);
    for (int i = natexit; i-- > 0;) atexit_funcs[i]();
    Py_FinalizeEx();
    return status;
}

int piper_main_core(int argc, char **argv, PyObject *(*module_init)(void)) {
    piper_initialize_base();
    piper_set_argv(argc, argv);
    int status = piper_run_main_module(module_init);
    Py_FinalizeEx();
    return status;
}

/* ---- helpers for compiled code -------------------------------------------------- */

PyObject *piper_load_global(PyObject *globals, PyObject *builtins, PyObject *name) {
    PyObject *v = PyDict_GetItemWithError(globals, name);
    if (v) return Py_NewRef(v);
    if (PyErr_Occurred()) return NULL;
    v = PyDict_GetItemWithError(builtins, name);
    if (v) return Py_NewRef(v);
    if (PyErr_Occurred()) return NULL;
    PyErr_Format(PyExc_NameError, "name '%U' is not defined", name);
    return NULL;
}
PyObject *piper_load_name(PyObject *locals, PyObject *globals, PyObject *builtins, PyObject *name) {
    if (locals) {
        PyObject *v = PyDict_CheckExact(locals) ? PyDict_GetItemWithError(locals, name) : NULL;
        if (v) return Py_NewRef(v);
        if (PyErr_Occurred()) return NULL;
        if (!PyDict_CheckExact(locals)) { v = PyObject_GetItem(locals, name); if (v) return v; if (!PyErr_ExceptionMatches(PyExc_KeyError)) return NULL; PyErr_Clear(); }
    }
    return piper_load_global(globals, builtins, name);
}
int piper_store_global(PyObject *globals, PyObject *name, PyObject *v) { return PyDict_SetItem(globals, name, v); }
int piper_delete_global(PyObject *globals, PyObject *name) {
    int r = PyDict_DelItem(globals, name);
    if (r < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) { PyErr_Clear(); PyErr_Format(PyExc_NameError, "name '%U' is not defined", name); }
    return r;
}

int piper_unpack_sequence(PyObject *seq, Py_ssize_t n, PyObject **out) {
    if (PyTuple_CheckExact(seq) || PyList_CheckExact(seq)) {
        Py_ssize_t m = Py_SIZE(seq);
        if (m != n) {
            if (m < n) PyErr_Format(PyExc_ValueError, "not enough values to unpack (expected %zd, got %zd)", n, m);
            else PyErr_Format(PyExc_ValueError, "too many values to unpack (expected %zd, got %zd)", n, m);
            return -1;
        }
        PyObject **items = PySequence_Fast_ITEMS(seq);
        for (Py_ssize_t i = 0; i < n; i++) out[i] = Py_NewRef(items[i]);
        return 0;
    }
    PyObject *it = PyObject_GetIter(seq);
    if (!it) { if (PyErr_ExceptionMatches(PyExc_TypeError) && !Py_TYPE(seq)->tp_iter && !PySequence_Check(seq)) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "cannot unpack non-iterable %s object", Py_TYPE(seq)->tp_name); } return -1; }
    Py_ssize_t i = 0;
    for (; i < n; i++) {
        out[i] = PyIter_Next(it);
        if (!out[i]) {
            if (!PyErr_Occurred()) PyErr_Format(PyExc_ValueError, "not enough values to unpack (expected %zd, got %zd)", n, i);
            for (Py_ssize_t j = 0; j < i; j++) Py_DECREF(out[j]);
            Py_DECREF(it);
            return -1;
        }
    }
    PyObject *extra = PyIter_Next(it);
    Py_DECREF(it);
    if (extra) { Py_DECREF(extra); for (Py_ssize_t j = 0; j < n; j++) Py_DECREF(out[j]); PyErr_Format(PyExc_ValueError, "too many values to unpack (expected %zd)", n); return -1; }
    if (PyErr_Occurred()) { for (Py_ssize_t j = 0; j < n; j++) Py_DECREF(out[j]); return -1; }
    return 0;
}

int piper_unpack_ex(PyObject *seq, Py_ssize_t before, Py_ssize_t after, PyObject **out) {
    PyObject *l = PySequence_List(seq);
    if (!l) { if (PyErr_ExceptionMatches(PyExc_TypeError) && !Py_TYPE(seq)->tp_iter && !PySequence_Check(seq)) { PyErr_Clear(); PyErr_Format(PyExc_TypeError, "cannot unpack non-iterable %s object", Py_TYPE(seq)->tp_name); } return -1; }
    Py_ssize_t n = PyList_GET_SIZE(l);
    if (n < before + after) { PyErr_Format(PyExc_ValueError, "not enough values to unpack (expected at least %zd, got %zd)", before + after, n); Py_DECREF(l); return -1; }
    for (Py_ssize_t i = 0; i < before; i++) out[i] = Py_NewRef(PyList_GET_ITEM(l, i));
    out[before] = PyList_GetSlice(l, before, n - after);
    for (Py_ssize_t i = 0; i < after; i++) out[before + 1 + i] = Py_NewRef(PyList_GET_ITEM(l, n - after + i));
    Py_DECREF(l);
    return 0;
}

/* ---- class creation --------------------------------------------------------------- */

PyObject *piper_build_class_body_ns(void) { return PyDict_New(); }
int piper_setup_annotations(PyObject *ns) {
    PyObject *a = PyDict_CheckExact(ns) ? PyDict_GetItemString(ns, "__annotations__") : NULL;
    if (a) return 0;
    a = PyDict_New();
    if (!a) return -1;
    int r = PyObject_SetItem(ns, piper_intern("__annotations__"), a);
    Py_DECREF(a);
    return r;
}

/* func_body(ns) fills the namespace; then metaclass(name, bases, ns, **kwds). */
PyObject *piper_make_class(PyObject *func_body, PyObject *name, PyObject *bases, PyObject *kwds, PyObject *globals) {
    PIPER_UNUSED(globals);
    PyObject *meta = NULL;
    PyObject *mkw = kwds ? PyDict_Copy(kwds) : NULL;
    if (mkw) { meta = PyDict_GetItemString(mkw, "metaclass"); if (meta) { Py_INCREF(meta); PyDict_DelItemString(mkw, "metaclass"); } }
    /* __mro_entries__ */
    PyObject *orig_bases = bases;
    PyObject *new_bases = NULL;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        PyObject *b = PyTuple_GET_ITEM(bases, i);
        if (PyType_Check(b)) { if (new_bases) PyList_Append(new_bases, b); continue; }
        PyObject *me;
        if (PyObject_GetOptionalAttrString(b, "__mro_entries__", &me) < 0) { Py_XDECREF(new_bases); Py_XDECREF(meta); Py_XDECREF(mkw); return NULL; }
        if (!me) { if (new_bases) PyList_Append(new_bases, b); continue; }
        PyObject *r = PyObject_CallOneArg(me, bases);
        Py_DECREF(me);
        if (!r) { Py_XDECREF(new_bases); Py_XDECREF(meta); Py_XDECREF(mkw); return NULL; }
        if (!PyTuple_Check(r)) { Py_DECREF(r); PyErr_SetString(PyExc_TypeError, "__mro_entries__ must return a tuple"); Py_XDECREF(new_bases); Py_XDECREF(meta); Py_XDECREF(mkw); return NULL; }
        if (!new_bases) { new_bases = PyList_New(0); for (Py_ssize_t j = 0; j < i; j++) PyList_Append(new_bases, PyTuple_GET_ITEM(bases, j)); }
        for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(r); j++) PyList_Append(new_bases, PyTuple_GET_ITEM(r, j));
        Py_DECREF(r);
    }
    if (new_bases) { bases = PyList_AsTuple(new_bases); Py_DECREF(new_bases); } else Py_INCREF(bases);
    if (!meta) {
        if (PyTuple_GET_SIZE(bases) == 0) meta = Py_NewRef((PyObject *)&PyType_Type);
        else meta = Py_NewRef((PyObject *)Py_TYPE(PyTuple_GET_ITEM(bases, 0)));
    }
    if (PyType_Check(meta)) {
        /* most derived metaclass */
        PyTypeObject *winner = (PyTypeObject *)meta;
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(bases); i++) {
            PyTypeObject *bm = Py_TYPE(PyTuple_GET_ITEM(bases, i));
            if (PyType_IsSubtype(winner, bm)) continue;
            if (PyType_IsSubtype(bm, winner)) { winner = bm; continue; }
            PyErr_SetString(PyExc_TypeError, "metaclass conflict: the metaclass of a derived class must be a (non-strict) subclass of the metaclasses of all its bases");
            Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases); return NULL;
        }
        if ((PyObject *)winner != meta) { Py_DECREF(meta); meta = Py_NewRef((PyObject *)winner); }
    }
    /* __prepare__ */
    PyObject *ns = NULL;
    PyObject *prep;
    if (PyObject_GetOptionalAttrString(meta, "__prepare__", &prep) < 0) { Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases); return NULL; }
    if (prep) { PyObject *pargs[2] = { name, bases }; ns = PyObject_VectorcallDict(prep, pargs, 2, mkw); Py_DECREF(prep); }
    else ns = PyDict_New();
    if (!ns) { Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases); return NULL; }
    if (!PyMapping_Check(ns)) { PyErr_Format(PyExc_TypeError, "%s.__prepare__() must return a mapping, not %s", PyType_Check(meta) ? ((PyTypeObject *)meta)->tp_name : "<metaclass>", Py_TYPE(ns)->tp_name); Py_DECREF(ns); Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases); return NULL; }
    /* run the body: func_body(ns) -> returns the __classcell__ or None */
    PyObject *cell = PyObject_CallOneArg(func_body, ns);
    if (!cell) { Py_DECREF(ns); Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases); return NULL; }
    if (bases != orig_bases) { if (PyObject_SetItem(ns, piper_intern("__orig_bases__"), orig_bases) < 0) { Py_DECREF(cell); Py_DECREF(ns); Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases); return NULL; } }
    PyObject *margs[3] = { name, bases, ns };
    PyObject *cls = PyObject_VectorcallDict(meta, margs, 3, mkw);
    if (cls && cell != Py_None && Py_IS_TYPE(cell, &PyCell_Type)) {
        PyObject *cur = PyCell_Get(cell);
        if (!cur) PyCell_Set(cell, cls);
        else { if (cur != cls) { PyErr_Format(PyExc_TypeError, "__class__ set to %R defining %R as %R", cur, name, cls); Py_DECREF(cur); Py_CLEAR(cls); } else Py_DECREF(cur); }
    }
    Py_DECREF(cell); Py_DECREF(ns); Py_DECREF(meta); Py_XDECREF(mkw); Py_DECREF(bases);
    return cls;
}

/* ---- imports (in-process modules only, for now) ---------------------------------- */

PyObject *PyImport_ImportModuleLevelObject(PyObject *name, PyObject *globals, PyObject *locals, PyObject *fromlist, int level) {
    PIPER_UNUSED(locals);
    PyObject *absname;
    if (level > 0) {
        PyObject *pkg = globals ? PyDict_GetItemString(globals, "__package__") : NULL;
        if (!pkg || pkg == Py_None) { PyObject *n = globals ? PyDict_GetItemString(globals, "__name__") : NULL; PyObject *path = globals ? PyDict_GetItemString(globals, "__path__") : NULL; if (n && path) pkg = n; else if (n) { const char *s = PyUnicode_AsUTF8(n); const char *dot = strrchr(s, '.'); pkg = dot ? PyUnicode_FromStringAndSize(s, dot - s) : NULL; if (!pkg) { PyErr_SetString(PyExc_ImportError, "attempted relative import with no known parent package"); return NULL; } } }
        else Py_INCREF(pkg);
        if (!pkg) { PyErr_SetString(PyExc_ImportError, "attempted relative import with no known parent package"); return NULL; }
        const char *base = PyUnicode_AsUTF8(pkg);
        char *buf = PyMem_Malloc(strlen(base) + 1);
        strcpy(buf, base);
        for (int i = 1; i < level; i++) { char *dot = strrchr(buf, '.'); if (!dot) { PyMem_Free(buf); Py_DECREF(pkg); PyErr_SetString(PyExc_ImportError, "attempted relative import beyond top-level package"); return NULL; } *dot = 0; }
        absname = PyUnicode_GET_LENGTH(name) ? PyUnicode_FromFormat("%s.%U", buf, name) : PyUnicode_FromString(buf);
        PyMem_Free(buf); Py_DECREF(pkg);
    } else absname = Py_NewRef(name);
    PyObject *modules = piper_modules_dict();
    PyObject *mod = PyDict_GetItemWithError(modules, absname);
    if (!mod) {
        if (PyErr_Occurred()) { Py_DECREF(absname); return NULL; }
        extern PyObject *piper_import_hook(PyObject *absname);
        mod = piper_import_hook(absname);
        if (!mod) { if (!PyErr_Occurred()) PyErr_Format(PyExc_ModuleNotFoundError, "No module named '%U'", absname); Py_DECREF(absname); return NULL; }
        Py_DECREF(mod); /* registered in sys.modules by the hook */
    }
    Py_INCREF(mod);
    if (!fromlist || fromlist == Py_None || PyObject_IsTrue(fromlist) <= 0) {
        if (level == 0 || PyUnicode_GET_LENGTH(name) > 0) {
            /* return the top-level package */
            const char *s = PyUnicode_AsUTF8(absname);
            const char *dot = strchr(s, '.');
            if (dot && level == 0) {
                PyObject *top = PyUnicode_FromStringAndSize(s, dot - s);
                PyObject *tm = PyDict_GetItem(modules, top);
                Py_DECREF(top);
                Py_DECREF(mod);
                mod = Py_XNewRef(tm);
            } else if (dot) {
                Py_ssize_t cut = PyUnicode_GET_LENGTH(absname) - PyUnicode_GET_LENGTH(name);
                const char *nm = PyUnicode_AsUTF8(name);
                const char *nd = strchr(nm, '.');
                Py_ssize_t keep = cut + (nd ? (Py_ssize_t)(nd - nm) : PyUnicode_GET_LENGTH(name));
                PyObject *top = PyUnicode_Substring(absname, 0, keep);
                PyObject *tm = PyDict_GetItem(modules, top);
                Py_DECREF(top);
                Py_DECREF(mod);
                mod = Py_XNewRef(tm);
            }
        }
    } else {
        /* import submodules named in fromlist */
        PyObject *it = PyObject_GetIter(fromlist);
        if (it) {
            for (;;) {
                PyObject *x = PyIter_Next(it);
                if (!x) break;
                if (PyUnicode_Check(x) && !PyUnicode_EqualToUTF8(x, "*") && !PyObject_HasAttr(mod, x)) {
                    PyObject *sub = PyUnicode_FromFormat("%U.%U", absname, x);
                    PyObject *sm = PyDict_GetItem(modules, sub);
                    if (!sm) { extern PyObject *piper_import_hook(PyObject *absname); sm = piper_import_hook(sub); if (sm) Py_DECREF(sm); else PyErr_Clear(); }
                    Py_DECREF(sub);
                }
                Py_DECREF(x);
            }
            Py_DECREF(it);
        } else PyErr_Clear();
    }
    Py_DECREF(absname);
    return mod;
}
PyObject *piper_import_name(PyObject *globals, PyObject *name, PyObject *fromlist, int level) {
    PyObject *hook = PyDict_GetItemString(piper_builtins(), "__import__");
    if (hook && !PyCFunction_Check(hook)) { PyObject *lv = PyLong_FromLong(level); PyObject *r = PyObject_CallFunctionObjArgs(hook, name, globals, Py_None, fromlist ? fromlist : Py_None, lv, NULL); Py_DECREF(lv); return r; }
    return PyImport_ImportModuleLevelObject(name, globals, NULL, fromlist, level);
}
PyObject *piper_import(PyObject *name, PyObject *globals, PyObject *fromlist, int level) { return piper_import_name(globals, name, fromlist, level); }
PyObject *PyImport_Import(PyObject *name) {
    PyObject *star = PyUnicode_FromString("*");
    if (!star) return NULL;
    PyObject *fromlist = PyTuple_Pack(1, star);
    Py_DECREF(star);
    if (!fromlist) return NULL;
    PyObject *module = PyImport_ImportModuleLevelObject(name, NULL, NULL, fromlist, 0);
    Py_DECREF(fromlist);
    return module;
}
PyObject *PyImport_ImportModule(const char *name) { PyObject *n = PyUnicode_FromString(name); if (!n) return NULL; PyObject *r = PyImport_Import(n); Py_DECREF(n); return r; }
PyObject *piper_import_from(PyObject *module, PyObject *name) {
    PyObject *r = PyObject_GetAttr(module, name);
    if (r) return r;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return NULL;
    PyErr_Clear();
    PyObject *modname = PyObject_GetAttrString(module, "__name__");
    if (modname && PyUnicode_Check(modname)) {
        PyObject *full = PyUnicode_FromFormat("%U.%U", modname, name);
        PyObject *sub = PyDict_GetItem(piper_modules_dict(), full);
        if (sub) { Py_DECREF(full); Py_DECREF(modname); return Py_NewRef(sub); }
        extern PyObject *piper_import_hook(PyObject *absname);
        sub = piper_import_hook(full);
        Py_DECREF(full);
        if (sub) { Py_DECREF(modname); return sub; }
        PyErr_Clear();
        PyErr_Format(PyExc_ImportError, "cannot import name '%U' from '%U'", name, modname);
        Py_DECREF(modname);
        return NULL;
    }
    PyErr_Clear();
    PyErr_Format(PyExc_ImportError, "cannot import name '%U'", name);
    return NULL;
}
int piper_import_star(PyObject *module, PyObject *globals) {
    PyObject *all;
    if (PyObject_GetOptionalAttrString(module, "__all__", &all) < 0) return -1;
    int skip_underscore = 0;
    if (!all) { PyObject *d; if (PyObject_GetOptionalAttrString(module, "__dict__", &d) < 0) return -1; if (!d) { PyErr_SetString(PyExc_ImportError, "from-import-* object has no __dict__ and no __all__"); return -1; } all = PyMapping_Keys(d); Py_DECREF(d); if (!all) return -1; skip_underscore = 1; }
    PyObject *it = PyObject_GetIter(all);
    Py_DECREF(all);
    if (!it) return -1;
    for (;;) {
        PyObject *n = PyIter_Next(it);
        if (!n) break;
        if (!PyUnicode_Check(n)) { Py_DECREF(n); Py_DECREF(it); PyErr_SetString(PyExc_TypeError, "__all__ items must be str"); return -1; }
        if (skip_underscore && PyUnicode_GET_LENGTH(n) && PyUnicode_READ_CHAR(n, 0) == '_') { Py_DECREF(n); continue; }
        PyObject *v = PyObject_GetAttr(module, n);
        if (!v || PyDict_SetItem(globals, n, v) < 0) { Py_XDECREF(v); Py_DECREF(n); Py_DECREF(it); return -1; }
        Py_DECREF(v); Py_DECREF(n);
    }
    Py_DECREF(it);
    return PyErr_Occurred() ? -1 : 0;
}
PyObject *piper_builtin_import_hook(PyObject *name, PyObject *globals, PyObject *locals, PyObject *fromlist, int level) { return PyImport_ImportModuleLevelObject(name, globals, locals, fromlist, level); }

/* ---- with / async helpers ----------------------------------------------------------- */

int piper_with_enter(PyObject *mgr, PyObject **exit_out, PyObject **value_out) {
    PyObject *enter = piper_type_lookup(Py_TYPE(mgr), piper_intern("__enter__"));
    PyObject *exit = piper_type_lookup(Py_TYPE(mgr), piper_intern("__exit__"));
    if (!enter || !exit) {
        PyErr_Format(PyExc_TypeError, "'%s' object does not support the context manager protocol%s", Py_TYPE(mgr)->tp_name, (enter && !exit) ? " (missed __exit__ method)" : (!enter && exit) ? " (missed __enter__ method)" : "");
        return -1;
    }
    descrgetfunc g = Py_TYPE(exit)->tp_descr_get;
    *exit_out = g ? g(exit, mgr, (PyObject *)Py_TYPE(mgr)) : Py_NewRef(exit);
    if (!*exit_out) return -1;
    PyObject *args[1] = { mgr };
    *value_out = PyObject_Vectorcall(enter, args, 1, NULL);
    if (!*value_out) { Py_CLEAR(*exit_out); return -1; }
    return 0;
}
/* Returns 1 if the exception is suppressed, 0 to propagate, -1 on error. */
int piper_with_exit(PyObject *exit, PyObject *exc) {
    PyObject *r;
    if (!exc) { PyObject *a[3] = { Py_None, Py_None, Py_None }; r = PyObject_Vectorcall(exit, a, 3, NULL); }
    else { PyObject *tb = PyException_GetTraceback(exc); PyObject *a[3] = { (PyObject *)Py_TYPE(exc), exc, tb ? tb : Py_None }; r = PyObject_Vectorcall(exit, a, 3, NULL); Py_XDECREF(tb); }
    if (!r) return -1;
    int t = PyObject_IsTrue(r);
    Py_DECREF(r);
    return t;
}

PyObject *piper_get_awaitable(PyObject *o) {
    PyAsyncMethods *am = Py_TYPE(o)->tp_as_async;
    if (am && am->am_await) { PyObject *r = am->am_await(o); if (r && !PyIter_Check(r)) { PyErr_Format(PyExc_TypeError, "__await__() returned non-iterator of type '%s'", Py_TYPE(r)->tp_name); Py_DECREF(r); return NULL; } return r; }
    if (PyIter_Check(o) && !strcmp(Py_TYPE(o)->tp_name, "coroutine")) return Py_NewRef(o);
    PyErr_Format(PyExc_TypeError, "'%s' object can't be awaited", Py_TYPE(o)->tp_name);
    return NULL;
}
PyObject *piper_get_aiter(PyObject *o) {
    PyAsyncMethods *am = Py_TYPE(o)->tp_as_async;
    if (!am || !am->am_aiter) { PyErr_Format(PyExc_TypeError, "'async for' requires an object with __aiter__ method, got %s", Py_TYPE(o)->tp_name); return NULL; }
    return am->am_aiter(o);
}
PyObject *piper_get_anext(PyObject *o) {
    PyAsyncMethods *am = Py_TYPE(o)->tp_as_async;
    if (!am || !am->am_anext) { PyErr_Format(PyExc_TypeError, "'async for' requires an iterator with __anext__ method, got %s", Py_TYPE(o)->tp_name); return NULL; }
    return am->am_anext(o);
}
PyObject *piper_anext_with_default(PyObject *o, PyObject *dflt) { PIPER_UNUSED(dflt); return piper_get_anext(o); }

/* ---- structural pattern matching helpers --------------------------------------------- */

int piper_match_sequence_check(PyObject *subject) {
    if (PyUnicode_Check(subject) || PyBytes_Check(subject) || PyByteArray_Check_impl(subject)) return 0;
    if (Py_TYPE(subject)->tp_flags & Py_TPFLAGS_SEQUENCE) return 1;
    if (PyTuple_Check(subject) || PyList_Check(subject) || PyRange_Check(subject) || PyMemoryView_Check_impl(subject)) return 1;
    return 0;
}
int piper_match_mapping_check(PyObject *subject) { return (Py_TYPE(subject)->tp_flags & Py_TPFLAGS_MAPPING) != 0 || PyDict_Check(subject); }
PyObject *piper_match_keys(PyObject *subject, PyObject *keys) {
    Py_ssize_t n = PyTuple_GET_SIZE(keys);
    PyObject *values = PyTuple_New(n);
    PyObject *get = PyObject_GetAttrString(subject, "get");
    if (!get) { Py_DECREF(values); return NULL; }
    PyObject *dummy = PyObject_CallNoArgs((PyObject *)&PyBaseObject_Type);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *a[2] = { PyTuple_GET_ITEM(keys, i), dummy };
        PyObject *v = PyObject_Vectorcall(get, a, 2, NULL);
        if (!v) { Py_DECREF(values); Py_DECREF(get); Py_DECREF(dummy); return NULL; }
        if (v == dummy) { Py_DECREF(v); Py_DECREF(values); Py_DECREF(get); Py_DECREF(dummy); Py_RETURN_NONE; }
        PyTuple_SET_ITEM(values, i, v);
    }
    Py_DECREF(get); Py_DECREF(dummy);
    return values;
}
PyObject *piper_match_class(PyObject *subject, PyObject *cls, Py_ssize_t npos, PyObject *kwnames) {
    if (!PyType_Check(cls)) { PyErr_SetString(PyExc_TypeError, "called match pattern must be a class"); return NULL; }
    int inst = PyObject_IsInstance(subject, cls);
    if (inst < 0) return NULL;
    if (!inst) Py_RETURN_NONE;
    Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
    PyObject *attrs = PyList_New(0);
    if (npos) {
        PyObject *match_args;
        int self_match = (((PyTypeObject *)cls)->tp_flags & Py_TPFLAGS_MATCH_SELF) != 0;
        if (PyObject_GetOptionalAttrString(cls, "__match_args__", &match_args) < 0) { Py_DECREF(attrs); return NULL; }
        if (match_args) {
            if (!PyTuple_Check(match_args)) { Py_DECREF(match_args); Py_DECREF(attrs); PyErr_Format(PyExc_TypeError, "%s.__match_args__ must be a tuple (got %s)", ((PyTypeObject *)cls)->tp_name, Py_TYPE(match_args)->tp_name); return NULL; }
            if (npos > PyTuple_GET_SIZE(match_args)) { Py_DECREF(match_args); Py_DECREF(attrs); PyErr_Format(PyExc_TypeError, "%s() accepts %zd positional sub-pattern%s (%zd given)", ((PyTypeObject *)cls)->tp_name, PyTuple_GET_SIZE(match_args), PyTuple_GET_SIZE(match_args) == 1 ? "" : "s", npos); return NULL; }
            for (Py_ssize_t i = 0; i < npos; i++) {
                PyObject *an = PyTuple_GET_ITEM(match_args, i);
                if (!PyUnicode_Check(an)) { Py_DECREF(match_args); Py_DECREF(attrs); PyErr_SetString(PyExc_TypeError, "__match_args__ elements must be strings"); return NULL; }
                PyObject *v = PyObject_GetAttr(subject, an);
                if (!v) { Py_DECREF(match_args); Py_DECREF(attrs); if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); Py_RETURN_NONE; } return NULL; }
                PyList_Append(attrs, v); Py_DECREF(v);
            }
            Py_DECREF(match_args);
        } else if (self_match) {
            if (npos > 1) { Py_DECREF(attrs); PyErr_Format(PyExc_TypeError, "%s() accepts 1 positional sub-pattern (%zd given)", ((PyTypeObject *)cls)->tp_name, npos); return NULL; }
            PyList_Append(attrs, subject);
        } else { Py_DECREF(attrs); PyErr_Format(PyExc_TypeError, "%s() accepts 0 positional sub-patterns (%zd given)", ((PyTypeObject *)cls)->tp_name, npos); return NULL; }
    }
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *v = PyObject_GetAttr(subject, PyTuple_GET_ITEM(kwnames, i));
        if (!v) { Py_DECREF(attrs); if (PyErr_ExceptionMatches(PyExc_AttributeError)) { PyErr_Clear(); Py_RETURN_NONE; } return NULL; }
        PyList_Append(attrs, v); Py_DECREF(v);
    }
    PyObject *t = PyList_AsTuple(attrs);
    Py_DECREF(attrs);
    return t;
}
int PyByteArray_Check_impl(PyObject *o) { return PyObject_TypeCheck(o, &PyByteArray_Type); }
int PyMemoryView_Check_impl(PyObject *o) { return Py_IS_TYPE(o, &PyMemoryView_Type); }

/* ---- super() zero-arg support: compiled code registers the current class cell ---- */
static __thread PyObject **super_class_stack = NULL;
static __thread PyObject **super_self_stack = NULL;
static __thread int super_depth = 0;
static __thread int super_cap = 0;
void piper_super_push(PyObject *cls, PyObject *self) {
    if (super_depth == super_cap) {
        int cap = super_cap ? super_cap * 2 : 32;
        PyObject **a = PyMem_RawRealloc(super_class_stack, (size_t)cap * sizeof(PyObject *));
        PyObject **b = PyMem_RawRealloc(super_self_stack, (size_t)cap * sizeof(PyObject *));
        if (a) super_class_stack = a;
        if (b) super_self_stack = b;
        if (a && b) super_cap = cap;
    }
    if (super_depth < super_cap) { super_class_stack[super_depth] = cls; super_self_stack[super_depth] = self; }
    super_depth++;
}
void piper_super_pop(void) { if (super_depth > 0) super_depth--; }
int piper_super_zero_arg(PyObject **type, PyObject **obj) {
    if (super_depth == 0 || super_depth > super_cap) { PyErr_SetString(PyExc_RuntimeError, "super(): no arguments"); return -1; }
    PyObject *cls = super_class_stack[super_depth - 1];
    if (Py_IS_TYPE(cls, &PyCell_Type)) cls = ((PyCellObject *)cls)->ob_ref;
    if (!cls) { PyErr_SetString(PyExc_RuntimeError, "super(): empty __class__ cell"); return -1; }
    *type = cls;
    *obj = super_self_stack[super_depth - 1];
    if (!*obj) { PyErr_SetString(PyExc_RuntimeError, "super(): no arguments"); return -1; }
    return 0;
}
