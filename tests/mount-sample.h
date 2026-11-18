/* Deterministic df-style samples; still require the fixture path to exist.
   Linked only into tests/meters and tests/layout with --wrap=statvfs. */
#ifndef TEST_MOUNT_SAMPLE_H
#define TEST_MOUNT_SAMPLE_H
#include <sys/statvfs.h>

static struct statvfs testMountSample = {
   .f_frsize = 1024 * 1024,
   .f_blocks = 1024 * 1024,
   .f_bfree = 448 * 1024,
   .f_bavail = 384 * 1024,
};

int __real_statvfs(const char* path, struct statvfs* st);
int __wrap_statvfs(const char* path, struct statvfs* st);

int __wrap_statvfs(const char* path, struct statvfs* st) {
   int result = __real_statvfs(path, st);
   if (result == 0)
      *st = testMountSample;
   return result;
}
#endif
