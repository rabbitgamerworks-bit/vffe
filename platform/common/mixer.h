/*
 * mixer.h - portable software mixer for FFE sound effects and the video
 * soundtrack stream.  Replaces the DirectSound code of the 32-bit build.
 *
 * The platform calls MixerRender() from its audio callback/thread and must
 * provide PlatformAudioLock()/PlatformAudioUnlock().
 */
#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIXER_RATE 44100

/* Loads the samples (fx/<name>.raw).  Returns 1 if the mixer is usable. */
int MixerInit(int audio_available);
void MixerCleanup(void);

/* Fill 'frames' stereo frames (interleaved int16 L,R) */
void MixerRender(int16_t *out, int frames);

/* Provided by the platform */
void PlatformAudioLock(void);
void PlatformAudioUnlock(void);
int PlatformMusicAvailable(void);

#ifdef __cplusplus
}
#endif

#endif
