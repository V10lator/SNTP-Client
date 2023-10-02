#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstring>

#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nn/pdm.h>
#include <notifications/notifications.h>
#include <wups.h>
#include <wups/config.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config/WUPSConfigItemStub.h>

#include "ConfigItemTime.h"
#include "timezones.h"

#define SYNCING_ENABLED_CONFIG_ID "enabledSync"
#define TIMEZONE_CONFIG_ID "timezone"
// Seconds between 1900 (NTP epoch) and 2000 (Wii U epoch)
#define NTP_TIMESTAMP_DELTA 3155673600llu
#define DEFAULT_TIMEZONE 321

#define LI_UNSYNC 0xc0
#define NTP_SERVER "pool.ntp.org"
#define MODE_MASK 0x07
#define MODE_CLIENT 0x03
#define MODE_SERVER 0x04

#define TIME_QUEUE_SIZE 2
#define NOTIF_QUEUE_SIZE 10
#define MSG_EXIT ((void *)0xDEADBABE)

// Important plugin information.
WUPS_PLUGIN_NAME("SNTP Client");
WUPS_PLUGIN_DESCRIPTION("A plugin that synchronizes a Wii U's clock with SNTP.");
WUPS_PLUGIN_VERSION("v1.8");
WUPS_PLUGIN_AUTHOR("Nightkingale & V10lator");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("SNTP Client");

static bool enabledSync = true;
static int32_t timezone = DEFAULT_TIMEZONE;
static volatile int32_t timezoneOffset;

static ConfigItemTime *sysTimeHandle;
static ConfigItemTime *ntpTimeHandle;

static OSMessageQueue timeQueue;
static OSMessageQueue notifQueue;
static OSMessage timeUpdates[TIME_QUEUE_SIZE];
static OSMessage notifs[NOTIF_QUEUE_SIZE];

static OSThread *timeThread = nullptr;
static OSThread *notifThread = nullptr;
static OSThread *settingsThread = nullptr;
static volatile bool settingsThreadActive;

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
    const char *msg;
} NOTIFICATION;

extern "C" int32_t CCRSysSetSystemTime(OSTime time);
extern "C" bool __OSSetAbsoluteSystemTime(OSTime time);

static int notifMain(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    OSMessage msg;
    bool ready;
    NOTIFICATION *notif;

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
            notif = static_cast<NOTIFICATION *>(msg.message);
            if(notif->error)
                NotificationModule_AddErrorNotification(notif->msg);
            else
                NotificationModule_AddInfoNotification(notif->msg);
        }

        MEMFreeToDefaultHeap(msg.message);
    } while(1);

    NotificationModule_DeInitLibrary();
    return 0;
}

static inline void showNotification(const char *notif, bool error)
{
    OSMessage msg;
    msg.message = MEMAllocFromDefaultHeap(sizeof(NOTIFICATION));
    if(msg.message == nullptr)
        return;

    static_cast<NOTIFICATION *>(msg.message)->error = error;
    static_cast<NOTIFICATION *>(msg.message)->msg = notif;

    if(!OSSendMessage(&notifQueue, &msg, OS_MESSAGE_FLAGS_NONE))
        MEMFreeToDefaultHeap(msg.message);
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
    struct hostent* server = gethostbyname(NTP_SERVER);
    if (server) {
        // Create a socket
        int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sockfd > -1) {
            // Prepare socket address
            struct sockaddr_in serv_addr;
            OSBlockSet(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;

            // Copy the server's IP address to the server address structure.
            OSBlockMove(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length, false);

            // Convert the port number integer to network big-endian style and save it to the server address structure.
            serv_addr.sin_port = htons(123); // UDP port

            // Create the packet
            ntp_packet packet __attribute__((__aligned__(0x40)));
            OSBlockSet(&packet, 0, sizeof(packet));
            // Set the first byte's bits to 00,001,011 for li = 0, vn = 1, and mode = 3.
            packet.li_vn_mode = (1 << 3) | MODE_CLIENT;

            // Call up the server using its IP address and port number.
            if (connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) == 0) {
                // Send it the NTP packet it wants.
                if (write(sockfd, &packet, sizeof(packet)) == sizeof(packet)) {
                    // Wait and receive the packet back from the server.
                    if (read(sockfd, &packet, sizeof(packet)) == sizeof(packet)) {
                        // Basic validity check:
                        // li != 11
                        // stratum != 0
                        // transmit timestamp != 0
                        if ((packet.li_vn_mode & LI_UNSYNC) != LI_UNSYNC && (packet.li_vn_mode & MODE_MASK) == MODE_SERVER && packet.stratum != 0 && (packet.txTm_s | packet.txTm_f)) {
                            // Adjust timestamp
                            packet.txTm_s = ntohl(packet.txTm_s);
                            packet.txTm_s -= NTP_TIMESTAMP_DELTA;
                            // Convert timezone
                            packet.txTm_s += timezoneOffset;

                            // Convert seconds to ticks
                            tick = OSSecondsToTicks(packet.txTm_s);

                            // Convert fraction of seconds
                            tick += OSNanosecondsToTicks((ntohl(packet.txTm_f) * 1000000000llu) >> 32);
                        }
                    }
                }

                close(sockfd);
            }
        }
    }

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
        {
            showNotification("Error getting time from " NTP_SERVER "!", true);
            continue;
        }

        tmpTime = static_cast<OSTime>(abs(time - tmpTime));

        if(tmpTime <= static_cast<OSTime>(OSMillisecondsToTicks(250)))
            continue; // Time difference is within 250 milliseconds, no need to update.

        if(SetSystemTime(time))
            showNotification("Time synced", false);
        else
            showNotification("Error setting hardware clock!", true);
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

static OSThread *startThread(const char *name, OSThreadEntryPointFn mainfunc, OSThreadAttributes attribs)
{
    OSThread *ost = static_cast<OSThread *>(MEMAllocFromDefaultHeapEx(sizeof(OSThread) + 0x1000, 8));
    if(ost != nullptr)
    {
        if(OSCreateThread(ost, mainfunc, 0, nullptr, reinterpret_cast<uint8_t *>(ost) + (0x1000 + sizeof(OSThread)), 0x1000, 5, attribs))
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

        WUPS_CloseStorage(); // Close the storage.
    }

    changeTimezone(nullptr, timezone);
}

ON_APPLICATION_START()
{
    OSInitMessageQueueEx(&notifQueue, notifs, NOTIF_QUEUE_SIZE, "SNTP Client Notifications");
    OSInitMessageQueueEx(&timeQueue, timeUpdates, TIME_QUEUE_SIZE, "SNTP Client Time Update Requests");

    notifThread = startThread("SNTP Client Notification Thread", notifMain, OS_THREAD_ATTRIB_AFFINITY_CPU0);
    timeThread = startThread("SNTP Client Time Update Thread", timeThreadMain, OS_THREAD_ATTRIB_AFFINITY_CPU2);

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

static void setTimeInSettings(OSTime *ntpTime, OSTime *localTime, bool sync) {
    OSCalendarTime ct;
    char timeString[256];

    if(sync)
    {
        *ntpTime = NTPGetTime();
        *localTime = OSGetTime();
    }

    if(*ntpTime == 0)
        WUPSConfigItem_SetDisplayName(ntpTimeHandle->handle, "Current NTP Time: N/A");
    else {
        OSTicksToCalendarTime(*ntpTime, &ct);
        snprintf(timeString, 255, "Current NTP Time: %04d-%02d-%02d %02d:%02d:%02d:%04d:%04d\n", ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec, ct.tm_msec, ct.tm_usec);
        WUPSConfigItem_SetDisplayName(ntpTimeHandle->handle, timeString);
    }

    OSTicksToCalendarTime(*localTime, &ct);
    snprintf(timeString, 255, "Current SYS Time: %04d-%02d-%02d %02d:%02d:%02d:%04d:%04d\n", ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec, ct.tm_msec, ct.tm_usec);
    WUPSConfigItem_SetDisplayName(sysTimeHandle->handle, timeString);

    //TODO: Update screen without the user needing to press a button
}

static int settingsThreadMain(int argc, const char **argv) {
    (void)argc;
    (void)argv;

    OSTime ntpTime;
    OSTime localTime;
    int i = 29;
    bool sync;

    while(settingsThreadActive) {
        sync = ++i == 30;
        if(sync)
            i = 0;
        else
        {
            ntpTime += OSSecondsToTicks(1);
            localTime += OSSecondsToTicks(1);
        }

        setTimeInSettings(&ntpTime, &localTime, sync);
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

    WUPSConfigItemBoolean_AddToCategoryHandled(settings, config, "enabledSync", "Syncing Enabled", enabledSync, &syncingEnabled);
    WUPSConfigItemMultipleValues_AddToCategoryHandled(settings, config, TIMEZONE_CONFIG_ID, "Timezone", timezone, timezonesReadable, sizeof(timezonesReadable) / sizeof(timezonesReadable[0]), &saveTimezone);

    sysTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "sysTime", "Current SYS Time: Loading...");
    ntpTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "ntpTime", "Current NTP Time: Loading...");
    settingsThreadActive = true;
    settingsThread = startThread("SNTP Client Settings Thread", settingsThreadMain, OS_THREAD_ATTRIB_AFFINITY_CPU1);

    return settings;
}

WUPS_CONFIG_CLOSED() {
    updateTime();
    
    settingsThreadActive = false;
    WUPS_CloseStorage(); // Save all changes.
    stopThread(settingsThread);
    settingsThread = nullptr;
}
