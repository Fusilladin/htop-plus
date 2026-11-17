/* htop-plus device discovery. Released under GPL-2.0-or-later; see COPYING. */
#include "config.h"
#include "linux/PlusDevices.h"
#include <dirent.h>
#include <dlfcn.h>
#include <ctype.h>
#include <math.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

static PlusDevices devices;
static bool discovered;
static bool registered;

bool Plus_path(char* out, size_t size, const char* path) {
   const char* root = "";
#ifdef HTOP_PLUS_TESTING
   const char* fixture = getenv("HTOP_PLUS_SYSROOT");
   if (fixture && *fixture)
      root = fixture;
#endif
   int n = snprintf(out, size, "%s%s", root, path);
   return n >= 0 && (size_t)n < size;
}

static bool append(void** list, size_t* count, size_t size, const void* item) {
   if (*count >= SIZE_MAX / size - 1)
      return false;
   void* next = realloc(*list, (*count + 1) * size);
   if (!next)
      return false;
   *list = next;
   memcpy((char*)next + *count * size, item, size);
   ++*count;
   return true;
}

static bool diskName(const char* name) {
   return *name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..") &&
          strncmp(name, "loop", 4) && strncmp(name, "ram", 3) && strncmp(name, "zram", 4);
}

bool Plus_diskSample(const char* name, PlusDiskSample* out) {
   if (!diskName(name))
      return false;
   char relative[PATH_MAX], path[PATH_MAX];
   snprintf(relative, sizeof(relative), "/sys/block/%s/stat", name);
   if (!Plus_path(path, sizeof(path), relative))
      return false;
   FILE* fp = fopen(path, "r");
   if (!fp)
      return false;
   unsigned long long f[11];
   bool ok = fscanf(fp, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                    f, f+1, f+2, f+3, f+4, f+5, f+6, f+7, f+8, f+9, f+10) == 11;
   fclose(fp);
   if (ok)
      *out = (PlusDiskSample){ .read = f[2], .write = f[6], .ticks = f[9] };
   return ok;
}

static int compareDisk(const void* a, const void* b) {
   return strcmp(((const PlusDisk*)a)->name, ((const PlusDisk*)b)->name);
}

static int compareMount(const void* a, const void* b) {
   return strcmp(((const PlusMount*)a)->path, ((const PlusMount*)b)->path);
}

static void discoverDisks(void) {
   const char* override = getenv("HTOP_PLUS_DISK");
   if (override && *override) {
      if (diskName(override) && strlen(override) <= NAME_MAX) {
         PlusDisk disk = {0};
         strcpy(disk.name, override);
         append((void**)&devices.disks, &devices.diskCount, sizeof(disk), &disk);
      }
      return;
   }
   char path[PATH_MAX];
   if (!Plus_path(path, sizeof(path), "/sys/block"))
      return;
   DIR* dir = opendir(path);
   if (!dir)
      return;
   struct dirent* entry;
   while ((entry = readdir(dir))) {
      if (!diskName(entry->d_name))
         continue;
      PlusDisk disk = {0};
      strcpy(disk.name, entry->d_name);
      append((void**)&devices.disks, &devices.diskCount, sizeof(disk), &disk);
   }
   closedir(dir);
   if (devices.diskCount > 1)
      qsort(devices.disks, devices.diskCount, sizeof(*devices.disks), compareDisk);
}

static void discoverMounts(void) {
   const char* override = getenv("HTOP_PLUS_MOUNT");
   if (override && *override) {
      if (*override == '/' && strlen(override) < PATH_MAX) {
         PlusMount mount = {0};
         strcpy(mount.path, override);
         append((void**)&devices.mounts, &devices.mountCount, sizeof(mount), &mount);
      }
      return;
   }
   char path[PATH_MAX];
   if (!Plus_path(path, sizeof(path), "/proc/self/mounts"))
      return;
   FILE* fp = setmntent(path, "r");
   if (!fp)
      return;
   struct mntent entry, *mnt;
   char buffer[32768];
   while ((mnt = getmntent_r(fp, &entry, buffer, sizeof(buffer)))) {
      /* Device-backed filesystems and ZFS datasets; exclude pseudo/network mounts. */
      if ((strncmp(mnt->mnt_fsname, "/dev/", 5) && strcmp(mnt->mnt_type, "zfs")) ||
          !strcmp(mnt->mnt_type, "tmpfs") || !strcmp(mnt->mnt_type, "squashfs") ||
          !strcmp(mnt->mnt_type, "overlay") || !strcmp(mnt->mnt_type, "devtmpfs") ||
          *mnt->mnt_dir != '/' || strlen(mnt->mnt_dir) >= PATH_MAX)
         continue;
      bool duplicate = false;
      for (size_t i = 0; i < devices.mountCount; i++)
         duplicate |= !strcmp(devices.mounts[i].path, mnt->mnt_dir);
      if (!duplicate) {
         PlusMount mount = {0};
         strcpy(mount.path, mnt->mnt_dir);
         append((void**)&devices.mounts, &devices.mountCount, sizeof(mount), &mount);
      }
   }
   endmntent(fp);
   if (devices.mountCount > 1)
      qsort(devices.mounts, devices.mountCount, sizeof(*devices.mounts), compareMount);
}

bool Plus_mountSample(const char* mount, double* used, double* total, double* avail) {
   char path[PATH_MAX];
   if (!Plus_path(path, sizeof(path), mount))
      return false;
   struct statvfs st;
   if (statvfs(path, &st) || !st.f_blocks || st.f_bfree > st.f_blocks)
      return false;
   double kib = (double)st.f_frsize / 1024.0;
   *used = (double)(st.f_blocks - st.f_bfree) * kib;
   *avail = (double)st.f_bavail * kib;
   *total = (double)st.f_blocks * kib;
   return *used + *avail > 0;
}

/* NVML is optional, resolved at runtime. Never query it while any NVIDIA device
   reports runtime suspend: even obtaining another device's handle can wake peers.
   See NVIDIA NVML Device Queries and Initialization/Cleanup documentation. */
#include "linux/PlusNvml.h"
static struct {
   void* library;
   int (*init)(void);
   int (*shutdown)(void);
   int (*count)(unsigned*);
   int (*byIndex)(unsigned, void**);
   int (*byPci)(const char*, void**);
   int (*memory)(void*, nvmlMemory_t*);
   int (*temperature)(void*, int, unsigned*);
   int (*threshold)(void*, int, unsigned*);
   bool tried, ready;
} nv;

static bool readLine(const char* relative, char* out, size_t size) {
   char path[PATH_MAX];
   if (!Plus_path(path, sizeof(path), relative))
      return false;
   FILE* fp = fopen(path, "r");
   if (!fp)
      return false;
   bool ok = fgets(out, (int)size, fp) != NULL;
   fclose(fp);
   if (ok)
      out[strcspn(out, "\n")] = 0;
   return ok;
}

static bool gpuAsleep(const PlusGpu* gpu) {
   char path[PATH_MAX + 32], status[32];
   snprintf(path, sizeof(path), "%s/power/runtime_status", gpu->device);
   /* Unknown power state is treated conservatively for NVIDIA. */
   if (!readLine(path, status, sizeof(status)))
      return gpu->kind == PLUS_NVIDIA;
   return strcmp(status, "active") != 0;
}

static bool nvidiaAsleep(void) {
   for (size_t i = 0; i < devices.gpuCount; i++)
      if (devices.gpus[i].kind == PLUS_NVIDIA && gpuAsleep(&devices.gpus[i]))
         return true;
   return false;
}

static bool nvReady(void) {
   if (nvidiaAsleep())
      return false;
   if (nv.tried)
      return nv.ready;
   nv.tried = true;
   nv.library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
   if (!nv.library)
      return false;
#define SYMBOL(member, symbol) *(void**)&nv.member = dlsym(nv.library, symbol)
   SYMBOL(init, "nvmlInit_v2");
   SYMBOL(shutdown, "nvmlShutdown");
   SYMBOL(count, "nvmlDeviceGetCount_v2");
   SYMBOL(byIndex, "nvmlDeviceGetHandleByIndex_v2");
   SYMBOL(byPci, "nvmlDeviceGetHandleByPciBusId_v2");
   SYMBOL(memory, "nvmlDeviceGetMemoryInfo");
   SYMBOL(temperature, "nvmlDeviceGetTemperature");
   SYMBOL(threshold, "nvmlDeviceGetTemperatureThreshold");
#undef SYMBOL
   if (!nv.init || !nv.shutdown || !nv.count || !nv.byIndex || !nv.memory || !nv.byPci)
      return false;
   nv.ready = nv.init() == 0;
   return nv.ready;
}

static int compareGpu(const void* a, const void* b) {
   const PlusGpu* x = a;
   const PlusGpu* y = b;
   int kind = (int)x->kind - (int)y->kind;
   return kind ? kind : strcmp(x->pci, y->pci);
}

static bool addGpu(PlusGpu* gpu) {
   for (size_t i = 0; i < devices.gpuCount; i++)
      if (!strcmp(devices.gpus[i].pci, gpu->pci))
         return true;
   return append((void**)&devices.gpus, &devices.gpuCount, sizeof(*gpu), gpu);
}

static void discoverGpus(void) {
   /* PCI enumeration covers NVIDIA boards even without DRM modesetting. It only
      reads identification and runtime status; no driver or sensor initialization. */
   char path[PATH_MAX];
   if (!Plus_path(path, sizeof(path), "/sys/bus/pci/devices"))
      return;
   DIR* dir = opendir(path);
   struct dirent* entry;
   if (dir) {
      while ((entry = readdir(dir))) {
         if (*entry->d_name == '.' || strlen(entry->d_name) >= 32)
            continue;
         char relative[PATH_MAX + 16], vendor[32], cls[32];
         snprintf(relative, sizeof(relative), "/sys/bus/pci/devices/%s/vendor", entry->d_name);
         if (!readLine(relative, vendor, sizeof(vendor)) || strcmp(vendor, "0x10de"))
            continue;
         snprintf(relative, sizeof(relative), "/sys/bus/pci/devices/%s/class", entry->d_name);
         if (!readLine(relative, cls, sizeof(cls)) || strncmp(cls, "0x03", 4))
            continue;
         PlusGpu gpu = { .kind = PLUS_NVIDIA };
         snprintf(gpu.name, sizeof(gpu.name), "NVIDIA %.31s", entry->d_name);
         strcpy(gpu.pci, entry->d_name);
         snprintf(gpu.device, sizeof(gpu.device), "/sys/bus/pci/devices/%s", entry->d_name);
         addGpu(&gpu);
      }
      closedir(dir);
   }
   if (!Plus_path(path, sizeof(path), "/sys/class/drm"))
      return;
   dir = opendir(path);
   if (dir) {
      while ((entry = readdir(dir))) {
         const char* number = entry->d_name + 4;
         if (strncmp(entry->d_name, "card", 4) || !*number)
            continue;
         bool digits = true;
         for (const char* c = number; *c; c++)
            digits &= isdigit((unsigned char)*c) != 0;
         if (!digits)
            continue;
         char relative[PATH_MAX + 16], vendor[32];
         snprintf(relative, sizeof(relative), "/sys/class/drm/%s/device/vendor", entry->d_name);
         if (!readLine(relative, vendor, sizeof(vendor)) || strcmp(vendor, "0x1002"))
            continue;
         PlusGpu gpu = { .kind = PLUS_AMD };
         snprintf(gpu.name, sizeof(gpu.name), "AMD %.240s", entry->d_name);
         snprintf(gpu.pci, sizeof(gpu.pci), "%.31s", entry->d_name);
         snprintf(gpu.device, sizeof(gpu.device), "/sys/class/drm/%s/device", entry->d_name);
         addGpu(&gpu);
      }
      closedir(dir);
   }
   if (devices.gpuCount > 1)
      qsort(devices.gpus, devices.gpuCount, sizeof(*devices.gpus), compareGpu);
   /* NVIDIA enumeration stays NVML-backed when safe. Sysfs identities remain
      available while suspended or when the library/driver cannot be loaded. */
   bool hasNvidia = false;
   for (size_t i = 0; i < devices.gpuCount; i++)
      hasNvidia |= devices.gpus[i].kind == PLUS_NVIDIA;
   if (hasNvidia && nvReady()) {
      unsigned count = 0;
      if (nv.count(&count) != 0)
         return;
      /* Cross-check enumeration without acquiring handles (which wake devices).
         Sampling uses PCI identity, never a guessed NVML index. */
      (void)count;
   }
}

static bool readBytes(const char* device, const char* file, double* value) {
   char path[PATH_MAX + 64], text[64], *end;
   snprintf(path, sizeof(path), "%s/%s", device, file);
   if (!readLine(path, text, sizeof(text)) || text[0] == '-')
      return false;
   unsigned long long n = strtoull(text, &end, 10);
   if (end == text || *end)
      return false;
   *value = n;
   return true;
}

bool Plus_gpuSample(size_t index, PlusGpuSample* out) {
   *out = (PlusGpuSample){ .temperature = NAN, .limit = NAN };
   if (index >= devices.gpuCount)
      return false;
   const PlusGpu* gpu = &devices.gpus[index];
   out->asleep = gpuAsleep(gpu);
   if (out->asleep)
      return false;
   if (gpu->kind == PLUS_AMD) {
      if (!readBytes(gpu->device, "mem_info_vram_used", &out->used) ||
          !readBytes(gpu->device, "mem_info_vram_total", &out->total))
         return false;
   } else {
      if (!nvReady())
         return false;
      void* handle = NULL;
      nvmlMemory_t memory;
      if (nv.byPci(gpu->pci, &handle) || nv.memory(handle, &memory))
         return false;
      out->used = memory.used;
      out->total = memory.total;
      unsigned value;
      if (nv.temperature && !nv.temperature(handle, 0, &value))
         out->temperature = value;
      if (nv.threshold && !nv.threshold(handle, 1, &value))
         out->limit = value;
   }
   return out->total > 0 && out->used <= out->total;
}

void Plus_cleanup(void) {
   free(devices.disks);
   free(devices.mounts);
   free(devices.gpus);
   if (nv.ready)
      nv.shutdown();
   if (nv.library)
      dlclose(nv.library);
   memset(&nv, 0, sizeof(nv));
   devices = (PlusDevices){0};
   discovered = false;
}

const PlusDevices* Plus_devices(void) {
   if (!discovered) {
      discovered = true;
      if (!registered) {
         atexit(Plus_cleanup);
         registered = true;
      }
      discoverDisks();
      discoverMounts();
      discoverGpus();
   }
   return &devices;
}
