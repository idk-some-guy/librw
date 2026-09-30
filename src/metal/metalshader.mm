#ifdef RW_METAL
#include "metalobjc.h"
#include "rwmetalshader.h"
#include "metalstate.h"
#include "metalkeys.h"

#define PLUGIN_ID 0

namespace rw {
namespace metal {

#include "shaders/header_metal.inc"

Shader *currentShader;
uint32 currentVariant;
static uint32 shaderIds[(1<<PIPEKEY_SHADERBITS)/32] = { 1 };

static uint32
allocShaderId(void)
{
	uint32 i;
	for(i = 1; i < (1<<PIPEKEY_SHADERBITS); i++)
		if((shaderIds[i/32] & (1u << i%32)) == 0){
			shaderIds[i/32] |= 1u << i%32;
			return i;
		}
	return 0;
}

static char*
copyString(const char *s)
{
	char *d = rwNewT(char, strlen(s)+1, MEMDUR_EVENT | ID_DRIVER);
	strcpy(d, s);
	return d;
}

static id<MTLFunction>
specialise(id<MTLLibrary> lib, const char *name, uint32 variant)
{
	MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
	NSError *err = nil;
	id<MTLFunction> fn;
	bool v;
	int i;

	for(i = 0; i < 4; i++){
		v = (variant >> i) & 1;
		[values setConstantValue:&v type:MTLDataTypeBool atIndex:i];
	}
	fn = [lib newFunctionWithName:[NSString stringWithUTF8String:name] constantValues:values error:&err];
	if(fn == nil)
		RWERROR((ERR_GENERAL, err ? err.localizedDescription.UTF8String : name));
	return fn;
}

Shader*
Shader::create(const char **src, const char *vs, const char *fs, uint32 variantMask)
{
	MetalContext *ctx = getContext();
	NSMutableString *all;
	NSError *err = nil;
	id<MTLLibrary> lib;
	Shader *sh;
	void *vfn, *ffn;
	uint32 sid;
	int i;

	if(ctx == nil)
		return nil;
	@autoreleasepool {
		all = [NSMutableString string];
		for(i = 0; src[i]; i++)
			[all appendString:[NSString stringWithUTF8String:src[i]]];
		lib = [ctx->device newLibraryWithSource:all options:nil error:&err];
		if(lib == nil){
			RWERROR((ERR_GENERAL, err.localizedDescription.UTF8String));
			return nil;
		}
		if(err)
			fprintf(stderr, "rw::metal: shader %s/%s: %s\n", vs, fs, err.localizedDescription.UTF8String);
		sid = allocShaderId();
		if(sid == 0){
			RWERROR((ERR_GENERAL, "too many shaders"));
			return nil;
		}

		sh = rwNewT(Shader, 1, MEMDUR_EVENT | ID_DRIVER);
		memset(sh, 0, sizeof(Shader));
		sh->library = (__bridge_retained void*)lib;
		sh->vertexName = copyString(vs);
		sh->fragmentName = copyString(fs);
		sh->shaderId = sid;
		sh->variantMask = variantMask & (NUMVARIANTS-1);
		sh->textureStages = -1;
		if(!sh->getFunctions(0, &vfn, &ffn) || !sh->getFunctions(sh->variantMask, &vfn, &ffn)){
			sh->destroy();
			return nil;
		}
	}
	return sh;
}

bool32
Shader::getFunctions(uint32 variant, void **vs, void **fs)
{
	id<MTLLibrary> lib = (__bridge id<MTLLibrary>)this->library;
	id<MTLFunction> fn;

	variant &= NUMVARIANTS-1;
	if(this->vertexFns[variant] == nil){
		fn = specialise(lib, this->vertexName, variant);
		if(fn == nil)
			return 0;
		this->vertexFns[variant] = (__bridge_retained void*)fn;
	}
	if(this->fragmentFns[variant] == nil){
		fn = specialise(lib, this->fragmentName, variant);
		if(fn == nil)
			return 0;
		this->fragmentFns[variant] = (__bridge_retained void*)fn;
	}
	*vs = this->vertexFns[variant];
	*fs = this->fragmentFns[variant];
	return 1;
}

void
Shader::use(uint32 variant)
{
	currentShader = this;
	currentVariant = variant & this->variantMask;
}

void
Shader::destroy(void)
{
	int i;

	if(currentShader == this)
		currentShader = nil;
	for(i = 0; i < NUMVARIANTS; i++){
		if(this->vertexFns[i])
			CFBridgingRelease(this->vertexFns[i]);
		if(this->fragmentFns[i])
			CFBridgingRelease(this->fragmentFns[i]);
	}
	CFBridgingRelease(this->library);
	forgetShaderPipelines(this->shaderId);
	shaderIds[this->shaderId/32] &= ~(1u << this->shaderId%32);
	rwFree(this->vertexName);
	rwFree(this->fragmentName);
	rwFree(this);
}

}
}
#endif
