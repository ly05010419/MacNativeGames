#include "symbols.h"
#include <string.h>

static TmntSymbolStatus address_of(const TmntSymbolModule *module, const TmntSymbol *symbol, uintptr_t *out) {
    if (symbol->absolute) { *out = symbol->value; return TMNT_SYMBOL_FOUND; }
    if (symbol->value >= module->image_size || symbol->value > UINTPTR_MAX - module->base) return TMNT_SYMBOL_INVALID;
    *out = module->base + symbol->value;
    return TMNT_SYMBOL_FOUND;
}

TmntSymbolStatus tmnt_bind_symbol(const TmntSymbolModule *modules, size_t count,
                                  size_t owner, const TmntSymbol *symbol, uintptr_t *address) {
    if (!modules || owner >= count || !symbol || !address || !symbol->name || !modules[owner].loaded ||
        symbol->binding > 2 || symbol->visibility > 3) return TMNT_SYMBOL_INVALID;
    if (symbol->defined && (symbol->binding == 0 || symbol->visibility != 0 || !*symbol->name))
        return address_of(&modules[owner], symbol, address);
    for (size_t i = 0; i < count && *symbol->name; i++) {
        const TmntSymbolModule *m = &modules[i];
        if (!m->loaded) continue;
        if (m->count && !m->symbols) return TMNT_SYMBOL_INVALID;
        for (size_t j = 0; j < m->count; j++) {
            const TmntSymbol *candidate = &m->symbols[j];
            if (!candidate->defined || !candidate->name || candidate->binding == 0 ||
                candidate->binding > 2 || candidate->visibility == 1 || candidate->visibility == 2 ||
                candidate->visibility > 3 || strcmp(symbol->name, candidate->name)) continue;
            return address_of(m, candidate, address);
        }
    }
    if (symbol->defined) return address_of(&modules[owner], symbol, address);
    return TMNT_SYMBOL_MISSING;
}
