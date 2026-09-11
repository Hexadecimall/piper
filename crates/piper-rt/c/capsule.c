#include "internal.h"

typedef struct {
    PyObject_HEAD
    void *pointer;
    const char *name;
    void *context;
    PyCapsule_Destructor destructor;
} PyCapsuleObject;

static int capsule_name_matches(const char *stored, const char *requested) {
    if (!stored || !requested) return stored == requested;
    return strcmp(stored, requested) == 0;
}

static PyCapsuleObject *capsule_checked(PyObject *capsule, const char *function) {
    if (!capsule || !PyCapsule_CheckExact(capsule)) {
        PyErr_Format(PyExc_ValueError, "%s called with invalid PyCapsule object", function);
        return NULL;
    }
    return (PyCapsuleObject *)capsule;
}

static void capsule_dealloc(PyObject *self) {
    PyCapsuleObject *capsule = (PyCapsuleObject *)self;
    if (capsule->destructor) capsule->destructor(self);
    PyObject_Free(self);
}

static PyObject *capsule_repr(PyObject *self) {
    PyCapsuleObject *capsule = (PyCapsuleObject *)self;
    char text[256];
    if (capsule->name) {
        snprintf(text, sizeof(text), "<capsule object \"%s\" at %p>", capsule->name, (void *)self);
    } else {
        snprintf(text, sizeof(text), "<capsule object NULL at %p>", (void *)self);
    }
    return PyUnicode_FromString(text);
}

PyTypeObject PyCapsule_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "PyCapsule",
    .tp_basicsize = sizeof(PyCapsuleObject),
    .tp_dealloc = capsule_dealloc,
    .tp_repr = capsule_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Capsule objects let C code exchange opaque pointers.",
};

PyObject *PyCapsule_New(void *pointer, const char *name, PyCapsule_Destructor destructor) {
    if (!pointer) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_New called with null pointer");
        return NULL;
    }
    PyCapsuleObject *capsule = PyObject_New(PyCapsuleObject, &PyCapsule_Type);
    if (!capsule) return NULL;
    capsule->pointer = pointer;
    capsule->name = name;
    capsule->context = NULL;
    capsule->destructor = destructor;
    return (PyObject *)capsule;
}

int PyCapsule_IsValid(PyObject *capsule, const char *name) {
    if (!capsule || !PyCapsule_CheckExact(capsule)) return 0;
    PyCapsuleObject *value = (PyCapsuleObject *)capsule;
    return value->pointer != NULL && capsule_name_matches(value->name, name);
}

void *PyCapsule_GetPointer(PyObject *capsule, const char *name) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_GetPointer");
    if (!value) return NULL;
    if (!capsule_name_matches(value->name, name)) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_GetPointer called with incorrect name");
        return NULL;
    }
    return value->pointer;
}

PyCapsule_Destructor PyCapsule_GetDestructor(PyObject *capsule) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_GetDestructor");
    return value ? value->destructor : NULL;
}

void *PyCapsule_GetContext(PyObject *capsule) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_GetContext");
    return value ? value->context : NULL;
}

const char *PyCapsule_GetName(PyObject *capsule) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_GetName");
    return value ? value->name : NULL;
}

int PyCapsule_SetPointer(PyObject *capsule, void *pointer) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_SetPointer");
    if (!value) return -1;
    if (!pointer) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_SetPointer called with null pointer");
        return -1;
    }
    value->pointer = pointer;
    return 0;
}

int PyCapsule_SetDestructor(PyObject *capsule, PyCapsule_Destructor destructor) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_SetDestructor");
    if (!value) return -1;
    value->destructor = destructor;
    return 0;
}

int PyCapsule_SetContext(PyObject *capsule, void *context) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_SetContext");
    if (!value) return -1;
    value->context = context;
    return 0;
}

int PyCapsule_SetName(PyObject *capsule, const char *name) {
    PyCapsuleObject *value = capsule_checked(capsule, "PyCapsule_SetName");
    if (!value) return -1;
    value->name = name;
    return 0;
}

void *PyCapsule_Import(const char *name, int no_block) {
    PIPER_UNUSED(no_block);
    if (!name) {
        PyErr_SetString(PyExc_ValueError, "PyCapsule_Import called with null name");
        return NULL;
    }
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) {
        PyErr_Format(PyExc_ImportError, "PyCapsule_Import could not import module %s", name);
        return NULL;
    }
    size_t module_length = (size_t)(dot - name);
    char *module_name = PyMem_Malloc(module_length + 1);
    if (!module_name) return PyErr_NoMemory(), NULL;
    memcpy(module_name, name, module_length);
    module_name[module_length] = '\0';
    PyObject *module = PyImport_ImportModule(module_name);
    PyMem_Free(module_name);
    if (!module) return NULL;
    PyObject *capsule = PyObject_GetAttrString(module, dot + 1);
    Py_DECREF(module);
    if (!capsule) return NULL;
    void *pointer = PyCapsule_GetPointer(capsule, name);
    Py_DECREF(capsule);
    return pointer;
}
