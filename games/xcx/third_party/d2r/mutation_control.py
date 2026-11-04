"""Observable negative controls for ordinary AArch64 rewrite blocks."""
import struct


def corrupt_plain_block(image, instruction_offset, exit_branch, index, mnemonic):
    # Prefetch has no architectural state effect in Unicorn: replacing it
    # with NOP is an equivalent program, not a valid negative control.
    prefetch = mnemonic in ('prfm', 'prfum')
    if (index % 2 or prefetch) and exit_branch is not None:
        offset, word = exit_branch
        if word & 0xfc000000 != 0x14000000:
            raise ValueError('Expected unconditional exit branch')
        struct.pack_into('<I', image, offset,
                         (word & 0xfc000000) | ((word + 1) & 0x3ffffff))
        return 'exit+4'
    if prefetch:
        raise ValueError('Prefetch mutation requires an exit branch')
    struct.pack_into('<I', image, instruction_offset, 0xd503201f)
    return 'nop'
