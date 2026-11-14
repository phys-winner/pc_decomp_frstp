/* Stand-in OpenAL library for tests/openal_reconnect_fixture.c: the thirty typed routes of the
 * bridge do nothing, and the connection state of the one output is under the test's control. */
#include <AL/al.h>
#include <AL/alc.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
enum { FAKE_CONNECTED, FAKE_REOPEN_ALLOWED, FAKE_EXTENSION, FAKE_REOPENS, FAKE_DISABLES, FAKE_OTHER_DISABLES,
       FAKE_QUERIES, FAKE_ERROR_READS, FAKE_ATTRIBUTE_COUNT, FAKE_NAMED_REOPENS, FAKE_PAUSES, FAKE_RESUMES };
static int state[12] = {1, 1, 1};
static ALCint attributes[258];
static ALCenum device_error;
static int device_token, context_token[4];
static unsigned contexts;

EXPORT void fake_set(int what, int value) { state[what] = value; }
EXPORT int fake_get(int what) { return state[what]; }
EXPORT const ALCint *fake_attributes(void) { return attributes; }

EXPORT void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei rate) { (void)buffer; (void)format; (void)data; (void)size; (void)rate; }
EXPORT void alDeleteBuffers(ALsizei n, const ALuint *names) { (void)n; (void)names; }
EXPORT void alDeleteSources(ALsizei n, const ALuint *names) { (void)n; (void)names; }
EXPORT void alDistanceModel(ALenum model) { (void)model; }
EXPORT void alGenBuffers(ALsizei n, ALuint *names) { for (ALsizei i = 0; i < n; ++i) names[i] = (ALuint)i + 1; }
EXPORT void alGenSources(ALsizei n, ALuint *names) { for (ALsizei i = 0; i < n; ++i) names[i] = (ALuint)i + 1; }
EXPORT ALenum alGetError(void) { return AL_NO_ERROR; }
EXPORT void alGetListener3f(ALenum p, ALfloat *x, ALfloat *y, ALfloat *z) { (void)p; *x = *y = *z = 0; }
EXPORT void alGetListenerfv(ALenum p, ALfloat *values) { (void)p; (void)values; }
EXPORT void alGetSourcei(ALuint source, ALenum p, ALint *value) { (void)source; (void)p; *value = 0; }
EXPORT const ALchar *alGetString(ALenum p) { (void)p; return ""; }
EXPORT void alListenerfv(ALenum p, const ALfloat *values) { (void)p; (void)values; }
EXPORT void alSourcePlay(ALuint source) { (void)source; }
EXPORT void alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *names) { (void)source; (void)n; (void)names; }
EXPORT void alSourceStop(ALuint source) { (void)source; }
EXPORT void alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *names) { (void)source; (void)n; (void)names; }
EXPORT void alSourcef(ALuint source, ALenum p, ALfloat value) { (void)source; (void)p; (void)value; }
EXPORT void alSourcefv(ALuint source, ALenum p, const ALfloat *values) { (void)source; (void)p; (void)values; }
EXPORT void alSourcei(ALuint source, ALenum p, ALint value) { (void)source; (void)p; (void)value; }
EXPORT ALCboolean alcCloseDevice(ALCdevice *device) { (void)device; return ALC_TRUE; }
EXPORT ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *list) {
    (void)list;
    return device && contexts < 4 ? (ALCcontext *)&context_token[contexts++] : NULL;
}
EXPORT void alcDestroyContext(ALCcontext *context) { (void)context; }
EXPORT ALCenum alcGetError(ALCdevice *device) { (void)device; ALCenum value = device_error; device_error = ALC_NO_ERROR; ++state[FAKE_ERROR_READS]; return value; }
/* As in the real library, the reopen function is no exported symbol: only alcGetProcAddress gives it out. */
static ALCboolean reopen(ALCdevice *device, const ALCchar *name, const ALCint *list);
EXPORT void *alcGetProcAddress(ALCdevice *device, const ALCchar *name) {
    ALCboolean (*function)(ALCdevice *, const ALCchar *, const ALCint *) = reopen;
    void *address = NULL;
    (void)device;
    if (name && !strcmp(name, "alcReopenDeviceSOFT")) memcpy(&address, &function, sizeof(address));
    return address;
}
EXPORT const ALCchar *alcGetString(ALCdevice *device, ALCenum p) { (void)device; (void)p; return ""; }
EXPORT ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *name) { (void)device; (void)name; return ALC_FALSE; }
EXPORT ALCboolean alcMakeContextCurrent(ALCcontext *context) { (void)context; return ALC_TRUE; }
EXPORT ALCdevice *alcOpenDevice(const ALCchar *name) { (void)name; return (ALCdevice *)&device_token; }
EXPORT void alcProcessContext(ALCcontext *context) { (void)context; }
EXPORT void alcSuspendContext(ALCcontext *context) { (void)context; }

EXPORT void alcDevicePauseSOFT(ALCdevice *device) { (void)device; ++state[FAKE_PAUSES]; }
EXPORT void alcDeviceResumeSOFT(ALCdevice *device) { (void)device; ++state[FAKE_RESUMES]; }
EXPORT void alcGetIntegerv(ALCdevice *device, ALCenum p, ALCsizei size, ALCint *values) {
    (void)device;
    if (p == 0x313 && size >= 1) { values[0] = state[FAKE_CONNECTED]; ++state[FAKE_QUERIES]; }
}
static ALCboolean reopen(ALCdevice *device, const ALCchar *name, const ALCint *list) {
    (void)device;
    ++state[FAKE_REOPENS];
    if (name) ++state[FAKE_NAMED_REOPENS];
    state[FAKE_ATTRIBUTE_COUNT] = 0;
    memset(attributes, 0, sizeof(attributes));
    if (list) {
        int count = 0;
        while (count < 256 && list[count]) { attributes[count] = list[count]; attributes[count + 1] = list[count + 1]; count += 2; }
        state[FAKE_ATTRIBUTE_COUNT] = count + 1;
    }
    if (!state[FAKE_REOPEN_ALLOWED]) { device_error = ALC_INVALID_VALUE; return ALC_FALSE; }
    state[FAKE_CONNECTED] = 1;
    return ALC_TRUE;
}
EXPORT void alDisable(ALenum capability) { ++state[capability == 0x19AB ? FAKE_DISABLES : FAKE_OTHER_DISABLES]; }
EXPORT ALboolean alIsExtensionPresent(const ALchar *name) { return state[FAKE_EXTENSION] && !strcmp(name, "AL_SOFTX_hold_on_disconnect") ? AL_TRUE : AL_FALSE; }
