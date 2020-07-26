#ifndef _PYRINAS_CLOUD_VERSION_H
#define _PYRINAS_CLOUD_VERSION_H

union pyrinas_cloud_version
{
  struct
  {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
  };
  uint8_t all[3];
};

void ver_from_str(union pyrinas_cloud_version *ver, char *ver_str);
int ver_comp(union pyrinas_cloud_version *first, union pyrinas_cloud_version *second);

#endif /* _PYRINAS_CLOUD_VERSION_H */