/*
 * mixer.c - software mixer implementing the sample / stream part of the
 * FFE sound API (ffeapi.h).  Behaviour follows the original Win32
 * DirectSound implementation (16 channels, fixed voices, pitch & volume).
 *
 * Samples are 16-bit signed mono .raw files at 22050 Hz.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mixer.h"
#include "../../ffeapi.h"

typedef struct {
	const char *name;
	int loopcount;
	int pitchmod;
	int voice;
} FFESample;

#define NUMSAMPLES 0x28
#define NUMCHANNELS 16

static const FFESample sampledesc[NUMSAMPLES] = {
	{ "nowt", 0, 0, -1 }, { "nowt", 0, 0, -1 }, { "nowt", 0, 0, -1 }, { "nowt", 0, 0, -1 },
	{ "nowt", 0, 0, -1 }, { "shipexp", 0, 0, -1 }, { "missexpl", 0, 0, -1 }, { "nowt", 0, 0, -1 },
	{ "nowt", 0, 0, -1 }, { "nowt", 0, 0, -1 }, { "laser1", 0, 0, -1 }, { "laser2", 0, 0, -1 },
	{ "airopen", 0, 0, -1 }, { "airopen", 0, 0, -1 }, { "wind", 0, -0x4000, 0 }, { "station", 0x32, 0, 1 },
	{ "station", 0x32, 0x1000, 2 }, { "nowt", 0, 0, -1 }, { "airopen", 0, 0, -1 }, { "airopen", 0, 0, -1 },
	{ "bip", 0, 0, -1 }, { "bip", 0, 0, -1 }, { "bong", 0, 0, -1 }, { "chime", 0, 0, -1 },
	{ "nowt", 0, 0, -1 }, { "ecm", 2, 0, 3 }, { "necm", 2, 0, 4 }, { "shipexp", 0, 0, -1 },
	{ "missexpl", 0, 0, -1 }, { "damage", 0, 0, -1 }, { "siren", 3, 0, 5 }, { "boom", 0, 0, 6 },
	{ "launch", 0, 0, -1 }, { "hype", 0, 0, -1 }, { "nowt", 0, 0, -1 }, { "laser3", 0, 0, -1 },
	{ "laser4", 0, 0, -1 }, { "wind", 0, -0x4000, 7 }, { "nowt", 0, 0, -1 }, { "camera", 0, 0, -1 }
};

static int16_t *sampledata[NUMSAMPLES];
static uint32_t samplelen[NUMSAMPLES];     /* in samples */

typedef struct {
	int active;          /* slot allocated (like a duplicated DS buffer) */
	int playing;
	int index;
	uint64_t pos;        /* 32.32 fixed point position */
	uint64_t step;
	int32_t gain;        /* 0..65536 */
} Channel;

static Channel chans[NUMCHANNELS];
static int usedchannels;
static int mixer_ok;

/* stream (AVI soundtrack): 16-bit mono 22050 Hz */
static int16_t *streambuf;
static uint32_t streamcap, streamwrite, streamblocks;
static uint32_t blockend[1000];
static int stream_on, stream_playing;
static uint64_t streampos;   /* 32.32 in samples */

static int LoadSample(int index)
{
	char path[512];
	FILE *f;
	long size;
	int loops = sampledesc[index].loopcount ? sampledesc[index].loopcount : 1, i;
	uint8_t *data;

	DirMakeSampleName(path, (char *)sampledesc[index].name);
	strcat(path, ".raw");
	f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 1) { fclose(f); return 0; }
	size &= ~1L;
	data = (uint8_t *)malloc((size_t)size * (size_t)loops);
	if (!data) { fclose(f); return 0; }
	if (fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return 0; }
	fclose(f);
	for (i = 1; i < loops; i++) memcpy(data + (size_t)i * (size_t)size, data, (size_t)size);
	/* .raw is little-endian 16-bit; convert if host is big-endian */
	{
		uint16_t probe = 1;
		if (*(uint8_t *)&probe == 0) {
			long k;
			for (k = 0; k < size * loops; k += 2) { uint8_t t = data[k]; data[k] = data[k + 1]; data[k + 1] = t; }
		}
	}
	sampledata[index] = (int16_t *)data;
	samplelen[index] = (uint32_t)(size / 2 * loops);
	return 1;
}

int MixerInit(int audio_available)
{
	int i;
	memset(chans, 0, sizeof chans);
	usedchannels = 0;
	mixer_ok = audio_available;
	if (!audio_available) return 0;
	for (i = 0; i < NUMSAMPLES; i++) LoadSample(i);
	return 1;
}

void MixerCleanup(void)
{
	int i;
	PlatformAudioLock();
	mixer_ok = 0;
	memset(chans, 0, sizeof chans);
	stream_on = stream_playing = 0;
	free(streambuf); streambuf = NULL;
	PlatformAudioUnlock();
	for (i = 0; i < NUMSAMPLES; i++) { free(sampledata[i]); sampledata[i] = NULL; samplelen[i] = 0; }
}

static int32_t clamp16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

void MixerRender(int16_t *out, int frames)
{
	int i, c;
	PlatformAudioLock();
	for (i = 0; i < frames; i++) {
		int32_t acc = 0;
		for (c = 0; c < NUMCHANNELS; c++) {
			Channel *ch = &chans[c];
			uint32_t p;
			if (!ch->active || !ch->playing) continue;
			p = (uint32_t)(ch->pos >> 32);
			if (p >= samplelen[ch->index]) { ch->playing = 0; continue; }
			acc += (int32_t)(((int64_t)sampledata[ch->index][p] * ch->gain) >> 16);
			ch->pos += ch->step;
		}
		if (stream_playing) {
			uint32_t p = (uint32_t)(streampos >> 32);
			if (p < streamwrite) {
				acc += streambuf[p];
				streampos += ((uint64_t)22050 << 32) / MIXER_RATE;
			}
		}
		acc = clamp16(acc);
		out[2 * i] = (int16_t)acc;
		out[2 * i + 1] = (int16_t)acc;
	}
	PlatformAudioUnlock();
}

/* ---- ffeapi.h sound functions ---- */

void SoundCheckInit(int32_t *pAll, int32_t *pDigi, int32_t *pMidi)
{
	*pDigi = *pAll = mixer_ok ? 1 : 0;
	*pMidi = PlatformMusicAvailable() ? 1 : 0;
}

void SoundStopAllSamples(void)
{
	PlatformAudioLock();
	memset(chans, 0, sizeof chans);
	usedchannels = 0;
	PlatformAudioUnlock();
}

static void start_channel(Channel *ch, int index, int32_t gain, int32_t pitch)
{
	ch->active = 1;
	ch->playing = 1;
	ch->index = index;
	ch->pos = 0;
	ch->gain = gain;
	ch->step = ((uint64_t)(uint32_t)pitch << 32) / MIXER_RATE;
}

void SoundPlaySample(int32_t index, int32_t vol, int32_t pitch)
{
	int fixednum, i;
	int32_t gain;

	if (!mixer_ok) return;
	if (index < 0 || index >= NUMSAMPLES) return;
	if (!sampledata[index]) return;

	if (pitch == -1) pitch = 22050;
	pitch += sampledesc[index].pitchmod;
	if (pitch > 100000) pitch = 100000;
	if (pitch < 100) pitch = 100;
	fixednum = sampledesc[index].voice;
	/* original volume: linear, ~32768 = full volume */
	if (vol < 0) vol = 0;
	gain = (int32_t)(((int64_t)vol * 65536) / 32768);
	if (gain > 65536) gain = 65536;

	PlatformAudioLock();
	/* free one stopped channel if all are used */
	if (usedchannels == NUMCHANNELS) {
		for (i = 0; i < NUMCHANNELS; i++) {
			if (!chans[i].active || chans[i].playing) continue;
			chans[i].active = 0;
			usedchannels--;
			break;
		}
	}
	/* channel already used by this sample? */
	for (i = 0; i < NUMCHANNELS; i++) {
		Channel *ch = &chans[i];
		if (!ch->active || ch->index != index) continue;
		if (!ch->playing) {
			start_channel(ch, index, gain, pitch);
			PlatformAudioUnlock();
			return;
		}
		if (index == 16) {
			ch->gain = gain;
			ch->step = ((uint64_t)(uint32_t)pitch << 32) / MIXER_RATE;
			PlatformAudioUnlock();
			return;
		}
		else if (fixednum != -1) { PlatformAudioUnlock(); return; }
	}
	for (i = 0; i < NUMCHANNELS; i++) if (!chans[i].active) break;
	if (i == NUMCHANNELS) {
		for (i = 0; i < NUMCHANNELS; i++) {
			if (chans[i].index != 0xb) continue;
			chans[i].active = 0;
			usedchannels--;
			break;
		}
	}
	if (i < NUMCHANNELS) {
		start_channel(&chans[i], index, gain, pitch);
		usedchannels++;
	}
	PlatformAudioUnlock();
}

void SoundStreamReset(void)
{
	if (!mixer_ok) return;
	SoundStreamStop();
	PlatformAudioLock();
	streamcap = 22050 * 20;     /* 20 seconds, like the original */
	streambuf = (int16_t *)calloc(streamcap, sizeof(int16_t));
	streamwrite = 0;
	streamblocks = 0;
	streampos = 0;
	stream_on = streambuf != NULL;
	stream_playing = 0;
	PlatformAudioUnlock();
}

void SoundStreamAddBlock(void *pData, int32_t size)
{
	uint32_t n;
	if (!stream_on || size <= 0) return;
	PlatformAudioLock();
	n = (uint32_t)size / 2;
	if (streamwrite + n > streamcap) n = streamcap - streamwrite;
	memcpy(streambuf + streamwrite, pData, n * 2);
	streamwrite += n;
	if (streamblocks < 1000) blockend[streamblocks++] = streamwrite;
	PlatformAudioUnlock();
}

int32_t SoundStreamGetUsedBlocks(void)
{
	uint32_t pos, n = 0;
	if (!stream_on || !streamblocks) return 0;
	PlatformAudioLock();
	pos = (uint32_t)(streampos >> 32);
	while (n < streamblocks && blockend[n] <= pos) n++;
	PlatformAudioUnlock();
	return (int32_t)n;
}

void SoundStreamStart(void)
{
	if (!stream_on) return;
	PlatformAudioLock();
	stream_playing = 1;
	PlatformAudioUnlock();
}

void SoundStreamWait(void)
{
	if (!stream_on) return;
	for (;;) {
		uint32_t pos;
		PlatformAudioLock();
		pos = (uint32_t)(streampos >> 32);
		PlatformAudioUnlock();
		if (!stream_playing || pos >= streamwrite) break;
		TimerSleep();
	}
	SoundStreamStop();
}

void SoundStreamStop(void)
{
	PlatformAudioLock();
	stream_on = stream_playing = 0;
	free(streambuf);
	streambuf = NULL;
	streamwrite = streamblocks = 0;
	PlatformAudioUnlock();
}
