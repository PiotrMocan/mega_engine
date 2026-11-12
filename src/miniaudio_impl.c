#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
