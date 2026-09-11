events = []
try:
    raise ExceptionGroup("problems", [ValueError("a"), TypeError("b"), ValueError("c")])
except* ValueError as group:
    events.append((type(group).__name__, len(group.exceptions)))
except* TypeError as group:
    events.append((type(group).__name__, len(group.exceptions)))

print(events)

try:
    raise ValueError("plain")
except* ValueError as group:
    print(type(group).__name__, len(group.exceptions), str(group.exceptions[0]))

try:
    try:
        raise ExceptionGroup("mixed", [ValueError("handled"), TypeError("left")])
    except* ValueError:
        print("handled one")
except ExceptionGroup as remaining:
    print("remaining", type(remaining.exceptions[0]).__name__)
