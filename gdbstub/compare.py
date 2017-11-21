import sys, struct
import numpy as np
from decompress import decompress_block, fake_decompress_block
from cStringIO import StringIO
args = sys.argv[1:]
assert len(args) >= 2
dtype, expr = args[-2:]
dtype = np.dtype('>' + dtype)

files = []
COMPRESSED = 1
def load():
    for filename in args[:-2]:
        chunks = []
        fp = open(filename, 'rb')

        if COMPRESSED:
            while True:
                header = decompress_block(fp)
                if header == b'OKOK':
                    break
                addr, size = struct.unpack('>II', header)
                i = 0
                while i < size:
                    offset = fp.tell()
                    dlen = fake_decompress_block(fp)
                    def mmff(fp, offset):
                        def mmf():
                            fp.seek(offset)
                            data = decompress_block(fp)
                            return np.frombuffer(data, dtype, len(data) / dtype.itemsize) 
                        return mmf
                    chunks.append((addr + i, dlen, mmff(fp, offset)))
                    i += dlen
                assert i == size
        else:
            while True:
                x = fp.read(4)
                if x == b'OKOK':
                    break
                fp.seek(-4, 1)
                addr, size = struct.unpack('>II', fp.read(8))
                offset = fp.tell()
                mm = np.memmap(fp, dtype, 'r', offset, (size / dtype.itemsize,))
                chunks.append((addr, size, lambda: mm))
                fp.seek(offset + size)
        print 'done'
        files.append(chunks)
def is_sorted(xs):
    return np.all(xs[:-1] <= xs[1:], axis=0)
def is_strictly_sorted(xs):
    return np.all(xs[:-1] < xs[1:], axis=0)
a = np.array
land = np.logical_and
lor = np.logical_or
def test(expr):
    for nth_chunks in zip(*files):
        info = set((addr, size) for addr, size, mmf in nth_chunks)
        assert len(info) == 1
        addr, size = info.pop()
        def i2a(index):
            assert 0 <= index < size
            return addr + index * dtype.itemsize
        f = [mmf() for addr, size, mmf in nth_chunks]
        truths = eval(expr)
        assert len(truths) == len(f[0])
        indices = np.argwhere(truths)
        del truths
        for index, in indices:
            print '0x%x' % i2a(index), ', '.join(str(mm[index]) for mm in f)
load()
test(expr)

