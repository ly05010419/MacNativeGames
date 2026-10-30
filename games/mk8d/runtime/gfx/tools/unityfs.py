#!/usr/bin/env python3
# Unity asset bundle reader (enough for shaders): UnityFS bundle → serialized files → objects decoded by their type trees → each platform's subprograms of a Shader.
#   · UnityFS ('UnityFS\0', format 6..8): big-endian header + block info (LZMA / LZ4 / LZ4HC or stored) + data blocks; the block info lists the file nodes (offset, size, path).
#   · Serialized file (format 17..22): metadata (type table with type trees, object table) + object data. A type tree string offset with the high bit set refers to Unity's built-in common string table,
#     which is not shipped with the assets; it is taken from the game's own main ('AABB\0AnimationClip\0…', see common_strings). A type tree node is 32 bytes from format 19 (Unity 2019.1, the trailing
#     refTypeHash u64) and 24 bytes before it (format 17: Unity 5.5 to 2018.4).
#   · Shader (class 48): the compressedBlob of platform platforms[i] is LZ4-compressed in segments offsets[i][s] / compressedLengths[i][s] / decompressedLengths[i][s] (2019.3 and later);
#     inside a decompressed segment: u32 subprogram count, then 12-byte entries {offset, length, segment}. Before 2019.3 each platform has one segment (offsets[i] is a number) and the entries are
#     8 bytes {offset, length}. A subprogram = version, program type, stats, keywords, program code bytes, ...
#     From Unity 2021.2 (subprogram version 202012090) the local keyword list is gone; Unity 2022 also keeps each program's parameters in the blob as entries of their own (the parsed form's
#     m_ParameterBlobIndices), and the code entries are the m_BlobIndex of m_PlayerSubPrograms (Ace Attorney Investigations Collection, Unity 2022.3.17f1)
#   · TextAsset (class 49): name and bytes (text_assets), for games that keep whole GLSLC outputs of their own in one (FINAL FANTASY CRYSTAL CHRONICLES Remastered Edition, Unity 2018.4)
#   The pre-2019.3 layouts were found by armyja on Cuphead (Unity 2017.4.9f1, serialized format 17), GitHub pull request #2
# Only depends on lz4 (the same package shader_split uses for NSOs)
import lzma, struct

CLASS_SHADER = 48

def _lz4(raw, size):
    import lz4.block
    return lz4.block.decompress(raw, uncompressed_size=size)

def _decomp(raw, size, kind):
    if kind == 0: return raw
    if kind == 1:   # Unity's LZMA: 5 property bytes + raw stream
        props = raw[0]; dict_size = struct.unpack_from('<I', raw, 1)[0]
        lc, lp, pb = props % 9, (props // 9) % 5, props // 45
        f = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dict_size, 'lc': lc, 'lp': lp, 'pb': pb}])
        return f.decompress(raw[5:], size)[:size]
    if kind in (2, 3): return _lz4(raw, size)
    raise ValueError(f'UnityFS: unknown compression {kind}')

def _cstr(d, p):
    e = d.index(b'\0', p); return d[p:e].decode('utf-8', 'replace'), e + 1

def bundle_files(d):
    """UnityFS bundle → [(path, bytes)]"""
    if d[:8] != b'UnityFS\0': raise ValueError('not UnityFS')
    fmt = struct.unpack_from('>I', d, 8)[0]
    _, p = _cstr(d, 12); _, p = _cstr(d, p)
    size, csz, usz, flags = struct.unpack_from('>qIII', d, p); p += 20
    if fmt >= 7: p = (p + 15) & ~15
    if flags & 0x80: info_raw = d[len(d) - csz:]
    else: info_raw = d[p:p + csz]; p += csz
    info = _decomp(info_raw, usz, flags & 0x3f)
    q = 16; nb = struct.unpack_from('>i', info, q)[0]; q += 4
    blocks = []
    for _ in range(nb):
        u, c, f = struct.unpack_from('>IIH', info, q); q += 10; blocks.append((u, c, f))
    nn = struct.unpack_from('>i', info, q)[0]; q += 4
    nodes = []
    for _ in range(nn):
        off, sz, f = struct.unpack_from('>qqI', info, q); q += 20
        name, q = _cstr(info, q); nodes.append((off, sz, name))
    if flags & 0x200: p = (p + 15) & ~15   # BlockInfoNeedPaddingAtStart
    out = bytearray()
    for u, c, f in blocks:
        out += _decomp(d[p:p + c], u, f & 0x3f); p += c
    return [(name, bytes(out[off:off + sz])) for off, sz, name in nodes]

def common_strings(image):
    """Unity's common string table in the game's main → {offset: string}"""
    a = image.find(b'AABB\0AnimationClip\0AnimationCurve\0')
    if a < 0: raise ValueError('no Unity common string table in main (AABB\\0AnimationClip...)')
    tab, p = {}, a
    while True:
        e = image.index(b'\0', p)
        if e == p: break
        s = image[p:e]
        if not all(0x20 <= ch < 0x7f for ch in s): break
        tab[p - a] = s.decode(); p = e + 1
    return tab

class Node:
    __slots__ = ('type', 'name', 'size', 'flags', 'kids')
    def __init__(s, t, n, z, f): s.type, s.name, s.size, s.flags, s.kids = t, n, z, f, []

class Reader:
    def __init__(s, d, le): s.d, s.p, s.e = d, 0, '<' if le else '>'
    def u(s, fmt):
        v = struct.unpack_from(s.e + fmt, s.d, s.p); s.p += struct.calcsize(s.e + fmt); return v[0] if len(v) == 1 else v
    def align(s): s.p = (s.p + 3) & ~3

PRIM = {'SInt8': 'b', 'UInt8': 'B', 'char': 'B', 'bool': '?', 'SInt16': 'h', 'short': 'h', 'UInt16': 'H', 'unsigned short': 'H',
        'SInt32': 'i', 'int': 'i', 'UInt32': 'I', 'unsigned int': 'I', 'Type*': 'i', 'SInt64': 'q', 'long long': 'q', 'UInt64': 'Q',
        'unsigned long long': 'Q', 'FileSize': 'Q', 'float': 'f', 'double': 'd'}

def _read(r, n):
    if n.type in PRIM and not n.kids:
        v = r.u(PRIM[n.type])
    elif n.type == 'string':
        ln = r.u('i'); v = r.d[r.p:r.p + ln]; r.p += ln
        try: v = v.decode('utf-8')
        except UnicodeDecodeError: pass
        if n.kids and n.kids[0].flags & 0x4000: r.align()
    elif n.kids and n.kids[0].type == 'Array':
        arr = n.kids[0]; cnt = r.u('i'); el = arr.kids[1]
        if el.type in ('UInt8', 'char', 'SInt8') and not el.kids: v = r.d[r.p:r.p + cnt]; r.p += cnt
        elif el.type in PRIM and not el.kids:
            f = PRIM[el.type]; w = struct.calcsize(f); v = list(struct.unpack_from(r.e + f * cnt, r.d, r.p)); r.p += w * cnt
        else: v = [_read(r, el) for _ in range(cnt)]
        if arr.flags & 0x4000: r.align()
    elif n.type == 'TypelessData':
        cnt = r.u('i'); v = r.d[r.p:r.p + cnt]; r.p += cnt
    else:
        v = {k.name: _read(r, k) for k in n.kids}
    if n.flags & 0x4000: r.align()
    return v

class NoTree(ValueError):
    """the serialized file has no type tree and no tree of the same class and hash to borrow"""

# Type trees written out for players whose serialized files carry none and that ship no bundle to borrow one from (Brain Memory Training Academy, Unity 2022.3.5f1: only Data/*.assets;
# Patrick's Parabox, Unity 2020.3.30f1: only the built-in shaders in Data/Resources).
# {(class ID, type hash): (root type, layout)}. The layout is one struct per line, "Name: type field; type field; …", in serialization order. A type is a primitive, a struct of the layout,
# string, vector<element name:type>, map<data:pair(key type,value type)>; a trailing ~ = the container's Array is not aligned after it, ! = the field itself is aligned after it (the 0x4000 flags).
# The structs up to SerializedProgramParameters are the same in both layouts below
_SHADER_COMMON = '''
SerializedTextureProperty: string m_DefaultName; int m_TexDim
SerializedProperty: string m_Name; string m_Description; vector<data:string> m_Attributes; int m_Type; unsigned int m_Flags; float m_DefValue[0]; float m_DefValue[1]; float m_DefValue[2]; float m_DefValue[3]; SerializedTextureProperty m_DefTexture
SerializedProperties: vector<data:SerializedProperty> m_Props
Hash128: ''' + '; '.join(f'UInt8 bytes[{i}]' for i in range(16)) + '''
SerializedShaderFloatValue: float val; string name
SerializedShaderRTBlendState: SerializedShaderFloatValue srcBlend; SerializedShaderFloatValue destBlend; SerializedShaderFloatValue srcBlendAlpha; SerializedShaderFloatValue destBlendAlpha; SerializedShaderFloatValue blendOp; SerializedShaderFloatValue blendOpAlpha; SerializedShaderFloatValue colMask
SerializedStencilOp: SerializedShaderFloatValue pass; SerializedShaderFloatValue fail; SerializedShaderFloatValue zFail; SerializedShaderFloatValue comp
SerializedShaderVectorValue: SerializedShaderFloatValue x; SerializedShaderFloatValue y; SerializedShaderFloatValue z; SerializedShaderFloatValue w; string name
SerializedTagMap: map<data:pair(string,string)>~ tags
SerializedShaderState: string m_Name; ''' + '; '.join(f'SerializedShaderRTBlendState rtBlend{i}' for i in range(8)) + '''; bool! rtSeparateBlend; SerializedShaderFloatValue zClip; SerializedShaderFloatValue zTest; SerializedShaderFloatValue zWrite; SerializedShaderFloatValue culling; SerializedShaderFloatValue conservative; SerializedShaderFloatValue offsetFactor; SerializedShaderFloatValue offsetUnits; SerializedShaderFloatValue alphaToMask; SerializedStencilOp stencilOp; SerializedStencilOp stencilOpFront; SerializedStencilOp stencilOpBack; SerializedShaderFloatValue stencilReadMask; SerializedShaderFloatValue stencilWriteMask; SerializedShaderFloatValue stencilRef; SerializedShaderFloatValue fogStart; SerializedShaderFloatValue fogEnd; SerializedShaderFloatValue fogDensity; SerializedShaderVectorValue fogColor; int fogMode; int gpuProgramID; SerializedTagMap m_Tags; int m_LOD; bool! lighting
ShaderBindChannel: SInt8 source; SInt8 target
ParserBindChannels: vector<data:ShaderBindChannel>! m_Channels; int m_SourceMap
VectorParameter: int m_NameIndex; int m_Index; int m_ArraySize; SInt8 m_Type; SInt8! m_Dim
MatrixParameter: int m_NameIndex; int m_Index; int m_ArraySize; SInt8 m_Type; SInt8! m_RowCount
TextureParameter: int m_NameIndex; int m_Index; int m_SamplerIndex; bool m_MultiSampled; SInt8! m_Dim
BufferBinding: int m_NameIndex; int m_Index; int m_ArraySize
StructParameter: int m_NameIndex; int m_Index; int m_ArraySize; int m_StructSize; vector<data:VectorParameter> m_VectorMembers; vector<data:MatrixParameter> m_MatrixMembers
ConstantBuffer: int m_NameIndex; vector<data:MatrixParameter> m_MatrixParams; vector<data:VectorParameter> m_VectorParams; vector<data:StructParameter> m_StructParams; int m_Size; bool! m_IsPartialCB
UAVParameter: int m_NameIndex; int m_Index; int m_OriginalIndex
SamplerParameter: unsigned int sampler; int bindPoint
SerializedProgramParameters: vector<data:VectorParameter> m_VectorParams; vector<data:MatrixParameter> m_MatrixParams; vector<data:TextureParameter> m_TextureParams; vector<data:BufferBinding> m_BufferParams; vector<data:ConstantBuffer> m_ConstantBuffers; vector<data:BufferBinding> m_ConstantBufferBindings; vector<data:UAVParameter> m_UAVParams; vector<data:SamplerParameter> m_Samplers
'''
# The Unity 2022.3 Shader matches, node for node with those flags, the type tree in Ace Attorney Investigations Collection's bundles (Unity 2022.3.17f1), whose hash is the same
SHADER_2022_3 = _SHADER_COMMON + '''SerializedSubProgram: unsigned int m_BlobIndex; ParserBindChannels m_Channels; vector<data:UInt16> m_KeywordIndices; SInt8 m_ShaderHardwareTier; SInt8! m_GpuProgramType; SerializedProgramParameters m_Parameters; SInt64 m_ShaderRequirements
SerializedPlayerSubProgram: unsigned int m_BlobIndex; vector<data:UInt16> m_KeywordIndices; SInt64 m_ShaderRequirements; SInt8! m_GpuProgramType
SerializedProgram: vector<data:SerializedSubProgram> m_SubPrograms; vector<data:vector<data:SerializedPlayerSubProgram>> m_PlayerSubPrograms; vector<data:vector<data:unsigned int>> m_ParameterBlobIndices; SerializedProgramParameters m_CommonParameters; vector<data:UInt16> m_SerializedKeywordStateMask
SerializedPass: vector<data:Hash128> m_EditorDataHash; vector<data:UInt8> m_Platforms; map<data:pair(string,int)>~ m_NameIndices; int m_Type; SerializedShaderState m_State; unsigned int m_ProgramMask; SerializedProgram progVertex; SerializedProgram progFragment; SerializedProgram progGeometry; SerializedProgram progHull; SerializedProgram progDomain; SerializedProgram progRayTracing; bool m_HasInstancingVariant; bool! m_HasProceduralInstancingVariant; string m_UseName; string m_Name; string m_TextureName; SerializedTagMap m_Tags
SerializedSubShader: vector<data:SerializedPass> m_Passes; SerializedTagMap m_Tags; int m_LOD
SerializedShaderDependency: string from; string to
SerializedCustomEditorForRenderPipeline: string customEditorName; string renderPipelineType
SerializedShader: SerializedProperties m_PropInfo; vector<data:SerializedSubShader> m_SubShaders; vector<data:string> m_KeywordNames; vector<data:UInt8> m_KeywordFlags; string m_Name; string m_CustomEditorName; string m_FallbackName; vector<data:SerializedShaderDependency> m_Dependencies; vector<data:SerializedCustomEditorForRenderPipeline> m_CustomEditorForRenderPipelines; bool! m_DisableNoSubshadersMessage
PPtr<Shader>: int m_FileID; SInt64 m_PathID
PPtr<Texture>: int m_FileID; SInt64 m_PathID
Shader: string m_Name; SerializedShader m_ParsedForm; vector<data:unsigned int> platforms; vector<data:vector<data:unsigned int>> offsets; vector<data:vector<data:unsigned int>> compressedLengths; vector<data:vector<data:unsigned int>> decompressedLengths; vector<data:UInt8> compressedBlob; vector<data:unsigned int> stageCounts; vector<data:PPtr<Shader>> m_Dependencies; map<data:pair(string,PPtr<Texture>)>~ m_NonModifiableTextures; bool! m_ShaderIsBaked
'''
# Unity 6000.0.28f1 (Donkey Kong Country Returns HD) strips every tree and ships no bundles. Its Shader hash is the one BALL x PIT's 6000.0.62f1 bundles carry:
# the 2022.3 layout without the pass's editor hashes / platforms, with a final asset GUID. Checked against that bundle's nodes, order and alignment flags,
# and all 214 stripped Shader objects read to their exact ends. Keep the older layout intact: its pass headers still contain those two arrays.
SHADER_6000_0 = SHADER_2022_3.replace('vector<data:Hash128> m_EditorDataHash; vector<data:UInt8> m_Platforms; ', '').replace(
    'bool! m_ShaderIsBaked\n', 'bool! m_ShaderIsBaked; GUID m_AssetGUID\n') + 'GUID: unsigned int data[0]; unsigned int data[1]; unsigned int data[2]; unsigned int data[3]\n'
# The Unity 2020.3 Shader (Patrick's Parabox, Unity 2020.3.30f1: only Data/*.assets, its shaders the built-in ones in Data/Resources) matches the type tree in Fire Emblem Engage's bundles
# (Unity 2020.3.18f1), whose hash is the same: global and local keyword indices per subprogram, every subprogram in m_SubPrograms, no parameter blobs, keyword names or stage counts
SHADER_2020_3 = _SHADER_COMMON + '''SerializedSubProgram: unsigned int m_BlobIndex; ParserBindChannels m_Channels; vector<data:UInt16> m_GlobalKeywordIndices; vector<data:UInt16> m_LocalKeywordIndices; SInt8 m_ShaderHardwareTier; SInt8! m_GpuProgramType; SerializedProgramParameters m_Parameters; int m_ShaderRequirements
SerializedProgram: vector<data:SerializedSubProgram> m_SubPrograms; SerializedProgramParameters m_CommonParameters
SerializedPass: vector<data:Hash128> m_EditorDataHash; vector<data:UInt8> m_Platforms; vector<data:UInt16> m_LocalKeywordMask; vector<data:UInt16> m_GlobalKeywordMask; map<data:pair(string,int)>~ m_NameIndices; int m_Type; SerializedShaderState m_State; unsigned int m_ProgramMask; SerializedProgram progVertex; SerializedProgram progFragment; SerializedProgram progGeometry; SerializedProgram progHull; SerializedProgram progDomain; SerializedProgram progRayTracing; bool m_HasInstancingVariant; bool! m_HasProceduralInstancingVariant; string m_UseName; string m_Name; string m_TextureName; SerializedTagMap m_Tags
SerializedSubShader: vector<data:SerializedPass> m_Passes; SerializedTagMap m_Tags; int m_LOD
SerializedShaderDependency: string from; string to
SerializedShader: SerializedProperties m_PropInfo; vector<data:SerializedSubShader> m_SubShaders; string m_Name; string m_CustomEditorName; string m_FallbackName; vector<data:SerializedShaderDependency> m_Dependencies; bool! m_DisableNoSubshadersMessage
PPtr<Shader>: int m_FileID; SInt64 m_PathID
PPtr<Texture>: int m_FileID; SInt64 m_PathID
Shader: string m_Name; SerializedShader m_ParsedForm; vector<data:unsigned int> platforms; vector<data:vector<data:unsigned int>> offsets; vector<data:vector<data:unsigned int>> compressedLengths; vector<data:vector<data:unsigned int>> decompressedLengths; vector<data:UInt8> compressedBlob; vector<data:PPtr<Shader>> m_Dependencies; map<data:pair(string,PPtr<Texture>)>~ m_NonModifiableTextures; bool! m_ShaderIsBaked
'''
# The Unity 2018.3 Shader (Raging Loop, Unity 2018.3.10f1: no bundles, its 48 shaders in Data/*.assets and Data/Resources), from Unity 2018.3.10f1's type tree as AssetRipper's TypeTreeDumps
# records it (StructsDump/release/2018.3.10f1.dump), checked node for node with the flags: older than the common structs above (no conservative raster state, no buffer array sizes, no partial
# constant buffers), the parameters in each subprogram, one segment per platform (offsets, compressedLengths and decompressedLengths are plain numbers per platform), no local keywords
SHADER_2018_3 = '''SerializedTextureProperty: string m_DefaultName; int m_TexDim
SerializedProperty: string m_Name; string m_Description; vector<data:string> m_Attributes; int m_Type; unsigned int m_Flags; float m_DefValue[0]; float m_DefValue[1]; float m_DefValue[2]; float m_DefValue[3]; SerializedTextureProperty m_DefTexture
SerializedProperties: vector<data:SerializedProperty> m_Props
SerializedShaderFloatValue: float val; string name
SerializedShaderRTBlendState: SerializedShaderFloatValue srcBlend; SerializedShaderFloatValue destBlend; SerializedShaderFloatValue srcBlendAlpha; SerializedShaderFloatValue destBlendAlpha; SerializedShaderFloatValue blendOp; SerializedShaderFloatValue blendOpAlpha; SerializedShaderFloatValue colMask
SerializedStencilOp: SerializedShaderFloatValue pass; SerializedShaderFloatValue fail; SerializedShaderFloatValue zFail; SerializedShaderFloatValue comp
SerializedShaderVectorValue: SerializedShaderFloatValue x; SerializedShaderFloatValue y; SerializedShaderFloatValue z; SerializedShaderFloatValue w; string name
SerializedTagMap: map<data:pair(string,string)>~ tags
SerializedShaderState: string m_Name; ''' + '; '.join(f'SerializedShaderRTBlendState rtBlend{i}' for i in range(8)) + '''; bool! rtSeparateBlend; SerializedShaderFloatValue zClip; SerializedShaderFloatValue zTest; SerializedShaderFloatValue zWrite; SerializedShaderFloatValue culling; SerializedShaderFloatValue offsetFactor; SerializedShaderFloatValue offsetUnits; SerializedShaderFloatValue alphaToMask; SerializedStencilOp stencilOp; SerializedStencilOp stencilOpFront; SerializedStencilOp stencilOpBack; SerializedShaderFloatValue stencilReadMask; SerializedShaderFloatValue stencilWriteMask; SerializedShaderFloatValue stencilRef; SerializedShaderFloatValue fogStart; SerializedShaderFloatValue fogEnd; SerializedShaderFloatValue fogDensity; SerializedShaderVectorValue fogColor; int fogMode; int gpuProgramID; SerializedTagMap m_Tags; int m_LOD; bool! lighting
ShaderBindChannel: SInt8 source; SInt8 target
ParserBindChannels: vector<data:ShaderBindChannel>! m_Channels; int m_SourceMap
VectorParameter: int m_NameIndex; int m_Index; int m_ArraySize; SInt8 m_Type; SInt8! m_Dim
MatrixParameter: int m_NameIndex; int m_Index; int m_ArraySize; SInt8 m_Type; SInt8! m_RowCount
TextureParameter: int m_NameIndex; int m_Index; int m_SamplerIndex; bool m_MultiSampled; SInt8! m_Dim
BufferBinding: int m_NameIndex; int m_Index
StructParameter: int m_NameIndex; int m_Index; int m_ArraySize; int m_StructSize; vector<data:VectorParameter> m_VectorMembers; vector<data:MatrixParameter> m_MatrixMembers
ConstantBuffer: int m_NameIndex; vector<data:MatrixParameter> m_MatrixParams; vector<data:VectorParameter> m_VectorParams; vector<data:StructParameter> m_StructParams; int m_Size
UAVParameter: int m_NameIndex; int m_Index; int m_OriginalIndex
SamplerParameter: unsigned int sampler; int bindPoint
SerializedSubProgram: unsigned int m_BlobIndex; ParserBindChannels m_Channels; vector<data:UInt16> m_KeywordIndices; SInt8 m_ShaderHardwareTier; SInt8! m_GpuProgramType; vector<data:VectorParameter> m_VectorParams; vector<data:MatrixParameter> m_MatrixParams; vector<data:TextureParameter> m_TextureParams; vector<data:BufferBinding> m_BufferParams; vector<data:ConstantBuffer> m_ConstantBuffers; vector<data:BufferBinding> m_ConstantBufferBindings; vector<data:UAVParameter> m_UAVParams; vector<data:SamplerParameter> m_Samplers; int m_ShaderRequirements
SerializedProgram: vector<data:SerializedSubProgram> m_SubPrograms
SerializedPass: map<data:pair(string,int)>~ m_NameIndices; int m_Type; SerializedShaderState m_State; unsigned int m_ProgramMask; SerializedProgram progVertex; SerializedProgram progFragment; SerializedProgram progGeometry; SerializedProgram progHull; SerializedProgram progDomain; bool m_HasInstancingVariant; bool! m_HasProceduralInstancingVariant; string m_UseName; string m_Name; string m_TextureName; SerializedTagMap m_Tags
SerializedSubShader: vector<data:SerializedPass> m_Passes; SerializedTagMap m_Tags; int m_LOD
SerializedShaderDependency: string from; string to
SerializedShader: SerializedProperties m_PropInfo; vector<data:SerializedSubShader> m_SubShaders; string m_Name; string m_CustomEditorName; string m_FallbackName; vector<data:SerializedShaderDependency> m_Dependencies; bool! m_DisableNoSubshadersMessage
PPtr<Shader>: int m_FileID; SInt64 m_PathID
PPtr<Texture>: int m_FileID; SInt64 m_PathID
Shader: string m_Name; SerializedShader m_ParsedForm; vector<data:unsigned int> platforms; vector<data:unsigned int> offsets; vector<data:unsigned int> compressedLengths; vector<data:unsigned int> decompressedLengths; vector<data:UInt8> compressedBlob; vector<data:PPtr<Shader>> m_Dependencies; map<data:pair(string,PPtr<Texture>)>~ m_NonModifiableTextures; bool! m_ShaderIsBaked
'''
# Unity 2017.4.28f1 (Return of the Obra Dinn): the same layout except for these two absent fields, checked against
# AssetRipper's TypeTreeDumps StructsDump/release/2017.4.28f1.dump. The built-in resources strip their trees and no bundle
# supplies this hash. The instancing flag itself takes the alignment formerly carried by the following flag.
SHADER_2017_4 = SHADER_2018_3.replace('bool m_HasInstancingVariant; bool! m_HasProceduralInstancingVariant',
                                      'bool! m_HasInstancingVariant').replace(
    '; map<data:pair(string,PPtr<Texture>)>~ m_NonModifiableTextures', '')
# The Unity 2022.3 ComputeShader (Monster Train 2, Unity 2022.3.54f1: 12 in Data/sharedassets2.assets, which carries no type trees, and no bundle has one), from Unity 2022.3.54f1's type tree
# as AssetRipper's TypeTreeDumps records it (StructsDump/release/2022.3.54f1.dump). Each kernel variant's code is a program code of one stage, compute (the layout of a Shader's subprogram code)
COMPUTE_2022_3 = '''ComputeShaderResource: string name; string generatedName; int bindPoint; int samplerBindPoint; int texDimension
ComputeShaderBuiltinSampler: unsigned int sampler; int bindPoint
ComputeShaderKernel: vector<data:unsigned int> cbVariantIndices; vector<data:ComputeShaderResource> cbs; vector<data:ComputeShaderResource> textures; vector<data:ComputeShaderBuiltinSampler> builtinSamplers; vector<data:ComputeShaderResource> inBuffers; vector<data:ComputeShaderResource> outBuffers; vector<data:UInt8> code; vector<data:unsigned int>~ threadGroupSize; SInt64 requirements
ComputeShaderKernelParent: string name; vector<data:ComputeShaderKernel> uniqueVariants; vector<data:pair(string,unsigned int)> variantIndices; vector<data:string> globalKeywords; vector<data:string> localKeywords; vector<data:string> dynamicKeywords
ComputeShaderParam: string name; int type; unsigned int offset; unsigned int arraySize; unsigned int rowCount; unsigned int colCount
ComputeShaderCB: string name; int byteSize; vector<data:ComputeShaderParam> params
ComputeShaderPlatformVariant: int targetRenderer; int targetLevel; vector<data:ComputeShaderKernelParent> kernels; vector<data:ComputeShaderCB> constantBuffers; bool! resourcesResolved
ComputeShader: string m_Name; vector<data:ComputeShaderPlatformVariant> variants
'''
# Unity 2021.3.18f1 (Shin chan: Shiro and the Coal Town, PR #440): checked against all 144 nodes and alignment flags
# of AssetRipper's StructsDump/release/2021.3.18f1.dump. Code is bytes in both layouts; 2021.3 stores keyword variants
# in a map and uses a staticvector for threadGroupSize. Keep resourcesResolved aligned between platform variants.
COMPUTE_2021_3 = COMPUTE_2022_3.replace(
    'vector<data:ComputeShaderKernel> uniqueVariants; vector<data:pair(string,unsigned int)> variantIndices',
    'map<data:pair(string,ComputeShaderKernel)>~ variantMap').replace(
    '; vector<data:string> dynamicKeywords', '').replace(
    'vector<data:unsigned int>~ threadGroupSize', 'staticvector<data:unsigned int>~ threadGroupSize')
# Unity 2019.4.27f1's release tree (AssetRipper TypeTreeDumps, StructsDump/release/2019.4.27f1.dump): kernels hold code directly, without keyword variants or requirements.
# Pokemon Brilliant Diamond's stripped built-ins use this hash; their GPU skinning dispatch needs these kernels. Checked against the dump's nodes and alignment flags.
COMPUTE_2019_4 = '''ComputeShaderResource: string name; string generatedName; int bindPoint; int samplerBindPoint; int texDimension
ComputeShaderBuiltinSampler: unsigned int sampler; int bindPoint
ComputeShaderKernel: string name; vector<data:ComputeShaderResource> cbs; vector<data:ComputeShaderResource> textures; vector<data:ComputeShaderBuiltinSampler> builtinSamplers; vector<data:ComputeShaderResource> inBuffers; vector<data:ComputeShaderResource> outBuffers; vector<data:UInt8> code; staticvector<data:unsigned int>~ threadGroupSize
ComputeShaderParam: string name; int type; int offset; int arraySize; int rowCount; int colCount
ComputeShaderCB: string name; int byteSize; vector<data:ComputeShaderParam> params
ComputeShaderVariant: int targetRenderer; int targetLevel; vector<data:ComputeShaderKernel> kernels; vector<data:ComputeShaderCB> constantBuffers; bool! resourcesResolved
ComputeShader: string m_Name; vector<data:ComputeShaderVariant> variants
'''
# Unity 2017.4.28f1's release tree (AssetRipper TypeTreeDumps, StructsDump/release/2017.4.28f1.dump, class 72):
# resources hold a ComputeBufferCounter instead of the later sampler / dimension fields; kernels already hold code directly.
# Return of the Obra Dinn's stripped built-ins use this hash; its internalskinning object (12 kernels) reads to its exact end.
COMPUTE_2017_4 = COMPUTE_2019_4.replace('int samplerBindPoint; int texDimension', 'ComputeBufferCounter counter') + \
    'ComputeBufferCounter: int bindpoint; int offset\n'
CLASS_COMPUTE_SHADER = 72
KNOWN_TREES = {(CLASS_SHADER, bytes.fromhex('ea424a5fc1e48db62144df956f9e1005')): ('Shader', SHADER_2022_3),
               (CLASS_SHADER, bytes.fromhex('2ec7223dce3af4232b14ae1441bd7999')): ('Shader', SHADER_6000_0),
               (CLASS_SHADER, bytes.fromhex('a30e3dcf6c6d25153d6f64e5e41caf4e')): ('Shader', SHADER_2020_3),
               (CLASS_SHADER, bytes.fromhex('8db760073e1392061e4a36995f0735af')): ('Shader', SHADER_2018_3),
               (CLASS_SHADER, bytes.fromhex('266d53113fa30d2b858f2768f92eaa14')): ('Shader', SHADER_2017_4),
               (CLASS_COMPUTE_SHADER, bytes.fromhex('bfdf63ab0ce84d5a2c765ee560d90fe7')): ('ComputeShader', COMPUTE_2022_3),
               (CLASS_COMPUTE_SHADER, bytes.fromhex('abd9135b8ce83d043fef4e9ec7f53366')): ('ComputeShader', COMPUTE_2021_3),
               (CLASS_COMPUTE_SHADER, bytes.fromhex('1a95529ac2eb1a97cd9d2a4356bd722f')): ('ComputeShader', COMPUTE_2019_4),
               (CLASS_COMPUTE_SHADER, bytes.fromhex('20291e020d10074c35d8286f5191593f')): ('ComputeShader', COMPUTE_2017_4)}

def _split_top(s, sep):   # s split at sep outside <> and ()
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<(': depth += 1
        elif ch in '>)': depth -= 1
        if ch == sep and not depth: out.append(cur); cur = ''
        else: cur += ch
    return out + [cur]

def tree_from_layout(root, layout):
    """a KNOWN_TREES entry → the type tree's root Node, as a serialized file with type trees would give it (types, names and alignment flags; byte sizes are not kept)"""
    structs = {}
    for line in layout.strip().split('\n'):
        name, body = line.split(': ', 1)
        structs[name] = [f.rsplit(' ', 1) for f in body.split('; ')]
    def node(t, name):
        flags = 0
        if t.endswith('!'): t, flags = t[:-1], 0x4000
        arr_align = 0x4000
        if t.endswith('~'): t, arr_align = t[:-1], 0
        def array(el):
            a = Node('Array', 'Array', -1, arr_align); a.kids = [Node('int', 'size', 4, 0), el]; return a
        if t == 'string':
            n = Node('string', name, -1, flags); n.kids = [array(Node('char', 'data', 1, 0))]
        elif t.startswith(('vector<', 'map<', 'staticvector<')):
            kind, inner = t[:-1].split('<', 1); el_name, el_type = inner.split(':', 1)
            n = Node(kind, name, -1, flags); n.kids = [array(node(el_type, el_name))]
        elif t.startswith('pair('):
            first, second = _split_top(t[5:-1], ',')
            n = Node('pair', name, -1, flags); n.kids = [node(first, 'first'), node(second, 'second')]
        elif t in structs:
            n = Node(t, name, -1, flags); n.kids = [node(ft, fn) for ft, fn in structs[t]]
        elif t in PRIM: n = Node(t, name, struct.calcsize(PRIM[t]), flags)
        else: raise ValueError(f'type tree layout: unknown type {t}')
        return n
    return node(root, 'Base')

_KNOWN = {}
def known_tree(cid, h):
    if (cid, h) not in _KNOWN: _KNOWN[cid, h] = tree_from_layout(*KNOWN_TREES[cid, h])
    return _KNOWN[cid, h]

def serialized_objects(d, common, want=None, trees=None):
    """serialized file → yields (class ID, path ID, object dict); want = only decode these class IDs.
    trees = {(class ID, type hash): type tree}: files with type trees record theirs, files without (the game's own data.unity3d and so on) borrow one of the same class and hash -- the same hash means the same layout"""
    le, doff, tlist, objs = _tables(d, common, want, trees)
    for pid, start, size, ti in objs:
        cid, h, root = tlist[ti]
        if want and cid not in want: continue
        if root is None: raise NoTree(f'class {cid} (type hash {h.hex()}) has no type tree available')
        o = Reader(d, le); o.d = d[doff + start:doff + start + size]
        yield cid, pid, _read(o, root)

CLASS_TEXT_ASSET = 49

def text_assets(d, common):
    """serialized file → yields (path ID, name, script bytes) of its TextAsset objects (class 49). The object starts with the same two fields in every Unity version these tools read
    (m_Name, then m_Script: an aligned string each), so it is read without a type tree, also from files that carry none"""
    le, doff, tlist, objs = _tables(d, common, {CLASS_TEXT_ASSET}, None)
    for pid, start, size, ti in objs:
        if tlist[ti][0] != CLASS_TEXT_ASSET: continue
        r = Reader(d, le); r.d = d[doff + start:doff + start + size]
        if len(r.d) < 8: continue
        ln = r.u('i')
        # A TextAsset is m_Name then m_Script, two aligned strings. The first 4 bytes are the name's length (already read), so the name must leave 4 more
        # for the script's length field after it: the -8 is those two 4-byte length fields. Anything else is not this layout, so leave it out.
        if not 0 <= ln <= len(r.d) - 8: continue
        name = r.d[r.p:r.p + ln].decode('utf-8', 'replace'); r.p += ln; r.align()
        if r.p + 4 > len(r.d): continue   # the name's padding left no room for the script's length
        ln = r.u('i')
        if not 0 <= ln <= len(r.d) - r.p: continue
        yield pid, name, r.d[r.p:r.p + ln]

def _tables(d, common, want, trees):
    """serialized file → (little-endian, data offset, [(class ID, type hash, type tree or None)], [(path ID, start, size, type index)]): its type table and object table"""
    msz, fsz, ver, doff = struct.unpack_from('>IIII', d, 0)
    if not 17 <= ver <= 22: raise ValueError(f'serialized file format {ver} is not supported')
    p = 16; le = d[p] == 0; p += 4
    if ver >= 22: msz, fsz, doff, _ = struct.unpack_from('>Iqqq', d, p); p += 28
    r = Reader(d, le); r.p = p
    _, r.p = _cstr(d, r.p); r.u('i'); tt = r.u('?')
    tlist = []
    for _ in range(r.u('i')):
        cid = r.u('i'); r.u('B'); r.u('h')
        if cid == 114: r.p += 16   # script ID
        h = d[r.p:r.p + 16]; r.p += 16
        root = None
        if tt:
            nn, sb = r.u('i'), r.u('i'); raw = [r.u('HBBIIiiiQ' if ver >= 19 else 'HBBIIiii') for _ in range(nn)]   # refTypeHash from format 19
            buf = d[r.p:r.p + sb]; r.p += sb
            def st(o):
                if o & 0x80000000: return common.get(o & 0x7fffffff, f'?{o & 0x7fffffff}')
                return buf[o:buf.index(b'\0', o)].decode('utf-8', 'replace')
            stack = []
            for _v, lv, _tf, to, no, bs, _ix, mf, *_ in raw:
                nd = Node(st(to), st(no), bs, mf)
                del stack[lv:]
                if stack: stack[-1].kids.append(nd)
                else: root = nd
                stack.append(nd)
            if ver >= 21: n = r.u('i'); r.p += 4 * n   # type dependencies
            if trees is not None and (want is None or cid in want): trees.setdefault((cid, h), root)
        elif trees is not None: root = trees.get((cid, h))
        tlist.append((cid, h, root))
    objs = []
    for _ in range(r.u('i')):
        r.align(); pid = r.u('q'); start = r.u('q') if ver >= 22 else r.u('I'); size = r.u('I'); ti = r.u('i')
        objs.append((pid, start, size, ti))
    return le, doff, tlist, objs

def is_serialized(d):
    """does it look like a serialized file (big-endian header: format 17..22; from 22 the sizes follow +0x14)"""
    if len(d) < 48: return False
    ver = struct.unpack_from('>I', d, 8)[0]
    if not 17 <= ver <= 22 or d[16] not in (0, 1): return False
    if ver >= 22: msz, fsz, doff = struct.unpack_from('>Iqq', d, 20)
    else: msz, fsz, _, doff = struct.unpack_from('>IIII', d, 0)
    return fsz == len(d) and 0 < msz < fsz and doff <= fsz

def shader_programs(sh, only=None):
    """Shader object → yields (platform, subprogram index, subprogram bytes); only = the subprogram indices to yield (None: all)"""
    blob = sh['compressedBlob']
    for i, plat in enumerate(sh['platforms']):
        offs, clen, dlen = sh['offsets'][i], sh['compressedLengths'][i], sh['decompressedLengths'][i]
        if not isinstance(offs, list):   # before 2019.3: one segment per platform, 8-byte entries {offset, length}
            s0 = _lz4(blob[offs:offs + clen], dlen); n = struct.unpack_from('<I', s0, 0)[0]
            for k in range(n):
                off, ln = struct.unpack_from('<II', s0, 4 + 8 * k)
                if only is None or k in only: yield plat, k, s0[off:off + ln]
            continue
        segs = [_lz4(blob[o:o + c], u) for o, c, u in zip(offs, clen, dlen)]
        if not segs: continue
        s0 = segs[0]; n = struct.unpack_from('<I', s0, 0)[0]
        for k in range(n):
            off, ln, seg = struct.unpack_from('<III', s0, 4 + 12 * k)
            if only is None or k in only: yield plat, k, segs[seg][off:off + ln]

def subprogram(b):
    """subprogram header → (version, program type, keywords, local keywords, program code, offset after the code)"""
    r = Reader(b, True)
    ver, ptype = r.u('i'), r.u('i'); r.p += 12
    if ver >= 201608170: r.p += 4
    def strs():
        out = []
        for _ in range(r.u('i')):
            ln = r.u('i'); out.append(b[r.p:r.p + ln].decode('utf-8', 'replace')); r.p += ln; r.align()
        return out
    kw = strs(); lkw = strs() if 201806140 <= ver < 202012090 else []   # subprogram version 202012090 (Unity 2021.2 on) has no local keyword list (Ace Attorney Investigations Collection, Unity 2022.3)
    ln = r.u('i'); code = b[r.p:r.p + ln]; r.p += ln; r.align()
    return ver, ptype, kw, lkw, code, r.p
