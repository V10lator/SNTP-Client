#include "kbd.h"
#include "schrift.h"
#include <cstdbool>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <coreinit/memory.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <wups.h>

uint8_t *tvBuffer;
uint8_t *drcBuffer;

uint32_t tvSize;
uint32_t drcSize;

static bool isBackBuffer;
static SFT pFont = {};

union Color {
    explicit Color(uint32_t color) {
        this->color = color;
    }

    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        this->r = r;
        this->g = g;
        this->b = b;
        this->a = a;
    }

    uint32_t color{};
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };
};

#define SCREEN_WIDTH  854
#define SCREEN_HEIGHT 480
#define TV_WIDTH  0x500
#define DRC_WIDTH 0x380
#define STEP ((SCREEN_WIDTH - (8 * 2) - (3 * 10)) / 10)
#define COLOR_BACKGROUND Color(238, 238, 238, 255)
#define COLOR_BLACK      Color(0, 0, 0, 255)
#define COLOR_BORDER     Color(204, 204, 204, 255)
#define COLOR_BLUE       Color(0x3478e4FF)
#define FONT_SIZE 24

static char keymap[(10 * 4) - 3] = {
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
            tvSize = OSScreenGetBufferSizeEx(SCREEN_TV);
            break;
        case SCREEN_DRC:
            drcBuffer = (uint8_t *)addr;
            drcSize = OSScreenGetBufferSizeEx(SCREEN_DRC);
            break;
    }

    real_OSScreenSetBufferEx(screen, addr);
}
WUPS_MUST_REPLACE(OSScreenSetBufferEx, WUPS_LOADER_LIBRARY_COREINIT, OSScreenSetBufferEx);

static void drawPixel(uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if(a == 0)
        return;

    float opacity;

    // put pixel in the drc buffer
    uint32_t i = (x + y * DRC_WIDTH) * 4;
    if (i + 3 < drcSize / 2) {
        if (isBackBuffer) {
            i += drcSize / 2;
        }
        if (a == 0xFF) {
            drcBuffer[i]   = r;
            drcBuffer[++i] = g;
            drcBuffer[++i] = b;
        } else {
            opacity = a / 255.0f;
            drcBuffer[i]   = r * opacity + drcBuffer[i] * (1 - opacity);
            drcBuffer[++i] = g * opacity + drcBuffer[i + 1] * (1 - opacity);
            drcBuffer[++i] = b * opacity + drcBuffer[i + 2] * (1 - opacity);
        }
    }

    uint32_t USED_TV_WIDTH = TV_WIDTH;
    float scale            = 1.5f;
    if (tvSize == 0x00FD2000) {
        USED_TV_WIDTH = 1920;
        scale         = 2.25f;
    }

    // scale and put pixel in the tv buffer
    for (uint32_t yy = (y * scale); yy < ((y * scale) + (uint32_t) scale); yy++) {
        for (uint32_t xx = (x * scale); xx < ((x * scale) + (uint32_t) scale); xx++) {
            uint32_t i = (xx + yy * USED_TV_WIDTH) * 4;
            if (i + 3 < tvSize / 2) {
                if (isBackBuffer) {
                    i += tvSize / 2;
                }
                if (a == 0xFF) {
                    tvBuffer[i]   = r;
                    tvBuffer[++i] = g;
                    tvBuffer[++i] = b;
                } else {
                    tvBuffer[i]   = r * opacity + tvBuffer[i] * (1 - opacity);
                    tvBuffer[++i] = g * opacity + tvBuffer[i + 1] * (1 - opacity);
                    tvBuffer[++i] = b * opacity + tvBuffer[i + 2] * (1 - opacity);
                }
            }
        }
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

    auto *src = (uint8_t *) bmp->pixels;

    for (i = x, p = 0; i < x_max; i++, p++) {
        for (j = y, q = 0; j < y_max; j++, q++) {
            if (i < 0 || j < 0 || i >= SCREEN_WIDTH || j >= SCREEN_HEIGHT) {
                continue;
            }

            float opacity = src[q * bmp->width + p] / 255.0f;
            drawPixel(i, j, COLOR_BLACK.r, COLOR_BLACK.g, COLOR_BLACK.b, COLOR_BLACK.a * opacity);
        }
    }
}

static void print(uint32_t x, uint32_t y, const wchar_t *string) {
    auto penX = (int32_t) x;
    auto penY = (int32_t) y;

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
                    .pixels = nullptr,
                    .width  = textureWidth,
                    .height = textureHeight,
            };

            if (textureWidth == 0)
                textureWidth = 4;

            if (textureHeight == 0)
                textureHeight = 4;

            auto buffer = new uint8_t[img.width * img.height];
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

    return (uint32_t) width;
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

void drawKeyboard(uint32_t x, uint32_t y, uint32_t z, wchar_t *str, uint32_t maxLength)
{
    OSScreenClearBufferEx(SCREEN_DRC, COLOR_BACKGROUND.color);
    OSScreenClearBufferEx(SCREEN_TV, COLOR_BACKGROUND.color);

    drawRectFilled(8 + 3 + (x * (STEP + 3)), SCREEN_HEIGHT - 8 - 5 - (44 * 4) + 3 + (y * 44), STEP, 44 - 3, COLOR_BLUE);

    wchar_t buf[maxLength];
    for(y = 0; y < maxLength - 1; ++y)
        buf[y] = 'm';

    buf[maxLength - 1] = 0;
    y = getTextWidth(buf);
    x = (SCREEN_WIDTH / 2) - 8 - 3 - (y / 2),
    drawRectFilled(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (44 / 2), y + 16, 3, COLOR_BLUE);
    drawRectFilled(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) + (44 / 2), y + 16, 3, COLOR_BLUE);
    drawRectFilled(x, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (44 / 2), 3, 44 + 3, COLOR_BLUE);
    drawRectFilled(x + y + 16, (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) - (44 / 2), 3, 44 + 3, COLOR_BLUE);
    print((SCREEN_WIDTH / 2) - 3 - (y / 2), (SCREEN_HEIGHT / 2) - ((44 * 5) / 2) + (FONT_SIZE / 2), str);

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

void renderKeyboard(char *str, uint32_t maxLength)
{
    void *font = nullptr;
    uint32_t size = 0;
    OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font, &size);

    if(!font || !size)
        return;

    pFont.font = sft_loadmem(font, size);
    if(!pFont.font)
    {
        pFont = {};
        return;
    }

    pFont.xScale = FONT_SIZE;
    pFont.yScale = FONT_SIZE;
    pFont.flags = SFT_DOWNWARD_Y;

    size = *(uint32_t *) tvBuffer;

    // check which buffer is currently used
    OSScreenPutPixelEx(SCREEN_TV, 0, 0, 0xABCDEF90);
    isBackBuffer = *(uint32_t *) tvBuffer != 0xABCDEF90;

    // restore the pixel we used for checking
    *(uint32_t *) tvBuffer = size;

    wchar_t wstr[maxLength];
    OSBlockSet(wstr, 0, sizeof(wchar_t) * maxLength);
    size = strlen(str);
    for(uint32_t i = 0; i < size; i++)
        wstr[i] = str[i];

    VPADStatus vpad;
    VPADReadError verror;

    KPADStatus kpad{};
    KPADError kerror;

    int32_t x = 0;
    int32_t y = 0;
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
                if(--size == -1)
                    break;

                wstr[size] = 0;
                cooldown = 25;
            }
        }
        else
            --cooldown;

        if(cooldown == 25)
        {
            uint32_t z = (y * 10) + x;
            if(trigger)
            {
                if(z < (4 * 10) - 3)
                {
                    if(size < maxLength - 1)
                    {
                        wstr[size] = keymap[z];
                        if(z != 29 && z > 9)
                            wstr[size] += 32;

                        ++size;
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
                    // TODO
                }
                else if(z == (4 * 10) - 3)
                {
                    // TODO
                }

                trigger = false;
            }

            drawKeyboard(x, y, z, wstr, maxLength);
        }

        OSSleepTicks(OSMillisecondsToTicks(20));
    } while(1);

    sft_freefont(pFont.font);
    pFont.font = nullptr;
    pFont = {};
}
