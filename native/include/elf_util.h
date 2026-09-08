/*
 * Derived from LSPosed/SandHook ElfImg (GPL-3.0-or-later).
 * Used only to resolve libart symbols required by LSPlant.
 */
#pragma once

#include <link.h>
#include <linux/elf.h>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef SHT_GNU_HASH
#define SHT_GNU_HASH 0x6ffffff6
#endif

namespace wastatushd {
class ElfImg {
public:
    explicit ElfImg(std::string_view elf_name);
    ~ElfImg();

    bool isValid() const { return base_ != nullptr && header_ != nullptr; }

    template <typename T = void *>
    requires(std::is_pointer_v<T>)
    T getSymbAddress(std::string_view name) const {
        auto offset = getSymbOffset(name, GnuHash(name), ElfHash(name));
        if (offset > 0 && base_) {
            return reinterpret_cast<T>(static_cast<ElfW(Addr)>(
                reinterpret_cast<uintptr_t>(base_) + offset - bias_));
        }
        return nullptr;
    }

    template <typename T = void *>
    requires(std::is_pointer_v<T>)
    T getSymbPrefixFirstAddress(std::string_view prefix) const {
        auto offset = PrefixLookupFirst(prefix);
        if (offset > 0 && base_) {
            return reinterpret_cast<T>(static_cast<ElfW(Addr)>(
                reinterpret_cast<uintptr_t>(base_) + offset - bias_));
        }
        return nullptr;
    }

private:
    bool findModuleBase();
    void MayInitLinearMap() const;
    ElfW(Addr) getSymbOffset(std::string_view name, uint32_t gnu_hash, uint32_t elf_hash) const;
    ElfW(Addr) ElfLookup(std::string_view name, uint32_t hash) const;
    ElfW(Addr) GnuLookup(std::string_view name, uint32_t hash) const;
    ElfW(Addr) LinearLookup(std::string_view name) const;
    ElfW(Addr) PrefixLookupFirst(std::string_view prefix) const;
    static constexpr uint32_t ElfHash(std::string_view name);
    static constexpr uint32_t GnuHash(std::string_view name);

    std::string elf_;
    void *base_ = nullptr;
    off_t size_ = 0;
    off_t bias_ = -4396;
    ElfW(Ehdr) *header_ = nullptr;
    ElfW(Shdr) *symtab_ = nullptr;
    ElfW(Sym) *symtab_start_ = nullptr;
    ElfW(Sym) *dynsym_start_ = nullptr;
    ElfW(Sym) *strtab_start_ = nullptr;
    ElfW(Off) symtab_count_ = 0;
    ElfW(Off) symstr_offset_for_symtab_ = 0;

    uint32_t nbucket_ = 0;
    uint32_t *bucket_ = nullptr;
    uint32_t *chain_ = nullptr;
    uint32_t gnu_nbucket_ = 0;
    uint32_t gnu_symndx_ = 0;
    uint32_t gnu_bloom_size_ = 0;
    uint32_t gnu_shift2_ = 0;
    uintptr_t *gnu_bloom_filter_ = nullptr;
    uint32_t *gnu_bucket_ = nullptr;
    uint32_t *gnu_chain_ = nullptr;

    mutable std::map<std::string_view, ElfW(Sym) *> symtabs_;
};

constexpr uint32_t ElfImg::ElfHash(std::string_view name) {
    uint32_t h = 0, g;
    for (unsigned char p : name) {
        h = (h << 4) + p;
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

constexpr uint32_t ElfImg::GnuHash(std::string_view name) {
    uint32_t h = 5381;
    for (unsigned char p : name) h += (h << 5) + p;
    return h;
}
} // namespace wastatushd
