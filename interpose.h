#ifndef _INTERPOSE_H_
#define _INTERPOSE_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
bool interpose(const char *name, void *impl);
#ifdef __cplusplus
}
#endif

#endif