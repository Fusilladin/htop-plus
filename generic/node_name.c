/*
htop - generic/node_name.c
(C) 2021 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h"  // IWYU pragma: keep

#include "generic/node_name.h"

#include <sys/utsname.h>
#include <string.h>


void Generic_node_name(char* buffer, size_t size) {
   if (!size)
      return;
   struct utsname info;
   buffer[0] = '\0';
   if (uname(&info) == 0) {
      strncpy(buffer, info.nodename, size - 1);
      buffer[size - 1] = '\0';
   }
}
