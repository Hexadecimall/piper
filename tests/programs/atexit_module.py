import atexit


def announce(label, suffix=""):
    print(label + suffix)


def removed():
    print("not reached")


atexit.register(announce, "first")
atexit.register(removed)
atexit.unregister(removed)
atexit.register(announce, "second", suffix="!")
print("callbacks", atexit._ncallbacks())
