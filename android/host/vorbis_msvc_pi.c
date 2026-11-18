/* lib/mdct.c and lib/lsp.c of libvorbis 1.3.7 as the game's MSVC build compiled them.
 * MSVC's math.h defines no M_PI (without _USE_MATH_DEFINES), so libvorbis's os.h supplies
 * `#define M_PI (3.1415926536f)`, a float: the game's mdct_init (0x141C5DA10) computes its
 * angles in float, (float)i * (3.1415927f / (float)n), before cos/sin.  Bionic's math.h defines
 * a double M_PI, which gives other trig tables and PCM that differs by one LSB now and then
 * (found with debug.fp.vorbis_verify, android/host/vorbisfile_native.c). */
#include <math.h>
#undef M_PI
#define M_PI (3.1415926536f)
#include "../../reference/libvorbis-1.3.7/lib/mdct.c"
#include "../../reference/libvorbis-1.3.7/lib/lsp.c"
