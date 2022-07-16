
/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <pyrinas_cloud/pyrinas_cloud_helper.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud_helper);

/* Number of entries in version struct*/
#define VER_ENTIRES 4

/* Returns -1 if first is greater, 0 if equal, 1 if second is greater */
int ver_comp(const union pyrinas_cloud_ota_version *first, const union pyrinas_cloud_ota_version *second)
{

  /* First check version numbers */
  for (int i = 0; i < VER_ENTIRES; i++)
  {

    if (first->raw[i] > second->raw[i])
    {
      return -1;
    }
    else if (first->raw[i] == second->raw[i])
    {
      continue;
    }
    else
    {
      return 1;
    }
  }

  /* Then compare hashes. Not equal if hashes are not equal. */
  int ret = memcmp(&first->hash, &second->hash, sizeof(first->hash));
  if (ret != 0)
  {
    return 1;
  }
  else
  {
    return 0;
  }
}
