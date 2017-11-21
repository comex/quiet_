import lz4.block
import sys, struct

def decompress_block(fp):
    header = fp.read(8)
    if len(header) == 0:
        return None
    assert len(header) == 8
    clen, dlen = struct.unpack('>II', header)
    compressed = fp.read(clen)
    assert len(compressed) == clen
    return lz4.block.decompress(compressed, dlen)

def fake_decompress_block(fp):
    header = fp.read(8)
    if len(header) == 0:
        return None
    assert len(header) == 8
    clen, dlen = struct.unpack('>II', header)
    fp.seek(clen, 1)
    return dlen

if __name__ == '__main__':
    assert len(sys.argv) == 1
    while True:
        block = decompress_block(sys.stdin)
        if block is None:
            break
        sys.stdout.write(block)
