#ifndef PRX_LIBC_INCLUDE_SPECIFICS_LINUX_ELFTYPES_HPP
#define PRX_LIBC_INCLUDE_SPECIFICS_LINUX_ELFTYPES_HPP

#ifdef __linux__
#include <link.h>
#else

#include <cstdint>
#include <cstddef>

struct Elf64_Phdr {
    std::uint32_t p_type;
    std::uint32_t p_flags;
    std::uint64_t p_offset;
    std::uint64_t p_vaddr;
    std::uint64_t p_paddr;
    std::uint64_t p_filesz;
    std::uint64_t p_memsz;
    std::uint64_t p_align;
};

struct Elf64_Dyn {
    std::int64_t d_tag;
    union {
        std::uint64_t d_val;
        std::uint64_t d_ptr;
    } d_un;
};

#define ElfW(type) Elf64_##type

struct dl_phdr_info {
    std::uintptr_t dlpi_addr;
    const char* dlpi_name;
    const Elf64_Phdr* dlpi_phdr;
    std::uint16_t dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    std::size_t dlpi_tls_modid;
    void* dlpi_tls_data;
};

static constexpr std::uint32_t PT_LOAD = 1;
static constexpr std::uint32_t PT_DYNAMIC = 2;
static constexpr std::uint32_t PT_TLS = 7;
static constexpr std::uint32_t PT_GNU_EH_FRAME = 0x6474e550;
static constexpr std::uint32_t PF_X = 1;
static constexpr std::uint32_t PF_W = 2;
static constexpr std::uint32_t PF_R = 4;
static constexpr std::int64_t DT_NULL = 0;
static constexpr std::int64_t DT_INIT = 12;
static constexpr std::int64_t DT_FINI = 13;

#if defined(__APPLE__) && !defined(ANYPS5_GUEST_LIFTED)
extern "C" int dl_iterate_phdr(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data) __asm__("_dl_iterate_phdr_nid_no_patch");
#elif !defined(ANYPS5_GUEST_LIFTED)
extern "C" int dl_iterate_phdr(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data);
#endif

#endif

#ifdef ANYPS5_GUEST_LIFTED
#include <cstddef>
extern "C" int GuestCpuIterateImages_nid_no_patch(int (*callback)(dl_phdr_info*, std::size_t, void*), void* data);
#define dl_iterate_phdr GuestCpuIterateImages_nid_no_patch
#endif

#endif
