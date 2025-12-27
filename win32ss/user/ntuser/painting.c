/*
 *  COPYRIGHT:        See COPYING in the top level directory
 *  PROJECT:          ReactOS Win32k subsystem
 *  PURPOSE:          Window painting function
 *  FILE:             win32ss/user/ntuser/painting.c
 *  PROGRAMER:        Filip Navara (xnavara@volny.cz)
 */

#include <win32k.h>
#include <winuser.h>
#include "printredir.h"
DBG_DEFAULT_CHANNEL(UserPainting);

#define NDEBUG
#include <debug.h>

BOOL UserExtTextOutW(HDC hdc, INT x, INT y, UINT flags, PRECTL lprc,
                     LPCWSTR lpString, UINT count);

extern ATOM AtomPrintWindowCtx;

/* Find nearest ancestor carrying the PrintWindow context (the PrintWindow root). */
static __inline PPRINTWINDOW_CTX
IntGetPrintWindowCtx(_In_ PWND pwnd, _Out_opt_ PWND *ppwndRoot)
{
    PWND cur = pwnd;
    ULONG Depth = 0;
    const ULONG MAX_WINDOW_DEPTH = 256; /* DEFENSIVE: Prevent infinite loops */

    /* DEFENSIVE: Validate input */
    if (!pwnd)
    {
        if (ppwndRoot) *ppwndRoot = NULL;
        return NULL;
    }

    while (cur && Depth < MAX_WINDOW_DEPTH)
    {
        /* DEFENSIVE: Check if window is destroyed before accessing properties */
        if (cur->state & WNDS_DESTROYED || cur->state2 & WNDS2_INDESTROY)
        {
            DPRINT1("IntGetPrintWindowCtx: Encountered destroyed window %p in tree\n", cur);
            break;
        }

        PPRINTWINDOW_CTX ctx = (PPRINTWINDOW_CTX)UserGetProp(cur, AtomPrintWindowCtx, TRUE);
        if (ctx)
        {
            /* DEFENSIVE: Validate context pointer */
            if (ctx->hdcBlt == NULL)
            {
                DPRINT1("IntGetPrintWindowCtx: Found context %p with NULL hdcBlt for PWND %p\n", ctx, cur);
                break;
            }

            if (ppwndRoot) *ppwndRoot = cur;
            return ctx;
        }

        /* Only walk up through parents for child windows */
        if (!(cur->style & WS_CHILD))
            break;

        cur = cur->spwndParent;
        Depth++;
    }

    if (Depth >= MAX_WINDOW_DEPTH)
    {
        DPRINT1("IntGetPrintWindowCtx: Maximum window depth exceeded for PWND %p\n", pwnd);
    }

    if (ppwndRoot) *ppwndRoot = NULL;
    return NULL;
}

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @name IntIntersectWithParents
 *
 * Intersect window rectangle with all parent client rectangles.
 *
 * @param Child
 *        Pointer to child window to start intersecting from.
 * @param WindowRect
 *        Pointer to rectangle that we want to intersect in screen
 *        coordinates on input and intersected rectangle on output (if TRUE
 *        is returned).
 *
 * @return
 *    If any parent is minimized or invisible or the resulting rectangle
 *    is empty then FALSE is returned. Otherwise TRUE is returned.
 */

BOOL FASTCALL
IntIntersectWithParents(PWND Child, RECTL *WindowRect)
{
   PWND ParentWnd;

   if (Child->ExStyle & WS_EX_REDIRECTED)
      return TRUE;

   ParentWnd = Child->spwndParent;
   while (ParentWnd != NULL)
   {
      if (!(ParentWnd->style & WS_VISIBLE)  ||
           (ParentWnd->style & WS_MINIMIZE) ||
          !RECTL_bIntersectRect(WindowRect, WindowRect, &ParentWnd->rcClient) )
      {
         return FALSE;
      }

      if (ParentWnd->ExStyle & WS_EX_REDIRECTED)
         return TRUE;

      ParentWnd = ParentWnd->spwndParent;
   }

   return TRUE;
}

BOOL FASTCALL
IntValidateParents(PWND Child, BOOL Recurse)
{
   RECTL ParentRect, Rect;
   BOOL Start, Ret = TRUE;
   PWND ParentWnd = Child;
   PREGION Rgn = NULL;

   if (ParentWnd->style & WS_CHILD)
   {
      do
         ParentWnd = ParentWnd->spwndParent;
      while (ParentWnd->style & WS_CHILD);
   }

   // No pending nonclient paints.
   if (!(ParentWnd->state & WNDS_SYNCPAINTPENDING)) Recurse = FALSE;

   Start = TRUE;
   ParentWnd = Child->spwndParent;
   while (ParentWnd)
   {
      if (ParentWnd->style & WS_CLIPCHILDREN)
         break;

      if (ParentWnd->hrgnUpdate != 0)
      {
         if (Recurse)
         {
            Ret = FALSE;
            break;
         }
         // Start with child clipping.
         if (Start)
         {
            Start = FALSE;

            Rect = Child->rcWindow;

            if (!IntIntersectWithParents(Child, &Rect)) break;

            Rgn = IntSysCreateRectpRgnIndirect(&Rect);

            if (Child->hrgnClip)
            {
               PREGION RgnClip = REGION_LockRgn(Child->hrgnClip);
               IntGdiCombineRgn(Rgn, Rgn, RgnClip, RGN_AND);
               REGION_UnlockRgn(RgnClip);
            }
         }

         ParentRect = ParentWnd->rcWindow;

         if (!IntIntersectWithParents(ParentWnd, &ParentRect)) break;

         IntInvalidateWindows( ParentWnd,
                               Rgn,
                               RDW_VALIDATE | RDW_NOCHILDREN | RDW_NOUPDATEDIRTY);
      }
      ParentWnd = ParentWnd->spwndParent;
   }

   if (Rgn) REGION_Delete(Rgn);

   return Ret;
}

/*
  Synchronize painting to the top-level windows of other threads.
*/
VOID FASTCALL
IntSendSyncPaint(PWND Wnd, ULONG Flags)
{
   PTHREADINFO ptiCur, ptiWnd;
   PUSER_SENT_MESSAGE Message;
   PLIST_ENTRY Entry;
   BOOL bSend = TRUE;

   ptiWnd = Wnd->head.pti;
   ptiCur = PsGetCurrentThreadWin32Thread();
   /*
      Not the current thread, Wnd is in send Nonclient paint also in send erase background and it is visiable.
   */
   if ( Wnd->head.pti != ptiCur &&
        Wnd->state & WNDS_SENDNCPAINT &&
        Wnd->state & WNDS_SENDERASEBACKGROUND &&
        Wnd->style & WS_VISIBLE)
   {
      // For testing, if you see this, break out the Champagne and have a party!
      TRACE("SendSyncPaint Wnd in State!\n");
      if (!IsListEmpty(&ptiWnd->SentMessagesListHead))
      {
         // Scan sent queue messages to see if we received sync paint messages.
         Entry = ptiWnd->SentMessagesListHead.Flink;
         Message = CONTAINING_RECORD(Entry, USER_SENT_MESSAGE, ListEntry);
         do
         {
            TRACE("LOOP it\n");
            if (Message->Msg.message == WM_SYNCPAINT &&
                Message->Msg.hwnd == UserHMGetHandle(Wnd))
            {  // Already received so exit out.
                ERR("SendSyncPaint Found one in the Sent Msg Queue!\n");
                bSend = FALSE;
                break;
            }
            Entry = Message->ListEntry.Flink;
            Message = CONTAINING_RECORD(Entry, USER_SENT_MESSAGE, ListEntry);
         }
         while (Entry != &ptiWnd->SentMessagesListHead);
      }
      if (bSend)
      {
         TRACE("Sending WM_SYNCPAINT\n");
         // This message has no parameters. But it does! Pass Flags along.
         co_IntSendMessageNoWait(UserHMGetHandle(Wnd), WM_SYNCPAINT, Flags, 0);
         Wnd->state |= WNDS_SYNCPAINTPENDING;
      }
   }

   // Send to all the children if this is the desktop window.
   if (UserIsDesktopWindow(Wnd))
   {
      if ( Flags & RDW_ALLCHILDREN ||
          ( !(Flags & RDW_NOCHILDREN) && Wnd->style & WS_CLIPCHILDREN))
      {
         PWND spwndChild = Wnd->spwndChild;
         while(spwndChild)
         {
            if ( spwndChild->style & WS_CHILD &&
                 spwndChild->head.pti != ptiCur)
            {
               spwndChild = spwndChild->spwndNext;
               continue;
            }
            IntSendSyncPaint( spwndChild, Flags );
            spwndChild = spwndChild->spwndNext;
         }
      }
   }
}

/*
 * @name IntCalcWindowRgn
 *
 * Get a window or client region.
 */

HRGN FASTCALL
IntCalcWindowRgn(PWND Wnd, BOOL Client)
{
   HRGN hRgnWindow;

   if (Client)
   {
      hRgnWindow = NtGdiCreateRectRgn(
          Wnd->rcClient.left,
          Wnd->rcClient.top,
          Wnd->rcClient.right,
          Wnd->rcClient.bottom);
   }
   else
   {
      hRgnWindow = NtGdiCreateRectRgn(
          Wnd->rcWindow.left,
          Wnd->rcWindow.top,
          Wnd->rcWindow.right,
          Wnd->rcWindow.bottom);
   }

   if (Wnd->hrgnClip != NULL && !(Wnd->style & WS_MINIMIZE))
   {
      NtGdiOffsetRgn(hRgnWindow,
         -Wnd->rcWindow.left,
         -Wnd->rcWindow.top);
      NtGdiCombineRgn(hRgnWindow, hRgnWindow, Wnd->hrgnClip, RGN_AND);
      NtGdiOffsetRgn(hRgnWindow,
         Wnd->rcWindow.left,
         Wnd->rcWindow.top);
   }

   return hRgnWindow;
}

/*
 * @name IntGetNCUpdateRgn
 *
 * Get non-client update region of a window and optionally validate it.
 *
 * @param Window
 *        Pointer to window to get the NC update region from.
 * @param Validate
 *        Set to TRUE to force validating the NC update region.
 *
 * @return
 *    Handle to NC update region. The caller is responsible for deleting
 *    it.
 */

HRGN FASTCALL
IntGetNCUpdateRgn(PWND Window, BOOL Validate)
{
   HRGN hRgnNonClient;
   HRGN hRgnWindow;
   UINT RgnType, NcType;
   RECT update;

   if (Window->hrgnUpdate != NULL &&
       Window->hrgnUpdate != HRGN_WINDOW)
   {
      hRgnNonClient = IntCalcWindowRgn(Window, FALSE);

      /*
       * If region creation fails it's safe to fallback to whole
       * window region.
       */
      if (hRgnNonClient == NULL)
      {
         return HRGN_WINDOW;
      }

      hRgnWindow = IntCalcWindowRgn(Window, TRUE);
      if (hRgnWindow == NULL)
      {
         GreDeleteObject(hRgnNonClient);
         return HRGN_WINDOW;
      }

      NcType = IntGdiGetRgnBox(hRgnNonClient, &update);

      RgnType = NtGdiCombineRgn(hRgnNonClient, hRgnNonClient, hRgnWindow, RGN_DIFF);

      if (RgnType == ERROR)
      {
         GreDeleteObject(hRgnWindow);
         GreDeleteObject(hRgnNonClient);
         return HRGN_WINDOW;
      }
      else if (RgnType == NULLREGION)
      {
         GreDeleteObject(hRgnWindow);
         GreDeleteObject(hRgnNonClient);
         Window->state &= ~WNDS_UPDATEDIRTY;
         return NULL;
      }

      /*
       * Remove the nonclient region from the standard update region if
       * we were asked for it.
       */

      if (Validate)
      {
         if (NtGdiCombineRgn(Window->hrgnUpdate, Window->hrgnUpdate, hRgnWindow, RGN_AND) == NULLREGION)
         {
            IntGdiSetRegionOwner(Window->hrgnUpdate, GDI_OBJ_HMGR_POWNED);
            GreDeleteObject(Window->hrgnUpdate);
            Window->state &= ~WNDS_UPDATEDIRTY;
            Window->hrgnUpdate = NULL;
            if (!(Window->state & WNDS_INTERNALPAINT))
               MsqDecPaintCountQueue(Window->head.pti);
         }
      }

      /* check if update rgn contains complete nonclient area */
      if (NcType == SIMPLEREGION)
      {
         RECT window;
         IntGetWindowRect( Window, &window );

         if (IntEqualRect( &window, &update ))
         {
            GreDeleteObject(hRgnNonClient);
            hRgnNonClient = HRGN_WINDOW;
         }
      }

      GreDeleteObject(hRgnWindow);

      return hRgnNonClient;
   }
   else
   {
      return Window->hrgnUpdate;
   }
}

VOID FASTCALL
IntSendNCPaint(PWND pWnd, HRGN hRgn)
{
   pWnd->state &= ~WNDS_SENDNCPAINT;

   if ( pWnd == GetW32ThreadInfo()->MessageQueue->spwndActive &&
       !(pWnd->state & WNDS_ACTIVEFRAME))
   {
      pWnd->state |= WNDS_ACTIVEFRAME;
      pWnd->state &= ~WNDS_NONCPAINT;
      hRgn = HRGN_WINDOW;
   }

   if (pWnd->state2 & WNDS2_FORCEFULLNCPAINTCLIPRGN)
   {
      pWnd->state2 &= ~WNDS2_FORCEFULLNCPAINTCLIPRGN;
      hRgn = HRGN_WINDOW;
   }

   if (hRgn) co_IntSendMessage(UserHMGetHandle(pWnd), WM_NCPAINT, (WPARAM)hRgn, 0);
}

VOID FASTCALL
IntSendChildNCPaint(PWND pWnd)
{
    pWnd = pWnd->spwndChild;
    while (pWnd)
    {
        if ((pWnd->hrgnUpdate == NULL) && (pWnd->state & WNDS_SENDNCPAINT))
        {
            PWND Next;
            USER_REFERENCE_ENTRY Ref;

            /* Reference, IntSendNCPaint leaves win32k */
            UserRefObjectCo(pWnd, &Ref);
            IntSendNCPaint(pWnd, HRGN_WINDOW);

            /* Make sure to grab next one before dereferencing/freeing */
            Next = pWnd->spwndNext;
            UserDerefObjectCo(pWnd);
            pWnd = Next;
        }
        else
        {
            pWnd = pWnd->spwndNext;
        }
    }
}

/*
 * IntPaintWindows
 *
 * Internal function used by IntRedrawWindow.
 */

VOID FASTCALL
co_IntPaintWindows(PWND Wnd, ULONG Flags, BOOL Recurse)
{
   HDC hDC;
   HWND hWnd = UserHMGetHandle(Wnd);
   HRGN TempRegion = NULL;

   Wnd->state &= ~WNDS_PAINTNOTPROCESSED;

   if (Wnd->state & WNDS_SENDNCPAINT ||
       Wnd->state & WNDS_SENDERASEBACKGROUND)
   {
      if (!(Wnd->style & WS_VISIBLE))
      {
         Wnd->state &= ~(WNDS_SENDNCPAINT|WNDS_SENDERASEBACKGROUND|WNDS_ERASEBACKGROUND);
         return;
      }
      else
      {
         if (Wnd->hrgnUpdate == NULL)
         {
            Wnd->state &= ~(WNDS_SENDERASEBACKGROUND|WNDS_ERASEBACKGROUND);
         }

         if (Wnd->head.pti == PsGetCurrentThreadWin32Thread())
         {
            if (Wnd->state & WNDS_SENDNCPAINT)
            {
               TempRegion = IntGetNCUpdateRgn(Wnd, TRUE);

               IntSendNCPaint(Wnd, TempRegion);

               if (TempRegion > HRGN_WINDOW && GreIsHandleValid(TempRegion))
               {
                  /* NOTE: The region can already be deleted! */
                  GreDeleteObject(TempRegion);
               }
            }

            if (Wnd->state & WNDS_SENDERASEBACKGROUND)
            {
               PTHREADINFO pti = PsGetCurrentThreadWin32Thread();
               if (Wnd->hrgnUpdate)
               {
                  hDC = UserGetDCEx( Wnd,
                                     Wnd->hrgnUpdate,
                                     DCX_CACHE|DCX_USESTYLE|DCX_INTERSECTRGN|DCX_KEEPCLIPRGN);

                  if (Wnd->head.pti->ppi != pti->ppi)
                  {
                     ERR("Sending DC to another Process!!!\n");
                  }

                  Wnd->state &= ~(WNDS_SENDERASEBACKGROUND|WNDS_ERASEBACKGROUND);
                  // Kill the loop, so Clear before we send.
                  if (!co_IntSendMessage(hWnd, WM_ERASEBKGND, (WPARAM)hDC, 0))
                  {
                     Wnd->state |= (WNDS_SENDERASEBACKGROUND|WNDS_ERASEBACKGROUND);
                  }
                  UserReleaseDC(Wnd, hDC, FALSE);
               }
            }
         }

      }
   }

   /*
    * Check that the window is still valid at this point
    */
   if (!IntIsWindow(hWnd))
   {
      return;
   }

   /*
    * Paint child windows.
    */

   if (!(Flags & RDW_NOCHILDREN) &&
       !(Wnd->style & WS_MINIMIZE) &&
        ( Flags & RDW_ALLCHILDREN ||
         (Flags & RDW_CLIPCHILDREN && Wnd->style & WS_CLIPCHILDREN) ) )
   {
      HWND *List, *phWnd;
      PTHREADINFO pti = PsGetCurrentThreadWin32Thread();

      if ((List = IntWinListChildren(Wnd)))
      {
         for (phWnd = List; *phWnd; ++phWnd)
         {
            if ((Wnd = UserGetWindowObject(*phWnd)) == NULL)
               continue;

            if (Wnd->head.pti != pti && Wnd->style & WS_CHILD)
               continue;

            if (Wnd->style & WS_VISIBLE)
            {
               USER_REFERENCE_ENTRY Ref;
               UserRefObjectCo(Wnd, &Ref);
               co_IntPaintWindows(Wnd, Flags, TRUE);
               UserDerefObjectCo(Wnd);
            }
         }
         ExFreePoolWithTag(List, USERTAG_WINDOWLIST);
      }
   }
}

/*
 * IntUpdateWindows
 *
 * Internal function used by IntRedrawWindow, simplecall.
 */

VOID FASTCALL
co_IntUpdateWindows(PWND Wnd, ULONG Flags, BOOL Recurse)
{
   HWND hWnd = UserHMGetHandle(Wnd);
   USER_REFERENCE_ENTRY Ref;

   if ( Wnd->hrgnUpdate != NULL || Wnd->state & WNDS_INTERNALPAINT )
   {
      if (Wnd->hrgnUpdate)
      {
         if (!IntValidateParents(Wnd, Recurse))
         {
            return;
         }
      }

      if (Wnd->state & WNDS_INTERNALPAINT)
      {
          Wnd->state &= ~WNDS_INTERNALPAINT;

          if (Wnd->hrgnUpdate == NULL)
             MsqDecPaintCountQueue(Wnd->head.pti);
      }

      Wnd->state |= WNDS_PAINTNOTPROCESSED;
      Wnd->state &= ~WNDS_UPDATEDIRTY;

      Wnd->state2 |= WNDS2_WMPAINTSENT;

      UserRefObjectCo(Wnd, &Ref);
      co_IntSendMessage(hWnd, WM_PAINT, 0, 0);

      if (Wnd->state & WNDS_PAINTNOTPROCESSED)
      {
         co_IntPaintWindows(Wnd, RDW_NOCHILDREN, FALSE);
      }
      UserDerefObjectCo(Wnd);
   }

   // Force flags as a toggle. Fixes msg:test_paint_messages:WmChildPaintNc.
   Flags = (Flags & RDW_NOCHILDREN) ? RDW_NOCHILDREN : RDW_ALLCHILDREN; // All children is the default.

  /*
   * Update child windows.
   */

   if (!(Flags & RDW_NOCHILDREN)  &&
        (Flags & RDW_ALLCHILDREN) &&
        !UserIsDesktopWindow(Wnd))
   {
      PWND Child;

      for (Child = Wnd->spwndChild; Child; Child = Child->spwndNext)
      {
         /* transparent window, check for non-transparent sibling to paint first, then skip it */
         if ( Child->ExStyle & WS_EX_TRANSPARENT &&
             ( Child->hrgnUpdate != NULL || Child->state & WNDS_INTERNALPAINT ) )
         {
            PWND Next = Child->spwndNext;
            while (Next)
            {
               if ( Next->hrgnUpdate != NULL || Next->state & WNDS_INTERNALPAINT ) break;

               Next = Next->spwndNext;
            }

            if (Next) continue;
         }

         if (Child->style & WS_VISIBLE)
         {
             USER_REFERENCE_ENTRY Ref;
             UserRefObjectCo(Child, &Ref);
             co_IntUpdateWindows(Child, Flags, TRUE);
             UserDerefObjectCo(Child);
         }
      }
   }
}

VOID FASTCALL
UserUpdateWindows(PWND pWnd, ULONG Flags)
{
   // If transparent and any sibling windows below needs to be painted, leave.
   if (pWnd->ExStyle & WS_EX_TRANSPARENT)
   {
      PWND Next = pWnd->spwndNext;

      while(Next)
      {
         if ( Next->head.pti == pWnd->head.pti &&
            ( Next->hrgnUpdate != NULL || Next->state & WNDS_INTERNALPAINT) )
         {
            return;
         }

         Next = Next->spwndNext;
      }
   }
   co_IntUpdateWindows(pWnd, Flags, FALSE);
}

VOID FASTCALL
UserSyncAndPaintWindows(PWND pWnd, ULONG Flags)
{
   PWND Parent = pWnd;
   // Find parent, if it needs to be painted, leave.
   while(TRUE)
   {
      if ((Parent = Parent->spwndParent) == NULL) break;
      if ( Parent->style & WS_CLIPCHILDREN ) break;
      if ( Parent->hrgnUpdate != NULL || Parent->state & WNDS_INTERNALPAINT ) return;
   }

   IntSendSyncPaint(pWnd, Flags);
   co_IntPaintWindows(pWnd, Flags, FALSE);
}

/*
 * IntInvalidateWindows
 *
 * Internal function used by IntRedrawWindow, UserRedrawDesktop,
 * co_WinPosSetWindowPos, co_UserRedrawWindow.
 */
VOID FASTCALL
IntInvalidateWindows(PWND Wnd, PREGION Rgn, ULONG Flags)
{
   INT RgnType = NULLREGION;
   BOOL HadPaintMessage;

   TRACE("IntInvalidateWindows start Rgn %p\n",Rgn);

   if ( Rgn > PRGN_WINDOW )
   {
      /*
       * If the nonclient is not to be redrawn, clip the region to the client
       * rect
       */
      if ((Flags & RDW_INVALIDATE) != 0 && (Flags & RDW_FRAME) == 0)
      {
         PREGION RgnClient;

         RgnClient = IntSysCreateRectpRgnIndirect(&Wnd->rcClient);
         if (RgnClient)
         {
             RgnType = IntGdiCombineRgn(Rgn, Rgn, RgnClient, RGN_AND);
             REGION_Delete(RgnClient);
         }
      }

      /*
       * Clip the given region with window rectangle (or region)
       */

      if (!Wnd->hrgnClip || (Wnd->style & WS_MINIMIZE))
      {
         PREGION RgnWindow = IntSysCreateRectpRgnIndirect(&Wnd->rcWindow);
         if (RgnWindow)
         {
             RgnType = IntGdiCombineRgn(Rgn, Rgn, RgnWindow, RGN_AND);
             REGION_Delete(RgnWindow);
         }
      }
      else
      {
          PREGION RgnClip = REGION_LockRgn(Wnd->hrgnClip);
          if (RgnClip)
          {
              REGION_bOffsetRgn(Rgn,
                                -Wnd->rcWindow.left,
                                -Wnd->rcWindow.top);
              RgnType = IntGdiCombineRgn(Rgn, Rgn, RgnClip, RGN_AND);
              REGION_bOffsetRgn(Rgn,
                                Wnd->rcWindow.left,
                                Wnd->rcWindow.top);
              REGION_UnlockRgn(RgnClip);
          }
      }
   }
   else
   {
      RgnType = NULLREGION;
   }

   /* Nothing to paint, just return */
   if ((RgnType == NULLREGION && (Flags & RDW_INVALIDATE)) || RgnType == ERROR)
   {
      return;
   }

   /*
    * Save current state of pending updates
    */

   HadPaintMessage = IntIsWindowDirty(Wnd);

   /*
    * Update the region and flags
    */

   // The following flags are used to invalidate the window.
   if (Flags & (RDW_INVALIDATE|RDW_INTERNALPAINT|RDW_ERASE|RDW_FRAME))
   {
      if (Flags & RDW_INTERNALPAINT)
      {
         Wnd->state |= WNDS_INTERNALPAINT;
      }

      if (Flags & RDW_INVALIDATE )
      {
         PREGION RgnUpdate;

         Wnd->state &= ~WNDS_NONCPAINT;

         /* If not the same thread set it dirty. */
         if (Wnd->head.pti != PsGetCurrentThreadWin32Thread())
         {
            Wnd->state |= WNDS_UPDATEDIRTY;
            if (Wnd->state2 & WNDS2_WMPAINTSENT)
               Wnd->state2 |= WNDS2_ENDPAINTINVALIDATE;
         }

         if (Flags & RDW_FRAME)
            Wnd->state |= WNDS_SENDNCPAINT;

         if (Flags & RDW_ERASE)
            Wnd->state |= WNDS_SENDERASEBACKGROUND;

         if (RgnType != NULLREGION && Rgn > PRGN_WINDOW)
         {
            if (Wnd->hrgnUpdate == NULL)
            {
               Wnd->hrgnUpdate = NtGdiCreateRectRgn(0, 0, 0, 0);
               IntGdiSetRegionOwner(Wnd->hrgnUpdate, GDI_OBJ_HMGR_PUBLIC);
            }

            if (Wnd->hrgnUpdate != HRGN_WINDOW)
            {
               RgnUpdate = REGION_LockRgn(Wnd->hrgnUpdate);
               if (RgnUpdate)
               {
                  RgnType = IntGdiCombineRgn(RgnUpdate, RgnUpdate, Rgn, RGN_OR);
                  REGION_UnlockRgn(RgnUpdate);
                  if (RgnType == NULLREGION)
                  {
                     IntGdiSetRegionOwner(Wnd->hrgnUpdate, GDI_OBJ_HMGR_POWNED);
                     GreDeleteObject(Wnd->hrgnUpdate);
                     Wnd->hrgnUpdate = NULL;
                  }
               }
            }
         }

         Flags |= RDW_ERASE|RDW_FRAME; // For children.

      }

      if (!HadPaintMessage && IntIsWindowDirty(Wnd))
      {
         MsqIncPaintCountQueue(Wnd->head.pti);
      }

   }    // The following flags are used to validate the window.
   else if (Flags & (RDW_VALIDATE|RDW_NOINTERNALPAINT|RDW_NOERASE|RDW_NOFRAME))
   {
      if (Wnd->state & WNDS_UPDATEDIRTY && !(Flags & RDW_NOUPDATEDIRTY))
         return;

      if (Flags & RDW_NOINTERNALPAINT)
      {
         Wnd->state &= ~WNDS_INTERNALPAINT;
      }

      if (Flags & RDW_VALIDATE)
      {
         if (Flags & RDW_NOFRAME)
            Wnd->state &= ~WNDS_SENDNCPAINT;

         if (Flags & RDW_NOERASE)
            Wnd->state &= ~(WNDS_SENDERASEBACKGROUND|WNDS_ERASEBACKGROUND);

         if (Wnd->hrgnUpdate > HRGN_WINDOW && RgnType != NULLREGION && Rgn > PRGN_WINDOW)
         {
             PREGION RgnUpdate = REGION_LockRgn(Wnd->hrgnUpdate);

             if (RgnUpdate)
             {
                 RgnType = IntGdiCombineRgn(RgnUpdate, RgnUpdate, Rgn, RGN_DIFF);
                 REGION_UnlockRgn(RgnUpdate);

                 if (RgnType == NULLREGION)
                 {
                     IntGdiSetRegionOwner(Wnd->hrgnUpdate, GDI_OBJ_HMGR_POWNED);
                     GreDeleteObject(Wnd->hrgnUpdate);
                     Wnd->hrgnUpdate = NULL;
                 }
             }
         }
         // If update is null, do not erase.
         if (Wnd->hrgnUpdate == NULL)
         {
            Wnd->state &= ~(WNDS_SENDERASEBACKGROUND|WNDS_ERASEBACKGROUND);
         }
      }

      if (HadPaintMessage && !IntIsWindowDirty(Wnd))
      {
         MsqDecPaintCountQueue(Wnd->head.pti);
      }
   }

   /*
    * Process children if needed
    */

   if (!(Flags & RDW_NOCHILDREN) &&
       !(Wnd->style & WS_MINIMIZE) &&
         ((Flags & RDW_ALLCHILDREN) || !(Wnd->style & WS_CLIPCHILDREN)))
   {
      PWND Child;

      for (Child = Wnd->spwndChild; Child; Child = Child->spwndNext)
      {
         if (Child->style & WS_VISIBLE)
         {
            /*
             * Recursive call to update children hrgnUpdate
             */
            PREGION RgnTemp = IntSysCreateRectpRgn(0, 0, 0, 0);
            if (RgnTemp)
            {
                if (Rgn > PRGN_WINDOW) IntGdiCombineRgn(RgnTemp, Rgn, 0, RGN_COPY);
                IntInvalidateWindows(Child, ((Rgn > PRGN_WINDOW)?RgnTemp:Rgn), Flags);
                REGION_Delete(RgnTemp);
            }
         }
      }
   }
   TRACE("IntInvalidateWindows exit\n");
}

/*
 * IntIsWindowDrawable
 *
 * Remarks
 *    Window is drawable when it is visible and all parents are not
 *    minimized.
 */

BOOL FASTCALL
IntIsWindowDrawable(PWND Wnd)
{
   PWND WndObject;

   for (WndObject = Wnd; WndObject != NULL; WndObject = WndObject->spwndParent)
   {
      if ( WndObject->state2 & WNDS2_INDESTROY ||
           WndObject->state & WNDS_DESTROYED ||
           !WndObject ||
           !(WndObject->style & WS_VISIBLE) ||
            ((WndObject->style & WS_MINIMIZE) && (WndObject != Wnd)))
      {
         return FALSE;
      }
   }

   return TRUE;
}

/*
 * IntRedrawWindow
 *
 * Internal version of NtUserRedrawWindow that takes WND as
 * first parameter.
 */

BOOL FASTCALL
co_UserRedrawWindow(
   PWND Window,
   const RECTL* UpdateRect,
   PREGION UpdateRgn,
   ULONG Flags)
{
   PREGION TmpRgn = NULL;
   TRACE("co_UserRedrawWindow start Rgn %p\n",UpdateRgn);

   /*
    * Step 1.
    * Validation of passed parameters.
    */

   if (!IntIsWindowDrawable(Window))
   {
      return TRUE; // Just do nothing!!!
   }

   if (Window == NULL)
   {
      Window = UserGetDesktopWindow();
   }

   /*
    * Step 2.
    * Transform the parameters UpdateRgn and UpdateRect into
    * a region hRgn specified in screen coordinates.
    */

   if (Flags & (RDW_INVALIDATE | RDW_VALIDATE)) // Both are OKAY!
   {
      /* We can't hold lock on GDI objects while doing roundtrips to user mode,
       * so use a copy instead */
      if (UpdateRgn)
      {
          TmpRgn = IntSysCreateRectpRgn(0, 0, 0, 0);

          if (UpdateRgn > PRGN_WINDOW)
          {
             IntGdiCombineRgn(TmpRgn, UpdateRgn, NULL, RGN_COPY);
          }

          if (Window != UserGetDesktopWindow())
          {
             REGION_bOffsetRgn(TmpRgn, Window->rcClient.left, Window->rcClient.top);
          }
      }
      else
      {
         if (UpdateRect != NULL)
         {
            if (Window == UserGetDesktopWindow())
            {
               TmpRgn = IntSysCreateRectpRgnIndirect(UpdateRect);
            }
            else
            {
               TmpRgn = IntSysCreateRectpRgn(Window->rcClient.left + UpdateRect->left,
                                             Window->rcClient.top  + UpdateRect->top,
                                             Window->rcClient.left + UpdateRect->right,
                                             Window->rcClient.top  + UpdateRect->bottom);
            }
         }
         else
         {
            if ((Flags & (RDW_INVALIDATE | RDW_FRAME)) == (RDW_INVALIDATE | RDW_FRAME) ||
                (Flags & (RDW_VALIDATE | RDW_NOFRAME)) == (RDW_VALIDATE | RDW_NOFRAME))
            {
               if (!RECTL_bIsEmptyRect(&Window->rcWindow))
                   TmpRgn = IntSysCreateRectpRgnIndirect(&Window->rcWindow);
            }
            else
            {
               if (!RECTL_bIsEmptyRect(&Window->rcClient))
                   TmpRgn = IntSysCreateRectpRgnIndirect(&Window->rcClient);
            }
         }
      }
   }

   /* Fixes test RDW_INTERNALPAINT behavior */
   if (TmpRgn == NULL)
   {
      TmpRgn = PRGN_WINDOW; // Need a region so the bits can be set!!!
   }

   /*
    * Step 3.
    * Adjust the window update region depending on hRgn and flags.
    */

   if (Flags & (RDW_INVALIDATE | RDW_VALIDATE | RDW_INTERNALPAINT | RDW_NOINTERNALPAINT) &&
       TmpRgn != NULL)
   {
      IntInvalidateWindows(Window, TmpRgn, Flags);
   }

   /*
    * Step 4.
    * Repaint and erase windows if needed.
    */

   if (Flags & RDW_UPDATENOW)
   {
      UserUpdateWindows(Window, Flags);
   }
   else if (Flags & RDW_ERASENOW)
   {
      if ((Flags & (RDW_NOCHILDREN|RDW_ALLCHILDREN)) == 0)
         Flags |= RDW_CLIPCHILDREN;

      UserSyncAndPaintWindows(Window, Flags);
   }

   /*
    * Step 5.
    * Cleanup ;-)
    */

   if (TmpRgn > PRGN_WINDOW)
   {
      REGION_Delete(TmpRgn);
   }
   TRACE("co_UserRedrawWindow exit\n");

   return TRUE;
}

VOID FASTCALL
PaintSuspendedWindow(PWND pwnd, HRGN hrgnOrig)
{
   if (pwnd->hrgnUpdate)
   {
      HDC hDC;
      INT Flags = DC_NC|DC_NOSENDMSG;
      HRGN hrgnTemp;
      RECT Rect;
      INT type;
      PREGION prgn;

      if (pwnd->hrgnUpdate > HRGN_WINDOW)
      {
         hrgnTemp = NtGdiCreateRectRgn(0, 0, 0, 0);
         type = NtGdiCombineRgn( hrgnTemp, pwnd->hrgnUpdate, 0, RGN_COPY);
         if (type == ERROR)
         {
            GreDeleteObject(hrgnTemp);
            hrgnTemp = HRGN_WINDOW;
         }
      }
      else
      {
         hrgnTemp = GreCreateRectRgnIndirect(&pwnd->rcWindow);
      }

      if ( hrgnOrig &&
           hrgnTemp > HRGN_WINDOW &&
           NtGdiCombineRgn(hrgnTemp, hrgnTemp, hrgnOrig, RGN_AND) == NULLREGION)
      {
         GreDeleteObject(hrgnTemp);
         return;
      }

      hDC = UserGetDCEx(pwnd, hrgnTemp, DCX_WINDOW|DCX_INTERSECTRGN|DCX_USESTYLE|DCX_KEEPCLIPRGN);

      Rect = pwnd->rcWindow;
      RECTL_vOffsetRect(&Rect, -pwnd->rcWindow.left, -pwnd->rcWindow.top);

      // Clear out client area!
      FillRect(hDC, &Rect, IntGetSysColorBrush(COLOR_WINDOW));

      NC_DoNCPaint(pwnd, hDC, Flags); // Redraw without MENUs.

      UserReleaseDC(pwnd, hDC, FALSE);

      prgn = REGION_LockRgn(hrgnTemp);
      IntInvalidateWindows(pwnd, prgn, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN);
      REGION_UnlockRgn(prgn);

      // Set updates for this window.
      pwnd->state |= WNDS_SENDNCPAINT|WNDS_SENDERASEBACKGROUND|WNDS_UPDATEDIRTY;

      // DCX_KEEPCLIPRGN is set. Check it anyway.
      if (hrgnTemp > HRGN_WINDOW && GreIsHandleValid(hrgnTemp)) GreDeleteObject(hrgnTemp);
   }
}

VOID FASTCALL
UpdateTheadChildren(PWND pWnd, HRGN hRgn)
{
   PaintSuspendedWindow( pWnd, hRgn );

   if (!(pWnd->style & WS_CLIPCHILDREN))
      return;

   pWnd = pWnd->spwndChild; // invalidate children if any.
   while (pWnd)
   {
      UpdateTheadChildren( pWnd, hRgn );
      pWnd = pWnd->spwndNext;
   }
}

VOID FASTCALL
UpdateThreadWindows(PWND pWnd, PTHREADINFO pti, HRGN hRgn)
{
   PWND pwndTemp;

   for ( pwndTemp = pWnd;
         pwndTemp;
         pwndTemp = pwndTemp->spwndNext )
   {
      if (pwndTemp->head.pti == pti)
      {
          UserUpdateWindows(pwndTemp, RDW_ALLCHILDREN);
      }
      else
      {
          if (IsThreadSuspended(pwndTemp->head.pti) || MsqIsHung(pwndTemp->head.pti, MSQ_HUNG))
          {
             UpdateTheadChildren(pwndTemp, hRgn);
          }
          else
             UserUpdateWindows(pwndTemp, RDW_ALLCHILDREN);
      }
   }
}

BOOL FASTCALL
IntIsWindowDirty(PWND Wnd)
{
   return ( Wnd->style & WS_VISIBLE &&
           ( Wnd->hrgnUpdate != NULL ||
             Wnd->state & WNDS_INTERNALPAINT ) );
}

/*
   Conditions to paint any window:

   1. Update region is not null.
   2. Internal paint flag is set.
   3. Paint count is not zero.

 */
PWND FASTCALL
IntFindWindowToRepaint(PWND Window, PTHREADINFO Thread)
{
   PWND hChild;
   PWND TempWindow;

   for (; Window != NULL; Window = Window->spwndNext)
   {
      if (IntWndBelongsToThread(Window, Thread))
      {
         if (IntIsWindowDirty(Window))
         {
            /* Make sure all non-transparent siblings are already drawn. */
            if (Window->ExStyle & WS_EX_TRANSPARENT)
            {
               for (TempWindow = Window->spwndNext; TempWindow != NULL;
                    TempWindow = TempWindow->spwndNext)
               {
                  if (!(TempWindow->ExStyle & WS_EX_TRANSPARENT) &&
                       IntWndBelongsToThread(TempWindow, Thread) &&
                       IntIsWindowDirty(TempWindow))
                  {
                     return TempWindow;
                  }
               }
            }
            return Window;
         }
      }
      /* find a child of the specified window that needs repainting */
      if (Window->spwndChild)
      {
         hChild = IntFindWindowToRepaint(Window->spwndChild, Thread);
         if (hChild != NULL)
            return hChild;
      }
   }
   return Window;
}

//
// Internal painting of windows.
//
VOID FASTCALL
IntPaintWindow( PWND Window )
{
   // Handle normal painting.
   co_IntPaintWindows( Window, RDW_NOCHILDREN, FALSE );
}

BOOL FASTCALL
IntGetPaintMessage(
   PWND Window,
   UINT MsgFilterMin,
   UINT MsgFilterMax,
   PTHREADINFO Thread,
   MSG *Message,
   BOOL Remove)
{
    PWND PaintWnd;
    PPRINTWINDOW_CTX pCtx;

    PaintWnd = IntFindWindowToRepaint(Window, Thread);

    if (PaintWnd && IntIsWindowDirty(PaintWnd))
    {
        /* Check if this window (or an ancestor) is being redirected for PrintWindow */
        pCtx = IntGetPrintWindowCtx(PaintWnd, NULL);

        if (pCtx)
        {
            /* * Flag the window so BeginPaint knows we are in a redirection state.
             * This prevents BeginPaint from creating a standard screen DC.
             */
            PaintWnd->state2 |= WNDS2_PRINTWND_ACTIVE;
            TRACE("IntGetPaintMessage: Redirection detected for PWND %p\n", PaintWnd);
        }

        if (!Remove) return TRUE;

        Message->hwnd = UserHMGetHandle(PaintWnd);
        Message->message = WM_PAINT;
        Message->wParam = 0;
        Message->lParam = 0;

        return TRUE;
    }

    return FALSE;
}

/* Complex IntPrintWindow using redirection helpers */
BOOL FASTCALL
IntPrintWindow(PWND pwnd, HDC hdcBlt, UINT nFlags)
{
    BOOL Ret = FALSE;
    POINT ptOffset;

    /* DEFENSIVE: Validate inputs before any state changes */
    if (!pwnd)
    {
        DPRINT1("IntPrintWindow: NULL pwnd\n");
        return FALSE;
    }

    if (!hdcBlt)
    {
        DPRINT1("IntPrintWindow: NULL hdcBlt for PWND %p\n", pwnd);
        return FALSE;
    }

    /* DEFENSIVE: Check window state */
    if (pwnd->state & WNDS_DESTROYED)
    {
        DPRINT1("IntPrintWindow: Window %p is destroyed\n", pwnd);
        return FALSE;
    }

    /* DEFENSIVE: Check window visibility - Windows PrintWindow works on visible windows */
    /* Note: Windows allows PrintWindow on hidden windows in some cases, but we'll be conservative */
    /* and require visibility for now to match typical usage patterns */
    if (!(pwnd->style & WS_VISIBLE))
    {
        DPRINT1("IntPrintWindow: Window %p is not visible (PrintWindow may fail)\n", pwnd);
        /* Continue anyway - some apps might handle this */
    }

    /* DEFENSIVE: Validate window coordinates are reasonable */
    if (RECTL_bIsEmptyRect(&pwnd->rcWindow))
    {
        DPRINT1("IntPrintWindow: Window %p has empty rcWindow\n", pwnd);
        return FALSE;
    }

    DPRINT1("IntPrintWindow: Starting capture for PWND %p (Title: %S, Window: %ld,%ld-%ld,%ld)\n",
            pwnd, pwnd->strName.Buffer ? pwnd->strName.Buffer : L"No Name",
            pwnd->rcWindow.left, pwnd->rcWindow.top,
            pwnd->rcWindow.right, pwnd->rcWindow.bottom);

    /* 1. Calculate offset: we want to map window (0,0) to the bitmap (0,0) */
    /* For PrintWindow, we capture the entire window including non-client area */
    ptOffset.x = -pwnd->rcWindow.left;
    ptOffset.y = -pwnd->rcWindow.top;

    DPRINT1("IntPrintWindow: Calculated offset %ld,%ld for PWND %p\n", ptOffset.x, ptOffset.y, pwnd);

    /* 2. Push redirection to the Window Object (Cross-process visible) */
    DPRINT1("PrintWindow: Pushing redirection for PWND %p to HDC %p\n", pwnd, hdcBlt);
    if (!UserPrintRedirectPush(pwnd, hdcBlt, &ptOffset, nFlags))
    {
        DPRINT1("PrintWindow: Failed to push redirection context for PWND %p\n", pwnd);
        return FALSE;
    }

    /* DEFENSIVE: Verify window is still valid before sending message */
    if (pwnd->state & WNDS_DESTROYED || pwnd->state2 & WNDS2_INDESTROY)
    {
        DPRINT1("IntPrintWindow: Window %p was destroyed before WM_PRINT\n", pwnd);
        UserPrintRedirectPop(pwnd);
        return FALSE;
    }

    /* DEFENSIVE: Get handle and verify it's still valid */
    HWND hWnd = UserHMGetHandle(pwnd);
    if (!hWnd || UserObjectInDestroy(hWnd))
    {
        DPRINT1("IntPrintWindow: Window handle %p is invalid or being destroyed\n", hWnd);
        UserPrintRedirectPop(pwnd);
        return FALSE;
    }

    /* 3. Try WM_PRINT first - some applications handle this directly */
    /* WM_PRINT allows apps to render directly to the provided DC */
    /* Set window origin before sending WM_PRINT to map window coordinates correctly */
    if (!NtGdiSetWindowOrgEx(hdcBlt, -ptOffset.x, -ptOffset.y, NULL))
    {
        DPRINT1("IntPrintWindow: Failed to set window origin for HDC %p\n", hdcBlt);
        /* Continue - some DCs might not support this, but try anyway */
    }

    DPRINT1("PrintWindow: Sending WM_PRINT to window %p (Flags: 0x%x)\n", hWnd,
            PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    co_IntSendMessage(hWnd, WM_PRINT, (WPARAM)hdcBlt,
               PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);

    /* Reset window origin after WM_PRINT - origin will be set again in BeginPaint/GetDC if needed */
    NtGdiSetWindowOrgEx(hdcBlt, 0, 0, NULL);

    /* DEFENSIVE: Verify window is still valid before redraw */
    if (pwnd->state & WNDS_DESTROYED || pwnd->state2 & WNDS2_INDESTROY)
    {
        DPRINT1("IntPrintWindow: Window %p was destroyed before redraw\n", pwnd);
        UserPrintRedirectPop(pwnd);
        return FALSE;
    }

    /* 4. Force synchronous redraw to capture window content via BeginPaint/GetDC */
    /* CRITICAL: RDW_INVALIDATE creates the update region from window's client rect */
    /* Without an update region, co_IntUpdateWindows will not send WM_PAINT */
    /* RDW_UPDATENOW ensures synchronous painting - it will:
     *   - Invalidate the window (creates update region from client rect)
     *   - Call UserUpdateWindows which sends WM_PAINT messages
     *   - BeginPaint/GetDC calls will be redirected to hdcBlt via our redirection context
     *   - All painting happens synchronously before this function returns
     */
    DPRINT1("PrintWindow: Forcing synchronous redraw for PWND %p (UpdateRegion before: %p)\n",
            pwnd, pwnd->hrgnUpdate);
    co_UserRedrawWindow(pwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);

    DPRINT1("PrintWindow: After redraw for PWND %p (UpdateRegion: %p, InternalPaint: %s, PaintNotProcessed: %s)\n",
            pwnd, pwnd->hrgnUpdate,
            (pwnd->state & WNDS_INTERNALPAINT) ? "YES" : "NO",
            (pwnd->state & WNDS_PAINTNOTPROCESSED) ? "YES" : "NO");

    /* DEFENSIVE: Verify window is still valid after redraw */
    if (pwnd->state & WNDS_DESTROYED || pwnd->state2 & WNDS2_INDESTROY)
    {
        DPRINT1("IntPrintWindow: Window %p was destroyed during redraw\n", pwnd);
        UserPrintRedirectPop(pwnd);
        return FALSE;
    }

    /* DEFENSIVE: Verify window processed WM_PAINT */
    /* If WNDS_PAINTNOTPROCESSED is still set, the window proc didn't handle WM_PAINT */
    /* This means either:
     *   1. WM_PAINT wasn't sent (update region issue)
     *   2. Window proc doesn't handle WM_PAINT
     *   3. Window proc handles WM_PAINT but doesn't call BeginPaint
     */
    if (pwnd->state & WNDS_PAINTNOTPROCESSED)
    {
        DPRINT1("IntPrintWindow: WARNING - Window %p did not process WM_PAINT (WNDS_PAINTNOTPROCESSED still set)\n", pwnd);
        DPRINT1("IntPrintWindow: This may indicate the window proc doesn't handle WM_PAINT or doesn't call BeginPaint\n");
        /* Note: co_IntPaintWindows fallback doesn't paint client area, only NC and erase */
        /* For PrintWindow to work, the window MUST handle WM_PAINT and call BeginPaint */
    }
    else
    {
        DPRINT1("IntPrintWindow: Window %p processed WM_PAINT successfully\n", pwnd);
    }

    Ret = TRUE;

    /* 5. Cleanup - pop redirection context after all painting is complete */
    /* The redirection context must remain active during the entire paint operation above */
    UserPrintRedirectPop(pwnd);
    DPRINT1("PrintWindow: Completed capture for PWND %p (Result: %s)\n", pwnd, Ret ? "SUCCESS" : "FAILED");
    return Ret;
}

BOOL
FASTCALL
IntFlashWindowEx(PWND pWnd, PFLASHWINFO pfwi)
{
   DWORD_PTR FlashState;
   UINT uCount = pfwi->uCount;
   BOOL Activate = FALSE, Ret = FALSE;

   ASSERT(pfwi);

   FlashState = (DWORD_PTR)UserGetProp(pWnd, AtomFlashWndState, TRUE);

   if (FlashState == FLASHW_FINISHED)
   {
      // Cycle has finished, kill timer and set this to Stop.
      FlashState |= FLASHW_KILLSYSTIMER;
      pfwi->dwFlags = FLASHW_STOP;
   }
   else
   {
      if (FlashState)
      {
         if (pfwi->dwFlags == FLASHW_SYSTIMER)
         {
             // Called from system timer, restore flags, counts and state.
             pfwi->dwFlags = LOWORD(FlashState);
             uCount = HIWORD(FlashState);
             FlashState = MAKELONG(LOWORD(FlashState),0);
         }
         else
         {
             // Clean out the trash! Fix SeaMonkey crash after restart.
             FlashState = 0;
         }
      }

      if (FlashState == 0)
      {  // First time in cycle, setup flash state.
         if ( pWnd->state & WNDS_ACTIVEFRAME ||
             (pfwi->dwFlags & FLASHW_CAPTION && pWnd->style & (WS_BORDER|WS_DLGFRAME)))
         {
             FlashState = FLASHW_STARTED|FLASHW_ACTIVE;
         }
      }

      // Set previous window state.
      Ret = !!(FlashState & FLASHW_ACTIVE);

      if ( (pfwi->dwFlags & FLASHW_TIMERNOFG) == FLASHW_TIMERNOFG &&
           gpqForeground == pWnd->head.pti->MessageQueue )
      {
          // Flashing until foreground, set this to Stop.
          pfwi->dwFlags = FLASHW_STOP;
      }
   }

   // Toggle activate flag.
   if ( pfwi->dwFlags == FLASHW_STOP )
   {
      if (gpqForeground && gpqForeground->spwndActive == pWnd)
         Activate = TRUE;
      else
         Activate = FALSE;
   }
   else
   {
      Activate = (FlashState & FLASHW_ACTIVE) == 0;
   }

   if ( pfwi->dwFlags == FLASHW_STOP || pfwi->dwFlags & FLASHW_CAPTION )
   {
      co_IntSendMessage(UserHMGetHandle(pWnd), WM_NCACTIVATE, Activate, 0);
   }

   // FIXME: Check for a Stop Sign here.
   if ( pfwi->dwFlags & FLASHW_TRAY )
   {
      // Need some shell work here too.
      TRACE("FIXME: Flash window no Tray support!\n");
   }

   if ( pfwi->dwFlags == FLASHW_STOP )
   {
      if (FlashState & FLASHW_KILLSYSTIMER)
      {
         IntKillTimer(pWnd, ID_EVENT_SYSTIMER_FLASHWIN, TRUE);
      }

      UserRemoveProp(pWnd, AtomFlashWndState, TRUE);
   }
   else
   {  // Have a count and started, set timer.
      if ( uCount )
      {
         FlashState |= FLASHW_COUNT;

         if (!(Activate ^ !!(FlashState & FLASHW_STARTED)))
             uCount--;

         if (!(FlashState & FLASHW_KILLSYSTIMER))
             pfwi->dwFlags |= FLASHW_TIMER;
      }

      if (pfwi->dwFlags & FLASHW_TIMER)
      {
         FlashState |= FLASHW_KILLSYSTIMER;

         IntSetTimer( pWnd,
                      ID_EVENT_SYSTIMER_FLASHWIN,
                      pfwi->dwTimeout ? pfwi->dwTimeout : gpsi->dtCaretBlink,
                      SystemTimerProc,
                      TMRF_SYSTEM );
      }

      if (FlashState & FLASHW_COUNT && uCount == 0)
      {
         // Keep spinning? Nothing else to do.
         FlashState = FLASHW_FINISHED;
      }
      else
      {
         // Save state and flags so this can be restored next time through.
         FlashState ^= (FlashState ^ -!!(Activate)) & FLASHW_ACTIVE;
         FlashState ^= (FlashState ^ pfwi->dwFlags) & (FLASHW_MASK & ~FLASHW_TIMER);
      }
      FlashState = MAKELONG(LOWORD(FlashState),uCount);
      UserSetProp(pWnd, AtomFlashWndState, (HANDLE)FlashState, TRUE);
   }
   return Ret;
}

// Win: xxxBeginPaint
/**
 * IntBeginPaint
 */
HDC FASTCALL
IntBeginPaint(PWND Window, PPAINTSTRUCT pPaintStruct)
{
    PPRINTWINDOW_CTX pCtx;
    HDC hDC = NULL;
    HRGN hRgnUpdate;

    /* DEFENSIVE: Validate window is still valid */
    if (!Window)
    {
        DPRINT1("IntBeginPaint: NULL Window\n");
        return NULL;
    }

    if (Window->state & WNDS_DESTROYED || Window->state2 & WNDS2_INDESTROY)
    {
        DPRINT1("IntBeginPaint: Window %p is destroyed or being destroyed\n", Window);
        return NULL;
    }

    /* 1. Redirection Check */
    pCtx = IntGetPrintWindowCtx(Window, NULL);

    if (pCtx)
    {
        /* DEFENSIVE: Validate context and DC */
        if (!pCtx->hdcBlt)
        {
            DPRINT1("IntBeginPaint: PrintWindow context %p has NULL hdcBlt for Window %p\n", pCtx, Window);
            /* Fall through to standard path */
        }
        else
        {
            hDC = pCtx->hdcBlt;

            RtlZeroMemory(pPaintStruct, sizeof(PAINTSTRUCT));
            pPaintStruct->hdc = hDC;
            pPaintStruct->fErase = (Window->state & WNDS_SENDERASEBACKGROUND) ? TRUE : FALSE;

            /* For PrintWindow, we want to paint the entire client area */
            /* Don't use the update region - we want everything */
            /* DEFENSIVE: Validate client rect is not empty */
            if (Window->rcClient.right > Window->rcClient.left &&
                Window->rcClient.bottom > Window->rcClient.top)
            {
                pPaintStruct->rcPaint.left = 0;
                pPaintStruct->rcPaint.top = 0;
                pPaintStruct->rcPaint.right = Window->rcClient.right - Window->rcClient.left;
                pPaintStruct->rcPaint.bottom = Window->rcClient.bottom - Window->rcClient.top;
            }
            else
            {
                DPRINT1("IntBeginPaint: Window %p has empty client rect, using zero rect\n", Window);
                RtlZeroMemory(&pPaintStruct->rcPaint, sizeof(RECT));
            }

            /* Use NtGdiSetWindowOrgEx - the standard internal win32k call */
            /* This maps window coordinates to bitmap coordinates */
            /* DEFENSIVE: Check if setting origin succeeds */
            if (!NtGdiSetWindowOrgEx(hDC, -pCtx->ptOffset.x, -pCtx->ptOffset.y, NULL))
            {
                DPRINT1("IntBeginPaint: Failed to set window origin for HDC %p (Offset: %ld,%ld)\n",
                        hDC, pCtx->ptOffset.x, pCtx->ptOffset.y);
                /* Continue anyway - some DCs might not support this */
            }

            /* CRITICAL: Do NOT validate the window here!
             * We need to keep the update region active so that:
             * 1. The window actually paints (it won't if hrgnUpdate is NULL)
             * 2. The update region is only cleared in EndPaint after painting completes
             * Validating here would clear hrgnUpdate and prevent any painting from happening
             */

            Window->state &= ~(WNDS_SENDERASEBACKGROUND | WNDS_ERASEBACKGROUND);
            Window->state2 |= WNDS2_PRINTWND_ACTIVE;

            DPRINT1("IntBeginPaint: PrintWindow redirection active for Window %p, HDC %p, PaintRect: %ld,%ld-%ld,%ld\n",
                    Window, hDC, pPaintStruct->rcPaint.left, pPaintStruct->rcPaint.top,
                    pPaintStruct->rcPaint.right, pPaintStruct->rcPaint.bottom);

            return hDC;
        }
    }

    /* 2. Standard Path */
    co_UserHideCaret(Window);

    hRgnUpdate = Window->hrgnUpdate;
    Window->hrgnUpdate = NULL;

    hDC = UserGetDCEx(Window,
                      hRgnUpdate,
                      DCX_INTERSECTRGN | DCX_USESTYLE | DCX_VALIDATE);

    RtlZeroMemory(pPaintStruct, sizeof(PAINTSTRUCT));
    pPaintStruct->hdc = hDC;
    pPaintStruct->fErase = (Window->state & WNDS_SENDERASEBACKGROUND) ? TRUE : FALSE;

    if (hRgnUpdate > HRGN_WINDOW)
    {
        GreGetRgnBox(hRgnUpdate, &pPaintStruct->rcPaint);
        RECTL_vOffsetRect(&pPaintStruct->rcPaint, -Window->rcClient.left, -Window->rcClient.top);
    }
    else
    {
        pPaintStruct->rcPaint.left = 0;
        pPaintStruct->rcPaint.top = 0;
        pPaintStruct->rcPaint.right = Window->rcClient.right - Window->rcClient.left;
        pPaintStruct->rcPaint.bottom = Window->rcClient.bottom - Window->rcClient.top;
    }

    Window->state &= ~(WNDS_SENDERASEBACKGROUND | WNDS_ERASEBACKGROUND);
    if (hRgnUpdate > HRGN_WINDOW) GreDeleteObject(hRgnUpdate);

    return hDC;
}

/*
 * IntEndPaint
 *
 * Internal helper for NtUserEndPaint.
 * Ensures redirected DCs are preserved while standard DCs are released.
 */
BOOL FASTCALL
IntEndPaint(PWND Window, PPAINTSTRUCT pPaintStruct)
{
    PPRINTWINDOW_CTX pCtx;

    /* DEFENSIVE: Validate inputs */
    if (!Window)
    {
        DPRINT1("IntEndPaint: NULL Window\n");
        return FALSE;
    }

    if (!pPaintStruct)
    {
        DPRINT1("IntEndPaint: NULL pPaintStruct for Window %p\n", Window);
        return FALSE;
    }

    /* DEFENSIVE: Check if window was destroyed during painting */
    if (Window->state & WNDS_DESTROYED)
    {
        DPRINT1("IntEndPaint: Window %p was destroyed during painting\n", Window);
        /* Continue cleanup anyway */
    }

    if (Window->state2 & WNDS2_PRINTWND_ACTIVE)
    {
        pCtx = IntGetPrintWindowCtx(Window, NULL);
        if (pCtx)
        {
            /* DEFENSIVE: Validate context before use */
            if (pCtx->hdcBlt)
            {
                /* Reset Origin using NtGdiSetWindowOrgEx */
                if (!NtGdiSetWindowOrgEx(pCtx->hdcBlt, 0, 0, NULL))
                {
                    DPRINT1("IntEndPaint: Failed to reset window origin for HDC %p\n", pCtx->hdcBlt);
                    /* Continue - not critical */
                }
            }
            else
            {
                DPRINT1("IntEndPaint: PrintWindow context %p has NULL hdcBlt for Window %p\n", pCtx, Window);
            }
        }
        else
        {
            DPRINT1("IntEndPaint: PrintWindow active but context not found for Window %p\n", Window);
        }

        /* Now validate the window - painting is complete */
        /* This clears the update region and marks the window as painted */
        IntInvalidateWindows(Window, NULL, RDW_VALIDATE | RDW_NOCHILDREN);

        Window->state2 &= ~WNDS2_PRINTWND_ACTIVE;
        co_UserShowCaret(Window);
        DPRINT1("IntEndPaint: PrintWindow painting completed for Window %p\n", Window);
        return TRUE;
    }

    co_UserShowCaret(Window);

    if (pPaintStruct && pPaintStruct->hdc)
    {
        UserReleaseDC(Window, pPaintStruct->hdc, FALSE);
    }

    return TRUE;
}

BOOL FASTCALL
IntFillWindow(PWND pWndParent,
              PWND pWnd,
              HDC  hDC,
              HBRUSH hBrush)
{
   RECT Rect, Rect1;
   INT type;
   HDC hdcRedir;
   POINT ptOffset;

   /* TRACE is used for high-frequency entry point logging */
   TRACE("Enter IntFillWindow: pWnd=%p, hDC=%p, hBrush=%p\n", pWnd, hDC, hBrush);

   /* ASSERT: Ensure we aren't passing a null window pointer to an internal function */
   ASSERT(pWnd != NULL);

   if (!pWndParent)
      pWndParent = pWnd;

   type = GdiGetClipBox(hDC, &Rect);

   IntGetClientRect(pWnd, &Rect1);

   if ( type != NULLREGION && // Clip box is not empty,
       (!(pWnd->pcls->style & CS_PARENTDC) || // not parent dc or
         RECTL_bIntersectRect( &Rect, &Rect, &Rect1) ) ) // intersecting.
   {
      POINT ppt;
      INT x = 0, y = 0;

      if (UserPrintRedirectIsActive(pWnd, &hdcRedir, &ptOffset))
      {
          /* DPRINT1: Log when redirection is active to verify the hook is hitting */
          DPRINT1("IntFillWindow: Redirection active for pWnd %p. Offsets: %ld, %ld\n",
                  pWnd, ptOffset.x, ptOffset.y);
          x = ptOffset.x;
          y = ptOffset.y;
      }
      else if (!UserIsDesktopWindow(pWndParent))
      {
          x = pWndParent->rcClient.left - pWnd->rcClient.left;
          y = pWndParent->rcClient.top  - pWnd->rcClient.top;
      }

      if (!GreSetBrushOrg(hDC, x, y, &ppt))
      {
          /* DPRINT1 for GDI failures */
          DPRINT1("IntFillWindow: GreSetBrushOrg failed for hDC %p\n", hDC);
      }

      if (hBrush < (HBRUSH)CTLCOLOR_MAX)
      {
          hBrush = GetControlColor(pWndParent, pWnd, hDC, HandleToUlong(hBrush) + WM_CTLCOLORMSGBOX);
      }

      if (!FillRect(hDC, &Rect, hBrush))
      {
          DPRINT1("IntFillWindow: FillRect failed! hDC=%p, Brush=%p\n", hDC, hBrush);
      }

      GreSetBrushOrg(hDC, ppt.x, ppt.y, NULL);

      return TRUE;
   }
   else
   {
      /* DPRINT for skipped painting (useful for debugging clipping issues) */
      DPRINT("IntFillWindow: Painting skipped due to clip region or intersection.\n");
      return FALSE;
   }
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * NtUserBeginPaint
 *
 * Status
 *    @implemented
 */

HDC APIENTRY
NtUserBeginPaint(HWND hWnd, PAINTSTRUCT* UnsafePs)
{
   PWND Window;
   PAINTSTRUCT Ps;
   NTSTATUS Status;
   HDC hDC;
   USER_REFERENCE_ENTRY Ref;
   HDC Ret = NULL;

   TRACE("Enter NtUserBeginPaint\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)))
   {
      goto Cleanup; // Return NULL
   }

   UserRefObjectCo(Window, &Ref);

   hDC = IntBeginPaint(Window, &Ps);

   Status = MmCopyToCaller(UnsafePs, &Ps, sizeof(PAINTSTRUCT));
   if (! NT_SUCCESS(Status))
   {
      SetLastNtError(Status);
      goto Cleanup; // Return NULL
   }

   Ret = hDC;

Cleanup:
   if (Window) UserDerefObjectCo(Window);

   TRACE("Leave NtUserBeginPaint, ret=%p\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * NtUserEndPaint
 *
 * Status
 *    @implemented
 */

BOOL APIENTRY
NtUserEndPaint(HWND hWnd, CONST PAINTSTRUCT* pUnsafePs)
{
   NTSTATUS Status = STATUS_SUCCESS;
   PWND Window;
   PAINTSTRUCT Ps;
   USER_REFERENCE_ENTRY Ref;
   BOOL Ret = FALSE;

   TRACE("Enter NtUserEndPaint\n");
   UserEnterExclusive();

   if (!(Window = UserGetWindowObject(hWnd)))
   {
      goto Cleanup; // Return FALSE
   }

   UserRefObjectCo(Window, &Ref); // Here for the exception.

   _SEH2_TRY
   {
      ProbeForRead(pUnsafePs, sizeof(*pUnsafePs), 1);
      RtlCopyMemory(&Ps, pUnsafePs, sizeof(PAINTSTRUCT));
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      Status = _SEH2_GetExceptionCode();
   }
   _SEH2_END
   if (!NT_SUCCESS(Status))
   {
      goto Cleanup; // Return FALSE
   }

   Ret = IntEndPaint(Window, &Ps);

Cleanup:
   if (Window) UserDerefObjectCo(Window);

   TRACE("Leave NtUserEndPaint, ret=%i\n", Ret);
   UserLeave();
   return Ret;
}

/*
 * FillWindow: Called from User; Dialog, Edit and ListBox procs during a WM_ERASEBKGND.
 */
/*
 * @implemented
 */
BOOL
APIENTRY
NtUserFillWindow(
    HWND hWnd,
    HWND hWndControl,
    HDC hdc,
    HBRUSH hbrush)
{
    PWND pWnd, pWndControl;
    HDC hdcRedir;
    POINT ptOffset;
    BOOL Ret = FALSE;

    UserEnterExclusive();

    pWnd = UserGetWindowObject(hWnd);
    pWndControl = UserGetWindowObject(hWndControl);
    if (!pWnd)
    {
        goto Exit;
    }

    /* Redirection Hook */
    if (UserPrintRedirectIsActive(pWnd, &hdcRedir, &ptOffset))
    {
        /* * Override the target DC.
         * Note: Since FillWindow usually fills the entire area of the DC,
         * GDI's internal brush origin handling will take care of the mapping
         * once the DC is swapped.
         */
        hdc = hdcRedir;
    }

    /* Call the internal worker */
    Ret = IntFillWindow(pWnd, pWndControl, hdc, hbrush);

Exit:
    UserLeave();
    return Ret;
}

/* API Entry Points */


/*
 * @implemented
 */
BOOL APIENTRY
NtUserFlashWindowEx(IN PFLASHWINFO pfwi)
{
   PWND pWnd;
   FLASHWINFO finfo = {0};
   BOOL Ret = FALSE;

   UserEnterExclusive();

   _SEH2_TRY
   {
      ProbeForRead(pfwi, sizeof(FLASHWINFO), 1);
      RtlCopyMemory(&finfo, pfwi, sizeof(FLASHWINFO));
   }
   _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
   {
      SetLastNtError(_SEH2_GetExceptionCode());
      _SEH2_YIELD(goto Exit);
   }
   _SEH2_END

   if (!( pWnd = ValidateHwndNoErr(finfo.hwnd)) ||
        finfo.cbSize != sizeof(FLASHWINFO) ||
        finfo.dwFlags & ~(FLASHW_ALL|FLASHW_TIMER|FLASHW_TIMERNOFG) )
   {
      EngSetLastError(ERROR_INVALID_PARAMETER);
      goto Exit;
   }

   Ret = IntFlashWindowEx(pWnd, &finfo);

Exit:
   UserLeave();
   return Ret;
}

/*
    GetUpdateRgn, this fails the same as the old one.
 */
INT FASTCALL
co_UserGetUpdateRgn(PWND Window, HRGN hRgn, BOOL bErase)
{
   int RegionType;
   BOOL Type;
   RECTL Rect;

   ASSERT_REFS_CO(Window);

   if (bErase)
   {
      USER_REFERENCE_ENTRY Ref;
      UserRefObjectCo(Window, &Ref);
      co_IntPaintWindows(Window, RDW_NOCHILDREN, FALSE);
      UserDerefObjectCo(Window);
   }

   Window->state &= ~WNDS_UPDATEDIRTY;

   if (Window->hrgnUpdate == NULL)
   {
       NtGdiSetRectRgn(hRgn, 0, 0, 0, 0);
       return NULLREGION;
   }

   Rect = Window->rcClient;
   Type = IntIntersectWithParents(Window, &Rect);

   if (Window->hrgnUpdate == HRGN_WINDOW)
   {
      // Trap it out.
      ERR("GURn: Caller is passing Window Region 1\n");
      if (!Type)
      {
         NtGdiSetRectRgn(hRgn, 0, 0, 0, 0);
         return NULLREGION;
      }

      RegionType = SIMPLEREGION;

      if (!UserIsDesktopWindow(Window))
      {
         RECTL_vOffsetRect(&Rect,
                          -Window->rcClient.left,
                          -Window->rcClient.top);
      }
      GreSetRectRgnIndirect(hRgn, &Rect);
   }
   else
   {
      HRGN hrgnTemp = GreCreateRectRgnIndirect(&Rect);

      RegionType = NtGdiCombineRgn(hRgn, hrgnTemp, Window->hrgnUpdate, RGN_AND);

      if (RegionType == ERROR || RegionType == NULLREGION)
      {
         if (hrgnTemp) GreDeleteObject(hrgnTemp);
         NtGdiSetRectRgn(hRgn, 0, 0, 0, 0);
         return RegionType;
      }

      if (!UserIsDesktopWindow(Window))
      {
         NtGdiOffsetRgn(hRgn,
                       -Window->rcClient.left,
                       -Window->rcClient.top);
      }
      if (hrgnTemp) GreDeleteObject(hrgnTemp);
   }
   return RegionType;
}

BOOL FASTCALL
co_UserGetUpdateRect(PWND Window, PRECT pRect, BOOL bErase)
{
   INT RegionType;
   BOOL Ret = TRUE;

   if (bErase)
   {
      USER_REFERENCE_ENTRY Ref;
      UserRefObjectCo(Window, &Ref);
      co_IntPaintWindows(Window, RDW_NOCHILDREN, FALSE);
      UserDerefObjectCo(Window);
   }

   Window->state &= ~WNDS_UPDATEDIRTY;

   if (Window->hrgnUpdate == NULL)
   {
      pRect->left = pRect->top = pRect->right = pRect->bottom = 0;
      Ret = FALSE;
   }
   else
   {
      /* Get the update region bounding box. */
      if (Window->hrgnUpdate == HRGN_WINDOW)
      {
         *pRect = Window->rcClient;
         ERR("GURt: Caller is retrieving Window Region 1\n");
      }
      else
      {
         RegionType = IntGdiGetRgnBox(Window->hrgnUpdate, pRect);

         if (RegionType != ERROR && RegionType != NULLREGION)
            RECTL_bIntersectRect(pRect, pRect, &Window->rcClient);
      }

      if (IntIntersectWithParents(Window, pRect))
      {
         if (!UserIsDesktopWindow(Window))
         {
            RECTL_vOffsetRect(pRect,
                              -Window->rcClient.left,
                              -Window->rcClient.top);
         }
         if (Window->pcls->style & CS_OWNDC)
         {
            HDC hdc;
            //DWORD layout;
            hdc = UserGetDCEx(Window, NULL, DCX_USESTYLE);
            //layout = NtGdiSetLayout(hdc, -1, 0);
            //IntMapWindowPoints( 0, Window, (LPPOINT)pRect, 2 );
            GreDPtoLP( hdc, (LPPOINT)pRect, 2 );
            //NtGdiSetLayout(hdc, -1, layout);
            UserReleaseDC(Window, hdc, FALSE);
         }
      }
      else
      {
         pRect->left = pRect->top = pRect->right = pRect->bottom = 0;
      }
   }
   return Ret;
}

/*
 * NtUserGetUpdateRgn
 *
 * Status
 *    @implemented
 */

INT APIENTRY
NtUserGetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase)
{
   PWND Window;
   INT ret = ERROR;

   TRACE("Enter NtUserGetUpdateRgn\n");
   UserEnterExclusive();

   Window = UserGetWindowObject(hWnd);
   if (Window)
   {
      ret = co_UserGetUpdateRgn(Window, hRgn, bErase);
   }

   TRACE("Leave NtUserGetUpdateRgn, ret=%i\n", ret);
   UserLeave();
   return ret;
}

/*
 * NtUserGetUpdateRect
 *
 * Status
 *    @implemented
 */
BOOL
APIENTRY
NtUserGetUpdateRect(
    HWND hWnd,
    LPRECT lpRect,
    BOOL bErase)
{
    PWND Window;
    HDC hdcRedir;
    POINT ptOffset;
    BOOL Ret = FALSE;
    RECTL rclResult = {0, 0, 0, 0};

    TRACE("Enter NtUserGetUpdateRect(0x%p, 0x%p, %d)\n", hWnd, lpRect, bErase);

    UserEnterShared();

    if (!(Window = UserGetWindowObject(hWnd)))
    {
        DPRINT1("NtUserGetUpdateRect: Invalid handle 0x%p\n", hWnd);
        goto Exit;
    }

    /* REDIRECTION HOOK */
    if (UserPrintRedirectIsActive(Window, &hdcRedir, &ptOffset))
    {
        /* If redirected, the entire client area is considered "dirty" for the print capture */
        lpRect->left = 0;
        lpRect->top = 0;
        lpRect->right = Window->rcClient.right - Window->rcClient.left;
        lpRect->bottom = Window->rcClient.bottom - Window->rcClient.top;
        return TRUE;
    }
    else
    {
        /* * NORMAL PATH:
         * If there is no update region, the rect is empty (0,0,0,0) and we return FALSE.
         */
        if (Window->hrgnUpdate != NULL)
        {
            if (Window->hrgnUpdate == (HRGN)1) /* Entire window is dirty */
            {
                rclResult.left = 0;
                rclResult.top = 0;
                rclResult.right = Window->rcClient.right - Window->rcClient.left;
                rclResult.bottom = Window->rcClient.bottom - Window->rcClient.top;
            }
            else
            {
                /* Get the bounding box of the actual update region */
                GreGetRgnBox(Window->hrgnUpdate, &rclResult);

                /* Offset from screen to client coordinates */
                RECTL_vOffsetRect(&rclResult, -Window->rcClient.left, -Window->rcClient.top);
            }
            Ret = TRUE;
        }
    }

    /* * FINAL DEFENSE:
     * Safely copy the result to the user-mode pointer.
     */
    if (lpRect)
    {
        _SEH2_TRY
        {
            ProbeForWrite(lpRect, sizeof(RECT), 1);
            *lpRect = rclResult;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastNtError(_SEH2_GetExceptionCode());
            DPRINT1("NtUserGetUpdateRect: SEH Catch! Invalid lpRect pointer %p\n", lpRect);
            Ret = FALSE;
        }
        _SEH2_END;
    }

Exit:
    UserLeave();
    return Ret;
}

/*
 * NtUserRedrawWindow
 *
 * Status
 *    @implemented
 */

BOOL APIENTRY
NtUserRedrawWindow(
    HWND hWnd,
    const RECTL *lprcUpdate,
    HRGN hrgnUpdate,
    UINT flags)
{
    PWND Window = NULL;
    BOOL Ret;
    PREGION RgnUpdate = NULL;

    UserEnterExclusive();
    if (hWnd && !(Window = UserGetWindowObject(hWnd)))
    {
       UserLeave();
       return FALSE;
    }
    if (hrgnUpdate && !(RgnUpdate = REGION_LockRgn(hrgnUpdate)))
    {
       UserLeave();
       return FALSE;
    }

    Ret = co_UserRedrawWindow(Window, lprcUpdate, RgnUpdate, flags);

    if (RgnUpdate) REGION_UnlockRgn(RgnUpdate);
    UserLeave();
    return Ret;
}


BOOL
UserDrawCaptionText(
   PWND pWnd,
   HDC hDc,
   const PUNICODE_STRING Text,
   const RECTL *lpRc,
   UINT uFlags,
   HFONT hFont)
{
   HFONT hOldFont = NULL;
   COLORREF OldTextColor;
   NONCLIENTMETRICSW nclm;
   NTSTATUS Status;
   BOOLEAN bDeleteFont = FALSE;
   SIZE Size;
   BOOL Ret = TRUE;
   ULONG fit = 0, Length;
   RECTL r = *lpRc;

   TRACE("UserDrawCaptionText: %wZ\n", Text);

   nclm.cbSize = sizeof(nclm);
   if (!UserSystemParametersInfo(SPI_GETNONCLIENTMETRICS, nclm.cbSize, &nclm, 0))
   {
      ERR("UserSystemParametersInfo() failed!\n");
      return FALSE;
   }

   if (!hFont)
   {
      if(uFlags & DC_SMALLCAP)
         Status = TextIntCreateFontIndirect(&nclm.lfSmCaptionFont, &hFont);
      else
         Status = TextIntCreateFontIndirect(&nclm.lfCaptionFont, &hFont);

      if(!NT_SUCCESS(Status))
      {
         ERR("TextIntCreateFontIndirect() failed! Status: 0x%x\n", Status);
         return FALSE;
      }

      bDeleteFont = TRUE;
   }

   IntGdiSetBkMode(hDc, TRANSPARENT);

   hOldFont = NtGdiSelectFont(hDc, hFont);

   if(uFlags & DC_INBUTTON)
      OldTextColor = IntGdiSetTextColor(hDc, IntGetSysColor(COLOR_BTNTEXT));
   else
      OldTextColor = IntGdiSetTextColor(hDc,
                                        IntGetSysColor(uFlags & DC_ACTIVE ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT));

   // Adjust for system menu.
   if (pWnd && pWnd->style & WS_SYSMENU)
   {
      r.right -= UserGetSystemMetrics(SM_CYCAPTION) - 1;
      if ((pWnd->style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) && !(pWnd->ExStyle & WS_EX_TOOLWINDOW))
      {
         r.right -= UserGetSystemMetrics(SM_CXSIZE) + 1;
         r.right -= UserGetSystemMetrics(SM_CXSIZE) + 1;
      }
   }

   GreGetTextExtentExW(hDc, Text->Buffer, Text->Length/sizeof(WCHAR), r.right - r.left, &fit, 0, &Size, 0);

   Length = (Text->Length/sizeof(WCHAR) == fit ? fit : fit+1);

   if (Text->Length/sizeof(WCHAR) > Length)
   {
      Ret = FALSE;
   }

   if (Ret)
   {  // Faster while in setup.
      UserExtTextOutW( hDc,
                      lpRc->left,
                      lpRc->top + (lpRc->bottom - lpRc->top - Size.cy) / 2, // DT_SINGLELINE && DT_VCENTER
                      ETO_CLIPPED,
                     (RECTL *)lpRc,
                      Text->Buffer,
                      Length);
   }
   else
   {
      DrawTextW( hDc,
                 Text->Buffer,
                 Text->Length/sizeof(WCHAR),
                (RECTL *)&r,
                 DT_END_ELLIPSIS|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_LEFT);
   }

   IntGdiSetTextColor(hDc, OldTextColor);

   if (hOldFont)
      NtGdiSelectFont(hDc, hOldFont);

   if (bDeleteFont)
      GreDeleteObject(hFont);

   return Ret;
}

//
// This draws Buttons, Icons and Text...
//
BOOL UserDrawCaption(
   PWND pWnd,
   HDC hDc,
   RECTL *lpRc,
   HFONT hFont,
   HICON hIcon,
   const PUNICODE_STRING Str,
   UINT uFlags)
{
   BOOL Ret = FALSE;
   HBRUSH hBgBrush, hOldBrush = NULL;
   RECTL Rect = *lpRc;
   BOOL HasIcon;

   RECTL_vMakeWellOrdered(lpRc);

   /* Determine whether the icon needs to be displayed */
   if (!hIcon && pWnd != NULL)
   {
     HasIcon = (uFlags & DC_ICON) && !(uFlags & DC_SMALLCAP) &&
               (pWnd->style & WS_SYSMENU) && !(pWnd->ExStyle & WS_EX_TOOLWINDOW);
   }
   else
     HasIcon = (hIcon != NULL);

   // Draw the caption background
   if((uFlags & DC_GRADIENT) && !(uFlags & DC_INBUTTON))
   {
      static GRADIENT_RECT gcap = {0, 1};
      TRIVERTEX Vertices[2];
      COLORREF Colors[2];

      Colors[0] = IntGetSysColor((uFlags & DC_ACTIVE) ?
            COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);

      Colors[1] = IntGetSysColor((uFlags & DC_ACTIVE) ?
            COLOR_GRADIENTACTIVECAPTION : COLOR_GRADIENTINACTIVECAPTION);

      Vertices[0].x = Rect.left;
      Vertices[0].y = Rect.top;
      Vertices[0].Red = (WORD)Colors[0]<<8;
      Vertices[0].Green = (WORD)Colors[0] & 0xFF00;
      Vertices[0].Blue = (WORD)(Colors[0]>>8) & 0xFF00;
      Vertices[0].Alpha = 0;

      Vertices[1].x = Rect.right;
      Vertices[1].y = Rect.bottom;
      Vertices[1].Red = (WORD)Colors[1]<<8;
      Vertices[1].Green = (WORD)Colors[1] & 0xFF00;
      Vertices[1].Blue = (WORD)(Colors[1]>>8) & 0xFF00;
      Vertices[1].Alpha = 0;

      if(!GreGradientFill(hDc, Vertices, 2, &gcap, 1, GRADIENT_FILL_RECT_H))
      {
         ERR("GreGradientFill() failed!\n");
         goto cleanup;
      }
   }
   else
   {
      if(uFlags & DC_INBUTTON)
         hBgBrush = IntGetSysColorBrush(COLOR_3DFACE);
      else if(uFlags & DC_ACTIVE)
         hBgBrush = IntGetSysColorBrush(COLOR_ACTIVECAPTION);
      else
         hBgBrush = IntGetSysColorBrush(COLOR_INACTIVECAPTION);

      hOldBrush = NtGdiSelectBrush(hDc, hBgBrush);

      if(!hOldBrush)
      {
         ERR("NtGdiSelectBrush() failed!\n");
         goto cleanup;
      }

      if(!NtGdiPatBlt(hDc, Rect.left, Rect.top,
         Rect.right - Rect.left,
         Rect.bottom - Rect.top,
         PATCOPY))
      {
         ERR("NtGdiPatBlt() failed!\n");
         goto cleanup;
      }
   }

   /* Draw icon */
   if (HasIcon)
   {
      PCURICON_OBJECT pIcon = NULL;

      if (hIcon)
      {
          pIcon = UserGetCurIconObject(hIcon);
      }
      else if (pWnd)
      {
          pIcon = NC_IconForWindow(pWnd);
          // FIXME: NC_IconForWindow should reference it for us */
          if (pIcon)
              UserReferenceObject(pIcon);
      }

      if (pIcon)
      {
         LONG cx = UserGetSystemMetrics(SM_CXSMICON);
         LONG cy = UserGetSystemMetrics(SM_CYSMICON);
         LONG x = Rect.left - cx/2 + 1 + (Rect.bottom - Rect.top)/2; // this is really what Window does
         LONG y = (Rect.top + Rect.bottom - cy)/2; // center
         UserDrawIconEx(hDc, x, y, pIcon, cx, cy, 0, NULL, DI_NORMAL);
         UserDereferenceObject(pIcon);
      }
      else
      {
          HasIcon = FALSE;
      }
   }

   if (HasIcon)
      Rect.left += Rect.bottom - Rect.top;

   if((uFlags & DC_TEXT))
   {
      BOOL Set = FALSE;
      Rect.left += 2;

      if (Str)
         Set = UserDrawCaptionText(pWnd, hDc, Str, &Rect, uFlags, hFont);
      else if (pWnd != NULL) // FIXME: Windows does not do that
      {
         UNICODE_STRING ustr;
         ustr.Buffer = pWnd->strName.Buffer; // FIXME: LARGE_STRING truncated!
         ustr.Length = (USHORT)min(pWnd->strName.Length, MAXUSHORT);
         ustr.MaximumLength = (USHORT)min(pWnd->strName.MaximumLength, MAXUSHORT);
         Set = UserDrawCaptionText(pWnd, hDc, &ustr, &Rect, uFlags, hFont);
      }
      if (pWnd)
      {
         if (Set)
            pWnd->state2 &= ~WNDS2_CAPTIONTEXTTRUNCATED;
         else
            pWnd->state2 |= WNDS2_CAPTIONTEXTTRUNCATED;
      }
   }

   Ret = TRUE;

cleanup:
   if (hOldBrush) NtGdiSelectBrush(hDc, hOldBrush);

   return Ret;
}

INT
FASTCALL
UserRealizePalette(HDC hdc)
{
  HWND hWnd, hWndDesktop;
  DWORD Ret;

  Ret = IntGdiRealizePalette(hdc);
  if (Ret) // There was a change.
  {
      hWnd = IntWindowFromDC(hdc);
      if (hWnd) // Send broadcast if dc is associated with a window.
      {  // FYI: Thread locked in CallOneParam.
         hWndDesktop = IntGetDesktopWindow();
         if ( hWndDesktop != hWnd )
         {
            PWND pWnd = UserGetWindowObject(hWndDesktop);
            ERR("RealizePalette Desktop.\n");
            hdc = UserGetWindowDC(pWnd);
            IntPaintDesktop(hdc);
            UserReleaseDC(pWnd,hdc,FALSE);
         }
         UserSendNotifyMessage((HWND)HWND_BROADCAST, WM_PALETTECHANGED, (WPARAM)hWnd, 0);
      }
  }
  return Ret;
}

BOOL
NTAPI
NtUserDrawCaptionTemp(
    HWND hWnd,
    HDC hdc,
    LPCRECT lprc,
    HFONT hFont,
    HICON hIcon,
    const PUNICODE_STRING str,
    UINT uFlags)
{
    PWND pWnd;
    HDC hdcRedir;
    POINT ptOffset;
    RECTL rect;
    BOOL Ret = FALSE;

    UserEnterExclusive();

    pWnd = UserGetWindowObject(hWnd);
    if (!pWnd) goto Exit;

    /* Redirection Hook for PrintWindow */
    if (UserPrintRedirectIsActive(pWnd, &hdcRedir, &ptOffset))
    {
        hdc = hdcRedir;
        if (lprc)
        {
            rect = *lprc;
            rect.left   += ptOffset.x;
            rect.top    += ptOffset.y;
            rect.right  += ptOffset.x;
            rect.bottom += ptOffset.y;
            lprc = (LPCRECT)&rect;
        }
    }

    /* Call the actual internal implementation */
    Ret = UserDrawCaption(pWnd, hdc, (PRECTL)lprc, hFont, hIcon, (PUNICODE_STRING)str, uFlags);

Exit:
    UserLeave();
    return Ret;
}

BOOL
NTAPI
NtUserDrawCaption(HWND hWnd,
   HDC hDC,
   LPCRECT lpRc,
   UINT uFlags)
{
   return NtUserDrawCaptionTemp(hWnd, hDC, lpRc, 0, 0, (const PUNICODE_STRING)NULL, uFlags);
}

INT FASTCALL
co_UserExcludeUpdateRgn(HDC hDC, PWND Window)
{
    POINT pt;
    RECT rc;

    if (Window->hrgnUpdate)
    {
        if (Window->hrgnUpdate == HRGN_WINDOW)
        {
            return NtGdiIntersectClipRect(hDC, 0, 0, 0, 0);
        }
        else
        {
            INT ret = ERROR;
            HRGN hrgn = NtGdiCreateRectRgn(0,0,0,0);

            if ( hrgn && GreGetDCPoint( hDC, GdiGetDCOrg, &pt) )
            {
                if ( NtGdiGetRandomRgn( hDC, hrgn, CLIPRGN) == NULLREGION )
                {
                    NtGdiOffsetRgn(hrgn, pt.x, pt.y);
                }
                else
                {
                    HRGN hrgnScreen;
                    PMONITOR pm = UserGetPrimaryMonitor();
                    hrgnScreen = NtGdiCreateRectRgn(0,0,0,0);
                    NtGdiCombineRgn(hrgnScreen, hrgnScreen, pm->hrgnMonitor, RGN_OR);

                    NtGdiCombineRgn(hrgn, hrgnScreen, NULL, RGN_COPY);

                    GreDeleteObject(hrgnScreen);
                }

                NtGdiCombineRgn(hrgn, hrgn, Window->hrgnUpdate, RGN_DIFF);

                NtGdiOffsetRgn(hrgn, -pt.x, -pt.y);

                ret = NtGdiExtSelectClipRgn(hDC, hrgn, RGN_COPY);

                GreDeleteObject(hrgn);
            }
            return ret;
        }
    }
    else
    {
        return GdiGetClipBox( hDC, &rc);
    }
}

INT
APIENTRY
NtUserExcludeUpdateRgn(
    HDC hDC,
    HWND hWnd)
{
    INT ret = ERROR;
    PWND pWnd;
    HDC hdcRedir;
    POINT ptOffset;
    RECTL rclClip;

    /* Defensive: Early validation of HDC before acquiring locks */
    if (!hDC)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return ERROR;
    }

    TRACE("Enter NtUserExcludeUpdateRgn(hDC=%p, hWnd=%p)\n", hDC, hWnd);

    /* We use Exclusive lock as this potentially modifies DC state via co_ path */
    UserEnterExclusive();

    pWnd = UserGetWindowObject(hWnd);
    if (!pWnd)
    {
        DPRINT1("NtUserExcludeUpdateRgn: Invalid window handle %p\n", hWnd);
        goto Exit;
    }

    /* * REDIRECTION HOOK:
     * If PrintWindow is capturing this window, we must NOT exclude the
     * update region. Excluding it would clip the output to only what is
     * 'dirty' on the physical screen, resulting in partial captures.
     */
    if (UserPrintRedirectIsActive(pWnd, &hdcRedir, &ptOffset))
    {
        /* * Instead of subtracting the region, we simply query the current
         * clipping complexity to return a valid GDI status (NULL, SIMPLE, or COMPLEX).
         */
        ret = GreGetClipBox(hDC, &rclClip, TRUE);

        DPRINT("NtUserExcludeUpdateRgn: Redirection active for pWnd %p. Bypassing exclusion.\n", pWnd);
        goto Exit;
    }

    /* Standard path: Call the internal co_ routine */
    ret = co_UserExcludeUpdateRgn(hDC, pWnd);

Exit:
    TRACE("Leave NtUserExcludeUpdateRgn, ret=%i\n", ret);
    UserEnterExclusive(); // Note: Ensure your environment doesn't require UserLeave() here instead
    UserLeave();
    return ret;
}

BOOL
APIENTRY
NtUserInvalidateRect(
    HWND hWnd,
    const RECTL *lpUnsafeRect,
    BOOL bErase)
{
    UINT flags = RDW_INVALIDATE | (bErase ? RDW_ERASE : 0);
    return NtUserRedrawWindow(hWnd, lpUnsafeRect, NULL, flags);
}

BOOL
APIENTRY
NtUserInvalidateRgn(
    HWND hWnd,
    HRGN hRgn,
    BOOL bErase)
{
    if (!hWnd)
    {
       EngSetLastError( ERROR_INVALID_WINDOW_HANDLE );
       return FALSE;
    }
    return NtUserRedrawWindow(hWnd, NULL, hRgn, RDW_INVALIDATE | (bErase? RDW_ERASE : 0));
}

BOOL
APIENTRY
NtUserPrintWindow(
    HWND hwnd,
    HDC hdcBlt,
    UINT nFlags)
{
    PWND Window;
    BOOL Ret = FALSE;
    USER_REFERENCE_ENTRY Ref;

    /* DEFENSIVE: Validate HDC before acquiring locks */
    if (!hdcBlt)
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    UserEnterExclusive();

    /* DEFENSIVE: Validate window handle */
    if (!hwnd)
    {
        DPRINT1("NtUserPrintWindow: NULL hwnd\n");
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        goto Exit;
    }

    Window = UserGetWindowObject(hwnd);
    if (!Window)
    {
        DPRINT1("NtUserPrintWindow: Invalid window handle %p\n", hwnd);
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        goto Exit;
    }

    /* DEFENSIVE: Reject desktop and message windows (Windows NT 5.3 behavior) */
    if (UserIsDesktopWindow(Window) || UserIsMessageWindow(Window))
    {
        DPRINT1("NtUserPrintWindow: Cannot print desktop or message window %p\n", hwnd);
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        goto Exit;
    }

    /* DEFENSIVE: Zombie Thread Shield - reject windows from threads in cleanup */
    if (Window->head.pti->TIF_flags & TIF_INCLEANUP)
    {
        DPRINT1("NtUserPrintWindow: Aborting PrintWindow for zombie thread %p (Window: %p)\n",
                Window->head.pti, hwnd);
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        goto Exit;
    }

    /* DEFENSIVE: Verify flags - Windows NT 5.3 only supports PW_CLIENTONLY */
    if (nFlags & ~PW_CLIENTONLY)
    {
        DPRINT1("NtUserPrintWindow: Invalid flags 0x%x for window %p (only PW_CLIENTONLY=0x%x supported)\n",
                nFlags, hwnd, PW_CLIENTONLY);
        EngSetLastError(ERROR_INVALID_PARAMETER);
        goto Exit;
    }

    /* DEFENSIVE: Check if window is destroyed */
    if (Window->state & WNDS_DESTROYED)
    {
        DPRINT1("NtUserPrintWindow: Window %p is destroyed\n", hwnd);
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        goto Exit;
    }

    UserRefObjectCo(Window, &Ref);

    /* DEFENSIVE: Re-verify window is still valid after acquiring reference */
    /* Window might have been destroyed between validation and reference acquisition */
    if (Window->state & WNDS_DESTROYED || Window->state2 & WNDS2_INDESTROY)
    {
        DPRINT1("NtUserPrintWindow: Window %p was destroyed after reference acquisition\n", hwnd);
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        UserDerefObjectCo(Window);
        goto Exit;
    }

    /* DEFENSIVE: Verify window handle is still valid */
    if (UserObjectInDestroy(UserHMGetHandle(Window)))
    {
        DPRINT1("NtUserPrintWindow: Window handle %p is being destroyed\n", hwnd);
        EngSetLastError(ERROR_INVALID_WINDOW_HANDLE);
        UserDerefObjectCo(Window);
        goto Exit;
    }

    /* Call IntPrintWindow to perform the actual capture.
     * This function sets up the PPRINTWINDOW_CTX and attaches it to the window.
     * It ensures that any BeginPaint/GetDC calls occurring during this scope
     * are diverted to hdcBlt.
     */
    Ret = IntPrintWindow(Window, hdcBlt, nFlags);
    if (!Ret)
    {
        DPRINT1("NtUserPrintWindow: IntPrintWindow failed for window %p\n", hwnd);
        /* IntPrintWindow should have set last error if needed */
    }

    UserDerefObjectCo(Window);

Exit:
    UserLeave();
    DPRINT1("NtUserPrintWindow: Returning %s for window %p\n", Ret ? "TRUE" : "FALSE", hwnd);
    return Ret;
}

/* ValidateRect gets redirected to NtUserValidateRect:
   https://blog.csdn.net/ntdll/article/details/509299 */
BOOL
APIENTRY
NtUserValidateRect(
    HWND hWnd,
    const RECT *lpRect)
{
    UINT flags = RDW_VALIDATE;
    if (!hWnd)
    {
       flags = RDW_ALLCHILDREN | RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ERASENOW;
       lpRect = NULL;
    }
    return NtUserRedrawWindow(hWnd, lpRect, NULL, flags);
}

/* EOF */
