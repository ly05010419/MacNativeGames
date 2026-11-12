#ifndef TMNT_SYMBOLS_H
#define TMNT_SYMBOLS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uintptr_t value;
    unsigned defined, binding, visibility, absolute;
} TmntSymbol;

typedef struct {
    uintptr_t base;
    size_t image_size;
    const TmntSymbol *symbols;
    size_t count;
    int loaded;
} TmntSymbolModule;

typedef enum { TMNT_SYMBOL_FOUND, TMNT_SYMBOL_MISSING, TMNT_SYMBOL_INVALID } TmntSymbolStatus;
/* Resolve a relocation against currently loaded modules in load order. Defined local,
   hidden, internal, and protected symbols bind to their own module. Default-visible
   definitions can be interposed; protected symbols can also be exported to others.
   A missing weak symbol is reported as MISSING and left to the relocation caller. */
TmntSymbolStatus tmnt_bind_symbol(const TmntSymbolModule *modules, size_t count,
                                  size_t owner, const TmntSymbol *symbol,
                                  uintptr_t *address);
#endif
