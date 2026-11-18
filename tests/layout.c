/* Default GPU rows and real ncurses rendering in all modes. GPL-2.0-or-later. */
#include "config.h"
#include "CRT.h"
#include "Header.h"
#include "MemoryMeter.h"
#include "Settings.h"
#include "linux/ExtraMeters.h"
#include "linux/LinuxMachine.h"
#include "linux/PlusDevices.h"
#include "tests/mount-sample.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

const char* program = "htop-plus";

static void checkBar(Meter* meter, const char* expected) {
   char line[81];
   assert(!strcmp(meter->txtBuffer, expected));
   erase();
   meter->draw(meter, 0, 0, 80);
   assert(mvinnstr(0, 0, line, 80) != ERR);
   const char* left = strchr(line, '[');
   const char* right = strchr(line, ']');
   assert(left && right && right > left);
   size_t width = right - left - 1, length = strlen(expected);
   assert(length < width); /* the complete value fits in this column */
   const char* value = right - length;
   for (size_t i = 0; i < length; i++) {
      /* The existing bar renderer fills spaces inside text with bar marks. */
      assert(value[i] == expected[i] || (expected[i] == ' ' && value[i] == '|'));
   }
   assert(!strncmp(line, Meter_getCaption(meter), strlen(Meter_getCaption(meter))));
   /* No change to the colored temperature column outside the bar. */
   if (meter->tempPartCount)
      assert(strstr(right + 1, "55"));
}

static void checkGraphCell(int y, int x, bool utf8) {
#ifdef HAVE_LIBNCURSESW
   /* Read wide cells directly; A_CHARTEXT only describes narrow characters. */
   wchar_t cell[2] = {0};
   assert(mvinnwstr(y, x, cell, 1) != ERR);
   assert(cell[0] == (utf8 ? L'\u28ff' : L':'));
#else
   (void)utf8;
   assert((mvinch(y, x) & A_CHARTEXT) == ':');
#endif
}

static void checkCaptionModes(Meter* meter, const char* expected) {
   const int x = 3, y = 2;
   for (int utf8 = 0; utf8 <= 1; utf8++) {
      CRT_utf8 = utf8;
      for (MeterModeId mode = BAR_METERMODE; mode < LAST_METERMODE; mode++) {
         Meter_setMode(meter, mode);
         assert(meter->mode == mode);
         /* Full and half-width columns must retain the same caption start. */
         for (int width = 74; width >= 37; width -= 37) {
            erase();
            meter->draw(meter, x, y, width);
            if (mode == GRAPH_METERMODE) {
               /* Fill the history so graph pixels exercise the caption boundary. */
               for (size_t i = 0; i < meter->drawData.nValues; i++)
                  meter->drawData.values[i] = meter->total > 0 ? meter->total : 1;
               meter->draw(meter, x, y, width);
               /* Every top-row graph cell must be full, starting after the caption. */
               for (int col = strlen(expected); col < width; col++)
                  checkGraphCell(y, x + col, utf8);
               for (int row = 1; row < meter->h; row++)
                  for (size_t col = 0; col < strlen(expected); col++)
                     assert((mvinch(y + row, x + col) & A_CHARTEXT) == ' ');
            }
            int captionY = mode == LED_METERMODE ? y + (utf8 ? 1 : 2) : y;
            char caption[32];
            assert(mvinnstr(captionY, x, caption, strlen(expected)) != ERR);
            assert(!strcmp(caption, expected));
            assert((mvinch(captionY, x - 1) & A_CHARTEXT) == ' ');
         }
      }
   }
   CRT_utf8 = false;
   Meter_setMode(meter, BAR_METERMODE);
}

int main(int argc, char** argv) {
   assert(argc == 3);
   /* This test explicitly exercises UTF-8 even when the container defaults to C. */
   assert(setlocale(LC_ALL, "C.UTF-8"));
   assert(MB_CUR_MAX > 1);
   unsigned expected = strtoul(argv[1], NULL, 10);
   CRT_degreeSign = "°";
   static const int colors[LAST_COLORELEMENT] = { [METER_VALUE_ERROR] = A_BOLD };
   CRT_colors = colors;
   LinuxMachine linuxHost = {0};
   Machine* host = &linuxHost.super;
   host->monotonicMs = 1000;
   Settings* settings = Settings_new(host, NULL, NULL, NULL);
   host->settings = settings;
   assert(settings->hLayout == HF_ONE_100);
   assert(settings->hColumns[0].len == 8 + expected);
   Header* header = Header_new(host, settings->hLayout);
   Header_populateFromSettings(header);
   unsigned count = 0;
   for (int i = 0; i < header->columns[0]->items; i++) {
      Meter* meter = (Meter*)header->columns[0]->array[i];
      if (As_Meter(meter) != &VRAMMeter_class)
         continue;
      assert(meter->param == ++count);
      Meter_updateValues(meter);
      PlusGpuSample sample;
      assert(Plus_gpuSample(count - 1, &sample));
      assert(meter->total == sample.total / 1024 && meter->values[0] == sample.used / 1024);
      char caption[32];
      if (expected > 1)
         snprintf(caption, sizeof(caption), "VRM%u", count);
      else
         snprintf(caption, sizeof(caption), "VRM");
      assert(!strcmp(Meter_getCaption(meter), caption));
   }
   assert(count == expected);
   /* Simulate a saved pre-fix aggregate selection; it must expand, even at zero GPUs. */
   for (size_t i = 0; i < settings->hColumns[0].len; i++)
      free(settings->hColumns[0].names[i]);
   settings->hColumns[0].len = 1;
   settings->hColumns[0].names[0] = strdup("VRAM");
   settings->hColumns[0].names[1] = NULL;
   settings->hColumns[0].modes[0] = BAR_METERMODE;
   Header_populateFromSettings(header);
   assert(header->columns[0]->items == (int)expected);
   for (unsigned i = 0; i < expected; i++)
      assert(((Meter*)header->columns[0]->array[i])->param == i + 1);

   FILE* output = fopen(argv[2], "w");
   FILE* input = fopen("/dev/null", "r");
   assert(output && input);
   SCREEN* screen = newterm("xterm-256color", output, input);
   assert(screen);
   assert(resizeterm(24, 80) != ERR);
   assert(COLS == 80);
   Meter* memory = Meter_new(host, 0, &MemoryMeter_class);
   host->usedMem = 29.8 * 1024 * 1024;
   host->totalMem = 62.5 * 1024 * 1024;
   Meter_updateValues(memory);
   checkBar(memory, "29.8G/62.5G");
   checkCaptionModes(memory, "RAM");
   Meter_delete((Object*)memory);
   /* Check every GPU identity, including second/third GPUs and unavailable fallback. */
   for (unsigned i = 0; i < (expected ? expected : 1); i++) {
      Meter* gpu = Meter_new(host, i + 1, &VRAMMeter_class);
      Meter_updateValues(gpu);
      char caption[32];
      if (expected > 1)
         snprintf(caption, sizeof(caption), "VRM%u", i + 1);
      else
         snprintf(caption, sizeof(caption), "VRM");
      checkCaptionModes(gpu, caption);
      Meter_delete((Object*)gpu);
   }
   if (expected) {
      Meter* gpu = Meter_new(host, 1, &VRAMMeter_class);
      Meter_updateValues(gpu);
      checkBar(gpu, gpu->txtBuffer);
      /* Validate memory formatting at realistic GPU capacities independently of stub size. */
      char used[16], total[16];
      Meter_humanUnitCompact(used, 1.5 * 1024 * 1024, sizeof(used), true);
      Meter_humanUnitCompact(total, 8.0 * 1024 * 1024, sizeof(total), true);
      snprintf(gpu->txtBuffer, sizeof(gpu->txtBuffer), "%s/%s", used, total);
      checkBar(gpu, "1.5G/8.0G");
      Meter_delete((Object*)gpu);
   }
   /* A fixture mount with deterministic reserved blocks exercises the real formatter. */
   assert(setenv("HTOP_PLUS_MOUNT", "/", 1) == 0);
   Plus_cleanup();
   Meter* disk = Meter_new(host, 1, &DiskSpaceMeter_class);
   Meter_updateValues(disk);
   checkBar(disk, "576G/1.00T 60.0% 384G free");
   checkCaptionModes(disk, "DSK");
   testMountSample.f_bfree = 256 * 1024;
   testMountSample.f_bavail = 192 * 1024;
   Meter_updateValues(disk);
   checkBar(disk, "768G/1.00T 80.0% 192G free");
   char line[81];
   assert(mvinnstr(0, 0, line, 80) != ERR);
   const char* percent = strstr(line, "80.0%");
   assert(percent);
   for (int i = 0; i < 5; i++)
      assert((mvinch(0, (int)(percent - line) + i) & A_BOLD) != 0);
   assert((mvinch(0, (int)(percent - line) - 1) & A_BOLD) == 0);
   assert((mvinch(0, (int)(percent - line) + 5) & A_BOLD) == 0);
   Meter_setMode(disk, TEXT_METERMODE);
   erase();
   disk->draw(disk, 0, 0, 80);
   assert(mvinnstr(0, 0, line, 80) != ERR);
   assert(strstr(line, disk->txtBuffer));
   percent = strstr(line, "80.0%");
   assert(percent);
   for (int i = 0; i < 5; i++)
      assert((mvinch(0, (int)(percent - line) + i) & A_BOLD) != 0);
   /* Cooling below the limit clears both bar and text highlighting. */
   testMountSample.f_bfree = 448 * 1024;
   testMountSample.f_bavail = 384 * 1024;
   Meter_updateValues(disk);
   assert(disk->txtHighlightLen == 0);
   erase();
   disk->draw(disk, 0, 0, 80);
   assert(mvinnstr(0, 0, line, 80) != ERR);
   percent = strstr(line, "60.0%");
   assert(percent && (mvinch(0, (int)(percent - line)) & A_BOLD) == 0);
   Meter_delete((Object*)disk);
   Meter* ssd = Meter_new(host, 1, &SSDMeter_class);
   char read[16], write[16];
   Meter_humanUnitCompact(read, 13.2 * 1024, sizeof(read), false);
   Meter_humanUnitCompact(write, 2.95 * 1024, sizeof(write), false);
   snprintf(ssd->txtBuffer, sizeof(ssd->txtBuffer), "%.0f%% R%s W%s", 86.4, read, write);
   checkBar(ssd, "86% R13M W3M");
   Meter_delete((Object*)ssd);
   endwin();
   delscreen(screen);
   fclose(input);
   fclose(output);
   Header_delete(header);
   Settings_delete(settings);
   Plus_cleanup();
   printf("Default and legacy GPU rows: %u; full captions in bar/text/graph/LED (ASCII/UTF-8, full/half width), aligned bars and exact DSK text/limit highlighting: PASS\n", expected);
   return 0;
}
