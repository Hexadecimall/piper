def risky(n):
    if n == 0:
        raise ValueError("zero")
    return 10 // n
for n in (2, 0):
    try:
        print(risky(n))
    except ValueError as e:
        print("caught", e, type(e).__name__)
    else:
        print("no error")
    finally:
        print("finally")
try:
    {}["k"]
except (KeyError, IndexError) as e:
    print("key", repr(e))
class MyErr(Exception):
    pass
try:
    try:
        raise MyErr("inner")
    except MyErr:
        raise RuntimeError("outer")
except RuntimeError as e:
    print(e, type(e.__context__).__name__)
def f():
    try:
        return "body"
    finally:
        print("cleanup")
print(f())
with open(__file__) as fh:
    print(fh.readline().strip())
import sys
print(sys.argv[1:], "hello".upper(), "-".join(["a", "b"]), "x,y".split(","))
try:
    raise ValueError("uncaught")
except Exception as e:
    print("handled", e)
