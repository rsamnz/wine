/*
 * Copyright (C) 2004 Mike Hearn, for CodeWeavers
 * Copyright (C) 2005 Robert Shearman
 * Copyright (C) 2008 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <wine/debug.h>
#include <wine/list.h>

#include "explorer_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

struct notify_data  /* platform-independent format for NOTIFYICONDATA */
{
    LONG  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    } u;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    /* data for the icon bitmap */
    UINT width;
    UINT height;
    UINT planes;
    UINT bpp;
};

static int (CDECL *wine_notify_icon)(DWORD,NOTIFYICONDATAW *);

#define ICON_DISPLAY_HIDDEN -1

/* an individual systray icon, unpacked from the NOTIFYICONDATA and always in unicode */
struct icon
{
    struct list    entry;
    HICON          image;    /* the image to render */
    HWND           owner;    /* the HWND passed in to the Shell_NotifyIcon call */
    HWND           window;   /* the adaptor window */
    HWND           tooltip;  /* Icon tooltip */
    UINT           state;    /* state flags */
    UINT           id;       /* the unique id given by the app */
    UINT           callback_message;
    int            display;  /* index in display list, or -1 if hidden */
    WCHAR          tiptext[128]; /* Tooltip text. If empty => tooltip disabled */
    WCHAR          info_text[256];  /* info balloon text */
    WCHAR          info_title[64];  /* info balloon title */
    UINT           info_flags;      /* flags for info balloon */
    UINT           info_timeout;    /* timeout for info balloon */
    HICON          info_icon;       /* info balloon icon */
    UINT           version;         /* notify icon api version */
};

static struct list icon_list = LIST_INIT( icon_list );

struct taskbar_button
{
    struct list entry;
    HWND        hwnd;
    HWND        button;
    BOOL        active;
    BOOL        visible;
};

static struct list taskbar_buttons = LIST_INIT( taskbar_buttons );

static HWND tray_window;

static unsigned int nb_displayed;

static BOOL hide_systray, enable_shell;
static int icon_cx, icon_cy, tray_width, tray_height;
static int start_button_width, taskbar_button_width;
static WCHAR start_label[50];

static struct icon *balloon_icon;
static HWND balloon_window;

#define MIN_DISPLAYED 8
#define ICON_BORDER  2

#define BALLOON_CREATE_TIMER 1
#define BALLOON_SHOW_TIMER   2

#define BALLOON_CREATE_TIMEOUT   2000
#define BALLOON_SHOW_MIN_TIMEOUT 10000
#define BALLOON_SHOW_MAX_TIMEOUT 30000

#define WM_POPUPSYSTEMMENU  0x0313

static LRESULT WINAPI shell_traywnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );
static LRESULT WINAPI tray_icon_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );

static BOOL show_icon( struct icon *icon );
static BOOL hide_icon( struct icon *icon );
static BOOL delete_icon( struct icon *icon );

static WNDCLASSEXW shell_traywnd_class =
{
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_DBLCLKS | CS_HREDRAW,
    .lpfnWndProc = shell_traywnd_proc,
    .hbrBackground = (HBRUSH)COLOR_WINDOW,
    .lpszClassName = L"Shell_TrayWnd",
};
static WNDCLASSEXW tray_icon_class =
{
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
    .lpfnWndProc = tray_icon_wndproc,
    .lpszClassName = L"__wine_tray_icon",
};

static void do_hide_systray(void);
static void do_show_systray(void);

/* Retrieves icon record by owner window and ID */
static struct icon *get_icon(HWND owner, UINT id)
{
    struct icon *this;

    /* search for the icon */
    LIST_FOR_EACH_ENTRY( this, &icon_list, struct icon, entry )
        if ((this->id == id) && (this->owner == owner)) return this;

    return NULL;
}

static void init_common_controls(void)
{
    static BOOL initialized = FALSE;

    if (!initialized)
    {
        INITCOMMONCONTROLSEX init_tooltip;

        init_tooltip.dwSize = sizeof(INITCOMMONCONTROLSEX);
        init_tooltip.dwICC = ICC_TAB_CLASSES|ICC_STANDARD_CLASSES;

        InitCommonControlsEx(&init_tooltip);
        initialized = TRUE;
    }
}

/* Creates tooltip window for icon. */
static void create_tooltip(struct icon *icon)
{
    TTTOOLINFOW ti;

    init_common_controls();
    icon->tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     icon->window, NULL, NULL, NULL );

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;
    SendMessageW(icon->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void set_balloon_position( struct icon *icon )
{
    RECT rect;
    POINT pos;

    GetWindowRect( icon->window, &rect );
    pos.x = (rect.left + rect.right) / 2;
    pos.y = (rect.top + rect.bottom) / 2;
    SendMessageW( balloon_window, TTM_TRACKPOSITION, 0, MAKELONG( pos.x, pos.y ));
}

static void update_systray_balloon_position(void)
{
    RECT rect;
    POINT pos;

    if (!balloon_icon) return;
    GetWindowRect( balloon_icon->window, &rect );
    pos.x = (rect.left + rect.right) / 2;
    pos.y = (rect.top + rect.bottom) / 2;
    SendMessageW( balloon_window, TTM_TRACKPOSITION, 0, MAKELONG( pos.x, pos.y ));
}

static void balloon_create_timer( struct icon *icon )
{
    TTTOOLINFOW ti;

    init_common_controls();
    balloon_window = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_BALLOON | TTS_CLOSE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                      icon->window, NULL, NULL, NULL );

    memset( &ti, 0, sizeof(ti) );
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.uFlags = TTF_TRACK | TTF_IDISHWND;
    ti.lpszText = icon->info_text;
    SendMessageW( balloon_window, TTM_ADDTOOLW, 0, (LPARAM)&ti );
    if ((icon->info_flags & NIIF_ICONMASK) == NIIF_USER)
        SendMessageW( balloon_window, TTM_SETTITLEW, (WPARAM)icon->info_icon, (LPARAM)icon->info_title );
    else
        SendMessageW( balloon_window, TTM_SETTITLEW, icon->info_flags, (LPARAM)icon->info_title );
    balloon_icon = icon;
    update_systray_balloon_position();
    SendMessageW( balloon_window, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti );
    KillTimer( icon->window, BALLOON_CREATE_TIMER );
    SetTimer( icon->window, BALLOON_SHOW_TIMER, icon->info_timeout, NULL );
}

static BOOL show_balloon( struct icon *icon )
{
    if (icon->display == ICON_DISPLAY_HIDDEN) return FALSE;  /* not displayed */
    if (!icon->info_text[0]) return FALSE;  /* no balloon */
    balloon_icon = icon;
    SetTimer( icon->window, BALLOON_CREATE_TIMER, BALLOON_CREATE_TIMEOUT, NULL );
    return TRUE;
}

static void hide_balloon( struct icon *icon )
{
    if (!balloon_icon) return;
    if (balloon_window)
    {
        KillTimer( balloon_icon->window, BALLOON_SHOW_TIMER );
        DestroyWindow( balloon_window );
        balloon_window = 0;
    }
    else KillTimer( balloon_icon->window, BALLOON_CREATE_TIMER );
    balloon_icon = NULL;
}

static void show_next_balloon(void)
{
    struct icon *icon;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        if (show_balloon( icon )) break;
}

static void update_balloon( struct icon *icon )
{
    if (balloon_icon == icon)
    {
        hide_balloon( icon );
        show_balloon( icon );
    }
    else if (!balloon_icon)
    {
        show_balloon( icon );
    }
}

static void balloon_timer( struct icon *icon )
{
    icon->info_text[0] = 0;  /* clear text now that balloon has been shown */
    hide_balloon( icon );
    show_next_balloon();
}

/* Synchronize tooltip text with tooltip window */
static void update_tooltip_text(struct icon *icon)
{
    TTTOOLINFOW ti;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;

    SendMessageW(icon->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

/* get the position of an icon in the stand-alone tray */
static POINT get_icon_pos( struct icon *icon )
{
    POINT pos;

    pos.x = tray_width - icon_cx * (icon->display + 1);
    pos.y = (tray_height - icon_cy) / 2;

    return pos;
}

/* synchronize tooltip position with tooltip window */
static void update_tooltip_position( struct icon *icon )
{
    TTTOOLINFOW ti;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;
    SendMessageW( icon->tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti );
    if (balloon_icon == icon) set_balloon_position( icon );
}

static BOOL notify_owner( struct icon *icon, UINT msg, LPARAM lparam )
{
    WPARAM wp = icon->id;
    LPARAM lp = msg;

    if (icon->version >= NOTIFYICON_VERSION_4)
    {
        POINT pt = {.x = (short)LOWORD(lparam), .y = (short)HIWORD(lparam)};
        ClientToScreen( icon->window, &pt );
        wp = MAKEWPARAM( pt.x, pt.y );
        lp = MAKELPARAM( msg, icon->id );
    }

    TRACE( "relaying 0x%x\n", msg );
    if (!SendNotifyMessageW( icon->owner, icon->callback_message, wp, lp ) &&
        (GetLastError() == ERROR_INVALID_WINDOW_HANDLE))
    {
        WARN( "application window was destroyed, removing icon %u\n", icon->id );
        delete_icon( icon );
        return FALSE;
    }
    return TRUE;
}

/* window procedure for the individual tray icon window */
static LRESULT WINAPI tray_icon_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct icon *icon = (struct icon *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    TRACE( "hwnd %p, msg %#x, wparam %#Ix, lparam %#Ix\n", hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_NCCREATE:
    {
        /* set the icon data for the window from the data passed into CreateWindow */
        const CREATESTRUCTW *info = (const CREATESTRUCTW *)lparam;
        icon = info->lpCreateParams;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)icon );
        break;
    }

    case WM_CREATE:
        icon->window = hwnd;
        create_tooltip( icon );
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc;
        int cx = GetSystemMetrics( SM_CXSMICON );
        int cy = GetSystemMetrics( SM_CYSMICON );

        hdc = BeginPaint( hwnd, &ps );
        GetClientRect( hwnd, &rc );
        TRACE( "painting rect %s\n", wine_dbgstr_rect( &rc ) );
        DrawIconEx( hdc, (rc.left + rc.right - cx) / 2, (rc.top + rc.bottom - cy) / 2,
                    icon->image, cx, cy, 0, 0, DI_DEFAULTSIZE | DI_NORMAL );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    {
        MSG message = {.hwnd = hwnd, .message = msg, .wParam = wparam, .lParam = lparam};
        SendMessageW( icon->tooltip, TTM_RELAYEVENT, 0, (LPARAM)&message );
        if (!notify_owner( icon, msg, lparam )) break;
        if (icon->version > 0)
        {
            if (msg == WM_LBUTTONUP) notify_owner( icon, NIN_SELECT, lparam );
            if (msg == WM_RBUTTONUP) notify_owner( icon, WM_CONTEXTMENU, lparam );
        }
        break;
    }

    case WM_WINDOWPOSCHANGED:
        update_systray_balloon_position();
        break;

    case WM_TIMER:
        switch (wparam)
        {
        case BALLOON_CREATE_TIMER: balloon_create_timer( icon ); break;
        case BALLOON_SHOW_TIMER: balloon_timer( icon ); break;
        }
        return 0;
    }

    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* add an icon to the system tray window */
static void systray_add_icon( struct icon *icon )
{
    POINT pos;

    if (icon->display != ICON_DISPLAY_HIDDEN) return;  /* already added */

    icon->display = nb_displayed++;
    pos = get_icon_pos( icon );
    SetWindowPos( icon->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW );

    if (nb_displayed == 1 && !hide_systray) do_show_systray();
    TRACE( "added %u now %d icons\n", icon->id, nb_displayed );
}

/* remove an icon from the stand-alone tray */
static void systray_remove_icon( struct icon *icon )
{
    struct icon *ptr;
    POINT pos;

    if (icon->display == ICON_DISPLAY_HIDDEN) return;  /* already removed */

    assert( nb_displayed );
    LIST_FOR_EACH_ENTRY( ptr, &icon_list, struct icon, entry )
    {
        if (ptr == icon) continue;
        if (ptr->display < icon->display) continue;
        ptr->display--;
        update_tooltip_position( ptr );
        pos = get_icon_pos( ptr );
        SetWindowPos( ptr->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
    }

    if (!--nb_displayed && !enable_shell) do_hide_systray();
    TRACE( "removed %u now %d icons\n", icon->id, nb_displayed );

    icon->display = ICON_DISPLAY_HIDDEN;
}

/* make an icon visible */
static BOOL show_icon(struct icon *icon)
{
    TRACE( "id=0x%x, hwnd=%p\n", icon->id, icon->owner );

    if (icon->display != ICON_DISPLAY_HIDDEN) return TRUE;  /* already displayed */

    systray_add_icon( icon );

    update_tooltip_position( icon );
    update_balloon( icon );
    return TRUE;
}

/* make an icon invisible */
static BOOL hide_icon(struct icon *icon)
{
    TRACE( "id=0x%x, hwnd=%p\n", icon->id, icon->owner );

    if (icon->display == ICON_DISPLAY_HIDDEN) return TRUE;  /* already hidden */

    ShowWindow( icon->window, SW_HIDE );
    systray_remove_icon( icon );

    update_balloon( icon );
    update_tooltip_position( icon );
    return TRUE;
}

/* Modifies an existing icon record */
static BOOL modify_icon( struct icon *icon, NOTIFYICONDATAW *nid )
{
    TRACE( "id=0x%x, hwnd=%p\n", nid->uID, nid->hWnd );

    /* demarshal the request from the NID */
    if (!icon)
    {
        WARN( "Invalid icon ID (0x%x) for HWND %p\n", nid->uID, nid->hWnd );
        return FALSE;
    }

    if (nid->uFlags & NIF_STATE)
    {
        icon->state = (icon->state & ~nid->dwStateMask) | (nid->dwState & nid->dwStateMask);
    }

    if (nid->uFlags & NIF_ICON)
    {
        if (icon->image) DestroyIcon(icon->image);
        icon->image = CopyIcon(nid->hIcon);
        if (icon->display >= 0) InvalidateRect( icon->window, NULL, TRUE );
    }

    if (nid->uFlags & NIF_MESSAGE)
    {
        icon->callback_message = nid->uCallbackMessage;
    }
    if (nid->uFlags & NIF_TIP)
    {
        lstrcpynW( icon->tiptext, nid->szTip, ARRAY_SIZE( icon->tiptext ));
        if (icon->display != ICON_DISPLAY_HIDDEN) update_tooltip_text(icon);
    }
    if (nid->uFlags & NIF_INFO && nid->cbSize >= NOTIFYICONDATAA_V2_SIZE)
    {
        lstrcpynW( icon->info_text, nid->szInfo, ARRAY_SIZE( icon->info_text ));
        lstrcpynW( icon->info_title, nid->szInfoTitle, ARRAY_SIZE( icon->info_title ));
        icon->info_flags = nid->dwInfoFlags;
        icon->info_timeout = max(min(nid->uTimeout, BALLOON_SHOW_MAX_TIMEOUT), BALLOON_SHOW_MIN_TIMEOUT);
        icon->info_icon = nid->hBalloonIcon;
        update_balloon( icon );
    }
    if (icon->state & NIS_HIDDEN) hide_icon( icon );
    else show_icon( icon );
    return TRUE;
}

/* Adds a new icon record to the list */
static BOOL add_icon(NOTIFYICONDATAW *nid)
{
    struct icon  *icon;

    TRACE( "id=0x%x, hwnd=%p\n", nid->uID, nid->hWnd );

    if ((icon = get_icon(nid->hWnd, nid->uID)))
    {
        WARN( "duplicate tray icon add, buggy app?\n" );
        return FALSE;
    }

    if (!(icon = calloc( 1, sizeof(*icon) )))
    {
        ERR( "out of memory\n" );
        return FALSE;
    }

    ZeroMemory(icon, sizeof(struct icon));
    icon->id     = nid->uID;
    icon->owner  = nid->hWnd;
    icon->display = ICON_DISPLAY_HIDDEN;

    CreateWindowW( tray_icon_class.lpszClassName, NULL, WS_CHILD,
                   0, 0, icon_cx, icon_cy, tray_window, NULL, NULL, icon );
    if (!icon->window) ERR( "Failed to create systray icon window\n" );

    list_add_tail(&icon_list, &icon->entry);

    return modify_icon( icon, nid );
}

/* Deletes tray icon window and icon record */
static BOOL delete_icon( struct icon *icon )
{
    hide_icon( icon );
    list_remove( &icon->entry );
    DestroyWindow( icon->tooltip );
    DestroyWindow( icon->window );
    DestroyIcon( icon->image );
    free( icon );
    return TRUE;
}

/* cleanup icons belonging to a window that has been destroyed */
static void cleanup_systray_window( HWND hwnd )
{
    struct icon *icon, *next;

    LIST_FOR_EACH_ENTRY_SAFE( icon, next, &icon_list, struct icon, entry )
        if (icon->owner == hwnd) delete_icon( icon );

    if (wine_notify_icon)
    {
        NOTIFYICONDATAW nid = { sizeof(nid), hwnd };
        wine_notify_icon( 0xdead, &nid );
    }
}

/* update the taskbar buttons when something changed */
static void sync_taskbar_buttons(void)
{
    struct taskbar_button *win;
    int pos = 0, count = 0;
    int width = taskbar_button_width;
    int right = tray_width - nb_displayed * icon_cx;
    HWND foreground = GetAncestor( GetForegroundWindow(), GA_ROOTOWNER );

    if (!IsWindowVisible( tray_window )) return;

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!win->hwnd)  /* start button */
        {
            SetWindowPos( win->button, 0, pos, 0, start_button_width, tray_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            pos += start_button_width;
            continue;
        }
        win->active = (win->hwnd == foreground);
        win->visible = IsWindowVisible( win->hwnd ) && !GetWindow( win->hwnd, GW_OWNER );
        if (win->visible) count++;
    }

    /* shrink buttons if space is tight */
    if (count && (count * width > right - pos))
        width = max( taskbar_button_width / 4, (right - pos) / count );

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!win->hwnd) continue;  /* start button */
        if (win->visible && right - pos >= width)
        {
            SetWindowPos( win->button, 0, pos, 0, width, tray_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            InvalidateRect( win->button, NULL, TRUE );
            pos += width;
        }
        else SetWindowPos( win->button, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW );
    }
}

static BOOL handle_incoming(HWND hwndSource, COPYDATASTRUCT *cds)
{
    struct icon *icon = NULL;
    const struct notify_data *data;
    NOTIFYICONDATAW nid;
    int ret = FALSE;

    if (cds->cbData < sizeof(*data)) return FALSE;
    data = cds->lpData;

    nid.cbSize           = sizeof(nid);
    nid.hWnd             = LongToHandle( data->hWnd );
    nid.uID              = data->uID;
    nid.uFlags           = data->uFlags;
    nid.uCallbackMessage = data->uCallbackMessage;
    nid.hIcon            = 0;
    nid.dwState          = data->dwState;
    nid.dwStateMask      = data->dwStateMask;
    nid.uTimeout         = data->u.uTimeout;
    nid.dwInfoFlags      = data->dwInfoFlags;
    nid.guidItem         = data->guidItem;
    lstrcpyW( nid.szTip, data->szTip );
    lstrcpyW( nid.szInfo, data->szInfo );
    lstrcpyW( nid.szInfoTitle, data->szInfoTitle );
    nid.hBalloonIcon     = 0;

    /* FIXME: if statement only needed because we don't support interprocess
     * icon handles */
    if ((nid.uFlags & NIF_ICON) && cds->cbData > sizeof(*data))
    {
        LONG cbMaskBits;
        LONG cbColourBits;
        const char *buffer = (const char *)(data + 1);

        cbMaskBits = (data->width * data->height + 15) / 16 * 2;
        cbColourBits = (data->planes * data->width * data->height * data->bpp + 15) / 16 * 2;

        if (cds->cbData < sizeof(*data) + cbMaskBits + cbColourBits)
        {
            ERR( "buffer underflow\n" );
            return FALSE;
        }
        nid.hIcon = CreateIcon(NULL, data->width, data->height, data->planes, data->bpp,
                               buffer, buffer + cbMaskBits);
    }

    /* try forwarding to the display driver first */
    if (cds->dwData == NIM_ADD || !(icon = get_icon( nid.hWnd, nid.uID )))
    {
        if (wine_notify_icon && ((ret = wine_notify_icon( cds->dwData, &nid )) != -1))
        {
            if (nid.hIcon) DestroyIcon( nid.hIcon );
            return ret;
        }
        ret = FALSE;
    }

    switch (cds->dwData)
    {
    case NIM_ADD:
        ret = add_icon(&nid);
        break;
    case NIM_DELETE:
        if (icon) ret = delete_icon( icon );
        break;
    case NIM_MODIFY:
        if (icon) ret = modify_icon( icon, &nid );
        break;
    case NIM_SETVERSION:
        if (icon)
        {
            icon->version = nid.uVersion;
            ret = TRUE;
        }
        break;
    default:
        FIXME( "unhandled tray message: %Id\n", cds->dwData );
        break;
    }

    if (nid.hIcon) DestroyIcon( nid.hIcon );
    sync_taskbar_buttons();
    return ret;
}

static void add_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win;

    if (hide_systray) return;

    /* ignore our own windows */
    if (hwnd)
    {
        DWORD process;
        if (!GetWindowThreadProcessId( hwnd, &process ) || process == GetCurrentProcessId()) return;
    }

    if (!(win = malloc( sizeof(*win) ))) return;
    win->hwnd = hwnd;
    win->button = CreateWindowW( WC_BUTTONW, NULL, WS_CHILD | BS_OWNERDRAW,
                                 0, 0, 0, 0, tray_window, (HMENU)hwnd, 0, 0 );
    list_add_tail( &taskbar_buttons, &win->entry );
}

static struct taskbar_button *find_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win;

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
        if (win->hwnd == hwnd) return win;

    return NULL;
}

static void remove_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win = find_taskbar_button( hwnd );

    if (!win) return;
    list_remove( &win->entry );
    DestroyWindow( win->button );
    free( win );
}

static void paint_taskbar_button( const DRAWITEMSTRUCT *dis )
{
    RECT rect;
    UINT flags = DC_TEXT;
    struct taskbar_button *win = find_taskbar_button( LongToHandle( dis->CtlID ));

    if (!win) return;
    GetClientRect( dis->hwndItem, &rect );
    DrawFrameControl( dis->hDC, &rect, DFC_BUTTON, DFCS_BUTTONPUSH | DFCS_ADJUSTRECT |
                      ((dis->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0 ));
    if (win->hwnd)
    {
        flags |= win->active ? DC_ACTIVE : DC_INBUTTON;
        DrawCaptionTempW( win->hwnd, dis->hDC, &rect, 0, 0, NULL, flags );
    }
    else  /* start button */
        DrawCaptionTempW( 0, dis->hDC, &rect, 0, 0, start_label, flags | DC_INBUTTON | DC_ICON );
}

static void click_taskbar_button( HWND button )
{
    LONG_PTR id = GetWindowLongPtrW( button, GWLP_ID );
    HWND hwnd = (HWND)id;

    if (!hwnd)  /* start button */
    {
        do_startmenu( tray_window );
        return;
    }

    if (IsIconic( hwnd ))
    {
        SendMessageW( hwnd, WM_SYSCOMMAND, SC_RESTORE, 0 );
        return;
    }

    if (IsWindowEnabled( hwnd ))
    {
        if (hwnd == GetForegroundWindow())
        {
            SendMessageW( hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0 );
            return;
        }
    }
    else  /* look for an enabled window owned by this one */
    {
        HWND owned = GetWindow( GetDesktopWindow(), GW_CHILD );
        while (owned && owned != hwnd)
        {
            if (IsWindowVisible( owned ) &&
                IsWindowEnabled( owned ) &&
                (GetWindow( owned, GW_OWNER ) == hwnd))
                break;
            owned = GetWindow( owned, GW_HWNDNEXT );
        }
        hwnd = owned;
    }
    SetForegroundWindow( hwnd );
}

static void show_taskbar_contextmenu( HWND button, LPARAM lparam )
{
    ULONG_PTR id = GetWindowLongPtrW( button, GWLP_ID );

    if (id) SendNotifyMessageW( (HWND)id, WM_POPUPSYSTEMMENU, 0, lparam );
}

static void do_hide_systray(void)
{
    ShowWindow( tray_window, SW_HIDE );
}

static void do_show_systray(void)
{
    SIZE size;
    NONCLIENTMETRICSW ncm;
    HFONT font;
    HDC hdc = GetDC( 0 );

    ncm.cbSize = sizeof(NONCLIENTMETRICSW);
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0 );
    font = CreateFontIndirectW( &ncm.lfCaptionFont );
    /* FIXME: Implement BCM_GETIDEALSIZE and use that instead. */
    SelectObject( hdc, font );
    GetTextExtentPointA( hdc, "abcdefghijklmnopqrstuvwxyz", 26, &size );
    taskbar_button_width = size.cx;
    GetTextExtentPointW( hdc, start_label, lstrlenW(start_label), &size );
    /* add some margins (FIXME) */
    size.cx += 12 + GetSystemMetrics( SM_CXSMICON );
    size.cy += 4;
    ReleaseDC( 0, hdc );
    DeleteObject( font );

    tray_width = GetSystemMetrics( SM_CXSCREEN );
    tray_height = max( icon_cy, size.cy );
    start_button_width = size.cx;
    SetWindowPos( tray_window, 0, 0, GetSystemMetrics( SM_CYSCREEN ) - tray_height,
                  tray_width, tray_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
    sync_taskbar_buttons();
}

static LRESULT WINAPI shell_traywnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_COPYDATA:
        return handle_incoming((HWND)wparam, (COPYDATASTRUCT *)lparam);

    case WM_DISPLAYCHANGE:
        if (hide_systray || (!nb_displayed && !enable_shell)) do_hide_systray();
        else do_show_systray();
        break;

    case WM_MOVE:
        update_systray_balloon_position();
        break;

    case WM_CLOSE:
        /* don't destroy the tray window, just hide it */
        ShowWindow( hwnd, SW_HIDE );
        return 0;

    case WM_DRAWITEM:
        paint_taskbar_button( (const DRAWITEMSTRUCT *)lparam );
        break;

    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) click_taskbar_button( (HWND)lparam );
        break;

    case WM_CONTEXTMENU:
        show_taskbar_contextmenu( (HWND)wparam, lparam );
        break;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_INITMENUPOPUP:
    case WM_MENUCOMMAND:
        return menu_wndproc(hwnd, msg, wparam, lparam);

    default:
        return DefWindowProcW( hwnd, msg, wparam, lparam );
    }
    return 0;
}

/* notification posted to the desktop window */
void handle_parent_notify( HWND hwnd, WPARAM wp )
{
    switch (LOWORD(wp))
    {
    case WM_CREATE:
        add_taskbar_button( hwnd );
        break;
    case WM_DESTROY:
        remove_taskbar_button( hwnd );
        cleanup_systray_window( hwnd );
        break;
    }
    sync_taskbar_buttons();
}

/* this function creates the listener window */
void initialize_systray( HMODULE graphics_driver, BOOL using_root, BOOL arg_enable_shell )
{
    RECT work_rect, primary_rect, taskbar_rect;

    if (using_root && graphics_driver) wine_notify_icon = (void *)GetProcAddress( graphics_driver, "wine_notify_icon" );

    shell_traywnd_class.hIcon = LoadIconW( 0, (const WCHAR *)IDI_WINLOGO );
    shell_traywnd_class.hCursor = LoadCursorW( 0, (const WCHAR *)IDC_ARROW );
    tray_icon_class.hIcon = shell_traywnd_class.hIcon;
    tray_icon_class.hCursor = shell_traywnd_class.hCursor;

    icon_cx = GetSystemMetrics( SM_CXSMICON ) + 2*ICON_BORDER;
    icon_cy = GetSystemMetrics( SM_CYSMICON ) + 2*ICON_BORDER;
    hide_systray = using_root;
    enable_shell = arg_enable_shell;

    /* register the systray listener window class */
    if (!RegisterClassExW( &shell_traywnd_class ))
    {
        ERR( "Could not register SysTray window class\n" );
        return;
    }
    if (!wine_notify_icon && !RegisterClassExW( &tray_icon_class ))
    {
        ERR( "Could not register Wine SysTray window classes\n" );
        return;
    }

    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work_rect, 0 );
    SetRect( &primary_rect, 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
    SubtractRect( &taskbar_rect, &primary_rect, &work_rect );

    tray_window = CreateWindowExW( WS_EX_NOACTIVATE, shell_traywnd_class.lpszClassName, NULL, WS_POPUP, taskbar_rect.left,
                                   taskbar_rect.top, taskbar_rect.right - taskbar_rect.left,
                                   taskbar_rect.bottom - taskbar_rect.top, 0, 0, 0, 0 );
    if (!tray_window)
    {
        ERR( "Could not create tray window\n" );
        return;
    }

    LoadStringW( NULL, IDS_START_LABEL, start_label, ARRAY_SIZE( start_label ));

    add_taskbar_button( 0 );

    if (hide_systray) do_hide_systray();
    else if (enable_shell) do_show_systray();
}
