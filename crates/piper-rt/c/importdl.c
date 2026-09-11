/* Native extension discovery and loading without invoking an external tool. */
#include "internal.h"
#ifdef _WIN32
#include <windows.h>
#define DIR_SEP '\\'
#else
#include <dlfcn.h>
#define DIR_SEP '/'
#endif

typedef PyObject *(*extension_init_fn)(void);
typedef PyObject *(*static_import_fn)(PyObject *);
static static_import_fn static_importer;

void piper_set_static_importer(void *importer) { static_importer = (static_import_fn)importer; }

PyObject *piper_static_import_module(void *initializer, const char *fullname) {
    PyObject *existing = PyDict_GetItemString(piper_modules_dict(), fullname);
    if (existing) return Py_NewRef(existing);
    extension_init_fn init = (extension_init_fn)initializer;
    PyObject *module = init();
    if (!module) return NULL;
    const char *dot = strrchr(fullname, '.');
    if (dot) {
        PyObject *parent_name = PyUnicode_FromStringAndSize(fullname, dot - fullname);
        PyObject *parent = parent_name ? PyDict_GetItem(piper_modules_dict(), parent_name) : NULL;
        if (!parent || PyObject_SetAttrString(parent, dot + 1, module) < 0) {
            Py_XDECREF(parent_name);
            Py_DECREF(module);
            return NULL;
        }
        Py_DECREF(parent_name);
    }
    return module;
}

static int regular_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void *open_library(const char *path) {
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
}

static void *library_symbol(void *handle, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static const char *library_error(void) {
#ifdef _WIN32
    return "operating system loader rejected the extension";
#else
    const char *e = dlerror(); return e ? e : "dynamic loader rejected the extension";
#endif
}

static int bind_to_parent(const char *fullname, PyObject *module) {
    const char *dot = strrchr(fullname, '.');
    if (!dot) return 0;
    PyObject *parent_name = PyUnicode_FromStringAndSize(fullname, dot - fullname);
    if (!parent_name) return -1;
    PyObject *parent = PyDict_GetItem(piper_modules_dict(), parent_name);
    Py_DECREF(parent_name);
    if (!parent) {
        PyErr_Format(PyExc_ImportError, "parent package for %s is not loaded", fullname);
        return -1;
    }
    return PyObject_SetAttrString(parent, dot + 1, module);
}

static PyObject *load_candidate(const char *path, const char *fullname, const char *leaf) {
    if (!regular_file(path)) return NULL;
    void *handle = open_library(path);
    if (!handle) { PyErr_Format(PyExc_ImportError, "cannot load extension '%s': %s", path, library_error()); return NULL; }
    size_t symbol_len = strlen(leaf) + 8;
    char *symbol = PyMem_Malloc(symbol_len);
    if (!symbol) return PyErr_NoMemory();
    snprintf(symbol, symbol_len, "PyInit_%s", leaf);
    extension_init_fn init = (extension_init_fn)library_symbol(handle, symbol);
    PyMem_Free(symbol);
    if (!init) { PyErr_Format(PyExc_ImportError, "extension '%s' has no PyInit_%s entry point", path, leaf); return NULL; }
    PyObject *created = init();
    if (!created) return NULL;
    PyObject *module;
    if (Py_TYPE(created) == &PyModuleDef_Type) module = piper_module_from_def((PyModuleDef *)created, fullname);
    else { module = created; piper_register_module(fullname, module); }
    if (module) {
        PyObject *file = PyUnicode_FromString(path);
        if (file) { PyDict_SetItemString(PyModule_GetDict(module), "__file__", file); Py_DECREF(file); }
        if (bind_to_parent(fullname, module) < 0) { Py_DECREF(module); return NULL; }
    }
    return module;
}

static PyObject *try_directory(const char *dir, const char *fullname, const char *relative, const char *leaf) {
#ifdef _WIN32
    const char *suffixes[] = { ".pyd", ".dll", NULL };
#elif defined(__APPLE__)
    const char *suffixes[] = { ".cpython-314-darwin.so", ".abi3.so", ".so", ".dylib", NULL };
#elif defined(__aarch64__)
    const char *suffixes[] = { ".cpython-314-aarch64-linux-gnu.so", ".abi3.so", ".so", NULL };
#else
    const char *suffixes[] = { ".cpython-314-x86_64-linux-gnu.so", ".abi3.so", ".so", NULL };
#endif
    for (int i = 0; suffixes[i]; i++) {
        size_t n = strlen(dir) + strlen(relative) + strlen(suffixes[i]) + 3;
        char *path = PyMem_Malloc(n);
        if (!path) return PyErr_NoMemory();
        if (*dir) snprintf(path, n, "%s%c%s%s", dir, DIR_SEP, relative, suffixes[i]);
        else snprintf(path, n, "%s%s", relative, suffixes[i]);
        PyObject *m = load_candidate(path, fullname, leaf);
        PyMem_Free(path);
        if (m || PyErr_Occurred()) return m;
    }
    return NULL;
}

PyObject *piper_import_hook(PyObject *absname) {
    if (static_importer) {
        PyObject *module = static_importer(absname);
        if (module || PyErr_Occurred()) return module;
    }
    const char *fullname = PyUnicode_AsUTF8(absname);
    if (!fullname) return NULL;
    size_t n = strlen(fullname);
    char *relative = PyMem_Malloc(n + 1);
    if (!relative) return PyErr_NoMemory();
    memcpy(relative, fullname, n + 1);
    for (size_t i = 0; i < n; i++) if (relative[i] == '.') relative[i] = DIR_SEP;
    const char *leaf = strrchr(fullname, '.'); leaf = leaf ? leaf + 1 : fullname;
    PyObject *path = PySys_GetObject("path");
    if (path && PyList_Check(path)) {
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(path); i++) {
            PyObject *entry = PyList_GET_ITEM(path, i);
            if (!PyUnicode_Check(entry)) continue;
            const char *dir = PyUnicode_AsUTF8(entry);
            if (!dir) { PyErr_Clear(); continue; }
            PyObject *m = try_directory(dir, fullname, relative, leaf);
            if (m || PyErr_Occurred()) { PyMem_Free(relative); return m; }
        }
    }
    PyMem_Free(relative);
    return NULL;
}
