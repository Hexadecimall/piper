/* Native operating-system primitives used by the Python os module. */
#include "internal.h"
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define getcwd _getcwd
#define chdir _chdir
#define getpid _getpid
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/random.h>
#include <unistd.h>
extern char **environ;
#endif

static PyObject *stat_field(PyObject *object, void *closure) {
    return Py_NewRef(PyTuple_GET_ITEM(object, (Py_ssize_t)(intptr_t)closure));
}

static PyObject *stat_time_ns(PyObject *object, void *closure) {
    PyObject *seconds = PyTuple_GET_ITEM(object, (Py_ssize_t)(intptr_t)closure);
    long long value = PyLong_AsLongLong(seconds);
    if (value == -1 && PyErr_Occurred()) return NULL;
    return PyLong_FromLongLong(value * 1000000000LL);
}

#define STAT_FIELD(name, index) { name, stat_field, NULL, NULL, (void *)(intptr_t)(index) }
#define STAT_NS(name, index) { name, stat_time_ns, NULL, NULL, (void *)(intptr_t)(index) }
static PyGetSetDef stat_result_getsets[] = {
    STAT_FIELD("st_mode", 0), STAT_FIELD("st_ino", 1), STAT_FIELD("st_dev", 2),
    STAT_FIELD("st_nlink", 3), STAT_FIELD("st_uid", 4), STAT_FIELD("st_gid", 5),
    STAT_FIELD("st_size", 6), STAT_FIELD("st_atime", 7), STAT_FIELD("st_mtime", 8),
    STAT_FIELD("st_ctime", 9), STAT_NS("st_atime_ns", 7), STAT_NS("st_mtime_ns", 8),
    STAT_NS("st_ctime_ns", 9), { NULL, NULL, NULL, NULL, NULL }
};
#undef STAT_FIELD
#undef STAT_NS

static PyTypeObject platform_stat_result_type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "os.stat_result",
    .tp_basicsize = sizeof(PyTupleObject),
    .tp_flags = Py_TPFLAGS_BASETYPE,
    .tp_getset = stat_result_getsets,
    .tp_base = &PyTuple_Type,
};

static PyObject *platform_getcwd(PyObject *module, PyObject *unused) {
    PIPER_UNUSED(module); PIPER_UNUSED(unused);
    char *path = getcwd(NULL, 0);
    if (!path) return PyErr_SetFromErrno(PyExc_OSError);
    PyObject *result = PyUnicode_DecodeFSDefault(path);
    free(path);
    return result;
}

static PyObject *platform_getcwdb(PyObject *module, PyObject *unused) {
    PIPER_UNUSED(module); PIPER_UNUSED(unused);
    char *path = getcwd(NULL, 0);
    if (!path) return PyErr_SetFromErrno(PyExc_OSError);
    PyObject *result = PyBytes_FromString(path);
    free(path);
    return result;
}

static PyObject *platform_chdir(PyObject *module, PyObject *path) {
    PIPER_UNUSED(module);
    const char *value = PyUnicode_AsUTF8(path);
    if (!value) return NULL;
    if (chdir(value) < 0) return PyErr_SetFromErrnoWithFilename(PyExc_OSError, value);
    Py_RETURN_NONE;
}

static PyObject *platform_getpid(PyObject *module, PyObject *unused) {
    PIPER_UNUSED(module); PIPER_UNUSED(unused);
    return PyLong_FromLong((long)getpid());
}

static PyObject *platform_cpu_count(PyObject *module, PyObject *unused) {
    PIPER_UNUSED(module); PIPER_UNUSED(unused);
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info);
    return PyLong_FromUnsignedLong((unsigned long)info.dwNumberOfProcessors);
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) Py_RETURN_NONE;
    return PyLong_FromLong(count);
#endif
}

static int platform_random_bytes(unsigned char *buffer, Py_ssize_t length) {
#ifdef _WIN32
    typedef BOOLEAN (WINAPI *random_fn)(void *, ULONG);
    static random_fn generate = NULL;
    if (!generate) {
        HMODULE library = LoadLibraryA("advapi32.dll");
        if (library) generate = (random_fn)(void *)GetProcAddress(library, "SystemFunction036");
    }
    if (!generate) { errno = EIO; return -1; }
    while (length > 0) {
        ULONG amount = length > 0xffffffffLL ? 0xffffffffUL : (ULONG)length;
        if (!generate(buffer, amount)) { errno = EIO; return -1; }
        buffer += amount;
        length -= amount;
    }
#else
    while (length > 0) {
        size_t amount = length > 256 ? 256 : (size_t)length;
        if (getentropy(buffer, amount) < 0) return -1;
        buffer += amount;
        length -= (Py_ssize_t)amount;
    }
#endif
    return 0;
}

static PyObject *platform_urandom(PyObject *module, PyObject *size) {
    PIPER_UNUSED(module);
    Py_ssize_t length = PyLong_AsSsize_t(size);
    if (length == -1 && PyErr_Occurred()) return NULL;
    if (length < 0) { PyErr_SetString(PyExc_ValueError, "negative argument not allowed"); return NULL; }
    PyObject *result = PyBytes_FromStringAndSize(NULL, length);
    if (!result) return NULL;
    if (platform_random_bytes((unsigned char *)PyBytes_AS_STRING(result), length) < 0) {
        Py_DECREF(result);
        return PyErr_SetFromErrno(PyExc_OSError);
    }
    return result;
}

static const char *platform_path(PyObject *object) {
    if (PyUnicode_Check(object)) return PyUnicode_AsUTF8(object);
    if (PyBytes_Check(object)) return PyBytes_AsString(object);
    PyErr_SetString(PyExc_TypeError, "path should be string, bytes, or path-like object");
    return NULL;
}

static PyObject *platform_stat(PyObject *module, PyObject *path) {
    PIPER_UNUSED(module);
    const char *value = platform_path(path);
    if (!value) return NULL;
    struct stat info;
    if (stat(value, &info) < 0) return PyErr_SetFromErrnoWithFilename(PyExc_OSError, value);
    PyObject *result = PyTuple_New(10);
    if (!result) return NULL;
    PyTuple_SET_ITEM(result, 0, PyLong_FromUnsignedLong((unsigned long)info.st_mode));
    PyTuple_SET_ITEM(result, 1, PyLong_FromUnsignedLongLong((unsigned long long)info.st_ino));
    PyTuple_SET_ITEM(result, 2, PyLong_FromUnsignedLongLong((unsigned long long)info.st_dev));
    PyTuple_SET_ITEM(result, 3, PyLong_FromUnsignedLong((unsigned long)info.st_nlink));
    PyTuple_SET_ITEM(result, 4, PyLong_FromUnsignedLong((unsigned long)info.st_uid));
    PyTuple_SET_ITEM(result, 5, PyLong_FromUnsignedLong((unsigned long)info.st_gid));
    PyTuple_SET_ITEM(result, 6, PyLong_FromLongLong((long long)info.st_size));
    PyTuple_SET_ITEM(result, 7, PyLong_FromLongLong((long long)info.st_atime));
    PyTuple_SET_ITEM(result, 8, PyLong_FromLongLong((long long)info.st_mtime));
    PyTuple_SET_ITEM(result, 9, PyLong_FromLongLong((long long)info.st_ctime));
    Py_SET_TYPE(result, &platform_stat_result_type);
    return result;
}

static PyObject *platform_unlink(PyObject *module, PyObject *path) {
    PIPER_UNUSED(module); const char *value = platform_path(path); if (!value) return NULL;
    if (remove(value) < 0) return PyErr_SetFromErrnoWithFilename(PyExc_OSError, value);
    Py_RETURN_NONE;
}

static PyObject *platform_rmdir(PyObject *module, PyObject *path) {
    PIPER_UNUSED(module); const char *value = platform_path(path); if (!value) return NULL;
    if (rmdir(value) < 0) return PyErr_SetFromErrnoWithFilename(PyExc_OSError, value);
    Py_RETURN_NONE;
}

static PyObject *platform_mkdir(PyObject *module, PyObject *const *args, Py_ssize_t count) {
    PIPER_UNUSED(module);
    if (piper_args_range("mkdir", count, 1, 2) < 0) return NULL;
    const char *value = platform_path(args[0]); if (!value) return NULL;
    long mode = count == 2 ? PyLong_AsLong(args[1]) : 0777;
    if ((mode == -1 && PyErr_Occurred()) || mkdir(value, (int)mode) < 0) return PyErr_SetFromErrnoWithFilename(PyExc_OSError, value);
    Py_RETURN_NONE;
}

static PyObject *platform_rename(PyObject *module, PyObject *const *args, Py_ssize_t count) {
    PIPER_UNUSED(module);
    if (piper_args_range("rename", count, 2, 2) < 0) return NULL;
    const char *source = platform_path(args[0]); if (!source) return NULL;
    const char *target = platform_path(args[1]); if (!target) return NULL;
    if (rename(source, target) < 0) return PyErr_SetFromErrnoWithFilename(PyExc_OSError, source);
    Py_RETURN_NONE;
}

static PyObject *platform_create_environ(PyObject *module, PyObject *unused) {
    PIPER_UNUSED(module); PIPER_UNUSED(unused);
    PyObject *mapping = PyDict_New();
    if (!mapping) return NULL;
#ifdef _WIN32
    char **items = _environ;
#else
    char **items = environ;
#endif
    for (char **item = items; item && *item; item++) {
        const char *equal = strchr(*item, '=');
        if (!equal) continue;
        PyObject *key = PyBytes_FromStringAndSize(*item, equal - *item);
        PyObject *value = PyBytes_FromString(equal + 1);
        if (!key || !value || PyDict_SetItem(mapping, key, value) < 0) {
            Py_XDECREF(key); Py_XDECREF(value); Py_DECREF(mapping); return NULL;
        }
        Py_DECREF(key); Py_DECREF(value);
    }
    return mapping;
}

static PyMethodDef platform_methods[] = {
    { "getcwd", platform_getcwd, METH_NOARGS, NULL },
    { "getcwdb", platform_getcwdb, METH_NOARGS, NULL },
    { "chdir", platform_chdir, METH_O, NULL },
    { "getpid", platform_getpid, METH_NOARGS, NULL },
    { "cpu_count", platform_cpu_count, METH_NOARGS, NULL },
    { "urandom", platform_urandom, METH_O, NULL },
    { "stat", platform_stat, METH_O, NULL },
    { "unlink", platform_unlink, METH_O, NULL },
    { "remove", platform_unlink, METH_O, NULL },
    { "rmdir", platform_rmdir, METH_O, NULL },
    { "mkdir", (PyCFunction)(void (*)(void))platform_mkdir, METH_FASTCALL, NULL },
    { "rename", (PyCFunction)(void (*)(void))platform_rename, METH_FASTCALL, NULL },
    { "_create_environ", platform_create_environ, METH_NOARGS, NULL },
    { NULL, NULL, 0, NULL },
};

void piper_init_platform(void) {
#ifdef _WIN32
    PyObject *module = piper_new_stdlib_module("nt");
#else
    PyObject *module = piper_new_stdlib_module("posix");
#endif
    if (!module) return;
    if (PyType_Ready(&platform_stat_result_type) < 0) return;
    PyModule_AddFunctions(module, platform_methods);
    PyModule_AddObjectRef(module, "stat_result", (PyObject *)&platform_stat_result_type);
    PyModule_AddObject(module, "environ", platform_create_environ(module, NULL));
    PyModule_AddObject(module, "_have_functions", PyList_New(0));
    const char *exports[] = { "environ", "getcwd", "getcwdb", "chdir", "getpid", "cpu_count", "urandom", "stat", "unlink", "remove", "rmdir", "mkdir", "rename",
        "stat_result", "O_RDONLY", "O_WRONLY", "O_RDWR", "O_CREAT", "O_EXCL", "O_TRUNC", "O_APPEND", NULL };
    PyObject *all = PyList_New(0);
    for (int i = 0; exports[i]; i++) {
        PyObject *name = PyUnicode_FromString(exports[i]);
        if (name) { PyList_Append(all, name); Py_DECREF(name); }
    }
    PyModule_AddObject(module, "__all__", all);
    PyModule_AddIntConstant(module, "F_OK", 0);
    PyModule_AddIntConstant(module, "R_OK", 4);
    PyModule_AddIntConstant(module, "W_OK", 2);
    PyModule_AddIntConstant(module, "X_OK", 1);
    PyModule_AddIntConstant(module, "O_RDONLY", O_RDONLY);
    PyModule_AddIntConstant(module, "O_WRONLY", O_WRONLY);
    PyModule_AddIntConstant(module, "O_RDWR", O_RDWR);
    PyModule_AddIntConstant(module, "O_CREAT", O_CREAT);
    PyModule_AddIntConstant(module, "O_EXCL", O_EXCL);
    PyModule_AddIntConstant(module, "O_TRUNC", O_TRUNC);
    PyModule_AddIntConstant(module, "O_APPEND", O_APPEND);
#ifdef O_NONBLOCK
    PyModule_AddIntConstant(module, "O_NONBLOCK", O_NONBLOCK);
    PyObject *nonblock = PyUnicode_FromString("O_NONBLOCK"); PyList_Append(all, nonblock); Py_DECREF(nonblock);
#endif
#ifdef O_DIRECTORY
    PyModule_AddIntConstant(module, "O_DIRECTORY", O_DIRECTORY);
    PyObject *directory = PyUnicode_FromString("O_DIRECTORY"); PyList_Append(all, directory); Py_DECREF(directory);
#endif
#ifdef O_CLOEXEC
    PyModule_AddIntConstant(module, "O_CLOEXEC", O_CLOEXEC);
    PyObject *cloexec = PyUnicode_FromString("O_CLOEXEC"); PyList_Append(all, cloexec); Py_DECREF(cloexec);
#endif
}
