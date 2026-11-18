#include "application/pw_builtin.h"
#include "application/pw_nt7508.h"
#include "project.h"
#include "pokewalker_host.h"
#include "pokewalker.h"
#include <string.h>

/* Two logical images retain bank-selection semantics, with no LCD register
 * model or controller command parser. The source raster is already column
 * bitplanes; preserve those drawings and decode only for native presentation.
 */
static u8 banks[2][8][96][2];
static u8 pixels[96 * 64];
static u8 shown;

static void column(int x, int page, u8 hi, u8 lo)
{
  if (x < 0 || x >= 96 || page < 0 || page >= 8) {
    return;
  }
  banks[g_displayBank.bytes.index & 1][page][x][0] = hi;
  banks[g_displayBank.bytes.index & 1][page][x][1] = lo;
}

static void unpack_bank(u8 bank)
{
  bank &= 1;
  for (unsigned y = 0; y < 64; y++) {
    for (unsigned x = 0; x < 96; x++) {
      const u8 *planes = banks[bank][y / 8][x];
      unsigned bit = y & 7;
      pixels[y * 96 + x] = ((planes[0] >> bit) & 1) * 2 + ((planes[1] >> bit) & 1);
    }
  }
}

static void present(u8 bank)
{
  shown = bank;
  unpack_bank(bank);
  pw_host_present(pixels);
}

void DisplayInit(void)
{
  memset(banks, 0, sizeof(banks));
  memset(pixels, 0, sizeof(pixels));
  shown = 0;
  g_displayBank.bytes.index = 0;
  pw_host_contrast(g_state.save.contrast);
}

void DisplaySetContrast(u8 value)
{
  pw_host_contrast(value);
}

void DisplayAddress(u8 x, u8 page)
{
  (void)x;
  (void)page;
}

void DisplayToggleBank(void)
{
  present(g_displayBank.bytes.index & 1);
  g_displayBank.bytes.index ^= 1;
}

void DisplaySelectBank(u8 bank)
{
  if (bank < 2) {
    if (shown != bank) {
      present(bank);
    }
    g_displayBank.bytes.index = bank ^ 1;
  }
}

void DisplayFillRect(u8 x, u8 y, u8 width, u8 height, u8 shade)
{
  for (unsigned page = y / 8; page < (unsigned)(y + height) / 8; page++) {
    for (unsigned col = x; col < (unsigned)x + width; col++) {
      column(col, page, (shade & 2) ? 255 : 0, (shade & 1) ? 255 : 0);
    }
  }
}

void DisplayFill(u8 shade)
{
  DisplayFillRect(0, 0, 96, 64, shade);
}

void DisplayClear(u8 height)
{
  DisplayFillRect(0, 0, 96, (height / 8) * 8, 0);
}

void DisplayExitPowerSave(void)
{
  pw_host_display_sleep(false);
  present(shown);
}

void DisplayEnterPowerSave(void)
{
  pw_host_display_sleep(true);
}

void RasterOr(u8 width, u8 height, const u8 *source, u8 x, u8 y,
              u8 *destination, u8 destinationWidth, u8 destinationHeight)
{
  (void)destinationHeight;
  u8 band;
  u8 column;
  u8 first;
  u8 second;
  u8 pixel;
  u8 *row;

  height /= RASTER_PAGE_HEIGHT;
  row = destination;
  row += ((y / RASTER_PAGE_HEIGHT) * destinationWidth + x) * 2;
  for (band = 0; band < height; ++band) {
    for (column = 0; column < width; ++column) {
      first = *(source + column * 2);
      second = *(source + column * 2 + 1);
      pixel = *(row + column * 2);
      pixel = pixel | (first << (y & 7));
      *(row + column * 2) = pixel;
      pixel = *(row + column * 2 + 1);
      pixel = pixel | (second << (y & 7));
      *(row + column * 2 + 1) = pixel;
      pixel = *(row + (destinationWidth + column) * 2);
      pixel = pixel | (first >> (RASTER_PAGE_HEIGHT - (y & 7)));
      *(row + (destinationWidth + column) * 2) = pixel;
      pixel = *(row + (destinationWidth + column) * 2 + 1);
      pixel = pixel | (second >> (RASTER_PAGE_HEIGHT - (y & 7)));
      *(row + (destinationWidth + column) * 2 + 1) = pixel;
    }
    row += destinationWidth * 2;
    source += width * 2;
  }
}

/* Keep the original whole-page overwrite and signed-page edge behavior.
 * This intentionally retains WriteSpan's crossed carry planes on middle pages.
 */
static void blit(int x, int y, unsigned width, unsigned height,
                 const u8 *raster, bool span)
{
  unsigned ybit = (unsigned)y & 7;
  int first = span ? y / 8 : (u8)(y / 8);
  int end = span ? (int)((u16)(y + height + 7) >> 3)
                 : (u8)(((int)height + y + 7) / 8);
  const u8 *columns = raster;
  for (int page = first; page < end; page++) {
    if (span && page >= 8) {
      break;
    }
    for (unsigned col = 0; col < width; col++, columns += 2) {
      if (page < 0 || (span && x + (int)col < 0)) {
        continue;
      }
      u8 hi, lo;
      if (!ybit) {
        hi = columns[0];
        lo = columns[1];
      } else if (page == end - 1) {
        hi = columns[-(int)width * 2] >> (8 - ybit);
        lo = columns[-(int)width * 2 + 1] >> (8 - ybit);
      } else if (page != first) {
        hi = (columns[-(int)width * 2 + (span ? 1 : 0)] >> (8 - ybit)) |
             (columns[0] << ybit);
        lo = (columns[-(int)width * 2 + (span ? 0 : 1)] >> (8 - ybit)) |
             (columns[1] << ybit);
      } else {
        hi = columns[0] << ybit;
        lo = columns[1] << ybit;
      }
      column(x + (int)col, page, hi, lo);
    }
  }
}

void DisplayBlit(u8 x, u8 y, u8 width, u8 height, const u8 *raster)
{
  blit(x, (s8)y, width, height, raster, false);
}

void DisplayWriteSpan(s8 x, s8 y, uint width, uint height, RasterColumn *raster)
{
  blit(x, y, width, height, (const u8 *)raster, true);
}

void DisplayText(u8 x, u8 y, const char *text)
{
  unsigned col = x;
  while (*text) {
    unsigned c = (u8)*text++;
    int index = (c >= '0' && c <= '9')   ? (int)c - '0'
                : (c >= 'A' && c <= 'Z') ? (int)c - '7'
                                         : -1;
    for (unsigned i = 0; i < 3; i++) {
      u8 value =
          index < 0
              ? 0
              : g_residentResources.assets.alphanumericFont[index * 3 + i];
      column(col++, y / 8, value, value);
    }
    column(col++, y / 8, 0, 0);
  }
}

void DisplayNameFrame(void)
{
  for (int x = 0; x < 96; x++) {
    u8 value = x == 0 || x == 95 ? 255 : 1;
    column(x, 4, value, value);
  }
  column(0, 5, 255, 255);
  column(95, 5, 255, 255);
}

void DisplayBottomNameFrame(void)
{
  for (int x = 0; x < 96; x++) {
    bool edge = x == 0 || x == 95;
    column(x, 6, edge ? 255 : 1, edge ? 255 : 1);
    column(x, 7, edge ? 255 : 128, edge ? 255 : 128);
  }
}

void DisplayMessageRule(void)
{
  for (int x = 0; x < 96; x++) {
    column(x, 6, 1, 1);
  }
}
