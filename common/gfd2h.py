# TODO const
from construct import *
import sys, types

GFDFileHeader = Struct(
    Const(b'Gfx2'),
    Const(8*4, Int32ub),
    'major' / Int32ub,
    'minor' / Int32ub,
    'gpu' / Int32ub,
    'align' / Int32ub,
    Padding(8),
)
GFDBlockHeader = Struct(
    Const(b'BLK{'),
    Const(8*4, Int32ub),
    'major' / Int32ub,
    'minor' / Int32ub,
    'type' / Enum(Int32ub,
        eof=1,
        padding=2,
        vertex_shader_header=3,
        vertex_shader_program=5,
        pixel_shader_header=6,
        pixel_shader_program=7,
        geometry_shader_header=8,
        geometry_shader_program=9,
        geometry_shader_copy_program=10,
        texture_header=11,
        texture_image=12,
        texture_mipmap=13,
        compute_shader_header=14,
        compute_shader_program=15
    ),
    'data_size' / Int32ub,
    'id' / Int32ub,
    'index' / Int32ub,
)
GFDRelocationHeader = Struct(
    Const(b'}BLK'),
    Const(10*4, Int32ub),
    'unk1' / Int32ub,
    'data_size' / Int32ub,
    'data_offset' / Int32ub,
    'text_size' / Int32ub,
    'text_offset' / Int32ub,
    'patch_base' / Int32ub,
    'patch_count' / Int32ub,
    'patch_offset' / Int32ub,
)
GFDRelocations = Struct(
    'header' / GFDRelocationHeader,
    'patches' / Pointer(this.header.patch_offset & 0xfffff,
                        Int32ub[this.header.patch_count])
)
GX2RData = Struct(
    'flags' / Int32ub,
    'elem_size' / Int32ub,
    'elem_count' / Int32ub,
    Const(0, Int32ub),
)

def XPointer(subcon):
    return Struct(
        'offset' / Int32ub,
        'content' / Pointer(this.offset & 0xfffff, subcon),
    )

UniformBlock = Struct(
    'ctype' / Computed('struct GX2UniformBlock'),
    'name' / XPointer(CString(encoding='utf8')),
    'offset' / Int32ub,
    'size' / Int32ub,
)
UniformVar = Struct(
    'ctype' / Computed('struct GX2UniformVar'),
    'name' / XPointer(CString(encoding='utf8')),
    'type' / Int32ub,
    'count' / Int32ub,
    'offset' / Int32ub,
    'block' / Int32sb,
)
InitialValue = Struct(
    'ctype' / Computed('struct GX2UniformInitialValue'),
    'value' / Float32b[4],
    'offset' / Int32ub,
)
LoopVar = Struct(
    'ctype' / Computed('struct GX2LoopVar'),
    'offset' / Int32ub,
    'value' / Int32ub,
)
SamplerVar = Struct(
    'ctype' / Computed('struct GX2SamplerVar'),
    'name' / XPointer(CString(encoding='utf8')),
    'type' / Int32ub,
    'location' / Int32ub,
)
AttribVar = Struct(
    'ctype' / Computed('struct GX2AttribVar'),
    'name' / XPointer(CString(encoding='utf8')),
    'type' / Int32ub,
    'count' / Int32ub,
    'location' / Int32ub,
)
def CountOffset(subcon):
    return Struct(
        'count' / Int32ub,
        'pointer' / XPointer(subcon[this._.count])
    )
GFDVertexShaderHeader = Struct(
    'ctype' / Computed('__attribute__((aligned(64))) struct GX2VertexShader'),
    'sq_pgm_resources_vs' / Int32ub,
    'vgt_primitiveid_en' / Int32ub,
    'spi_vs_out_config' / Int32ub,
    'num_spi_vs_out_id' / Int32ub,
    'spi_vs_out_id' / Int32ub[10],
    'pa_cl_vs_out_cntl' / Int32ub,
    'sq_vtx_semantic_clear' / Int32ub,
    'num_sq_vtx_semantic' / Int32ub,
    'sq_vtx_semantic' / Int32ub[32],
    'vgt_strmout_buffer_en' / Int32ub,
    'vgt_vertex_reuse_block_cntl' / Int32ub,
    'vgt_hos_reuse_depth' / Int32ub,
    'shader_size' / Int32ub,
    'shader_ptr' / Int32ub,
    'mode' / Int32ub,
    'uniform_blocks' / CountOffset(UniformBlock),
    'uniform_vars' / CountOffset(UniformVar),
    'initial_values' / CountOffset(InitialValue),
    'loop_vars' / CountOffset(LoopVar),
    'sampler_vars' / CountOffset(SamplerVar),
    'attrib_vars' / CountOffset(AttribVar),
    'ring_item_size' / Int32ub,
    'has_stream_out' / Int32ub,
    'stream_out_stride' / Int32ub[4],
    'gx2rdata' / GX2RData,
)
GFDPixelShaderHeader = Struct(
    'ctype' / Computed('__attribute__((aligned(64))) struct GX2PixelShader'),
    'sq_pgm_resources_ps' / Int32ub,
    'sq_pgm_exports_ps' / Int32ub,
    'spi_ps_in_control_0' / Int32ub,
    'spi_ps_in_control_1' / Int32ub,
    'num_spi_ps_input_cntl' / Int32ub,
    'spi_ps_input_cntl' / Int32ub[32],
    'cb_shader_mask' / Int32ub,
    'cb_shader_control' / Int32ub,
    'db_shader_control' / Int32ub,
    'spi_input_z' / Int32ub,
    'shader_size' / Int32ub,
    'shader_ptr' / Int32ub,
    'mode' / Int32ub,
    'uniform_blocks' / CountOffset(UniformBlock),
    'uniform_vars' / CountOffset(UniformVar),
    'initial_values' / CountOffset(InitialValue),
    'loop_vars' / CountOffset(LoopVar),
    'sampler_vars' / CountOffset(SamplerVar),
    'gx2rdata' / GX2RData,
)
def Relocated(subcon):
    return Struct(
        Seek(-GFDRelocationHeader.sizeof(), 2),
        'relocations' / GFDRelocations,
        Seek(0),
        Embedded(subcon),
    )

GFDBlock = Struct(
    'header' / GFDBlockHeader,
    'content' / Prefixed(Computed(this.header.data_size), Switch(this.header.type, {
        'vertex_shader_header': Relocated(GFDVertexShaderHeader),
        'pixel_shader_header': Relocated(GFDPixelShaderHeader),
    }, default=Bytes(this.header.data_size)))
)

GFD = Struct(
    'header' / GFDFileHeader,
    'blocks' / GFDBlock[:],
    Terminated
)

def format_bytes(data, indent):
    assert isinstance(data, bytes)
    indent2 = indent + '    '
    width = (80 - len(indent2)) / len('0x00, ')
    width = max(width & ~3, 1)
    out = '{'
    for i, byte in enumerate(data):
        if isinstance(byte, bytes):
            byte = ord(byte)
        if i % width == 0:
            out += '\n' + indent2
        else:
            out += ' '
        out += '0x{:02x},'.format(byte)
    out += '\n' + indent + '}'
    return out

def format_struct(ctx, indent, **kwargs):
    indent2 = indent + '    '
    out = '{'
    for key, value in ctx.items():
        if key in {'relocations', 'ctype'}:
            continue
        if key == 'pointer':
            if ctx.count == 0:
                valfmt = 'NULL'
            else:
                valfmt = '({ty}[]){fmt}'.format(
                    ty=value.content[0].ctype,
                    fmt=format_list(value.content, indent2, **kwargs))
        elif key == 'shader_ptr':
            valfmt = kwargs['content_var_name']
        elif hasattr(value, 'offset') and hasattr(value, 'content'):
            valfmt = format_value(value.content, indent2, **kwargs)
        else:
            valfmt = format_value(value, indent2, **kwargs)
        out += '\n{indent}.{key} = {valfmt},'.format(
            indent=indent2, key=key, valfmt=valfmt)
    out += '\n' + indent + '}'
    return out

def format_list(lst, indent, **kwargs):
    indent2 = indent + '    '
    out = '{'
    for i, value in enumerate(lst):
        valfmt = format_value(value, indent2, **kwargs)
        out += '\n{indent}[{i}] = {valfmt},'.format(
            indent=indent2, i=i, valfmt=valfmt)
    out += '\n' + indent + '}'
    return out

def format_value(value, indent, **kwargs):
    if hasattr(value, 'real'):
        if value <= 9:
            return str(value)
        else:
            return '0x{:x}'.format(value)
    elif isinstance(value, list):
        return format_list(value, indent)
    elif isinstance(value, Container):
        return format_struct(value, indent, **kwargs)
    elif isinstance(value, type(u'')):
        return '"{value}"'.format(value=value)
    else:
        raise ValueError(value)


def format_shader_header(content, header_var_name, content_var_name):
    return 'static {ty} {header_var_name} = {fmt};\n'.format(
        content=content, ty=content.ctype,
        header_var_name=header_var_name, content_var_name=content_var_name,
        fmt=format_struct(content, '', content_var_name=content_var_name))

def format_shader_content(content, content_var_name):
    return 'static uint8_t {content_var_name}[0x{len:x}] __attribute__((aligned(0x100))) = {fmt};\n'.format(
        content_var_name=content_var_name,
        len=len(content),
        fmt=format_bytes(content, indent=''))

def format_header(parsed, prefix):
    out = ''
    out += '#include "decls.h"\n'
    for block in parsed.blocks[::-1]:
        if block.header.type == 'vertex_shader_header':
            out += format_shader_header(block.content,
                                        prefix + '_vsh',
                                        prefix + '_vsh_content')
        elif block.header.type == 'vertex_shader_program':
            out += format_shader_content(block.content,
                                         prefix + '_vsh_content')
        elif block.header.type == 'pixel_shader_header':
            out += format_shader_header(block.content,
                                        prefix + '_psh',
                                        prefix + '_psh_content')
        elif block.header.type == 'pixel_shader_program':
            out += format_shader_content(block.content,
                                         prefix + '_psh_content')
        elif block.header.type == 'eof':
            continue
        else:
            raise ValueError(block.header.type)
        out += '\n'
    return out

gfd_filename = sys.argv[1]
prefix = sys.argv[2]
out_filename = sys.argv[3]
with open(gfd_filename, 'rb') as fp:
    parsed = GFD.parse_stream(fp)
fmt = format_header(parsed, prefix)
with open(out_filename, 'w') as fp:
    fp.write(fmt)
