"""Thread primitives used by the bundled library."""

TIMEOUT_MAX = 9223372036.0
error = RuntimeError

class _local:
    pass

def get_ident():
    return 1

def get_native_id():
    return 1

class LockType:
    def __init__(self):
        self._locked = False

    def acquire(self, blocking=True, timeout=-1):
        if self._locked and not blocking:
            return False
        self._locked = True
        return True

    def release(self):
        if not self._locked:
            raise RuntimeError("release unlocked lock")
        self._locked = False

    def locked(self):
        return self._locked

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.release()

class RLock:
    def __init__(self):
        self._depth = 0

    def acquire(self, blocking=True, timeout=-1):
        self._depth += 1
        return True

    def release(self):
        if self._depth == 0:
            raise RuntimeError("cannot release un-acquired lock")
        self._depth -= 1

    def _is_owned(self):
        return self._depth != 0

    def _release_save(self):
        depth = self._depth
        self._depth = 0
        return depth

    def _acquire_restore(self, depth):
        self._depth = depth

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.release()

class _ThreadHandle:
    def __init__(self, ident=None):
        self.ident = get_ident() if ident is None else ident
        self._done = False

    def is_done(self):
        return self._done

    def join(self, timeout=None):
        return None

    def _set_done(self):
        self._done = True

def _make_thread_handle(ident):
    return _ThreadHandle(ident)

def _get_main_thread_ident():
    return 1

def _is_main_interpreter():
    return True

def daemon_threads_allowed():
    return True

def set_name(name):
    return None

def _shutdown():
    return None

def allocate_lock():
    return LockType()

allocate = allocate_lock

def stack_size(size=None):
    return 0

def interrupt_main(signum=2):
    raise KeyboardInterrupt

def exit():
    raise SystemExit

exit_thread = exit

def start_new_thread(function, args, kwargs=None):
    raise RuntimeError("native thread creation is not available")

def start_joinable_thread(function, args=(), kwargs=None, handle=None, daemon=True):
    raise RuntimeError("native thread creation is not available")

start_new = start_new_thread
