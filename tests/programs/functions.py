def add(a, b=2, *args, c=3, **kw):
    return a + b + c + sum(args) + len(kw)
print(add(1), add(1, 5), add(1, 2, 3, 4), add(1, c=10, z=0))
def make_counter():
    count = 0
    def inc():
        nonlocal count
        count += 1
        return count
    return inc
ctr = make_counter()
ctr(); ctr()
print(ctr())
sq = lambda v: v * v
print(sq(7), list(map(sq, [1, 2, 3])))
def fact(n):
    return 1 if n <= 1 else n * fact(n - 1)
print(fact(20), fact(30))
def outer():
    x = "outer"
    def mid():
        def inner():
            return x
        return inner()
    return mid()
print(outer())
print(f"{add(1)=} {3.5:.2f} {'hi':>5}|{42:04d} {255:#x}")
def default_scope(transform=lambda value: value + 1):
    return transform(8)
print(default_scope())
