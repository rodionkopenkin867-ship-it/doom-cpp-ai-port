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
#include <SDL2/SDL_mixer.h>

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
// MUS -> MIDI conversion (ported verbatim from i_sound_w32.cpp: the Ben
// Ryves mus2mid algorithm as used by Chocolate Doom) + SDL_mixer playback.
// The converted type-0 MIDI stays in memory and is handed to SDL_mixer via
// a read-only RWops - no temp file needed.
//
// NOTE: MIDI playback inside SDL2_mixer is an optional backend; Debian's
// libsdl2-mixer-2.0-0 ships it, but a distro build without MIDI support
// will fail Mix_LoadMUS_RW - we log and keep running with SFX only.
//
// ===========================================================================

// Growable (fixed-cap) output buffer for the converted MIDI stream.
typedef struct
{
    byte*   data;
    size_t  len;
    size_t  cap;
    int     overflow;
} membuf_t;

static void mb_put(membuf_t* b, byte v)
{
    if (b->len < b->cap)
        b->data[b->len++] = v;
    else
        b->overflow = 1;
}

static void mb_write(membuf_t* b, const byte* p, size_t n)
{
    size_t i;
    for (i=0 ; i<n ; ++i)
        mb_put(b, p[i]);
}

#define NUM_MIDICHANS       16
#define MIDI_PERCUSSION_CHAN 9
#define MUS_PERCUSSION_CHAN  15

// MUS event codes
enum
{
    mus_releasekey       = 0x00,
    mus_presskey         = 0x10,
    mus_pitchwheel       = 0x20,
    mus_systemevent      = 0x30,
    mus_changecontroller = 0x40,
    mus_scoreend         = 0x60
};

// MIDI event codes
enum
{
    midi_releasekey       = 0x80,
    midi_presskey         = 0x90,
    midi_changecontroller = 0xB0,
    midi_changepatch      = 0xC0,
    midi_pitchwheel       = 0xE0
};

// Standard MIDI type 0 header + track header
static const byte midiheader[] =
{
    'M', 'T', 'h', 'd',     // Main header
    0x00, 0x00, 0x00, 0x06, // Header size
    0x00, 0x00,             // MIDI type (0)
    0x00, 0x01,             // Number of tracks
    0x00, 0x46,             // Resolution
    'M', 'T', 'r', 'k',     // Start of track
    0x00, 0x00, 0x00, 0x00  // Placeholder for track length
};

// Cached channel velocities
static byte channelvelocities[NUM_MIDICHANS];

// Timestamps between sequences of MUS events
static unsigned int queuedtime = 0;

// Counter for the length of the track
static unsigned int tracksize;

static const byte controller_map[] =
{
    0x00, 0x20, 0x01, 0x07, 0x0A, 0x0B, 0x5B, 0x5D,
    0x40, 0x43, 0x78, 0x7B, 0x7E, 0x7F, 0x79
};

static int channel_map[NUM_MIDICHANS];

static boolean WriteTime(unsigned int time, membuf_t* out)
{
    unsigned int buffer = time & 0x7F;

    while ((time >>= 7) != 0)
    {
        buffer <<= 8;
        buffer |= ((time & 0x7F) | 0x80);
    }

    for (;;)
    {
        byte writeval = (byte)(buffer & 0xFF);

        mb_put(out, writeval);
        if (out->overflow)
            return true;

        ++tracksize;

        if ((buffer & 0x80) != 0)
        {
            buffer >>= 8;
        }
        else
        {
            queuedtime = 0;
            return false;
        }
    }
}

static boolean WriteEndTrack(membuf_t* out)
{
    byte endtrack[] = {0xFF, 0x2F, 0x00};

    if (WriteTime(queuedtime, out))
        return true;

    mb_write(out, endtrack, 3);
    if (out->overflow)
        return true;

    tracksize += 3;
    return false;
}

static boolean WritePressKey(byte channel, byte key, byte velocity,
                             membuf_t* out)
{
    byte working[3];
    working[0] = (byte)(midi_presskey | channel);
    working[1] = (byte)(key & 0x7F);
    working[2] = (byte)(velocity & 0x7F);

    if (WriteTime(queuedtime, out))
        return true;

    mb_write(out, working, 3);
    if (out->overflow)
        return true;

    tracksize += 3;
    return false;
}

static boolean WriteReleaseKey(byte channel, byte key, membuf_t* out)
{
    byte working[3];
    working[0] = (byte)(midi_releasekey | channel);
    working[1] = (byte)(key & 0x7F);
    working[2] = 0;

    if (WriteTime(queuedtime, out))
        return true;

    mb_write(out, working, 3);
    if (out->overflow)
        return true;

    tracksize += 3;
    return false;
}

static boolean WritePitchWheel(byte channel, short wheel, membuf_t* out)
{
    byte working[3];
    working[0] = (byte)(midi_pitchwheel | channel);
    working[1] = (byte)(wheel & 0x7F);
    working[2] = (byte)((wheel >> 7) & 0x7F);

    if (WriteTime(queuedtime, out))
        return true;

    mb_write(out, working, 3);
    if (out->overflow)
        return true;

    tracksize += 3;
    return false;
}

static boolean WriteChangePatch(byte channel, byte patch, membuf_t* out)
{
    byte working[2];
    working[0] = (byte)(midi_changepatch | channel);
    working[1] = (byte)(patch & 0x7F);

    if (WriteTime(queuedtime, out))
        return true;

    mb_write(out, working, 2);
    if (out->overflow)
        return true;

    tracksize += 2;
    return false;
}

static boolean WriteChangeController_Valued(byte channel, byte control,
                                            byte value, membuf_t* out)
{
    byte working[3];
    working[0] = (byte)(midi_changecontroller | channel);
    working[1] = (byte)(control & 0x7F);

    // Quirk in vanilla DOOM? MUS controller values should be
    // 7-bit, not 8-bit. Fix to stop MIDI players complaining.
    working[2] = value;
    if (working[2] & 0x80)
        working[2] = 0x7F;

    if (WriteTime(queuedtime, out))
        return true;

    mb_write(out, working, 3);
    if (out->overflow)
        return true;

    tracksize += 3;
    return false;
}

static boolean WriteChangeController_Valueless(byte channel, byte control,
                                               membuf_t* out)
{
    return WriteChangeController_Valued(channel, control, 0, out);
}

// Allocate a free MIDI channel.
static int AllocateMIDIChannel(void)
{
    int result;
    int max = -1;
    int i;

    // Find the current highest-allocated channel.
    for (i=0 ; i<NUM_MIDICHANS ; ++i)
        if (channel_map[i] > max)
            max = channel_map[i];

    // max is now equal to the highest-allocated MIDI channel. We can
    // now allocate the next available channel. This also works if
    // no channels are currently allocated (max=-1).
    result = max + 1;

    // Don't allocate the MIDI percussion channel!
    if (result == MIDI_PERCUSSION_CHAN)
        ++result;

    return result;
}

// Given a MUS channel number, get the MIDI channel number to use.
static int GetMIDIChannel(int mus_channel, membuf_t* out)
{
    // MUS channel 15 is the percussion channel.
    if (mus_channel == MUS_PERCUSSION_CHAN)
    {
        return MIDI_PERCUSSION_CHAN;
    }
    else
    {
        if (channel_map[mus_channel] == -1)
        {
            channel_map[mus_channel] = AllocateMIDIChannel();

            // First time using the channel, send an "all notes off"
            // event. This fixes "The D_DDTBLU disease".
            WriteChangeController_Valueless(channel_map[mus_channel],
                                            0x7b, out);
        }

        return channel_map[mus_channel];
    }
}

static unsigned short rd16le(const byte* p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

// Read a MUS file from a buffer and output a type-0 MIDI file.
// Returns 0 on success, 1 on failure.
static int mus2mid(const byte* mus, int muslen, membuf_t* out)
{
    int pos;
    int end_limit;
    int hitscoreend = 0;
    int i;

    if (muslen < 16)
        return 1;

    if (mus[0] != 'M' || mus[1] != 'U' || mus[2] != 'S' || mus[3] != 0x1A)
        return 1;

    {
        unsigned short scorelength = rd16le(mus + 4);
        unsigned short scorestart  = rd16le(mus + 6);
        pos = scorestart;
        // The score is scorelength bytes starting at scorestart;
        // allow a little slack, but never read past the lump.
        end_limit = scorestart + scorelength + 64;
        if (end_limit > muslen)
            end_limit = muslen;
        if (pos < 0 || pos >= end_limit)
            return 1;
    }

    // Initialise channel map to mark all channels as unused.
    for (i=0 ; i<NUM_MIDICHANS ; ++i)
    {
        channel_map[i] = -1;
        channelvelocities[i] = 127;
    }

    // So, we can assume the MUS file is faintly legit. Let's start
    // writing MIDI data...
    mb_write(out, midiheader, sizeof(midiheader));
    if (out->overflow)
        return 1;
    tracksize = 0;
    queuedtime = 0;

    // Now, process the MUS file:
    while (!hitscoreend)
    {
        byte eventdescriptor;
        byte key;
        byte controllernumber;
        byte controllervalue;
        int  channel;

        if (pos >= end_limit)
            return 1;
        eventdescriptor = mus[pos++];

        channel = GetMIDIChannel(eventdescriptor & 0x0F, out);
        if (out->overflow)
            return 1;

        switch (eventdescriptor & 0x70)
        {
            case mus_releasekey:
                if (pos >= end_limit) return 1;
                key = mus[pos++];
                if (WriteReleaseKey(channel, key, out)) return 1;
                break;

            case mus_presskey:
                if (pos >= end_limit) return 1;
                key = mus[pos++];
                if (key & 0x80)
                {
                    if (pos >= end_limit) return 1;
                    channelvelocities[eventdescriptor & 0x0F] =
                        (byte)(mus[pos++] & 0x7F);
                }
                if (WritePressKey(channel, key,
                                  channelvelocities[eventdescriptor & 0x0F],
                                  out)) return 1;
                break;

            case mus_pitchwheel:
                if (pos >= end_limit) return 1;
                key = mus[pos++];
                if (WritePitchWheel(channel, (short)(key * 64), out)) return 1;
                break;

            case mus_systemevent:
                if (pos >= end_limit) return 1;
                controllernumber = mus[pos++];
                if (controllernumber < 10 || controllernumber > 14)
                    return 1;
                if (WriteChangeController_Valueless(
                        channel, controller_map[controllernumber], out))
                    return 1;
                break;

            case mus_changecontroller:
                if (pos >= end_limit) return 1;
                controllernumber = mus[pos++];
                if (pos >= end_limit) return 1;
                controllervalue = mus[pos++];

                if (controllernumber == 0)
                {
                    if (WriteChangePatch(channel, controllervalue, out))
                        return 1;
                }
                else
                {
                    if (controllernumber < 1 || controllernumber > 9)
                        return 1;
                    if (WriteChangeController_Valued(
                            channel, controller_map[controllernumber],
                            controllervalue, out))
                        return 1;
                }
                break;

            case mus_scoreend:
                hitscoreend = 1;
                break;

            default:
                return 1;
        }

        if (!hitscoreend && (eventdescriptor & 0x80))
        {
            // Now we need to read the time code:
            unsigned int timedelay = 0;
            for (;;)
            {
                byte working;
                if (pos >= end_limit) return 1;
                working = mus[pos++];
                timedelay = timedelay * 128 + (working & 0x7F);
                if ((working & 0x80) == 0)
                    break;
            }
            queuedtime += timedelay;
        }
    }

    // End of track
    if (WriteEndTrack(out))
        return 1;

    // Write the track size into the stream (offset 18 in the header).
    if (out->len < 22)
        return 1;
    out->data[18] = (byte)((tracksize >> 24) & 0xff);
    out->data[19] = (byte)((tracksize >> 16) & 0xff);
    out->data[20] = (byte)((tracksize >> 8) & 0xff);
    out->data[21] = (byte)(tracksize & 0xff);

    return 0;
}

// ---------------------------------------------------------------------------
// SDL_mixer music state
// ---------------------------------------------------------------------------

// Keep a small slot table: s_sound registers a new song and may unregister
// the current one later; handles are the 1-based slot numbers. The
// converted MIDI buffer stays alive for the whole Mix_Music lifetime
// because streamable loaders may read from it lazily.
#define SDL_MAX_SONGS 8

typedef struct
{
    int         inuse;
    Mix_Music*  mus;
    byte*       midibuf;     // the converted MIDI, freed at UnRegister
} sdl_song_t;

static sdl_song_t sdl_songs[SDL_MAX_SONGS];
static int        sdl_cur_song = 0;   // handle currently fed to the mixer
static boolean    sdl_music_open = false;

void I_InitMusic (void)
{
    if (sdl_music_open)
        return;

    if (Mix_Init(MIX_INIT_MID) == 0)
    {
        char msg[512];
        sprintf(msg, "I_InitMusic: Mix_Init(MID) reported: %s\n",
                SDL_GetError());
        fprintf(stderr, "%s", msg);
        // Not fatal: some SDL_mixer builds load MIDI without this flag.
    }

    // Same parameters as the SFX device: 11025 Hz, signed 16-bit, stereo.
    // SDL2 allows several audio devices per process, so music and the
    // SFX mixer coexist.
    if (Mix_OpenAudio(SAMPLERATE, AUDIO_S16SYS, 2, 512) < 0)
    {
        fprintf(stderr, "I_InitMusic: Mix_OpenAudio failed: %s\n",
                Mix_GetError());
        return;
    }

    Mix_VolumeMusic(0);     // silent until S_SetMusicVolume says otherwise
    sdl_music_open = true;
}

void I_ShutdownMusic (void)
{
    int i;

    if (!sdl_music_open)
        return;

    Mix_HaltMusic();
    for (i=0 ; i<SDL_MAX_SONGS ; ++i)
    {
        if (sdl_songs[i].inuse)
        {
            if (sdl_songs[i].mus)
                Mix_FreeMusic(sdl_songs[i].mus);
            free(sdl_songs[i].midibuf);
            sdl_songs[i].inuse = 0;
        }
    }
    sdl_cur_song = 0;
    Mix_CloseAudio();
    Mix_Quit();
    sdl_music_open = false;
}

void I_SetMusicVolume (int volume)
{
    // Menu values are 0..15 (S_SetMusicVolume also passes 127 first as
    // a DMX-era quirk; clamping turns that into full volume, and the
    // follow-up call with the real value always wins).
    if (volume < 0)  volume = 0;
    if (volume > 15) volume = 15;

    if (sdl_music_open)
        Mix_VolumeMusic(volume * SDL_MIX_MAXVOLUME / 15);
}

// NOP: only meaningful while a song is registered.
void I_PauseSong (int handle)
{
    (void)handle;
    if (sdl_music_open && sdl_cur_song)
        Mix_PauseMusic();
}

// NOP.
void I_ResumeSong (int handle)
{
    (void)handle;
    if (sdl_music_open && sdl_cur_song)
        Mix_ResumeMusic();
}

//
// Convert the raw MUS lump to type-0 MIDI in memory and load it into
// SDL_mixer. Returns a positive handle, or 0 on failure (no music).
//
int I_RegisterSong (void* data, int length)
{
    membuf_t out;
    byte*    midi = 0;
    int      i;
    int      slot = 0;

    if (!sdl_music_open)
        I_InitMusic();
    if (!sdl_music_open)
        return 0;

    // Find a free slot; songs are short-lived, 8 is far more than enough.
    for (i=0 ; i<SDL_MAX_SONGS ; ++i)
    {
        if (!sdl_songs[i].inuse)
        {
            slot = i;
            break;
        }
    }
    if (i == SDL_MAX_SONGS)
    {
        fprintf(stderr, "I_RegisterSong: no free song slot\n");
        return 0;
    }

    out.cap = 2 * 1024 * 1024;
    out.data = (byte*)malloc(out.cap);
    out.len = 0;
    out.overflow = 0;
    if (!out.data)
        return 0;

    if (mus2mid((const byte*)data, length, &out) != 0)
    {
        free(out.data);
        fprintf(stderr, "I_RegisterSong: bad MUS lump, no music\n");
        return 0;
    }

    // Shrink to the exact converted size; the buffer must outlive the
    // Mix_Music, so it is kept in the slot.
    midi = (byte*)malloc(out.len + 1);
    if (!midi)
    {
        free(out.data);
        return 0;
    }
    memcpy(midi, out.data, out.len);
    free(out.data);
    out.data = 0;

    sdl_songs[slot].midibuf = midi;

    // Hand the in-memory MIDI straight to SDL_mixer (freesrc=1 disposes
    // of the RWops wrapper, data ownership stays with us).
    {
        SDL_RWops* src = SDL_RWFromConstMem(midi, (int)out.len);
        if (src)
        {
            sdl_songs[slot].mus = Mix_LoadMUS_RW(src, 1);
        }
    }
    if (!sdl_songs[slot].mus)
    {
        fprintf(stderr, "I_RegisterSong: Mix_LoadMUS failed: %s\n",
                Mix_GetError());
        fprintf(stderr, "  (Is your SDL2_mixer built with MIDI support? "
                        "Debian: libsdl2-mixer-2.0-0 ships it)\n");
        free(sdl_songs[slot].midibuf);
        sdl_songs[slot].midibuf = 0;
        return 0;
    }

    sdl_songs[slot].inuse = 1;
    return slot + 1;        // 1-based handle
}

void I_PlaySong (int handle, int looping)
{
    if (!sdl_music_open || handle <= 0 || handle > SDL_MAX_SONGS)
        return;
    if (!sdl_songs[handle-1].inuse || !sdl_songs[handle-1].mus)
        return;

    // -1 makes SDL_mixer loop the track forever.
    if (Mix_PlayMusic(sdl_songs[handle-1].mus, looping ? -1 : 1) == 0)
        sdl_cur_song = handle;
    else
    {
        fprintf(stderr, "I_PlaySong: Mix_PlayMusic failed: %s\n",
                Mix_GetError());
        sdl_cur_song = 0;
    }
}

void I_StopSong(int handle)
{
    (void)handle;
    if (!sdl_music_open)
        return;

    Mix_HaltMusic();
    sdl_cur_song = 0;
}

void I_UnRegisterSong(int handle)
{
    int slot;

    if (!sdl_music_open || handle <= 0 || handle > SDL_MAX_SONGS)
        return;

    slot = handle - 1;
    if (!sdl_songs[slot].inuse)
        return;

    if (sdl_cur_song == handle)
    {
        Mix_HaltMusic();
        sdl_cur_song = 0;
    }
    if (sdl_songs[slot].mus)
        Mix_FreeMusic(sdl_songs[slot].mus);
    free(sdl_songs[slot].midibuf);
    sdl_songs[slot].mus = 0;
    sdl_songs[slot].midibuf = 0;
    sdl_songs[slot].inuse = 0;
}
