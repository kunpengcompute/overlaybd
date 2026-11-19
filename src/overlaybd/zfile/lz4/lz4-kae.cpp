/* lz4-kae.cpp — Kunpeng KAE LZ4 batch compress via libkaelz4 polling session.
 *
 * Batch flow mirrors lz4-qat.cpp: submit every block as one session task,
 * poll from the same (caller) thread until all complete, then copy the
 * produced sizes out. Any failure drains in-flight tasks and returns -1 so
 * compressor.cpp falls through to its CPU loop for the whole batch. */

#ifdef ENABLE_KAE

#include "lz4-kae.h"
#include "../kae/kae_support.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <photon/thread/thread.h>

namespace {

/* libkaelz4 2.2.0 polling-mode session ABI, dlsym'd from kaelz4.h. */
typedef void *(*kae_create_session_fn)(void *usr_map, const KaeLz4DeviceConfig *config);
typedef void (*kae_destroy_session_fn)(void *sess);
typedef void (*kae_reset_session_fn)(void *sess);
typedef int (*kae_compress_in_session_fn)(void *sess, const KaeLz4BufferList *src,
                                          KaeLz4BufferList *dst, KaeLz4Callback cb,
                                          KaeLz4Result *result);
typedef void (*kae_polling_fn)(void *sess, int budget);

static const char *KAE_LZ4_LIB_PATH = "/usr/local/kaelz4/lib/libkaelz4.so";
/* libkaelz4.so DT_NEEDEDs the patched liblz4 (frame API) from the same
 * install prefix, which is not on the default loader path. Preload it by
 * full path so the main dlopen resolves; failure is tolerated and surfaces
 * through the main dlopen's error. */
static const char *KAE_LZ4_DEP_PATH = "/usr/local/kaelz4/lib/liblz4.so.1";
static const int KAE_POLL_BUDGET = 128;
/* Same bound as the QAT path. */
static const double KAE_POLL_TIMEOUT_S = 30.0;
static const size_t KAE_MAX_BATCH = 256;

/* The library is a process-global resource (like the QAT DMA pool): loaded
 * once, symbols resolved once; each LZ4_kae_param owns one session. */
static void *g_lib = nullptr;
static kae_create_session_fn g_create_session = nullptr;
static kae_destroy_session_fn g_destroy_session = nullptr;
static kae_reset_session_fn g_reset_session = nullptr;
static kae_compress_in_session_fn g_compress_in_session = nullptr;
static kae_polling_fn g_polling = nullptr;
/* 0 = unprobed; 1 = loaded; 2 = failed. */
static std::atomic<int> g_lib_state{0};

struct KaeTaskCtx {
    LZ4_kae_param *p;
    uint32_t idx;
};

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Runs on the polling thread (the submitter) while inside
 * LZ4_compress_kae; the session is thread-bound so no cross-thread sync
 * beyond the completed/errors counters is needed. */
static void kae_result_cb(KaeLz4Result *result) {
    if (result == nullptr || result->user_data == nullptr) {
        return;
    }
    auto *ctx = (KaeTaskCtx *)result->user_data;
    if (result->status != 0) {
        ctx->p->errors.fetch_add(1, std::memory_order_relaxed);
    }
    ctx->p->completed.fetch_add(1, std::memory_order_release);
}

static bool load_lib() {
    int st = g_lib_state.load(std::memory_order_acquire);
    if (st != 0) {
        return st == 1;
    }
    if (!ZFile::Kae::support_available()) {
        g_lib_state.store(2, std::memory_order_release);
        return false;
    }
    void *dep = ZFile::Kae::open_library(KAE_LZ4_DEP_PATH);
    if (dep == nullptr) {
        fprintf(stderr, "[lz4-kae] preload %s failed: %s\n", KAE_LZ4_DEP_PATH, dlerror());
    }
    g_lib = ZFile::Kae::open_library(KAE_LZ4_LIB_PATH);
    if (g_lib == nullptr) {
        fprintf(stderr, "[lz4-kae] dlopen %s failed: %s\n", KAE_LZ4_LIB_PATH, dlerror());
        g_lib_state.store(2, std::memory_order_release);
        return false;
    }
    bool ok = true;
    auto sym = [&](const char *name) {
        void *s = dlsym(g_lib, name);
        if (s == nullptr) {
            fprintf(stderr, "[lz4-kae] dlsym %s failed\n", name);
            ok = false;
        }
        return s;
    };
    g_create_session = (kae_create_session_fn)sym("KAELZ4_create_async_compress_session");
    g_destroy_session = (kae_destroy_session_fn)sym("KAELZ4_destroy_async_compress_session");
    g_reset_session = (kae_reset_session_fn)sym("KAELZ4_reset_session");
    g_compress_in_session = (kae_compress_in_session_fn)sym("KAELZ4_compress_async_in_session");
    g_polling = (kae_polling_fn)sym("KAELZ4_async_polling_in_session");
    if (!ok) {
        dlclose(g_lib);
        g_lib = nullptr;
        g_lib_state.store(2, std::memory_order_release);
        return false;
    }
    g_lib_state.store(1, std::memory_order_release);
    return true;
}

/* Polls until `until` tasks have completed. Returns true when reached,
 * false on timeout. */
static bool poll_until(LZ4_kae_param *p, int until, double deadline_ms) {
    while (p->completed.load(std::memory_order_acquire) < until) {
        g_polling(p->sess, KAE_POLL_BUDGET);
        photon::thread_yield();
        if (now_ms() > deadline_ms) {
            fprintf(stderr, "[lz4-kae] poll timeout completed=%d/%d\n",
                    p->completed.load(), until);
            return false;
        }
    }
    return true;
}

} // namespace

/* Layout guards for the ABI mirrors declared in lz4-kae.h (LP64). */
static_assert(sizeof(KaeLz4Buffer) == 16, "KaeLz4Buffer layout mismatch");
static_assert(sizeof(KaeLz4BufferList) == 24, "KaeLz4BufferList layout mismatch");
static_assert(sizeof(KaeLz4Result) == 48, "KaeLz4Result layout mismatch");
static_assert(sizeof(KaeLz4DeviceConfig) == 16, "KaeLz4DeviceConfig layout mismatch");

int kae_init(LZ4_kae_param *p) {
    if (!load_lib()) {
        return -1;
    }
    KaeLz4DeviceConfig cfg;
    cfg.policy = 0; // KAELZ4_SELECT_AUTO
    cfg.param.numa = 0;
    /* usr_map == NULL: the library DMA-maps plain user memory (no hugepages
     * required); per-block sizes stay within the non-SGL limit. */
    p->sess = g_create_session(nullptr, &cfg);
    if (p->sess == nullptr) {
        fprintf(stderr, "[lz4-kae] create session failed\n");
        return -1;
    }
    return 0;
}

int kae_uninit(LZ4_kae_param *p) {
    if (p->sess != nullptr && g_destroy_session != nullptr) {
        g_destroy_session(p->sess);
    }
    p->sess = nullptr;
    return 0;
    /* g_lib is intentionally kept open: process-global, shared with other
     * instances, and pointers must stay valid (same reasoning as the QAT
     * pool, which is never torn down on instance exit). */
}

int LZ4_compress_kae(LZ4_kae_param *p,
                     unsigned char **src, size_t *src_len,
                     unsigned char **dst, size_t *dst_len,
                     size_t n) {
    if (p == nullptr || p->sess == nullptr || n == 0 || n > KAE_MAX_BATCH) {
        return -1;
    }
    if (p->broken.load(std::memory_order_relaxed)) {
        return -1;
    }

    p->completed.store(0, std::memory_order_relaxed);
    p->errors.store(0, std::memory_order_relaxed);

    KaeTaskCtx ctx[KAE_MAX_BATCH];
    KaeLz4Buffer src_bufs[KAE_MAX_BATCH], dst_bufs[KAE_MAX_BATCH];
    KaeLz4BufferList src_lists[KAE_MAX_BATCH], dst_lists[KAE_MAX_BATCH];
    KaeLz4Result results[KAE_MAX_BATCH];

    size_t submitted = 0;
    for (size_t i = 0; i < n; i++) {
        ctx[i].p = p;
        ctx[i].idx = (uint32_t)i;
        src_bufs[i].buf_len = src_len[i];
        src_bufs[i].data = src[i];
        dst_bufs[i].buf_len = dst_len[i]; // in: per-block capacity
        dst_bufs[i].data = dst[i];
        src_lists[i].buf_num = 1;
        src_lists[i].rsvd = 0;
        src_lists[i].buf = &src_bufs[i];
        src_lists[i].usr_data = nullptr;
        dst_lists[i].buf_num = 1;
        dst_lists[i].rsvd = 0;
        dst_lists[i].buf = &dst_bufs[i];
        dst_lists[i].usr_data = nullptr;
        results[i].status = 0;
        results[i].rsvd = 0;
        results[i].user_data = &ctx[i];
        results[i].src_size = src_len[i];
        results[i].dst_len = dst_len[i];
        results[i].ibuf_crc = nullptr;
        results[i].obuf_crc = nullptr;

        int r = g_compress_in_session(p->sess, &src_lists[i], &dst_lists[i],
                                      kae_result_cb, &results[i]);
        if (r != 0) {
            fprintf(stderr, "[lz4-kae] submit[%zu]=%d, drain & give up\n", i, r);
            /* In-flight tasks write into our dst buffers; drain them before
             * handing the buffers back to the CPU loop. */
            double deadline = now_ms() + KAE_POLL_TIMEOUT_S * 1000.0;
            if (!poll_until(p, (int)submitted, deadline)) {
                /* Hardware stuck: cancel everything and stop using KAE on
                 * this instance. The reset cancels pending tasks; buffers of
                 * this batch may still see residual hardware writes, same
                 * trade-off the QAT path documents for its timeout path. */
                g_reset_session(p->sess);
                p->broken.store(true, std::memory_order_relaxed);
                return -1;
            }
            return -1;
        }
        submitted++;
    }

    double deadline = now_ms() + KAE_POLL_TIMEOUT_S * 1000.0;
    if (!poll_until(p, (int)n, deadline)) {
        g_reset_session(p->sess);
        p->broken.store(true, std::memory_order_relaxed);
        return -1;
    }
    if (p->errors.load(std::memory_order_acquire) > 0) {
        fprintf(stderr, "[lz4-kae] %d errors in batch n=%zu\n",
                p->errors.load(), n);
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        size_t produced = results[i].dst_len;
        if (produced == 0 || produced > dst_len[i]) {
            fprintf(stderr, "[lz4-kae] block[%zu] bad produced=%zu cap=%zu\n",
                    i, produced, dst_len[i]);
            return -1;
        }
        dst_len[i] = produced;
    }
    return 0;
}

#endif /* ENABLE_KAE */
