#ifndef HEADER_PlusDevices
#define HEADER_PlusDevices
/* htop-plus; GPL-2.0-or-later. Device discovery and sampling, without curses. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef struct { char name[NAME_MAX + 1]; } PlusDisk;
typedef struct { char path[PATH_MAX]; } PlusMount;
typedef struct { uint64_t read, write, ticks; } PlusDiskSample;

typedef enum { PLUS_NVIDIA, PLUS_AMD } PlusGpuKind;
typedef struct {
   PlusGpuKind kind;
   char name[NAME_MAX + 1];
   char device[PATH_MAX]; /* DRM or PCI sysfs directory */
   char pci[32];
} PlusGpu;
typedef struct {
   double used, total; /* bytes */
   double temperature, limit; /* Celsius, NAN when unknown */
   bool asleep;
} PlusGpuSample;

typedef struct {
   PlusDisk* disks;
   size_t diskCount;
   PlusMount* mounts;
   size_t mountCount;
   PlusGpu* gpus;
   size_t gpuCount;
} PlusDevices;

const PlusDevices* Plus_devices(void);
void Plus_cleanup(void);
bool Plus_gpuSample(size_t index, PlusGpuSample* out);
bool Plus_path(char* out, size_t size, const char* path);
bool Plus_diskSample(const char* name, PlusDiskSample* out);
bool Plus_mountSample(const char* mount, double* used, double* total, double* avail);
#endif
