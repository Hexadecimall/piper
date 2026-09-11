import time

start = time.monotonic()
time.sleep(0.001)
end = time.monotonic()
print(end >= start)
print(time.time() > 0)
print(time.time_ns() > 0)
print(time.process_time() >= 0)
