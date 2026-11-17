/*
htop - linux/ExtraMeters.c
SSD (busy % + read/write speed), disk space and NVIDIA VRAM meters for htop-plus,
plus CPU/SSD/RAM/GPU temperatures shown on those meters' lines.
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/ExtraMeters.h"

#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "CPUMeter.h"
#include "CRT.h"
#include "Machine.h"
#include "Macros.h"
#include "Meter.h"
#include "Object.h"
#include "RichString.h"
#include "XUtils.h"


/* ---------------------------------------------------------------- Disk meters */
#include "linux/PlusDevices.h"

typedef struct TempSensor_ TempSensor;

typedef struct {
   PlusDiskSample prev;
   uint64_t prevMs;
   bool valid;
   double readRate, writeRate;
   TempSensor* temperature;
} DiskState;

static const int SSDMeter_attributes[] = { METER_VALUE_IOWRITE };
static const int DiskSpaceMeter_attributes[] = { METER_VALUE_IOWRITE };

static const char* SSDMeter_device(const Meter* meter) {
   const PlusDevices* d = Plus_devices();
   size_t index = meter->param ? meter->param - 1 : 0;
   return index < d->diskCount ? d->disks[index].name : "";
}

static const char* DiskSpaceMeter_mount(const Meter* meter) {
   const PlusDevices* d = Plus_devices();
   size_t index = meter->param ? meter->param - 1 : 0;
   return index < d->mountCount ? d->mounts[index].path : "";
}

static void DiskMeter_init(Meter* this) {
   if (As_Meter(this) == &SSDMeter_class && !this->meterData)
      this->meterData = xCalloc(1, sizeof(DiskState));
}

static void DiskMeter_done(Meter* this) {
   if (As_Meter(this) == &SSDMeter_class && this->meterData)
      free(((DiskState*)this->meterData)->temperature);
   free(this->meterData);
   this->meterData = NULL;
}

static void DiskMeter_name(const Meter* this, char* out, size_t size) {
   const char* name = As_Meter(this) == &SSDMeter_class ? SSDMeter_device(this) : DiskSpaceMeter_mount(this);
   snprintf(out, size, "%s: %s", Meter_uiName(this), *name ? name : "n/a");
}

static void DiskMeter_unavailable(Meter* this) {
   this->total = 0;
   this->values[0] = 0;
   this->tempPartCount = 0;
   this->txtHighlightLen = 0;
   strcpy(this->txtBuffer, "n/a");
}

static void SSDMeter_updateValues(Meter* this) {
   DiskState* s = this->meterData;
   PlusDiskSample sample;
   if (!Plus_diskSample(SSDMeter_device(this), &sample)) {
      s->valid = false;
      DiskMeter_unavailable(this);
      return;
   }
   uint64_t now = this->host->monotonicMs;
   double busy = this->values[0];
   if (!s->valid || sample.read < s->prev.read || sample.write < s->prev.write || sample.ticks < s->prev.ticks) {
      busy = s->readRate = s->writeRate = 0;
      s->prev = sample;
      s->prevMs = now;
   } else if (now > s->prevMs) {
      double dt = now - s->prevMs;
      busy = CLAMP(100.0 * (sample.ticks - s->prev.ticks) / dt, 0.0, 100.0);
      s->readRate = (sample.read - s->prev.read) * 500.0 / dt; /* KiB/s, sectors = 512 bytes */
      s->writeRate = (sample.write - s->prev.write) * 500.0 / dt;
      s->prev = sample;
      s->prevMs = now;
   }
   s->valid = true;
   this->total = 100;
   this->values[0] = busy;
   char rd[16], wr[16];
   Meter_humanUnitCompact(rd, s->readRate, sizeof(rd), false);
   Meter_humanUnitCompact(wr, s->writeRate, sizeof(wr), false);
   snprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0f%% R%s W%s", busy, rd, wr);
   ExtraMeters_temperature(EXTRA_TEMP_SSD, this, false);
}

static void DiskSpaceMeter_updateValues(Meter* this) {
   double used, size, avail;
   const char* mount = DiskSpaceMeter_mount(this);
   if (!*mount || !Plus_mountSample(mount, &used, &size, &avail)) {
      DiskMeter_unavailable(this);
      return;
   }
   this->total = used + avail;
   this->values[0] = used;
   char a[16];
   Meter_humanUnitCompact(a, avail, sizeof(a), false);
   snprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0f%% %s free", 100.0 * used / this->total, a);
}

const MeterClass SSDMeter_class = {
   .super = { .extends = Class(Meter), .delete = Meter_delete },
   .init = DiskMeter_init, .done = DiskMeter_done, .getUiName = DiskMeter_name,
   .updateValues = SSDMeter_updateValues, .defaultMode = BAR_METERMODE,
   .supportedModes = METERMODE_DEFAULT_SUPPORTED, .maxItems = 1, .total = 100,
   .attributes = SSDMeter_attributes, .name = "SSD", .uiName = "SSD busy + read/write",
   .caption = "SSD", .captionColor = METER_CAPTION_SSD,
};

const MeterClass DiskSpaceMeter_class = {
   .super = { .extends = Class(Meter), .delete = Meter_delete },
   .getUiName = DiskMeter_name, .updateValues = DiskSpaceMeter_updateValues,
   .defaultMode = BAR_METERMODE, .supportedModes = METERMODE_DEFAULT_SUPPORTED,
   .maxItems = 1, .total = 100, .attributes = DiskSpaceMeter_attributes,
   .name = "DiskSpace", .uiName = "Mount space used/free", .caption = "DSK", .captionColor = METER_CAPTION_DISK,
};

/* ---------------------------------------------------------------- Temperatures */

#define TEMP_MAX_INPUTS 4
#define TEMP_PATH_LEN (2 * NAME_MAX + 96)

/* Recent peaks and heat color of one component, kept across updates */
typedef struct {
   double minuteMax[60];   /* °C, highest per minute, slot = minute % 60 */
   uint64_t minuteOf[60];  /* which minute (+1) each slot holds; 0 = empty */
   int color;              /* heat color of the latest reading */
   uint64_t colorSinceMs;  /* when the reading turned that color; 0 = no reading yet */
} TempHistory;

/* One component's hwmon sensor, found by hwmon name (and temp label) and cached until a read fails */
struct TempSensor_ {
   const char* hwmonName;
   const char* label;      /* tempN_label to match; NULL = temp1 of every matching hwmon (one per DIMM) */
   char inputs[TEMP_MAX_INPUTS][TEMP_PATH_LEN];
   int count;
   double limit;           /* °C, NAN when the sensor reports none */
   TempHistory history;
   uint64_t lastReadMs, nextSearchMs;
   double latest;
   bool sampled;
};

static TempSensor ExtraMeters_temps[EXTRA_TEMP_COUNT] = {
   [EXTRA_TEMP_CPU] = { .hwmonName = "coretemp", .label = "Package id 0" },
   [EXTRA_TEMP_SSD] = { .hwmonName = "nvme", .label = "Composite" },
   [EXTRA_TEMP_RAM] = { .hwmonName = "spd5118" },
};

static bool Temp_readLine(const char* path, char* buf, size_t size) {
   char rooted[PATH_MAX];
   if (!Plus_path(rooted, sizeof(rooted), path))
      return false;
   FILE* fp = fopen(rooted, "r");
   if (!fp)
      return false;
   bool ok = fgets(buf, (int)size, fp) != NULL;
   fclose(fp);
   if (ok)
      buf[strcspn(buf, "\n")] = '\0';
   return ok;
}

/* hwmon values are millidegrees */
static double Temp_readMilli(const char* path) {
   char buf[32], *end;
   if (!Temp_readLine(path, buf, sizeof(buf)))
      return NAN;
   long value = strtol(buf, &end, 10);
   return end == buf ? NAN : (double)value / 1000.0;
}

static void Temp_addInput(TempSensor* t, const char* dir, int n) {
   if (t->count >= TEMP_MAX_INPUTS)
      return;
   xSnprintf(t->inputs[t->count++], sizeof(t->inputs[0]), "%s/temp%d_input", dir, n);

   /* Prefer the critical limit: spd5118's "max" is an alarm threshold below normal running temps */
   char path[TEMP_PATH_LEN + 32];
   xSnprintf(path, sizeof(path), "%s/temp%d_crit", dir, n);
   double limit = Temp_readMilli(path);
   if (!(limit > 0.0 && limit < 200.0)) {
      xSnprintf(path, sizeof(path), "%s/temp%d_max", dir, n);
      limit = Temp_readMilli(path);
   }
   if (limit > 0.0 && limit < 200.0 && (isNaN(t->limit) || limit < t->limit))
      t->limit = limit;
}

static void Temp_search(TempSensor* t, const char* base) {
   t->count = 0;
   t->limit = NAN;
   char rooted[PATH_MAX];
   if (!Plus_path(rooted, sizeof(rooted), base))
      return;
   DIR* dir = opendir(rooted);
   if (!dir)
      return;
   const struct dirent* entry;
   while ((entry = readdir(dir)) != NULL) {
      if (!String_startsWith(entry->d_name, "hwmon"))
         continue;
      char hwmon[TEMP_PATH_LEN], path[TEMP_PATH_LEN + 32], buf[64];
      xSnprintf(hwmon, sizeof(hwmon), "%s/%s", base, entry->d_name);
      xSnprintf(path, sizeof(path), "%s/name", hwmon);
      if (!Temp_readLine(path, buf, sizeof(buf)) || !String_eq(buf, t->hwmonName))
         continue;
      if (!t->label) {
         Temp_addInput(t, hwmon, 1);
         continue;
      }
      for (int n = 1; n <= 64; n++) {
         xSnprintf(path, sizeof(path), "%s/temp%d_label", hwmon, n);
         if (Temp_readLine(path, buf, sizeof(buf)) && String_eq(buf, t->label)) {
            Temp_addInput(t, hwmon, n);
            break;
         }
      }
   }
   closedir(dir);
}

/* The Fahrenheit setting only exists in libsensors builds */
static bool Temp_fahrenheit(const Settings* settings) {
   #ifdef BUILD_WITH_CPU_TEMP
   return settings->degreeFahrenheit;
   #else
   (void) settings;
   return false;
   #endif
}

static double Temp_unit(const Settings* settings, double celsius) {
   return Temp_fahrenheit(settings) ? celsius * 9.0 / 5.0 + 32.0 : celsius;
}

/* Green below 70% of the hardware limit, yellow to 85%, red above; plain when the limit is unknown */
static int Temp_color(double celsius, double limit) {
   if (isNaN(limit))
      return METER_TEXT;
   if (celsius < 0.70 * limit)
      return METER_VALUE_OK;
   if (celsius < 0.85 * limit)
      return METER_VALUE_WARN;
   return METER_VALUE_ERROR;
}

/* Highest reading over the past hour, kept in one-minute slots */
static double Temp_hourPeak(TempHistory* h, double now, uint64_t nowMs) {
   uint64_t minute = nowMs / 60000 + 1;
   size_t slot = minute % 60;
   if (h->minuteOf[slot] != minute || now > h->minuteMax[slot]) {
      h->minuteMax[slot] = now;
      h->minuteOf[slot] = minute;
   }
   double peak = now;
   for (size_t i = 0; i < 60; i++)
      if (h->minuteOf[i] && minute - h->minuteOf[i] < 60 && h->minuteMax[i] > peak)
         peak = h->minuteMax[i];
   return peak;
}

/* "77° 89°C/100°C 3m12s": now, peak over the past hour, limit (left out when unknown),
   and optionally how long it has been in its current color */
static void Temp_format(Meter* meter, TempHistory* h, double now, double limit, bool showTimer) {
   const Settings* settings = meter->host->settings;
   uint64_t nowMs = meter->host->monotonicMs;
   double peak = Temp_hourPeak(h, now, nowMs);
   int color = Temp_color(now, limit);
   /* Cooling only drops a color 2° below its line, so a reading hovering on it does not keep resetting the timer */
   if (h->colorSinceMs != 0 && color != h->color && Temp_color(now + 2.0, limit) == h->color)
      color = h->color;
   if (color != h->color || h->colorSinceMs == 0) {
      h->color = color;
      h->colorSinceMs = nowMs;
   }

   const char* unit = Temp_fahrenheit(settings) ? "F" : "C";
   MeterTempPart* part = meter->tempParts;
   int n = 0;
   xSnprintf(part[n].text, sizeof(part[n].text), "%.0f%s", Temp_unit(settings, now), CRT_degreeSign);
   part[n++].color = color;
   xSnprintf(part[n].text, sizeof(part[n].text), " %.0f%s%s", Temp_unit(settings, peak), CRT_degreeSign, unit);
   part[n++].color = Temp_color(peak, limit);
   if (!isNaN(limit)) {
      xSnprintf(part[n].text, sizeof(part[n].text), "/%.0f%s%s", Temp_unit(settings, limit), CRT_degreeSign, unit);
      part[n++].color = METER_TEXT;
   }
   if (showTimer) {
      char duration[12];
      CPUMeter_formatDuration(duration, sizeof(duration), nowMs - h->colorSinceMs);
      xSnprintf(part[n].text, sizeof(part[n].text), " %s", duration);
      part[n++].color = color;
   }
   meter->tempPartCount = (uint8_t)n;
}

void ExtraMeters_temperature(ExtraTemp which, Meter* meter, bool showTimer) {
   TempSensor* t = &ExtraMeters_temps[which];
   if (which == EXTRA_TEMP_SSD) {
      DiskState* state = meter->meterData;
      if (!state->temperature) {
         state->temperature = xCalloc(1, sizeof(TempSensor));
         state->temperature->hwmonName = "nvme";
         state->temperature->label = "Composite";
      }
      t = state->temperature;
   }
   meter->tempPartCount = 0;
   uint64_t nowMs = meter->host->monotonicMs;
   /* Sysfs sensor reads can be expensive. Reuse readings for five seconds and
      back off missing-sensor discovery for one minute. Timers still update. */
   if (t->sampled && nowMs >= t->lastReadMs && nowMs - t->lastReadMs < 5000) {
      if (!isNaN(t->latest))
         Temp_format(meter, &t->history, t->latest, t->limit, showTimer);
      return;
   }
   if (t->count == 0) {
      if (nowMs < t->nextSearchMs)
         return;
      t->nextSearchMs = nowMs + 60000;
      char base[NAME_MAX + 32];
      /* The SSD's own controller, so a second NVMe drive is not picked up instead */
      if (which == EXTRA_TEMP_SSD)
         xSnprintf(base, sizeof(base), "/sys/block/%s/device", SSDMeter_device(meter));
      else
         xSnprintf(base, sizeof(base), "/sys/class/hwmon");
      Temp_search(t, base);
   }

   /* Several inputs (one per DIMM): show the hottest */
   double now = NAN;
   for (int i = 0; i < t->count; i++) {
      double value = Temp_readMilli(t->inputs[i]);
      if (!isNaN(value) && (isNaN(now) || value > now))
         now = value;
   }
   t->sampled = true;
   t->lastReadMs = nowMs;
   t->latest = now;
   if (isNaN(now)) {
      t->count = 0; /* search again next time, e.g. after a driver reload */
      return;
   }
   Temp_format(meter, &t->history, now, t->limit, showTimer);
}


/* ---------------------------------------------------------------- VRAM */
static const int VRAMMeter_attributes[] = { MEMORY_USED };

typedef struct { TempHistory history; } GpuState;

static void VRAMMeter_init(Meter* this) {
   if (!this->meterData)
      this->meterData = xCalloc(1, sizeof(GpuState));
   const PlusDevices* d = Plus_devices();
   size_t index = this->param ? this->param - 1 : 0;
   char caption[32];
   if (d->gpuCount > 1)
      snprintf(caption, sizeof(caption), "VRM%zu", index + 1);
   else
      snprintf(caption, sizeof(caption), "VRM");
   Meter_setCaption(this, caption);
}

static void VRAMMeter_name(const Meter* this, char* out, size_t size) {
   const PlusDevices* d = Plus_devices();
   snprintf(out, size, "VRAM: %s", (this->param ? this->param - 1 : 0) < d->gpuCount ?
             d->gpus[this->param ? this->param - 1 : 0].name : "n/a");
}

static void VRAMMeter_updateValues(Meter* this) {
   const PlusDevices* d = Plus_devices();
   size_t index = this->param ? this->param - 1 : 0;
   PlusGpuSample sample = {0};
   bool complete = index < d->gpuCount && Plus_gpuSample(index, &sample);
   double used = sample.used, total = sample.total;
   this->tempPartCount = 0;
   this->txtHighlightLen = 0;
   if (!complete || total <= 0) {
      DiskMeter_unavailable(this);
      if (sample.asleep)
         strcpy(this->txtBuffer, "asleep");
      return;
   }
   this->total = total / 1024.0;
   this->values[0] = used / 1024.0;
   char u[16], t[16];
   Meter_humanUnitCompact(u, this->values[0], sizeof(u), true);
   Meter_humanUnitCompact(t, this->total, sizeof(t), true);
   xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%s/%s", u, t);
   if (!isNaN(sample.temperature)) {
      GpuState* s = this->meterData;
      Temp_format(this, &s->history, sample.temperature, sample.limit, false);
   }
}

const MeterClass VRAMMeter_class = {
   .super = { .extends = Class(Meter), .delete = Meter_delete },
   .init = VRAMMeter_init, .done = DiskMeter_done, .getUiName = VRAMMeter_name,
   .updateValues = VRAMMeter_updateValues, .defaultMode = BAR_METERMODE,
   .supportedModes = METERMODE_DEFAULT_SUPPORTED, .maxItems = 1, .total = 100,
   .attributes = VRAMMeter_attributes, .name = "VRAM", .uiName = "GPU video memory",
   .caption = "VRM", .captionColor = METER_CAPTION_VRAM, .fullCaption = true,
};
