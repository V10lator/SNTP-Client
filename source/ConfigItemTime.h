#pragma once
#include <wups.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ConfigItemTime {
    WUPSConfigItemHandle handle;
    volatile uint32_t *ap;
    uint32_t mask;
} ConfigItemTime;

ConfigItemTime *WUPSConfigItemTime_AddToCategory(WUPSConfigCategoryHandle cat, const char *configID, const char *displayName, volatile uint32_t *ap, uint32_t mask);
ConfigItemTime *WUPSConfigItemTime_AddToCategoryHandled(WUPSConfigHandle config, WUPSConfigCategoryHandle cat, const char *configID, const char *displayName, volatile uint32_t *ap, uint32_t mask);

#ifdef __cplusplus
}
#endif
