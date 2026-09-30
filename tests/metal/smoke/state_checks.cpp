#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include <src/metal/metalinst.h>
#include "state_checks.h"
#include "objc_checks.h"

using namespace rw;

static char details[4096];
static int detailsLen = 0;

static void
Detail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(details + detailsLen, sizeof(details) - detailsLen, fmt, ap);
	va_end(ap);
	if(n > 0)
		detailsLen += n;
	if(detailsLen >= (int)sizeof(details))
		detailsLen = sizeof(details) - 1;
}

static bool
Report(bool ok, const char *name)
{
	printf("%s %s\n%s", ok ? "PASS" : "FAIL", name, details);
	details[0] = '\0';
	detailsLen = 0;
	return ok;
}

static const char *testShaderSrc =
"vertex VertexOut testVS(uint vid [[vertex_id]], constant State &state [[buffer(BUFFER_STATE)]],\n"
"                        constant Scene &scene [[buffer(BUFFER_SCENE)]],\n"
"                        constant Object &object [[buffer(BUFFER_OBJECT)]],\n"
"                        constant Lights &lights [[buffer(BUFFER_LIGHTS)]]) {\n"
"	VertexOut out;\n"
"	out.position = MetalDepth(scene.proj * object.world * float4(0.0, 0.0, 0.0, 1.0));\n"
"	out.color = float4(DoDynamicLight(float3(0.0), float3(0.0, 0.0, 1.0), lights), 1.0);\n"
"	out.tex0 = float2(0.0);\n"
"	out.fog = DoFog(1.0, state);\n"
"	out.pointSize = 1.0;\n"
"	return out;\n"
"}\n"
"fragment float4 testFS(FragmentIn in [[stage_in]], constant State &state [[buffer(BUFFER_STATE)]],\n"
"                       constant Material &material [[buffer(BUFFER_MATERIAL)]]) {\n"
"	DoAlphaTest(in.color.a, state);\n"
"	return in.color * material.matColor;\n"
"}\n";

static bool
AllVariants(metal::Shader *sh, const char *name)
{
	bool ok = true;
	for(uint32 v = 0; v < metal::NUMVARIANTS; v++){
		void *vs = nil, *fs = nil;
		if(!sh->getFunctions(v, &vs, &fs) || vs == nil || fs == nil){
			Detail("  %s variant %u did not specialise\n", name, v);
			ok = false;
		}
	}
	return ok;
}

bool
CheckShaderVariants(void)
{
	bool ok = metal::im2dShader != nil;
	if(!ok)
		Detail("  no im2d shader\n");
	else
		ok &= AllVariants(metal::im2dShader, "im2d");

	const char *src[] = { metal::header_metal_src, testShaderSrc, nil };
	metal::Shader *sh = metal::Shader::create(src, "testVS", "testFS", 0);
	if(sh == nil){
		Detail("  header with lighting, fog and alpha test did not compile\n");
		ok = false;
	}else{
		ok &= AllVariants(sh, "header");
		uint32 id = sh->shaderId;
		sh->destroy();
		sh = metal::Shader::create(src, "testVS", "testFS", 0);
		if(sh == nil || sh->shaderId != id){
			Detail("  shader id %u was not reused\n", id);
			ok = false;
		}
		if(sh)
			sh->destroy();
	}
	return Report(ok, "im2d and header shaders compile in all 16 variants");
}

bool
CheckPrewarmedPipelines(void)
{
	metal::StateStats s = metal::getStateStats();
	bool radar = metal::pipelineCached(0x000023e210010401ull);
	if(!radar)
		Detail("  radar mask pipeline 000023e210010401 is not cached\n");
	bool ok = s.pipelinesAtInit == 51 && s.pipelineFailures == 0 && s.blockSizeMismatches == 0 && radar;
	if(!ok)
		Detail("  pipelines at init %u, after init %u, failures %u, uniform block size mismatches %u\n",
		       s.pipelinesAtInit, s.pipelinesLate, s.pipelineFailures, s.blockSizeMismatches);
	return Report(ok, "51 im2d, world, skin, env and im3d pipeline states prewarmed at init, radar mask included");
}

struct StateValue
{
	int32 state;
	uint32 value;
	const char *name;
};

bool
CheckRenderStateRoundTrip(Raster *raster)
{
	static const StateValue values[] = {
		{ TEXTUREADDRESS, Texture::CLAMP, "TEXTUREADDRESS" },
		{ TEXTUREADDRESSU, Texture::MIRROR, "TEXTUREADDRESSU" },
		{ TEXTUREADDRESSV, Texture::BORDER, "TEXTUREADDRESSV" },
		{ TEXTUREFILTER, Texture::LINEARMIPLINEAR, "TEXTUREFILTER" },
		{ VERTEXALPHA, 1, "VERTEXALPHA" },
		{ SRCBLEND, BLENDONE, "SRCBLEND" },
		{ DESTBLEND, BLENDINVDESTALPHA, "DESTBLEND" },
		{ ZTESTENABLE, 0, "ZTESTENABLE" },
		{ ZWRITEENABLE, 0, "ZWRITEENABLE" },
		{ FOGENABLE, 1, "FOGENABLE" },
		{ FOGCOLOR, 0x80402010, "FOGCOLOR" },
		{ CULLMODE, CULLBACK, "CULLMODE" },
		{ STENCILENABLE, 1, "STENCILENABLE" },
		{ STENCILFAIL, STENCILZERO, "STENCILFAIL" },
		{ STENCILZFAIL, STENCILINVERT, "STENCILZFAIL" },
		{ STENCILPASS, STENCILREPLACE, "STENCILPASS" },
		{ STENCILFUNCTION, STENCILNOTEQUAL, "STENCILFUNCTION" },
		{ STENCILFUNCTIONREF, 7, "STENCILFUNCTIONREF" },
		{ STENCILFUNCTIONMASK, 0x0F, "STENCILFUNCTIONMASK" },
		{ STENCILFUNCTIONWRITEMASK, 0xF0, "STENCILFUNCTIONWRITEMASK" },
		{ ALPHATESTFUNC, ALPHALESS, "ALPHATESTFUNC" },
		{ ALPHATESTREF, 77, "ALPHATESTREF" },
		{ GSALPHATEST, 1, "GSALPHATEST" },
		{ GSALPHATESTREF, 100, "GSALPHATESTREF" },
	};
	const int n = sizeof(values)/sizeof(values[0]);
	uint32 saved[n];
	bool ok = true;

	void *savedRaster = GetRenderStatePtr(TEXTURERASTER);
	SetRenderStatePtr(TEXTURERASTER, raster);
	if(GetRenderStatePtr(TEXTURERASTER) != raster){
		Detail("  TEXTURERASTER did not read back\n");
		ok = false;
	}
	SetRenderStatePtr(TEXTURERASTER, nil);
	if(GetRenderStatePtr(TEXTURERASTER) != nil){
		Detail("  TEXTURERASTER nil did not read back\n");
		ok = false;
	}

	for(int i = 0; i < n; i++)
		saved[i] = GetRenderState(values[i].state);
	for(int i = 0; i < n; i++){
		SetRenderState(values[i].state, values[i].value);
		uint32 got = GetRenderState(values[i].state);
		if(got != values[i].value){
			Detail("  %s set to 0x%x reads back 0x%x\n", values[i].name, values[i].value, got);
			ok = false;
		}
	}
	SetRenderState(TEXTUREADDRESSU, Texture::WRAP);
	SetRenderState(TEXTUREADDRESSV, Texture::CLAMP);
	if(GetRenderState(TEXTUREADDRESS) != 0){
		Detail("  TEXTUREADDRESS with U and V different is not 0\n");
		ok = false;
	}
	for(int i = 0; i < n; i++)
		SetRenderState(values[i].state, saved[i]);
	SetRenderStatePtr(TEXTURERASTER, savedRaster);
	return Report(ok, "every render state reads back what was set");
}

bool
CheckLightingShaderBlockSizes(void)
{
	const char *src[] = { metal::header_metal_src, testShaderSrc, nil };
	const uint32 want = 1<<metal::BUFFER_SCENE | 1<<metal::BUFFER_OBJECT | 1<<metal::BUFFER_LIGHTS |
		1<<metal::BUFFER_MATERIAL | 1<<metal::BUFFER_STATE;
	metal::Shader *sh = metal::Shader::create(src, "testVS", "testFS", 0);
	if(sh == nil){
		Detail("  lighting test shader did not compile\n");
		return Report(false, "lighting shader uniform blocks match their C structs");
	}
	uint32 before = metal::getStateStats().blockSizeMismatches;
	uint32 checked = metal::checkShaderBlockSizes(sh, metal::NUMVARIANTS-1);
	uint32 mismatches = metal::getStateStats().blockSizeMismatches - before;
	sh->destroy();
	bool ok = checked == want && mismatches == 0;
	if(!ok)
		Detail("  blocks used 0x%x, expected 0x%x, mismatches %u\n", checked, want, mismatches);
	return Report(ok, "lighting shader uniform blocks match their C structs");
}

static metal::MetalRaster*
MetalExt(Raster *raster)
{
	using namespace metal;
	return GETMETALRASTEREXT(raster);
}

static bool
AddressingIs(Raster *ras, uint32 rasU, uint32 rasV, uint32 stateU, uint32 stateV, const char *step)
{
	metal::MetalRaster *natras = MetalExt(ras);
	uint32 u = GetRenderState(TEXTUREADDRESSU);
	uint32 v = GetRenderState(TEXTUREADDRESSV);
	if(natras->addressU == rasU && natras->addressV == rasV && u == stateU && v == stateV)
		return true;
	Detail("  %s: raster %u,%u expected %u,%u; render state %u,%u expected %u,%u\n", step,
	       natras->addressU, natras->addressV, rasU, rasV, u, v, stateU, stateV);
	return false;
}

bool
CheckRasterAddressing(void)
{
	const bool gl3 = metal::gl3Addressing;
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE);
	if(ras == nil)
		return Report(false, "texture addressing reaches the bound raster");
	void *savedRaster = GetRenderStatePtr(TEXTURERASTER);
	uint32 savedU = GetRenderState(TEXTUREADDRESSU);
	uint32 savedV = GetRenderState(TEXTUREADDRESSV);

	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(TEXTUREADDRESS, Texture::WRAP);
	SetRenderStatePtr(TEXTURERASTER, ras);
	bool ok = AddressingIs(ras, Texture::WRAP, Texture::WRAP, Texture::WRAP, Texture::WRAP, "bind");

	SetRenderState(TEXTUREADDRESSU, Texture::CLAMP);
	SetRenderState(TEXTUREADDRESSV, Texture::MIRROR);
	ok &= AddressingIs(ras, gl3 ? Texture::WRAP : Texture::CLAMP, gl3 ? Texture::WRAP : Texture::MIRROR,
		Texture::CLAMP, Texture::MIRROR, "set U and V");

	SetRenderState(TEXTUREADDRESS, Texture::BORDER);
	ok &= AddressingIs(ras, gl3 ? Texture::WRAP : Texture::BORDER, gl3 ? Texture::WRAP : Texture::BORDER,
		Texture::BORDER, Texture::BORDER, "set both");

	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(TEXTUREADDRESSU, savedU);
	SetRenderState(TEXTUREADDRESSV, savedV);
	SetRenderStatePtr(TEXTURERASTER, savedRaster);
	ras->destroy();
	return Report(ok, gl3 ? "texture addressing on the bound raster follows gl3"
	                      : "texture addressing reaches the bound raster");
}

struct RingGrowth
{
	metal::RingSpace a, b;
	uint32 size;
	bool first, grew;
};

static uint32 forcedRingGrowthBytes;

uint32
ForcedRingGrowthBytes(void)
{
	return forcedRingGrowthBytes;
}

static void
AllocateAcrossGrowth(void *arg)
{
	RingGrowth *g = (RingGrowth*)arg;
	g->first = metal::ringAlloc(256, 256, &g->a) != 0;
	if(!g->first)
		return;
	memset(g->a.cpu, 0xA5, 256);
	WatchObject(g->a.buffer);
	g->size = metal::getStateStats().ringSize;
	g->grew = metal::ringAlloc(g->size + 256, 256, &g->b) && g->b.buffer != g->a.buffer;
}

bool
CheckRingGrowthKeepsHandedOutBuffer(void)
{
	RingGrowth g = {};
	uint32 growsBefore = metal::getStateStats().ringGrows;
	RunInPool(AllocateAcrossGrowth, &g);
	metal::StateStats s = metal::getStateStats();
	metal::RingSpace &a = g.a;
	bool ok = g.first;
	if(ok){
		ok = g.grew;
		if(!ok)
			Detail("  ring did not grow past %u bytes\n", g.size);
		forcedRingGrowthBytes = g.size + 256;
		if(s.ringGrows != growsBefore + 1){
			Detail("  %u ring grows counted, expected 1\n", s.ringGrows - growsBefore);
			ok = false;
		}
		if(s.frameRingBytes < g.size + 256){
			Detail("  %u ring bytes counted in the frame, expected at least %u\n", s.frameRingBytes, g.size + 256);
			ok = false;
		}
		if(!WatchedObjectAlive()){
			Detail("  buffer handed out before the growth was released\n");
			ok = false;
		}else
			for(int i = 0; i < 256; i++)
				if(a.cpu[i] != 0xA5){
					Detail("  byte %d of the earlier allocation changed\n", i);
					ok = false;
					break;
				}
	}else
		Detail("  ringAlloc failed\n");
	WatchObject(nil);
	return Report(ok, "ring growth keeps a buffer handed out earlier in the frame");
}

struct RingPadding
{
	metal::RingSpace a, b;
	uint32 before, after;
	bool allocated;
};

static void
AllocatePadded(void *arg)
{
	RingPadding *p = (RingPadding*)arg;
	p->allocated = metal::ringAlloc(2, 256, &p->a) != 0;
	p->before = metal::getStateStats().frameRingBytes;
	p->allocated = p->allocated && metal::ringAlloc(1, 256, &p->b);
	p->after = metal::getStateStats().frameRingBytes;
}

bool
CheckRingBytesCountPadding(void)
{
	RingPadding p = {};
	RunInPool(AllocatePadded, &p);
	bool ok = p.allocated;
	if(ok){
		if(p.b.buffer != p.a.buffer || p.b.offset != p.a.offset + 256){
			Detail("  second allocation at %p+%u, expected %p+%u\n", p.b.buffer, p.b.offset, p.a.buffer, p.a.offset + 256);
			ok = false;
		}
		if(p.after - p.before != 255){
			Detail("  %u ring bytes counted for a 1-byte allocation after 254 bytes of padding, expected 255\n",
			       p.after - p.before);
			ok = false;
		}
	}else
		Detail("  ringAlloc failed\n");
	return Report(ok, "ring bytes counted per frame include alignment padding");
}

static const char *brokenShaderSrc =
"struct BrokenIn { float4 position [[position]]; float4 extra [[user(extra)]]; };\n"
"vertex VertexOut brokenVS(uint vid [[vertex_id]]) {\n"
"	VertexOut out;\n"
"	out.position = float4(0.0, 0.0, 0.0, 1.0);\n"
"	out.color = float4(1.0);\n"
"	out.tex0 = float2(0.0);\n"
"	out.fog = 1.0;\n"
"	out.pointSize = 1.0;\n"
"	return out;\n"
"}\n"
"fragment float4 brokenFS(BrokenIn in [[stage_in]]) {\n"
"	return in.extra;\n"
"}\n";

bool
CheckFlushCacheDropsFailedPipeline(void)
{
	const char *src[] = { metal::header_metal_src, brokenShaderSrc, nil };
	metal::Shader *broken = metal::Shader::create(src, "brokenVS", "brokenFS", 0);
	if(broken == nil){
		Detail("  broken test shader did not compile\n");
		return Report(false, "flushCache reports a pipeline that failed to build");
	}
	if(!BeginTestEncoder()){
		Detail("  no test encoder\n");
		broken->destroy();
		return Report(false, "flushCache reports a pipeline that failed to build");
	}
	bool ok = true;
	uint32 droppedBefore = metal::getStateStats().droppedDraws;
	uint32 before = metal::getStateStats().pipelineFailures;
	metal::setVertexLayout(metal::im2dVertexLayout);
	metal::im2dShader->use();
	if(!metal::flushCache()){
		Detail("  flushCache failed with the im2d shader\n");
		ok = false;
	}
	broken->use();
	if(metal::flushCache()){
		Detail("  flushCache accepted a pipeline that failed to build\n");
		ok = false;
	}
	uint32 failed = metal::getStateStats().pipelineFailures - before;
	if(failed != 1){
		Detail("  %u pipeline failures, expected 1\n", failed);
		ok = false;
	}
	uint32 dropped = metal::getStateStats().droppedDraws - droppedBefore;
	if(dropped != 1){
		Detail("  %u draws counted as dropped, expected 1\n", dropped);
		ok = false;
	}
	const char *line = metal::getDropLine();
	if(strstr(line, "its pipeline failed to build") == nil || strstr(line, "brokenVS/brokenFS") == nil){
		Detail("  the drop line is \"%s\"\n", line);
		ok = false;
	}
	EndTestEncoder();
	metal::currentShader = nil;
	metal::setVertexLayout(0);
	broken->destroy();
	return Report(ok, "flushCache reports a pipeline that failed to build");
}

static const char *attribShaderSrc =
"struct AttribIn {\n"
"	float4 pos [[attribute(ATTRIB_POS)]];\n"
"	float4 color [[attribute(ATTRIB_COLOR)]];\n"
"	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];\n"
"	float4 normal [[attribute(ATTRIB_NORMAL)]];\n"
"	uchar4 indices [[attribute(ATTRIB_INDICES)]];\n"
"};\n"
"struct PlainIn {\n"
"	float4 pos [[attribute(ATTRIB_POS)]];\n"
"	float4 color [[attribute(ATTRIB_COLOR)]];\n"
"};\n"
"struct Custom { float4 color; };\n"
"static VertexOut screenVertex(float4 pos, constant Scene &scene) {\n"
"	VertexOut out;\n"
"	out.position = pos;\n"
"	out.position.xy = out.position.xy * scene.xform.xy + scene.xform.zw;\n"
"	out.position.xyz *= out.position.w;\n"
"	out.position = MetalDepth(out.position);\n"
"	out.color = float4(1.0);\n"
"	out.tex0 = float2(0.0);\n"
"	out.fog = 1.0;\n"
"	out.pointSize = 1.0;\n"
"	return out;\n"
"}\n"
"vertex VertexOut defaultAttribVS(AttribIn in [[stage_in]], constant Scene &scene [[buffer(BUFFER_SCENE)]]) {\n"
"	VertexOut out = screenVertex(in.pos, scene);\n"
"	out.color = float4(float3(0.2, 0.4, 0.6)*in.normal.w + float3(in.normal.xyz) + float3(in.indices.xyz),\n"
"	                   float(in.indices.w));\n"
"	return out;\n"
"}\n"
"vertex VertexOut plainVS(PlainIn in [[stage_in]], constant Scene &scene [[buffer(BUFFER_SCENE)]]) {\n"
"	VertexOut out = screenVertex(in.pos, scene);\n"
"	out.color = in.color;\n"
"	return out;\n"
"}\n"
"fragment float4 colorFS(FragmentIn in [[stage_in]]) {\n"
"	return in.color;\n"
"}\n"
"fragment float4 customFS(FragmentIn in [[stage_in]], constant Custom &custom [[buffer(BUFFER_CUSTOM)]]) {\n"
"	return custom.color;\n"
"}\n"
"struct BigCustom { float4 v[80]; };\n"
"fragment float4 bigCustomFS(FragmentIn in [[stage_in]], constant BigCustom &custom [[buffer(BUFFER_CUSTOM)]]) {\n"
"	return custom.v[79];\n"
"}\n";

static uint64
UnblendedKey(metal::Shader *sh, uint32 variant)
{
	metal::PipelineDesc d = {};
	d.shader = sh->shaderId;
	d.variant = variant;
	d.vertexLayout = metal::im2dVertexLayout;
	d.blendEnable = false;
	d.writeMask = 15;
	d.colorFormat = metal::COLORFMT_RGBA8;
	d.depthFormat = metal::DEPTHFMT_NONE;
	d.sampleCount = 1;
	return metal::pipelineKey(d);
}

static bool
FlushCaches(metal::Shader *sh, uint32 variant, const char *step)
{
	uint64 key = UnblendedKey(sh, variant);
	bool ok = true;
	if(metal::pipelineCached(key)){
		Detail("  %s: variant %u key cached before the draw\n", step, variant);
		ok = false;
	}
	if(!metal::flushCache()){
		Detail("  %s: flushCache failed\n", step);
		ok = false;
	}
	if(!metal::pipelineCached(key)){
		Detail("  %s: no pipeline cached for variant %u\n", step, variant);
		ok = false;
	}
	return ok;
}

struct UnblendedState
{
	void *raster;
	uint32 vertexAlpha;
};

static UnblendedState
BeginUnblended(void)
{
	UnblendedState s = { GetRenderStatePtr(TEXTURERASTER), GetRenderState(VERTEXALPHA) };
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(VERTEXALPHA, 0);
	metal::setVertexLayout(metal::im2dVertexLayout);
	return s;
}

static void
EndUnblended(const UnblendedState &s)
{
	metal::currentShader = nil;
	metal::setVertexLayout(0);
	SetRenderState(VERTEXALPHA, s.vertexAlpha);
	SetRenderStatePtr(TEXTURERASTER, s.raster);
}

bool
CheckVariantPerDraw(void)
{
	const char *name = "flushCache builds the pipeline for the variant chosen per draw";
	const char *src[] = { metal::header_metal_src, testShaderSrc, nil };
	if(!BeginTestEncoder()){
		Detail("  no test encoder\n");
		return Report(false, name);
	}
	UnblendedState saved = BeginUnblended();
	bool ok = true;
	metal::Shader *sh = metal::Shader::create(src, "testVS", "testFS", metal::VARIANT_ALL);
	if(sh == nil){
		Detail("  test shader did not compile\n");
		ok = false;
	}else{
		sh->use(metal::VARIANT_DIRECTIONALS | metal::VARIANT_ALPHATEST);
		ok &= FlushCaches(sh, 3, "directionals with alpha test");
		sh->use(0);
		ok &= FlushCaches(sh, 0, "no lights, no alpha test");
	}
	metal::im2dShader->use(metal::VARIANT_DIRECTIONALS);
	if(!metal::flushCache()){
		Detail("  flushCache failed with the im2d shader\n");
		ok = false;
	}
	if(metal::currentVariant != 0){
		Detail("  im2d shader with directionals uses variant %u, expected 0\n", metal::currentVariant);
		ok = false;
	}
	EndTestEncoder();
	EndUnblended(saved);
	if(sh)
		sh->destroy();
	return Report(ok, name);
}

static bool
UploadsRose(uint32 before, uint32 index, uint32 want, const char *step)
{
	uint32 now = metal::getStateStats().blockUploads[index];
	if(now - before == want)
		return true;
	Detail("  %s: block %u uploaded %u times, expected %u\n", step, index, now - before, want);
	return false;
}

static bool
BindsMatch(const metal::StateStats &before, uint32 vertex, uint32 fragment, const char *step)
{
	metal::StateStats now = metal::getStateStats();
	bool ok = true;
	for(uint32 i = 0; i < nelem(now.vertexBlockBinds); i++){
		uint32 v = now.vertexBlockBinds[i] - before.vertexBlockBinds[i];
		uint32 f = now.fragmentBlockBinds[i] - before.fragmentBlockBinds[i];
		uint32 wantV = vertex>>i & 1;
		uint32 wantF = fragment>>i & 1;
		if(v != wantV || f != wantF){
			Detail("  %s: block %u bound %u times on the vertex stage and %u on the fragment stage, expected %u and %u\n",
				step, i, v, f, wantV, wantF);
			ok = false;
		}
	}
	return ok;
}

bool
CheckOnlyReadBlocksBound(void)
{
	using namespace metal;
	const char *name = "a draw uploads and binds only the uniform blocks its shader reads";
	const char *src[] = { header_metal_src, testShaderSrc, nil };
	static const float custom[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
	Shader *sh = Shader::create(src, "testVS", "testFS", VARIANT_ALL);
	Shader *cs = CreateCustomConstantShader();
	bool ok = sh != nil && cs != nil;
	if(!ok)
		Detail("  test shaders did not compile\n");
	if(ok && !BeginTestEncoder()){
		Detail("  no test encoder\n");
		ok = false;
	}
	if(ok){
		UnblendedState saved = BeginUnblended();
		Matrix identity;
		identity.setIdentity();
		setWorldMatrix(&identity);

		StateStats s = getStateStats();
		im2dShader->use();
		ok &= flushCache() != 0;
		ok &= UploadsRose(s.blockUploads[BUFFER_OBJECT], BUFFER_OBJECT, 0, "im2d shader");
		ok &= BindsMatch(s, 1<<BUFFER_SCENE | 1<<BUFFER_STATE, 1<<BUFFER_STATE, "im2d shader");
		uint32 scene = getStateStats().blockUploads[BUFFER_SCENE] - s.blockUploads[BUFFER_SCENE];
		if(scene > 1){
			Detail("  im2d shader: scene block uploaded %u times\n", scene);
			ok = false;
		}

		s = getStateStats();
		sh->use(VARIANT_DIRECTIONALS);
		ok &= flushCache() != 0;
		ok &= UploadsRose(s.blockUploads[BUFFER_OBJECT], BUFFER_OBJECT, 1, "lighting shader");
		ok &= BindsMatch(s, 1<<BUFFER_STATE | 1<<BUFFER_SCENE | 1<<BUFFER_OBJECT | 1<<BUFFER_LIGHTS,
			1<<BUFFER_MATERIAL, "lighting shader");

		s = getStateStats();
		setCustomConstants(custom, sizeof(custom));
		cs->use();
		ok &= flushCache() != 0;
		ok &= UploadsRose(s.blockUploads[BUFFER_CUSTOM], BUFFER_CUSTOM, 1, "custom constant shader");
		ok &= BindsMatch(s, 1<<BUFFER_SCENE, 1<<BUFFER_CUSTOM, "custom constant shader");

		EndTestEncoder();
		EndUnblended(saved);
	}
	if(sh)
		sh->destroy();
	if(cs)
		cs->destroy();
	return Report(ok, name);
}

static bool
TextureBindsRose(uint32 before, uint32 want, const char *step)
{
	uint32 now = metal::getStateStats().textureStageBinds;
	if(now - before == want)
		return true;
	Detail("  %s: %u texture stages bound, expected %u\n", step, now - before, want);
	return false;
}

bool
CheckOnlySampledTexturesBound(void)
{
	using namespace metal;
	const char *name = "a draw binds a texture and sampler only when its shader samples them";
	Shader *cs = CreateDefaultAttribShader();
	bool ok = cs != nil;
	if(!ok)
		Detail("  test shader did not compile\n");
	if(ok && !BeginTestEncoder()){
		Detail("  no test encoder\n");
		ok = false;
	}
	if(ok){
		UnblendedState saved = BeginUnblended();
		uint32 before = getStateStats().textureStageBinds;
		cs->use();
		ok &= flushCache() != 0;
		ok &= TextureBindsRose(before, 0, "shader without a texture");
		before = getStateStats().textureStageBinds;
		im2dShader->use();
		ok &= flushCache() != 0;
		ok &= TextureBindsRose(before, 1, "im2d shader");
		EndTestEncoder();
		EndUnblended(saved);
	}
	if(cs)
		cs->destroy();
	return Report(ok, name);
}

bool
CheckDefaultShader(void)
{
	using namespace metal;
	const char *name = "default shader compiles in all variants and reads the world blocks";
	if(defaultShader == nil){
		Detail("  no default shader\n");
		return Report(false, name);
	}
	bool ok = AllVariants(defaultShader, "default");
	if(defaultShader_noAT != defaultShader || defaultShader_fullLight != defaultShader ||
	   defaultShader_fullLight_noAT != defaultShader){
		Detail("  the noAT and fullLight names do not point at the default shader\n");
		ok = false;
	}
	const uint32 want = 1<<BUFFER_SCENE | 1<<BUFFER_OBJECT | 1<<BUFFER_LIGHTS |
		1<<BUFFER_MATERIAL | 1<<BUFFER_STATE;
	uint32 before = getStateStats().blockSizeMismatches;
	uint32 used = checkShaderBlockSizes(defaultShader, VARIANT_ALL);
	uint32 mismatches = getStateStats().blockSizeMismatches - before;
	if(used != want || mismatches != 0){
		Detail("  blocks used 0x%x, expected 0x%x, mismatches %u\n", used, want, mismatches);
		ok = false;
	}
	return Report(ok, name);
}

static bool
RingProbe(metal::RingSpace *space)
{
	return metal::ringAlloc(1, 256, space) != 0;
}

static uint32
RingBytesSince(const metal::RingSpace &before)
{
	metal::RingSpace after;
	if(!RingProbe(&after) || after.buffer != before.buffer)
		return ~0u;
	return after.offset - before.offset - 256;
}

bool
CheckWorldBlocksUploadOnChange(void)
{
	using namespace metal;
	const char *name = "world matrix, lights and material upload only when they change";
	if(defaultShader == nil){
		Detail("  no default shader\n");
		return Report(false, name);
	}
	InstAttrib inst[MAXINSTATTRIBS];
	AttribDesc attribs[MAXINSTATTRIBS];
	int32 n = defaultVertexLayout(false, false, 0, inst);
	memcpy(attribs, inst, n*sizeof(AttribDesc));
	uint32 layout = registerVertexLayout(attribs, n);

	Light *amb = Light::create(Light::AMBIENT);
	Light *dir = Light::create(Light::DIRECTIONAL);
	amb->setFrame(Frame::create());
	dir->setFrame(Frame::create());
	amb->setColor(0.2f, 0.2f, 0.2f);
	dir->setColor(0.8f, 0.7f, 0.6f);
	Light *directionals[1] = { dir };
	WorldLights wl = {};
	wl.numAmbients = 1;
	wl.ambient = amb->color;
	wl.numDirectionals = 1;
	wl.directionals = directionals;

	if(!BeginTestEncoder()){
		Detail("  no test encoder\n");
		return Report(false, name);
	}
	UnblendedState saved = BeginUnblended();
	setVertexLayout(layout);
	defaultShader->use(0);
	bool ok = true;
	StateStats s;
	RingSpace probe;
	uint32 perAtomic = ~0u, perMesh = ~0u;

	s = getStateStats();
	int32 bits = setLights(&wl);
	if(bits != (VSLIGHT_AMBIENT | VSLIGHT_DIRECT)){
		Detail("  setLights returned 0x%x for an ambient and a directional, expected 0x%x\n",
		       bits, VSLIGHT_AMBIENT | VSLIGHT_DIRECT);
		ok = false;
	}
	ok &= flushCache() != 0;
	ok &= UploadsRose(s.blockUploads[BUFFER_LIGHTS], BUFFER_LIGHTS, 1, "new lights");
	s = getStateStats();
	setLights(&wl);
	ok &= flushCache() != 0;
	ok &= UploadsRose(s.blockUploads[BUFFER_LIGHTS], BUFFER_LIGHTS, 0, "same lights");
	s = getStateStats();
	wl.ambient.red = 0.5f;
	setLights(&wl);
	ok &= flushCache() != 0;
	ok &= UploadsRose(s.blockUploads[BUFFER_LIGHTS], BUFFER_LIGHTS, 1, "ambient changed");

	Light *nine[9];
	for(int i = 0; i < 9; i++){
		nine[i] = Light::create(Light::DIRECTIONAL);
		nine[i]->setFrame(Frame::create());
		nine[i]->setColor(0.1f*(i+1), 0.0f, 0.0f);
	}
	WorldLights wl9 = {};
	wl9.numDirectionals = 9;
	wl9.directionals = nine;
	s = getStateStats();
	ok &= RingProbe(&probe);
	bits = setLights(&wl9);
	ok &= flushCache() != 0;
	ok &= UploadsRose(s.blockUploads[BUFFER_LIGHTS], BUFFER_LIGHTS, 1, "nine directionals");
	uint32 uploads = 0;
	StateStats s9 = getStateStats();
	for(int i = 0; i < (int)nelem(s9.blockUploads); i++)
		uploads += s9.blockUploads[i] - s.blockUploads[i];
	uint32 lightBytes = RingBytesSince(probe);
	if(bits != VSLIGHT_DIRECT || uploads != 1 || lightBytes != 768){
		Detail("  nine directionals: bits 0x%x, %u block uploads, %u ring bytes, expected 0x%x, 1 and 768\n",
		       bits, uploads, lightBytes, VSLIGHT_DIRECT);
		ok = false;
	}else{
		const float32 *block = (const float32*)(probe.cpu + 256);
		for(int i = 0; i < 8; i++){
			float32 type = block[4 + i*4];
			float32 red = block[4 + 32*3 + i*4];
			if(type != 1.0f || red != nine[i]->color.red){
				Detail("  nine directionals: slot %d has type %g, red %g, expected 1 and %g\n",
				       i, type, red, nine[i]->color.red);
				ok = false;
			}
		}
	}
	for(int i = 0; i < 9; i++){
		Frame *f = nine[i]->getFrame();
		nine[i]->destroy();
		f->destroy();
	}

	RGBA col = { 10, 20, 30, 40 };
	SurfaceProperties surf = { 0.3f, 0.0f, 0.9f };
	s = getStateStats();
	ok &= RingProbe(&probe);
	setMaterial(col, surf);
	ok &= flushCache() != 0;
	perMesh = RingBytesSince(probe);
	ok &= UploadsRose(s.blockUploads[BUFFER_MATERIAL], BUFFER_MATERIAL, 1, "new material");
	s = getStateStats();
	setMaterial(col, surf);
	ok &= flushCache() != 0;
	ok &= UploadsRose(s.blockUploads[BUFFER_MATERIAL], BUFFER_MATERIAL, 0, "same material");

	Matrix m;
	m.setIdentity();
	m.pos.set(1.0f, 2.0f, 3.0f);
	s = getStateStats();
	ok &= RingProbe(&probe);
	setWorldMatrix(&m);
	ok &= flushCache() != 0;
	perAtomic = RingBytesSince(probe);
	ok &= UploadsRose(s.blockUploads[BUFFER_OBJECT], BUFFER_OBJECT, 1, "new world matrix");
	s = getStateStats();
	setWorldMatrix(&m);
	ok &= flushCache() != 0;
	ok &= UploadsRose(s.blockUploads[BUFFER_OBJECT], BUFFER_OBJECT, 0, "same world matrix");

	if(perAtomic > 256 || perMesh > 256){
		Detail("  ring bytes per atomic %u, per changed material %u, expected at most 256\n", perAtomic, perMesh);
		ok = false;
	}

	Matrix identity;
	identity.setIdentity();
	setWorldMatrix(&identity);
	WorldLights none = {};
	setLights(&none);
	RGBA zero = { 0, 0, 0, 0 };
	SurfaceProperties zeroSurf = { 0.0f, 0.0f, 0.0f };
	setMaterial(zero, zeroSurf);

	EndTestEncoder();
	EndUnblended(saved);
	Frame *f = amb->getFrame();
	amb->destroy();
	f->destroy();
	f = dir->getFrame();
	dir->destroy();
	f->destroy();
	return Report(ok, name);
}

metal::Shader*
CreateDefaultAttribShader(void)
{
	const char *src[] = { metal::header_metal_src, attribShaderSrc, nil };
	return metal::Shader::create(src, "defaultAttribVS", "colorFS", 0);
}

metal::Shader*
CreateCustomConstantShader(void)
{
	const char *src[] = { metal::header_metal_src, attribShaderSrc, nil };
	return metal::Shader::create(src, "plainVS", "customFS", 0);
}

static const char *uv2ShaderSrc =
"fragment float4 uv2CoordFS(FragmentInUV2 in [[stage_in]]) {\n"
"	return float4(in.tex1.x, in.tex1.y, in.tex0.x, 1.0);\n"
"}\n"
"fragment float4 uv2TexFS(FragmentInUV2 in [[stage_in]],\n"
"                         texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],\n"
"                         texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]]) {\n"
"	return in.color*tex0.sample(s0, in.tex0)*tex1.sample(s1, in.tex1);\n"
"}\n";

metal::Shader*
CreateUV2CoordShader(void)
{
	const char *src[] = { metal::header_metal_src, metal::im2d_uv2_metal_src, uv2ShaderSrc, nil };
	return metal::Shader::create(src, "im2dUV2VS", "uv2CoordFS", 0);
}

metal::Shader*
CreateUV2TextureShader(void)
{
	const char *src[] = { metal::header_metal_src, metal::im2d_uv2_metal_src, uv2ShaderSrc, nil };
	return metal::Shader::create(src, "im2dUV2VS", "uv2TexFS", 0);
}

static const char *twoUVSrc =
"struct TwoUVIn {\n"
"	float3 pos [[attribute(ATTRIB_POS)]];\n"
"	float4 color [[attribute(ATTRIB_COLOR)]];\n"
"	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];\n"
"	float2 tex1 [[attribute(ATTRIB_TEXCOORDS1)]];\n"
"};\n"
"struct TwoUVOut { float4 position [[position, invariant]]; float4 color; float2 tex0; float2 tex1; };\n"
"struct TwoUVFragIn { float4 position [[position]]; float4 color; float2 tex0; float2 tex1; };\n"
"vertex TwoUVOut twoUVVS(TwoUVIn in [[stage_in]], constant Scene &scene [[buffer(BUFFER_SCENE)]],\n"
"                        constant Object &object [[buffer(BUFFER_OBJECT)]]) {\n"
"	TwoUVOut out;\n"
"	float4 V = object.world * float4(in.pos, 1.0);\n"
"	out.position = MetalDepth(scene.proj * scene.view * V);\n"
"	out.color = in.color;\n"
"	out.tex0 = in.tex0;\n"
"	out.tex1 = in.tex1;\n"
"	return out;\n"
"}\n"
"fragment float4 twoUVFS(TwoUVFragIn in [[stage_in]],\n"
"                        texture2d<float> tex0 [[texture(0)]], sampler s0 [[sampler(0)]],\n"
"                        texture2d<float> tex1 [[texture(1)]], sampler s1 [[sampler(1)]]) {\n"
"	return in.color*tex0.sample(s0, in.tex0)*tex1.sample(s1, in.tex1);\n"
"}\n";

static const char *secondPassSrc =
"struct SecondPassOut { float4 position [[position, invariant]]; };\n"
"struct SecondPassIn { float4 position [[position]]; };\n"
"vertex SecondPassOut secondPassVS(DefaultIn in [[stage_in]], constant Scene &scene [[buffer(BUFFER_SCENE)]],\n"
"                                  constant Object &object [[buffer(BUFFER_OBJECT)]]) {\n"
"	SecondPassOut out;\n"
"	float4 V = object.world * float4(in.pos, 1.0);\n"
"	out.position = scene.proj * scene.view * V;\n"
"	out.position = MetalDepth(out.position);\n"
"	return out;\n"
"}\n"
"fragment float4 secondPassFS(SecondPassIn in [[stage_in]]) {\n"
"	return float4(0.0, 0.0, 0.25, 0.0);\n"
"}\n";

metal::Shader*
CreateHostDefaultShader(void)
{
	const char *src[] = { metal::header_metal_src, metal::default_metal_src, metal::simple_metal_src, nil };
	return metal::Shader::create(src, "defaultVS", "simpleFS", metal::VARIANT_ALL);
}

metal::Shader*
CreateHostSkinShader(void)
{
	const char *src[] = { metal::header_metal_src, metal::skin_metal_src, metal::simple_metal_src, nil };
	return metal::Shader::create(src, "skinVS", "simpleFS", metal::VARIANT_ALL);
}

metal::Shader*
CreateTwoUVShader(void)
{
	const char *src[] = { metal::header_metal_src, twoUVSrc, nil };
	return metal::Shader::create(src, "twoUVVS", "twoUVFS", 0);
}

metal::Shader*
CreateSecondPassShader(void)
{
	const char *src[] = { metal::header_metal_src, metal::default_metal_src, secondPassSrc, nil };
	return metal::Shader::create(src, "secondPassVS", "secondPassFS", 0);
}

metal::Shader*
CreateOversizedCustomShader(void)
{
	const char *src[] = { metal::header_metal_src, attribShaderSrc, nil };
	return metal::Shader::create(src, "plainVS", "bigCustomFS", 0);
}

metal::Shader*
CreateBrokenShader(void)
{
	const char *src[] = { metal::header_metal_src, brokenShaderSrc, nil };
	return metal::Shader::create(src, "brokenVS", "brokenFS", 0);
}
