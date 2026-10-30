"""Image atomics run natively on the Metal texture (MSL 3.1 texture atomics).

SPIRV-Cross below MSL 3.1 emulates an image atomic with a raw device pointer
beside the texture (<image>_atomic), indexed by the bound texture's width: a
block-linear image is a deswizzled Metal copy whose pool memory has another
layout, so those atomics went to a scratch buffer that nothing read, and a 3D
image did not compile at all (an int3 subscript). Native texture atomics act on
the texture every other access of the image uses, for buffer, 2D, 2D-array and
3D images. The translator still guards each coordinate (IGN bounds) before the
atomic.

Only shaders with image atomics (OpImageTexelPointer) are written at MSL 3.1:
every other shader keeps its MSL 2.3 source byte for byte, so its library comes
from the MSL-keyed compile cache. The emulation must never come back silently:
check_msl fails the shader when SPIRV-Cross writes the pointer form anyway.
"""
import re
import struct

MSL_VERSION = '20300'
MSL_VERSION_ATOMICS = '30100'
OP_IMAGE_TEXEL_POINTER = 60


def uses_image_atomics(spv):
    """Whether the SPIR-V module (path) has an image texel pointer, the operand of every image atomic"""
    with open(spv, 'rb') as f:
        data = f.read()
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError('not a SPIR-V module: ' + spv)
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if op == OP_IMAGE_TEXEL_POINTER:
            return True
        if n == 0:
            raise ValueError('malformed SPIR-V instruction in ' + spv)
        i += n
    return False


def cross_args(xc, spv):
    """spirv-cross arguments for this module: xc with its --msl-version raised to 3.1 when the module has image atomics"""
    if not uses_image_atomics(spv):
        return list(xc)
    args = list(xc)
    args[args.index('--msl-version') + 1] = MSL_VERSION_ATOMICS
    return args


_EMULATED = re.compile(r'\bspvImage2DAtomicCoord\b|device\s+atomic_u?int\s*\*\s*\w+_atomic\b')


def check_msl(source):
    """Fail closed on SPIRV-Cross's pointer emulation of image atomics; return the source unchanged"""
    if _EMULATED.search(source):
        raise ValueError('image atomics emulated through a raw pointer; they must be native texture atomics (MSL 3.1)')
    return source
