/* lz4-kae.h — Kunpeng KAE (libkaelz4) LZ4 batch compress for ZFile.
 *
 * libkaelz4 is dlopen()ed at runtime, never linked: its ELF constructor
 * aborts the process on unsupported CPUs, so loading is gated by
 * Kae::support_available() (see ../kae/kae_support.h). The interface mirrors
 * lz4-qat.h so compressor.cpp treats both accelerators identically:
 * dst_len[i] in = capacity / out = compressed bytes; 0 on success, -1 on any
 * KAE issue (caller falls through to the CPU loop). */
#ifndef ZFILE_LZ4_KAE_H
#define ZFILE_LZ4_KAE_H

#include <cstddef>
#include <atomic>

struct LZ4_kae_param;

int kae_init(LZ4_kae_param *p);
int kae_uninit(LZ4_kae_param *p);

/* n <= 256. src_len[i] in = uncompressed bytes. dst_len[i] in = capacity,
 * out = compressed bytes. Returns 0 on success, -1 on any KAE issue
 * (submit failure, poll timeout, per-task error, bad output size). */
int LZ4_compress_kae(LZ4_kae_param *p,
                     unsigned char **src, size_t *src_len,
                     unsigned char **dst, size_t *dst_len,
                     size_t n);

/* Layout mirrors of the libkaelz4 2.2.0 ABI (KAELz4/include/kaelz4.h). They
 * are declared here because the library is dlopen()ed, not linked, so its
 * headers are not a build dependency. Checked with static_asserts in
 * lz4-kae.cpp. */
struct KaeLz4Buffer {
    size_t buf_len;
    void *data;
};
struct KaeLz4BufferList {
    unsigned int buf_num;
    unsigned int rsvd;
    KaeLz4Buffer *buf;
    void *usr_data;
};
struct KaeLz4Result {
    int status;              /* KAE_LZ4_SUCC (0) on success */
    unsigned int rsvd;
    void *user_data;
    size_t src_size;
    size_t dst_len;          /* out: actual compressed bytes */
    unsigned int *ibuf_crc;
    unsigned int *obuf_crc;
};
typedef void (*KaeLz4Callback)(KaeLz4Result *result);
struct KaeLz4DeviceConfig {
    int policy;              /* 0 = KAELZ4_SELECT_AUTO */
    union {
        const void *dev;     /* KAELZ4_SELECT_BY_DEV */
        unsigned int numa;   /* KAELZ4_SELECT_BY_NUMA */
    } param;
};

struct LZ4_kae_param {
    void *sess;              /* polling session, thread-bound to the caller */
    std::atomic<int> completed;
    std::atomic<int> errors;
    /* Set when the hardware stopped answering (poll timeout); the instance
     * stops submitting after that. */
    std::atomic<bool> broken;

    LZ4_kae_param() : sess(nullptr), completed(0), errors(0), broken(false) {}
};

#endif /* ZFILE_LZ4_KAE_H */
