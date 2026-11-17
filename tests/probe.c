/* Minimal subprocess probe for the real device backend. GPL-2.0-or-later. */
#include "linux/PlusDevices.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
   const PlusDevices* d = Plus_devices();
   if (argc > 1 && !strcmp(argv[1], "disk")) {
      PlusDiskSample sample;
      if (argc < 3 || !Plus_diskSample(argv[2], &sample))
         return 2;
      printf("%llu %llu %llu\n", (unsigned long long)sample.read,
             (unsigned long long)sample.write, (unsigned long long)sample.ticks);
   } else if (argc > 1 && !strcmp(argv[1], "mount")) {
      double used, total, avail;
      if (argc < 3 || !Plus_mountSample(argv[2], &used, &total, &avail))
         return 2;
      printf("%d\n", used >= 0 && total > 0 && avail >= 0);
   } else {
      for (size_t i = 0; i < d->diskCount; i++)
         printf("disk %s\n", d->disks[i].name);
      for (size_t i = 0; i < d->mountCount; i++)
         printf("mount %s\n", d->mounts[i].path);
      for (size_t i = 0; i < d->gpuCount; i++) {
         PlusGpuSample s;
         bool ok = Plus_gpuSample(i, &s);
         printf("gpu %s %d %.0f %.0f %d\n", d->gpus[i].name, ok, s.used, s.total, s.asleep);
      }
   }
   Plus_cleanup();
   Plus_cleanup(); /* idempotent, atexit is safe too */
   return 0;
}
