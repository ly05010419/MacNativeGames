#include "runtime/pe_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace recomp {

namespace {

constexpr uint32_t kScnMemWrite = 0x80000000;
constexpr uint16_t kFileDll = 0x2000;

template <typename T>
T Rd(const std::vector<uint8_t>& d, size_t off) {
  T v{};
  if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
  return v;
}

Prot SectionProt(uint32_t ch) {
  // Guest code is lifted, never executed, so .text only needs to be
  // readable (jump tables and constants live there).
  return (ch & kScnMemWrite) ? Prot::RW : Prot::R;
}

std::string CStr(const GuestMemory& mem, gaddr va) {
  return std::string(mem.ptr<const char>(va));
}

}  // namespace

bool PeImage::Load(const std::string& path, GuestMemory& mem,
                   std::string* err, gaddr base, const ImportBinder& bind) {
  *this = PeImage{};
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

  if (Rd<uint16_t>(d, 0) != 0x5A4D) {
    *err = "missing MZ header";
    return false;
  }
  uint32_t pe = Rd<uint32_t>(d, 0x3C);
  uint16_t machine = Rd<uint16_t>(d, pe + 4);
  if (Rd<uint32_t>(d, pe) != 0x00004550 || (machine != 0x14C && machine != 0x8664)) {
    *err = "not a PE32 i386 or PE32+ AMD64 image";
    return false;
  }
  x64 = machine == 0x8664;
  uint16_t nsec = Rd<uint16_t>(d, pe + 6);
  uint16_t opt_size = Rd<uint16_t>(d, pe + 20);
  size_t oh = pe + 24;
  if (Rd<uint16_t>(d, oh) != (x64 ? 0x20B : 0x10B)) {
    *err = x64 ? "optional header is not PE32+" : "optional header is not PE32";
    return false;
  }
  dll = (Rd<uint16_t>(d, pe + 22) & kFileDll) != 0;
  entry = Rd<uint32_t>(d, oh + 16);
  // PE32+ widens ImageBase and the stack and heap sizes to 64 bits (and
  // drops BaseOfData), which moves the data directories by 16 bytes.
  preferred_base = x64 ? Rd<uint64_t>(d, oh + 24) : Rd<uint32_t>(d, oh + 28);
  image_base = base ? base : preferred_base;
  image_size = Rd<uint32_t>(d, oh + 56);
  uint32_t headers_size = Rd<uint32_t>(d, oh + 60);
  stack_reserve = x64 ? Rd<uint64_t>(d, oh + 72) : Rd<uint32_t>(d, oh + 72);
  if (entry) entry += image_base;  // DLLs may have no entry point
  const size_t dirs = oh + (x64 ? 112 : 96);
  auto dir = [&](int i) {
    return std::pair<uint32_t, uint32_t>{Rd<uint32_t>(d, dirs + i * 8),
                                         Rd<uint32_t>(d, dirs + 4 + i * 8)};
  };
  const uint32_t ptr_size = x64 ? 8 : 4;
  auto rd_ptr = [&](gaddr va) -> uint64_t {
    return x64 ? mem.read<uint64_t>(va) : mem.read<uint32_t>(va);
  };

  if (image_base + image_size > mem.size()) {
    *err = "image does not fit in the guest arena";
    return false;
  }
  for (uint32_t a = 0; a < image_size; a += GuestMemory::kPageSize)
    if (mem.IsCommitted(image_base + a)) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "image range %08llX-%08llX is already in use",
                    (unsigned long long)image_base,
                    (unsigned long long)(image_base + image_size));
      *err = buf;
      return false;
    }

  // Map everything RW first so imports can be patched, then tighten.
  if (!mem.Commit(image_base, image_size, Prot::RW)) {
    *err = "cannot commit image range";
    return false;
  }
  std::memcpy(mem.ptr(image_base), d.data(),
              std::min<size_t>(headers_size, d.size()));

  size_t sh = oh + opt_size;
  for (uint16_t i = 0; i < nsec; i++) {
    size_t o = sh + i * 40;
    char name[9] = {};
    std::memcpy(name, d.data() + o, 8);
    uint32_t vsize = Rd<uint32_t>(d, o + 8);
    uint32_t va = Rd<uint32_t>(d, o + 12);
    uint32_t rsize = Rd<uint32_t>(d, o + 16);
    uint32_t rptr = Rd<uint32_t>(d, o + 20);
    uint32_t ch = Rd<uint32_t>(d, o + 36);
    uint32_t n = std::min(vsize ? vsize : rsize, rsize);
    if (rptr + n > d.size()) {
      *err = std::string("section ") + name + " runs past end of file";
      return false;
    }
    std::memcpy(mem.ptr(image_base + va), d.data() + rptr, n);
    sections.push_back({name, image_base + va, vsize, ch});
  }

  // Base relocations: add the load delta to every HIGHLOW (and, in x64
  // images, DIR64) fixup.
  if (image_base != preferred_base) {
    auto [rel_rva, rel_size] = dir(5);
    if (!rel_size) {
      *err = "image has no relocations and cannot move from its preferred base";
      return false;
    }
    const uint64_t delta = image_base - preferred_base;
    for (gaddr blk = image_base + rel_rva; blk < image_base + rel_rva + rel_size;) {
      uint32_t page = mem.read<uint32_t>(blk), size = mem.read<uint32_t>(blk + 4);
      if (size < 8) break;
      for (uint32_t i = 8; i + 2 <= size; i += 2) {
        uint16_t e = mem.read<uint16_t>(blk + i);
        gaddr at = image_base + page + (e & 0xFFF);
        switch (e >> 12) {
          case 0:  // IMAGE_REL_BASED_ABSOLUTE: padding
            break;
          case 3:  // IMAGE_REL_BASED_HIGHLOW
            mem.write<uint32_t>(at, uint32_t(mem.read<uint32_t>(at) + delta));
            break;
          case 10:  // IMAGE_REL_BASED_DIR64
            mem.write<uint64_t>(at, mem.read<uint64_t>(at) + delta);
            break;
          default:
            *err = "unsupported base relocation type " + std::to_string(e >> 12);
            return false;
        }
      }
      blk += size;
    }
  }

  // Imports: point every IAT slot at its thunk address.
  const gaddr thunk_base = x64 ? kThunkBase64 : kThunkBase;
  if (!bind && !mem.Commit(thunk_base, kThunkRegionSize, Prot::R)) {
    *err = "cannot commit thunk region";
    return false;
  }
  auto [imp_rva, imp_size] = dir(1);
  for (gaddr desc = image_base + imp_rva; imp_size; desc += 20) {
    uint32_t ilt = mem.read<uint32_t>(desc);
    uint32_t name_rva = mem.read<uint32_t>(desc + 12);
    uint32_t iat = mem.read<uint32_t>(desc + 16);
    if (!name_rva) break;
    std::string dll = CStr(mem, image_base + name_rva);
    gaddr lookup = image_base + (ilt ? ilt : iat);
    for (gaddr slot = image_base + iat;; slot += ptr_size, lookup += ptr_size) {
      uint64_t v = rd_ptr(lookup);
      if (!v) break;
      const uint64_t by_ordinal = x64 ? 1ull << 63 : 1ull << 31;
      std::string sym = (v & by_ordinal)
                            ? "#" + std::to_string(v & 0xFFFF)
                            : CStr(mem, image_base + uint32_t(v) + 2);
      gaddr thunk = bind ? bind(dll, sym)
                         : thunk_base + uint32_t(imports.size()) * kThunkStride;
      if (!bind && thunk >= thunk_base + kThunkRegionSize) {
        *err = "thunk region exhausted";
        return false;
      }
      if (x64)
        mem.write<uint64_t>(slot, thunk);
      else
        mem.write<uint32_t>(slot, uint32_t(thunk));
      imports.push_back({dll, sym, slot, thunk});
    }
  }

  auto [exp_rva, exp_size] = dir(0);
  if (exp_size) {
    gaddr e = image_base + exp_rva;
    uint32_t ord_base = mem.read<uint32_t>(e + 16);
    uint32_t nfuncs = mem.read<uint32_t>(e + 20), nnames = mem.read<uint32_t>(e + 24);
    gaddr funcs = image_base + mem.read<uint32_t>(e + 28);
    gaddr names = image_base + mem.read<uint32_t>(e + 32);
    gaddr ords = image_base + mem.read<uint32_t>(e + 36);
    std::vector<std::string> by_index(nfuncs);
    for (uint32_t i = 0; i < nnames; i++) {
      uint16_t idx = mem.read<uint16_t>(ords + 2 * i);
      if (idx < nfuncs)
        by_index[idx] = CStr(mem, image_base + mem.read<uint32_t>(names + 4 * i));
    }
    for (uint32_t i = 0; i < nfuncs; i++) {
      uint32_t rva = mem.read<uint32_t>(funcs + 4 * i);
      if (!rva || (rva >= exp_rva && rva < exp_rva + exp_size))
        continue;  // unused slot, or a forwarder string
      exports.push_back({by_index[i], ord_base + i, image_base + rva});
    }
  }

  auto [tls_rva, tls_size] = dir(9);
  if (tls_size) {
    // IMAGE_TLS_DIRECTORY: four VAs, then SizeOfZeroFill.
    gaddr t = image_base + tls_rva;
    tls.raw_start = rd_ptr(t);
    tls.raw_end = rd_ptr(t + ptr_size);
    tls.index_va = rd_ptr(t + 2 * ptr_size);
    tls.callbacks = rd_ptr(t + 3 * ptr_size);
    tls.zero_fill = mem.read<uint32_t>(t + 4 * ptr_size);
    has_tls = true;
  }

  // x64 exception directory: RUNTIME_FUNCTION {begin, end, unwind} RVAs.
  // An unwind RVA with bit 0 set points at the parent's RUNTIME_FUNCTION
  // (a chain in short form).
  auto [exc_rva, exc_size] = dir(3);
  if (x64)
    for (uint32_t o = 0; o + 12 <= exc_size; o += 12) {
      gaddr e = image_base + exc_rva + o;
      uint32_t b = mem.read<uint32_t>(e), en = mem.read<uint32_t>(e + 4);
      uint32_t u = mem.read<uint32_t>(e + 8);
      if (!b) continue;
      uint8_t fl = (u & 1) ? 4 : mem.read<uint8_t>(image_base + u) >> 3;
      functions.push_back({e, image_base + b, image_base + en, image_base + (u & ~1u), fl});
    }

  mem.Protect(image_base, headers_size, Prot::R);
  for (const auto& s : sections)
    mem.Protect(s.va, s.vsize, SectionProt(s.characteristics));
  return true;
}

bool PeImage::IsX64File(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> d(0x1000);
  f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
  d.resize(size_t(f.gcount()));
  uint32_t pe = Rd<uint32_t>(d, 0x3C);
  return Rd<uint16_t>(d, 0) == 0x5A4D && Rd<uint32_t>(d, pe) == 0x00004550 &&
         Rd<uint16_t>(d, pe + 4) == 0x8664;
}

}  // namespace recomp
