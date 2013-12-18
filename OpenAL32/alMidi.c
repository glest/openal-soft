
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "alMidi.h"
#include "alMain.h"
#include "alError.h"
#include "evtqueue.h"
#include "rwlock.h"
#include "alu.h"


/* Microsecond resolution */
#define TICKS_PER_SECOND (1000000)

/* MIDI events */
#define SYSEX_EVENT  (0xF0)

/* MIDI controllers */
#define CTRL_BANKSELECT_MSB  (0)
#define CTRL_BANKSELECT_LSB  (32)
#define CTRL_ALLNOTESOFF     (123)


void MidiSynth_Construct(MidiSynth *self, ALCdevice *device)
{
    InitEvtQueue(&self->EventQueue);

    RWLockInit(&self->Lock);

    self->Gain = 1.0f;
    self->State = AL_INITIAL;

    self->LastEvtTime = 0;
    self->NextEvtTime = UINT64_MAX;
    self->SamplesSinceLast = 0.0;
    self->SamplesToNext = 0.0;

    self->SamplesPerTick = (ALdouble)device->Frequency / TICKS_PER_SECOND;
}

void MidiSynth_Destruct(MidiSynth *self)
{
    ResetEvtQueue(&self->EventQueue);
}

const char *MidiSynth_getFontName(const MidiSynth* UNUSED(self), const char *filename)
{
    if(!filename || !filename[0])
        filename = getenv("ALSOFT_SOUNDFONT");
    if(!filename || !filename[0])
        filename = GetConfigValue("midi", "soundfont", "");
    if(!filename[0])
        WARN("No default soundfont found\n");

    return filename;
}

extern inline void MidiSynth_setGain(MidiSynth *self, ALfloat gain);
extern inline ALfloat MidiSynth_getGain(const MidiSynth *self);
extern inline void MidiSynth_setState(MidiSynth *self, ALenum state);

void MidiSynth_stop(MidiSynth *self)
{
    ResetEvtQueue(&self->EventQueue);

    self->LastEvtTime = 0;
    self->NextEvtTime = UINT64_MAX;
    self->SamplesSinceLast = 0.0;
    self->SamplesToNext = 0.0;
}

extern inline void MidiSynth_reset(MidiSynth *self);

ALuint64 MidiSynth_getTime(const MidiSynth *self)
{
    ALuint64 time = self->LastEvtTime + (self->SamplesSinceLast/self->SamplesPerTick);
    return clampu(time, self->LastEvtTime, self->NextEvtTime);
}

extern inline ALuint64 MidiSynth_getNextEvtTime(const MidiSynth *self);

void MidiSynth_setSampleRate(MidiSynth *self, ALdouble srate)
{
    ALdouble sampletickrate = srate / TICKS_PER_SECOND;

    self->SamplesSinceLast = self->SamplesSinceLast * sampletickrate / self->SamplesPerTick;
    self->SamplesToNext = self->SamplesToNext * sampletickrate / self->SamplesPerTick;
    self->SamplesPerTick = sampletickrate;
}

extern inline void MidiSynth_update(MidiSynth *self, ALCdevice *device);

ALenum MidiSynth_insertEvent(MidiSynth *self, ALuint64 time, ALuint event, ALsizei param1, ALsizei param2)
{
    MidiEvent entry;
    ALenum err;

    entry.time = time;
    entry.event = event;
    entry.param.val[0] = param1;
    entry.param.val[1] = param2;

    err = InsertEvtQueue(&self->EventQueue, &entry);
    if(err != AL_NO_ERROR) return err;

    if(entry.time < self->NextEvtTime)
    {
        self->NextEvtTime = entry.time;

        self->SamplesToNext  = (self->NextEvtTime - self->LastEvtTime) * self->SamplesPerTick;
        self->SamplesToNext -= self->SamplesSinceLast;
    }

    return AL_NO_ERROR;
}

ALenum MidiSynth_insertSysExEvent(MidiSynth *self, ALuint64 time, const ALbyte *data, ALsizei size)
{
    MidiEvent entry;
    ALenum err;

    entry.time = time;
    entry.event = SYSEX_EVENT;
    entry.param.sysex.size = size;
    entry.param.sysex.data = malloc(size);
    if(!entry.param.sysex.data)
        return AL_OUT_OF_MEMORY;
    memcpy(entry.param.sysex.data, data, size);

    err = InsertEvtQueue(&self->EventQueue, &entry);
    if(err != AL_NO_ERROR)
    {
        free(entry.param.sysex.data);
        return err;
    }

    if(entry.time < self->NextEvtTime)
    {
        self->NextEvtTime = entry.time;

        self->SamplesToNext  = (self->NextEvtTime - self->LastEvtTime) * self->SamplesPerTick;
        self->SamplesToNext -= self->SamplesSinceLast;
    }

    return AL_NO_ERROR;
}


MidiSynth *SynthCreate(ALCdevice *device)
{
    MidiSynth *synth = FSynth_create(device);
    if(!synth) synth = DSynth_create(device);
    return synth;
}


AL_API ALboolean AL_APIENTRY alIsSoundfontSOFT(const char *filename)
{
    ALCdevice *device;
    ALCcontext *context;
    ALboolean ret;

    context = GetContextRef();
    if(!context) return AL_FALSE;

    device = context->Device;
    ret = V(device->Synth,isSoundfont)(filename);

    ALCcontext_DecRef(context);

    return ret;
}

AL_API void AL_APIENTRY alMidiSoundfontSOFT(const char *filename)
{
    ALCdevice *device;
    ALCcontext *context;
    MidiSynth *synth;
    ALenum err;

    context = GetContextRef();
    if(!context) return;

    device = context->Device;
    synth = device->Synth;

    WriteLock(&synth->Lock);
    if(synth->State == AL_PLAYING || synth->State == AL_PAUSED)
        alSetError(context, AL_INVALID_OPERATION);
    else
    {
        err = V(synth,loadSoundfont)(filename);
        if(err != AL_NO_ERROR)
            alSetError(context, err);
    }
    WriteUnlock(&synth->Lock);

    ALCcontext_DecRef(context);
}

AL_API void AL_APIENTRY alMidiEventSOFT(ALuint64SOFT time, ALenum event, ALsizei channel, ALsizei param1, ALsizei param2)
{
    ALCdevice *device;
    ALCcontext *context;
    ALenum err;

    context = GetContextRef();
    if(!context) return;

    if(!(event == AL_NOTEOFF_SOFT || event == AL_NOTEON_SOFT ||
         event == AL_AFTERTOUCH_SOFT || event == AL_CONTROLLERCHANGE_SOFT ||
         event == AL_PROGRAMCHANGE_SOFT || event == AL_CHANNELPRESSURE_SOFT ||
         event == AL_PITCHBEND_SOFT))
        SET_ERROR_AND_GOTO(context, AL_INVALID_ENUM, done);
    if(!(channel >= 0 && channel <= 15))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    if(!(param1 >= 0 && param1 <= 127))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    if(!(param2 >= 0 && param2 <= 127))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);

    device = context->Device;
    ALCdevice_Lock(device);
    err = MidiSynth_insertEvent(device->Synth, time, event|channel, param1, param2);
    ALCdevice_Unlock(device);
    if(err != AL_NO_ERROR)
        alSetError(context, err);

done:
    ALCcontext_DecRef(context);
}

AL_API void AL_APIENTRY alMidiSysExSOFT(ALuint64SOFT time, const ALbyte *data, ALsizei size)
{
    ALCdevice *device;
    ALCcontext *context;
    ALenum err;
    ALsizei i;

    context = GetContextRef();
    if(!context) return;

    if(!data || size < 0)
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    for(i = 0;i < size;i++)
    {
        if((data[i]&0x80))
            SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    }

    device = context->Device;
    ALCdevice_Lock(device);
    err = MidiSynth_insertSysExEvent(device->Synth, time, data, size);
    ALCdevice_Unlock(device);
    if(err != AL_NO_ERROR)
        alSetError(context, err);

done:
    ALCcontext_DecRef(context);
}

AL_API void AL_APIENTRY alMidiPlaySOFT(void)
{
    ALCcontext *context;
    MidiSynth *synth;

    context = GetContextRef();
    if(!context) return;

    synth = context->Device->Synth;
    WriteLock(&synth->Lock);
    V(synth,setState)(AL_PLAYING);
    WriteUnlock(&synth->Lock);

    ALCcontext_DecRef(context);
}

AL_API void AL_APIENTRY alMidiPauseSOFT(void)
{
    ALCcontext *context;
    MidiSynth *synth;

    context = GetContextRef();
    if(!context) return;

    synth = context->Device->Synth;
    WriteLock(&synth->Lock);
    V(synth,setState)(AL_PAUSED);
    WriteUnlock(&synth->Lock);

    ALCcontext_DecRef(context);
}

AL_API void AL_APIENTRY alMidiStopSOFT(void)
{
    ALCdevice *device;
    ALCcontext *context;
    MidiSynth *synth;

    context = GetContextRef();
    if(!context) return;

    device = context->Device;
    synth = device->Synth;

    WriteLock(&synth->Lock);
    V(synth,setState)(AL_STOPPED);

    ALCdevice_Lock(device);
    V0(synth,stop)();
    ALCdevice_Unlock(device);
    WriteUnlock(&synth->Lock);

    ALCcontext_DecRef(context);
}

AL_API void AL_APIENTRY alMidiResetSOFT(void)
{
    ALCdevice *device;
    ALCcontext *context;
    MidiSynth *synth;

    context = GetContextRef();
    if(!context) return;

    device = context->Device;
    synth = device->Synth;

    WriteLock(&synth->Lock);
    V(synth,setState)(AL_INITIAL);

    ALCdevice_Lock(device);
    V0(synth,reset)();
    ALCdevice_Unlock(device);
    WriteUnlock(&synth->Lock);

    ALCcontext_DecRef(context);
}


AL_API void AL_APIENTRY alMidiGainSOFT(ALfloat value)
{
    ALCdevice *device;
    ALCcontext *context;

    context = GetContextRef();
    if(!context) return;

    if(!(value >= 0.0f && isfinite(value)))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);

    device = context->Device;
    V(device->Synth,setGain)(value);

done:
    ALCcontext_DecRef(context);
}


void InitEvtQueue(EvtQueue *queue)
{
    queue->events = NULL;
    queue->maxsize = 0;
    queue->size = 0;
    queue->pos = 0;
}

void ResetEvtQueue(EvtQueue *queue)
{
    ALsizei i;
    for(i = 0;i < queue->size;i++)
    {
        if(queue->events[i].event == SYSEX_EVENT)
        {
            free(queue->events[i].param.sysex.data);
            queue->events[i].param.sysex.data = NULL;
        }
    }

    free(queue->events);
    queue->events = NULL;
    queue->maxsize = 0;
    queue->size = 0;
    queue->pos = 0;
}

ALenum InsertEvtQueue(EvtQueue *queue, const MidiEvent *evt)
{
    ALsizei pos;

    if(queue->maxsize == queue->size)
    {
        if(queue->pos > 0)
        {
            /* Queue has some stale entries, remove them to make space for more
             * events. */
            for(pos = 0;pos < queue->pos;pos++)
            {
                if(queue->events[pos].event == SYSEX_EVENT)
                {
                    free(queue->events[pos].param.sysex.data);
                    queue->events[pos].param.sysex.data = NULL;
                }
            }
            memmove(&queue->events[0], &queue->events[queue->pos],
                    (queue->size-queue->pos)*sizeof(queue->events[0]));
            queue->size -= queue->pos;
            queue->pos = 0;
        }
        else
        {
            /* Queue is full, double the allocated space. */
            void *temp = NULL;
            ALsizei newsize;

            newsize = (queue->maxsize ? (queue->maxsize<<1) : 16);
            if(newsize > queue->maxsize)
                temp = realloc(queue->events, newsize * sizeof(queue->events[0]));
            if(!temp)
                return AL_OUT_OF_MEMORY;

            queue->events = temp;
            queue->maxsize = newsize;
        }
    }

    pos = queue->pos;
    if(queue->size > 0)
    {
        ALsizei high = queue->size - 1;
        while(pos < high)
        {
            ALsizei mid = pos + (high-pos)/2;
            if(queue->events[mid].time < evt->time)
                pos = mid + 1;
            else
                high = mid;
        }
        while(pos < queue->size && queue->events[pos].time <= evt->time)
            pos++;

        if(pos < queue->size)
            memmove(&queue->events[pos+1], &queue->events[pos],
                    (queue->size-pos)*sizeof(queue->events[0]));
    }

    queue->events[pos] = *evt;
    queue->size++;

    return AL_NO_ERROR;
}
