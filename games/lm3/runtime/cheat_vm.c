#include "cheat_vm.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Encodings follow Atmosphère's dmnt cheat VM (stratosphere/dmnt/source/cheat/impl/dmnt_cheat_vm.cpp). Nibbles are
   numbered from the most significant: in "5TMRI0AA" nibble 1 is T (width), 2 M (memory type), 3 R (register)... */
#define NIB(x, i) (((x) >> (28 - 4 * (i))) & 0xF)

static int set_err(char *err, size_t errn, const char *fmt, unsigned a, uint32_t b) {
    if (err && errn) snprintf(err, errn, fmt, a, b);
    return 0;
}

int cheat_parse(const char *s, Cheat *out, int max, char *err, size_t errn) {
    int count = 0;
    Cheat *cur = NULL;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (*s == '[' || *s == '{') {
            char close = *s == '[' ? ']' : '}';
            const char *e = strchr(s + 1, close);
            if (!e) { set_err(err, errn, "unterminated section name at character %u", 0, 0); return -1; }
            if (count == max) { set_err(err, errn, "more than %u cheats", (unsigned)max, 0); return -1; }
            cur = &out[count++];
            memset(cur, 0, sizeof *cur);
            size_t len = (size_t)(e - s - 1);
            if (len >= sizeof cur->name) len = sizeof cur->name - 1;
            memcpy(cur->name, s + 1, len);
            cur->master = close == '}';
            s = e + 1;
            continue;
        }
        unsigned digits = 0;
        uint32_t v = 0;
        while (isxdigit((unsigned char)s[digits]) && digits < 9) {
            char c = s[digits];
            v = v << 4 | (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            digits++;
        }
        if (digits != 8 || (s[8] && !isspace((unsigned char)s[8]))) {
            set_err(err, errn, "cheat %u: expected an 8-digit hex word", (unsigned)count, 0);
            return -1;
        }
        if (!cur) { set_err(err, errn, "instruction words before the first [name]%s", 0, 0); return -1; }
        if (cur->n == CHEAT_MAX_WORDS) { set_err(err, errn, "cheat %u: more than %u words", (unsigned)count, CHEAT_MAX_WORDS); return -1; }
        cur->w[cur->n++] = v;
        s += 8;
    }
    return count;
}

unsigned cheat_insn_len(const uint32_t *w, unsigned n, unsigned i) {
    if (i >= n) return 0;
    uint32_t x = w[i];
    unsigned t = NIB(x, 1), len;
    switch (NIB(x, 0)) {
    case 0x0: case 0x1: len = t == 8 ? 4 : 3; break;
    case 0x2: len = 1; break;
    case 0x3: len = t == 0 ? 2 : 1; break;   // 300R0000 VVVVVVVV start / 310R0000 end
    case 0x4: len = 3; break;
    case 0x5: len = 2; break;
    case 0x6: len = 3; break;
    case 0x7: len = 2; break;
    case 0x8: len = 1; break;
    case 0x9: len = NIB(x, 5) ? (t == 8 ? 3 : 2) : 1; break;
    case 0xA: { unsigned o = NIB(x, 5); len = (o == 2 || o == 4 || o == 5) ? 2 : 1; break; }
    case 0xC:
        if (t != 0) { len = 1; break; }   // C1 / C2 / C3: one word
        switch (NIB(x, 5)) {
        case 0: case 2: len = 2; break;
        case 1: case 3: case 5: len = 1; break;
        case 4: len = NIB(x, 2) == 8 ? 3 : 2; break;
        default: return 0;
        }
        break;
    case 0xF: len = 1; break;
    default: return 0;
    }
    return i + len <= n ? len : 0;
}

static int width_ok(unsigned t) { return t == 1 || t == 2 || t == 4 || t == 8; }
static uint64_t mask(uint64_t v, unsigned t) { return t == 8 ? v : v & ((1ull << (8 * t)) - 1); }
static int compare(uint64_t a, unsigned cond, uint64_t b, int *ok) {
    *ok = 1;
    switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: *ok = 0; return 0;
    }
}
static int arith(uint64_t a, unsigned op, uint64_t b, uint64_t *out) {
    switch (op) {
    case 0: *out = a + b; return 1;
    case 1: *out = a - b; return 1;
    case 2: *out = a * b; return 1;
    case 3: *out = b < 64 ? a << b : 0; return 1;
    case 4: *out = b < 64 ? a >> b : 0; return 1;
    case 5: *out = a & b; return 1;
    case 6: *out = a | b; return 1;
    case 7: *out = ~a; return 1;
    case 8: *out = a ^ b; return 1;
    case 9: *out = a; return 1;
    default: return 0;
    }
}

/* From the instruction after a failed condition (or a taken else), skips to the matching end (2 0) or, if stop_at_else,
   the matching else (2 1). Returns the index after it, or 0 when there is none. */
static unsigned skip_block(const Cheat *c, unsigned i, int stop_at_else) {
    int depth = 0;
    while (i < c->n) {
        unsigned len = cheat_insn_len(c->w, c->n, i);
        if (!len) return 0;
        uint32_t x = c->w[i];
        unsigned op = NIB(x, 0);
        if (op == 1 || op == 8 || (op == 0xC && NIB(x, 1) == 0)) depth++;
        else if (op == 2) {
            if (depth == 0 && (NIB(x, 1) == 0 || stop_at_else)) return i + len;
            if (NIB(x, 1) == 0) depth--;
        }
        i += len;
    }
    return 0;
}

int cheat_run(const Cheat *c, const CheatEnv *env, char *err, size_t errn) {
    uint64_t reg[16] = {0}, saved[16] = {0};
    unsigned loop_top[16] = {0};
    unsigned steps = 0, i = 0;
    while (i < c->n) {
        if (++steps > 1u << 20) return set_err(err, errn, "more than %u steps (endless loop?)%s", 1u << 20, 0);
        unsigned len = cheat_insn_len(c->w, c->n, i);
        if (!len) return set_err(err, errn, "malformed instruction %08x at word %u", c->w[i], i);
        const uint32_t *w = &c->w[i];
        uint32_t x = w[0];
        unsigned t = NIB(x, 1), next = i + len;
        switch (NIB(x, 0)) {
        case 0x0: {   // 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV): store static value to base(M) + reg[R] + address
            unsigned m = NIB(x, 2), r = NIB(x, 3);
            if (!width_ok(t) || m > 3) goto bad;
            uint64_t addr = env->base[m] + reg[r] + ((uint64_t)(x & 0xFF) << 32 | w[1]);
            uint64_t v = t == 8 ? (uint64_t)w[2] << 32 | w[3] : w[2];
            if (!env->write(env->ctx, addr, &v, t)) return set_err(err, errn, "write refused at word %u%s", i, 0);
            break;
        }
        case 0x1: {   // 1TMC00AA AAAAAAAA VVVVVVVV (VVVVVVVV): if [base(M) + address] <cond> value
            unsigned m = NIB(x, 2), cond = NIB(x, 3);
            if (!width_ok(t) || m > 3) goto bad;
            uint64_t v = 0, want = t == 8 ? (uint64_t)w[2] << 32 | w[3] : w[2];
            if (!env->read(env->ctx, env->base[m] + ((uint64_t)(x & 0xFF) << 32 | w[1]), &v, t)) return 0;
            int ok, pass = compare(mask(v, t), cond, mask(want, t), &ok);
            if (!ok) goto bad;
            if (!pass && !(next = skip_block(c, next, 1))) goto bad;
            break;
        }
        case 0x2:   // 20000000 end of conditional; 21000000 else: reached while executing, so skip the else branch
            if (t == 1 && !(next = skip_block(c, next, 0))) goto bad;
            break;
        case 0x3: {   // 300R0000 VVVVVVVV loop start (reg[R] = count) / 310R0000 loop end
            unsigned r = NIB(x, 3);
            if (t == 0) { if (!w[1]) goto bad; reg[r] = w[1]; loop_top[r] = next; }
            else if (!loop_top[r]) goto bad;
            else if (--reg[r]) next = loop_top[r];
            break;
        }
        case 0x4: reg[NIB(x, 3)] = (uint64_t)w[1] << 32 | w[2]; break;   // 400R0000 VVVVVVVV VVVVVVVV
        case 0x5: {   // 5TMRI0AA AAAAAAAA: reg[R] = [base(M) + address] (I=0) or [reg[R] + address] (I=1)
            unsigned m = NIB(x, 2), r = NIB(x, 3), from = NIB(x, 4);
            if (!width_ok(t) || m > 3 || from > 1) goto bad;
            uint64_t rel = (uint64_t)(x & 0xFF) << 32 | w[1], v = 0;
            if (!env->read(env->ctx, (from ? reg[r] : env->base[m]) + rel, &v, t)) return 0;
            reg[r] = mask(v, t);
            break;
        }
        case 0x6: {   // 6T0RIor0 VVVVVVVV VVVVVVVV: [reg[R] (+ reg[r])] = value; I: reg[R] += width
            unsigned r = NIB(x, 3), inc = NIB(x, 4), useofs = NIB(x, 5), ro = NIB(x, 6);
            if (!width_ok(t)) goto bad;
            uint64_t v = (uint64_t)w[1] << 32 | w[2];
            if (!env->write(env->ctx, reg[r] + (useofs ? reg[ro] : 0), &v, t)) return set_err(err, errn, "write refused at word %u%s", i, 0);
            if (inc) reg[r] += t;
            break;
        }
        case 0x7: {   // 7T0RC000 VVVVVVVV: reg[R] = reg[R] <op> value (legacy: add sub mul lsl lsr)
            unsigned r = NIB(x, 3), op = NIB(x, 4);
            if (!width_ok(t) || op > 4 || !arith(reg[r], op, w[1], &reg[r])) goto bad;
            reg[r] = mask(reg[r], t);
            break;
        }
        case 0x8:   // 8kkkkkkk: if every button in the mask is held
            if ((env->keys & (x & 0x0FFFFFFF)) != (x & 0x0FFFFFFF) && !(next = skip_block(c, next, 1))) goto bad;
            break;
        case 0x9: {   // 9TCRS0s0: reg[R] = reg[S] <op> reg[s]; 9TCRS100 VVVVVVVV (VVVVVVVV): reg[R] = reg[S] <op> value
            unsigned op = NIB(x, 2), r = NIB(x, 3), s = NIB(x, 4);
            if (!width_ok(t)) goto bad;
            uint64_t b = NIB(x, 5) ? (t == 8 ? (uint64_t)w[1] << 32 | w[2] : w[1]) : reg[NIB(x, 6)];
            if (!arith(reg[s], op, b, &reg[r])) goto bad;
            reg[r] = mask(reg[r], t);
            break;
        }
        case 0xA: {   // ATSRIOxa (aaaaaaaa): [address] = reg[S]; I: reg[R] += width
            unsigned s = NIB(x, 2), r = NIB(x, 3), inc = NIB(x, 4), o = NIB(x, 5), xx = NIB(x, 6);
            if (!width_ok(t)) goto bad;
            uint64_t rel = len == 2 ? (uint64_t)(x & 0xF) << 32 | w[1] : 0, addr;
            switch (o) {
            case 0: addr = reg[r]; break;
            case 1: addr = reg[r] + reg[xx]; break;
            case 2: addr = reg[r] + rel; break;
            case 3: if (xx > 3) goto bad; addr = env->base[xx] + reg[r]; break;
            case 4: if (xx > 3) goto bad; addr = env->base[xx] + rel; break;
            case 5: if (xx > 3) goto bad; addr = env->base[xx] + reg[r] + rel; break;
            default: goto bad;
            }
            uint64_t v = reg[s];
            if (!env->write(env->ctx, addr, &v, t)) return set_err(err, errn, "write refused at word %u%s", i, 0);
            if (inc) reg[r] += t;
            break;
        }
        case 0xC:
            if (t == 0) {   // C0TcSX..: if reg[S] <cond> operand
                unsigned wt = NIB(x, 2), cond = NIB(x, 3), s = NIB(x, 4), kind = NIB(x, 5), a = NIB(x, 6), b = NIB(x, 7);
                if (!width_ok(wt)) goto bad;
                uint64_t rel = len == 2 && kind != 4 ? (uint64_t)b << 32 | w[1] : 0, v = 0;
                switch (kind) {
                case 0: if (a > 3 || !env->read(env->ctx, env->base[a] + rel, &v, wt)) return 0; break;
                case 1: if (a > 3 || !env->read(env->ctx, env->base[a] + reg[b], &v, wt)) return 0; break;
                case 2: if (!env->read(env->ctx, reg[a] + rel, &v, wt)) return 0; break;
                case 3: if (!env->read(env->ctx, reg[a] + reg[b], &v, wt)) return 0; break;
                case 4: v = wt == 8 ? (uint64_t)w[1] << 32 | w[2] : w[1]; break;
                case 5: v = reg[a]; break;
                default: goto bad;
                }
                int ok, pass = compare(mask(reg[s], wt), cond, mask(v, wt), &ok);
                if (!ok) goto bad;
                if (!pass && !(next = skip_block(c, next, 1))) goto bad;
            } else if (t == 1) {   // C10D0Sx0: save (x=1) / restore (x=0) one register; x=2/3 clear
                unsigned d = NIB(x, 3), s = NIB(x, 5), op = NIB(x, 6);
                if (op == 0) reg[d] = saved[s]; else if (op == 1) saved[d] = reg[s]; else if (op == 2) saved[d] = 0; else if (op == 3) reg[d] = 0; else goto bad;
            } else if (t == 2) {   // C2x0XXXX: the same for every register in the mask
                unsigned op = NIB(x, 2);
                for (unsigned k = 0; k < 16; k++) if (x >> k & 1) {
                    if (op == 0) reg[k] = saved[k]; else if (op == 1) saved[k] = reg[k]; else if (op == 2) saved[k] = 0; else if (op == 3) reg[k] = 0; else goto bad;
                }
            } else goto bad;   // C3 (static registers) is not supported
            break;
        case 0xF:   // FF0 pause / FF1 resume / FFF debug log: nothing to do here
            break;
        default:
            goto bad;
        }
        if (!next) goto bad;
        i = next;
    }
    return 1;
bad:
    return set_err(err, errn, "word %u: unsupported or malformed instruction %08x", i, i < c->n ? c->w[i] : 0);
}
