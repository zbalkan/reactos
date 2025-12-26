#include <win32k.h>
#include "printredir.h"

/* Define the debug channel so TRACE/ERR/WARN macros work */
DBG_DEFAULT_CHANNEL(UserPainting);
//#define NDEBUG
#include <debug.h>

#define USERTAG_PRINTREDIR 'rPtW'
ATOM AtomPrintWindowCtx = 0;

static void EnsureAtomInit()
{
    if (AtomPrintWindowCtx == 0)
    {
        /* IntAddGlobalAtom expects LPWSTR, not PUNICODE_STRING */
        UNICODE_STRING Name = RTL_CONSTANT_STRING(L"PrintWindowContextInternal");
        AtomPrintWindowCtx = IntAddGlobalAtom(Name.Buffer, FALSE);
    }
}

BOOL FASTCALL
UserPrintRedirectPush(PWND pwndRoot, HDC hdcPrint, const POINT *pptOffset, UINT nFlags)
{
    DPRINT1("UserPrintRedirectPush: Attaching RedirCtx to PWND %p, Target HDC %p\n", pwndRoot, hdcPrint);
    PPRINTWINDOW_CTX pCtx;
    EnsureAtomInit();

    pCtx = ExAllocatePoolWithTag(PagedPool, sizeof(PRINTWINDOW_CTX), USERTAG_PRINTREDIR);
    if (!pCtx) return FALSE;

    pCtx->hdcBlt = hdcPrint;
    pCtx->nFlags = nFlags;
    pCtx->ptOffset = *pptOffset;

    /* Attach context to the window object for cross-process visibility */
    DPRINT1("RedirPush: Context %p attached to PWND %p (Atom: %d)\n", pCtx, pwndRoot, AtomPrintWindowCtx);
    return UserSetProp(pwndRoot, AtomPrintWindowCtx, (HANDLE)pCtx, TRUE);
}

VOID FASTCALL
UserPrintRedirectPop(PWND pwndRoot)
{
    PPRINTWINDOW_CTX pCtx;
    pCtx = (PPRINTWINDOW_CTX)UserRemoveProp(pwndRoot, AtomPrintWindowCtx, TRUE);
    if (pCtx)
    {
        ExFreePoolWithTag(pCtx, USERTAG_PRINTREDIR);
    }
}

BOOL FASTCALL
UserPrintRedirectIsActive(PWND pwnd, HDC *phdcOut, POINT *pptOffsetOut)
{
    PWND cur = pwnd;
    PPRINTWINDOW_CTX pCtx = NULL;

    /* 1. Ensure the atom is ready before we start searching */
    EnsureAtomInit();

    /* 2. Search up the window tree for the redirection root */
    while (cur)
    {
        pCtx = (PPRINTWINDOW_CTX)UserGetProp(cur, AtomPrintWindowCtx, TRUE);
        if (pCtx) break;

        /* Only walk up through parents for child windows to match IntGetPrintWindowCtx logic */
        if (!(cur->style & WS_CHILD))
            break;

        cur = cur->spwndParent;
    }

    /* 3. Handle the result */
    if (pCtx)
    {
        /* Fixed Debug Logic: Now pCtx is actually populated when this triggers */
        TRACE("UserPrintRedirectIsActive: Redirection FOUND for PWND %p -> Target HDC %p\n", pwnd, pCtx->hdcBlt);

        if (phdcOut) *phdcOut = pCtx->hdcBlt;
        if (pptOffsetOut) *pptOffsetOut = pCtx->ptOffset;
        return TRUE;
    }

    return FALSE;
}
