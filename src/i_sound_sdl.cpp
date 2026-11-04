// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DOOM SDL2 sound backend (Linux counterpart of i_sound_w32.cpp)
//
// The classic linuxdoom "sound module" design: up to 8 internal mixing
// channels of signed 8-bit DOOM PCM samples, mixed into 11025 Hz stereo
// 16-bit output. Instead of waveOut buffers, an SDL_AudioStream callback
// pulls freshly mixed samples whenever the device needs them, so SFX are
// continuously mixed on the SDL audio thread.
//
// Music (MUS lumps) is stubbed for now: the Win32 backend converts MUS to
// MIDI and plays it via MCI; there is no such facility under SDL2 out of
// the box, so I_RegisterSong/Play are no-ops. The game runs silent-only.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <SDL2/SDL.h>

#include "doomstat.h"
#include "i_sound.h"
#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"
#include "m_argv.h"

// Kept for the defaults table (m_misc.cpp references it); our SDL2 sound
// backend runs in-process, so no external server binary is spawned.
char* sndserver_filename = (char*)"./sndserver ";

// The number of internal mixing channels.
#define NUM_CHANNELS    8
// Output sample rate (DOOM SFX lumps are 11025 Hz).
#define SAMPLERATE      11025
// Padding granularity used by getsfx(), kept for parity with the original.
#define SAMPLECOUNT     512

// The actual lengths of all sound effects.
int lengths[NUMSFX];

// The channel step amount...
unsigned int channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
unsigned int channelstepremainder[NUM_CHANNELS];

// The channel data pointers, start and end.
unsigned char*  channels[NUM_CHANNELS];
unsigned char*  channelsend[NUM_CHANNELS];

// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
int channelstart[NUM_CHANNELS];

// The sound in channel handles, determined on registration.
int channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int channelids[NUM_CHANNELS];

// Pitch to stepping lookup.
int steptable[256];

// Volume lookups.
int vol_lookup[128*256];

// Hardware left and right channel volume lookup.
int* channelleftvol_lookup[NUM_CHANNELS];
int* channelrightvol_lookup[NUM_CHANNELS];

// --- SDL audio state ---

static SDL_AudioDeviceID sdl_dev = 0;
static boolean   sdl_sound_started = false;

//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
void*
getsfx
( char*         sfxname,
  int*          len )
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    sprintf(name, "ds%s", sfxname);

    // Not gamemode aware: sounds from DOOM II will be requested even
    //  with DOOM shareware; fall back to a default sound for replacement.
    if ( W_CheckNumForName(name) == -1 )
        sfxlump = W_GetNumForName("dspistol");
    else
        sfxlump = W_GetNumForName(name);

    size = W_LumpLength( sfxlump );

    sfx = (unsigned char*)W_CacheLumpNum( sfxlump, PU_STATIC );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    paddedsize = ((size-8 + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;

    // Allocate from zone memory.
    paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );

    // Now copy and pad.
    memcpy(  paddedsfx, sfx, size );
    for (i=size ; i<paddedsize+8 ; i++)
        paddedsfx[i] = 128;

    // Remove the cached lump.
    Z_Free( sfx );

    // Preserve padded length.
    *len = paddedsize;

    // Return allocated padded data.
    return (void *) (paddedsfx + 8);
}

//
// Set the left/right volume lookup pointers of a mixing channel.
// Volume arrives already scaled to 0..127, separation 0..255.
//
static void
sdl_setChannelVol
( int   slot,
  int   volume,
  int   seperation )
{
    int leftvol;
    int rightvol;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 separation, adjust volume properly.
    leftvol =
        volume - ((volume*seperation*seperation) >> 16);
    seperation = seperation - 257;
    rightvol =
        volume - ((volume*seperation*seperation) >> 16);

    // Clamp volume (the original I_Error'ed here).
    if (rightvol < 0)   rightvol = 0;
    if (rightvol > 127) rightvol = 127;
    if (leftvol < 0)    leftvol = 0;
    if (leftvol > 127)  leftvol = 127;

    // Get the proper lookup table piece for this volume level.
    channelleftvol_lookup[slot]  = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];
}

//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int
addsfx
( int           sfxid,
  int           volume,
  int           step,
  int           seperation )
{
    static unsigned short       handlenums = 0;

    int         i;
    int         rc = -1;

    int         oldest = gametic;
    int         oldestnum = 0;
    int         slot;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
         || sfxid == sfx_sawidl
         || sfxid == sfx_sawful
         || sfxid == sfx_sawhit
         || sfxid == sfx_stnmov
         || sfxid == sfx_pistol  )
    {
        // Loop all channels, check.
        for (i=0 ; i<NUM_CHANNELS ; i++)
        {
            // Active, and using the same SFX?
            if ( (channels[i])
                 && (channelids[i] == sfxid) )
            {
                // Reset.
                channels[i] = 0;
                // We are sure that iff,
                //  there will only be one.
                break;
            }
        }
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
    {
        if (channelstart[i] < oldest)
        {
            oldestnum = i;
            oldest = channelstart[i];
        }
    }

    // If we found a channel, fine.
    // If not, we simply overwrite the first one, 0.
    if (i == NUM_CHANNELS)
        slot = oldestnum;
    else
        slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) S_sfx[sfxid].data;
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + lengths[sfxid];

    // Reset current handle number, limited to 0..100.
    if (!handlenums)
        handlenums = 100;

    // Assign current handle number.
    channelhandles[slot] = rc = handlenums++;

    // Set stepping.
    channelstep[slot] = step;
    channelstepremainder[slot] = 0;
    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Volume arrives scaled to 0..127 from I_StartSound.
    sdl_setChannelVol(slot, volume, seperation);

    // Preserve sound SFX id, e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // You tell me.
    return rc;
}

//
// SDL audio thread callback: mix `total` stereo sample pairs directly
// into stream. Same inner loop as the w32/waveOut mixer, just pulled
// from the device instead of pushed per tic.
//
static void
sdl_audioCallback
( void*     userdata,
  unsigned char* stream,
  int       len )
{
    short*  dst = (short*)stream;
    int     total = len / (int)(2 * sizeof(short));
    int     s;

    memset(stream, 0, len);

    for (s = 0; s < total; ++s)
    {
        unsigned int sample;
        int dl = 0;
        int dr = 0;
        int chan;

        for (chan = 0; chan < NUM_CHANNELS; ++chan)
        {
            // Check channel, if active.
            if (channels[chan])
            {
                // Get the raw data from the channel.
                sample = *channels[chan];
                // Add left and right part for this channel (sound)
                //  to the current data. Adjust volume accordingly.
                dl += channelleftvol_lookup[chan][sample];
                dr += channelrightvol_lookup[chan][sample];
                // Increment index.
                channelstepremainder[chan] += channelstep[chan];
                // MSB is next sample.
                channels[chan] += channelstepremainder[chan] >> 16;
                // Limit to LSB.
                channelstepremainder[chan] &= 65536-1;

                // Check whether we are done.
                if (channels[chan] >= channelsend[chan])
                    channels[chan] = 0;
            }
        }

        // Clamp to range, left then right hardware channel.
        if (dl > 0x7fff)        dl = 0x7fff;
        else if (dl < -0x8000)  dl = -0x8000;
        if (dr > 0x7fff)        dr = 0x7fff;
        else if (dr < -0x8000)  dr = -0x8000;

        dst[s*2]   = (short)dl;
        dst[s*2+1] = (short)dr;
    }
}

//
// SFX API
// Called by S_Init: sets up the internal mixing lookups.
//
void I_SetChannels()
{
    int         i;
    int         j;

    int*        steptablemid = steptable + 128;

    // Okay, reset internal mixing channels to zero.
    for (i=0; i<NUM_CHANNELS; i++)
    {
        channels[i] = 0;
        channelhandles[i] = 0;
        channelids[i] = -1;
        channelstart[i] = 0;
        channelstep[i] = 0;
        channelstepremainder[i] = 0;
        channelleftvol_lookup[i] = vol_lookup;
        channelrightvol_lookup[i] = vol_lookup;
    }

    // This table provides step widths for pitch parameters.
    for (i=-128 ; i<128 ; i++)
        steptablemid[i] = (int)(pow(2.0, (i/64.0))*65536.0);

    // Generates volume lookup tables
    //  which also turn the unsigned samples
    //  into signed samples.
    for (i=0 ; i<128 ; i++)
        for (j=0 ; j<256 ; j++)
            vol_lookup[i*256+j] = (i*(j-128)*256)/127;
}

void I_SetSfxVolume(int volume)
{
    // Menu/config setting propagates to the state
    //  variable used in the mixing.
    snd_SfxVolume = volume;
}

//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
//
int
I_StartSound
( int           id,
  int           vol,
  int           sep,
  int           pitch,
  int           priority )
{
    // UNUSED
    (void)priority;

    if (!sdl_sound_started)
        return -1;

    // The menu keeps snd_SfxVolume in 0..15; scale it up to the
    // 0..127 range the mixer lookup tables expect (as Chocolate Doom
    // does by passing volume*8 into S_Init).
    vol <<= 3;
    if (vol > 127) vol = 127;

    if (sdl_dev)
        SDL_LockAudioDevice(sdl_dev);

    // Returns a handle (not used by the caller beyond IsPlaying).
    id = addsfx( id, vol, steptable[pitch], sep );

    if (sdl_dev)
        SDL_UnlockAudioDevice(sdl_dev);

    return id;
}

void I_StopSound (int handle)
{
    // Find the internal channel with this handle and free it.
    int i;

    if (!sdl_sound_started)
        return;

    if (sdl_dev)
        SDL_LockAudioDevice(sdl_dev);
    for (i=0 ; i<NUM_CHANNELS ; i++)
    {
        if (channels[i] && channelhandles[i] == handle)
        {
            channels[i] = 0;
            break;
        }
    }
    if (sdl_dev)
        SDL_UnlockAudioDevice(sdl_dev);
}

int I_SoundIsPlaying(int handle)
{
    // The handle is playing while it owns an internal mixing channel.
    int i;
    for (i=0 ; i<NUM_CHANNELS ; i++)
        if (channels[i] && channelhandles[i] == handle)
            return 1;
    return 0;
}

void
I_UpdateSoundParams
( int   handle,
  int   vol,
  int   sep,
  int   pitch)
{
    // Update volume/separation/pitch of the channel owning the handle.
    int i;
    if (!sdl_sound_started)
        return;

    vol <<= 3;
    if (vol > 127) vol = 127;

    if (sdl_dev)
        SDL_LockAudioDevice(sdl_dev);
    for (i=0 ; i<NUM_CHANNELS ; i++)
    {
        if (channels[i] && channelhandles[i] == handle)
        {
            sdl_setChannelVol(i, vol, sep);
            channelstep[i] = steptable[pitch];
            break;
        }
    }
    if (sdl_dev)
        SDL_UnlockAudioDevice(sdl_dev);
}

//
// Mixing runs continuously on the SDL audio thread, so per-tic push work
// (waveOut buffer refill in the Win32 backend) is not needed.
//
void I_UpdateSound (void)
{
}

void I_SubmitSound(void)
{
}

void I_InitSound (void)
{
    int i;

    printf ("--- I_InitSound... ---\n");
    fflush (stdout);

    if (sdl_sound_started)
        return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    {
        fprintf(stderr, "I_InitSound: SDL_Init(AUDIO) failed: %s\n",
                SDL_GetError());
        sdl_sound_started = true;   // keep the game logic happy
        return;
    }

    // Initialize external data (all sounds) at start, keep static.
    for (i=1 ; i<NUMSFX ; i++)
    {
        // Alias? Example is the chaingun sound linked to pistol.
        if (!S_sfx[i].link)
        {
            // Load data from WAD file.
            S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
        }
        else
        {
            // Previously loaded already?
            S_sfx[i].data = S_sfx[i].link->data;
            lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
        }
    }
    // NOTE: I_SetChannels() is called by S_Init() in s_sound.c after the
    // engine asks us to init, exactly like the Win32 backend assumes.

    // Open the SDL audio device: 11025 Hz, stereo, signed 16-bit.
    {
        SDL_AudioSpec want;
        SDL_AudioSpec have;

        memset(&want, 0, sizeof(want));
        want.freq     = SAMPLERATE;
        want.format   = AUDIO_S16SYS;
        want.channels = 2;
        want.samples  = 512;
        want.callback = sdl_audioCallback;
        want.userdata = 0;

        sdl_dev = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
        if (!sdl_dev)
        {
            fprintf(stderr, "I_InitSound: SDL_OpenAudioDevice failed, no SFX: %s\n",
                    SDL_GetError());
            sdl_sound_started = true;   // keep the game logic happy
            return;
        }
        if (have.freq != SAMPLERATE || have.format != want.format)
        {
            // SDL will have converted internally anyway; just note it.
            fprintf (stderr, "I_InitSound: device gave %d Hz fmt %d\n",
                     have.freq, have.format);
        }
        SDL_PauseAudioDevice(sdl_dev, 0);   // start playing
    }

    sdl_sound_started = true;
}

void I_ShutdownSound(void)
{
    if (!sdl_sound_started)
        return;

    if (sdl_dev)
    {
        SDL_LockAudioDevice(sdl_dev);
        {
            int i;
            for (i=0 ; i<NUM_CHANNELS ; i++)
                channels[i] = 0;
        }
        SDL_UnlockAudioDevice(sdl_dev);

        SDL_CloseAudioDevice(sdl_dev);
        sdl_dev = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_sound_started = false;
}

// ===========================================================================
//                          MUSIC
// ===========================================================================
//
// Stub for now. The Win32 backend converts the MUS lump to MIDI and plays
// it through MCI; SDL2 has no built-in MUS/MIDI player. The game runs and
// mixes SFX; music is silently absent until a proper SDL2 music backend
// (e.g. SDL_mixer or a native MUS/OPL renderer) lands.
//
// The no-op implementations keep the S_*() API contract intact.
//

void I_InitMusic (void)
{
}

void I_ShutdownMusic (void)
{
}

void I_SetMusicVolume (int volume)
{
    (void)volume;
}

// NOP.
void I_PauseSong (int handle)
{
    (void)handle;
}

// NOP.
void I_ResumeSong (int handle)
{
    (void)handle;
}

//
// Songs merely say "registered"; nothing is actually playable yet.
//
int I_RegisterSong (void* data, int length)
{
    (void)data;
    (void)length;
    return 1;
}

// NOP.
void I_PlaySong (int handle, int looping)
{
    (void)handle;
    (void)looping;
}

// NOP.
void I_StopSong(int handle)
{
    (void)handle;
}

// NOP.
void I_UnRegisterSong(int handle)
{
    (void)handle;
}
