class Animal:
    kind = "animal"
    def __init__(self, name):
        self.name = name
    def speak(self):
        return f"{self.name} makes a sound"
    def __repr__(self):
        return f"Animal({self.name!r})"
class Dog(Animal):
    def speak(self):
        return super().speak() + " (woof)"
    @property
    def loud(self):
        return self.name.upper()
    @staticmethod
    def create(n):
        return Dog(n)
    @classmethod
    def kind_of(cls):
        return cls.kind
d = Dog.create("rex")
print(d, d.speak(), d.loud, Dog.kind_of(), isinstance(d, Animal), type(d).__name__)
class Vec:
    def __init__(self, x, y): self.x, self.y = x, y
    def __add__(self, o): return Vec(self.x + o.x, self.y + o.y)
    def __eq__(self, o): return (self.x, self.y) == (o.x, o.y)
    def __len__(self): return 2
    def __getitem__(self, i): return (self.x, self.y)[i]
    def __str__(self): return f"<{self.x}, {self.y}>"
v = Vec(1, 2) + Vec(3, 4)
print(v, v == Vec(4, 6), len(v), v[1], list(v))
def enclosing(value):
    class Inner:
        value = "class"
        def read(self):
            return value
    return Inner
print(enclosing("closure")().read(), enclosing("closure").value)
