#ifdef RW_METAL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#include "rwmetal.h"
#include "rwmetalplg.h"
#include "rwmetalimpl.h"

namespace rw {
namespace metal {

Texture *readNativeTexture(Stream *stream) { return nil; }
void writeNativeTexture(Texture *tex, Stream *stream) { }
uint32 getSizeNativeTexture(Texture *tex) { return 0; }

}
}
#endif
