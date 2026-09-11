"""Print CPython's token stream for a file, one token per line, for diffing."""
import sys, tokenize, token

with open(sys.argv[1], "rb") as f:
    try:
        for t in tokenize.tokenize(f.readline):
            if t.type == token.ENCODING:
                continue
            print(f"{t.start[0]},{t.start[1]}-{t.end[0]},{t.end[1]} {token.tok_name[t.type]} {t.string!r}")
    except (tokenize.TokenError, SyntaxError, IndentationError) as e:
        print(f"ERROR {e}")
