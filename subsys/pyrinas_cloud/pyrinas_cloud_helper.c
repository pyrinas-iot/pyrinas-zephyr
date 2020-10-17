
#include <zephyr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pyrinas_cloud_helper.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud_helper);

/* Gathers the numeric represnation of a string version*/
// TODO: get these stupid ver_from_string working
void ver_from_str(union pyrinas_cloud_version *ver, char *ver_str)
{
  size_t first_period = strcspn(ver_str, ".");
  size_t second_period = strcspn(ver_str + first_period + 1, ".") + first_period + 1;

  char *start_ptr = ver_str;
  char *end_ptr = ver_str + first_period - 1;

  ver->major = strtoul(start_ptr, &end_ptr, 10);
  LOG_DBG("ver: %d ", ver->major);

  start_ptr = ver_str + first_period + 1;
  end_ptr = ver_str + second_period - 1;

  ver->minor = strtoul(start_ptr, &end_ptr, 10);
  LOG_DBG("%d ", ver->minor);

  start_ptr = ver_str + second_period + 1;
  end_ptr = ver_str + strlen(ver_str) - 1;

  ver->patch = strtoul(start_ptr, &end_ptr, 10);
  LOG_DBG("%d\n", ver->patch);
}

/* Returns -1 if first is greater, 0 if equal, 1 if second is greater */
int ver_comp(union pyrinas_cloud_version *first, union pyrinas_cloud_version *second)
{

  for (int i = sizeof(first->all) - 1; i >= 0; i--)

    if (first->all[i] > second->all[i])
    {
      return -1;
    }
    else if (first->all[i] == second->all[i])
    {
      continue;
    }
    else
    {
      return 1;
    }

  return 0;
}