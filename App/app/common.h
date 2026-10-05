
#ifndef APP_COMMON_H
#define APP_COMMON_H

#include "functions.h"
#include "settings.h"
#include "ui/ui.h"

void COMMON_KeypadLockToggle();
void COMMON_SwitchVFOs();
void COMMON_SwitchVFOMode();
#ifdef ENABLE_FEAT_F4HWN_VFO_C
void COMMON_SwapVfoC(void);
#endif

#endif