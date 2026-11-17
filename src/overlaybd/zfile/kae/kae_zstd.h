/* kae_zstd.h — KAEZstd (patched libzstd) one-shot compress entry for ZFile.
 *
 * KAEZstd ships as a libzstd-compatible library (its output is bit-compatible
 * with community zstd), but like libkaelz4 it aborts in an ELF constructor on
 * unsupported CPUs, so it is dlopen()ed after the Kae gate passes instead of
 * being linked. See kae_support.h and design.md D4/D5. */
#ifndef ZFILE_KAE_ZSTD_H
#define ZFILE_KAE_ZSTD_H

#include <cstddef>

namespace ZFile {
namespace Kae {

/* ABI subset of libzstd used by Compressor_zstd. The KAEZstd library is a
 * full zstd build with the hardware path wired into its compress entry, so
 * these signatures are the standard zstd ones. */
struct KaeZstdApi {
    size_t (*compress)(void *dst, size_t dst_cap, const void *src, size_t src_size, int level);
    unsigned (*isError)(size_t code);
    const char *(*getErrorName)(size_t code);
    size_t (*compressBound)(size_t src_size);
};

/* Returns the KAEZstd API, or nullptr when unavailable (gate failed, no
 * usable library, or previously disabled). Cached process-wide; a non-null
 * return stays valid forever. The environment variable ZFILE_KAE_ZSTD_LIB
 * overrides the candidate library paths (absolute .so path). */
const KaeZstdApi *zstd_api();

/* Permanently disable the KAEZstd path (e.g. a compress call failed); later
 * zstd_api() calls return nullptr so traffic falls back to the linked
 * software zstd. */
void disable_zstd();

} // namespace Kae
} // namespace ZFile

#endif /* ZFILE_KAE_ZSTD_H */
