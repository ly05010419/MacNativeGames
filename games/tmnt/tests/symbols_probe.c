#include "symbols.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
    TmntSymbol a[] = {{"shared", 0x10, 1, 1, 0, 0}, {"protected", 0x18, 1, 1, 3, 0}, {"hidden", 0x20, 1, 1, 2, 0}};
    TmntSymbol b[] = {{"shared", 0x30, 1, 1, 0, 0}, {"protected", 0x38, 1, 1, 3, 0}, {"hidden", 0x40, 1, 1, 0, 0}, {"absolute", 123, 1, 1, 0, 1}};
    TmntSymbolModule m[] = {{0x1000, 0x100, a, 3, 1}, {0x2000, 0x100, b, 4, 1}};
    uintptr_t out=0;
    assert(tmnt_bind_symbol(m,2,1,&b[0],&out)==TMNT_SYMBOL_FOUND && out==0x1010);
    assert(tmnt_bind_symbol(m,2,1,&b[1],&out)==TMNT_SYMBOL_FOUND && out==0x2038);
    assert(tmnt_bind_symbol(m,2,1,&b[2],&out)==TMNT_SYMBOL_FOUND && out==0x2040);
    TmntSymbol ref={"protected",0,0,1,0,0};
    assert(tmnt_bind_symbol(m,2,1,&ref,&out)==TMNT_SYMBOL_FOUND && out==0x1018);
    ref.name="hidden";
    assert(tmnt_bind_symbol(m,2,0,&ref,&out)==TMNT_SYMBOL_FOUND && out==0x2040);
    TmntSymbol local={"shared",0x50,1,0,0,0};
    assert(tmnt_bind_symbol(m,2,1,&local,&out)==TMNT_SYMBOL_FOUND && out==0x2050);
    ref.name="absolute";
    assert(tmnt_bind_symbol(m,2,0,&ref,&out)==TMNT_SYMBOL_FOUND && out==123);
    m[0].loaded=0;
    assert(tmnt_bind_symbol(m,2,1,&b[0],&out)==TMNT_SYMBOL_FOUND && out==0x2030);
    ref.name="missing"; out=99;
    assert(tmnt_bind_symbol(m,2,1,&ref,&out)==TMNT_SYMBOL_MISSING && out==99);
    b[0].value=0x100; assert(tmnt_bind_symbol(m,2,1,&b[0],&out)==TMNT_SYMBOL_INVALID && out==99);
    b[0].value=0x30; m[1].base=UINTPTR_MAX-0x10;
    assert(tmnt_bind_symbol(m,2,1,&b[0],&out)==TMNT_SYMBOL_INVALID && out==99);
    return 0;
}
