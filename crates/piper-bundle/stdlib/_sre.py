"""Regular-expression execution engine for Piper."""

MAGIC = 20230612
CODESIZE = 4
MAXREPEAT = (1 << 32) - 1
MAXGROUPS = (1 << 31) - 1


def getcodesize():
    return CODESIZE


def ascii_iscased(value):
    return 65 <= value <= 90 or 97 <= value <= 122


def ascii_tolower(value):
    if 65 <= value <= 90:
        return value + 32
    return value


def unicode_iscased(value):
    character = chr(value)
    return character.lower() != character.upper()


def unicode_tolower(value):
    lowered = chr(value).lower()
    if len(lowered) == 1:
        return ord(lowered)
    return value


def _character(string, position):
    value = string[position]
    if isinstance(value, int):
        return value
    return ord(value)


def _same(left, right, flags):
    if flags & 2:
        if flags & 256:
            return ascii_tolower(left) == ascii_tolower(right)
        return chr(left).casefold() == chr(right).casefold()
    return left == right


def _category(name, character, flags):
    value = chr(character)
    if name.endswith("DIGIT"):
        result = value.isdecimal() if "UNI" in name else 48 <= character <= 57
    elif name.endswith("SPACE"):
        result = value.isspace() if "UNI" in name else value in " \t\n\r\f\v"
    elif name.endswith("WORD"):
        result = (value.isalnum() or value == "_") if "UNI" in name else (character < 128 and (value.isalnum() or value == "_"))
    elif name.endswith("LINEBREAK"):
        result = value in "\n\r\v\f\x1c\x1d\x1e\x85\u2028\u2029" if "UNI" in name else value == "\n"
    else:
        result = False
    if "NOT_" in name:
        return not result
    return result


def _at(name, string, position, start, end, flags):
    if name in ("AT_BEGINNING", "AT_BEGINNING_STRING"):
        return position == start
    if name == "AT_BEGINNING_LINE":
        return position == start or position > start and _character(string, position - 1) == 10
    if name == "AT_END_STRING":
        return position == end
    if name in ("AT_END", "AT_END_LINE"):
        return position == end or position + 1 == end and _character(string, position) == 10
    if "BOUNDARY" in name:
        left = position > start and _category("CATEGORY_UNI_WORD", _character(string, position - 1), flags)
        right = position < end and _category("CATEGORY_UNI_WORD", _character(string, position), flags)
        boundary = left != right
        return not boundary if "NON_BOUNDARY" in name else boundary
    return False


def _in(items, character, flags):
    negate = False
    matched = False
    for operation, argument in items:
        name = operation.name
        if name == "NEGATE":
            negate = True
        elif name == "LITERAL" and _same(character, argument, flags):
            matched = True
        elif name == "RANGE" and argument[0] <= character <= argument[1]:
            matched = True
        elif name == "CATEGORY" and _category(argument.name, character, flags):
            matched = True
    return not matched if negate else matched


def _sequence(nodes, index, string, position, start, end, flags, groups):
    if index >= len(nodes):
        yield position, groups
        return
    operation, argument = nodes[index]
    name = operation.name
    if name == "LITERAL":
        if position < end and _same(_character(string, position), argument, flags):
            yield from _sequence(nodes, index + 1, string, position + 1, start, end, flags, groups)
    elif name == "NOT_LITERAL":
        if position < end and not _same(_character(string, position), argument, flags):
            yield from _sequence(nodes, index + 1, string, position + 1, start, end, flags, groups)
    elif name == "ANY":
        if position < end and ((flags & 16) or _character(string, position) != 10):
            yield from _sequence(nodes, index + 1, string, position + 1, start, end, flags, groups)
    elif name == "IN":
        if position < end and _in(argument, _character(string, position), flags):
            yield from _sequence(nodes, index + 1, string, position + 1, start, end, flags, groups)
    elif name == "CATEGORY":
        if position < end and _category(argument.name, _character(string, position), flags):
            yield from _sequence(nodes, index + 1, string, position + 1, start, end, flags, groups)
    elif name == "AT":
        if _at(argument.name, string, position, start, end, flags):
            yield from _sequence(nodes, index + 1, string, position, start, end, flags, groups)
    elif name == "BRANCH":
        for branch in argument[1]:
            for branch_end, branch_groups in _sequence(branch, 0, string, position, start, end, flags, groups.copy()):
                yield from _sequence(nodes, index + 1, string, branch_end, start, end, flags, branch_groups)
    elif name == "SUBPATTERN":
        group, add_flags, del_flags, body = argument
        nested_flags = (flags | add_flags) & ~del_flags
        for nested_end, nested_groups in _sequence(body, 0, string, position, start, end, nested_flags, groups.copy()):
            if group:
                nested_groups[group] = (position, nested_end)
            yield from _sequence(nodes, index + 1, string, nested_end, start, end, flags, nested_groups)
    elif name in ("MAX_REPEAT", "MIN_REPEAT", "POSSESSIVE_REPEAT"):
        minimum, maximum, body = argument
        levels = [[(position, groups.copy())]]
        count = 0
        while count < maximum:
            next_states = []
            for repeat_start, repeat_groups in levels[-1]:
                for repeat_end, next_groups in _sequence(body, 0, string, repeat_start, start, end, flags, repeat_groups.copy()):
                    if repeat_end != repeat_start:
                        next_states.append((repeat_end, next_groups))
            if not next_states:
                break
            levels.append(next_states)
            count += 1
        eligible = levels[minimum:]
        if name != "MIN_REPEAT":
            eligible = list(reversed(eligible))
        if name == "POSSESSIVE_REPEAT" and eligible:
            eligible = eligible[:1]
        for level in eligible:
            for repeated, repeat_groups in level:
                for result in _sequence(nodes, index + 1, string, repeated, start, end, flags, repeat_groups):
                    yield result
    elif name == "GROUPREF":
        span = groups.get(argument)
        if span is not None:
            value = string[span[0]:span[1]]
            stop = position + len(value)
            if stop <= end and string[position:stop] == value:
                yield from _sequence(nodes, index + 1, string, stop, start, end, flags, groups)
    elif name == "GROUPREF_EXISTS":
        group, yes, no = argument
        branch = yes if group in groups else no
        for branch_end, branch_groups in _sequence(branch or [], 0, string, position, start, end, flags, groups.copy()):
            yield from _sequence(nodes, index + 1, string, branch_end, start, end, flags, branch_groups)
    elif name in ("ASSERT", "ASSERT_NOT"):
        direction, body = argument
        probe = position if direction >= 0 else position + direction
        success = False
        if probe >= start:
            for assertion_end, unused in _sequence(body, 0, string, probe, start, end, flags, groups.copy()):
                if direction >= 0 or assertion_end == position:
                    success = True
                    break
        if success == (name == "ASSERT"):
            yield from _sequence(nodes, index + 1, string, position, start, end, flags, groups)
    elif name == "ATOMIC_GROUP":
        for atomic_end, atomic_groups in _sequence(argument, 0, string, position, start, end, flags, groups.copy()):
            yield from _sequence(nodes, index + 1, string, atomic_end, start, end, flags, atomic_groups)
            break
    elif name == "SUCCESS":
        yield position, groups


class Match:
    def __init__(self, pattern, string, start, end, groups, pos, endpos):
        self.re = pattern
        self.string = string
        self.pos = pos
        self.endpos = endpos
        self.lastindex = max(groups) if groups else None
        self.lastgroup = pattern._indexgroup[self.lastindex] if self.lastindex is not None and self.lastindex < len(pattern._indexgroup) else None
        self._spans = {0: (start, end)}
        self._spans.update(groups)

    def _index(self, group):
        if isinstance(group, str):
            return self.re.groupindex[group]
        return group

    def group(self, *groups):
        if not groups:
            groups = (0,)
        values = []
        for group in groups:
            span = self._spans.get(self._index(group))
            values.append(None if span is None else self.string[span[0]:span[1]])
        if len(values) == 1:
            return values[0]
        return tuple(values)

    def groups(self, default=None):
        return tuple(self.group(index) if index in self._spans else default for index in range(1, self.re.groups + 1))

    def groupdict(self, default=None):
        return {name: self.group(index) if index in self._spans else default for name, index in self.re.groupindex.items()}

    def start(self, group=0):
        span = self._spans.get(self._index(group))
        return -1 if span is None else span[0]

    def end(self, group=0):
        span = self._spans.get(self._index(group))
        return -1 if span is None else span[1]

    def span(self, group=0):
        return self.start(group), self.end(group)

    def __getitem__(self, group):
        return self.group(group)

    def expand(self, template):
        result = template
        for index in range(1, self.re.groups + 1):
            result = result.replace("\\" + str(index), self.group(index) or "")
        for name in self.re.groupindex:
            result = result.replace("\\g<" + name + ">", self.group(name) or "")
        return result

    def __bool__(self):
        return True


class _Scanner:
    def __init__(self, pattern, string, pos, endpos):
        self.pattern = pattern
        self.string = string
        self.position = pos
        self.endpos = endpos

    def match(self):
        result = self.pattern.match(self.string, self.position, self.endpos)
        if result is not None:
            self.position = result.end()
        return result

    def search(self):
        result = self.pattern.search(self.string, self.position, self.endpos)
        if result is not None:
            self.position = result.end()
        return result


class Pattern:
    def __init__(self, pattern, flags, tree, groups, groupindex, indexgroup):
        self.pattern = pattern
        self.flags = flags
        self.groups = groups
        self.groupindex = groupindex
        self._indexgroup = indexgroup
        self._tree = tree

    def _match_at(self, string, position, start, end):
        for match_end, groups in _sequence(self._tree, 0, string, position, start, end, self.flags, {}):
            return Match(self, string, position, match_end, groups, start, end)
        return None

    def match(self, string, pos=0, endpos=None):
        if endpos is None:
            endpos = len(string)
        return self._match_at(string, pos, pos, endpos)

    def fullmatch(self, string, pos=0, endpos=None):
        if endpos is None:
            endpos = len(string)
        result = self._match_at(string, pos, pos, endpos)
        if result is not None and result.end() == endpos:
            return result
        return None

    def search(self, string, pos=0, endpos=None):
        if endpos is None:
            endpos = len(string)
        position = pos
        while position <= endpos:
            result = self._match_at(string, position, pos, endpos)
            if result is not None:
                return result
            position += 1
        return None

    def finditer(self, string, pos=0, endpos=None):
        if endpos is None:
            endpos = len(string)
        matches = []
        position = pos
        while position <= endpos:
            result = self.search(string, position, endpos)
            if result is None:
                break
            matches.append(result)
            position = result.end()
            if result.end() == result.start():
                position += 1
        return iter(matches)

    def findall(self, string, pos=0, endpos=None):
        results = []
        for result in self.finditer(string, pos, endpos):
            if self.groups == 0:
                results.append(result.group())
            elif self.groups == 1:
                results.append(result.group(1) or string[0:0])
            else:
                results.append(tuple(value or string[0:0] for value in result.groups()))
        return results

    def split(self, string, maxsplit=0):
        result = []
        position = 0
        count = 0
        for match in self.finditer(string):
            if maxsplit and count >= maxsplit:
                break
            result.append(string[position:match.start()])
            result.extend(match.groups())
            position = match.end()
            count += 1
        result.append(string[position:])
        return result

    def subn(self, replacement, string, count=0):
        result = []
        position = 0
        replaced = 0
        for match in self.finditer(string):
            if count and replaced >= count:
                break
            result.append(string[position:match.start()])
            value = replacement(match) if callable(replacement) else match.expand(replacement)
            result.append(value)
            position = match.end()
            replaced += 1
        result.append(string[position:])
        return string[0:0].join(result), replaced

    def sub(self, replacement, string, count=0):
        return self.subn(replacement, string, count)[0]

    def scanner(self, string, pos=0, endpos=None):
        if endpos is None:
            endpos = len(string)
        return _Scanner(self, string, pos, endpos)

    def __repr__(self):
        return "re.compile(%r)" % self.pattern


def compile(pattern, flags, code, groups, groupindex, indexgroup):
    return Pattern(pattern, flags, code, groups, groupindex, indexgroup)
