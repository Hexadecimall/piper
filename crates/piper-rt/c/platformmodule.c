/* Native operating-system primitives used by the Python os module. */
#include "internal.h"
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
#include <unistd.h>
extern char **environ;
#endif

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
    PyModule_AddFunctions(module, platform_methods);
    PyModule_AddObject(module, "environ", platform_create_environ(module, NULL));
    PyModule_AddObject(module, "_have_functions", PyList_New(0));
    const char *exports[] = { "environ", "getcwd", "getcwdb", "chdir", "getpid", "cpu_count", "stat", "unlink", "remove", "rmdir", "mkdir", "rename", NULL };
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
}
