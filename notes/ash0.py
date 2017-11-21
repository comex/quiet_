import os, sys, struct, zlib

class BitReader:
    def __init__(self, buf):
        self.bits = []
        for byte in bytes(buf):
            for shift in range(7, -1, -1):
                self.bits.append((byte >> shift) & 1)
        self.off = 0
    def read(self, nbits):
        off = self.off
        assert off + nbits <= len(self.bits)
        self.off = off + nbits
        #print(self.bits[off:off+nbits])
        return sum(self.bits[off+i] << (nbits-1-i) for i in range(nbits))

class ASH0Reader(BitReader):
    def __init__(self, buf, base, skip_indirect, indirect_bits):
        super().__init__(buf)
        self.base = base
        self.tmp = []
        if skip_indirect:
            self.indirect_start = self.read(32)
        else:
            self.indirect_start = self.read_indirect(indirect_bits)

    def read_indirect(self, nbits):
        #print(f'read_indirect({nbits})')
        if self.read(1) == 0:
            ret = self.read(nbits)
        else:
            idx = len(self.tmp)
            self.tmp.append(None)
            self.tmp[idx] = (self.read_indirect(nbits), self.read_indirect(nbits))
            ret = self.base + idx
        #print(f'ri -> {ret}')
        return ret

    def get_from_indirect(self):
        idx = self.indirect_start
        while idx >= self.base:
            which = self.read(1)
            #print(f'bit:{which} idx:{idx:x}')
            idx = self.tmp[idx - self.base][which]
        return idx

def decompress_ash0(data):
    assert data[0:4] == b'ASH0'
    out = bytearray()
    uncompressed_size, o1off_and_bits = struct.unpack('>II', data[4:0xc])
    o1off = o1off_and_bits & ~3
    o2 = ASH0Reader(data[0xc:o1off], 0x200, bool(o1off_and_bits & 2), 9)
    o1 = ASH0Reader(data[o1off:], 0x800, bool(o1off_and_bits & 1), 11)
    while len(out) < uncompressed_size:
        o2idx = o2.get_from_indirect()
        if o2idx < 0x100:
            out.append(o2idx)
            #print(f'OUTl: {out[-1]:02x}')
        else:
            o1idx = o1.get_from_indirect()
            copy_off = -o1idx - 1
            copy_length = o2idx - 0xfd
            #print(f'copy {copy_off} length={copy_length}')
            for i in range(copy_length):
                assert -len(out) <= copy_off <= -1
                out.append(out[copy_off])
                #print(f'OUT: {out[-1]:02x}')
    compressed_length = o1off + (o1.off + 7) // 8
    return out, compressed_length

def go():
    in_path, out_path = sys.argv[1:]
    input = open(in_path, 'rb').read()
    input_off = 0
    content_idx = 0
    while input_off < len(input):
        decompressed, compressed_length = decompress_ash0(input[input_off:])
        input_off += (compressed_length + 3) & ~3

        #sys.stdout.buffer.write(decompressed)
        calced_crc = zlib.crc32(decompressed[4:])
        stored_crc, stored_length = struct.unpack('>II', decompressed[:8])
        import binascii
        print(f'#{content_idx}: length={len(decompressed):x} crc={calced_crc:x} stored_length={stored_length:x} stored_crc={stored_crc:x}')
        if stored_crc == calced_crc:
            print('  got CRC-wrapped')
            data = decompressed[8:8+stored_length]
            rest = data[8+stored_length:]
            assert rest == b'\0' * len(rest)
            filename = f'{content_idx}.unwrap'
        else:
            print('  got raw')
            data = decompressed
            filename = f'{content_idx}.raw'
        open(os.path.join(out_path, filename), 'wb').write(data)
        content_idx += 1

go()
