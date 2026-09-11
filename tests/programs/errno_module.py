import errno

print(errno.ENOENT > 0)
print(errno.errorcode[errno.ENOENT] == "ENOENT")
print(errno.EINVAL > 0)
