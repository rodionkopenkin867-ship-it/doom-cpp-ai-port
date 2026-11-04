// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>
#ifdef _WIN32
#include "w32compat.h"     // POSIX shims over the Windows CRT
#else
#include <time.h>       // clock_gettime
#include <unistd.h>     // usleep
#endif#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"




int	mb_used = 128;


void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    *size = mb_used*1024*1024;
    // Zone allocator needs 8-byte aligned base on x64 (ZONE_ALIGN in z_zone.cpp).
    byte* base = (byte *) malloc (*size + 8);
    base = (byte *)(((size_t)base + 7) & ~(size_t)7);
    printf ("I_ZoneBase: %i MB at %p\n", mb_used, (void*)base);
    fflush (stdout);
    return base;
}



//
// I_GetTime
// returns time in 1/70th second tics
//
#ifdef _WIN32
int  I_GetTime (void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    static __int64 basetime=0;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    if (!basetime)
	basetime = counter.QuadPart;
    // time in 1/70th of a second
    return (int)((counter.QuadPart - basetime) * TICRATE / freq.QuadPart);
}
#else
int  I_GetTime (void)
{
    struct timespec now;
    static long long basems = 0;
    long long ms;

    clock_gettime (CLOCK_MONOTONIC, &now);
    ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
    if (!basems)
	basems = ms;
    // time in 1/70th of a second
    return (int)((ms - basems) * TICRATE / 1000);
}
#endif



//
// I_Init
//
void I_Init (void)
{
    I_InitSound();
    //  I_InitGraphics();
}

//
// I_Quit
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();
    exit(0);
}

void I_WaitVBL(int count)
{
#ifdef _WIN32
    Sleep (count * (1000/70) );
#else
    usleep (count * (1000/70) * 1000);
#endif
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;
        
    mem = (byte *)malloc (length);
    memset (mem,0,length);
    return mem;
}


//
// I_Error
//
extern boolean demorecording;

void I_Error (char *error, ...)
{
    va_list	argptr;

    // Message first.
    va_start (argptr,error);
    fprintf (stderr, "Error: ");
    vfprintf (stderr,error,argptr);
    fprintf (stderr, "\n");
    va_end (argptr);

    fflush( stderr );

    // Shutdown. Here might be other errors.
    if (demorecording)
	G_CheckDemoStatus();

    D_QuitNetGame ();
    I_ShutdownGraphics();
    
    exit(-1);
}
