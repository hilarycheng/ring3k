/*
 * native test suite
 *
 * Copyright 2008 Mike McCormack
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

#include "ntapi.h"
#include "rtlapi.h"
#include "ntwin32.h"
#include "log.h"

void* get_teb( void )
{
	void *p;
	__asm__ ( "movl %%fs:0x18, %%eax\n\t" : "=a" (p) );
	return p;
}

const int ofs_peb_in_teb = 0x30;

void* get_peb( void )
{
	void **p = get_teb();
	return p[ofs_peb_in_teb/sizeof (*p)];
}

HANDLE get_process_heap( void )
{
	void **p = get_peb();
	return p[0x18/sizeof (*p)];
}

char hex(BYTE x)
{
	if (x<10)
		return x+'0';
	return x+'A'-10;
}

void dump_bin(BYTE *buf, ULONG sz)
{
	char str[0x33];
	int i;
	for (i=0; i<sz; i++)
	{
		str[(i%16)*3] = hex(buf[i]>>4);
		str[(i%16)*3+1] = hex(buf[i]&0x0f);
		str[(i%16)*3+2] = ' ';
		str[(i%16)*3+3] = 0;
		if ((i+1)%16 == 0 || (i+1) == sz)
			dprintf("%s\n", str);
	}
}

typedef struct _font_enum_entry {
	ULONG size;
	ULONG offset;
	ULONG fonttype;
	ENUMLOGFONTEXW elfew;
	NEWTEXTMETRICEXW ntme;
} font_enum_entry;

void test_font_enum( void )
{
	HANDLE heap = get_process_heap();
	HANDLE henum, hdc;
	BOOL r;
	ULONG buf[8];
	ULONG size = 0, retlen = 0;
	PVOID buffer;
	ULONG ofs;

	hdc = NtGdiOpenDCW(0,0,0,0,0,0,&buf);
	ok( hdc != 0, "NtGdiOpenDCW failed\n");

	henum = NtGdiEnumFontOpen(hdc,3,0,0,0,1,&size);
	ok( henum != 0, "NtGdiEnumFontOpen failed\n");
	ok( size != 0, "size not set\n");

	buffer = RtlAllocateHeap( heap, 0, size );
	ok( buffer != 0, "RtlAllocateHeap failed\n");

	r = NtGdiEnumFontChunk(hdc, henum, size, &retlen, buffer);
	ok(r, "NtGdiEnumFontChunk failed\n");

	ofs = 0;
	while (ofs < size)
	{
		font_enum_entry *fe = (font_enum_entry*)(buffer+ofs);

		/*dprintf("offset %04lx type=%04lx name=%S\n",
			 ofs, fe->fonttype, fe->elfew.elfFullName);*/

		if (!fe->size)
			break;

		// next
		ofs += fe->size;
	}
	ok( ofs == size, "length mismatch\n");

	//dump_bin( buffer, retlen );

	r = NtGdiEnumFontClose(henum);
	ok(r, "NtGdiEnumFontClose failed\n");
}

// magic numbers for everybody
void NTAPI init_callback(void *arg)
{
	NtCallbackReturn( 0, 0, 0 );
}

void *callback_table[NUM_USER32_CALLBACKS];

void init_callbacks( void )
{
	callback_table[NTWIN32_THREAD_INIT_CALLBACK] = &init_callback;
	__asm__ (
		"movl %%fs:0x18, %%eax\n\t"
		"movl 0x30(%%eax), %%eax\n\t"
		"movl %%ebx, 0x2c(%%eax)\n\t"  // set PEB's KernelCallbackTable
		: : "b" (&callback_table) : "eax" );
}

void become_gui_thread( void )
{
	init_callbacks();
	NtUserGetThreadState(0x11);
}

void NtProcessStartup( void )
{
	log_init();
	become_gui_thread();
	test_font_enum();
	log_fini();
}