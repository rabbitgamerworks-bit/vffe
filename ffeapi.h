#ifndef __FFEAPI__H__
#define __FFEAPI__H__

#ifdef __cplusplus
extern "C" {
#endif


#ifdef __FFEWIN64__
#define __CONFIGFILE__ "ffewin64.cfg"
#endif
#ifdef __FFESDL2__
#define __CONFIGFILE__ "ffesdl2.cfg"
#endif

#ifndef __CONFIGFILE__
#error Unknown version: Config file not set
#endif

/* Fixed-size types: the recompiled core passes 32-bit values, and 'long'
 * is 64-bit on LP64 hosts (Linux) but 32-bit on Windows x64. */
#include <stdint.h>

#ifndef _WINDEF_
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
#endif
typedef uint32_t FFE_ULONG;

void SystemInit (void);
void SystemCleanup (void);

void VideoBlit (uint8_t *pData, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump);
void VideoMaskedBlit (uint8_t *pData, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump);
void VideoReverseBlit (uint8_t *pData, int32_t x, int32_t y, int32_t w, int32_t h, int32_t jump);
void VideoGetPalValue (int32_t palindex, uint8_t *pVal);
void VideoSetPalValue (int32_t palindex, uint8_t *pVal);
int32_t VideoPointerExclusive (void);
void VideoPointerEnable (void);
void VideoPointerDisable (void);

int32_t InputMouseReadButtons (void);
void InputMouseReadMickeys (int32_t *pXMick, int32_t *pYMick);
void InputMouseReadPos (int32_t *pXPos, int32_t *pYPos);

void InputJoyReadPos (int32_t *pXPos, int32_t *pYPos);
int32_t InputJoyReadButtons (void);

void InputKeybReadStates (uint8_t *pKeyArray);
int32_t InputKeybGetLastKey (void);
void InputKeybSetLastKey (int32_t);

uint32_t TimerGetTimeStamp (void);
void TimerSleep (void);
void TimerFrameUpdate (void);

void SoundCheckInit (int32_t *pAll, int32_t *pDigi, int32_t *pMidi);
void SoundPlaySong (int32_t index);
void SoundStopSong (void);
int32_t SoundSongDone (void);
void SoundStopAllSamples (void);
void SoundPlaySample (int32_t index, int32_t vol, int32_t pitch);

int32_t SoundStreamGetUsedBlocks (void);
void SoundStreamReset (void);
void SoundStreamAddBlock (void *pData, int32_t size);
void SoundStreamStart (void);
void SoundStreamWait (void);
void SoundStreamStop (void);

typedef struct {
	char pName[16];
	int type;
	int size;
} FileInfo;

char *DirMakeAVIName (char *pBuf, char *pStub);
char *DirMakeSampleName (char *pBuf, char *pFilename);
char *DirMakeSongName (char *pBuf, char *pFilename);
char *DirMakeCmmdrName (char *pBuf, char *pFilename);
char *DirGetCmmdrPath (void);
void DirResetCmmdrPath (void);
void DirNavigateTree (FileInfo *pFile);
int DirFindFirst (FileInfo *pFile);
int DirFindNext (FileInfo *pFile);


#ifdef __cplusplus
}
#endif

#endif /* __FFEAPI_H__ */
