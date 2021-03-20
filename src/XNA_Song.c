/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2021 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#ifndef DISABLE_XNASONG

#include "FAudio_internal.h"

/* stb_vorbis */

#define malloc FAudio_malloc
#define realloc FAudio_realloc
#define free FAudio_free
#ifdef memset /* Thanks, Apple! */
#undef memset
#endif
#define memset FAudio_memset
#ifdef memcpy /* Thanks, Apple! */
#undef memcpy
#endif
#define memcpy FAudio_memcpy
#define memcmp FAudio_memcmp

#define pow FAudio_pow
#define log(x) FAudio_log(x)
#define sin(x) FAudio_sin(x)
#define cos(x) FAudio_cos(x)
#define floor FAudio_floor
#define abs(x) FAudio_abs(x)
#define ldexp(v, e) FAudio_ldexp((v), (e))
#define exp(x) FAudio_exp(x)

#define qsort FAudio_qsort

#define assert FAudio_assert

#define FILE FAudioIOStream
#ifdef SEEK_SET
#undef SEEK_SET
#endif
#ifdef SEEK_END
#undef SEEK_END
#endif
#ifdef EOF
#undef EOF
#endif
#define SEEK_SET FAUDIO_SEEK_SET
#define SEEK_END FAUDIO_SEEK_END
#define EOF FAUDIO_EOF
#define fopen(path, mode) FAudio_fopen(path)
#define fopen_s(io, path, mode) (!(*io = FAudio_fopen(path)))
#define fclose(io) FAudio_close(io)
#define fread(dst, size, count, io) io->read(io->data, dst, size, count)
#define fseek(io, offset, whence) io->seek(io->data, offset, whence)
#define ftell(io) io->seek(io->data, 0, FAUDIO_SEEK_CUR)

#define STB_VORBIS_NO_PUSHDATA_API 1
#define STB_VORBIS_NO_INTEGER_CONVERSION 1
#include "stb_vorbis.h"

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#endif /* HAVE_GSTREAMER */

/* Globals */

static float songVolume = 1.0f;
static FAudio *songAudio = NULL;
static FAudioMasteringVoice *songMaster = NULL;

static FAudioSourceVoice *songVoice = NULL;
static FAudioVoiceCallback callbacks;

/* How nice of stb_vorbis to define a struct that we can use for all formats... */
static stb_vorbis_info activeVorbisInfo;

static stb_vorbis *activeVorbis = NULL;

#ifdef HAVE_GSTREAMER
typedef struct XNASongGSTREAMER {
	GstPad* srcpad;
	GstElement* pipeline;
	GstElement* dst;
	GstElement* resampler;
	GstSegment segment;
	uint8_t* convertCache, * prevConvertCache;
	size_t convertCacheLen, prevConvertCacheLen;
	uint32_t curBlock, prevBlock;
	size_t* blockSizes;
	uint32_t blockAlign;
	uint32_t blockCount;
	size_t maxBytes;
} XNASongGSTREAMER;

static XNASongGSTREAMER *activeGStreamer;
#endif /* HAVE_GSTREAMER */

static uint8_t *songCache;

/* Internal Functions */

static void XNA_SongSubmitBuffer(FAudioVoiceCallback *callback, void *pBufferContext)
{
	FAudioBuffer buffer;
	uint32_t decoded = stb_vorbis_get_samples_float_interleaved(
		activeVorbis,
		activeVorbisInfo.channels,
		(float*) songCache,
		activeVorbisInfo.sample_rate * activeVorbisInfo.channels
	);
	if (decoded == 0)
	{
		return;
	}
	buffer.Flags = (decoded < activeVorbisInfo.sample_rate) ?
		FAUDIO_END_OF_STREAM :
		0;
	buffer.AudioBytes = decoded * activeVorbisInfo.channels * sizeof(float);
	buffer.pAudioData = songCache;
	buffer.PlayBegin = 0;
	buffer.PlayLength = decoded;
	buffer.LoopBegin = 0;
	buffer.LoopLength = 0;
	buffer.LoopCount = 0;
	buffer.pContext = NULL;
	FAudioSourceVoice_SubmitSourceBuffer(
		songVoice,
		&buffer,
		NULL
	);
}

static void XNA_SongKill()
{
	if (songVoice != NULL)
	{
#ifdef HAVE_GSTREAMER
		if (songVoice->src.gstreamer)
		{
			FAudio_GSTREAMER_free(songVoice);
		}
#endif /* HAVE_GSTREAMER */
		FAudioSourceVoice_Stop(songVoice, 0, 0);
		FAudioVoice_DestroyVoice(songVoice);
		songVoice = NULL;
	}
	if (songCache != NULL)
	{
		FAudio_free(songCache);
		songCache = NULL;
	}
	if (activeVorbis != NULL)
	{
		stb_vorbis_close(activeVorbis);
		activeVorbis = NULL;
	}
}

/* "Public" API */

FAUDIOAPI void XNA_SongInit()
{
	FAudioCreate(&songAudio, 0, FAUDIO_DEFAULT_PROCESSOR);
	FAudio_CreateMasteringVoice(
		songAudio,
		&songMaster,
		FAUDIO_DEFAULT_CHANNELS,
		FAUDIO_DEFAULT_SAMPLERATE,
		0,
		0,
		NULL
	);
}

FAUDIOAPI void XNA_SongQuit()
{
	XNA_SongKill();
	FAudioVoice_DestroyVoice(songMaster);
	FAudio_Release(songAudio);
}

FAUDIOAPI float XNA_PlaySong(const char *name)
{
	FAudioWaveFormatEx format;
	size_t namelength;
	float duration;

	XNA_SongKill();

	namelength = FAudio_strlen(name);
	if (	namelength >= 4 && (
		FAudio_strcmp(name + namelength - 4, ".ogg") == 0 ||
		FAudio_strcmp(name + namelength - 4, ".oga") == 0)	)
	{
		activeVorbis = stb_vorbis_open_filename(name, NULL, NULL);

		/* Set format info */
		activeVorbisInfo = stb_vorbis_get_info(activeVorbis);
		format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
		format.nChannels = activeVorbisInfo.channels;
		format.nSamplesPerSec = activeVorbisInfo.sample_rate;
		format.wBitsPerSample = sizeof(float) * 8;
		format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		format.cbSize = 0;

		/* Init voice */
		FAudio_zero(&callbacks, sizeof(FAudioVoiceCallback));
		callbacks.OnBufferEnd = XNA_SongSubmitBuffer;
		FAudio_CreateSourceVoice(
			songAudio,
			&songVoice,
			&format,
			0,
			1.0f, /* No pitch shifting here! */
			&callbacks,
			NULL,
			NULL
		);

		/* Allocate decode cache */
		songCache = (uint8_t*) FAudio_malloc(format.nAvgBytesPerSec);

		/* Okay, this song is decoding now */
		stb_vorbis_seek_start(activeVorbis);
		XNA_SongSubmitBuffer(NULL, NULL);

		/* Finally. */
		duration = stb_vorbis_stream_length_in_seconds(activeVorbis);
	}
	else
	{
		activeVorbis = NULL;
#ifdef HAVE_GSTREAMER
		duration = FAudio_GSTREAMER_play(songAudio, &songVoice, name, songVolume);
		if (duration <= 0.0f || songVoice == NULL)
		{
			return 0.0f;
		}
#else
		FAudio_assert(0 && "Unsupported song file format!");
		return 0.0f;
#endif /* HAVE_GSTREAMER */
	}

	FAudioVoice_SetVolume(songVoice, songVolume, 0);
	FAudioSourceVoice_Start(songVoice, 0, 0);
	return duration;
}

FAUDIOAPI void XNA_PauseSong()
{
	if (songVoice == NULL)
	{
		return;
	}
	FAudioSourceVoice_Stop(songVoice, 0, 0);
}

FAUDIOAPI void XNA_ResumeSong()
{
	if (songVoice == NULL)
	{
		return;
	}
	FAudioSourceVoice_Start(songVoice, 0, 0);
}

FAUDIOAPI void XNA_StopSong()
{
	XNA_SongKill();
}

FAUDIOAPI void XNA_SetSongVolume(float volume)
{
	songVolume = volume;
	if (songVoice != NULL)
	{
		FAudioVoice_SetVolume(songVoice, songVolume, 0);
	}
}

FAUDIOAPI uint32_t XNA_GetSongEnded()
{
	FAudioVoiceState state;
	if (songVoice == NULL)
	{
		return 1;
	}
	/* FIXME: Handle starvation, waiting for new samples and GStreamer state. */
	FAudioSourceVoice_GetState(songVoice, &state, 0);
	return state.BuffersQueued == 0;
}

FAUDIOAPI void XNA_EnableVisualization(uint32_t enable)
{
	/* TODO: Enable/Disable FAPO effect */
}

FAUDIOAPI uint32_t XNA_VisualizationEnabled()
{
	/* TODO: Query FAPO effect enabled */
	return 0;
}

FAUDIOAPI void XNA_GetSongVisualizationData(
	float *frequencies,
	float *samples,
	uint32_t count
) {
	/* TODO: Visualization FAPO that reads in Song samples, FFT analysis */
}

#endif /* DISABLE_XNASONG */

/* vim: set noexpandtab shiftwidth=8 tabstop=8: */
