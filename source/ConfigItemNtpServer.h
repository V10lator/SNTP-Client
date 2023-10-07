#pragma once
#include <wups.h>

#define MAX_NTP_SERVER_LENTGH 32

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ConfigItemNtpServer {
    WUPSConfigItemHandle handle;
    char *value;
} ConfigItemNtpServer;

bool WUPSConfigItemNtpServer_AddToCategory(WUPSConfigCategoryHandle cat, const char *configId, const char *displayName, char *value);

#define WUPSConfigItemNtpServer_AddToCategoryHandled(__config__, __cat__, __configId__, __displayName__, __value__)    \
    do {                                                                                                                                    \
        if (!WUPSConfigItemNtpServer_AddToCategory(__cat__, __configId__, __displayName__, __value__)) {               \
            WUPSConfig_Destroy(__config__);                                                                                                 \
            return 0;                                                                                                                       \
        }                                                                                                                                   \
    } while (0)

#ifdef __cplusplus
}
#endif
