/*
 * nt loader
 *
 * Copyright 2006-2008 Mike McCormack
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


#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include <new>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "ntcall.h"
#include "section.h"
#include "objdir.h"
#include "ntwin32.h"
#include "win32mgr.h"
#include "mem.h"
#include "debug.h"
#include "object.inl"
#include "alloc_bitmap.h"
#include "queue.h"
#include "message.h"
#include "win.h"

wndcls_list_tt wndcls_list;
window_tt *active_window;
window_tt *desktop_window;

ULONG NTAPI NtUserGetThreadState( ULONG InfoClass )
{
	switch (InfoClass)
	{
	case 0: // GetFocus
	case 1: // GetActiveWindow
	case 2: // GetCapture
	case 5: // GetInputState
	case 6: // GetCursor
	case 8: // used in PeekMessageW
	case 9: // GetMessageExtraInfo
	case 0x0a: // used in InSendMessageEx
	case 0x0b: // GetMessageTime
	case 0x0c: // ?
		return 0;
	case 0x10: // ?
		return 0;
	case 0x11: // sets TEB->Win32ThreadInfo for the current thread
		return 1;
	default:
		dprintf("%ld\n", InfoClass );
	}
	return 0;
}

// see http://winterdom.com/dev/ui/wnd.html

#define USER_HANDLE_WINDOW 1

struct user_handle_entry_t {
	union {
		void *object;
		USHORT next_free;
	};
	void *owner;
	USHORT type;
	USHORT highpart;
};

struct user_shared_mem_t {
	ULONG x1;
	ULONG x2;
	ULONG max_window_handle;
};

static const ULONG user_shared_mem_size = 0x20000;
static const ULONG user_shared_mem_reserve = 0x10000;

// section for user handle table
static section_t *user_handle_table_section = 0;

// kernel address for user handle table (shared)
static user_handle_entry_t *user_handle_table;

// section for user shared memory
static section_t *user_shared_section = 0;

// kernel address for memory shared with the user process
static user_shared_mem_t *user_shared;

// bitmap of free memory
allocation_bitmap_t user_shared_bitmap;

MESSAGE_MAP_SHARED_MEMORY message_maps[NUMBER_OF_MESSAGE_MAPS];

static USHORT next_user_handle = 1;

#define MAX_USER_HANDLES 0x200

void check_max_window_handle( ULONG n )
{
	n++;
	if (user_shared->max_window_handle<n)
		user_shared->max_window_handle = n;
	dprintf("max_window_handle = %04lx\n", user_shared->max_window_handle);
}

void init_user_handle_table()
{
	USHORT i;
	next_user_handle = 1;
	for ( i=next_user_handle; i<(MAX_USER_HANDLES-1); i++ )
	{
		user_handle_table[i].object = (void*) (i+1);
		user_handle_table[i].owner = 0;
		user_handle_table[i].type = 0;
		user_handle_table[i].highpart = 1;
	}
}

ULONG alloc_user_handle( void* obj, ULONG type, process_t *owner )
{
	assert( type != 0 );
	ULONG ret = next_user_handle;
	ULONG next = user_handle_table[ret].next_free;
	assert( next != ret );
	assert( user_handle_table[ret].type == 0 );
	assert( user_handle_table[ret].owner == 0 );
	assert( next <= MAX_USER_HANDLES );
	user_handle_table[ret].object = obj;
	user_handle_table[ret].type = type;
	user_handle_table[ret].owner = (void*) owner;
	next_user_handle = next;
	check_max_window_handle( ret );
	return (user_handle_table[ret].highpart << 16) | ret;
}

void free_user_handle( HANDLE handle )
{
	UINT n = (UINT) handle;
	USHORT lowpart = n&0xffff;

	dprintf("freeing handle %08x\n", n);
	user_handle_table[lowpart].type = 0;
	user_handle_table[lowpart].owner = 0;
	user_handle_table[lowpart].object = 0;

	// update the free handle list
	user_handle_table[lowpart].next_free = next_user_handle;
	next_user_handle = lowpart;

	// FIXME: maybe decrease max_window_handle?
}

void delete_user_object( ULONG i )
{
	user_handle_entry_t *entry = user_handle_table+i;
	dprintf("deleting user handle %ld\n", i);
	assert(entry->object != NULL);
	switch (entry->type)
	{
	case USER_HANDLE_WINDOW:
		delete (window_tt*) entry->object;
		break;
	default:
		dprintf("object %ld (%p), type = %08x owner = %p\n",
			i, entry->object, entry->type, entry->owner);
		assert(0);
	}
}

void free_user32_handles( process_t *p )
{
	ULONG i;
	assert( p != NULL );
	if (!user_handle_table)
		return;
	for (i=0; i<user_shared->max_window_handle; i++)
	{
		if (p == (process_t*) user_handle_table[i].owner)
			delete_user_object( i );
	}
}

void* user_obj_from_handle( HANDLE handle, ULONG type )
{
	UINT n = (UINT) handle;
	USHORT lowpart = n&0xffff;
	//USHORT highpart = (n>>16);

	if (lowpart == 0 || lowpart > user_shared->max_window_handle)
		return NULL;
	if (type != user_handle_table[lowpart].type)
		return NULL;
	//FIXME: check high part and type
	//if (user_handle_table[].highpart != highpart)
	return user_handle_table[lowpart].object;
}

window_tt *window_from_handle( HANDLE handle )
{
	void *obj = user_obj_from_handle( handle, 1 );
	if (!obj)
		return NULL;
	return (window_tt*) obj;
}

void *init_user_shared_memory()
{
	// read/write for the kernel and read only for processes
	if (!user_shared)
	{
		LARGE_INTEGER sz;
		NTSTATUS r;

		sz.QuadPart = sizeof (user_handle_entry_t) * MAX_USER_HANDLES;
		r = create_section( &user_handle_table_section, NULL, &sz, SEC_COMMIT, PAGE_READWRITE );
		if (r < STATUS_SUCCESS)
			return 0;

		user_handle_table = (user_handle_entry_t*) user_handle_table_section->get_kernel_address();

		init_user_handle_table();

		sz.QuadPart = user_shared_mem_size;
		r = create_section( &user_shared_section, NULL, &sz, SEC_COMMIT, PAGE_READWRITE );
		if (r < STATUS_SUCCESS)
			return 0;

		user_shared = (user_shared_mem_t*) user_shared_section->get_kernel_address();

		// setup the allocation bitmap for user objects (eg. windows)
		void *object_area = (void*) ((BYTE*) user_shared + user_shared_mem_reserve);
		user_shared_bitmap.set_area( object_area,
			user_shared_mem_size - user_shared_mem_reserve );

		// create the window stations directory too
		create_directory_object( (PWSTR) L"\\Windows\\WindowStations" );
	}

	dprintf("user_handle_table at %p\n", user_handle_table );
	dprintf("user_shared at %p\n", user_shared );

	return user_shared;
}

class ntusershm_tracer : public block_tracer
{
public:
	virtual void on_access( mblock *mb, BYTE *address, ULONG eip );
	virtual bool enabled() const;
};

bool ntusershm_tracer::enabled() const
{
	return trace_is_enabled( "usershm" );
}

bool message_map_on_access( BYTE *address, ULONG eip )
{
	for (ULONG i=0; i<NUMBER_OF_MESSAGE_MAPS; i++)
	{
		if (!message_maps[i].Bitmap)
			continue;
		if (address < message_maps[i].Bitmap)
			continue;
		ULONG ofs = address - message_maps[i].Bitmap;
		if (ofs > message_maps[i].MaxMessage/8)
			continue;
		fprintf(stderr, "%04lx: accessed message map[%ld][%04lx] from %08lx\n",
			current->trace_id(), i, ofs, eip);
		return true;
	}
	return false;
}

bool window_on_access( BYTE *address, ULONG eip )
{
	for (ULONG i=0; i<user_shared->max_window_handle; i++)
	{
		switch (user_handle_table[i].type)
		{
		case USER_HANDLE_WINDOW:
			{
			// window shared memory structures are variable size
			// have the window check itself
			window_tt* wnd = reinterpret_cast<window_tt*>( user_handle_table[i].object);
			if (wnd->on_access( address, eip ))
				return true;
			}
		}
	}
	return false;
}

void ntusershm_tracer::on_access( mblock *mb, BYTE *address, ULONG eip )
{
	ULONG ofs = address - mb->get_base_address();
	if (ofs < user_shared_mem_reserve)
	{
		const char *name = "";
		switch (ofs)
		{
		case 8:
			name = " (max_window_handle)";
			break;
		}
		fprintf(stderr, "%04lx: accessed ushm[%04lx]%s from %08lx\n",
			current->trace_id(), ofs, name, eip);
		return;
	}

	if (message_map_on_access( address, eip ))
		return;

	if (window_on_access( address, eip ))
		return;

	fprintf(stderr, "%04lx: accessed ushm[%04lx] from %08lx\n",
		current->trace_id(), ofs, eip);
}

static ntusershm_tracer ntusershm_trace;

class ntuserhandle_tracer : public block_tracer
{
public:
	virtual void on_access( mblock *mb, BYTE *address, ULONG eip );
	virtual bool enabled() const;
};

bool ntuserhandle_tracer::enabled() const
{
	return trace_is_enabled( "usershm" );
}

void ntuserhandle_tracer::on_access( mblock *mb, BYTE *address, ULONG eip )
{
	ULONG ofs = address - mb->get_base_address();
	const int sz = sizeof (user_handle_entry_t);
	ULONG number = ofs/sz;
	const char *field = "unknown";
	switch (ofs % sz)
	{
#define f(n, x) case n: field = #x; break;
	f( 0, owner )
	f( 4, object )
	f( 8, type )
	f( 10, highpart )
#undef f
	default:
		field = "unknown";
	}

	fprintf(stderr, "%04lx: accessed user handle[%04lx]+%s (%ld) from %08lx\n",
		current->trace_id(), number, field, ofs%sz, eip);
}
static ntuserhandle_tracer ntuserhandle_trace;

BYTE* alloc_message_bitmap( process_t* proc, MESSAGE_MAP_SHARED_MEMORY& map, ULONG last_message )
{
	ULONG sz = (last_message+7)/8;
	BYTE *msg_map = user_shared_bitmap.alloc( sz );
	memset( msg_map, 0, sz );
	ULONG ofs = (BYTE*)msg_map - (BYTE*)user_shared;
	map.Bitmap = (BYTE*) (proc->win32k_info->user_shared_mem + ofs);
	map.MaxMessage = last_message;
	dprintf("bitmap = %p last = %ld\n", map.Bitmap, map.MaxMessage);
	return msg_map;
}

NTUSERINFO *alloc_user_info()
{
	NTUSERINFO *info = (NTUSERINFO*) user_shared_bitmap.alloc( sizeof (NTUSERINFO) );
	info->DesktopWindow = desktop_window;
	ULONG ofs = (BYTE*)info - (BYTE*)user_shared;
	return (NTUSERINFO*) (current->process->win32k_info->user_shared_mem + ofs);
}

void create_desktop_window()
{
	if (desktop_window)
		return;

	desktop_window = new window_tt;
	if (!desktop_window)
		return;

	memset( desktop_window, 0, sizeof (window_tt) );
	desktop_window->rcWnd.left = 0;
	desktop_window->rcWnd.top = 0;
	desktop_window->rcWnd.right = 640;
	desktop_window->rcWnd.bottom = 480;
	desktop_window->rcClient = desktop_window->rcWnd;

	desktop_window->handle = (HWND) alloc_user_handle( desktop_window, USER_HANDLE_WINDOW, current->process );
}

// should be called from NtGdiInit to map the user32 shared memory
NTSTATUS map_user_shared_memory( process_t *proc )
{
	NTSTATUS r;

	assert( proc->win32k_info );
	BYTE*& user_shared_mem = proc->win32k_info->user_shared_mem;
	BYTE*& user_handles = proc->win32k_info->user_handles;

	// map the user shared memory block into the process's memory
	if (!init_user_shared_memory())
		return STATUS_UNSUCCESSFUL;

	// already mapped into this process?
	if (user_shared_mem)
		return STATUS_SUCCESS;

	r = user_shared_section->mapit( proc->vm, user_shared_mem, 0,
					MEM_COMMIT, PAGE_READONLY );
	if (r < STATUS_SUCCESS)
		return STATUS_UNSUCCESSFUL;

	if (option_trace)
	{
		proc->vm->set_tracer( user_shared_mem, ntusershm_trace );
		proc->vm->set_tracer( user_handles, ntuserhandle_trace );
	}

	// map the shared handle table
	r = user_handle_table_section->mapit( proc->vm, user_handles, 0,
					MEM_COMMIT, PAGE_READONLY );
	if (r < STATUS_SUCCESS)
		return STATUS_UNSUCCESSFUL;

	dprintf("user shared at %p\n", user_shared_mem);

	return STATUS_SUCCESS;
}

BOOLEAN do_gdi_init()
{
	NTSTATUS r;
	r = map_user_shared_memory( current->process );
	if (r < STATUS_SUCCESS)
		return FALSE;

	// check set the offset
	BYTE*& user_shared_mem = current->process->win32k_info->user_shared_mem;
	current->get_teb()->KernelUserPointerOffset = (BYTE*) user_shared - user_shared_mem;

	// create the desktop window for alloc_user_info
	create_desktop_window();
	current->get_teb()->NtUserInfo = alloc_user_info();

	return TRUE;
}

NTSTATUS NTAPI NtUserProcessConnect(HANDLE Process, PVOID Buffer, ULONG BufferSize)
{
	union {
		USER_PROCESS_CONNECT_INFO win2k;
		USER_PROCESS_CONNECT_INFO_XP winxp;
	} info;
	const ULONG version = 0x50000;
	NTSTATUS r;

	process_t *proc = 0;
	r = object_from_handle( proc, Process, 0 );
	if (r < STATUS_SUCCESS)
		return r;

	if (BufferSize != sizeof info.winxp && BufferSize != sizeof info.win2k)
	{
		dprintf("buffer size wrong %ld (not WinXP or Win2K?)\n", BufferSize);
		return STATUS_UNSUCCESSFUL;
	}

	r = copy_from_user( &info, Buffer, BufferSize );
	if (r < STATUS_SUCCESS)
		return STATUS_UNSUCCESSFUL;

	if (info.winxp.Version != version)
	{
		dprintf("version wrong %08lx %08lx\n", info.winxp.Version, version);
		return STATUS_UNSUCCESSFUL;
	}


	// check if we're already connected
	r = win32k_process_init( proc );
	if (r < STATUS_SUCCESS)
		return r;

	r = map_user_shared_memory( proc );
	if (r < STATUS_SUCCESS)
		return r;

	info.win2k.Ptr[0] = (void*)proc->win32k_info->user_shared_mem;
	info.win2k.Ptr[1] = (void*)proc->win32k_info->user_handles;
	info.win2k.Ptr[2] = (void*)0xbee30000;
	info.win2k.Ptr[3] = (void*)0xbee40000;

	for (ULONG i=0; i<NUMBER_OF_MESSAGE_MAPS; i++ )
	{
		info.win2k.MessageMap[i].MaxMessage = 0;
		info.win2k.MessageMap[i].Bitmap = (BYTE*)i;
	}

	alloc_message_bitmap( proc, info.win2k.MessageMap[0x1b], 0x400 );
	message_maps[0x1b] = info.win2k.MessageMap[0x1b];
	alloc_message_bitmap( proc, info.win2k.MessageMap[0x1c], 0x400 );
	message_maps[0x1c] = info.win2k.MessageMap[0x1c];

	r = copy_to_user( Buffer, &info, BufferSize );
	if (r < STATUS_SUCCESS)
		return STATUS_UNSUCCESSFUL;

	return STATUS_SUCCESS;
}

PVOID g_funcs[9];
PVOID g_funcsW[20];
PVOID g_funcsA[20];

// Funcs array has 9 function pointers
// FuncsW array has 20 function pointers
// FuncsA array has 20 function pointers
// Base is the Base address of the DLL containing the functions
BOOLEAN NTAPI NtUserInitializeClientPfnArrays(
	PVOID Funcs,
	PVOID FuncsW,
	PVOID FuncsA,
	PVOID Base)
{
	NTSTATUS r;

	r = copy_from_user( &g_funcs, Funcs, sizeof g_funcs );
	if (r < 0)
		return r;
	r = copy_from_user( &g_funcsW, FuncsW, sizeof g_funcsW );
	if (r < 0)
		return r;
	r = copy_from_user( &g_funcsA, FuncsA, sizeof g_funcsA );
	if (r < 0)
		return r;
	return 0;
}

BOOLEAN NTAPI NtUserInitialize(ULONG u_arg1, ULONG u_arg2, ULONG u_arg3)
{
	return TRUE;
}

ULONG NTAPI NtUserCallNoParam(ULONG Index)
{
	switch (Index)
	{
	case 0:
		return 0; // CreateMenu
	case 1:
		return 0; // CreatePopupMenu
	case 2:
		return 0; // DestroyCaret
	case 3:
		return 0; // ?
	case 4:
		return 0; // GetInputDesktop
	case 5:
		return 0; // GetMessagePos
	case 6:
		return 0; // ?
	case 7:
		return 0xfeed0007;
	case 8:
		return 0; // ReleaseCapture
	case 0x0a:
		return 0; // EndDialog?
	case 0x12:
		return 0; // ClientThreadSetup?
	case 0x15:
		return 0; // MsgWaitForMultipleObjects
	default:
		return FALSE;
	}
}

BOOLEAN NtReleaseDC( HANDLE hdc )
{
	dprintf("%p\n", hdc );
	return win32k_manager->release_dc( hdc );
}

BOOLEAN NtPostQuitMessage( ULONG ret )
{
	dprintf("%08lx\n", ret );
	if (current->queue)
		current->queue->post_quit_message( ret );
	return TRUE;
}

PVOID NtGetWindowPointer( HWND window )
{
	dprintf("%p\n", window );
	window_tt *win = window_from_handle( window );
	if (!win)
		return 0;
	return win->get_wininfo();
}

ULONG NTAPI NtUserCallOneParam(ULONG Param, ULONG Index)
{
	switch (Index)
	{
	case 0x16: // BeginDeferWindowPos
		return TRUE;
	case 0x17: // WindowFromDC
		return TRUE;
	case 0x18: // AllowSetForegroundWindow
		return TRUE;
	case 0x19: // used by CreateIconIndirect
		return TRUE;
	case 0x1a: // used by DdeUnitialize
		return TRUE;
	case 0x1b: // used by MsgWaitForMultipleObjectsEx
		return TRUE;
	case 0x1c: // EnumClipboardFormats
		return TRUE;
	case 0x1d: // used by MsgWaitForMultipleObjectsEx
		return TRUE;
	case 0x1e: // GetKeyboardLayout
		return TRUE;
	case 0x1f: // GetKeyboardType
		return TRUE;
	case 0x20: // GetQueueStatus
		return TRUE;
	case 0x21: // SetLockForegroundWindow
		return TRUE;
	case 0x22: // LoadLocalFonts, used by LoadRemoteFonts
		return TRUE;
	case NTUCOP_GETWNDPTR: // get the window pointer
		return (ULONG) NtGetWindowPointer( (HWND) Param );
	case 0x24: // MessageBeep
		return TRUE;
	case 0x25: // used by SoftModalMessageBox
		return TRUE;
	case NTUCOP_POSTQUITMESSAGE:
		return NtPostQuitMessage( Param );
	case 0x27: // RealizeUserPalette
		return TRUE;
	case 0x28: // used by ClientThreadSetup
		return TRUE;
	case NTUCOP_RELEASEDC: // used by ReleaseDC + DeleteDC (deref DC?)
		return NtReleaseDC( (HANDLE) Param );
	case 0x2a: // ReplyMessage
		return TRUE;
	case 0x2b: // SetCaretBlinkTime
		return TRUE;
	case 0x2c: // SetDoubleClickTime
		return TRUE;
	case 0x2d: // ShowCursor
		return TRUE;
	case 0x2e: // StartShowGlass
		return TRUE;
	case 0x2f: // SwapMouseButton
		return TRUE;
	case 0x30: // SetMessageExtraInfo
		return TRUE;
	case 0x31: // used by UserRegisterWowHandlers
		return TRUE;
	case 0x33: // GetProcessDefaultLayout
		return TRUE;
	case 0x34: // SetProcessDefaultLayout
		return TRUE;
	case 0x37: // GetWinStationInfo
		return TRUE;
	case 0x38: // ?
		return TRUE;
	default:
		return FALSE;
	}
}

// should be PASCAL calling convention?
ULONG NTAPI NtUserCallTwoParam(ULONG Param2, ULONG Param1, ULONG Index)
{
	switch (Index)
	{
	case 0x53:  // EnableWindow
		dprintf("EnableWindow (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x55:  // ShowOwnedPopups
		dprintf("ShowOwnedPopups (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x56:  // SwitchToThisWindow
		dprintf("SwitchToThisWindow (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x57:  // ValidateRgn
		dprintf("ValidateRgn (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x59: // GetMonitorInfo
		dprintf("GetMonitorInfo (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x5b:  // RegisterLogonProcess
		dprintf("RegisterLogonProcess (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x5c:  // RegisterSystemThread
		dprintf("RegisterSystemThread (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x5e:  // SetCaretPos
		dprintf("SetCaretPos (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x5f:  // SetCursorPos
		dprintf("SetCursorPos (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x60:  // UnhookWindowsHook
		dprintf("UnhookWindowsHook (%08lx, %08lx)\n", Param1, Param2);
		break;
	case 0x61:  // UserRegisterWowHandlers
		dprintf("UserRegisterWowHandlers (%08lx, %08lx)\n", Param1, Param2);
		break;
	default:
		dprintf("%lu (%08lx, %08lx)\n", Index, Param1, Param2);
		break;
	}
	return TRUE;
}

// returns a handle to the thread's desktop
HANDLE NTAPI NtUserGetThreadDesktop(
	ULONG ThreadId,
	ULONG u_arg2)
{
	return (HANDLE) 0xde5;
}

HANDLE NTAPI NtUserFindExistingCursorIcon(PUNICODE_STRING Library, PUNICODE_STRING str2, PVOID p_arg3)
{
	ULONG index;

	unicode_string_t us;
	NTSTATUS r;

	r = us.copy_from_user( Library );
	if (r == STATUS_SUCCESS)
		dprintf("Library=\'%pus\'\n", &us);

	r = us.copy_from_user( str2 );
	if (r == STATUS_SUCCESS)
		dprintf("str2=\'%pus\'\n", &us);

	r = copy_from_user( &index, p_arg3, sizeof index );
	if (r == STATUS_SUCCESS)
		dprintf("index = %lu\n", index);

	return 0;
}

HANDLE NTAPI NtUserGetDC(HANDLE Window)
{
	if (!Window)
		return win32k_manager->alloc_screen_dc();

	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	return win->get_dc();
}

HGDIOBJ NtUserSelectPalette(HGDIOBJ hdc, HPALETTE palette, BOOLEAN force_bg)
{
	return alloc_gdi_object( FALSE, GDI_OBJECT_PALETTE );
}

BOOLEAN NTAPI NtUserSetCursorIconData(
	HANDLE Handle,
	PVOID Module,
	PUNICODE_STRING ResourceName,
	PICONINFO IconInfo)
{
	return TRUE;
}

BOOLEAN NTAPI NtUserGetIconInfo(
	HANDLE Icon,
	PICONINFO IconInfo,
	PUNICODE_STRING lpInstName,
	PUNICODE_STRING lpResName,
	LPDWORD pbpp,
	BOOL bInternal)
{
	return TRUE;
}

void* wndcls_tt::operator new(size_t sz)
{
	dprintf("allocating window\n");
	assert( sz == sizeof (wndcls_tt));
	return user_shared_bitmap.alloc( sz );
}

void wndcls_tt::operator delete(void *p)
{
	user_shared_bitmap.free( (unsigned char*) p, sizeof (wndcls_tt) );
}

wndcls_tt::wndcls_tt( NTWNDCLASSEX& ClassInfo, const UNICODE_STRING& ClassName, const UNICODE_STRING& MenuName, ATOM a ) :
	name( ClassName ),
	menu( MenuName ),
	info( ClassInfo ),
	refcount( 0 )
{
	memset( this, 0, sizeof (WNDCLASS) );
	atomWindowType = a;
	pSelf = this;
}

wndcls_tt* wndcls_tt::from_name( const UNICODE_STRING& wndcls_name )
{
	for (wndcls_iter_tt i(wndcls_list); i; i.next())
	{
		wndcls_tt *cls = i;
		if (cls->get_name().is_equal( wndcls_name ))
			return cls;
	}
	return NULL;
}

ATOM NTAPI NtUserRegisterClassExWOW(
	PNTWNDCLASSEX ClassInfo,
	PUNICODE_STRING ClassName,
	PNTCLASSMENUNAMES MenuNames,
	USHORT,
	ULONG Flags,
	ULONG)
{
	NTWNDCLASSEX clsinfo;

	NTSTATUS r;
	r = copy_from_user( &clsinfo, ClassInfo, sizeof clsinfo );
	if (r < STATUS_SUCCESS)
		return 0;

	if (clsinfo.Size != sizeof clsinfo)
		return 0;

	unicode_string_t clsstr;
	r = clsstr.copy_from_user( ClassName );
	if (r < STATUS_SUCCESS)
		return 0;

	// for some reason, a structure with three of the same name is passed...
	NTCLASSMENUNAMES menu_strings;
	r = copy_from_user( &menu_strings, MenuNames, sizeof menu_strings );
	if (r < STATUS_SUCCESS)
		return 0;

	unicode_string_t menuname;
	r = menuname.copy_from_user( menu_strings.name_us );
	if (r < STATUS_SUCCESS)
		return 0;

	dprintf("window class = %pus  menu = %pus\n", &clsstr, &menuname);

	static ATOM atom = 0xc001;
	wndcls_tt* cls = new wndcls_tt( clsinfo, clsstr, menuname, atom );
	if (!cls)
		return 0;

	wndcls_list.append( cls );

	return cls->get_atom();
}

NTSTATUS NTAPI NtUserSetInformationThread(
	HANDLE ThreadHandle,
	ULONG InfoClass,
	PVOID Buffer,
	ULONG BufferLength)
{
	dprintf("%p %08lx %p %08lx\n", ThreadHandle, InfoClass, Buffer, BufferLength);
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtUserGetKeyboardLayoutList(ULONG x1, ULONG x2)
{
	dprintf("%08lx, %08lx\n", x1, x2);
	return STATUS_SUCCESS;
}

static int g_hack_desktop = 0xf00d2001;

HANDLE NTAPI NtUserCreateWindowStation(
	POBJECT_ATTRIBUTES WindowStationName,
	ACCESS_MASK DesiredAccess,
	HANDLE ObjectDirectory,
	ULONG x1,
	PVOID x2,
	ULONG Locale)
{
	dprintf("%p %08lx %p %08lx %p %08lx\n",
		WindowStationName, DesiredAccess, ObjectDirectory, x1, x2, Locale);

	// print out the name
	OBJECT_ATTRIBUTES oa;

	NTSTATUS r;
	r = copy_from_user( &oa, WindowStationName, sizeof oa );
	if (r < STATUS_SUCCESS)
		return 0;

	unicode_string_t us;
	r = us.copy_from_user( oa.ObjectName );
	if (r < STATUS_SUCCESS)
		return 0;

	dprintf("name = %pus\n", &us );

	return (HANDLE) g_hack_desktop++;
}

HANDLE NTAPI NtUserCreateDesktop(
	POBJECT_ATTRIBUTES DesktopName,
	ULONG x1,
	ULONG x2,
	ULONG x3,
	ACCESS_MASK DesiredAccess)
{
	// print out the name
	object_attributes_t oa;
	NTSTATUS r;
	r = oa.copy_from_user( DesktopName );
	if (r < STATUS_SUCCESS)
		return 0;

	dprintf("name = %pus\n", oa.ObjectName );
	dprintf("root = %p\n", oa.RootDirectory );

	return (HANDLE) g_hack_desktop++;
}

HANDLE NTAPI NtUserOpenDesktop(POBJECT_ATTRIBUTES DesktopName, ULONG, ACCESS_MASK DesiredAccess)
{
	object_attributes_t oa;
	NTSTATUS r;
	r = oa.copy_from_user( DesktopName );
	if (r < STATUS_SUCCESS)
		return 0;

	dprintf("name = %pus\n", oa.ObjectName );
	dprintf("root = %p\n", oa.RootDirectory );

	return (HANDLE) g_hack_desktop++;
}

BOOLEAN NTAPI NtUserSetProcessWindowStation(HANDLE WindowStation)
{
	dprintf("\n");
	current->process->window_station = WindowStation;
	return TRUE;
}

HANDLE NTAPI NtUserGetProcessWindowStation(void)
{
	dprintf("\n");
	return current->process->window_station;
}

BOOLEAN NTAPI NtUserSetThreadDesktop(HANDLE Desktop)
{
	dprintf("\n");
	return TRUE;
}

BOOLEAN NTAPI NtUserSetImeHotKey(ULONG x1, ULONG x2, ULONG x3, ULONG x4, ULONG x5)
{
	dprintf("\n");
	return TRUE;
}

BOOLEAN NTAPI NtUserLoadKeyboardLayoutEx(
	HANDLE File,
	ULONG x1,
	ULONG x2,
	PVOID x3,
	ULONG locale,
	ULONG flags)
{
	dprintf("\n");
	return TRUE;
}

BOOLEAN NTAPI NtUserUpdatePerUserSystemParameters(ULONG x1, ULONG x2)
{
	dprintf("\n");
	return TRUE;
}

BOOLEAN NTAPI NtUserSystemParametersInfo(ULONG x1, ULONG x2, ULONG x3, ULONG x4)
{
	dprintf("\n");
	return TRUE;
}

BOOLEAN NTAPI NtUserSetWindowStationUser(HANDLE WindowStation, PVOID, ULONG, ULONG)
{
	dprintf("\n");
	return TRUE;
}

ULONG NTAPI NtUserGetCaretBlinkTime(void)
{
	dprintf("\n");
	return 100;
}

ULONG message_no = 0xc001;

ULONG NTAPI NtUserRegisterWindowMessage(PUNICODE_STRING Message)
{
	dprintf("\n");
	unicode_string_t us;

	NTSTATUS r = us.copy_from_user( Message );
	if (r < STATUS_SUCCESS)
		return 0;

	dprintf("message = %pus -> %04lx\n", &us, message_no);

	return message_no++;
}

class user32_unicode_string_t : public unicode_string_t
{
public:
	NTSTATUS copy_from_user( PUSER32_UNICODE_STRING String );
};

NTSTATUS user32_unicode_string_t::copy_from_user( PUSER32_UNICODE_STRING String )
{
	USER32_UNICODE_STRING str;
	NTSTATUS r = ::copy_from_user( &str, String, sizeof str );
	if (r < STATUS_SUCCESS)
		return r;
	return copy_wstr_from_user( str.Buffer, str.Length );
}

window_tt::window_tt()
{
	memset( this, 0, sizeof *this );
}

void window_tt::link_window( window_tt* parent_win )
{
	assert( next == NULL );
	assert( parent == NULL );
	assert( parent_win != NULL );
	parent = parent_win;
	next = parent->first_child;
	parent->first_child = this;
	assert( next != this );
}

void window_tt::unlink_window()
{
	// special behaviour for desktop window
	// should replace window_tt::first with desktop...
	if (this == desktop_window)
	{
		desktop_window = NULL;
		return;
	}
	WND **p;
	assert (parent != NULL);
	p = &parent->first_child;

	while (*p != this)
		p = &((*p)->next);
	assert (*p);
	*p = next;
	next = NULL;
}

void* window_tt::operator new(size_t sz)
{
	dprintf("allocating window\n");
	assert( sz == sizeof (window_tt));
	return user_shared_bitmap.alloc( sz );
}

void window_tt::operator delete(void *p)
{
	user_shared_bitmap.free( (unsigned char*) p, sizeof (window_tt) );
}

// return true if address is in this window's shared memory
bool window_tt::on_access( BYTE *address, ULONG eip )
{
	BYTE *user_ptr = (BYTE*) get_wininfo();
	if (user_ptr > address)
		return false;

	ULONG ofs = address - user_ptr;
	ULONG sz = sizeof (WND) /*+ cbWndClsExtra + cbWndExtra */;
	if (ofs > sz)
		return false;
	const char* field = "";
	switch (ofs)
	{
#define f(n, x) case n: field = #x; break;
	f( 0, handle )
	f( 0x10, self )
	f( 0x14, dwFlags )
	f( 0x16, dwFlags )
	f( 0x18, exstyle )
	f( 0x1c, style )
	f( 0x20, hInstance )
	f( 0x28, next )
	f( 0x2c, parent )
	f( 0x30, first_child )
	f( 0x34, owner )
	f( 0x5c, wndproc )
	f( 0x60, wndcls )
#undef f
	}
	fprintf(stderr, "%04lx: accessed window[%p][%04lx] %s from %08lx\n", current->trace_id(), handle, ofs, field, eip);
	return true;
}

window_tt::~window_tt()
{
        if (win_dc) delete win_dc;

	unlink_window();
	free_user_handle( handle );
	dprintf("active window = %p this = %p\n", active_window, this);
	if (active_window == this)
	{
		dprintf("cleared active window handle\n");
		active_window = 0;
	}
}

PWND window_tt::get_wininfo()
{
	ULONG ofs = (BYTE*)this - (BYTE*)user_shared;
	return (PWND) (current->process->win32k_info->user_shared_mem + ofs);
}

NTSTATUS window_tt::send( message_tt& msg )
{
	thread_t*& thread = get_win_thread();
	if (thread->is_terminated())
		return STATUS_THREAD_IS_TERMINATING;

	PTEB teb = thread->get_teb();
	teb->CachedWindowHandle = handle;
	teb->CachedWindowPointer = get_wininfo();

	dprintf("sending %s\n", msg.description());

	msg.set_window_info( this );

	void *address = thread->push( msg.get_size() );

	NTSTATUS r = msg.copy_to_user( address );
	if (r >= STATUS_SUCCESS)
	{
		ULONG ret_len = 0;
		PVOID ret_buf = 0;

		r = thread->do_user_callback( msg.get_callback_num(), ret_len, ret_buf );
	}

	if (thread->is_terminated())
		return STATUS_THREAD_IS_TERMINATING;

	thread->pop( msg.get_size() );
	teb->CachedWindowHandle = 0;
	teb->CachedWindowPointer = 0;

	return r;
}

BOOLEAN window_tt::show( INT Show )
{
	// send a WM_SHOWWINDOW message
	showwindowmsg_tt sw( TRUE );
	send( sw );

	return TRUE;
}

HANDLE NTAPI NtUserCreateWindowEx(
	ULONG ExStyle,
	PUSER32_UNICODE_STRING ClassName,
	PUSER32_UNICODE_STRING WindowName,
	ULONG Style,
	LONG x,
	LONG y,
	LONG Width,
	LONG Height,
	HANDLE Parent,
	HANDLE Menu,
	PVOID Instance,
	PVOID Param,
	//ULONG ShowMode,
	BOOL UnicodeWindow)
{
	NTSTATUS r;

	user32_unicode_string_t window_name;
#if 0
	r = window_name.copy_from_user( WindowName );
	if (r < STATUS_SUCCESS)
		return 0;
#endif

	user32_unicode_string_t wndcls_name;
	r = wndcls_name.copy_from_user( ClassName );
	if (r < STATUS_SUCCESS)
		return 0;

	NTCREATESTRUCT cs;

	cs.lpCreateParams = Param;
	cs.hInstance = Instance;
	cs.hwndParent = (HWND) Parent;
	cs.hMenu = Menu;
	cs.cx = Width;
	cs.cy = Height;
	cs.x = x;
	cs.y = y;
	cs.style = Style;
	cs.lpszName = NULL;
	cs.lpszClass = NULL;
	cs.dwExStyle = ExStyle;

	window_tt* win = window_tt::do_create( window_name, wndcls_name, cs );
	if (!win)
		return NULL;
	return win->handle;
}

window_tt* window_tt::do_create( unicode_string_t& name, unicode_string_t& cls, NTCREATESTRUCT& cs )
{
	dprintf("window = %pus class = %pus\n", &name, &cls );

	window_tt* parent_win = 0;
	if (cs.hwndParent)
	{
		parent_win = window_from_handle( cs.hwndParent );
		if (!parent_win)
			return FALSE;
	}
	else
		parent_win = desktop_window;

	wndcls_tt* wndcls = wndcls_tt::from_name( cls );
	if (!wndcls)
		return 0;

	// tweak the styles
	cs.dwExStyle |= WS_EX_WINDOWEDGE;
	cs.dwExStyle &= ~0x80000000;

	if (cs.x == CW_USEDEFAULT)
		cs.x = 0;
	if (cs.y == CW_USEDEFAULT)
		cs.y = 0;

	if (cs.cx == CW_USEDEFAULT)
		cs.cx = 100;
	if (cs.cy == CW_USEDEFAULT)
		cs.cy = 100;

	// allocate a window
	window_tt *win = new window_tt;
	dprintf("new window %p\n", win);
	if (!win)
		return NULL;

	win->get_win_thread() = current;
	win->self = win;
	win->wndcls = wndcls;
	win->style = cs.style;
	win->exstyle = cs.dwExStyle;
	win->rcWnd.left = cs.x;
	win->rcWnd.top = cs.y;
	win->rcWnd.right = cs.x + cs.cx;
	win->rcWnd.bottom = cs.y + cs.cy;
	win->hInstance = cs.hInstance;

	win->link_window( parent_win );

	win->handle = (HWND) alloc_user_handle( win, USER_HANDLE_WINDOW, current->process );
	win->wndproc = wndcls->get_wndproc();

	// create a thread message queue if necessary
	if (!current->queue)
		current->queue = new thread_message_queue_tt;

	region_tt*& region = win->get_invalid_region();
	region = region_tt::alloc();
	region->empty_region();

	// send WM_GETMINMAXINFO
	getminmaxinfo_tt minmax;
	win->send( minmax );

	// send WM_NCCREATE
	nccreate_message_tt nccreate( cs, cls, name );
	win->send( nccreate );

	win->rcWnd.left = cs.x;
	win->rcWnd.top = cs.y;
	win->rcWnd.right = cs.x + cs.cx;
	win->rcWnd.bottom = cs.y + cs.cy;

	// FIXME: not always correct
	win->rcClient = win->rcWnd;

	// send WM_NCCALCSIZE
	nccalcsize_message_tt nccalcsize( FALSE, win->rcWnd );
	win->send( nccalcsize );

	win->style |= WS_CLIPSIBLINGS;

	win->create_dc();
	
	// send WM_CREATE
	create_message_tt create( cs, cls, name );
	win->send( create );

	if (win->style & WS_VISIBLE)
	{
		dprintf("Window has WS_VISIBLE\n");
		win->set_window_pos( SWP_SHOWWINDOW | SWP_NOMOVE );

		// move manually afterwards
		movemsg_tt move( win->rcWnd.left, win->rcWnd.top );
		win->send( move );
	}

	return win;
}

void window_tt::create_dc(void) {
  win_dc = win32k_manager->alloc_screen_dc_ptr();
}

window_tt* window_tt::find_window_to_repaint( HWND window, thread_t* thread )
{
	window_tt *win;
	if (window)
	{
		win = window_from_handle( window );
		if (!win)
			return FALSE;
	}
	else
		win = desktop_window;

	return find_window_to_repaint( win, thread );
}

window_tt* window_tt::find_window_to_repaint( window_tt* win, thread_t* thread )
{
	// special case the desktop window for the moment
	if (win->parent)
	{
		region_tt*& region = win->get_invalid_region();
		if (region->get_region_type() != NULLREGION)
			return win;
	}

	for (WND *p = win->first_child; p; p = p->next)
	{
		win = find_window_to_repaint( p->handle, thread );
		if (win)
			return win;
	}

	return NULL;
}

void window_tt::set_window_pos( UINT flags )
{
	if (!(style & WS_VISIBLE))
		return;

	if (flags & SWP_SHOWWINDOW)
	{
		show( SW_SHOW );

		region_tt*& rgn = get_invalid_region();
		rgn->set_rect( rcClient );
	}

	WINDOWPOS wp;
	memset( &wp, 0, sizeof wp );
	wp.hwnd = handle;
	if (!(flags & SWP_NOMOVE))
	{
		wp.x = rcWnd.left;
		wp.y = rcWnd.right;
		wp.cx = rcWnd.right - rcWnd.left;
		wp.cy = rcWnd.bottom - rcWnd.top;
	}

	if (flags & (SWP_SHOWWINDOW | SWP_HIDEWINDOW))
	{
		winposchanging_tt poschanging( wp );
		send( poschanging );
	}

	// send activate messages
	if (!(flags & SWP_NOACTIVATE))
	{
		activate();

		// painting probably should be done elsewhere
		ncpaintmsg_tt ncpaint;
		send( ncpaint );

		erasebkgmsg_tt erase( get_dc() );
		send( erase );
	}

	if (style & WS_VISIBLE)
	{
		winposchanged_tt poschanged( wp );
		send( poschanged );
	}

	if (flags & SWP_HIDEWINDOW)
	{
		// deactivate
		ncactivate_tt ncact;
		send( ncact );
	}

	if (!(flags & SWP_NOSIZE))
	{
		sizemsg_tt size( rcWnd.right - rcWnd.left,
				rcWnd.bottom - rcWnd.top );
		send( size );
	}

	if (!(flags & SWP_NOMOVE))
	{
		movemsg_tt move( rcWnd.left, rcWnd.top );
		send( move );
	}
}

device_context_t *window_tt::get_device_context()
{
  return win_dc;
}

HGDIOBJ window_tt::get_dc()
{
  /*
	device_context_t *dc = win32k_manager->alloc_screen_dc_ptr();
	if (!dc)
		return 0;

	dc->set_bounds_rect( rcClient );
	return dc->get_handle();
  */
  if (!win_dc) return 0;

  win_dc->set_bounds_rect( rcClient );
  win_dc->select();
  
  return win_dc->get_handle();
}

void window_tt::activate()
{
	if (active_window == this)
		return;

	if (active_window)
	{
		appactmsg_tt aa( WA_INACTIVE );
		active_window->send( aa );
	}

	active_window = this;
	appactmsg_tt aa( WA_ACTIVE );
	send( aa );

	ncactivate_tt ncact;
	send( ncact );

	activate_tt act;
	send( act );

	setfocusmsg_tt setfocus;
	send( setfocus );
}

BOOLEAN window_tt::destroy()
{
	// set the window to zero size
	set_window_pos( SWP_NOMOVE | SWP_NOSIZE |
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW );

	destroymsg_tt destroy;
	send( destroy );

	ncdestroymsg_tt ncdestroy;
	send( ncdestroy );

	delete this;
	return TRUE;
}

BOOLEAN NTAPI NtUserSetLogonNotifyWindow( HWND Window )
{
	return TRUE;
}

LONG NTAPI NtUserGetClassInfo(
	PVOID Module,
	PUNICODE_STRING ClassName,
	PVOID Buffer,
	PULONG Length,
	ULONG Unknown)
{
	unicode_string_t class_name;
	NTSTATUS r = class_name.copy_from_user( ClassName );
	if (r < STATUS_SUCCESS)
		return r;
	dprintf("%pus\n", &class_name );

	return 0;
}

BOOLEAN NTAPI NtUserNotifyProcessCreate( ULONG NewProcessId, ULONG CreatorId, ULONG, ULONG )
{
	return TRUE;
}

BOOLEAN NTAPI NtUserConsoleControl( ULONG Id, PVOID Information, ULONG Length )
{
	return TRUE;
}

BOOLEAN NTAPI NtUserGetObjectInformation( HANDLE Object, ULONG InformationClass, PVOID Buffer, ULONG Length, PULONG ReturnLength)
{
	return TRUE;
}

BOOLEAN NTAPI NtUserResolveDesktop(HANDLE Process, PVOID, PVOID, PHANDLE Desktop )
{
	return TRUE;
}

BOOLEAN NTAPI NtUserShowWindow( HWND Window, INT Show )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	return win->show( Show );
}

HANDLE NTAPI NtUserCreateAcceleratorTable( PVOID Accelerators, UINT Count )
{
	static UINT accelerator = 1;
	return (HANDLE) accelerator++;
}

BOOLEAN NTAPI NtUserMoveWindow( HWND Window, int x, int y, int width, int height, BOOLEAN repaint )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	return win->move_window( x, y, width, height, repaint );
}

BOOLEAN window_tt::move_window( int x, int y, int width, int height, BOOLEAN repaint )
{
	WINDOWPOS wp;
	memset( &wp, 0, sizeof wp );
	wp.hwnd = handle;

	wp.x = x;
	wp.y = y;
	wp.cx = width;
	wp.cy = height;

	winposchanging_tt poschanging( wp );
	send( poschanging );

	rcWnd.left = x;
	rcWnd.top = y;
	rcWnd.right = x + width;
	rcWnd.bottom = y + height;

	rcClient = rcWnd;

	nccalcsize_message_tt nccalcsize( TRUE, rcWnd );
	send( nccalcsize );

	winposchanged_tt poschanged( wp );
	send( poschanged );

	return TRUE;
}

BOOLEAN NTAPI NtUserRedrawWindow( HWND Window, RECT *Update, HANDLE Region, UINT Flags )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	if (!(win->style & WS_VISIBLE))
		return TRUE;

	RECT rect;
	if (Update)
	{
		NTSTATUS r = copy_from_user( &rect, Update );
		if (r < STATUS_SUCCESS)
			return FALSE;
	}
	else
	{
		rect = win->rcClient;
	}

	region_tt*& region = win->get_invalid_region();
	region->set_rect( rect );

	return TRUE;
}

ULONG NTAPI NtUserGetAsyncKeyState( ULONG Key )
{
	return win32k_manager->get_async_key_state( Key );
}

LRESULT NTAPI NtUserDispatchMessage( PMSG Message )
{
	MSG msg;
	NTSTATUS r;
	r = copy_from_user( &msg, Message );
	if (r < STATUS_SUCCESS)
		return 0;

	window_tt *win = window_from_handle( msg.hwnd );
	if (!win)
		return 0;

	switch (msg.message)
	{
	case WM_PAINT:
		{
		paintmsg_tt msg;
		win->send( msg );
		}
		break;
	default:
		dprintf("unknown message %04x\n", msg.message);
	}

	return 0;
}

BOOLEAN NTAPI NtUserInvalidateRect( HWND Window, const RECT* Rectangle, BOOLEAN Erase )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	if (!(win->style & WS_VISIBLE))
		return TRUE;

	RECT rect;
	if (Rectangle)
	{
		NTSTATUS r = copy_from_user( &rect, Rectangle );
		if (r < STATUS_SUCCESS)
			return FALSE;
	}
	else
	{
		rect = win->rcClient;
	}

	region_tt*& region = win->get_invalid_region();
	region->set_rect( rect );

	return TRUE;
}

BOOLEAN NTAPI NtUserMessageCall( HWND Window, ULONG, ULONG, PVOID, ULONG, ULONG, ULONG)
{
	return TRUE;
}

BOOLEAN NTAPI NtUserDestroyWindow( HWND Window )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	return win->destroy();
}

BOOLEAN NTAPI NtUserValidateRect( HWND Window, PRECT Rect )
{
	return TRUE;
}

BOOLEAN NTAPI NtUserGetUpdateRgn( HWND Window, HRGN Region, BOOLEAN Erase )
{
	return TRUE;
}

HDC NTAPI NtUserBeginPaint( HWND Window, PAINTSTRUCT* pps)
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return NULL;

	PAINTSTRUCT ps;
	memset( &ps, 0, sizeof ps );
	ps.rcPaint.left = 0;
	ps.rcPaint.top = 0;
	ps.rcPaint.bottom = win->rcClient.bottom - win->rcClient.top;
	ps.rcPaint.right = win->rcClient.right - win->rcClient.left;
	NTSTATUS r = copy_to_user( pps, &ps );
	if (r < STATUS_SUCCESS)
		return NULL;

	region_tt*& region = win->get_invalid_region();
	region->empty_region();

	return (HDC) win->get_dc();
}

BOOLEAN NTAPI NtUserEndPaint( HWND Window, PAINTSTRUCT* pps )
{
#if 0
	window_tt *win = window_from_handle( Window );
	if (!win)
		return NULL;

	device_context_t *dc = win32k_manager->alloc_screen_dc_ptr();
	if (!dc)
	  return TRUE;

	dc->set_bounds_rect( win->rcClient );

	dc->paint( );

	delete dc;
#endif

	window_tt *win = window_from_handle( Window );
	if (!win)
		return NULL;

	device_context_t *dc = win->get_device_context();
	dc->repaint( );

	return TRUE;
}

BOOLEAN NTAPI NtUserCallHwnd( HWND Window, ULONG )
{
	return TRUE;
}

BOOLEAN NTAPI NtUserSetMenu( HWND Window, ULONG, ULONG )
{
	return TRUE;
}

HWND NTAPI NtUserSetCapture( HWND Window )
{
	return 0;
}

int NTAPI NtUserTranslateAccelerator( HWND Window, HACCEL AcceleratorTable, PMSG Message )
{
	return 0;
}

BOOLEAN NTAPI NtUserTranslateMessage( PMSG Message, ULONG )
{
	return 0;
}

HWND window_tt::from_point( POINT& pt )
{
	for (PWND win = first_child; win; win = win->next)
	{
		rect_tt r( win->rcWnd );
		if (r.contains_point( pt ))
			return win->handle;
	}
	return handle;
}

HWND NTAPI NtUserWindowFromPoint( POINT pt )
{
	window_tt *win = desktop_window;
	if (!win)
		return 0;
	return win->from_point( pt );
}
