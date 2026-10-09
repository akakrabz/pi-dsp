/* The single translation unit that compiles miniaudio. Backends are loaded at
   runtime (dlopen), so no ALSA/PulseAudio/JACK headers are needed at build time. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"
