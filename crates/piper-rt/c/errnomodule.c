/* Platform errno names and numeric mappings. */
#include "internal.h"

static void add_errno(PyObject *module, PyObject *names, const char *name, int value) {
    PyObject *n = PyLong_FromLong(value);
    PyModule_AddObjectRef(module, name, n);
    PyObject *s = PyUnicode_FromString(name);
    PyDict_SetItem(names, n, s);
    Py_DECREF(s); Py_DECREF(n);
}

#define ADD(name) add_errno(m, names, #name, name)

void piper_init_errno(void) {
    PyObject *m = piper_new_stdlib_module("errno");
    if (!m) return;
    PyObject *names = PyDict_New();
    PyModule_AddObjectRef(m, "errorcode", names);
#ifdef EPERM
    ADD(EPERM);
#endif
#ifdef ENOENT
    ADD(ENOENT);
#endif
#ifdef ESRCH
    ADD(ESRCH);
#endif
#ifdef EINTR
    ADD(EINTR);
#endif
#ifdef EIO
    ADD(EIO);
#endif
#ifdef ENXIO
    ADD(ENXIO);
#endif
#ifdef E2BIG
    ADD(E2BIG);
#endif
#ifdef ENOEXEC
    ADD(ENOEXEC);
#endif
#ifdef EBADF
    ADD(EBADF);
#endif
#ifdef ECHILD
    ADD(ECHILD);
#endif
#ifdef EAGAIN
    ADD(EAGAIN);
#endif
#ifdef ENOMEM
    ADD(ENOMEM);
#endif
#ifdef EACCES
    ADD(EACCES);
#endif
#ifdef EFAULT
    ADD(EFAULT);
#endif
#ifdef EBUSY
    ADD(EBUSY);
#endif
#ifdef EEXIST
    ADD(EEXIST);
#endif
#ifdef EXDEV
    ADD(EXDEV);
#endif
#ifdef ENODEV
    ADD(ENODEV);
#endif
#ifdef ENOTDIR
    ADD(ENOTDIR);
#endif
#ifdef EISDIR
    ADD(EISDIR);
#endif
#ifdef EINVAL
    ADD(EINVAL);
#endif
#ifdef ENFILE
    ADD(ENFILE);
#endif
#ifdef EMFILE
    ADD(EMFILE);
#endif
#ifdef ENOTTY
    ADD(ENOTTY);
#endif
#ifdef EFBIG
    ADD(EFBIG);
#endif
#ifdef ENOSPC
    ADD(ENOSPC);
#endif
#ifdef ESPIPE
    ADD(ESPIPE);
#endif
#ifdef EROFS
    ADD(EROFS);
#endif
#ifdef EMLINK
    ADD(EMLINK);
#endif
#ifdef EPIPE
    ADD(EPIPE);
#endif
#ifdef EDOM
    ADD(EDOM);
#endif
#ifdef ERANGE
    ADD(ERANGE);
#endif
#ifdef EDEADLK
    ADD(EDEADLK);
#endif
#ifdef ENAMETOOLONG
    ADD(ENAMETOOLONG);
#endif
#ifdef ENOLCK
    ADD(ENOLCK);
#endif
#ifdef ENOSYS
    ADD(ENOSYS);
#endif
#ifdef ENOTEMPTY
    ADD(ENOTEMPTY);
#endif
#ifdef ELOOP
    ADD(ELOOP);
#endif
#ifdef ETIMEDOUT
    ADD(ETIMEDOUT);
#endif
#ifdef ECONNREFUSED
    ADD(ECONNREFUSED);
#endif
#ifdef ECONNRESET
    ADD(ECONNRESET);
#endif
#ifdef ECONNABORTED
    ADD(ECONNABORTED);
#endif
#ifdef ENOTCONN
    ADD(ENOTCONN);
#endif
#ifdef EISCONN
    ADD(EISCONN);
#endif
#ifdef EADDRINUSE
    ADD(EADDRINUSE);
#endif
#ifdef EADDRNOTAVAIL
    ADD(EADDRNOTAVAIL);
#endif
#ifdef ENETUNREACH
    ADD(ENETUNREACH);
#endif
#ifdef EHOSTUNREACH
    ADD(EHOSTUNREACH);
#endif
    Py_DECREF(names);
}
