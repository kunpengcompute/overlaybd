/* kae_support.h — Kunpeng KAE hardware gate + runtime library loading for ZFile.
 *
 * The KAE 2.2.0 components (libkaelz4, the KAEZstd libzstd) detect the CPU in
 * an ELF constructor and abort() the whole process on unsupported CPUs, so
 * they must never be linked implicitly — they are dlopen()ed only after this
 * gate passes. See design.md D4 for details. */
#ifndef ZFILE_KAE_SUPPORT_H
#define ZFILE_KAE_SUPPORT_H

namespace ZFile {
namespace Kae {

/* Process-cached gate: aarch64 + a Hisilicon CPU part accepted by the KAE
 * 2.2.0 stack + a hisi_zip-* device present under /sys/class/uacce. The
 * accepted part set mirrors KAELz4/src/utils/kaelz4_common.c exactly
 * ({0xd02, 0xd03, 0xd22, 0xd06}); widening it would let a later dlopen
 * abort the process. */
bool support_available();

/* dlopen(path, RTLD_LOCAL | RTLD_NOW). Returns nullptr on failure. */
void *open_library(const char *path);

/* dlopen with explicit flags — for dependency chains that need RTLD_LAZY
 * preloads or RTLD_GLOBAL symbol scope. */
void *open_library_flags(const char *path, int flags);

} // namespace Kae
} // namespace ZFile

#endif /* ZFILE_KAE_SUPPORT_H */
