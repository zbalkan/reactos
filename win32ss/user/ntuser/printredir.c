#include <win32k.h>
#include "printredir.h"

/* Define the debug channel so TRACE/ERR/WARN macros work */
DBG_DEFAULT_CHANNEL(UserPainting);
#define NDEBUG
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
    PPRINTWINDOW_CTX pCtx;
    BOOL Ret = FALSE;

    /* DEFENSIVE: Validate inputs before any state changes */
    if (!pwndRoot)
    {
        DPRINT1("UserPrintRedirectPush: NULL pwndRoot\n");
        return FALSE;
    }

    if (!hdcPrint)
    {
        DPRINT1("UserPrintRedirectPush: NULL hdcPrint for PWND %p\n", pwndRoot);
        return FALSE;
    }

    if (!pptOffset)
    {
        DPRINT1("UserPrintRedirectPush: NULL pptOffset for PWND %p\n", pwndRoot);
        return FALSE;
    }

    /* DEFENSIVE: Check if window is in valid state */
    if (pwndRoot->state & WNDS_DESTROYED || pwndRoot->state2 & WNDS2_INDESTROY)
    {
        DPRINT1("UserPrintRedirectPush: Window %p is destroyed or being destroyed\n", pwndRoot);
        return FALSE;
    }

    /* DEFENSIVE: Verify window handle is still valid */
    HWND hWnd = UserHMGetHandle(pwndRoot);
    if (!hWnd || UserObjectInDestroy(hWnd))
    {
        DPRINT1("UserPrintRedirectPush: Window handle %p is invalid or being destroyed for PWND %p\n", hWnd, pwndRoot);
        return FALSE;
    }

    DPRINT1("UserPrintRedirectPush: Attaching RedirCtx to PWND %p, Target HDC %p\n", pwndRoot, hdcPrint);
    EnsureAtomInit();

    /* DEFENSIVE: Allocate context - validate allocation before use */
    pCtx = ExAllocatePoolWithTag(PagedPool, sizeof(PRINTWINDOW_CTX), USERTAG_PRINTREDIR);
    if (!pCtx)
    {
        DPRINT1("UserPrintRedirectPush: Failed to allocate PRINTWINDOW_CTX for PWND %p\n", pwndRoot);
        return FALSE;
    }

    /* Initialize context */
    RtlZeroMemory(pCtx, sizeof(PRINTWINDOW_CTX));
    pCtx->hdcBlt = hdcPrint;
    pCtx->nFlags = nFlags;
    pCtx->ptOffset = *pptOffset;

    /* Attach context to the window object for cross-process visibility */
    /* DEFENSIVE: If UserSetProp fails, we must free the allocated context */
    Ret = UserSetProp(pwndRoot, AtomPrintWindowCtx, (HANDLE)pCtx, TRUE);
    if (!Ret)
    {
        DPRINT1("UserPrintRedirectPush: UserSetProp failed for PWND %p, freeing context\n", pwndRoot);
        ExFreePoolWithTag(pCtx, USERTAG_PRINTREDIR);
        return FALSE;
    }

    DPRINT1("RedirPush: Context %p attached to PWND %p (Atom: %d, Offset: %ld,%ld)\n",
            pCtx, pwndRoot, AtomPrintWindowCtx, pptOffset->x, pptOffset->y);
    return TRUE;
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
    ULONG Depth = 0;
    const ULONG MAX_WINDOW_DEPTH = 256; /* DEFENSIVE: Prevent infinite loops */

    /* DEFENSIVE: Validate input */
    if (!pwnd)
    {
        return FALSE;
    }

    /* DEFENSIVE: Check if window is destroyed */
    if (pwnd->state & WNDS_DESTROYED || pwnd->state2 & WNDS2_INDESTROY)
    {
        return FALSE;
    }

    /* 1. Ensure the atom is ready before we start searching */
    EnsureAtomInit();

    if (AtomPrintWindowCtx == 0)
    {
        /* Atom not initialized - no redirection possible */
        return FALSE;
    }

    /* 2. Search up the window tree for the redirection root */
    /* DEFENSIVE: Limit search depth to prevent infinite loops from corrupted window trees */
    while (cur && Depth < MAX_WINDOW_DEPTH)
    {
        /* DEFENSIVE: Validate window is not destroyed before accessing properties */
        if (cur->state & WNDS_DESTROYED || cur->state2 & WNDS2_INDESTROY)
        {
            DPRINT1("UserPrintRedirectIsActive: Encountered destroyed window %p in tree\n", cur);
            break;
        }

        pCtx = (PPRINTWINDOW_CTX)UserGetProp(cur, AtomPrintWindowCtx, TRUE);
        if (pCtx)
        {
            /* DEFENSIVE: Validate context pointer is reasonable */
            if (pCtx->hdcBlt == NULL)
            {
                DPRINT1("UserPrintRedirectIsActive: Found context %p with NULL hdcBlt for PWND %p\n", pCtx, cur);
                pCtx = NULL;
                break;
            }
            break;
        }

        /* Only walk up through parents for child windows to match IntGetPrintWindowCtx logic */
        if (!(cur->style & WS_CHILD))
            break;

        cur = cur->spwndParent;
        Depth++;
    }

    if (Depth >= MAX_WINDOW_DEPTH)
    {
        DPRINT1("UserPrintRedirectIsActive: Maximum window depth exceeded for PWND %p\n", pwnd);
    }

    /* 3. Handle the result */
    if (pCtx)
    {
        TRACE("UserPrintRedirectIsActive: Redirection FOUND for PWND %p -> Target HDC %p (Depth: %lu)\n",
              pwnd, pCtx->hdcBlt, Depth);

        if (phdcOut) *phdcOut = pCtx->hdcBlt;
        if (pptOffsetOut) *pptOffsetOut = pCtx->ptOffset;
        return TRUE;
    }

    return FALSE;
}
