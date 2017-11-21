#!/usr/bin/env python3

from guest_access import *
import socket, struct, sys
from threading import Lock
from binascii import hexlify
import time

class RPLSegment:
    def __init__(self, data):
        self.addr, self.slide, self.size = struct.unpack('>III', data)
    def __repr__(self):
        return 'RPLSegment(addr=%#x, slide=%#x, slide=%#x)' % (self.addr, self.slide, self.size)
class RPL:
    def __init__(self, data, guest):
        name_addr, = struct.unpack('>I', data[:4])
        self.name = GuestCString(guest, name_addr)
        self.text = RPLSegment(data[4:16])
        self.data = RPLSegment(data[16:28])
        self.rodata = RPLSegment(data[28:40])
    def __repr__(self):
        return 'RPL(name=%r, text=%r, data=%r, rodata=%r)' % (self.name, self.text, self.data, self.rodata)

class MemrwGuest(Guest):
    def __init__(self, host, port):
        self.sock = socket.socket()
        self.sock.connect((host, port))
        self.lock = Lock()
        self.verbose = False
        self.chunk_size = 0x800
        self.rpls = self.read_rpl_info()
        rpx = self.rpls[-2]
        assert rpx.name.as_str().endswith('.rpx')
        self.text_slide = rpx.text.slide
        self.data_slide = rpx.data.slide

    @classmethod
    def with_hostport(cls, hostport):
        if ':' not in hostport:
            raise ValueError('not in host:port format: %r' % (hostport,))
        host, port = hostport.rsplit(':', 1)
        port = int(port)
        return cls(host, port)
    def slide_text(self, addr):
        return (addr + self.text_slide) & 0xffffffff
    def unslide_text(self, addr):
        return (addr - self.text_slide) & 0xffffffff
    def slide_data(self, addr):
        return (addr + self.data_slide) & 0xffffffff
    def unslide_data(self, addr):
        return (addr - self.data_slide) & 0xffffffff

    def recvall(self, size):
        data = b''
        while len(data) < size:
            chunk = self.sock.recv(size)
            if chunk == '':
                raise Exception("recvall: got ''")
            data += chunk
            assert len(data) <= size
        return data
    def try_read(self, addr, size):
        data = b''
        while size:
            assert size >= 0
            chunk_size = min(size, self.chunk_size)
            chunk_data = self.try_read_chunk(addr, chunk_size)
            data += chunk_data
            addr += len(chunk_data)
            size -= len(chunk_data)
            if len(chunk_data) < chunk_size:
                break
        return data
    def try_write(self, addr, data):
        actual = 0
        while actual < len(data):
            chunk_size = min(len(data) - actual, self.chunk_size)
            chunk_actual = self.try_write_chunk(addr, data[actual:actual+chunk_size])
            addr += chunk_actual
            actual += chunk_actual
            if chunk_actual < chunk_size:
                break
        return actual

    def try_read_chunk(self, addr, size):
        assert size <= 0x100000
        with self.lock:
            if self.verbose:
                print('try_read(%#x, %#x)' % (addr, size), end='')
                sys.stdout.flush()
            self.sock.sendall(struct.pack('>4sIII', b'MEMQ', 0, addr, size))
            magic, actual = struct.unpack('>4sI', self.recvall(8))
            self.check_magic(magic)
            ret = self.recvall(actual)
            if self.verbose:
                print(' -> %s' % (hexlify(ret),))
            return ret
    def try_write_chunk(self, addr, data):
        with self.lock:
            if self.verbose:
                print('try_read(%#x, %s)' % (addr, hexlify(data)), end='')
            self.sock.sendall(struct.pack('>4sIII', b'MEMQ', 1, addr, len(data)) + data)
            magic, actual = struct.unpack('>4sI', self.recvall(8))
            self.check_magic(magic)
            if self.verbose:
                print(' -> %s' % (actual,))
            assert actual <= len(data)
            return actual
    def read_rpl_info(self):
        with self.lock:
            if self.verbose:
                print('read_rpl_info()', end='')
                sys.stdout.flush()
            self.sock.sendall(struct.pack('>4sIII', b'MEMQ', 2, 0, 0))
            magic, actual = struct.unpack('>4sI', self.recvall(8))
            self.check_magic(magic)
            ret = self.recvall(actual)
            assert actual % 40 == 0
        rpls = []
        for i in range(0, actual, 40):
            rpl = RPL(ret[i:i+40], self)
            if self.verbose:
                print(rpl)
            rpls.append(rpl)
        return rpls
    def check_magic(self, magic):
        if magic == b'OOM!':
            raise Exception('stub was out of memory')
        assert magic == b'MEMA'

class Point2D(GuestStruct):
    x = prop(0, f32)
    y = prop(4, f32)
    sizeof_star = 8
class Point3D(Point2D):
    z = prop(8, f32)
    sizeof_star = 12
class Size2D(GuestStruct):
    w = prop(0, f32)
    h = prop(4, f32)
    sizeof_star = 8

class TT2(GuestStruct):
    cb_direct = prop(0xc, u32)
    sizeof_star = 0x10

class TransThing(GuestStruct):
    tt2_cur = fixed_array(ptr_to(TT2), 10)
    tt2_avail = fixed_array(ptr_to(TT2), 47)
    sizeof_star = 0xe4

class State(GuestStruct):
    name = prop(0, ptr_to(GuestCString))
    #name_ptr = prop(0, u32)
    not_name_len = prop(8, u32)
    #@property
    #def name(self):
    #    name = self.guest.read(self.name_ptr, self.name_len)
    #    name = name[:name.index(b'\0')]
    #    return name
    sizeof_star = 0x24
    def dump(self, fp, indent):
        fp.write('state(name=%r)' % (self.name,))

class GuestMethodPtr(GuestStruct):
    offset_to_this = prop(0, u16)
    vtable_idx = prop(2, u16)
    callback_or_offset_to_vt = prop(4, u32)
    sizeof_star = 8
    def target_for(self, obj):
        vtable_idx = self.vtable_idx
        this = obj.raw_offset(self.offset_to_this, GuestPtr)
        if vtable_idx >= 0x8000:
            return this, self.callback_or_offset_to_vt
        else:
            vtable = this.raw_offset(self.callback_or_offset_to_vt, GuestPtrPtr).get()
            callback = vtable.raw_offset(8 * vtable_idx + 4, GuestU32Ptr).get()
            return this, callback

class StateMgr(GuestStruct):
    counter = prop(8, u32)
    state = prop(0xc, u32)
    oldstate = prop(0x10, u32)
    unk_count = prop(0x18, u32)
    state_list = prop(0x20, count_ptr(State))
    target_obj = prop(0x28, GuestPtrPtr)
    cb1s = prop(0x2c, count_ptr(GuestMethodPtr))
    cb2s = prop(0x34, count_ptr(GuestMethodPtr))
    cb3s = prop(0x3c, count_ptr(GuestMethodPtr))
    def print_cbs(self):
        target_obj = self.target_obj
        print('target_obj=%s' % (target_obj,))
        names = []
        for i in range(len(self.state_list)):
            nme = self.state_list[i].name
            names.append(name)
            print('# %#x: %s' % (i, name))
        for i, name in enumerate(names):
            for kind in ['cb1', 'cb2', 'cb3']:
                meth = getattr(self, kind + 's')[i]
                _, target = meth.target_for(target_obj)
                target = self.guest.unslide_text(target)
                print('MakeName(%#x, "ZZ_%s_%s")' % (target, kind, name.as_str()))

class Killer(GuestStruct):
    statemgr = prop(0x28, StateMgr)

class Entity(GuestStruct):
    allocator = prop(0, GuestPtrPtr)
    model = prop(4, GuestPtrPtr)
    idbits = prop(0x14, s32)
    objrec = prop(0x18, lambda: ptr_to(ObjRec))
    min_y = prop(0x94, f32)
    vtable = prop(0xb4, GuestPtrPtr)
    sizeof_star = 0xb8

class PYES(Entity):
    loc = prop(0xb8, Point3D)
    houvelo = prop(0xc4, Point3D)
    influence_from_moving_platform = prop(0xdc, Point2D)
    gravity = prop(0x108, f32)
    player_idx = prop(0x110, s32)
    size1 = prop(0x114, Point2D)
    size2 = prop(0x11c, Point2D)
    unsize = prop(0x12c, Point2D)
    despawn_outsets = prop(0x134, fixed_array(f32, 4))

    old_loc = prop(0x308, Point3D)
    another_loc = prop(0x314, Point3D)
    stackid = prop(0x334, s32)
    tower_idx = prop(0x338, s32)

    sizeof_star = 0x358

class Player(PYES):
    sizeof_star = 0x2264
    flags_430 = prop(0x430, u32)
    flags_434 = prop(0x434, u32)
    statemgr_main = prop(0x358, StateMgr)
    statemgr_demo = prop(0x39c, StateMgr)
    statemgr_mantanim = prop(0x3e0, StateMgr)
    flags428 = prop(0x428, u32)
    other_flags = prop(0x42c, u32)
    jumpstate = prop(0x438, u32)
    other_jumpstate = prop(0x450, u32)
    flags = prop(0x544, u32)
    killer = prop(0x1de8+0xd4, ptr_to(Killer))
    timer_1dfc = prop(0x1dfc, u32)
    timer_1e00 = prop(0x1e00, u32)
    timer_1e04 = prop(0x1e04, u32)
    timer_1e4c = prop(0x1e4c, u32)
    timer_1e68 = prop(0x1e68, u32)
    timer_1e6c = prop(0x1e6c, u32)
    timer_1edc = prop(0x1edc, u32)
    transthing = prop(0x1f10, ptr_to(TransThing))
    timer_2004 = prop(0x2004, u32)
    timer_2008 = prop(0x2008, u32)
    timer_2020 = prop(0x2020, u32)
    timer_2024 = prop(0x2024, u32)
    timer_2038 = prop(0x2038, u32)
    timer_203c = prop(0x203c, u32)
    timer_2044 = prop(0x2044, u32)
    timer_204c = prop(0x204c, u32)
    timer_2050 = prop(0x2050, u32)
    timer_2054 = prop(0x2054, u32)
    timer_2068 = prop(0x2068, u32)
    timer_2074 = prop(0x2074, u32)
    timer_2078 = prop(0x2078, u32)
    timer_207c = prop(0x207c, u32)
    timer_2088 = prop(0x2088, u32)
    timer_2098 = prop(0x2098, u32)
    timer_209c = prop(0x209c, u32)
    timer_20a0 = prop(0x20a0, u32)
    timer_20ac = prop(0x20ac, u32)
    timer_20c0 = prop(0x20c0, u32)
    timer_20c8 = prop(0x20c8, u32)
    timer_20d0 = prop(0x20d0, u32)
    timer_20d4 = prop(0x20d4, u32)
    timer_20d8 = prop(0x20d8, u32)
    timer_20dc = prop(0x20dc, u32)
    timer_20e4 = prop(0x20e4, u32)
    timer_20e8 = prop(0x20e8, u32)
    timer_20ec = prop(0x20ec, u32)
    cape_related = prop(0x2100, u32)
    star_maybe = prop(0x220c, u32) # ?
    timer_2104 = prop(0x2104, u32)
    timer_2108 = prop(0x2108, u32)
    timer_210c = prop(0x210c, u32)
    timer_2114 = prop(0x2114, u32)
    timer_2118 = prop(0x2118, u32)
    timer_21bc = prop(0x21bc, u32)

class ObjLink(GuestStruct):
    next = prop(0, lambda: ptr_to(ObjLink)) # or null
    this = prop(4, ptr_to(GuestStruct))
    free = prop(8, u32)

global_root = lambda guest: ObjLink(guest, guest.read32(guest.slide_data(0x1036C1B0)))

class ObjRec(GuestStruct):
    vt = prop(4, u32)
    ctor = prop(8, u32)
    idee = prop(0xc, u32)
    name = prop(0x20, ptr_to(GuestCString))
    @functools.lru_cache(None)
    def get_name(self):
        return '%s(%x)' % (self.name.as_str(), self.idee)

class AllocLink(GuestStruct):
    prev = prop(0, ptr_to(lambda: AllocLink))
    next = prop(4, ptr_to(lambda: AllocLink))
class AllocTracker(AllocLink):
    obj_count = prop(8, u32)
    link_offset = prop(0xc, u32)
    def iter_allocs(self):
        link_offset = self.link_offset
        link = self.next
        while link != self:
            yield link.raw_offset(-link_offset, GuestPtr)
            link = link.next

objrecs_by_idee = lambda guest: fixed_array(ptr_to(ObjRec), 0xee)(guest, guest.slide_data(0x101CF5B0))

class MP5(GuestStruct):
    pointers = prop(0, count_ptr(ptr_to(PYES)))

class FancyString(GuestStruct):
    cstr = prop(0, ptr_to(GuestCString))
    vtable = prop(4, u32)

class HeapSuper(GuestStruct):
    name = prop(0x10, FancyString)

class Heap(HeapSuper):
    elm_size = prop(0x94, u32)
    range_start = prop(0x98, u32)
    range_size = prop(0x9c, u32)
    free_size = prop(0xa0, u32)
    first_free = prop(0xa4, u32)

class MakesPlayerObj(GuestStruct):
    tracker = prop(0x48f44, AllocTracker)
    tracker2 = prop(0x48f54, AllocTracker)
    tracker3 = prop(0x48f64, AllocTracker)
    mp5 = prop(0x527ec, MP5)
    heaps = prop(0x18, fixed_array(ptr_to(Heap), (0x50 - 0x18)//4))
    @staticmethod
    def get(guest):
        return MakesPlayerObj(guest, guest.read32(guest.slide_data(0x10194B08)))

class SomeCoiListSub1(GuestStruct):
    tracker = prop(0, AllocTracker)
    sizeof_star = 0xa6c
class SomeCoiList(GuestStruct):
    sub1s = prop(0x18, fixed_array(SomeCoiListSub1, 2))
    @staticmethod
    def get_ptr(guest):
        return ptr_to(SomeCoiList)(guest, guest.slide_data(0x10195644))
class EditActorPlacementData(GuestStruct):
    x = prop(0, f32)
    y = prop(4, f32)
    q = prop(8, f32)
    width = prop(0xc, f32)
    height = prop(0x10, f32)
    my_flags = prop(0x14, u32)
    child_flags = prop(0x18, u32)
    extdata = prop(0x1c, u32)
    my_type = prop(0x20, u8)
    child_type = prop(0x21, u8)
    link_id = prop(0x22, u16)
    effect_idx = prop(0x24, u16)
    my_transform_id = prop(0x26, u8)
    child_transform_id = prop(0x27, u8)

class CourseThing(GuestStruct):
    center_x = prop(0x88, f32)
    camera_left = prop(0x90, f32)
    camera_right = prop(0x98, f32)
    alt_left = prop(0xa0, f32)
    alt_right = prop(0xa8, f32)
    def get_ptr(guest):
        return ptr_to(CourseThing)(guest, guest.slide_data(0x1019B2EC))

def main():
    real_guest = MemrwGuest.with_hostport(sys.argv[1])
    guest = CachingGuest(real_guest)
    vt_player = guest.slide_data(0x100CE7B8)

    print('text_slide=%#x data_slide=%#x' % (guest.text_slide, guest.data_slide))

    def print_yatsu_counts():
        by_objrec = {}
        with guest:
            for yatsu in mpobj.mp5.pointers.get_all():
                if yatsu:
                    by_objrec.setdefault(yatsu.objrec, []).append(yatsu)
            for objrec, yatsus in sorted(by_objrec.items()):
                print('%s: %d' % (objrec.get_name(), len(yatsus)))

    with guest:
        _objrecs_by_idee = objrecs_by_idee(guest)
        if 0:
            for i in range(0xee):
                _objrec = _objrecs_by_idee[i]
                #print(i, hex(i), _objrec.name, hex(_objrec.vt), hex(_objrec.ctor))
                print('_%x_%s = 0x%x,' % (i, _objrec.name.as_str(), i))
            return
        if 0:
            brickish_edit_types = guest.slide_data(0x101D227C)
            for i in range(21):
                idee = guest.read32(brickish_edit_types + 4 * i)
                print('%#x: %s' % (i, _objrecs_by_idee[idee].name))
            return


        mpobj = MakesPlayerObj.get(guest)
        player = None
        for obj in mpobj.tracker3.iter_allocs():
            #print(obj); continue

            vt = guest.read32(obj.addr + 0xb4)
            if vt == vt_player:
                player = obj.cast(Player)
                break

    if 1:
        im = guest.read(0x4ccfbd00, 1280*720*4) 
        open('/tmp/im.rgba', 'wb').write(im)
        return

    if 0:
        with guest:
            for heap in mpobj.heaps:
                print(heap, heap.name.cstr)


    if 1:
        with guest:
            #print_yatsu_counts()
            #print(mpobj.mp5.pointers.base)
            for yatsu in mpobj.mp5.pointers.get_all():
                if yatsu and (
                    #yatsu.objrec.idee in {0x2d, 0x2e} or # BlackPakkun
                    #yatsu.objrec.idee == 0x1b or # JumpStepSide
                    #yatsu.objrec.idee == 0x20 # Met
                    #yatsu.objrec.idee == 0x1c # PSwitch
                    # yatsu.objrec.idee == 0x14 # HatenaBlock
                    True
                ):
                    #dump(yatsu)
                    #print(yatsu.vtable)
                    print('%s @ %f,%f [%f,%f] %s %#x' % (yatsu.objrec.get_name(), yatsu.loc.x, yatsu.loc.y, yatsu.loc.x % 16, yatsu.loc.y % 16, yatsu, yatsu.idbits))
                    #print(yatsu.loc)

    if 0:
        with guest:
            bp_y = None
            for yatsu in mpobj.mp5.pointers.get_all():
                if yatsu and yatsu.objrec.idee == 0x1c: # PSwitch
                    yatsu.loc.x, yatsu.loc.y = 200., 141.949997
                    #print(yatsu.loc.get())

    if 0:
        with guest:
            scl = SomeCoiList.get_ptr(guest).get()
            for sub1 in scl.sub1s[:1]:
                tracker = sub1.tracker
                for eapd in tracker.iter_allocs():
                    eapd = eapd.cast(EditActorPlacementData)
                    print(eapd.my_type)
                    if eapd.my_type == 44:
                        eapd.my_type = 51
                    #print(eapd.x, eapd.y)
                    #eapd.x -= 6*16
                    #if eapd.x < 0: eapd.x = 0

    if 0:
        print(player.loc.x)
        #player.loc.x = 0
        #player.old_loc.x = 0
        #player.another_loc.x = 0
        print(player)

    if 0:
        dx = 1500
        coursething = CourseThing.get_ptr(guest).get()
        player.loc.x += dx
        player.old_loc.x += dx
        player.another_loc.x += dx
        coursething.center_x += dx
        coursething.camera_left += dx
        coursething.camera_right += dx
        coursething.alt_left += dx
        coursething.alt_right += dx
    return

    #dump(sys.stdout, mpobj.tracker)
    #dump(sys.stdout, mpobj.tracker2)
    #dump(sys.stdout, mpobj.tracker3)
    #dump(sys.stdout, player)
    #print(addrof(player.killer.statemgr, 'state'))
    #StateMgr(guest, 0x2bc7fc48+0x1d84).print_cbs()
    #return
    #player.killer.statemgr.print_cbs()
    if 0:
        while 1:
            for mgr in [player.statemgr_main, player.statemgr_demo, player.statemgr_mantanim, player.killer.statemgr]:
                state = mgr.state
                print(state, '<none>' if state == 0xffffffff else mgr.state_list[state].name, end=' ')
            print()
            #player.statemgr_demo.state = 5
            #print(addrof(player, 'min_y'))
            #print(player, hex(player.flags_430), hex(player.flags_434), player.y, player.min_y)
            time.sleep(0.05)
    


    #link = ObjLink(guest, guest.read32(slide_data(0x1036C1B0)))
    #while link:
    #    print(hex(link.addr), hex(link.objrec.addr), hex(link.free))
    #    #dump(sys.stdout, link.objrec)
    #    link = link.next

if __name__ == '__main__':
    main()
