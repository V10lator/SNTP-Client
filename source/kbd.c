#include "ConfigItemNtpServer.h"
#include "kbd.h"
#include "schrift.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <wups.h>

static uint8_t *tvBuffer;
static uint8_t *drcBuffer;

static uint32_t tvSize;
static uint32_t drcSize;

static bool isBackBuffer;
static SFT pFont;

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} Color;

#define SCREEN_WIDTH  854
#define SCREEN_HEIGHT 480
#define TV_WIDTH  0x500
#define DRC_WIDTH 0x380
#define STEP ((SCREEN_WIDTH - (8 * 2) - (3 * 10)) / 10)
#define COLOR_BACKGROUND ((Color){ .r = 238, .g = 238, .b = 238, .a = 255 })
#define COLOR_BLACK      ((Color){ .r =   0, .g =   0, .b =   0, .a = 255 })
#define COLOR_BORDER     ((Color){ .r = 204, .g = 204, .b = 204, .a = 255 })
#define COLOR_BLUE       ((Color){ .r =  52, .g = 120, .b = 228, .a = 255 })
#define FONT_SIZE 24

static const char keymap[(10 * 4) - 3] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '.',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M',
};

DECL_FUNCTION(void, OSScreenSetBufferEx, OSScreenID screen, void *addr)
{
    switch(screen)
    {
        case SCREEN_TV:
            tvBuffer = (uint8_t *)addr;
            tvSize = OSScreenGetBufferSizeEx(SCREEN_TV) / 2;
            break;
        case SCREEN_DRC:
            drcBuffer = (uint8_t *)addr;
            drcSize = OSScreenGetBufferSizeEx(SCREEN_DRC) / 2;
            break;
    }

    real_OSScreenSetBufferEx(screen, addr);
}
WUPS_MUST_REPLACE(OSScreenSetBufferEx, WUPS_LOADER_LIBRARY_COREINIT, OSScreenSetBufferEx);

static inline uint32_t colorToOSScreen(Color color)
{
    return (((uint32_t)color.r) << 24) | (((uint32_t)color.g) << 16) | (((uint32_t)color.b) << 8) | ((uint32_t)color.a);
}

static void drawPixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if(a == 0)
        return;

    uint32_t i = (x + y * DRC_WIDTH) * 4;
    if(i + 2 >= drcSize)
        return;

    // put pixel in the drc buffer
    if(isBackBuffer)
        i += drcSize;

    uint32_t pixel;
    float f;

    if(a == 0xFF)
    {
        pixel = r;
        pixel <<=  8;
        pixel |= g;
        pixel <<= 8;
        pixel |= b;
        pixel <<= 8;

    }
    else
    {
        f = a / 255.0f;

        pixel = (r * f) + (drcBuffer[i] * (1.0f - f));
        pixel <<= 8;
        pixel |= (uint32_t)((g * f) + (drcBuffer[++i] * (1.0f - f)));
        pixel <<= 8;
        pixel |= (uint32_t)((b * f) + (drcBuffer[++i] * (1.0f - f)));
        pixel <<= 8;

        i -= 2;
    }

    *((uint32_t *)(drcBuffer + i)) = pixel;

    uint32_t USED_TV_WIDTH;
    if(tvSize == 0x007E9000)
    {
        USED_TV_WIDTH = 1920;
        f             = 2.25f;
    }
    else
    {
        USED_TV_WIDTH = TV_WIDTH;
        f             = 1.5f;
    }

    for(uint32_t yy = (y * f); yy < ((y * f) + f); yy++)
        for(uint32_t xx = (x * f); xx < ((x * f) + f); xx++)
        {
            i = (xx + yy * USED_TV_WIDTH) * 4;
            if(isBackBuffer)
                i += tvSize;

            *((uint32_t *)(tvBuffer + i)) = pixel;
        }
}

static void drawRectFilled(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Color col)
{
    for(uint32_t xx = x; xx < x + w; ++xx)
        for(uint32_t yy = y; yy < y + h; ++yy)
            drawPixel(xx, yy, col.r, col.g, col.b, col.a);
}

static void draw_freetype_bitmap(SFT_Image *bmp, int32_t x, int32_t y) {
    int32_t i, j, p, q;

    int32_t x_max = x + bmp->width;
    int32_t y_max = y + bmp->height;

    uint8_t *src = (uint8_t *) bmp->pixels;
    float opacity;

    for (i = x, p = 0; i < x_max; i++, p++) {
        for (j = y, q = 0; j < y_max; j++, q++) {
            if (i < 0 || j < 0 || i >= SCREEN_WIDTH || j >= SCREEN_HEIGHT) {
                continue;
            }

            opacity = src[q * bmp->width + p] / 255.0f;
            drawPixel(i, j, 0x00, 0x00, 0x00, 0xFF * opacity);
        }
    }
}

static void print(uint32_t x, uint32_t y, const wchar_t *string) {
    uint32_t penX = (int32_t) x;
    uint32_t penY = (int32_t) y;

    uint16_t textureWidth = 0, textureHeight = 0;
    for (; *string; string++) {
        SFT_Glyph gid; //  unsigned long gid;
        if (sft_lookup(&pFont, *string, &gid) >= 0) {
            SFT_GMetrics mtx;
            if (sft_gmetrics(&pFont, gid, &mtx) < 0)
                return;

            textureWidth  = (mtx.minWidth + 3) & ~3;
            textureHeight = mtx.minHeight;

            SFT_Image img = {
                    .pixels = NULL,
                    .width  = textureWidth,
                    .height = textureHeight,
            };

            if (textureWidth == 0)
                textureWidth = 4;

            if (textureHeight == 0)
                textureHeight = 4;

            uint8_t *buffer = MEMAllocFromDefaultHeap(img.width * img.height);
            if(!buffer)
                return;

            img.pixels = buffer;
            if (sft_render(&pFont, gid, img) < 0)
                return;

            draw_freetype_bitmap(&img, (int32_t) (penX + mtx.leftSideBearing), (int32_t) (penY + mtx.yOffset));
            penX += (int32_t) mtx.advanceWidth;
        }
    }
}

static uint32_t getTextWidth(const wchar_t *string) {
    uint32_t width = 0;

    for (; *string; string++) {
        SFT_Glyph gid; //  unsigned long gid;
        if (sft_lookup(&pFont, *string, &gid) >= 0) {
            SFT_GMetrics mtx;
            if(sft_gmetrics(&pFont, gid, &mtx) >= 0)
                width += (int32_t) mtx.advanceWidth;
        }
    }

    return width;
}


static uint32_t mapClassicButtons(uint32_t buttonMap)
{
    uint32_t ret = 0;

    if(buttonMap & WPAD_CLASSIC_BUTTON_RIGHT)
        ret |= VPAD_BUTTON_RIGHT;
    if(buttonMap & WPAD_CLASSIC_BUTTON_LEFT)
        ret |= VPAD_BUTTON_LEFT;
    if(buttonMap & WPAD_CLASSIC_BUTTON_DOWN)
        ret |= VPAD_BUTTON_DOWN;
    if(buttonMap & WPAD_CLASSIC_BUTTON_UP)
        ret |= VPAD_BUTTON_UP;
    if(buttonMap & WPAD_CLASSIC_BUTTON_A)
        ret |= VPAD_BUTTON_A;
    if(buttonMap & WPAD_CLASSIC_BUTTON_B)
        ret |= VPAD_BUTTON_B;

    if(buttonMap & WPAD_CLASSIC_STICK_L_EMULATION_RIGHT)
        ret |= VPAD_BUTTON_RIGHT;
    if(buttonMap & WPAD_CLASSIC_STICK_L_EMULATION_LEFT)
        ret |= VPAD_BUTTON_LEFT;
    if(buttonMap & WPAD_CLASSIC_STICK_L_EMULATION_DOWN)
        ret |= VPAD_BUTTON_DOWN;
    if(buttonMap & WPAD_CLASSIC_STICK_L_EMULATION_UP)
        ret |= VPAD_BUTTON_UP;

    return ret;
}

static uint32_t mapWiiButtons(uint32_t buttonMap)
{
    uint32_t ret = 0;

    if(buttonMap & WPAD_BUTTON_RIGHT)
        ret |= VPAD_BUTTON_RIGHT;
    if(buttonMap & WPAD_BUTTON_LEFT)
        ret |= VPAD_BUTTON_LEFT;
    if(buttonMap & WPAD_BUTTON_DOWN)
        ret |= VPAD_BUTTON_DOWN;
    if(buttonMap & WPAD_BUTTON_UP)
        ret |= VPAD_BUTTON_UP;
    if(buttonMap & WPAD_BUTTON_A)
        ret |= VPAD_BUTTON_A;
    if(buttonMap & WPAD_BUTTON_B)
        ret |= VPAD_BUTTON_B;

    return ret;
}

static void drawKeyboard(uint32_t x, uint32_t y, const wchar_t *str, uint32_t cursor)
{
    OSScreenClearBufferEx(SCREEN_DRC, colorToOSScreen(COLOR_BACKGROUND));
    OSScreenClearBufferEx(SCREEN_TV, colorToOSScreen(COLOR_BACKGROUND));

    drawRectFilled(8 + 3 + (x * (STEP + 3)), SCREEN_HEIGHT - 8 - 5 - (44 * 4) + 3 + (y * 44), STEP, 44 - 3, COLOR_BLUE);

    wchar_t buf[MAX_NTP_SERVER_LENTGH + 1];
    for(y = 0; y < MAX_NTP_SERVER_LENTGH - 1; ++y)
        buf[y] = 'm';

    buf[MAX_NTP_SERVER_LENTGH - 1] = ' ';
    buf[MAX_NTP_SERVER_LENTGH] = 0;
    y = getTextWidth(buf);

    for(x = 0; x < cursor; ++x)
        buf[x] = str[x];

    buf[x] = ' ';

    if(str[x] != 0)
    {
        for(; str[x] != 0; ++x)
            buf[x + 1] = str[x];
    }

    buf[++x] = 0;

    x = (SCREEN_WIDTH / 2) - 8 - 3 - (y / 2),
    drawRectFilled(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (44 / 2), y + 16, 3, COLOR_BLUE);
    drawRectFilled(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) + (44 / 2), y + 16, 3, COLOR_BLUE);
    drawRectFilled(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (44 / 2), 3, 44 + 3, COLOR_BLUE);
    drawRectFilled(x + y + 16, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (44 / 2), 3, 44 + 3, COLOR_BLUE);
    x += 8;
    print(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) + (FONT_SIZE / 2), buf);

    for(y = 0; y < cursor; ++y)
        buf[y] = str[y];

    buf[cursor] = 0;
    x += getTextWidth(buf);

    buf[0] = ' ';
    buf[1] = 0;
    x += getTextWidth(buf) / 2;
    x += 3;

    drawRectFilled(x, ((SCREEN_HEIGHT / 2) - ((44 * 5) / 2) + (FONT_SIZE / 2)) + 3, 3, 2, COLOR_BLUE);
    drawRectFilled(x, ((SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (FONT_SIZE / 2)) + 1, 3, 2, COLOR_BLUE);

    x -= 4;
    drawRectFilled(x, ((SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (FONT_SIZE / 2) - 1) + 6, 2, 20, COLOR_BLUE);

    x -= 5;
    drawRectFilled(x, ((SCREEN_HEIGHT / 2) - ((44 * 5) / 2) + (FONT_SIZE / 2)) + 3, 3, 2, COLOR_BLUE);
    drawRectFilled(x, ((SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (FONT_SIZE / 2)) + 1, 3, 2, COLOR_BLUE);

    for(y = SCREEN_HEIGHT - 8 - 5; y > SCREEN_HEIGHT - (44 * 5); y -= 44)
        drawRectFilled(8, y, (STEP * 10) + (3 * 10), 3, COLOR_BORDER);

    for(x = 8; x < 8 + ((STEP + 3) * 11); x += STEP + 3)
         drawRectFilled(x, SCREEN_HEIGHT - 8 - 5 - (44 * 4), 3, (44 * 4) + 3, COLOR_BORDER);

    for(y = 0; y < 3; ++y)
        buf[y] = 0;

    for(y = 0; y < 4; ++y)
    {
        for(x = 0; x < (y < 3 ? 10 : 7); ++x)
        {
            buf[0] = keymap[(y * 10) + x];
            print((8 + 3 + (x * (STEP + 3)) + (STEP / 2)) - (getTextWidth(buf) / 2), SCREEN_HEIGHT - 8 - 5 - (44 * 3) - (FONT_SIZE / 2) + (44 * y), buf);
        }
    }

    mbstowcs(buf, "\uE091", strlen("\uE091")); // <
    print((8 + 3 + (7 * (STEP + 3)) + (STEP / 2)) - (getTextWidth(buf) / 2), SCREEN_HEIGHT - 8 - 5 - (FONT_SIZE / 2), buf);
    mbstowcs(buf, "\uE090", strlen("\uE090")); // >
    print((8 + 3 + (8 * (STEP + 3)) + (STEP / 2)) - (getTextWidth(buf) / 2), SCREEN_HEIGHT - 8 - 5 - (FONT_SIZE / 2), buf);

    mbstowcs(buf, "\uE056", strlen("\uE056")); // ENTER
    print((8 + 3 + (9 * (STEP + 3)) + (STEP / 2)) - (getTextWidth(buf) / 2), SCREEN_HEIGHT - 8 - 5 - (FONT_SIZE / 2), buf);

    OSScreenFlipBuffersEx(SCREEN_DRC);
    OSScreenFlipBuffersEx(SCREEN_TV);
    isBackBuffer = !isBackBuffer;
}

void renderKeyboard(char *str)
{
    void *font = NULL;
    uint32_t size = 0;
    OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font, &size);

    if(!font || !size)
        return;

    pFont.font = sft_loadmem(font, size);
    if(!pFont.font)
        return;

    pFont.xScale = FONT_SIZE;
    pFont.yScale = FONT_SIZE;
    pFont.flags = SFT_DOWNWARD_Y;

    size = *(uint32_t *) tvBuffer;

    // check which buffer is currently used
    OSScreenPutPixelEx(SCREEN_TV, 0, 0, 0xABCDEF90);
    isBackBuffer = *(uint32_t *) tvBuffer != 0xABCDEF90;

    // restore the pixel we used for checking
    *(uint32_t *) tvBuffer = size;

    wchar_t wstr[MAX_NTP_SERVER_LENTGH];
    OSBlockSet(wstr, 0, sizeof(wchar_t) * MAX_NTP_SERVER_LENTGH);
    size = strlen(str);
    for(uint32_t i = 0; i < size; i++)
        wstr[i] = str[i];

    VPADStatus vpad;
    VPADReadError verror;
    bool vpadTouchHeld = false;

    KPADStatus kpad;
    KPADError kerror;

    int32_t x = 0;
    int32_t y = 0;
    uint32_t cursor = size;
    uint32_t cooldown = 26;
    bool trigger = false;

    do
    {
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verror);
        if(verror == VPAD_READ_SUCCESS)
        {
            vpad.trigger |= vpad.hold;

            if(vpad.trigger & VPAD_STICK_L_EMULATION_RIGHT)
                vpad.trigger |= VPAD_BUTTON_RIGHT;
            if(vpad.trigger & VPAD_STICK_L_EMULATION_LEFT)
                vpad.trigger |= VPAD_BUTTON_LEFT;
            if(vpad.trigger & VPAD_STICK_L_EMULATION_DOWN)
                vpad.trigger |= VPAD_BUTTON_DOWN;
            if(vpad.trigger & VPAD_STICK_L_EMULATION_UP)
                vpad.trigger |= VPAD_BUTTON_UP;

            if(vpad.tpNormal.touched)
            {
                if(!vpadTouchHeld)
                {
                    vpadTouchHeld = true;
                    VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &vpad.tpFiltered1, &vpad.tpNormal);

                    vpad.tpFiltered1.x -=  8 + 3;
                    vpad.tpFiltered1.x /= STEP + 3;
                    if(vpad.tpFiltered1.x < 10)
                    {
                        vpad.tpFiltered1.y -= SCREEN_HEIGHT - 8 - 5 - (44 * 4) + 3;
                        vpad.tpFiltered1.y /= 44;
                        if(vpad.tpFiltered1.y < 4)
                        {
                            x = vpad.tpFiltered1.x;
                            y = vpad.tpFiltered1.y;
                            trigger = true;
                            cooldown = 26;
                        }
                    }
                }
            }
            else
                vpadTouchHeld = false;

        }
        else
            vpad.trigger = 0;

        for(int i = 0; i < 4; i++)
            if(KPADReadEx((KPADChan)i, &kpad, 1, &kerror) > 0 && kerror == KPAD_ERROR_OK && kpad.extensionType != 0xFF)
                vpad.trigger |= kpad.extensionType == WPAD_EXT_CORE || kpad.extensionType == WPAD_EXT_NUNCHUK ? mapWiiButtons(kpad.trigger & kpad.hold) : mapClassicButtons(kpad.classic.trigger & kpad.classic.hold);

        if(!cooldown)
        {
            if(vpad.trigger & VPAD_BUTTON_RIGHT)
            {
                if(++x > 9)
                    x = 0;

                cooldown = 25;
            }
            if(vpad.trigger & VPAD_BUTTON_LEFT)
            {
                if(--x < 0)
                    x = 9;

                cooldown = 25;
            }
            if(vpad.trigger& VPAD_BUTTON_DOWN)
            {
                if(++y > 3)
                    y = 0;

                cooldown = 25;
            }
            if(vpad.trigger & VPAD_BUTTON_UP)
            {
                if(--y < 0)
                    y = 3;

                cooldown = 25;
            }
            if(vpad.trigger & VPAD_BUTTON_A)
            {
                trigger = true;
                cooldown = 25;
            }
            if(vpad.trigger & VPAD_BUTTON_B)
            {
                if(!cursor)
                    break;

                if(cursor == size)
                    wstr[--size] = 0;
                else
                {
                    --size;

                    uint32_t i = cursor - 1;
                    do
                    {
                        wstr[i] = wstr[i + 1];
                        if(wstr[i] == 0)
                            break;

                        ++i;
                    }
                    while(1);
                }

                --cursor;
                cooldown = 25;
            }
        }
        else
        {
            if(vpad.trigger || cooldown == 26)
                --cooldown;
            else
                cooldown = 0;
        }

        if(cooldown == 25)
        {
            uint32_t z = (y * 10) + x;
            if(trigger)
            {
                if(z < (4 * 10) - 3)
                {
                    if(size < MAX_NTP_SERVER_LENTGH - 1)
                    {
                        if(cursor != size)
                            for(uint32_t i = size - 1; i > cursor - 1; --i)
                                wstr[i + 1] = wstr[i];

                        wstr[cursor] = keymap[z];
                        if(z != 29 && z > 9)
                            wstr[cursor] += 32;

                        ++size;
                        ++cursor;
                    }
                }
                else if(z == (4 * 10) - 1)
                {
                    if(size)
                    {
                        for(uint32_t i = 0; i < size; ++i)
                            str[i] = wstr[i];

                        str[size] = '\0';
                    }
                    else
                        strcpy(str, "pool.ntp.org");

                    break;
                }
                else if(z == (4 * 10) - 2)
                {
                    if(++cursor > size)
                        --cursor;
                }
                else if(z == (4 * 10) - 3)
                {
                    if(--cursor == (uint32_t)-1)
                        cursor = 0;
                }

                trigger = false;
            }

            drawKeyboard(x, y, wstr, cursor);
        }

        OSSleepTicks(OSMillisecondsToTicks(20));
    } while(1);

    sft_freefont(pFont.font);
}
