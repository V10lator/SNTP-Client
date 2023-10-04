#include "ConfigItemTime.h"
#include <coreinit/atomic.h>
#include <coreinit/memory.h>
#include <coreinit/memdefaultheap.h>
#include <wups.h>

void WUPSConfigItemTime_onDelete(void *context);

int32_t WUPSConfigItemTime_getCurrentValueDisplay(void *context, char *out_buf, int32_t out_size) {
    (void)context;
    OSBlockSet(out_buf, 0, out_size);
    return 0;
}

bool WUPSConfigItemTime_callCallback(void *context) {
    (void)context;
    return false;
}

void WUPSConfigItemTime_onButtonPressed(void *context, WUPSConfigButtons buttons) {
    (void)context;
    (void)buttons;
}

bool WUPSConfigItemTime_isMovementAllowed(void *context) {
    (void)context;
    return true;
}

void WUPSConfigItemTime_restoreDefault(void *context) {
    (void)context;
}

void WUPSConfigItemTime_onSelected(void *context, bool isSelected) {
    ConfigItemTime *item = (ConfigItemTime *) context;
    if(isSelected)
        OSOrAtomic(item->ap, item->mask);
    else
        OSAndAtomic(item->ap, ~(item->mask));
}

ConfigItemTime *WUPSConfigItemTime_AddToCategoryEx(WUPSConfigCategoryHandle cat, const char *configID, const char *displayName, volatile uint32_t *ap, uint32_t mask)
{
    if(cat == 0)
        return NULL;

    ConfigItemTime *item = (ConfigItemTime *) MEMAllocFromDefaultHeap(sizeof(ConfigItemTime));
    if(item == NULL)
        return NULL;

    item->ap = ap;
    item->mask = mask;

    WUPSConfigCallbacks_t callbacks = {
            .getCurrentValueDisplay         = &WUPSConfigItemTime_getCurrentValueDisplay,
            .getCurrentValueSelectedDisplay = &WUPSConfigItemTime_getCurrentValueDisplay,
            .onSelected                     = &WUPSConfigItemTime_onSelected,
            .restoreDefault                 = &WUPSConfigItemTime_restoreDefault,
            .isMovementAllowed              = &WUPSConfigItemTime_isMovementAllowed,
            .callCallback                   = &WUPSConfigItemTime_callCallback,
            .onButtonPressed                = &WUPSConfigItemTime_onButtonPressed,
            .onDelete                       = &WUPSConfigItemTime_onDelete};

    if(WUPSConfigItem_Create(&item->handle, configID, displayName, callbacks, item) < 0) {
        MEMFreeToDefaultHeap(item);
        return NULL;
    }

    if(WUPSConfigCategory_AddItem(cat, item->handle) < 0) {
        WUPSConfigItem_Destroy(item->handle);
        return NULL;
    }

    return item;
}

void WUPSConfigItemTime_onDelete(void *context) {
    MEMFreeToDefaultHeap(context);
}

ConfigItemTime *WUPSConfigItemTime_AddToCategory(WUPSConfigCategoryHandle cat, const char *configID, const char *displayName, volatile uint32_t *ap, uint32_t mask) {
    return WUPSConfigItemTime_AddToCategoryEx(cat, configID, displayName, ap, mask);
}

ConfigItemTime *WUPSConfigItemTime_AddToCategoryHandled(WUPSConfigHandle config, WUPSConfigCategoryHandle cat, const char *configID, const char *displayName, volatile uint32_t *ap, uint32_t mask) {
    ConfigItemTime *ret = WUPSConfigItemTime_AddToCategory(cat, configID, displayName, ap, mask);
    if(ret == NULL)
        WUPSConfig_Destroy(config);

    return ret;
}
