#include "stdSound.h"

#include "Gui/jkGUISound.h"
#include "Main/Main.h"
#include "stdPlatform.h"
#include "Platform/wuRegistry.h"

#include <stdio.h>

#ifdef TARGET_IOS
#include "Platform/iOS/iosAudioSession.h"
#endif

#ifdef TARGET_ANDROID
#include <SDL.h>
#include <jni.h>
#include <stdlib.h>
#endif

#include "jk.h"

flex_t stdSound_fMenuVolume = 1.0f;

#ifndef TARGET_TWL
uint32_t stdSound_ParseWav(stdFile_t sound_file, uint32_t *nSamplesPerSec, int32_t *bitsPerSample, int32_t *bStereo, int32_t *seekOffset)
{
    unsigned int result; // eax
    char v9[4]; // [esp+Ch] [ebp-14h] BYREF
    stdWaveFormat v10; // [esp+10h] [ebp-10h] BYREF
    uint32_t seekPos;

    std_g_pHS->fseek(sound_file, 8, 0);
    std_g_pHS->fileRead(sound_file, v9, 4);
    result = 0;
    if ( !_memcmp(v9, "WAVE", 4) )
    {
        std_g_pHS->fseek(sound_file, 4, SEEK_CUR);
        std_g_pHS->fileRead(sound_file, &seekPos, 4);
        std_g_pHS->fileRead(sound_file, &v10, sizeof(stdWaveFormat));
        *nSamplesPerSec = v10.nSamplesPerSec;
        *bitsPerSample = 8 * (v10.nBlockAlign / (int)v10.nChannels);
        *bStereo = v10.nChannels == 2;

        if (seekPos > 0x10 )
            std_g_pHS->fseek(sound_file, seekPos - 16, 1);

        // MoTS removed
        //std_g_pHS->fseek(sound_file, 4, SEEK_CUR);
        //std_g_pHS->fileRead(sound_file, &seekPos, 4);

        // MoTS added
        while (!std_g_pHS->fileEof(sound_file))
        {
            std_g_pHS->fileRead(sound_file, v9, 4);
            std_g_pHS->fileRead(sound_file, &seekPos, 4);

            if (!_memcmp(v9, "data", 4)) break;
            std_g_pHS->fseek(sound_file, seekPos, SEEK_CUR);
        }

        *seekOffset = std_g_pHS->ftell(sound_file);
        result = seekPos;
#ifdef AL_FORMAT_WAVE_EXT
        //*seekOffset = 0;
        //result += v8;
#endif
    }
    return result;
}
#endif

#ifdef TARGET_ANDROID
// Added: Android only grants a stream the low-latency FAST track when its rate
// matches the output's native rate. A mismatched stream lands on the normal
// mixer track with a ~280 ms buffer, which roughly doubles total latency and is
// very noticeable over Bluetooth. Ask the OS for that rate rather than assume it.
int stdSound_GetNativeOutputRate()
{
    static int nCached = 0;
    if (nCached) return nCached;

    int nRate = 0;
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (env && activity && (*env)->PushLocalFrame(env, 16) >= 0)
    {
        jclass clsCtx = (*env)->FindClass(env, "android/content/Context");
        jmethodID midSvc = clsCtx ? (*env)->GetMethodID(env, clsCtx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
        jobject am = midSvc ? (*env)->CallObjectMethod(env, activity, midSvc, (*env)->NewStringUTF(env, "audio")) : NULL;
        if (am && !(*env)->ExceptionCheck(env))
        {
            jclass clsAm = (*env)->GetObjectClass(env, am);
            jmethodID midProp = (*env)->GetMethodID(env, clsAm, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
            jstring strVal = midProp ? (jstring)(*env)->CallObjectMethod(env, am, midProp, (*env)->NewStringUTF(env, "android.media.property.OUTPUT_SAMPLE_RATE")) : NULL;
            if (strVal && !(*env)->ExceptionCheck(env))
            {
                const char* psz = (*env)->GetStringUTFChars(env, strVal, NULL);
                if (psz)
                {
                    nRate = atoi(psz);
                    (*env)->ReleaseStringUTFChars(env, strVal, psz);
                }
            }
        }
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->PopLocalFrame(env, NULL);
    }
    if (env && activity)
        (*env)->DeleteLocalRef(env, activity);

    if (nRate < 8000 || nRate > 192000)
    {
        stdPlatform_Printf("stdSound: OUTPUT_SAMPLE_RATE unreadable (%d), assuming 48000\n", nRate);
        nRate = 48000;
    }
    stdPlatform_Printf("stdSound: native output rate %d Hz\n", nRate);
    nCached = nRate;
    return nRate;
}
#endif

#ifdef STDSOUND_OPENAL

#define AL_FORMAT_MONO24 (0xFFFF0000)
#define AL_FORMAT_STEREO24 (0xFFFF0001)

ALCdevice *device;
ALCcontext *context;
static ALfloat stdSound_listenerPos[3] = {0.0f, 0.0f , 0.0f};
static ALfloat stdSound_listenerOri[6] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f };
static int stdSound_bListenerSent = 0; // Added: the two above are what OpenAL has

void stdSound_DS3DToAL(ALfloat* pOut, rdVector3* pIn)
{
    pOut[0] = pIn->x * 0.1;
    pOut[1] = pIn->y * 0.1;
    pOut[2] = pIn->z * 0.1;
}

void stdSound_ALToDS3D(rdVector3* pOut, ALfloat* pIn)
{
    pOut->x = pIn[0] * 10.0;
    pOut->y = pIn[1] * 10.0;
    pOut->z = pIn[2] * 10.0;
}

int stdSound_Startup()
{
    jkGuiSound_b3DSound_3 = 1;

    if (Main_bHeadless) return 1;

	ALboolean enumeration;
	const ALCchar *defaultDeviceName = NULL;
	int ret;
	char *bufferData;
	ALvoid *data;
	ALsizei size, freq;
	ALenum format;
	ALboolean loop = AL_FALSE;
	ALCenum error;
	ALint source_state;

	printf("Using OpenAL+ALUT as audio backend\n");

#ifdef TARGET_IOS
    // Added: iOS silently mutes a process that never configures an
    // AVAudioSession whenever the ring/silent switch is engaged. Must run
    // before alcOpenDevice() -- OpenAL's CoreAudio backend starts its audio unit
    // there, and starting it under the wrong category is what goes quiet.
    iosAudioSession_Initialize();
#endif

	// Added: a plain note, not an error: only the default device is ever opened
	// below, and the browser's OpenAL never has the extension.
	enumeration = alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT");
	if (enumeration == AL_FALSE)
		printf("OpenAL device enumeration not available, using the default device\n");

	if (!defaultDeviceName)
		defaultDeviceName = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);

	device = alcOpenDevice(defaultDeviceName);
	if (!device) {
		fprintf(stderr, "unable to open default device\n");
		return 0;
	}

	alGetError();

#ifdef TARGET_ANDROID
	// Added: mix at the output's native rate so the stream gets the fast track.
	ALCint aCtxAttrs[] = { ALC_FREQUENCY, stdSound_GetNativeOutputRate(), 0 };
	context = alcCreateContext(device, aCtxAttrs);
#else
	context = alcCreateContext(device, NULL);
#endif
	if (!alcMakeContextCurrent(context)) {
		fprintf(stderr, "failed to make default context\n");
		return 0;
	}

	/* set orientation */
	alListener3f(AL_POSITION, 0, 0, 1.0f);
    alListener3f(AL_VELOCITY, 0, 0, 0);
	alListenerfv(AL_ORIENTATION, stdSound_listenerOri);
    alListenerf(AL_ROLLOFF_FACTOR, 0.0f);
    alListenerf(AL_REFERENCE_DISTANCE, 5.0f);
    alListenerf(AL_MAX_GAIN, 1.0f);
    alListenerf(AL_MIN_GAIN, 0.0f);
    stdSound_bListenerSent = 0; // Added: the next listener update sends everything

    jkGuiSound_musicVolume = wuRegistry_GetFloat("musicVolume", jkGuiSound_musicVolume);
    jkGuiSound_sfxVolume = wuRegistry_GetFloat("sfxVolume", jkGuiSound_sfxVolume);
    jkGuiSound_numChannels = 256;
    jkGuiSound_bLowResSound = wuRegistry_GetBool("bLowRes", jkGuiSound_bLowResSound);
    jkGuiSound_b3DSound = wuRegistry_GetBool("b3DSound", jkGuiSound_b3DSound);
    jkGuiSound_b3DSound_2 = jkGuiSound_b3DSound;

    return 1;
}

void stdSound_Shutdown()
{
    if (Main_bHeadless) return;

	device = alcGetContextsDevice(context);
	alcMakeContextCurrent(NULL);
	alcDestroyContext(context);
	alcCloseDevice(device);
}

void stdSound_SetMenuVolume(flex_t a1)
{
    stdSound_fMenuVolume = a1;
}

stdSound_buffer_t* stdSound_BufferCreate(int bStereo, uint32_t nSamplesPerSec, uint16_t bitsPerSample, int bufferLen)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;
    
    _memset(out, 0, sizeof(*out));
    
    out->data = NULL;
    out->bStereo = bStereo;
    out->bufferLen = bufferLen;
    out->nSamplesPerSec = nSamplesPerSec;
    out->bitsPerSample = bitsPerSample;
    out->refcnt = 1;
    out->vol = 1.0;
    out->pitch = 1.0;

    out->pos[0] = 0.0;
    out->pos[1] = 0.0;
    out->pos[2] = 0.0;
    out->vel[0] = 0.0;
    out->vel[1] = 0.0;
    out->vel[2] = 0.0;
    
    if (!Main_bHeadless)
        alGenBuffers(1, &out->buffer);
    
    int format = 0;
    if (bStereo)
    {
        switch (bitsPerSample) {
            case 8:
                format = AL_FORMAT_STEREO8;
                break;
            case 16:
                format = AL_FORMAT_STEREO16;
                break;
            case 24:
                format = AL_FORMAT_STEREO24;
                break;
        }
    }
    else
    {
        switch (bitsPerSample) {
            case 8:
                format = AL_FORMAT_MONO8;
                break;
            case 16:
                format = AL_FORMAT_MONO16;
                break;
            case 24:
                format = AL_FORMAT_MONO24;
                break;
        }
    }
    
#ifdef AL_FORMAT_WAVE_EXT
    //format = AL_FORMAT_WAVE_EXT;
#endif

    out->format = format;
    
    return out;
}

void* stdSound_BufferSetData(stdSound_buffer_t* sound, int bufferBytes, int32_t* bufferMaxSize)
{
    sound->bufferBytes = bufferBytes;
    
    if (bufferMaxSize)
        *bufferMaxSize = bufferBytes;
    
    if (sound->data && !sound->bIsCopy)
        STD_FREE(sound->data);

    sound->data = STD_ALLOC(bufferBytes);
    sound->bufferBytes = bufferBytes;
    
    _memset(sound->data, 0, sound->bufferBytes);

    // Added: no alBufferData of the zeroed buffer here. Every caller fills the
    // data and then calls stdSound_BufferUnlock, which uploads it; uploading the
    // zeros first only cost a second conversion (a full float copy in the browser).

    return sound->data;
}

int stdSound_BufferUnlock(stdSound_buffer_t* sound, void* buffer, int bufferRead)
{
    if (Main_bHeadless) return 1;
    
    if (sound->format == AL_FORMAT_STEREO24 || sound->format == AL_FORMAT_MONO24)
    {
        void* tmp = STD_ALLOC(sound->bufferBytes);
        memcpy(tmp, sound->data, sound->bufferBytes);
        memset(sound->data, 0, sound->bufferBytes);

        uint8_t* tmp_8 = (uint8_t*)tmp;
        int16_t* out_16 = (int16_t*)sound->data;
        for (size_t i = 0; i < sound->bufferBytes / 3; i += 1)
        {
            uint32_t val = 0;

            val = val | *(tmp_8++);
            val = val | (*(tmp_8++) << 8);
            val = val | (*(tmp_8++) << 16);

            int32_t val_int = *(int32_t*)&val;

            val_int = val_int >> 8;
            *(out_16++) = val_int;
        }

        sound->bufferBytes = (sound->bufferBytes / 3) * 2;
        sound->format = AL_FORMAT_STEREO16;
        free(tmp);
    }

    alBufferData(sound->buffer, sound->format, sound->data, sound->bufferBytes, sound->nSamplesPerSec);

#if 0
    if (!sound->bIsCopy) {
        flex_t tmp = sound->vol;
        sound->vol = 0.0;
        stdSound_BufferPlay(sound, 0);
        sound->vol = tmp;
    }
#endif

    // Added: no AL source is made here any more. The mixer only ever plays
    // duplicates of a loaded sound, so the source made here for every loaded
    // sound sat idle until the level ended (in the browser, each one is a
    // GainNode rendered every audio block). stdSound_BufferPlay and
    // stdSound_BufferQueueAfterAnother make the source when it is first needed.

    return 1;
}

// Added: sets a buffer's volume and its source's AL gain. While a buffer has a
// source, that source's gain is sound->vol (written when the source is made
// and on every play), so an unchanged volume needs no AL call. The
// mixer sets the volume of every sound on every update, and in the browser each
// gain write is an AudioParam write.
static void stdSound_SetSourceVolume(stdSound_buffer_t* sound, flex_t vol, int bForce)
{
    flex_t newVol = vol * stdSound_fMenuVolume;
    int bChanged = bForce || newVol != sound->vol;

    sound->vol = newVol;
    if (!sound->source)
        return;

    if (bChanged)
        alSourcef(sound->source, AL_GAIN, sound->vol);

    if (vol == 0.0)
    {
        alSourcei(sound->source, AL_LOOPING, AL_FALSE);
        alSourceStop(sound->source);
    }
}

// Added: the AL_PITCH a new source gets. 0 is a buffer whose pitch was never
// set (OpenAL needs a pitch above 0), so it plays at the normal rate.
static ALfloat stdSound_SourcePitch(stdSound_buffer_t* buf)
{
    return buf->pitch > 0.0 ? buf->pitch : 1.0;
}

int stdSound_BufferPlay(stdSound_buffer_t* buf, int loop)
{
    ALint source_type;

    if (Main_bHeadless) return 1;

    //alSourceStop(buf->source);
    
    if (!buf->source)
    {
        alGenSources((ALuint)1, &buf->source);

        // Added: the source starts at the buffer's pitch, which may have been
        // set before it had a source (see stdSound_BufferSetFrequency).
	    alSourcef(buf->source, AL_PITCH, stdSound_SourcePitch(buf));
        if (buf->bHasPos) {
            alSourcefv(buf->source, AL_POSITION, buf->pos);
        }
        if (buf->bHasVel) {
            alSourcefv(buf->source, AL_VELOCITY, buf->vel);
        }
        if (!buf->bHasPos) {
            alSourcei(buf->source, AL_SOURCE_RELATIVE, AL_TRUE); // No 3D until we're given a position
        }
	    
	    //printf("%u %u\n", buf->source, buf->buffer);

        // Changed: a new source gets the buffer's volume as it is. buf->vol is
        // already scaled by the SFX volume (BufferCreate, BufferSetVolume), and
        // passing it through stdSound_BufferSetVolume again scaled it twice: a
        // sound started at the square of the SFX volume until something set its
        // volume again.
        alSourcef(buf->source, AL_GAIN, buf->vol);
	}

    alGetSourcei(buf->source, AL_SOURCE_TYPE, &source_type);
    if (source_type != AL_STREAMING) {
        alSourcei(buf->source, AL_BUFFER, buf->buffer);
        alSourcei(buf->source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
        alSourcef(buf->source, AL_GAIN, buf->vol);
    }
    else {
        alSourcei(buf->source, AL_BUFFER, 0);
    }
    
	alSourcePlay(buf->source);
    return 1;
}

// Added
int stdSound_BufferQueueAfterAnother(stdSound_buffer_t* bufPrev, stdSound_buffer_t* bufNext)
{
    ALint source_state = 0;
    ALint processed = 0;
    ALint source_type = AL_UNDETERMINED;
    if (Main_bHeadless) return 1;

    if (!bufPrev->source)
    {
        alGenSources((ALuint)1, &bufPrev->source);

        alSourcef(bufPrev->source, AL_PITCH, stdSound_SourcePitch(bufPrev));
        if (bufPrev->bHasPos) {
            alSourcefv(bufPrev->source, AL_POSITION, bufPrev->pos);
        }
        if (bufPrev->bHasVel) {
            alSourcefv(bufPrev->source, AL_VELOCITY, bufPrev->vel);
        }
        if (!bufPrev->bHasPos) {
            alSourcei(bufPrev->source, AL_SOURCE_RELATIVE, AL_TRUE); // No 3D until we're given a position
        }
        
        alSourcef(bufPrev->source, AL_GAIN, bufPrev->vol); // Changed: as in stdSound_BufferPlay
    }

    alGetSourcei(bufPrev->source, AL_SOURCE_TYPE, &source_type);
    if (source_type == AL_STATIC)
    {
        printf("OPENAL SOUND IS STATIC SOMEHOW\n");
        stdSound_BufferReset(bufPrev);
    }

    alGetSourcei(bufPrev->source, AL_BUFFERS_PROCESSED, &processed);
    //printf("processed %d\n", processed);
    while (processed) {
        ALuint unused;
        alSourceUnqueueBuffers(bufPrev->source, 1, &unused);
        processed--;
    }
    alGetSourcei(bufPrev->source, AL_BUFFERS_QUEUED, &processed);
    //printf("queued %d\n", processed);

    alSourcei(bufPrev->source, AL_BUFFER, 0);
    alSourcei(bufPrev->source, AL_LOOPING, AL_FALSE);
    alSourceQueueBuffers(bufPrev->source, 1, &bufNext->buffer);

    alGetSourcei(bufPrev->source, AL_SOURCE_STATE, &source_state);
    if (source_state != AL_PLAYING) {
        alSourcePlay(bufPrev->source);
    }

    return 1;
}

// Added: detach every already-played buffer from buf's source queue
void stdSound_BufferUnqueueProcessed(stdSound_buffer_t* buf)
{
    ALint processed = 0;
    if (Main_bHeadless) return;
    if (!buf || !buf->source) return;

    alGetSourcei(buf->source, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0) {
        ALuint unused;
        alSourceUnqueueBuffers(buf->source, 1, &unused);
        processed--;
    }
}

void stdSound_BufferRelease(stdSound_buffer_t* sound)
{
    ALint source_state;
    
    if (Main_bHeadless)
        goto end;

    //sound->refcnt--;
    //if (sound->refcnt > 0)
    //    return;
    
    if (sound->source) {
        alSourcei(sound->source, AL_LOOPING, AL_FALSE);
    }
    //alSourceStop(sound->source);
    
    /*alGetSourcei(sound->source, AL_SOURCE_STATE, &source_state);
    while (source_state == AL_PLAYING) {
		alGetSourcei(sound->source, AL_SOURCE_STATE, &source_state);
	}*/
	
	if (sound->source)
	{
	    //printf("del %u\n", sound->source);
    	alDeleteSources(1, &sound->source);
    }
	
	if (!sound->bIsCopy && sound->buffer)
	    alDeleteBuffers(1, &sound->buffer);

end:
	sound->source = 0;
	sound->buffer = 0;
	
	if (sound->data && !sound->bIsCopy)
	    STD_FREE(sound->data);

    memset(sound, 0, sizeof(*sound));
	STD_FREE(sound);
}

int stdSound_BufferReset(stdSound_buffer_t* sound)
{
    if (Main_bHeadless) return 1;

    //alSourcef(sound->source, AL_PITCH, 1.0);
	//alSourcef(sound->source, AL_GAIN, 1.0);
	//alSource3f(sound->source, AL_POSITION, 0, 0, 0);
	//alSource3f(sound->source, AL_VELOCITY, 0, 0, 0);

    sound->bHasPos = 0;
    sound->bHasVel = 0;
	
	if (sound->source)
	{
	    alSourcei(sound->source, AL_LOOPING, AL_FALSE);
	
	    alSourceStop(sound->source);
	    
	    //printf("del %u\n", sound->source);
	    alDeleteSources(1, &sound->source);
	    sound->source = 0;
	}
	
    return 1;
}

void stdSound_BufferSetPan(stdSound_buffer_t* a1, flex_t a2)
{
    
}

void stdSound_BufferSetFrequency(stdSound_buffer_t* sound, int freq)
{
    if (Main_bHeadless) return;

    flex_t pitch = (flex_d_t)freq / (flex_d_t)sound->nSamplesPerSec;

    // Added: keep the pitch even when there is no source yet, so the source
    // starts at it (DirectSound kept a buffer's frequency the same way).
    sound->pitch = pitch;
    if (sound->source) {
        alSourcef(sound->source, AL_PITCH, pitch);
    }
}

stdSound_buffer_t* stdSound_BufferDuplicate(stdSound_buffer_t* sound)
{
#if 1
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;
    
    _memset(out, 0, sizeof(*out));
    
    out->data = sound->data;
    out->bStereo = sound->bStereo;
    out->bufferLen = sound->bufferLen;
    out->nSamplesPerSec = sound->nSamplesPerSec;
    out->bitsPerSample = sound->bitsPerSample;
    out->refcnt = 1;
    out->vol = sound->vol;
    out->pitch = sound->pitch;
    out->format = sound->format;
    out->bufferBytes = sound->bufferBytes;
    out->bIsCopy = 1;
    out->buffer = sound->buffer;

    memcpy(out->pos, sound->pos, sizeof(out->pos));
    memcpy(out->vel, sound->vel, sizeof(out->pos));
    
    //stdSound_BufferSetData(out, sound->bufferBytes, NULL);
    
    //_memcpy(out->data, sound->data, out->bufferBytes);
    
    //stdSound_BufferUnlock(out, out->data, out->bufferBytes);
    
    //printf("%u %u\n", out->source, out->buffer);
    
    return out;
#endif
#if 0
    sound->refcnt++;
    return sound;
#endif
}

void stdSound_IA3D_idk(flex_t a)
{
}

int stdSound_BufferStop(stdSound_buffer_t* buf)
{
    if (Main_bHeadless) return 1;

    if (buf->source)
    {
        alSourcei(buf->source, AL_LOOPING, AL_FALSE);
        alSourcePause(buf->source);
    }
    return 1;
}

void stdSound_BufferSetVolume(stdSound_buffer_t* sound, flex_t vol)
{
    if (Main_bHeadless) return;
    if (!sound) return;

    stdSound_SetSourceVolume(sound, vol, 0);
}

int stdSound_3DSetMode(stdSound_buffer_t* a1, int a2)
{
    return 1;
}

stdSound_3dBuffer_t* stdSound_BufferQueryInterface(stdSound_buffer_t* pSoundBuffer)
{
    return pSoundBuffer;
}

void stdSound_CommitDeferredSettings()
{
}

void stdSound_SetPositionOrientation(rdVector3 *pos, rdVector3 *lvec, rdVector3 *uvec)
{
    ALfloat newPos[3];
    ALfloat newOri[6];

    if (!pos || !lvec || !uvec) return;

    stdSound_DS3DToAL(newPos, pos);

    stdSound_DS3DToAL(&newOri[0], lvec);
    stdSound_DS3DToAL(&newOri[3], uvec);

    // Added: send the listener only when it moved or turned. In the browser each
    // alListenerfv rewrites every listener AudioParam and goes over every source,
    // and the camera is often still.
    int bPosChanged = !stdSound_bListenerSent || memcmp(newPos, stdSound_listenerPos, sizeof(newPos)) != 0;
    int bOriChanged = !stdSound_bListenerSent || memcmp(newOri, stdSound_listenerOri, sizeof(newOri)) != 0;
    memcpy(stdSound_listenerPos, newPos, sizeof(newPos));
    memcpy(stdSound_listenerOri, newOri, sizeof(newOri));

    if (Main_bHeadless) return;

    if (bPosChanged)
        alListenerfv(AL_POSITION, stdSound_listenerPos);
    if (bOriChanged)
        alListenerfv(AL_ORIENTATION, stdSound_listenerOri);
    stdSound_bListenerSent = 1;
}

void stdSound_SetPosition(stdSound_buffer_t* pSoundBuf, rdVector3 *pos)
{
    ALfloat newPos[3];

    if (!pSoundBuf || !pos) return;

    stdSound_DS3DToAL(newPos, pos);

    // Added: while a buffer has a source and a position, that source is not
    // listener-relative and already holds pSoundBuf->pos (it gets both when it is
    // made, or below), so only a new position needs an AL call. The mixer sets
    // the position of every 3D sound on every update.
    int bHadPos = pSoundBuf->bHasPos;
    int bChanged = !bHadPos || memcmp(newPos, pSoundBuf->pos, sizeof(newPos)) != 0;
    memcpy(pSoundBuf->pos, newPos, sizeof(newPos));

    // Added: the position counts as known before the sound has a source, so
    // stdSound_BufferPlay places the new source here before it starts. Before,
    // a new 3D sound played from the listener's own position, and without the
    // distance loss, until the next mixer update.
    pSoundBuf->bHasPos = 1;

    if (!pSoundBuf->source)
        return;

    if (Main_bHeadless) return;

    if (!bHadPos)
        alSourcei(pSoundBuf->source, AL_SOURCE_RELATIVE, AL_FALSE);
    if (bChanged)
        alSourcefv(pSoundBuf->source, AL_POSITION, pSoundBuf->pos);
}

void stdSound_SetVelocity(stdSound_buffer_t* pSoundBuf, rdVector3 *vel)
{
    ALfloat newVel[3];

    if (!pSoundBuf || !vel) return;

    stdSound_DS3DToAL(newVel, vel);

    // Added: as in stdSound_SetPosition, only a new velocity needs an AL call.
    int bChanged = !pSoundBuf->bHasVel || memcmp(newVel, pSoundBuf->vel, sizeof(newVel)) != 0;
    memcpy(pSoundBuf->vel, newVel, sizeof(newVel));

    // Added: likewise, so a new source starts with its velocity (Doppler).
    pSoundBuf->bHasVel = 1;

    if (!pSoundBuf->source)
        return;

    if (Main_bHeadless) return;

    // A source with a position is already not relative (see above).
    if (!pSoundBuf->bHasPos)
        alSourcei(pSoundBuf->source, AL_SOURCE_RELATIVE, AL_FALSE);
    if (bChanged)
        alSourcefv(pSoundBuf->source, AL_VELOCITY, pSoundBuf->vel); // FLEXTODO
}

int stdSound_IsPlaying(stdSound_buffer_t* pSoundBuf, rdVector3 *pos)
{
    if (!pSoundBuf) return 0;

    if (pos) {
        stdSound_ALToDS3D(pos, pSoundBuf->pos);
    }
    
    if (!pSoundBuf->source)
        return 0;
    
    // Added
    if (pSoundBuf->vol == 0.0)
        return 0;

    if (Main_bHeadless) return 0;

    ALint source_state;
    alGetSourcei(pSoundBuf->source, AL_SOURCE_STATE, &source_state);
    
    return (source_state == AL_PLAYING);
}

void stdSound_3DBufferRelease(stdSound_3dBuffer_t* p3DBuffer)
{
    
}
#endif

#ifdef STDSOUND_NULL
int stdSound_Startup()
{
    jkGuiSound_b3DSound = 0;

    printf("Using STDSOUND_NULL as audio backend\n");

    return 1;
}

void stdSound_Shutdown()
{
}

void stdSound_SetMenuVolume(flex_t a1)
{
    stdSound_fMenuVolume = a1;
}

stdSound_buffer_t* stdSound_BufferCreate(int bStereo, uint32_t nSamplesPerSec, uint16_t bitsPerSample, int bufferLen)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;
    
    _memset(out, 0, sizeof(*out));
    
    out->data = NULL;
    out->bStereo = bStereo;
    out->bufferLen = bufferLen;
    out->nSamplesPerSec = nSamplesPerSec;
    out->bitsPerSample = bitsPerSample;
    out->refcnt = 1;
    out->vol = 1.0 * stdSound_fMenuVolume;
    
    out->format = 0;
    
    return out;
}

void* stdSound_BufferSetData(stdSound_buffer_t* sound, int bufferBytes, int32_t* bufferMaxSize)
{
    bufferBytes = 0x100; // HACK
    sound->bufferBytes = bufferBytes;
    
    if (bufferMaxSize)
        *bufferMaxSize = bufferBytes;
    
    if (sound->data && !sound->bIsCopy)
        STD_FREE(sound->data);

    
    sound->data = STD_ALLOC(bufferBytes);
    sound->bufferBytes = bufferBytes;
    
    _memset(sound->data, 0, sound->bufferBytes);

    return sound->data;
}

int stdSound_BufferUnlock(stdSound_buffer_t* sound, void* buffer, int bufferRead)
{
    return 1;
}

int stdSound_BufferPlay(stdSound_buffer_t* buf, int loop)
{
    return 1;
}

int stdSound_BufferQueueAfterAnother(stdSound_buffer_t* bufPrev, stdSound_buffer_t* bufNext)
{
    return 1;
}

void stdSound_BufferUnqueueProcessed(stdSound_buffer_t* buf)
{
}

void stdSound_BufferRelease(stdSound_buffer_t* sound)
{	
	if (sound->data && !sound->bIsCopy)
	    STD_FREE(sound->data);

    memset(sound, 0, sizeof(*sound));
    STD_FREE(sound);
}

int stdSound_BufferReset(stdSound_buffer_t* sound)
{
    return 1;
}

void stdSound_BufferSetPan(stdSound_buffer_t* a1, flex_t a2)
{
    
}

void stdSound_BufferSetFrequency(stdSound_buffer_t* sound, int freq)
{
    flex_t pitch = (flex_d_t)freq / (flex_d_t)sound->nSamplesPerSec;
}

stdSound_buffer_t* stdSound_BufferDuplicate(stdSound_buffer_t* sound)
{
    stdSound_buffer_t* out = (stdSound_buffer_t*)STD_ALLOC(sizeof(stdSound_buffer_t));
    if (!out)
        return NULL;
    
    _memset(out, 0, sizeof(*out));
    
    out->data = sound->data;
    out->bStereo = sound->bStereo;
    out->bufferLen = sound->bufferLen;
    out->nSamplesPerSec = sound->nSamplesPerSec;
    out->bitsPerSample = sound->bitsPerSample;
    out->refcnt = 1;
    out->vol = sound->vol;
    out->format = sound->format;
    out->bufferBytes = sound->bufferBytes;
    out->bIsCopy = 1;

    stdSound_BufferUnlock(out, out->data, out->bufferBytes);

    return out;
}

void stdSound_IA3D_idk(flex_t a)
{
}

int stdSound_BufferStop(stdSound_buffer_t* buf)
{
    return 1;
}

void stdSound_BufferSetVolume(stdSound_buffer_t* sound, flex_t vol)
{
    if (!sound) return;
    
    sound->vol = vol;
}

int stdSound_3DSetMode(stdSound_buffer_t* a1, int a2)
{
    return 1;
}

stdSound_3dBuffer_t* stdSound_BufferQueryInterface(stdSound_buffer_t* pSoundBuffer)
{
    return pSoundBuffer;
}

void stdSound_CommitDeferredSettings()
{
}

void stdSound_SetPositionOrientation(rdVector3 *pos, rdVector3 *lvec, rdVector3 *uvec)
{
}

void stdSound_SetPosition(stdSound_buffer_t* sound, rdVector3 *pos)
{

}

void stdSound_SetVelocity(stdSound_buffer_t* sound, rdVector3 *vel)
{
    
}

int stdSound_IsPlaying(stdSound_buffer_t* sound, rdVector3 *pos)
{
    return 0;
}

void stdSound_3DBufferRelease(stdSound_3dBuffer_t* p3DBuffer)
{

}
#endif
