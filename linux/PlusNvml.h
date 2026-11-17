#ifndef HEADER_PlusNvml
#define HEADER_PlusNvml
/* Public NVML memory ABI; no SDK or link dependency. GPL-2.0-or-later. */
typedef struct nvmlMemory_st {
   unsigned long long total, free, used;
} nvmlMemory_t;
#endif
