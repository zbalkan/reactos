#pragma once
#include <ntuser.h>

typedef struct _PRINTWINDOW_CTX
{
    HDC  hdcBlt;
    UINT nFlags;
    POINT ptOffset;
} PRINTWINDOW_CTX, *PPRINTWINDOW_CTX;

extern ATOM AtomPrintWindowCtx;

BOOL FASTCALL UserPrintRedirectPush(PWND pwndRoot, HDC hdcPrint, const POINT *pptOffset, UINT nFlags);
VOID FASTCALL UserPrintRedirectPop(PWND pwndRoot);
BOOL FASTCALL UserPrintRedirectIsActive(PWND pwnd, HDC *phdcOut, POINT *pptOffsetOut);
