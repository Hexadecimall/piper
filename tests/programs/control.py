total = 0
for i in range(10):
    if i % 2 == 0:
        continue
    if i > 7:
        break
    total += i
print(total)
n = 0
while n < 3:
    n += 1
else:
    print("done", n)
for c in "ab":
    print(c)
a, b = 1, 2
a, b = b, a
print(a, b)
first, *rest = [1, 2, 3, 4]
print(first, rest)
print([x * x for x in range(5) if x != 2], {k: k * 2 for k in (1, 2)}, sorted({3, 1, 2}))
