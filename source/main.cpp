#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <coreinit/atomic.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nn/pdm.h>
#include <notifications/notifications.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <wups.h>
#include <wups/config.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/function_patching.h>

#include "ConfigItemNtpServer.h"
#include "ConfigItemTime.h"
#include "timezones.h"

#define NTPSERVER_CONFIG_ID "ntpServer"
#define SYNCING_ENABLED_CONFIG_ID "enabledSync"
#define TIMEZONE_CONFIG_ID "timezone"
// Seconds between 1900 (NTP epoch) and 2000 (Wii U epoch)
#define NTP_TIMESTAMP_DELTA 3155673600llu
#define DEFAULT_TIMEZONE 321

#define LI_UNSYNC 0xc0
#define MODE_MASK 0x07
#define MODE_CLIENT 0x03
#define MODE_SERVER 0x04

#define TIME_QUEUE_SIZE 2
#define NOTIF_QUEUE_SIZE 10
#define MSG_EXIT ((void *)0xDEADBABE)

// Important plugin information.
WUPS_PLUGIN_NAME("SNTP Client");
WUPS_PLUGIN_DESCRIPTION("A plugin that synchronizes a Wii U's clock with SNTP.");
WUPS_PLUGIN_VERSION("v1.16");
WUPS_PLUGIN_AUTHOR("Nightkingale & V10lator");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("SNTP Client");

static bool enabledSync = true;
static volatile char ntp_server[MAX_NTP_SERVER_LENTGH] = "pool.ntp.org";
static int32_t timezone = DEFAULT_TIMEZONE;
static volatile int32_t timezoneOffset;

static ConfigItemTime *updTimeHandle;
static ConfigItemTime *sysTimeHandle;
static ConfigItemTime *ntpTimeHandle;
static volatile uint32_t previewMask;

static OSMessageQueue timeQueue;
static OSMessageQueue notifQueue;
static OSMessage timeUpdates[TIME_QUEUE_SIZE];
static OSMessage notifs[NOTIF_QUEUE_SIZE];

static OSThread *timeThread = nullptr;
static OSThread *notifThread = nullptr;
static OSThread *settingsThread = nullptr;
static volatile bool settingsThreadActive;
static volatile uint32_t fakePress = false;

// From https://github.com/lettier/ntpclient/blob/master/source/c/main.c
typedef struct
{
    uint8_t li_vn_mode;      // Eight bits. li, vn, and mode.
                                // li.   Two bits.   Leap indicator.
                                // vn.   Three bits. Version number of the protocol.
                                // mode. Three bits. Client will pick mode 3 for client.

    uint8_t stratum;         // Eight bits. Stratum level of the local clock.
    uint8_t poll;            // Eight bits. Maximum interval between successive messages.
    uint8_t precision;       // Eight bits. Precision of the local clock.

    uint32_t rootDelay;      // 32 bits. Total round trip delay time.
    uint32_t rootDispersion; // 32 bits. Max error aloud from primary clock source.
    uint32_t refId;          // 32 bits. Reference clock identifier.

    uint32_t refTm_s;        // 32 bits. Reference time-stamp seconds.
    uint32_t refTm_f;        // 32 bits. Reference time-stamp fraction of a second.

    uint32_t origTm_s;       // 32 bits. Originate time-stamp seconds.
    uint32_t origTm_f;       // 32 bits. Originate time-stamp fraction of a second.

    uint32_t rxTm_s;         // 32 bits. Received time-stamp seconds.
    uint32_t rxTm_f;         // 32 bits. Received time-stamp fraction of a second.

    uint32_t txTm_s;         // 32 bits and the most important field the client cares about. Transmit time-stamp seconds.
    uint32_t txTm_f;         // 32 bits. Transmit time-stamp fraction of a second.

} ntp_packet;              // Total: 384 bits or 48 bytes.

typedef struct
{
    bool error;
    char msg[1024];;
} NOTIFICATION;

extern "C" int32_t CCRSysSetSystemTime(OSTime time);
extern "C" bool __OSSetAbsoluteSystemTime(OSTime time);

#define get_ip_str(sad) inet_ntoa(reinterpret_cast<sockaddr_in *>(sad->ai_addr)->sin_addr)

static int notifMain(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    OSMessage msg;
    bool ready;

    NotificationModule_InitLibrary();
    do
    {
        OSReceiveMessage(&notifQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        if(msg.message == MSG_EXIT)
            break;

        while(NotificationModule_IsOverlayReady(&ready) == NOTIFICATION_MODULE_RESULT_SUCCESS && !ready)
            OSSleepTicks(OSMillisecondsToTicks(100));

        if(ready)
        {
            if(static_cast<NOTIFICATION *>(msg.message)->error)
                NotificationModule_AddErrorNotification(static_cast<NOTIFICATION *>(msg.message)->msg);
            else
                NotificationModule_AddInfoNotification(static_cast<NOTIFICATION *>(msg.message)->msg);
        }

        MEMFreeToDefaultHeap(msg.message);
    } while(1);

    NotificationModule_DeInitLibrary();
    return 0;
}

static void showNotification(bool error, const char *notif)
{
    OSMessage msg;
    msg.message = MEMAllocFromDefaultHeap(sizeof(NOTIFICATION));
    if(msg.message == nullptr)
        return;

    static_cast<NOTIFICATION *>(msg.message)->error = error;
    strncpy(static_cast<NOTIFICATION *>(msg.message)->msg, notif, 1023);
    static_cast<NOTIFICATION *>(msg.message)->msg[1023] = '\0';

    if(!OSSendMessage(&notifQueue, &msg, OS_MESSAGE_FLAGS_NONE))
        MEMFreeToDefaultHeap(msg.message);
}

static void showNotificationF(bool error, const char *notif, ...)
{
    char msg[1024];
    va_list va;
    va_start(va, notif);
    vsnprintf(msg, 1023, notif, va);
    va_end(va);
    showNotification(error, msg);
}

static inline bool SetSystemTime(OSTime time)
{
    bool res = false;
    nn::pdm::NotifySetTimeBeginEvent();

    if(CCRSysSetSystemTime(time) == 0)
        res = __OSSetAbsoluteSystemTime(time);

    nn::pdm::NotifySetTimeEndEvent();
    return res;
}

static OSTime NTPGetTime()
{
    OSTime tick = 0;

    // Get host address by name
    struct addrinfo *addys = NULL;
    struct addrinfo hints;
    OSBlockSet(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_ADDRCONFIG;


    // Create the packet
    ntp_packet packet __attribute__((__aligned__(0x40)));

    int sockfd = getaddrinfo((char *)ntp_server, "123", &hints, &addys);
    if(!sockfd && addys != NULL)
    {
        // Loop through all IP addys returned by the DNS
        for(struct addrinfo *addr = addys; addr != NULL; addr = addr->ai_next)
        {
            // Reset packet
            OSBlockSet(&packet, 0, sizeof(packet));
            // Set the first byte's bits to 00,001,011 for li = 0, vn = 1, and mode = 3.
            packet.li_vn_mode = (1 << 3) | MODE_CLIENT;

            // Create a socket
            sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
            if(sockfd != -1)
            {
                // Connect to the server
                if(connect(sockfd, addr->ai_addr, addr->ai_addrlen) == 0)
                {
                    // Send it the NTP packet it wants.
                    if(write(sockfd, &packet, sizeof(packet)) == sizeof(packet))
                    {
                        // Wait and receive the packet back from the server.
                        if(read(sockfd, &packet, sizeof(packet)) == sizeof(packet))
                        {
                            // Basic validity check:
                            // li != 11
                            // stratum != 0
                            // transmit timestamp != 0
                            if((packet.li_vn_mode & LI_UNSYNC) != LI_UNSYNC && (packet.li_vn_mode & MODE_MASK) == MODE_SERVER && packet.stratum != 0 && (packet.txTm_s | packet.txTm_f))
                            {
                                // Adjust timestamp
                                packet.txTm_s = ntohl(packet.txTm_s);
                                packet.txTm_s -= NTP_TIMESTAMP_DELTA;
                                // Convert timezone
                                packet.txTm_s += timezoneOffset;

                                // Convert seconds to ticks
                                tick = OSSecondsToTicks(packet.txTm_s);

                                // Convert fraction of seconds
                                tick += OSNanosecondsToTicks((ntohl(packet.txTm_f) * 1000000000llu) >> 32);
                                close(sockfd);
                                break;
                            }

                            showNotificationF(true, "SNTP Client: Got invalid reply from %s!", get_ip_str(addr));
                        }
                        else
                            showNotificationF(true, "SNTP Client: Error reading from %s: %s", get_ip_str(addr), strerror(errno));
                    }
                    else
                        showNotificationF(true, "SNTP Client: Error writing to %s: %s!", get_ip_str(addr), strerror(errno));
                }
                else
                    showNotificationF(true, "SNTP Client: Error connecting to %s: %s", get_ip_str(addr), strerror(errno));

                close(sockfd);
            }
            else
                showNotificationF(true, "SNTP Client: Error opening socket: %s", strerror(errno));
        } // End of IP loop

        freeaddrinfo(addys);
    }
    else
        showNotificationF(true, "SNTP Client: Error resolving host: %s", gai_strerror(sockfd));

    return tick;
}

static inline void updateTime() {
    if(enabledSync)
    {
        OSMessage msg;
        msg.message = nullptr;
        OSSendMessage(&timeQueue, &msg, OS_MESSAGE_FLAGS_NONE);
    }
}

static int timeThreadMain(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    OSMessage msg;
    OSTime time;
    OSTime tmpTime;

    do
    {
        OSReceiveMessage(&timeQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        if(msg.message == MSG_EXIT)
            return 0;

        time = NTPGetTime(); // Connect to the time server.
        tmpTime = OSGetTime();
        if(time == 0)
            continue;

        tmpTime = static_cast<OSTime>(abs(time - tmpTime));

        if(tmpTime <= static_cast<OSTime>(OSMillisecondsToTicks(250)))
            continue; // Time difference is within 250 milliseconds, no need to update.

        if(SetSystemTime(time))
            showNotification(false, "Time synced");
        else
            showNotification(true, "SNTP Client: Error setting hardware clock!");
    } while(1);
}

static void syncingEnabled(ConfigItemBoolean *item, bool value)
{
    (void)item;
    // If false, bro is literally a time traveler!
    WUPS_StoreBool(nullptr, SYNCING_ENABLED_CONFIG_ID, value);
    enabledSync = value;
}

static void changeTimezone(ConfigItemMultipleValues *item, uint32_t value)
{
    (void)item;

    setenv("TZ", timezonesPOSIX[value].valueName, 1);
    tzset();
    timezoneOffset = -_timezone;
    if(_daylight)
        timezoneOffset += 3600;

    timezone = value;
}

static void saveTimezone(ConfigItemMultipleValues *item, uint32_t value)
{
    (void)item;
    WUPS_StoreInt(nullptr, TIMEZONE_CONFIG_ID, value);
    changeTimezone(nullptr, value);
}

static void changeNtpServer(ConfigItemNtpServer *item, const char *value)
{
    (void)item;
    WUPS_StoreString(nullptr, NTPSERVER_CONFIG_ID, value);
    strcpy((char *)ntp_server, value);
}

static OSThread *startThread(const char *name, OSThreadEntryPointFn mainfunc, size_t stacksize, OSThreadAttributes attribs)
{
    OSThread *ost = static_cast<OSThread *>(MEMAllocFromDefaultHeapEx(sizeof(OSThread) + stacksize, 8));
    if(ost != nullptr)
    {
        if(OSCreateThread(ost, mainfunc, 0, nullptr, reinterpret_cast<uint8_t *>(ost) + stacksize + sizeof(OSThread), stacksize, 5, attribs))
        {
            OSSetThreadName(ost, name);
            OSResumeThread(ost);
            return ost;
        }

        MEMFreeToDefaultHeap(ost);
    }

    return nullptr;
}

static inline void stopThread(OSThread *thread)
{
    OSJoinThread(thread, nullptr);
    OSDetachThread(thread);
    MEMFreeToDefaultHeap(thread);
}

INITIALIZE_PLUGIN() {
    WUPSStorageError storageRes = WUPS_OpenStorage();
    // Check if the plugin's settings have been saved before.
    if(storageRes == WUPS_STORAGE_ERROR_SUCCESS) {
        if((storageRes = WUPS_GetBool(nullptr, SYNCING_ENABLED_CONFIG_ID, &enabledSync)) == WUPS_STORAGE_ERROR_NOT_FOUND)
            WUPS_StoreBool(nullptr, SYNCING_ENABLED_CONFIG_ID, enabledSync);

        if((storageRes = WUPS_GetInt(nullptr, TIMEZONE_CONFIG_ID, &timezone)) == WUPS_STORAGE_ERROR_NOT_FOUND)
            WUPS_StoreInt(nullptr, TIMEZONE_CONFIG_ID, timezone);

        if((storageRes = WUPS_GetString(nullptr, NTPSERVER_CONFIG_ID, (char *)ntp_server, MAX_NTP_SERVER_LENTGH - 1)) == WUPS_STORAGE_ERROR_NOT_FOUND)
            WUPS_StoreString(nullptr, NTPSERVER_CONFIG_ID, (char *)ntp_server);

        WUPS_CloseStorage(); // Close the storage.
    }

    changeTimezone(nullptr, timezone);
}

ON_APPLICATION_START()
{
    OSInitMessageQueueEx(&notifQueue, notifs, NOTIF_QUEUE_SIZE, "SNTP Client Notifications");
    OSInitMessageQueueEx(&timeQueue, timeUpdates, TIME_QUEUE_SIZE, "SNTP Client Time Update Requests");

    notifThread = startThread("SNTP Client Notification Thread", notifMain, 0x800, OS_THREAD_ATTRIB_AFFINITY_CPU0);
    timeThread = startThread("SNTP Client Time Update Thread", timeThreadMain, 0x1000, OS_THREAD_ATTRIB_AFFINITY_CPU2);

    updateTime();
}

ON_APPLICATION_ENDS() {
    OSMessage msg;
    msg.message = MSG_EXIT;

    if(timeThread != nullptr)
    {
        OSSendMessage(&timeQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        stopThread(timeThread);
        timeThread = nullptr;
    }

    if(notifThread != nullptr)
    {
        OSSendMessage(&notifQueue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
        stopThread(notifThread);
        notifThread = nullptr;
    }
}

static int settingsThreadMain(int argc, const char **argv) {
    (void)argc;
    (void)argv;

    uint32_t i = 1;
    OSTime ntpTime = 0;
    OSTime localTime = 0;
    OSCalendarTime ct;
    char timeString[64];

    while(settingsThreadActive)
    {
        if(!--i)
        {
            i = 30;
            ntpTime = NTPGetTime();
            localTime = OSGetTime();
        }
        else
        {
            if(ntpTime)
                ntpTime += OSSecondsToTicks(1);

            localTime += OSSecondsToTicks(1);


        }

        if(previewMask)
        {
            snprintf(timeString, 63, "Next update in %u seconds", i);
            WUPSConfigItem_SetDisplayName(updTimeHandle->handle, timeString);

            if(ntpTime)
            {
                OSTicksToCalendarTime(ntpTime, &ct);
                snprintf(timeString, 63, "Current NTP Time: %04d-%02d-%02d %02d:%02d:%02d:%04d:%04d\n", ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec, ct.tm_msec, ct.tm_usec);
                WUPSConfigItem_SetDisplayName(ntpTimeHandle->handle, timeString);
            }
            else
                WUPSConfigItem_SetDisplayName(ntpTimeHandle->handle, "Current NTP Time: N/A");

            OSTicksToCalendarTime(localTime, &ct);
            snprintf(timeString, 63, "Current SYS Time: %04d-%02d-%02d %02d:%02d:%02d:%04d:%04d\n", ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec, ct.tm_msec, ct.tm_usec);
            WUPSConfigItem_SetDisplayName(sysTimeHandle->handle, timeString);

            // Update screen with a fake button press
            fakePress = true;
        }

        OSSleepTicks(OSSecondsToTicks(1));
    }

    return 0;
}

WUPS_GET_CONFIG() {
    if(WUPS_OpenStorage() != WUPS_STORAGE_ERROR_SUCCESS)
        return 0;

    WUPSConfigHandle settings;
    WUPSConfig_CreateHandled(&settings, "Wii U Time Sync");

    WUPSConfigCategoryHandle config;
    WUPSConfig_AddCategoryByNameHandled(settings, "Configuration", &config);
    WUPSConfigCategoryHandle preview;
    WUPSConfig_AddCategoryByNameHandled(settings, "Preview Time", &preview);

    WUPSConfigItemBoolean_AddToCategoryHandled(settings, config, SYNCING_ENABLED_CONFIG_ID, "Syncing Enabled", enabledSync, &syncingEnabled);
    WUPSConfigItemMultipleValues_AddToCategoryHandled(settings, config, TIMEZONE_CONFIG_ID, "Timezone", timezone, timezonesReadable, sizeof(timezonesReadable) / sizeof(timezonesReadable[0]), &saveTimezone);
    WUPSConfigItemNtpServer_AddToCategoryHandled(settings, config, NTPSERVER_CONFIG_ID, "NTP Server", (char *)ntp_server, &changeNtpServer);

    previewMask = 0;
    sysTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "sysTime", "Current SYS Time: Loading...", &previewMask, 1);
    ntpTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "ntpTime", "Current NTP Time: Loading...", &previewMask, 2);
    updTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "updTime", "Next update in 30 seconds", &previewMask, 4);

    settingsThreadActive = true;
    settingsThread = startThread("SNTP Client Settings Thread", settingsThreadMain, 0x4000, OS_THREAD_ATTRIB_AFFINITY_CPU1);

    return settings;
}

WUPS_CONFIG_CLOSED() {
    settingsThreadActive = false;
    updateTime();
    WUPS_CloseStorage(); // Save all changes.
    if(settingsThread != nullptr)
    {
        stopThread(settingsThread);
        settingsThread = nullptr;
    }
}

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError)
{
    int32_t result = real_VPADRead(chan, buffers, count, outError);

    if(settingsThreadActive && *outError == VPAD_READ_SUCCESS && OSCompareAndSwapAtomic(&fakePress, true, false) && !buffers->trigger)
        buffers->trigger = VPAD_BUTTON_MINUS;

    return result;
}
WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

DECL_FUNCTION(int32_t, KPADReadEx, KPADChan chan, KPADStatus *data, uint32_t size, KPADError *error)
{
    int32_t result = real_KPADReadEx(chan, data, size, error);
    if(settingsThreadActive && result > 0 && *error == KPAD_ERROR_OK && data->extensionType != 0xFF && OSCompareAndSwapAtomic(&fakePress, true, false))
    {
        if(data->extensionType == WPAD_EXT_CORE || data->extensionType == WPAD_EXT_NUNCHUK)
        {
            if(!data->trigger)
                data->trigger = VPAD_BUTTON_MINUS;
        }
        else if(!data->classic.trigger)
            data->classic.trigger = WPAD_CLASSIC_BUTTON_MINUS;
    }


    return result;
}
WUPS_MUST_REPLACE(KPADReadEx, WUPS_LOADER_LIBRARY_PADSCORE, KPADReadEx);
