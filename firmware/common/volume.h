#ifndef VOLUME_H
#define VOLUME_H

#include <stdint.h>

void volume_set(int vol);
int volume_get(void);
void volume_apply(void *buf, size_t samples, int bit_depth);

#endif
