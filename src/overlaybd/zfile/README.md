# CRC

## The default order for CRC
If multiple features are included, the default order is:
1. DSA
2. AVX512
3. SSE4.2
4. software

## DSA

### Introduction

Intel® DSA is a high-performance data copy and transformation accelerator that will be
integrated into future Intel® processors, targeted for optimizing streaming data movement
and transformation operations common with applications for high-performance storage,
networking, persistent memory, and various data processing applications.

Intel® DSA replaces the Intel® QuickData Technology, which is a part of Intel® I/O 
Acceleration Technology.

The goal is to provide higher overall system performance for data mover and transformation
operations while freeing up CPU cycles for higher-level functions. Intel® DSA enables
high-performance data mover capability to/from volatile memory, persistent memory,
memory-mapped I/O, and through a Non-Transparent Bridge (NTB) device to/from remote volatile
and persistent memory on another node in a cluster. Enumeration and configuration are done
with a PCI Express* compatible programming interface to the Operating System (OS) and can be
controlled through a device driver.

Besides the basic data mover operations, Intel® DSA supports a set of transformation
operations on memory. For example:

Generate and test CRC checksum, or Data Integrity Field (DIF) to support storage and
networking applications.
Memory Compare and delta generate/merge to support VM migration, VM Fast check-pointing, and
software-managed memory deduplication usages.

We did a performance comparison test, and the QPS calculated by CRC were 960124 and 7977434
respectively in the case of 4K size without and with DSA. About a nine-times improvement
using DSA in performance.

### Check DSA hardware
```bash
$ lspci -nn | grep 8086:0b25
```
 
If get any information, which mean the system have DSA hardware.

Function check_dsa() is used to ensure that system have DSA hardware.

### Dependencies
Ubuntu 20.04 : apt-get install -y libpci-dev
CentOS 8.4 : yum install -y pciutils-devel

### Unit test for DSA Calculate CRC
```bash
$ cmake -DENABLE_DSA=1 -DBUILD_TESTING=1 ..
$ make -j
$./output/zfile_test
```

## AVX512

### Introduction

Here we use Intel(R) Intelligent Storage Acceleration Library for AVX512.

Intel(R) Intelligent Storage Acceleration Library is a collection of optimized low-level functions targeting storage applications. ISA-L includes:

Erasure codes - Fast block Reed-Solomon type erasure codes for any encode/decode matrix in GF(2^8).
CRC - Fast implementations of cyclic redundancy check. Six different polynomials supported.
    iscsi32, ieee32, t10dif, ecma64, iso64, jones64.
Raid - calculate and operate on XOR and P+Q parity found in common RAID implementations.
Compression - Fast deflate-compatible data compression.
De-compression - Fast inflate-compatible data compression.
igzip - A command line application like gzip, accelerated with ISA-L.

### Check if the system have AVX512

__builtin_cpu_supports("avx512f") is used to check if the system have AVX512.

### Dependencies
Ubuntu 20.04 : apt-get install -y nasm
CentOS 8.4 : yum install -y nasm

### Unit test for DSA Calculate CRC
```bash
$ cmake -DENABLE_ISAL=1 -DBUILD_TESTING=1 ..
$ make -j
$./output/zfile_test
```

# Compression

## QAT

### Introduction
Accelerate encryption and compression to provide improved
efficiency, scalability and performance – for data in motion or
at rest, in-flight or in process. Intel QuickAssist Technology (Intel QAT)
can save cycles, time and space of applications as varied as networking
to enterprise, cloud to storage, content delivery to database.

### Check QAT hardware
```bash
$ lspci -nn | grep 8086:4940
```

### Dependencies
Ubuntu 20.04 : apt-get install -y libpci-dev
CentOS 8.4 : yum install -y pciutils-devel

### Unit test for QAT compression and decompression
```bash
$ cmake -DENABLE_QAT=1 -DBUILD_TESTING=1 ..
$ make -j
$./output/zfile_test
```

## KAE (Kunpeng Accelerator Engine)

### Introduction
KAE offloads zfile compression (LZ4 and ZSTD) to the Kunpeng ZIP
accelerator on aarch64 platforms. The integration mirrors the QAT one:
compression-only (KAE provides no hardware decompression for LZ4/ZSTD),
batch submission, and transparent fallback to the CPU path whenever the
hardware is absent or fails. Artifacts are bit-compatible with the
software path — no change is needed on the decompression side.

### Hardware and platform requirements
- aarch64 with a Hisilicon CPU part the KAE 2.2.0 stack accepts
  (Kunpeng 920 parts 0xd02/0xd03/0xd22/0xd06; the runtime gate mirrors
  the KAE libraries' own detection — anything else is served by the
  software path, never the hardware)
- One or more HiSilicon ZIP engines: `lspci -nn | grep 19e5:a250`

### Software prerequisites (runtime, all user-space dlopen'ed — nothing
is linked at build time)
1. KAE kernel driver, built in no-SVA mode. The in-kernel driver registers
   uacce devices with the SVA flag, which the UADK v1 API rejects:
   ```bash
   $ cd KAEKernelDriver-OLK-6.6 && make && make nosva   # installs to /lib/modules/.../extra
   $ modprobe hisi_zip            # options hisi_zip uacce_mode=2 (see /etc/modprobe.d)
   $ ls /sys/class/uacce/         # hisi_zip-N devices, flags must NOT have the SVA bit
   ```
2. UADK (libwd / libwd_comp), built and installed from the KAE repository.
3. KAELz4 2.2.0 (`/usr/local/kaelz4/lib`) — the LZ4 path preloads its
   liblz4.so.1 dependency by full path automatically.
4. KAEZstd 2.2.0 (`/usr/local/kaezstd/lib`) — its two libraries reference
   each other and are built with `-z now`, so `libkaezstd.so.2.2.0` must be
   findable by the loader:
   ```bash
   $ ln -sf /usr/local/kaezstd/lib/libkaezstd.so.2.2.0 /usr/local/lib/
   $ ldconfig
   ```

### Build
```bash
$ cmake -DENABLE_KAE=1 ..
$ make -j
```
No KAE headers or libraries are needed at build time; the binary is safe
to run on any machine and silently uses the software path when the gate
does not pass.

### Runtime behavior
- Detection (CPU part + `/sys/class/uacce/hisi_zip-*`) is cached
  process-wide; probing never runs on the compression hot path.
- The LZ4 path submits whole batches (up to 256 blocks) through a polling
  session and harvests on the same thread; any submit/poll/task failure
  falls back to the CPU loop for that batch.
- The ZSTD path routes `ZSTD_compress` through the KAEZstd library;
  a failure disables the hardware path for the process and retries the
  block with the linked software zstd.

### Block size
Block sizes up to 64KB are accepted as before. Note an existing zfile
reader limitation unrelated to KAE: the reader's fixed 64KB staging buffer
(`MAX_READ_SIZE`) cannot hold blocks that compressed to more than 64KB
(possible at `--bs 64` for incompressible content; the software writer has
the same behavior). Block sizes up to 32KB avoid it entirely and measured
best on real image data (1.69GB filesystem-content tar, 3-run median,
software vs KAE):

| config | software | KAE | speedup | CPU | artifact size |
|---|---|---|---|---|---|
| lz4, bs=32 | 3.80s | 1.83s | 2.07× | -52% | -5.3% |
| zstd, bs=32 | 7.71s | 4.75s | 1.62× | -39% | -4.7% |

### Verifying KAE is engaged
```bash
$ overlaybd-commit -z --algorithm lz4 --bs 32 <data> <index> <out>
# log shows: LZ4 compression offloaded to KAE hardware
$ overlaybd-zfile --verify <out>       # artifact checksums pass
```
The KAE libraries log to `/var/log/kaelz4.log` when a config file is
provided via `KAELZ4_CONF_ENV`.