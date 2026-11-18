/* Exercise real meter callbacks and Header persistence. GPL-2.0-or-later. */
#include "config.h"
#include "linux/ExtraMeters.h"
#include "linux/PlusDevices.h"
#include "CRT.h"
#include "Header.h"
#include "Settings.h"
#include "tests/mount-sample.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char* program = "htop-plus";

static void sensorValue(const char* text) {
   char path[PATH_MAX];
   assert(Plus_path(path, sizeof(path), "/sys/block/sda/device/hwmon0/temp1_input"));
   FILE* fp = fopen(path, "w");
   assert(fp);
   fputs(text, fp);
   fclose(fp);
}

static void statFile(const char* disk, unsigned read, unsigned ticks) {
   char relative[256], path[PATH_MAX];
   snprintf(relative, sizeof(relative), "/sys/block/%s/stat", disk);
   assert(Plus_path(path, sizeof(path), relative));
   FILE* fp = fopen(path, "w");
   assert(fp);
   fprintf(fp, "1 0 %u 0 1 0 %u 0 0 %u 0\n", read, read, ticks);
   fclose(fp);
}

static void fixtureDir(const char* name) {
   char path[PATH_MAX];
   assert(Plus_path(path, sizeof(path), name));
   assert(mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void mountsFile(bool extra, bool home) {
   fixtureDir("/proc"); fixtureDir("/proc/self");
   fixtureDir("/boot"); fixtureDir("/boot/efi");
   fixtureDir("/data"); fixtureDir("/home"); fixtureDir("/models");
   fixtureDir("/aaa"); fixtureDir("/odd space(%)=disk");
   char path[PATH_MAX];
   assert(Plus_path(path, sizeof(path), "/proc/self/mounts"));
   FILE* fp = fopen(path, "w");
   assert(fp);
   fputs("/dev/sda1 / ext4 rw 0 0\n/dev/sda2 /boot/efi vfat rw 0 0\n"
         "/dev/sdb1 /data ext4 rw 0 0\n/dev/sdc1 /models ext4 rw 0 0\n"
         "/dev/sdd1 /odd\\040space(%)=disk ext4 rw 0 0\n", fp);
   if (home)
      fputs("/dev/sda3 /home ext4 rw 0 0\n", fp);
   if (extra)
      fputs("/dev/sde1 /aaa ext4 rw 0 0\n", fp);
   assert(fclose(fp) == 0);
   Plus_cleanup();
}

static Meter* columnMeter(Header* header, int index) {
   return (Meter*)header->columns[0]->array[index];
}

static void savedMounts(Machine* host, Settings* settings) {
   assert(unsetenv("HTOP_PLUS_MOUNT") == 0);
   mountsFile(false, true);
   Header* header = Header_new(host, HF_TWO_50_50);
   Header_addMeterByClass(header, &SSDMeter_class, 2, 1);
   Header_addMeterByClass(header, &VRAMMeter_class, 2, 1);
   /* Sorted list: /, /boot/efi, /data, /home, /models, /odd space(%)=disk. */
   const char* paths[] = { "/", "/boot/efi", "/data", "/home", "/models", "/odd space(%)=disk" };
   for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
      Meter* meter = Header_addMeterByClass(header, &DiskSpaceMeter_class, i + 1, 0);
      assert(!strcmp(DiskSpaceMeter_getMount(meter), paths[i]));
      Meter_setMode(meter, i % 2 ? TEXT_METERMODE : BAR_METERMODE);
   }
   Header_writeBackToSettings(header);
   assert(!strcmp(settings->hColumns[0].names[3], "DiskSpace(/home)"));
   assert(!strcmp(settings->hColumns[0].names[5], "DiskSpace(/odd%20space%28%25%29%3Ddisk)"));
   assert(!strcmp(settings->hColumns[1].names[0], "SSD(2)"));
   assert(!strcmp(settings->hColumns[1].names[1], "VRAM(2)"));
   assert(Settings_write(settings, false) == 0);
   Header_delete(header);

   /* Real htoprc read after a new earlier mount shifts every non-root index. */
   mountsFile(true, true);
   Settings* loaded = Settings_new(host, NULL, NULL, NULL);
   host->settings = loaded;
   header = Header_new(host, loaded->hLayout);
   Header_populateFromSettings(header);
   assert(header->columns[0]->items == 6);
   for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
      Meter* meter = columnMeter(header, i);
      assert(!strcmp(DiskSpaceMeter_getMount(meter), paths[i]));
      assert(meter->param == (i ? i + 2 : 1));
      assert(meter->mode == (i % 2 ? TEXT_METERMODE : BAR_METERMODE));
      Meter_updateValues(meter);
      assert(!strcmp(meter->txtBuffer, "576G/1.00T 60.0% 384G free"));
   }
   Header_reinit(header);
   assert(!strcmp(DiskSpaceMeter_getMount(columnMeter(header, 3)), "/home"));
   Header_writeBackToSettings(header);
   assert(Settings_write(loaded, false) == 0);

   /* An unmounted /home directory still exists: never report its parent filesystem. */
   mountsFile(true, false);
   Header_populateFromSettings(header);
   Meter* missing = columnMeter(header, 3);
   Meter_updateValues(missing);
   assert(missing->total == 0 && !strcmp(missing->txtBuffer, "n/a"));
   assert(!strcmp(DiskSpaceMeter_getMount(missing), "/home"));
   Header_writeBackToSettings(header);
   assert(!strcmp(loaded->hColumns[0].names[3], "DiskSpace(/home)"));

   /* A single compatibility override affects every row but never replaces saved paths. */
   assert(setenv("HTOP_PLUS_MOUNT", "/data", 1) == 0);
   Plus_cleanup();
   Header_populateFromSettings(header);
   for (int i = 0; i < header->columns[0]->items; i++) {
      Meter* meter = columnMeter(header, i);
      Meter_updateValues(meter);
      assert(meter->total > 0);
      char label[80];
      Meter_getUiName(meter, label, sizeof(label));
      assert(strstr(label, "/data"));
      assert(!strcmp(DiskSpaceMeter_getMount(meter), paths[i]));
   }
   Header_writeBackToSettings(header);
   assert(!strcmp(loaded->hColumns[0].names[3], "DiskSpace(/home)"));
   assert(setenv("HTOP_PLUS_MOUNT", "/missing", 1) == 0);
   Plus_cleanup();
   Meter_updateValues(columnMeter(header, 0));
   assert(!strcmp(columnMeter(header, 0)->txtBuffer, "n/a"));
   assert(unsetenv("HTOP_PLUS_MOUNT") == 0);
   mountsFile(false, true);
   Header_populateFromSettings(header);
   Meter_updateValues(columnMeter(header, 3));
   assert(columnMeter(header, 3)->total > 0);

   /* Legacy numeric and unparameterized names migrate on save. */
   free(loaded->hColumns[0].names[0]);
   free(loaded->hColumns[0].names[1]);
   loaded->hColumns[0].names[0] = strdup("DiskSpace(4)");
   loaded->hColumns[0].names[1] = strdup("DiskSpace");
   Header_populateFromSettings(header);
   assert(!strcmp(DiskSpaceMeter_getMount(columnMeter(header, 0)), "/home"));
   assert(!strcmp(DiskSpaceMeter_getMount(columnMeter(header, 1)), "/"));
   Header_writeBackToSettings(header);
   assert(!strcmp(loaded->hColumns[0].names[0], "DiskSpace(/home)"));
   assert(!strcmp(loaded->hColumns[0].names[1], "DiskSpace(/)"));

   /* Malformed path tokens are ignored safely. */
   const char* invalid[] = { "DiskSpace(/bad%00)", "DiskSpace(/bad%2)", "DiskSpace(/bad%GG)",
                            "DiskSpace(/bad", "DiskSpace(/bad)extra" };
   for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
      free(loaded->hColumns[0].names[0]);
      loaded->hColumns[0].names[0] = strdup(invalid[i]);
      Header_populateFromSettings(header);
      assert(header->columns[0]->items == 5);
   }
   Header_delete(header);
   Settings_delete(loaded);
   host->settings = settings;
}

int main(void) {
   CRT_degreeSign = "°";
   sensorValue("31000\n");
   char mount[222], rooted[PATH_MAX];
   mount[0] = '/';
   memset(mount + 1, 'm', 220);
   mount[221] = 0;
   assert(setenv("HTOP_PLUS_MOUNT", mount, 1) == 0);
   assert(Plus_path(rooted, sizeof(rooted), mount));
   (void)mkdir(rooted, 0700);
   Machine host = { .monotonicMs = 1000 };
   Settings* settings = Settings_new(&host, NULL, NULL, NULL);
   host.settings = settings;
   Settings_setHeaderLayout(settings, HF_TWO_50_50);
   statFile("sda", 100, 100);
   statFile("sdb", 1000, 1000);
   Meter* a = Meter_new(&host, 1, &SSDMeter_class);
   Meter* b = Meter_new(&host, 2, &SSDMeter_class);
   Meter_updateValues(a); Meter_updateValues(b);
   assert(a->values[0] == 0 && b->values[0] == 0);
   assert(a->tempPartCount > 0 && strstr(a->tempParts[0].text, "31"));
   sensorValue("55000\n");
   host.monotonicMs = 2000;
   statFile("sda", 120, 200); statFile("sdb", 1200, 1500);
   Meter_updateValues(a); Meter_updateValues(b);
   assert(a->values[0] == 10 && b->values[0] == 50);
   assert(strstr(a->tempParts[0].text, "31")); /* cached inside five seconds */
   assert(strstr(a->txtBuffer, "R10K") && strstr(b->txtBuffer, "R100K"));
   Meter* duplicate = Meter_new(&host, 1, &SSDMeter_class);
   Meter_updateValues(duplicate);
   assert(duplicate->values[0] == 0 && a->values[0] == 10);
   host.monotonicMs = 3000;
   statFile("sda", 1, 1); Meter_updateValues(a);
   assert(a->values[0] == 0 && strstr(a->txtBuffer, "R0K"));
   host.monotonicMs = 6000;
   Meter_updateValues(a);
   assert(strstr(a->tempParts[0].text, "55")); /* refreshed at five seconds */
   host.monotonicMs = 7000;
   char path[PATH_MAX];
   assert(Plus_path(path, sizeof(path), "/sys/block/sda/stat"));
   FILE* fp = fopen(path, "w");
   assert(fp);
   fputs("1 0 27035 0 1 0 6043 0 0 865 0\n", fp);
   fclose(fp);
   Meter_updateValues(a);
   assert(!strcmp(a->txtBuffer, "86% R13M W3M"));
   Meter_delete((Object*)a); Meter_delete((Object*)b); Meter_delete((Object*)duplicate);
   Meter* space = Meter_new(&host, 1, &DiskSpaceMeter_class);
   Meter_updateValues(space);
   assert(space->total > 0);
   assert(!strcmp(space->txtBuffer, "576G/1.00T 60.0% 384G free"));
   assert(space->values[0] == 576.0 * 1024 * 1024);
   assert(space->total == 960.0 * 1024 * 1024);
   assert(space->txtHighlightLen == 0);
   testMountSample.f_bfree = 256 * 1024;
   testMountSample.f_bavail = 192 * 1024;
   Meter_updateValues(space);
   assert(!strcmp(space->txtBuffer, "768G/1.00T 80.0% 192G free"));
   assert(space->txtHighlightStart == 11 && space->txtHighlightLen == 5);
   testMountSample.f_bfree = 0;
   testMountSample.f_bavail = 0;
   Meter_updateValues(space);
   assert(!strcmp(space->txtBuffer, "1.00T/1.00T 100.0% 0K free"));
   assert(space->txtHighlightLen == 6);
   testMountSample.f_blocks = 0;
   Meter_updateValues(space);
   assert(space->total == 0 && !strcmp(space->txtBuffer, "n/a") && space->txtHighlightLen == 0);
   testMountSample.f_blocks = 1024 * 1024;
   testMountSample.f_bfree = 448 * 1024;
   testMountSample.f_bavail = 384 * 1024;
   Meter_updateValues(space);
   assert(space->txtHighlightLen == 0);
   ListItem* item = Meter_toListItem(space, false);
   Object_delete(item);
   Meter_delete((Object*)space);
   Meter* gpu = Meter_new(&host, 0, &VRAMMeter_class);
   Meter_updateValues(gpu);
   assert(gpu->total == 8 && gpu->values[0] == 2); /* legacy direct construction selects first GPU */
   Meter* one = Meter_new(&host, 2, &VRAMMeter_class);
   Meter_updateValues(one);
   assert(one->total == 16 && one->values[0] == 4);
   assert(one->tempPartCount > 0);
   Meter* missing = Meter_new(&host, 999, &VRAMMeter_class);
   Meter_updateValues(missing);
   assert(missing->total == 0 && !strcmp(missing->txtBuffer, "n/a"));
   Meter_delete((Object*)gpu); Meter_delete((Object*)one); Meter_delete((Object*)missing);
   savedMounts(&host, settings);
   Settings_delete(settings);
   Plus_cleanup();
   puts("Meter deltas, counter reset, duplicate independence, per-GPU samples, missing GPU, temperatures, DSK format/limits, mount-path htoprc round-trip, reorder/missing/override/legacy: PASS");
   return 0;
}
