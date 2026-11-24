/** @file
  Pixel geometry shared by the graphics text console and its Setup banner.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Uefi.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Library/PcdLib.h>

typedef struct {
  UINTN    Left;
  UINTN    Top;
  UINTN    Width;
  UINTN    Height;
} GRAPHICS_CONSOLE_VIEWPORT;

STATIC
inline
BOOLEAN
GraphicsConsoleViewport (
  IN  UINT32                    ScreenWidth,
  IN  UINT32                    ScreenHeight,
  IN  UINTN                     Columns,
  IN  UINTN                     Rows,
  IN  UINTN                     Scale,
  OUT GRAPHICS_CONSOLE_VIEWPORT *View
  )
{
  if ((Columns == 0) || (Rows == 0) || (Columns > ScreenWidth) || (Rows > ScreenHeight)) {
    return FALSE;
  }

  if (FeaturePcdGet (PcdConOutFullScreen80x25) && (Columns == 80) && (Rows == 25)) {
    View->Width  = ScreenWidth;
    View->Height = ScreenHeight;
  } else {
    if ((Columns > ScreenWidth / (EFI_GLYPH_WIDTH * Scale)) ||
        (Rows > ScreenHeight / (EFI_GLYPH_HEIGHT * Scale)))
    {
      return FALSE;
    }

    View->Width  = Columns * EFI_GLYPH_WIDTH * Scale;
    View->Height = Rows * EFI_GLYPH_HEIGHT * Scale;
  }

  View->Left = (ScreenWidth - View->Width) / 2;
  View->Top  = (ScreenHeight - View->Height) / 2;
  return TRUE;
}

// Split the available pixels among the cells, including any partial remainder.
// Only the 80x25 viewport can have a remainder.
STATIC
inline
UINTN
GraphicsConsoleCellOffset (
  IN UINTN  Index,
  IN UINTN  Count,
  IN UINTN  Pixels
  )
{
  return Index * (Pixels / Count) + (Index * (Pixels % Count)) / Count;
}
