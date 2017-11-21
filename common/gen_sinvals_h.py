import math, sys

out = 'static const int8_t sinvals[] = {\n'
for ibase in range(0, 64, 8):
    out += '    '
    for i in range(ibase, ibase + 8):
        val = int(round(127 * math.sin((i / 255.0) * (2 * math.pi))))
        out += '%d, ' % (val,)
    out += '\n'
out += '};\n'
open(sys.argv[1], 'w').write(out)
