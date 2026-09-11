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
    { "sleep", t_sleep, METH_O, NULL }, { NULL, NULL, 0, NULL },
};

void piper_init_time(void) {
    PyObject *m = piper_new_stdlib_module("time");
    if (!m) return;
    PyModule_AddFunctions(m, time_methods);
    PyModule_AddObject(m, "CLOCK_MONOTONIC", PyLong_FromLong(1));
    PyModule_AddObject(m, "CLOCK_REALTIME", PyLong_FromLong(0));
}
