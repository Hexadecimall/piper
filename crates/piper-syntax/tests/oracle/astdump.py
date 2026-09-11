"""Print ast.dump(..., include_attributes=True) for a file, or ERROR."""
import ast, sys
with open(sys.argv[1], "rb") as f:
    src = f.read()
try:
    print(ast.dump(ast.parse(src, sys.argv[1]), include_attributes=True))
except SyntaxError as e:
    print(f"ERROR {e.lineno}")
