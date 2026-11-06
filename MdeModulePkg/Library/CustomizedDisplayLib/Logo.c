#include "CustomizedDisplayLibInternal.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PlatformLogo.h>

VOID
PrintFrontPageLogo (
  IN FORM_DISPLAY_ENGINE_FORM  *FormData
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL           *Gop;
  EDKII_PLATFORM_LOGO_PROTOCOL           *Logo;
  EFI_IMAGE_INPUT                       Image;
  EDKII_PLATFORM_LOGO_DISPLAY_ATTRIBUTE  Attribute;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL          *Scaled;
  UINT32                                Instance = 0;
  INTN                                  OffsetX, OffsetY;
  UINTN                                 Columns, Rows, Scale, GlyphWidth, GlyphHeight;
  UINTN                                 Rightmost, Start, Width, Height, MaxWidth, MaxHeight;
  UINTN                                 ScreenX, ScreenY, Margin, DestinationX, DestinationY;
  CHAR16                                *Text;
  EFI_STATUS                            Status;

  if (!PcdGetBool (PcdFrontPageLogoEnable) || (gBannerData == NULL)) {
    return;
  }

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL) || (Gop->Blt == NULL)) {
    return;
  }

  Status = gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows);
  if (EFI_ERROR (Status) || !Columns || !Rows ||
      (gScreenDimensions.RightColumn > Columns) ||
      (gScreenDimensions.RightColumn <= gScreenDimensions.LeftColumn) ||
      (Rows < FRONT_PAGE_HEADER_HEIGHT) || (gScreenDimensions.TopRow > Rows - FRONT_PAGE_HEADER_HEIGHT))
  {
    return;
  }

  ScreenX     = Gop->Mode->Info->HorizontalResolution;
  ScreenY     = Gop->Mode->Info->VerticalResolution;
  Scale       = MIN (ScreenX / (80 * EFI_GLYPH_WIDTH), ScreenY / (25 * EFI_GLYPH_HEIGHT));
  Scale       = MAX (1, MIN (Scale, PcdGet32 (PcdConOutGlyphScale)));
  GlyphWidth  = EFI_GLYPH_WIDTH * Scale;
  GlyphHeight = EFI_GLYPH_HEIGHT * Scale;
  if ((Columns > ScreenX / GlyphWidth) || (Rows > ScreenY / GlyphHeight)) {
    return;
  }

  Rightmost = gScreenDimensions.LeftColumn;
  for (UINTN Row = 0; Row < BANNER_HEIGHT; Row++) {
    for (UINTN Column = 0; Column < BANNER_COLUMNS; Column++) {
      if (gBannerData->Banner[Row][Column] == 0) {
        continue;
      }

      Text = LibGetToken (gBannerData->Banner[Row][Column], FormData->HiiHandle);
      if (Text == NULL) {
        return;
      }

      Width = LibGetStringWidth (Text) / sizeof (CHAR16);
      FreePool (Text);
      if (Width > 0) {
        Width--;
      }

      Start = gScreenDimensions.LeftColumn + (Column == 0 ? BANNER_LEFT_COLUMN_INDENT :
              (gScreenDimensions.RightColumn - gScreenDimensions.LeftColumn) * Column / BANNER_COLUMNS);
      if ((Start >= gScreenDimensions.RightColumn) || (Width >= gScreenDimensions.RightColumn - Start)) {
        return;
      }

      Rightmost = MAX (Rightmost, Start + Width);
    }
  }

  Margin    = 4 * Scale;
  MaxWidth  = (gScreenDimensions.RightColumn - Rightmost) * GlyphWidth;
  MaxHeight = FRONT_PAGE_HEADER_HEIGHT * GlyphHeight;
  if ((MaxWidth <= 2 * Margin) || (MaxHeight <= 2 * Margin)) {
    return;
  }

  MaxWidth  -= 2 * Margin;
  MaxHeight -= 2 * Margin;
  Status     = gBS->LocateProtocol (&gEdkiiPlatformLogoProtocolGuid, NULL, (VOID **)&Logo);
  if (EFI_ERROR (Status)) {
    return;
  }

  ZeroMem (&Image, sizeof (Image));
  Status = Logo->GetImage (Logo, &Instance, &Image, &Attribute, &OffsetX, &OffsetY);
  if (EFI_ERROR (Status)) {
    return;
  }

  if ((Image.Bitmap == NULL) || !Image.Width || !Image.Height ||
      (Image.Height > MAX_UINTN / Image.Width / sizeof (*Scaled)))
  {
    goto Out;
  }

  Width  = MIN (Image.Width, MaxWidth);
  Height = Image.Height * Width / Image.Width;
  if (Height > MaxHeight) {
    Height = MaxHeight;
    Width  = Image.Width * Height / Image.Height;
  }

  if (!Width || !Height || (Height > MAX_UINTN / Width / sizeof (*Scaled))) {
    goto Out;
  }

  Scaled = AllocatePool (Width * Height * sizeof (*Scaled));
  if (Scaled == NULL) {
    goto Out;
  }

  DestinationX = (ScreenX - Columns * GlyphWidth) / 2 + gScreenDimensions.RightColumn * GlyphWidth - Margin - Width;
  DestinationY = (ScreenY - Rows * GlyphHeight) / 2 + gScreenDimensions.TopRow * GlyphHeight + Margin;
  if ((Image.Flags & EFI_IMAGE_TRANSPARENT) != 0) {
    Status = Gop->Blt (Gop, Scaled, EfiBltVideoToBltBuffer,
                      DestinationX, DestinationY, 0, 0, Width, Height, 0);
    if (EFI_ERROR (Status)) {
      FreePool (Scaled);
      goto Out;
    }
  }

  for (UINTN Row = 0; Row < Height; Row++) {
    for (UINTN Column = 0; Column < Width; Column++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel = Image.Bitmap[(Row * Image.Height / Height) * Image.Width + Column * Image.Width / Width];
      if (((Image.Flags & EFI_IMAGE_TRANSPARENT) == 0) || Pixel.Red || Pixel.Green || Pixel.Blue) {
        Scaled[Row * Width + Column] = Pixel;
      }
    }
  }

  Gop->Blt (Gop, Scaled, EfiBltBufferToVideo, 0, 0, DestinationX, DestinationY, Width, Height, 0);
  FreePool (Scaled);

Out:
  if (Image.Bitmap != NULL) {
    FreePool (Image.Bitmap);
  }
}
