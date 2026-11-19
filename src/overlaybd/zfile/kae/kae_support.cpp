/* kae_support.cpp — Kunpeng KAE hardware gate + runtime library loading. */

#ifdef ENABLE_KAE

#include "kae_support.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>

namespace ZFile {
namespace Kae {

/* 0 = unprobed; 1 = available; 2 = unavailable. Process-wide, same pattern as
 * g_qat_state in compressor.cpp. */
static std::atomic<int> g_kae_state{0};

#ifdef __aarch64__
/* Same detection as the KAE libraries themselves: MIDR_EL1 vendor 0x48
 * (Hisilicon), part in the set their constructors accept. On anything else the
 * library would abort() from its ELF constructor, so this must mirror
 * KAELz4/src/utils/kaelz4_common.c exactly. */
static bool cpu_supported() {
    unsigned long long midr;
    __asm__ volatile("mrs %0, MIDR_EL1" : "=r"(midr));
    unsigned long long vendor = (midr >> 24) & 0xFF;
    unsigned long long part = (midr >> 4) & 0xFFF;
    if (part == 0xD22 || part == 0xD06) {
        return true;
    }
    return vendor == 0x48 && (part == 0xD02 || part == 0xD03);
}
#endif

static bool zip_device_present() {
    DIR *d = opendir("/sys/class/uacce");
    if (d == nullptr) {
        return false;
    }
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (strncmp(e->d_name, "hisi_zip-", 9) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

bool support_available() {
    int cached = g_kae_state.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached == 1;
    }

#ifdef __aarch64__
    bool ok = cpu_supported() && zip_device_present();
#else
    bool ok = false;
#endif
    g_kae_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}

void *open_library(const char *path) {
    return dlopen(path, RTLD_LOCAL | RTLD_NOW);
}

void *open_library_flags(const char *path, int flags) {
    return dlopen(path, flags);
}

} // namespace Kae
} // namespace ZFile

#endif /* ENABLE_KAE */
