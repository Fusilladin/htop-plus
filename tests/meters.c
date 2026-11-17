/* Exercise real meter callbacks and Header persistence. GPL-2.0-or-later. */
#include "config.h"
#include "linux/ExtraMeters.h"
#include "linux/PlusDevices.h"
#include "CRT.h"
#include "Header.h"
#include "Settings.h"
#include <assert.h>
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
   Settings* settings = calloc(1, sizeof(*settings));
   assert(settings);
   settings->hLayout = HF_TWO_50_50;
   settings->hColumns = calloc(2, sizeof(*settings->hColumns));
   assert(settings->hColumns);
   Machine host = { .settings = settings, .monotonicMs = 1000 };
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
   assert(strlen(space->txtBuffer) <= 15 && !strchr(space->txtBuffer, '/'));
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
   Header* header = Header_new(&host, HF_TWO_50_50);
   Header_addMeterByClass(header, &SSDMeter_class, 2, 0);
   Header_addMeterByClass(header, &DiskSpaceMeter_class, 3, 0);
   Header_addMeterByClass(header, &VRAMMeter_class, 2, 1);
   Header_writeBackToSettings(header);
   assert(!strcmp(settings->hColumns[0].names[0], "SSD(2)"));
   assert(!strcmp(settings->hColumns[0].names[1], "DiskSpace(3)"));
   assert(!strcmp(settings->hColumns[1].names[0], "VRAM(2)"));
   Header_populateFromSettings(header);
   assert(((Meter*)header->columns[0]->array[0])->param == 2);
   assert(((Meter*)header->columns[0]->array[1])->param == 3);
   assert(((Meter*)header->columns[1]->array[0])->param == 2);
   Header_delete(header);
   Settings_delete(settings);
   Plus_cleanup();
   puts("Meter deltas, counter reset, duplicate independence, per-GPU samples, missing GPU, temperatures and config round-trip: PASS");
   return 0;
}
