/* Test library, never installed. GPL-2.0-or-later. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linux/PlusNvml.h"
static void mark(char event) {
   const char* path = getenv("NVML_TEST_TRACE");
   if (!path) return;
   FILE* fp = fopen(path, "a");
   if (fp) { fputc(event, fp); fclose(fp); }
}
int nvmlInit_v2(void) { mark('I'); return getenv("NVML_TEST_FAIL") ? 1 : 0; }
int nvmlShutdown(void) { mark('S'); return 0; }
int nvmlDeviceGetCount_v2(unsigned* count) {
   const char* n = getenv("NVML_TEST_COUNT");
   *count = n ? (unsigned)atoi(n) : 0;
   return 0;
}
int nvmlDeviceGetHandleByIndex_v2(unsigned index, void** out) {
   unsigned count; nvmlDeviceGetCount_v2(&count);
   if (index >= count) return 1;
   *out = (void*)(uintptr_t)(index + 1); mark('H'); return 0;
}
int nvmlDeviceGetHandleByPciBusId_v2(const char* pci, void** out) {
   unsigned bus = 0;
   if (sscanf(pci, "%*x:%x", &bus) != 1 || !bus) return 1;
   return nvmlDeviceGetHandleByIndex_v2(bus - 1, out);
}
int nvmlDeviceGetMemoryInfo(void* device, nvmlMemory_t* out) {
   mark('M');
   if (getenv("NVML_TEST_MEMORY_FAIL")) return 1;
   unsigned long long i = (uintptr_t)device;
   *out = (nvmlMemory_t){ .total = i * 8192, .free = i * 6144, .used = i * 2048 };
   return 0;
}
#ifndef NVML_TEST_NO_TEMPERATURE
int nvmlDeviceGetTemperature(void* device, int kind, unsigned* value) {
   (void)device; (void)kind; *value = 55; return 0;
}
int nvmlDeviceGetTemperatureThreshold(void* device, int kind, unsigned* value) {
   (void)device; (void)kind; *value = 95; return 0;
}
#endif
