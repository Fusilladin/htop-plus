#ifndef HEADER_ExtraMeters
#define HEADER_ExtraMeters
/*
htop - linux/ExtraMeters.h
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include <stdbool.h>

#include "Meter.h"
#include "Settings.h"

typedef enum {
   EXTRA_TEMP_CPU,
   EXTRA_TEMP_SSD,
   EXTRA_TEMP_RAM,
   EXTRA_TEMP_COUNT
} ExtraTemp;

/* htop-plus: fills meter->tempParts with "77° 89°C/100°C" (now, peak over the past hour, limit), colored by heat,
   plus how long it has been that color when showTimer; no parts without a sensor */
void ExtraMeters_temperature(ExtraTemp which, Meter* meter, bool showTimer);

/* Saved selection (before HTOP_PLUS_MOUNT override), retained even when unmounted. */
const char* DiskSpaceMeter_getMount(const Meter* meter);
bool DiskSpaceMeter_setMount(Meter* meter, const char* mount);

extern const MeterClass DiskSpaceMeter_class;


extern const MeterClass SSDMeter_class;

extern const MeterClass VRAMMeter_class;

#endif
