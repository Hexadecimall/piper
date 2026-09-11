print("before")
def go():
    raise KeyError("boom")
go()
print("after")
