/* Portable clocks and sleeping for the built-in time module. */
#include "internal.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

static double wall_seconds(void) {
#ifdef _WIN32
    FILETIME ft; ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft); u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (double)(u.QuadPart - 116444736000000000ULL) / 10000000.0;
#else
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}

static double steady_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    ts.tv_sec = (time_t)wall_seconds(); ts.tv_nsec = 0;
#endif
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
#endif
}

static PyObject *t_time(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyFloat_FromDouble(wall_seconds()); }
static PyObject *t_time_ns(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyLong_FromUnsignedLongLong((unsigned long long)(wall_seconds() * 1000000000.0)); }
static PyObject *t_monotonic(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyFloat_FromDouble(steady_seconds()); }
static PyObject *t_monotonic_ns(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyLong_FromUnsignedLongLong((unsigned long long)(steady_seconds() * 1000000000.0)); }
static PyObject *t_process_time(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyFloat_FromDouble((double)clock() / CLOCKS_PER_SEC); }
static PyObject *t_process_time_ns(PyObject *m, PyObject *u) { PIPER_UNUSED(m); PIPER_UNUSED(u); return PyLong_FromUnsignedLongLong((unsigned long long)((double)clock() * 1000000000.0 / CLOCKS_PER_SEC)); }

static PyObject *struct_time_value(const struct tm *value) {
    PyObject *result = PyTuple_New(9);
    if (!result) return NULL;
    long values[] = { value->tm_year + 1900, value->tm_mon + 1, value->tm_mday,
        value->tm_hour, value->tm_min, value->tm_sec, (value->tm_wday + 6) % 7,
        value->tm_yday + 1, value->tm_isdst };
    for (int i = 0; i < 9; i++) PyTuple_SET_ITEM(result, i, PyLong_FromLong(values[i]));
    return result;
}

static int tuple_time(PyObject *value, struct tm *out) {
    if (!PyTuple_Check(value) && !PyList_Check(value)) { PyErr_SetString(PyExc_TypeError, "Tuple or struct_time argument required"); return -1; }
    if (PySequence_Size(value) < 9) { PyErr_SetString(PyExc_TypeError, "time tuple must have at least 9 elements"); return -1; }
    long fields[9];
    for (int i = 0; i < 9; i++) { PyObject *item = PySequence_GetItem(value, i); if (!item) return -1; fields[i] = PyLong_AsLong(item); Py_DECREF(item); if (PyErr_Occurred()) return -1; }
    memset(out, 0, sizeof(*out));
    out->tm_year = (int)fields[0] - 1900; out->tm_mon = (int)fields[1] - 1; out->tm_mday = (int)fields[2];
    out->tm_hour = (int)fields[3]; out->tm_min = (int)fields[4]; out->tm_sec = (int)fields[5];
    out->tm_wday = ((int)fields[6] + 1) % 7; out->tm_yday = (int)fields[7] - 1; out->tm_isdst = (int)fields[8];
    return 0;
}

static int broken_down(time_t stamp, int utc, struct tm *out) {
#ifdef _WIN32
    return utc ? gmtime_s(out, &stamp) : localtime_s(out, &stamp);
#else
    return (utc ? gmtime_r(&stamp, out) : localtime_r(&stamp, out)) ? 0 : -1;
#endif
}

static PyObject *t_broken_down(PyObject *m, PyObject *const *args, Py_ssize_t nargs, int utc) {
    PIPER_UNUSED(m); if (piper_args_range(utc ? "gmtime" : "localtime", nargs, 0, 1) < 0) return NULL;
    time_t stamp = nargs ? (time_t)PyFloat_AsDouble(args[0]) : time(NULL);
    if (nargs && PyErr_Occurred()) return NULL;
    struct tm value;
    if (broken_down(stamp, utc, &value) != 0) { PyErr_SetString(PyExc_OSError, "time conversion failed"); return NULL; }
    return struct_time_value(&value);
}
static PyObject *t_localtime(PyObject *m, PyObject *const *a, Py_ssize_t n) { return t_broken_down(m, a, n, 0); }
static PyObject *t_gmtime(PyObject *m, PyObject *const *a, Py_ssize_t n) { return t_broken_down(m, a, n, 1); }
static PyObject *t_mktime(PyObject *m, PyObject *value) { PIPER_UNUSED(m); struct tm tm; if (tuple_time(value, &tm) < 0) return NULL; return PyFloat_FromDouble((double)mktime(&tm)); }
static PyObject *t_strftime(PyObject *m, PyObject *const *args, Py_ssize_t nargs) {
    PIPER_UNUSED(m); if (piper_args_range("strftime", nargs, 1, 2) < 0) return NULL;
    const char *format = PyUnicode_AsUTF8(args[0]); if (!format) return NULL;
    struct tm value;
    if (nargs == 2) { if (tuple_time(args[1], &value) < 0) return NULL; }
    else { time_t now = time(NULL); if (broken_down(now, 0, &value) != 0) { PyErr_SetString(PyExc_OSError, "time conversion failed"); return NULL; } }
    char buffer[4096]; size_t size = strftime(buffer, sizeof(buffer), format, &value);
    if (!size && *format) { PyErr_SetString(PyExc_ValueError, "format string is too long"); return NULL; }
    return PyUnicode_FromStringAndSize(buffer, (Py_ssize_t)size);
}

static PyObject *t_sleep(PyObject *m, PyObject *o) {
    PIPER_UNUSED(m); double seconds = PyFloat_AsDouble(o);
    if (seconds == -1.0 && PyErr_Occurred()) return NULL;
    if (seconds < 0.0) { PyErr_SetString(PyExc_ValueError, "sleep length must be non-negative"); return NULL; }
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000.0));
#else
    struct timespec req = { (time_t)seconds, (long)((seconds - floor(seconds)) * 1000000000.0) };
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {}
#endif
    Py_RETURN_NONE;
}

static PyMethodDef time_methods[] = {
    { "time", t_time, METH_NOARGS, NULL }, { "time_ns", t_time_ns, METH_NOARGS, NULL },
    { "monotonic", t_monotonic, METH_NOARGS, NULL }, { "monotonic_ns", t_monotonic_ns, METH_NOARGS, NULL },
    { "perf_counter", t_monotonic, METH_NOARGS, NULL }, { "perf_counter_ns", t_monotonic_ns, METH_NOARGS, NULL },
    { "process_time", t_process_time, METH_NOARGS, NULL }, { "process_time_ns", t_process_time_ns, METH_NOARGS, NULL },
    { "localtime", (PyCFunction)(void (*)(void))t_localtime, METH_FASTCALL, NULL },
    { "gmtime", (PyCFunction)(void (*)(void))t_gmtime, METH_FASTCALL, NULL },
    { "mktime", t_mktime, METH_O, NULL },
    { "strftime", (PyCFunction)(void (*)(void))t_strftime, METH_FASTCALL, NULL },
    { "sleep", t_sleep, METH_O, NULL }, { NULL, NULL, 0, NULL },
};

void piper_init_time(void) {
    PyObject *m = piper_new_stdlib_module("time");
    if (!m) return;
    PyModule_AddFunctions(m, time_methods);
    PyModule_AddObject(m, "CLOCK_MONOTONIC", PyLong_FromLong(1));
    PyModule_AddObject(m, "CLOCK_REALTIME", PyLong_FromLong(0));
    PyModule_AddObject(m, "timezone", PyLong_FromLong(0));
    PyModule_AddObject(m, "altzone", PyLong_FromLong(0));
    PyModule_AddObjectRef(m, "daylight", Py_False);
}
