import token


_OPERATORS = sorted(token.EXACT_TOKEN_TYPES, key=len, reverse=True)


def _name_start(character):
    return character == "_" or character.isalpha() or ord(character) >= 128


def _name_part(character):
    return _name_start(character) or character.isdigit()


class TokenizerIter:
    def __init__(self, readline, encoding=None, extra_tokens=False):
        if not callable(readline):
            raise TypeError("'readline' must be callable")
        self._tokens = []
        self._index = 0
        self._extra = extra_tokens
        self._encoding = encoding
        self._scan(readline)

    def _emit(self, kind, text, line_number, start, end, line):
        self._tokens.append((kind, text, (line_number, start), (line_number, end), line))

    def _scan(self, readline):
        indents = [0]
        line_number = 0
        bracket_depth = 0
        while True:
            line = readline()
            if not line:
                break
            if isinstance(line, bytes):
                line = line.decode(self._encoding or "utf-8")
            line_number += 1
            length = len(line)
            position = 0
            if bracket_depth == 0:
                while position < length and line[position] in " \t\f":
                    position += 1
                if position < length and line[position] not in "#\r\n":
                    width = len(line[:position].expandtabs(8))
                    if width > indents[-1]:
                        indents.append(width)
                        self._emit(token.INDENT, line[:position], line_number, 0, position, line)
                    else:
                        while width < indents[-1]:
                            indents.pop()
                            self._emit(token.DEDENT, "", line_number, position, position, line)
                        if width != indents[-1]:
                            raise IndentationError("unindent does not match any outer indentation level")
            while position < length:
                character = line[position]
                if character in " \t\f":
                    position += 1
                    continue
                if character == "#":
                    end = position
                    while end < length and line[end] not in "\r\n":
                        end += 1
                    if self._extra:
                        self._emit(token.COMMENT, line[position:end], line_number, position, end, line)
                    position = end
                    continue
                if character in "\r\n":
                    end = position + 1
                    if character == "\r" and end < length and line[end] == "\n":
                        end += 1
                    kind = token.NL if bracket_depth else token.NEWLINE
                    self._emit(kind, line[position:end], line_number, position, end, line)
                    position = end
                    continue
                if _name_start(character):
                    end = position + 1
                    while end < length and _name_part(line[end]):
                        end += 1
                    self._emit(token.NAME, line[position:end], line_number, position, end, line)
                    position = end
                    continue
                if character.isdigit() or character == "." and position + 1 < length and line[position + 1].isdigit():
                    end = position + 1
                    while end < length and (line[end].isalnum() or line[end] in "._+-" and (line[end] not in "+-" or line[end - 1] in "eEjJ")):
                        end += 1
                    self._emit(token.NUMBER, line[position:end], line_number, position, end, line)
                    position = end
                    continue
                if character in "'\"":
                    quote = character
                    triple = line[position:position + 3] == quote * 3
                    end = position + (3 if triple else 1)
                    delimiter = quote * (3 if triple else 1)
                    while end < length:
                        if line.startswith(delimiter, end):
                            end += len(delimiter)
                            break
                        if line[end] == "\\":
                            end += 2
                        else:
                            end += 1
                    self._emit(token.STRING, line[position:end], line_number, position, end, line)
                    position = end
                    continue
                operator = next((value for value in _OPERATORS if line.startswith(value, position)), None)
                if operator is not None:
                    kind = token.OP if self._extra else token.EXACT_TOKEN_TYPES[operator]
                    self._emit(kind, operator, line_number, position, position + len(operator), line)
                    if operator in "([{":
                        bracket_depth += 1
                    elif operator in ")]}" and bracket_depth:
                        bracket_depth -= 1
                    position += len(operator)
                    continue
                self._emit(token.ERRORTOKEN, character, line_number, position, position + 1, line)
                position += 1
        while len(indents) > 1:
            indents.pop()
            self._emit(token.DEDENT, "", line_number + 1, 0, 0, "")
        self._emit(token.ENDMARKER, "", line_number + 1, 0, 0, "")

    def __iter__(self):
        return self

    def __next__(self):
        if self._index >= len(self._tokens):
            raise StopIteration
        result = self._tokens[self._index]
        self._index += 1
        return result
