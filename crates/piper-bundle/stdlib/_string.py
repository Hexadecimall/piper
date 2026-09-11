def formatter_field_name_split(field_name):
    position = 0
    while position < len(field_name) and field_name[position] not in ".[":
        position += 1
    first_text = field_name[:position]
    first = int(first_text) if first_text.isdigit() else first_text
    rest = []
    while position < len(field_name):
        if field_name[position] == ".":
            start = position + 1
            position = start
            while position < len(field_name) and field_name[position] not in ".[":
                position += 1
            if position == start:
                raise ValueError("Empty attribute in format string")
            rest.append((True, field_name[start:position]))
        else:
            end = field_name.find("]", position + 1)
            if end < 0:
                raise ValueError("Missing ']' in format string")
            key = field_name[position + 1:end]
            rest.append((False, int(key) if key.isdigit() else key))
            position = end + 1
    return first, iter(rest)


def formatter_parser(format_string):
    result = []
    literal = ""
    position = 0
    while position < len(format_string):
        character = format_string[position]
        if character == "{" and position + 1 < len(format_string) and format_string[position + 1] == "{":
            literal += "{"
            position += 2
            continue
        if character == "}" and position + 1 < len(format_string) and format_string[position + 1] == "}":
            literal += "}"
            position += 2
            continue
        if character == "}":
            raise ValueError("Single '}' encountered in format string")
        if character != "{":
            literal += character
            position += 1
            continue
        end = position + 1
        depth = 0
        while end < len(format_string):
            if format_string[end] == "{" :
                depth += 1
            elif format_string[end] == "}":
                if depth == 0:
                    break
                depth -= 1
            end += 1
        if end == len(format_string):
            raise ValueError("expected '}' before end of string")
        field = format_string[position + 1:end]
        conversion = None
        spec = ""
        split = len(field)
        for index, value in enumerate(field):
            if value in "!:":
                split = index
                break
        name = field[:split]
        if split < len(field) and field[split] == "!":
            if split + 1 >= len(field):
                raise ValueError("end of string while looking for conversion specifier")
            conversion = field[split + 1]
            split += 2
        if split < len(field):
            if field[split] != ":":
                raise ValueError("expected ':' after conversion specifier")
            spec = field[split + 1:]
        result.append((literal, name, spec, conversion))
        literal = ""
        position = end + 1
    if literal or not result:
        result.append((literal, None, None, None))
    return iter(result)
