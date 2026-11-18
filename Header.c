/*
htop - Header.c
(C) 2004-2011 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "Header.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CRT.h"
#include "CPUMeter.h"
#include "DynamicMeter.h"
#include "Macros.h"
#include "Object.h"
#include "Platform.h"
#include "ProvideCurses.h"
#include "Settings.h"
#include "XUtils.h"
#ifdef HTOP_LINUX
#include "linux/ExtraMeters.h"
#include "linux/PlusDevices.h"
#endif


Header* Header_new(Machine* host, HeaderLayout hLayout) {
   Header* this = xCalloc(1, sizeof(Header));
   this->columns = xMallocArray(HeaderLayout_getColumns(hLayout), sizeof(Vector*));
   this->headerLayout = hLayout;
   this->host = host;

   Header_forEachColumn(this, i) {
      this->columns[i] = Vector_new(Class(Meter), true, DEFAULT_SIZE);
   }

   return this;
}

void Header_delete(Header* this) {
   Header_forEachColumn(this, i) {
      Vector_delete(this->columns[i]);
   }

   free(this->columns);
   free(this);
}

void Header_setLayout(Header* this, HeaderLayout hLayout) {
   size_t oldColumns = HeaderLayout_getColumns(this->headerLayout);
   size_t newColumns = HeaderLayout_getColumns(hLayout);

   this->headerLayout = hLayout;

   if (newColumns == oldColumns)
      return;

   if (newColumns > oldColumns) {
      this->columns = xReallocArray(this->columns, newColumns, sizeof(Vector*));
      for (size_t i = oldColumns; i < newColumns; i++)
         this->columns[i] = Vector_new(Class(Meter), true, DEFAULT_SIZE);
   } else {
      // move meters from to-be-deleted columns into last one
      for (size_t i = newColumns; i < oldColumns; i++) {
         for (int j = this->columns[i]->items - 1; j >= 0; j--) {
            Vector_add(this->columns[newColumns - 1], Vector_take(this->columns[i], j));
         }
         Vector_delete(this->columns[i]);
      }
      this->columns = xReallocArray(this->columns, newColumns, sizeof(Vector*));
   }

   Header_calculateHeight(this);
}

#ifdef HTOP_LINUX
/* Meter names are space-separated htoprc tokens. Escape separators, parentheses,
   percent signs and non-ASCII bytes while keeping ordinary paths readable. */
static char* Header_encodeMount(const char* mount) {
   static const char hex[] = "0123456789ABCDEF";
   char* encoded = xMalloc(3 * strlen(mount) + 1);
   char* next = encoded;
   for (const unsigned char* p = (const unsigned char*)mount; *p; p++) {
      if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || strchr("/._-", *p)) {
         *next++ = *p;
      } else {
         *next++ = '%';
         *next++ = hex[*p >> 4];
         *next++ = hex[*p & 15];
      }
   }
   *next = '\0';
   return encoded;
}

static int Header_mountHex(char c) {
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   return -1;
}

static bool Header_decodeMount(const char* token, char* mount, size_t size) {
   size_t n = 0;
   const char* p;
   for (p = token; *p != ')'; p++) {
      if (!*p || n + 1 >= size)
         return false;
      unsigned char c = *p;
      if (c == '%') {
         if (!p[1] || !p[2])
            return false;
         int hi = Header_mountHex(p[1]), lo = Header_mountHex(p[2]);
         if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
            return false;
         c = (unsigned char)((hi << 4) | lo);
         p += 2;
      }
      mount[n++] = c;
   }
   mount[n] = '\0';
   return p[1] == '\0' && n > 0 && mount[0] == '/';
}
#endif

static void Header_addMeterByName(Header* this, const char* name, MeterModeId mode, unsigned int column) {
   assert(column < HeaderLayout_getColumns(this->headerLayout));

   Vector* meters = this->columns[column];

#ifdef HTOP_LINUX
   if (String_startsWith(name, "DiskSpace(/")) {
      char mount[PATH_MAX];
      if (!Header_decodeMount(name + strlen("DiskSpace("), mount, sizeof(mount)))
         return;
      Meter* meter = Meter_new(this->host, 0, &DiskSpaceMeter_class);
      if (!DiskSpaceMeter_setMount(meter, mount)) {
         Meter_delete((Object*)meter);
         return;
      }
      if (mode != 0)
         Meter_setMode(meter, mode);
      Vector_add(meters, meter);
      return;
   }
#endif

   const char* paren = strchr(name, '(');
   unsigned int param = 0;
   size_t nameLen;
   if (paren) {
      if (sscanf(paren, "(%10u)", &param) != 1) { // not CPUMeter
         char dynamic[32] = {0};
         if (sscanf(paren, "(%30s)", dynamic) == 1) { // DynamicMeter
            char* end;
            if ((end = strrchr(dynamic, ')')) == NULL)
               return;    // htoprc parse failure
            *end = '\0';
            const Settings* settings = this->host->settings;
            if (!DynamicMeter_search(settings->dynamicMeters, dynamic, &param))
               return;    // name lookup failure
         } else {
            param = 0;
         }
      }
      nameLen = paren - name;
   } else {
      nameLen = strlen(name);
   }

   for (const MeterClass* const* type = Platform_meterTypes; *type; type++) {
      if (0 == strncmp(name, (*type)->name, nameLen) && (*type)->name[nameLen] == '\0') {
#ifdef HTOP_LINUX
         /* Legacy unparameterized VRAM now means one row for every GPU. */
         if (*type == &VRAMMeter_class && param == 0) {
            size_t count = Plus_devices()->gpuCount;
            for (size_t i = 0; i < count; i++) {
               Meter* meter = Meter_new(this->host, (unsigned int)i + 1, *type);
               if (mode != 0)
                  Meter_setMode(meter, mode);
               Vector_add(meters, meter);
            }
            break;
         }
#endif
         Meter* meter = Meter_new(this->host, param, *type);
         if (mode != 0) {
            Meter_setMode(meter, mode);
         }
         Vector_add(meters, meter);
         break;
      }
   }
}

void Header_populateFromSettings(Header* this) {
   const Settings* settings = this->host->settings;
   Header_setLayout(this, settings->hLayout);

   Header_forEachColumn(this, col) {
      const MeterColumnSetting* colSettings = &settings->hColumns[col];
      Vector_prune(this->columns[col]);
      for (size_t i = 0; i < colSettings->len; i++) {
         Header_addMeterByName(this, colSettings->names[i], colSettings->modes[i], col);
      }
   }

   Header_calculateHeight(this);
}

void Header_writeBackToSettings(const Header* this) {
   Settings* settings = this->host->settings;
   Settings_setHeaderLayout(settings, this->headerLayout);

   Header_forEachColumn(this, col) {
      MeterColumnSetting* colSettings = &settings->hColumns[col];

      if (colSettings->names) {
         for (size_t j = 0; j < colSettings->len; j++)
            free(colSettings->names[j]);
         free(colSettings->names);
      }
      free(colSettings->modes);

      const Vector* vec = this->columns[col];
      int len = Vector_size(vec);

      colSettings->names = len ? xCalloc(len + 1, sizeof(*colSettings->names)) : NULL;
      colSettings->modes = len ? xCalloc(len, sizeof(*colSettings->modes)) : NULL;
      colSettings->len = len;

      for (int i = 0; i < len; i++) {
         const Meter* meter = (Meter*) Vector_get(vec, i);
         char* name;
#ifdef HTOP_LINUX
         if (As_Meter(meter) == &DiskSpaceMeter_class && *DiskSpaceMeter_getMount(meter)) {
            char* mount = Header_encodeMount(DiskSpaceMeter_getMount(meter));
            xAsprintf(&name, "%s(%s)", As_Meter(meter)->name, mount);
            free(mount);
         } else
#endif
         if (meter->param && As_Meter(meter) == &DynamicMeter_class) {
            const char* dynamic = DynamicMeter_lookup(settings->dynamicMeters, meter->param);
            xAsprintf(&name, "%s(%s)", As_Meter(meter)->name, dynamic);
         } else if (meter->param) {
            xAsprintf(&name, "%s(%u)", As_Meter(meter)->name, meter->param);
         } else {
            xAsprintf(&name, "%s", As_Meter(meter)->name);
         }
         colSettings->names[i] = name;
         colSettings->modes[i] = meter->mode;
      }
   }
}

Meter* Header_addMeterByClass(Header* this, const MeterClass* type, unsigned int param, unsigned int column) {
   assert(column < HeaderLayout_getColumns(this->headerLayout));

   Vector* meters = this->columns[column];

   Meter* meter = Meter_new(this->host, param, type);
   Vector_add(meters, meter);
   return meter;
}

void Header_reinit(Header* this) {
   Header_forEachColumn(this, col) {
      for (int i = 0; i < Vector_size(this->columns[col]); i++) {
         Meter* meter = (Meter*) Vector_get(this->columns[col], i);
         if (Meter_initFn(meter)) {
            Meter_init(meter);
         }
      }
   }
}

void Header_draw(const Header* this) {
   const int height = this->height;
   const int pad = this->pad;
   attrset(CRT_colors[RESET_COLOR]);
   for (int y = 0; y < height; y++) {
      mvhline(y, 0, ' ', COLS);
   }
   const int numCols = HeaderLayout_getColumns(this->headerLayout);
   const int width = COLS - 2 * pad - (numCols - 1);
   int x = pad;
   float roundingLoss = 0.0F;

   Header_forEachColumn(this, col) {
      Vector* meters = this->columns[col];
      float colWidth = (float)width * HeaderLayout_layouts[this->headerLayout].widths[col] / 100.0F;

      roundingLoss += colWidth - floorf(colWidth);
      if (roundingLoss >= 1.0F) {
         colWidth += 1.0F;
         roundingLoss -= 1.0F;
      }

      for (int y = (pad / 2), i = 0; i < Vector_size(meters); i++) {
         Meter* meter = (Meter*) Vector_get(meters, i);

         float actualWidth = colWidth;

         /* Let meters in text mode expand to the right on empty neighbors;
            except for multi column meters. */
         if (meter->mode == TEXT_METERMODE && !Meter_isMultiColumn(meter)) {
            for (int j = 1; j < meter->columnWidthCount; j++) {
               actualWidth++; /* separator column */
               actualWidth += (float)width * HeaderLayout_layouts[this->headerLayout].widths[col + j] / 100.0F;
            }
         }

         assert(meter->draw);
         meter->draw(meter, x, y, floorf(actualWidth));
         y += meter->h;
      }

      x += floorf(colWidth);
      x++; /* separator column */
   }
}

void Header_updateData(Header* this) {
   Header_forEachColumn(this, col) {
      Vector* meters = this->columns[col];
      int items = Vector_size(meters);
      for (int i = 0; i < items; i++) {
         Meter* meter = (Meter*) Vector_get(meters, i);
         Meter_updateValues(meter);
      }
   }
}

/*
 * Calculate how many columns the current meter is allowed to span,
 * by counting how many columns to the right are empty or contain a BlankMeter.
 * Returns the number of columns to span, i.e. if the direct neighbor is occupied 1.
 */
static int calcColumnWidthCount(const Header* this, const Meter* curMeter, const int pad, const unsigned int curColumn, const int curHeight) {
   for (size_t i = curColumn + 1; i < HeaderLayout_getColumns(this->headerLayout); i++) {
      const Vector* meters = this->columns[i];

      int height = pad;
      for (int j = 0; j < Vector_size(meters); j++) {
         const Meter* meter = (const Meter*) Vector_get(meters, j);

         if (height >= curHeight + curMeter->h)
            break;

         height += meter->h;
         if (height <= curHeight)
            continue;

         if (!Object_isA((const Object*) meter, (const ObjectClass*) &BlankMeter_class))
            return i - curColumn;
      }
   }

   return HeaderLayout_getColumns(this->headerLayout) - curColumn;
}

int Header_calculateHeight(Header* this) {
   const Settings* settings = this->host->settings;
   const int pad = settings->headerMargin ? 2 : 0;
   int maxHeight = pad;

   Header_forEachColumn(this, col) {
      const Vector* meters = this->columns[col];
      int height = pad;
      for (int i = 0; i < Vector_size(meters); i++) {
         Meter* meter = (Meter*) Vector_get(meters, i);
         meter->columnWidthCount = calcColumnWidthCount(this, meter, pad, col, height);
         height += meter->h;
      }
      maxHeight = MAXIMUM(maxHeight, height);
   }

   if (maxHeight == pad) {
      maxHeight = 0;
      this->pad = 0;
   } else {
      this->pad = pad;
   }

   if (settings->screenTabs) {
      maxHeight++;
   }

   this->height = maxHeight;

   return maxHeight;
}
