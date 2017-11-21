import sys, glob, os, re
from PIL import Image

in_path, out_path = sys.argv[1], sys.argv[2]
pngs = [png for png in os.listdir(in_path) if png.lower().endswith('.png')]

used_cps = {ord('\n')}
font_data_count = 0
font_data = ''
mapping_info = {}

custom_names = []

# put space first
pngs.sort(key=lambda png: (0,) if png[:-4] == '20_ '
                     else (1, png[:-4]) if len(png) == 5
                     else (2, png[:-4]))

for png in pngs:
    name = png[:-4]
    if name.startswith('CHAR_'):
        c_name = name
        c_val = len(custom_names) + 1
        custom_names.append(c_name)
    else:
        char = re.match('[0-9a-f][0-9a-f]_(.)', name, re.I).group(1)
        c_val = ord(char)
        used_cps.add(ord(char))
        if char == "'":
            c_name = r"'\''"
        else:
            c_name = "'%c'" % char

    im = Image.open(os.path.join(in_path, png))
    bytes = []
    for y in range(16):
        bits = 0
        for x in range(im.width):
            pixel = im.getpixel((x, y))
            if pixel == 255 or pixel == (255, 255, 255):
                assert x <= 7
                bits |= 1 << (7-x)
            elif pixel != 0 and pixel != (0, 0, 0):
                raise ValueError('unexpected pixel value %r' % (pixel,))
        bytes.append(bits)
    font_data += '    {%s, { /* %s */\n' % (im.width, c_name)
    for byte in bytes:
        font_data += '        0b%s,\n' % (bin(byte)[2:].rjust(8, '0'),)
    font_data += '    }},\n'
    mapping_info[c_val] = (c_name, font_data_count)
    font_data_count += 1

mapping = ''
for i in range(max(mapping_info.keys()) + 1):
    mi = mapping_info.get(i)
    if mi is not None:
        c_name, idx = mi
        mapping += '    %d, /* [%u] = %s */\n' % (idx, i, c_name)
    else:
        mapping += '    0, /* [%u] */\n' % (i,)


custom_name_cp = []
cp = 1
for c_name in custom_names:
    while cp in used_cps:
        cp += 1
    custom_name_cp.append((c_name, cp))
    cp += 1


out = ''
out += '#pragma once\n'
out += '#define FONT_WIDTH 8\n'
out += '#define FONT_HEIGHT 16\n'
out += 'enum {\n'
for name, cp in custom_name_cp:
    out += '    %s = %d,\n' % (name, cp)
out += '};\n'
out += '\n'
for name, cp in custom_name_cp:
    out += '#define %s "\\x%02x"\n' % (name.replace('CHAR_', 'STR_'), cp)
out += '\n'
out += 'struct font_data {\n'
out += '    unsigned char width;\n'
out += '    unsigned char pixel_data[16];\n'
out += '};\n'
out += 'static const struct font_data font_data[] = {\n'
out += font_data
out += '};\n'
out += 'static const unsigned char font_mapping[] = {\n'
out += mapping
out += '};\n'
open(out_path, 'w').write(out)
