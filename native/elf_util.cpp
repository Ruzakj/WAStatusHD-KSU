/*
 * Derived from LSPosed/SandHook ElfImg (GPL-3.0-or-later).
 */
#include "include/elf_util.h"

#include <android/log.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "WAStatusHD"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace wastatushd {

template <typename T>
inline constexpr auto offsetOf(ElfW(Ehdr) *head, ElfW(Off) off) {
    return reinterpret_cast<std::conditional_t<std::is_pointer_v<T>, T, T *>>(
        reinterpret_cast<uintptr_t>(head) + off);
}

ElfImg::ElfImg(std::string_view base_name) : elf_(base_name) {
    if (!findModuleBase()) return;

    int fd = open(elf_.c_str(), O_RDONLY);
    if (fd < 0) {
        LOGE("elf: failed to open %s", elf_.c_str());
        base_ = nullptr;
        return;
    }

    size_ = lseek(fd, 0, SEEK_END);
    if (size_ <= 0) {
        close(fd);
        base_ = nullptr;
        return;
    }

    header_ = reinterpret_cast<ElfW(Ehdr) *>(mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0));
    close(fd);
    if (header_ == MAP_FAILED) {
        header_ = nullptr;
        base_ = nullptr;
        return;
    }

    if (memcmp(header_->e_ident, ELFMAG, SELFMAG) != 0) {
        munmap(header_, size_);
        header_ = nullptr;
        base_ = nullptr;
        return;
    }

    auto *section_header = offsetOf<ElfW(Shdr) *>(header_, header_->e_shoff);
    uintptr_t shoff = reinterpret_cast<uintptr_t>(section_header);
    char *section_str = offsetOf<char *>(header_, section_header[header_->e_shstrndx].sh_offset);

    for (int i = 0; i < header_->e_shnum; ++i, shoff += header_->e_shentsize) {
        auto *section_h = reinterpret_cast<ElfW(Shdr) *>(shoff);
        char *sname = section_h->sh_name + section_str;
        const auto entsize = section_h->sh_entsize;
        switch (section_h->sh_type) {
            case SHT_DYNSYM:
                if (!dynsym_start_) dynsym_start_ = offsetOf<ElfW(Sym) *>(header_, section_h->sh_offset);
                break;
            case SHT_SYMTAB:
                if (strcmp(sname, ".symtab") == 0 && entsize != 0) {
                    symtab_ = section_h;
                    symtab_count_ = section_h->sh_size / entsize;
                    symtab_start_ = offsetOf<ElfW(Sym) *>(header_, section_h->sh_offset);
                }
                break;
            case SHT_STRTAB:
                if (!strtab_start_) strtab_start_ = offsetOf<ElfW(Sym) *>(header_, section_h->sh_offset);
                if (strcmp(sname, ".strtab") == 0) symstr_offset_for_symtab_ = section_h->sh_offset;
                break;
            case SHT_PROGBITS:
                if (strtab_start_ && dynsym_start_ && bias_ == -4396) {
                    bias_ = static_cast<off_t>(section_h->sh_addr) - static_cast<off_t>(section_h->sh_offset);
                }
                break;
            case SHT_HASH: {
                auto *d_un = offsetOf<ElfW(Word) *>(header_, section_h->sh_offset);
                nbucket_ = d_un[0];
                bucket_ = d_un + 2;
                chain_ = bucket_ + nbucket_;
                break;
            }
            case SHT_GNU_HASH: {
                auto *d_buf = reinterpret_cast<ElfW(Word) *>(reinterpret_cast<uintptr_t>(header_) + section_h->sh_offset);
                gnu_nbucket_ = d_buf[0];
                gnu_symndx_ = d_buf[1];
                gnu_bloom_size_ = d_buf[2];
                gnu_shift2_ = d_buf[3];
                gnu_bloom_filter_ = reinterpret_cast<uintptr_t *>(d_buf + 4);
                gnu_bucket_ = reinterpret_cast<uint32_t *>(gnu_bloom_filter_ + gnu_bloom_size_);
                gnu_chain_ = gnu_bucket_ + gnu_nbucket_ - gnu_symndx_;
                break;
            }
        }
    }
}

ElfImg::~ElfImg() {
    if (header_) munmap(header_, size_);
}

ElfW(Addr) ElfImg::ElfLookup(std::string_view name, uint32_t hash) const {
    if (!nbucket_ || !strtab_start_ || !dynsym_start_) return 0;
    const char *strings = reinterpret_cast<const char *>(strtab_start_);
    for (auto n = bucket_[hash % nbucket_]; n != 0; n = chain_[n]) {
        auto *sym = dynsym_start_ + n;
        if (name == strings + sym->st_name) return sym->st_value;
    }
    return 0;
}

ElfW(Addr) ElfImg::GnuLookup(std::string_view name, uint32_t hash) const {
    static constexpr auto bloom_mask_bits = sizeof(ElfW(Addr)) * 8;
    if (!gnu_nbucket_ || !gnu_bloom_size_ || !gnu_bloom_filter_ || !dynsym_start_ || !strtab_start_) return 0;

    auto bloom_word = gnu_bloom_filter_[(hash / bloom_mask_bits) % gnu_bloom_size_];
    uintptr_t mask = (uintptr_t{1} << (hash % bloom_mask_bits)) |
                     (uintptr_t{1} << ((hash >> gnu_shift2_) % bloom_mask_bits));
    if ((mask & bloom_word) != mask) return 0;

    auto sym_index = gnu_bucket_[hash % gnu_nbucket_];
    if (sym_index < gnu_symndx_) return 0;
    const char *strings = reinterpret_cast<const char *>(strtab_start_);
    do {
        auto *sym = dynsym_start_ + sym_index;
        if (((gnu_chain_[sym_index] ^ hash) >> 1) == 0 && name == strings + sym->st_name) {
            return sym->st_value;
        }
    } while ((gnu_chain_[sym_index++] & 1) == 0);
    return 0;
}

void ElfImg::MayInitLinearMap() const {
    if (!symtabs_.empty() || !symtab_start_ || !symstr_offset_for_symtab_) return;
    for (ElfW(Off) i = 0; i < symtab_count_; ++i) {
        unsigned int st_type = ELF_ST_TYPE(symtab_start_[i].st_info);
        const char *st_name = offsetOf<const char *>(header_, symstr_offset_for_symtab_ + symtab_start_[i].st_name);
        if ((st_type == STT_FUNC || st_type == STT_OBJECT) && symtab_start_[i].st_size) {
            symtabs_.emplace(st_name, &symtab_start_[i]);
        }
    }
}

ElfW(Addr) ElfImg::LinearLookup(std::string_view name) const {
    MayInitLinearMap();
    auto i = symtabs_.find(name);
    return i == symtabs_.end() ? 0 : i->second->st_value;
}

ElfW(Addr) ElfImg::PrefixLookupFirst(std::string_view prefix) const {
    MayInitLinearMap();
    auto i = symtabs_.lower_bound(prefix);
    if (i != symtabs_.end() && i->first.starts_with(prefix)) return i->second->st_value;
    return 0;
}

ElfW(Addr) ElfImg::getSymbOffset(std::string_view name, uint32_t gnu_hash, uint32_t elf_hash) const {
    if (auto off = GnuLookup(name, gnu_hash); off > 0) return off;
    if (auto off = ElfLookup(name, elf_hash); off > 0) return off;
    return LinearLookup(name);
}

bool ElfImg::findModuleBase() {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;

    char *buff = nullptr;
    size_t len = 0;
    ssize_t nread;
    bool found = false;
    uintptr_t load_addr = 0;

    while ((nread = getline(&buff, &len, maps)) != -1) {
        std::string_view line(buff, static_cast<size_t>(nread));
        if (line.find(elf_) == std::string_view::npos) continue;
        if (line.find("r-xp") == std::string_view::npos && line.find("r--p") == std::string_view::npos) continue;

        char *next = nullptr;
        load_addr = strtoull(buff, &next, 16);
        if (next == buff) continue;

        auto pos = line.find('/');
        if (pos != std::string_view::npos) {
            std::string path(line.substr(pos));
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
            elf_ = path;
            found = true;
            break;
        }
    }

    if (buff) free(buff);
    fclose(maps);
    if (!found || load_addr == 0) return false;
    base_ = reinterpret_cast<void *>(load_addr);
    LOGD("elf: libart=%s base=%p", elf_.c_str(), base_);
    return true;
}

} // namespace wastatushd
