/* kae_zstd.cpp — KAEZstd API loading via dlopen + dlsym. */

#ifdef ENABLE_KAE

#include "kae_zstd.h"
#include "kae_support.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace ZFile {
namespace Kae {

/* 0 = unprobed; 1 = loaded; 2 = unavailable. */
static std::atomic<int> g_zstd_state{0};
static std::atomic<bool> g_zstd_disabled{false};

static KaeZstdApi g_api{};
/* dlopen handle kept open for the process lifetime — the API pointers must
 * stay valid, and dlclose would also race any hardware context the library
 * holds. Process exits clean it up. */
static void *g_handle = nullptr;

/* KAEZstd's libzstd.so (the patched full zstd) DT_NEEDEDs libkaezstd.so
 * (the kae adapter), and the adapter eagerly imports ERR_getErrorString back
 * from libzstd. Both libraries are built with -z now, so the cycle can only
 * be resolved inside one dlopen call (glibc maps the dependency, then
 * relocates both against each other). That requires libkaezstd.so.2.2.0 to
 * be findable by name on the loader path — the deployment step links it into
 * the default library dir (see README). */
static const char *g_candidates[] = {
    "/usr/local/kaezstd/lib/libzstd.so",
    "/usr/local/kaezstd/lib/libzstd.so.1",
};

typedef size_t (*zstd_compress_fn)(void *, size_t, const void *, size_t, int);
typedef unsigned (*zstd_iserror_fn)(size_t);
typedef const char *(*zstd_geterrorname_fn)(size_t);
typedef size_t (*zstd_compressbound_fn)(size_t);

const KaeZstdApi *zstd_api() {
    if (g_zstd_disabled.load(std::memory_order_relaxed)) {
        return nullptr;
    }
    int st = g_zstd_state.load(std::memory_order_acquire);
    if (st != 0) {
        return st == 1 ? &g_api : nullptr;
    }

    st = 2;
    const char *override_path = getenv("ZFILE_KAE_ZSTD_LIB");
    if (support_available()) {
        for (const char *path : g_candidates) {
            const char *use = override_path ? override_path : path;
            void *h = open_library(use);
            if (h == nullptr) {
                if (override_path) break;
                continue;
            }
            auto compress = (zstd_compress_fn)dlsym(h, "ZSTD_compress");
            auto iserror = (zstd_iserror_fn)dlsym(h, "ZSTD_isError");
            auto errname = (zstd_geterrorname_fn)dlsym(h, "ZSTD_getErrorName");
            auto bound = (zstd_compressbound_fn)dlsym(h, "ZSTD_compressBound");
            if (compress && iserror && errname && bound) {
                g_api.compress = compress;
                g_api.isError = iserror;
                g_api.getErrorName = errname;
                g_api.compressBound = bound;
                g_handle = h;
                st = 1;
                fprintf(stderr, "[kae-zstd] loaded %s\n", use);
                break;
            }
            dlclose(h);
            if (override_path) break;
        }
    }
    if (st != 1 && override_path) {
        fprintf(stderr, "[kae-zstd] ZFILE_KAE_ZSTD_LIB=%s unusable: %s\n", override_path, dlerror());
    }
    g_zstd_state.store(st, std::memory_order_release);
    return st == 1 ? &g_api : nullptr;
}

void disable_zstd() {
    g_zstd_disabled.store(true, std::memory_order_relaxed);
}

} // namespace Kae
} // namespace ZFile

#endif /* ENABLE_KAE */
