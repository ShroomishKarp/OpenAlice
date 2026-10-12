/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.
===========================================================================
*/
/*
** SDL_SND.C
**
** SDL3 replacement for unix/linux_snd.c (OSS).
**
** The engine mixes into a ring buffer (dma.buffer) and asks where the
** "hardware" is currently reading (SNDDMA_GetDMAPos). Here an SDL audio
** stream callback plays the part of the hardware: whenever the device
** wants more data it copies the next chunk out of the ring buffer and
** advances the read position.
*/

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "../game/q_shared.h"
#include "../client/snd_local.h"

static SDL_AudioStream *audio_stream = NULL;
static SDL_AtomicInt   dma_read_pos; // in bytes
static int             dma_size;     // in bytes
static qboolean        snd_inited = qfalse;

void Snd_Memset( void *dest, const int val, const size_t count )
{
	Com_Memset( dest, val, count );
}

static void SDLCALL AudioCallback( void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount )
{
	int pos = SDL_GetAtomicInt( &dma_read_pos );

	while( additional_amount > 0 )
	{
		int len = dma_size - pos;

		if( len > additional_amount )
			len = additional_amount;

		SDL_PutAudioStreamData( stream, dma.buffer + pos, len );

		pos += len;
		additional_amount -= len;
		if( pos >= dma_size )
			pos = 0;
	}

	SDL_SetAtomicInt( &dma_read_pos, pos );
}

qboolean SNDDMA_Init( void )
{
	SDL_AudioSpec spec;
	cvar_t        *sndspeed;
	int           speed, samples;

	if( snd_inited )
		return qtrue;

	if( !SDL_InitSubSystem( SDL_INIT_AUDIO ))
	{
		Com_Printf( "SDL audio init failed: %s\n", SDL_GetError());
		return qfalse;
	}

	sndspeed = Cvar_Get( "sndspeed", "0", CVAR_ARCHIVE );
	speed = sndspeed->integer;
	if( !speed )
	{
		// same convention as the other platforms: s_khz picks the rate
		speed = s_khz->integer == 44 ? 44100 : ( s_khz->integer == 11 ? 11025 : 22050 );
	}

	SDL_zero( spec );
	spec.format = SDL_AUDIO_S16;
	spec.channels = 2;
	spec.freq = speed;

	audio_stream = SDL_OpenAudioDeviceStream( SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, NULL );
	if( !audio_stream )
	{
		Com_Printf( "SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
		SDL_QuitSubSystem( SDL_INIT_AUDIO );
		return qfalse;
	}

	// the mixer wants a power of two number of samples (about 0.3 s)
	samples = 512;
	while( samples < speed * spec.channels * 3 / 10 )
		samples <<= 1;

	Com_Memset( &dma, 0, sizeof( dma ));
	dma.channels = spec.channels;
	dma.samplebits = 16;
	dma.speed = speed;
	dma.samples = samples;
	dma.submission_chunk = 1;

	dma_size = dma.samples * ( dma.samplebits / 8 );
	dma.buffer = calloc( 1, dma_size );
	if( !dma.buffer )
	{
		SDL_DestroyAudioStream( audio_stream );
		audio_stream = NULL;
		SDL_QuitSubSystem( SDL_INIT_AUDIO );
		return qfalse;
	}

	SDL_SetAtomicInt( &dma_read_pos, 0 );
	SDL_ResumeAudioStreamDevice( audio_stream );

	Com_Printf( "SDL audio: %d Hz, %d channels, 16 bit, %d byte buffer\n", dma.speed, dma.channels, dma_size );

	snd_inited = qtrue;
	return qtrue;
}

int SNDDMA_GetDMAPos( void )
{
	if( !snd_inited )
		return 0;
	return SDL_GetAtomicInt( &dma_read_pos ) / ( dma.samplebits / 8 );
}

void SNDDMA_Shutdown( void )
{
	if( !snd_inited )
		return;

	if( audio_stream )
	{
		SDL_DestroyAudioStream( audio_stream ); // also closes the device
		audio_stream = NULL;
	}
	SDL_QuitSubSystem( SDL_INIT_AUDIO );

	free( dma.buffer );
	dma.buffer = NULL;
	snd_inited = qfalse;
}

void SNDDMA_BeginPainting( void )
{
}

void SNDDMA_Submit( void )
{
}
