from string.templatelib import Interpolation, Template, convert

width = 4
value = 12
template = t"value={value!r:{width}}"
print(type(template) is Template)
print(template.strings)
print(template.values)
print(len(template.interpolations))
part = template.interpolations[0]
print(type(part) is Interpolation)
print(part.value, part.expression, part.conversion, part.format_spec)
print(list(template))
print(convert(value, "r"))
