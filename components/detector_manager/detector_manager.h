#ifndef DETECTOR_MANAGER_H
#define DETECTOR_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void detector_manager_init(void);
bool detector_manager_accept_esfera_response(void);

#ifdef __cplusplus
}
#endif

#endif // DETECTOR_MANAGER_H
