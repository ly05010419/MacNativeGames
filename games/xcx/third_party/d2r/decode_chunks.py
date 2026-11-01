"""Decode fixed-width AArch64 runs without losing code after an unknown word.

Undecodable words remain untouched in the original image and separate runs.
Consumers must never build an instruction window across a run boundary.
"""


def decode_chunks(disassembler, data, address):
    if address % 4 or len(data) % 4:
        raise ValueError("AArch64 input must be word aligned")
    chunks = []
    cursor = 0
    while cursor < len(data):
        chunk = list(disassembler.disasm(data[cursor:], address + cursor))
        if chunk:
            expected = address + cursor
            for instruction in chunk:
                if instruction.address != expected or instruction.size != 4:
                    raise ValueError("Unexpected AArch64 instruction layout")
                expected += 4
            chunks.append(chunk)
            cursor = expected - address
        if cursor < len(data):
            cursor += 4
    return chunks
