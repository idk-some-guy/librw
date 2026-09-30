#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include <src/metal/metalinst.h>
#include "world_checks.h"
#include "state_checks.h"
#include "objc_checks.h"

using namespace rw;

static char details[8192];
static int detailsLen = 0;
static int failures = 0;

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
	fflush(stdout);
	details[0] = '\0';
	detailsLen = 0;
	if(!ok)
		failures++;
	return ok;
}

struct SavedCamera
{
	Camera *camera;
	float32 nearPlane, farPlane, fogPlane;
	V2d viewWindow;
	World *world;
};
static SavedCamera saved;

void
WorldOpen(Camera *camera)
{
	saved.camera = camera;
	saved.nearPlane = camera->nearPlane;
	saved.farPlane = camera->farPlane;
	saved.fogPlane = camera->fogPlane;
	saved.viewWindow = camera->viewWindow;
	V2d window = { 1.0f, 0.75f };
	camera->setNearPlane(1.0f);
	camera->setFarPlane(101.0f);
	camera->fogPlane = 51.0f;
	camera->setViewWindow(&window);
	saved.world = World::create();
	saved.world->addCamera(camera);
}

void
WorldClose(void)
{
	Camera *camera = saved.camera;
	saved.world->removeCamera(camera);
	saved.world->destroy();
	saved.world = nil;
	camera->setNearPlane(saved.nearPlane);
	camera->setFarPlane(saved.farPlane);
	camera->fogPlane = saved.fogPlane;
	camera->setViewWindow(&saved.viewWindow);
}

World*
TestWorld(void)
{
	return saved.world;
}

static Geometry*
BuildQuad(uint32 flags, float x0, float y0, float x1, float y1, float z,
          const RGBA col[4], const TexCoords uv[4], Material *mat0, Material *mat1)
{
	Geometry *geo = Geometry::create(4, 2, flags);
	if(geo == nil)
		return nil;
	V3d *v = geo->morphTargets[0].vertices;
	v[0].set(x0, y1, z);
	v[1].set(x1, y1, z);
	v[2].set(x1, y0, z);
	v[3].set(x0, y0, z);
	if(flags & Geometry::NORMALS)
		for(int i = 0; i < 4; i++)
			geo->morphTargets[0].normals[i].set(0.0f, 0.0f, -1.0f);
	if(geo->colors && col)
		for(int i = 0; i < 4; i++)
			geo->colors[i] = col[i];
	if(geo->texCoords[0] && uv)
		for(int i = 0; i < 4; i++)
			geo->texCoords[0][i] = uv[i];
	geo->matList.appendMaterial(mat0);
	if(mat1 != mat0)
		geo->matList.appendMaterial(mat1);
	Triangle *t = geo->triangles;
	t[0].v[0] = 0; t[0].v[1] = 1; t[0].v[2] = 2; t[0].matId = 0;
	t[1].v[0] = 0; t[1].v[1] = 2; t[1].v[2] = 3; t[1].matId = mat1 != mat0 ? 1 : 0;
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

Geometry*
QuadGeometry(uint32 flags, float x0, float y0, float x1, float y1, float z,
             const RGBA col[4], const TexCoords uv[4], Material *mat)
{
	return BuildQuad(flags, x0, y0, x1, y1, z, col, uv, mat, mat);
}

Geometry*
SplitQuadGeometry(uint32 flags, float x0, float y0, float x1, float y1, float z,
                  const RGBA col[4], const TexCoords uv[4], Material *mat0, Material *mat1)
{
	return BuildQuad(flags, x0, y0, x1, y1, z, col, uv, mat0, mat1);
}

Atomic*
MakeAtomic(Geometry *geo)
{
	Atomic *atomic = Atomic::create();
	Frame *frame = Frame::create();
	atomic->setFrame(frame);
	atomic->setGeometry(geo, 0);
	return atomic;
}

void
DestroyAtomic(Atomic *atomic)
{
	Frame *frame = atomic->getFrame();
	atomic->destroy();
	if(frame)
		frame->destroy();
}

void
WorldBegin(RGBA col)
{
	WorldBeginMode(col, Camera::CLEARIMAGE | Camera::CLEARZ);
}

void
WorldBeginMode(RGBA col, uint32 clearMode)
{
	Camera *camera = saved.camera;
	camera->clear(&col, clearMode);
	camera->beginUpdate();
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(FOGENABLE, 0);
	SetRenderState(CULLMODE, CULLNONE);
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(ALPHATESTREF, 3);
	SetRenderState(GSALPHATEST, 0);
	SetRenderStatePtr(TEXTURERASTER, nil);
}

void
WorldEnd(void)
{
	saved.camera->endUpdate();
	saved.camera->showRaster(0);
}

Image*
ReadCamera(void)
{
	Image *img = saved.camera->frameBuffer->toImage();
	if(img == nil)
		Detail("  toImage returned nil\n");
	else if(img->depth != 32 || img->bpp != 4 || img->stride != img->width*4){
		Detail("  image layout depth %d bpp %d stride %d\n", img->depth, img->bpp, img->stride);
		img->destroy();
		return nil;
	}
	return img;
}

bool
Near(Image *img, int x, int y, RGBA want, int tol)
{
	uint8 *p = &img->pixels[y*img->stride + x*4];
	int w[4] = { want.red, want.green, want.blue, want.alpha };
	for(int i = 0; i < 4; i++){
		int d = p[i] - w[i];
		if(d < -tol || d > tol){
			Detail("  pixel (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d within %d\n", x, y,
			       p[0], p[1], p[2], p[3], want.red, want.green, want.blue, want.alpha, tol);
			return false;
		}
	}
	return true;
}

Texture*
MakeTexture(int w, int h, const RGBA *texels, int32 filter, int32 addressU, int32 addressV)
{
	Image *img = Image::create(w, h, 32);
	img->allocate();
	for(int y = 0; y < h; y++)
		memcpy(img->pixels + y*img->stride, &texels[y*w], w*4);
	Raster *ras = Raster::create(w, h, 32, Raster::C8888 | Raster::TEXTURE);
	if(ras && ras->setFromImage(img) == nil){
		ras->destroy();
		ras = nil;
	}
	img->destroy();
	if(ras == nil){
		Detail("  a %dx%d texture could not be made\n", w, h);
		return nil;
	}
	Texture *tex = Texture::create(ras);
	tex->setFilter((Texture::FilterMode)filter);
	tex->setAddressU((Texture::Addressing)addressU);
	tex->setAddressV((Texture::Addressing)addressV);
	return tex;
}

static metal::InstanceDataHeader*
Instanced(Atomic *atomic)
{
	atomic->getPipeline()->instance(atomic);
	metal::InstanceDataHeader *header = (metal::InstanceDataHeader*)atomic->geometry->instData;
	if(header == nil)
		Detail("  the geometry has no instance data\n");
	return header;
}

static bool
Expect(bool cond, const char *fmt, ...)
{
	if(cond)
		return true;
	va_list ap;
	va_start(ap, fmt);
	char line[256];
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	Detail("  %s\n", line);
	return false;
}

static bool
CheckInstanceTwoMaterials(void)
{
	const char *name = "instancing two materials gives two meshes over one interleaved vertex buffer";
	static const RGBA col[4] = {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 10, 20, 30, 255 }, { 0, 0, 255, 128 } };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.25f, 0.75f }, { 0.0f, 1.0f } };
	Material *m0 = Material::create();
	Material *m1 = Material::create();
	Geometry *geo = BuildQuad(Geometry::PRELIT | Geometry::TEXTURED | Geometry::NORMALS | Geometry::POSITIONS,
	                          2.0f, -1.0f, -2.0f, 1.5f, 10.0f, col, uv, m0, m1);
	m0->destroy();
	m1->destroy();
	geo->morphTargets[0].normals[2].set(0.6f, 0.0f, -0.8f);
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil;
	if(h){
		ok &= Expect(h->platform == PLATFORM_METAL, "platform %d, expected %d", h->platform, PLATFORM_METAL);
		ok &= Expect(h->numMeshes == 2, "%u meshes, expected 2", h->numMeshes);
		ok &= Expect(h->primType == 3, "primType %u, expected 3", h->primType);
		ok &= Expect(h->numAttribs == 4, "%d attribs, expected 4", h->numAttribs);
		ok &= Expect(h->numAttribs > 0 && h->attribDesc[0].stride == 36, "stride %u, expected 36",
		             h->numAttribs > 0 ? h->attribDesc[0].stride : 0);
		ok &= Expect(h->vertexLayout != 0, "no vertex layout");
		if(h->numMeshes == 2){
			ok &= Expect(h->inst[0].offset == 0, "mesh 0 offset %u, expected 0", h->inst[0].offset);
			ok &= Expect(h->inst[1].offset == 8, "mesh 1 offset %u, expected 8", h->inst[1].offset);
			ok &= Expect(h->inst[0].numIndex == 3 && h->inst[1].numIndex == 3, "index counts %u, %u, expected 3, 3",
			             h->inst[0].numIndex, h->inst[1].numIndex);
			ok &= Expect(!h->inst[0].vertexAlpha, "mesh 0 has vertex alpha");
			ok &= Expect(h->inst[1].vertexAlpha, "mesh 1 has no vertex alpha");
		}
		float pos[3], nrm[3], texc[2];
		uint8 rgba[4];
		bool read = BufferBytes(h->mtlVertexBuffer, 2*36, pos, 12) &&
		            BufferBytes(h->mtlVertexBuffer, 2*36+12, nrm, 12) &&
		            BufferBytes(h->mtlVertexBuffer, 2*36+24, rgba, 4) &&
		            BufferBytes(h->mtlVertexBuffer, 2*36+28, texc, 8);
		ok &= Expect(read, "the vertex buffer could not be read at vertex 2");
		if(read){
			ok &= Expect(pos[0] == -2.0f && pos[1] == -1.0f && pos[2] == 10.0f,
			             "vertex 2 position %g,%g,%g, expected -2,-1,10", pos[0], pos[1], pos[2]);
			ok &= Expect(nrm[0] == 0.6f && nrm[1] == 0.0f && nrm[2] == -0.8f,
			             "vertex 2 normal %g,%g,%g, expected 0.6,0,-0.8", nrm[0], nrm[1], nrm[2]);
			ok &= Expect(rgba[0] == 10 && rgba[1] == 20 && rgba[2] == 30 && rgba[3] == 255,
			             "vertex 2 colour %d,%d,%d,%d, expected 10,20,30,255", rgba[0], rgba[1], rgba[2], rgba[3]);
			ok &= Expect(texc[0] == 0.25f && texc[1] == 0.75f, "vertex 2 uv %g,%g, expected 0.25,0.75", texc[0], texc[1]);
		}
		uint16 pad = 0xFFFF, idx[3];
		Mesh *mesh1 = &geo->meshHeader->getMeshes()[1];
		read = BufferBytes(h->mtlIndexBuffer, 6, &pad, 2) && BufferBytes(h->mtlIndexBuffer, 8, idx, 6);
		ok &= Expect(read, "the index buffer could not be read");
		if(read){
			ok &= Expect(pad == 0, "padding after mesh 0 is %u, expected 0", pad);
			ok &= Expect(memcmp(idx, mesh1->indices, 6) == 0, "mesh 1 indices %u,%u,%u, expected %u,%u,%u",
			             idx[0], idx[1], idx[2], mesh1->indices[0], mesh1->indices[1], mesh1->indices[2]);
		}
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckInstanceStrip(void)
{
	const char *name = "instancing a tristrip geometry draws strips";
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::TRISTRIP | Geometry::POSITIONS,
	                             2.0f, -1.0f, -2.0f, 1.5f, 10.0f, nil, nil, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil;
	if(h){
		Mesh *mesh = geo->meshHeader->getMeshes();
		ok &= Expect(geo->meshHeader->flags == 1, "mesh header flags %u, expected 1", geo->meshHeader->flags);
		ok &= Expect(h->primType == 4, "primType %u, expected 4", h->primType);
		ok &= Expect(h->numMeshes == geo->meshHeader->numMeshes && h->numMeshes > 0, "%u meshes", h->numMeshes);
		if(h->numMeshes > 0)
			ok &= Expect(h->inst[0].numIndex == mesh->numIndices, "%u indices, expected %u",
			             h->inst[0].numIndex, mesh->numIndices);
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckReinstance(void)
{
	const char *name = "a relocked geometry is reinstanced into a new vertex buffer, keeping attributes not relocked";
	static const RGBA col[4] = {
		{ 11, 22, 33, 44 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 0, 0, 255 } };
	static const TexCoords uv[4] = { { 0.125f, 0.625f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             2.0f, -1.0f, -2.0f, 1.5f, 10.0f, col, uv, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil;
	if(h){
		void *before = h->mtlVertexBuffer;
		geo->colors[0].red = 99;
		geo->texCoords[0][0].u = 0.5f;
		geo->lock(Geometry::LOCKVERTICES);
		geo->morphTargets[0].vertices[0].x = 7.5f;
		geo->unlock();
		metal::InstanceDataHeader *h2 = Instanced(atomic);
		ok &= Expect(h2 == h, "the instance header was replaced");
		if(h2 == h){
			ok &= Expect(h->mtlVertexBuffer != nil && h->mtlVertexBuffer != before, "the vertex buffer was not replaced");
			float x = 0.0f, texc[2] = { 0.0f, 0.0f };
			uint8 rgba[4] = { 0, 0, 0, 0 };
			ok &= Expect(BufferBytes(h->mtlVertexBuffer, 0, &x, 4) && x == 7.5f, "vertex 0 x is %g, expected 7.5", x);
			ok &= Expect(BufferBytes(h->mtlVertexBuffer, 12, rgba, 4) &&
			             rgba[0] == 11 && rgba[1] == 22 && rgba[2] == 33 && rgba[3] == 44,
			             "vertex 0 colour %d,%d,%d,%d, expected the old 11,22,33,44", rgba[0], rgba[1], rgba[2], rgba[3]);
			ok &= Expect(BufferBytes(h->mtlVertexBuffer, 16, texc, 8) && texc[0] == 0.125f && texc[1] == 0.625f,
			             "vertex 0 uv %g,%g, expected the old 0.125,0.625", texc[0], texc[1]);
		}
		ok &= Expect(geo->lockedSinceInst == 0, "lockedSinceInst is 0x%x", geo->lockedSinceInst);
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

struct DestroyArgs
{
	bool instanced;
	bool watchIndex;
};

static void
InstanceAndDestroy(void *arg)
{
	DestroyArgs *args = (DestroyArgs*)arg;
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::POSITIONS, 2.0f, -1.0f, -2.0f, 1.5f, 10.0f, nil, nil, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	args->instanced = h != nil && h->mtlVertexBuffer != nil && h->mtlIndexBuffer != nil;
	if(args->instanced)
		WatchObject(args->watchIndex ? h->mtlIndexBuffer : h->mtlVertexBuffer);
	DestroyAtomic(atomic);
	geo->destroy();
}

static bool
CheckDestroyNativeData(void)
{
	const char *name = "destroying a geometry releases its vertex and index buffers";
	bool ok = true;
	for(int i = 0; i < 2; i++){
		DestroyArgs args = { false, i == 1 };
		RunInPool(InstanceAndDestroy, &args);
		ok &= Expect(args.instanced, "the geometry was not instanced");
		if(args.instanced)
			ok &= Expect(!WatchedObjectAlive(), "the %s buffer is still alive", i == 1 ? "index" : "vertex");
		WatchObject(nil);
	}
	return Report(ok, name);
}

static const RGBA RED = { 255, 0, 0, 255 };
static const RGBA GREEN = { 0, 255, 0, 255 };
static const RGBA BLUE = { 0, 0, 255, 255 };
static const RGBA GREY = { 128, 128, 128, 255 };
static const RGBA WHITE = { 255, 255, 255, 255 };
static const RGBA BLACK = { 0, 0, 0, 255 };

static Geometry*
SolidQuad(float x0, float y0, float x1, float y1, float z, RGBA c)
{
	RGBA col[4] = { c, c, c, c };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS, x0, y0, x1, y1, z, col, nil, mat);
	mat->destroy();
	return geo;
}

static Geometry*
Square(float half, float z, RGBA c)
{
	return SolidQuad(half, -half, -half, half, z, c);
}

static void
RenderGeometry(Geometry *geo)
{
	Atomic *atomic = MakeAtomic(geo);
	atomic->render();
	DestroyAtomic(atomic);
}

static void
RenderSquare(float half, float z, RGBA c)
{
	Geometry *geo = Square(half, z, c);
	RenderGeometry(geo);
	geo->destroy();
}

static Image*
RenderOne(Atomic *atomic)
{
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	return ReadCamera();
}

static int
ScreenX(float x, float z)
{
	return (int)floorf(320.0f*(1.0f - x/z));
}

static int
ScreenY(float y, float z)
{
	return (int)floorf(240.0f*(1.0f - y/(0.75f*z)));
}

static float
DepthOf(float z)
{
	const float n = 1.0f, f = 101.0f;
	return 0.5f*(((f+n)*z - 2.0f*f*n)/((f-n)*z) + 1.0f);
}

static bool
DepthNear(int x, int y, float want, float tol)
{
	float d = -1.0f;
	if(!metal::readDepthPixel(saved.camera->zBuffer, x, y, &d)){
		Detail("  depth at (%d,%d) could not be read\n", x, y);
		return false;
	}
	if(!(fabsf(d - want) <= tol)){
		Detail("  depth at (%d,%d) is %.7f, expected %.7f within %g\n", x, y, d, want, tol);
		return false;
	}
	return true;
}

static bool
CheckAxesAndWorldMatrix(void)
{
	const char *name = "a quad lands where the camera maths puts it, and its frame moves it";
	Geometry *geo = SolidQuad(3.0f, 1.5f, 1.0f, 3.0f, 10.0f, RED);
	Atomic *atomic = MakeAtomic(geo);
	bool ok = true;
	Image *img = RenderOne(atomic);
	if(img){
		ok &= Near(img, 256, 168, RED, 0);
		ok &= Near(img, 384, 168, GREY, 0);
		ok &= Near(img, 256, 312, GREY, 0);
		ok &= Near(img, 218, 168, GREY, 0);
		ok &= Near(img, 294, 168, GREY, 0);
		img->destroy();
	}else
		ok = false;
	V3d right = { 1.0f, 0.0f, 0.0f };
	atomic->getFrame()->translate(&right);
	img = RenderOne(atomic);
	if(img){
		ok &= Near(img, 208, 168, RED, 0);
		ok &= Near(img, 280, 168, GREY, 0);
		img->destroy();
	}else
		ok = false;
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckRotatedWorldMatrix(void)
{
	const char *name = "a rotated and translated atomic covers the pixels and depths computed on the CPU";
	Geometry *geo = Square(1.0f, 0.0f, RED);
	Atomic *atomic = MakeAtomic(geo);
	Frame *f = atomic->getFrame();
	V3d zAxis = { 0.0f, 0.0f, 1.0f }, yAxis = { 0.0f, 1.0f, 0.0f }, pos = { 1.0f, 0.5f, 20.0f };
	f->rotate(&zAxis, 30.0f);
	f->rotate(&yAxis, 40.0f);
	f->translate(&pos);
	Matrix *m = f->getLTM();
	bool ok = true;
	Image *img = RenderOne(atomic);
	if(img){
		static const float pts[][3] = {
			{ 0.0f, 0.0f, 1 }, { 0.7f, 0.7f, 1 }, { -0.7f, 0.5f, 1 }, { 0.5f, -0.7f, 1 },
			{ 1.4f, 0.0f, 0 }, { 0.0f, 1.4f, 0 }, { -1.4f, 0.0f, 0 }, { 0.0f, -1.4f, 0 } };
		for(int i = 0; i < (int)nelem(pts); i++){
			float mx = pts[i][0], my = pts[i][1];
			V3d w = { m->right.x*mx + m->up.x*my + m->pos.x,
			          m->right.y*mx + m->up.y*my + m->pos.y,
			          m->right.z*mx + m->up.z*my + m->pos.z };
			int px = ScreenX(w.x, w.z), py = ScreenY(w.y, w.z);
			bool inside = pts[i][2] != 0;
			ok &= Near(img, px, py, inside ? RED : GREY, 0);
			float want = 1.0f;
			if(inside){
				V3d d = { 1.0f - (px+0.5f)/320.0f, 0.75f*(1.0f - (py+0.5f)/240.0f), 1.0f };
				float t = dot(m->at, m->pos)/dot(m->at, d);
				want = DepthOf(t);
			}
			ok &= DepthNear(px, py, want, 2e-5f);
		}
		img->destroy();
	}else
		ok = false;
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckDepthRange(void)
{
	const char *name = "3D depth maps the near and far planes to 0 and 1 once";
	bool ok = true;
	Image *img;

	WorldBegin(GREY);
	RenderSquare(0.05f, 0.9f, RED);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, GREY, 0);
		img->destroy();
	}else
		ok = false;

	WorldBegin(GREY);
	RenderSquare(0.1f, 1.5f, RED);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, RED, 0);
		img->destroy();
	}else
		ok = false;

	WorldBegin(GREY);
	RenderSquare(5.0f, 51.0f, RED);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, RED, 0);
		img->destroy();
	}else
		ok = false;
	ok &= DepthNear(320, 240, 0.990196f, 1e-5f);

	WorldBegin(GREY);
	RenderSquare(5.0f, 100.0f, RED);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, RED, 0);
		img->destroy();
	}else
		ok = false;

	WorldBegin(GREY);
	RenderSquare(5.0f, 102.0f, RED);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, GREY, 0);
		img->destroy();
	}else
		ok = false;

	WorldBegin(GREY);
	RenderSquare(2.0f, 20.0f, RED);
	RenderSquare(8.0f, 40.0f, BLUE);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, RED, 0);
		ok &= Near(img, 270, 190, BLUE, 0);
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static bool
CheckDepthOrderAndWrite(void)
{
	const char *name = "overlapping quads in both orders, with depth test and depth write on and off";
	struct Case { bool nearFirst; bool ztest; bool zwrite; RGBA centre; float centreDepth; float outerDepth; };
	const Case cases[] = {
		{ true, true, true, RED, DepthOf(20.0f), DepthOf(40.0f) },
		{ false, true, true, RED, DepthOf(20.0f), DepthOf(40.0f) },
		{ true, true, false, BLUE, 1.0f, 1.0f },
		{ false, true, false, RED, 1.0f, 1.0f },
		{ true, false, true, BLUE, DepthOf(40.0f), DepthOf(40.0f) },
		{ true, false, false, BLUE, 1.0f, 1.0f },
	};
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		WorldBegin(GREY);
		SetRenderState(ZTESTENABLE, c.ztest);
		SetRenderState(ZWRITEENABLE, c.zwrite);
		if(c.nearFirst){
			RenderSquare(2.0f, 20.0f, RED);
			RenderSquare(8.0f, 40.0f, BLUE);
		}else{
			RenderSquare(8.0f, 40.0f, BLUE);
			RenderSquare(2.0f, 20.0f, RED);
		}
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		WorldEnd();
		Image *img = ReadCamera();
		if(img == nil){
			ok = false;
			continue;
		}
		bool caseOk = Near(img, 320, 240, c.centre, 0);
		caseOk &= Near(img, 270, 190, BLUE, 0);
		img->destroy();
		caseOk &= DepthNear(320, 240, c.centreDepth, 1e-5f);
		caseOk &= DepthNear(270, 190, c.outerDepth, 1e-5f);
		if(!caseOk)
			Detail("  in case %s first, depth test %s, depth write %s\n", c.nearFirst ? "near" : "far",
			       c.ztest ? "on" : "off", c.zwrite ? "on" : "off");
		ok &= caseOk;
	}
	return Report(ok, name);
}

static Geometry*
TriangleGeometry(const V3d *p, const uint16 *order, RGBA c)
{
	Geometry *geo = Geometry::create(3, 1, Geometry::PRELIT | Geometry::POSITIONS);
	for(int i = 0; i < 3; i++){
		geo->morphTargets[0].vertices[i] = p[i];
		geo->colors[i] = c;
	}
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	Triangle *t = geo->triangles;
	t->v[0] = order[0];
	t->v[1] = order[1];
	t->v[2] = order[2];
	t->matId = 0;
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
CheckWindingAndCulling(void)
{
	const char *name = "3D culling follows gl3: counter-clockwise faces the camera";
	static const V3d p[3] = { { 1.0f, -1.0f, 10.0f }, { -1.0f, -1.0f, 10.0f }, { 0.0f, 1.0f, 10.0f } };
	static const uint16 ccw[3] = { 0, 1, 2 }, cw[3] = { 0, 2, 1 };
	struct Case { const uint16 *order; int32 cull; RGBA want; const char *what; };
	const Case cases[] = {
		{ ccw, CULLNONE, RED, "counter-clockwise, cull none" },
		{ ccw, CULLBACK, RED, "counter-clockwise, cull back" },
		{ ccw, CULLFRONT, GREY, "counter-clockwise, cull front" },
		{ cw, CULLNONE, RED, "clockwise, cull none" },
		{ cw, CULLBACK, GREY, "clockwise, cull back" },
		{ cw, CULLFRONT, RED, "clockwise, cull front" },
	};
	bool ok = true;
	Image *img;
	for(int i = 0; i < (int)nelem(cases); i++){
		Geometry *geo = TriangleGeometry(p, cases[i].order, RED);
		WorldBegin(GREY);
		SetRenderState(CULLMODE, cases[i].cull);
		RenderGeometry(geo);
		WorldEnd();
		geo->destroy();
		if((img = ReadCamera())){
			if(!Near(img, 320, 250, cases[i].want, 0)){
				Detail("  in case %s\n", cases[i].what);
				ok = false;
			}
			img->destroy();
		}else
			ok = false;
	}

	Geometry *quad = SolidQuad(-2.0f, -2.0f, 2.0f, 2.0f, 0.0f, RED);
	Atomic *atomic = MakeAtomic(quad);
	V3d away = { 0.0f, 0.0f, 10.0f }, yAxis = { 0.0f, 1.0f, 0.0f };
	for(int turned = 0; turned < 2; turned++){
		Frame *f = atomic->getFrame();
		f->matrix.setIdentity();
		if(turned)
			f->rotate(&yAxis, 180.0f);
		f->translate(&away);
		const int32 culls[3] = { CULLNONE, CULLBACK, CULLFRONT };
		for(int c = 0; c < 3; c++){
			bool visible = culls[c] == CULLNONE || (culls[c] == CULLBACK) == !turned;
			WorldBegin(GREY);
			SetRenderState(CULLMODE, culls[c]);
			atomic->render();
			WorldEnd();
			if((img = ReadCamera())){
				if(!Near(img, 320, 240, visible ? RED : GREY, 0)){
					Detail("  in case quad %s the camera, cull mode %d\n", turned ? "turned away from" : "facing", culls[c]);
					ok = false;
				}
				img->destroy();
			}else
				ok = false;
		}
	}
	DestroyAtomic(atomic);
	quad->destroy();

	V3d left[3];
	for(int i = 0; i < 3; i++){
		left[i] = p[i];
		left[i].x += 5.0f;
	}
	Geometry *front = TriangleGeometry(left, ccw, RED);
	Geometry *back = TriangleGeometry(p, cw, RED);
	Raster *parent = saved.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 600, 10, 16, 16 };
	sub->subRaster(parent, &r);
	WorldBegin(GREY);
	SetRenderState(CULLMODE, CULLBACK);
	RenderGeometry(front);
	saved.camera->frameBuffer = sub;
	RGBA blue = BLUE;
	saved.camera->clear(&blue, Camera::CLEARIMAGE);
	saved.camera->frameBuffer = parent;
	RenderGeometry(back);
	WorldEnd();
	sub->destroy();
	front->destroy();
	back->destroy();
	if((img = ReadCamera())){
		bool subOk = Near(img, 608, 18, BLUE, 0);
		subOk &= Near(img, ScreenX(5.0f, 10.0f), 250, RED, 0);
		subOk &= Near(img, 320, 250, GREY, 0);
		if(!subOk)
			Detail("  after a sub-rectangle clear with the pass open\n");
		ok &= subOk;
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static bool
Barycentric(const float *a, const float *b, const float *c, float px, float py, float *w)
{
	float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
	w[0] = ((b[1]-c[1])*(px-c[0]) + (c[0]-b[0])*(py-c[1]))/d;
	w[1] = ((c[1]-a[1])*(px-c[0]) + (a[0]-c[0])*(py-c[1]))/d;
	w[2] = 1.0f - w[0] - w[1];
	return w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f;
}

static RGBA
QuadColourAt(const RGBA *c, int x, int y)
{
	static const float corner[4][2] = { { 256.0f, 176.0f }, { 384.0f, 176.0f }, { 384.0f, 304.0f }, { 256.0f, 304.0f } };
	float px = x + 0.5f, py = y + 0.5f, w[3];
	int t[3] = { 0, 1, 2 };
	if(!Barycentric(corner[0], corner[1], corner[2], px, py, w)){
		t[1] = 2;
		t[2] = 3;
		Barycentric(corner[0], corner[2], corner[3], px, py, w);
	}
	float r = 0.0f, g = 0.0f, b = 0.0f;
	for(int i = 0; i < 3; i++){
		r += w[i]*c[t[i]].red;
		g += w[i]*c[t[i]].green;
		b += w[i]*c[t[i]].blue;
	}
	RGBA col = { (uint8)floorf(r + 0.5f), (uint8)floorf(g + 0.5f), (uint8)floorf(b + 0.5f), 255 };
	return col;
}

static bool
CheckVertexColours(void)
{
	const char *name = "prelight colours keep their byte order and interpolate, and a missing colour is black";
	static const RGBA solid = { 200, 40, 10, 255 };
	static const RGBA corners[4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
	static const RGBA mid = { 128, 0, 128, 255 };
	bool ok = true;
	Image *img;

	WorldBegin(GREY);
	RenderSquare(2.0f, 10.0f, solid);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, solid, 0);
		img->destroy();
	}else
		ok = false;

	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS, 2.0f, -2.0f, -2.0f, 2.0f, 10.0f,
	                             corners, nil, mat);
	mat->destroy();
	WorldBegin(GREY);
	RenderGeometry(geo);
	WorldEnd();
	geo->destroy();
	if((img = ReadCamera())){
		static const int probes[4][2] = { { 260, 180 }, { 380, 180 }, { 380, 300 }, { 260, 300 } };
		for(int i = 0; i < 4; i++)
			ok &= Near(img, probes[i][0], probes[i][1], QuadColourAt(corners, probes[i][0], probes[i][1]), 2);
		ok &= Near(img, 320, 240, mid, 3);
		img->destroy();
	}else
		ok = false;

	mat = Material::create();
	geo = QuadGeometry(Geometry::POSITIONS, 2.0f, -2.0f, -2.0f, 2.0f, 10.0f, nil, nil, mat);
	mat->destroy();
	WorldBegin(GREY);
	RenderGeometry(geo);
	WorldEnd();
	geo->destroy();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, BLACK, 0);
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static bool
CheckTextureOrigin(void)
{
	const char *name = "a texture's first row is at the top of a quad with v 0 at the top";
	static const RGBA texels[4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Texture *tex = MakeTexture(2, 2, texels, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	if(tex == nil)
		return Report(false, name);
	Material *mat = Material::create();
	mat->setTexture(tex);
	tex->destroy();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             2.0f, -2.0f, -2.0f, 2.0f, 10.0f, white, uv, mat);
	mat->destroy();
	WorldBegin(GREY);
	RenderGeometry(geo);
	WorldEnd();
	geo->destroy();
	bool ok = true;
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 288, 208, texels[0], 0);
		ok &= Near(img, 352, 208, texels[1], 0);
		ok &= Near(img, 288, 272, texels[2], 0);
		ok &= Near(img, 352, 272, texels[3], 0);
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static bool
CheckStripIsNotList(void)
{
	const char *name = "a tristrip mesh draws as a strip";
	Geometry *geo = Geometry::create(4, 0, Geometry::TRISTRIP | Geometry::PRELIT | Geometry::POSITIONS);
	V3d *v = geo->morphTargets[0].vertices;
	v[0].set(2.0f, 2.0f, 10.0f);
	v[1].set(-2.0f, 2.0f, 10.0f);
	v[2].set(-2.0f, -2.0f, 10.0f);
	v[3].set(2.0f, -2.0f, 10.0f);
	for(int i = 0; i < 4; i++)
		geo->colors[i] = RED;
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	MeshHeader *mh = geo->allocateMeshes(1, 4, 0);
	mh->flags = MeshHeader::TRISTRIP;
	Mesh *mesh = mh->getMeshes();
	mesh->numIndices = 4;
	mesh->material = geo->matList.materials[0];
	static const uint16 idx[4] = { 0, 1, 3, 2 };
	memcpy(mesh->indices, idx, sizeof(idx));
	geo->calculateBoundingSphere();
	geo->unlock();
	WorldBegin(GREY);
	RenderGeometry(geo);
	WorldEnd();
	geo->destroy();
	bool ok = true;
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 296, 200, RED, 0);
		ok &= Near(img, 376, 296, RED, 0);
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static bool
CheckTwoMeshes(void)
{
	const char *name = "each mesh of an atomic draws from its own index offset";
	Geometry *geo = Geometry::create(7, 3, Geometry::PRELIT | Geometry::POSITIONS);
	V3d *v = geo->morphTargets[0].vertices;
	v[0].set(3.0f, -1.0f, 10.0f);
	v[1].set(1.0f, -1.0f, 10.0f);
	v[2].set(2.0f, 1.0f, 10.0f);
	v[3].set(-1.0f, 1.0f, 10.0f);
	v[4].set(-3.0f, 1.0f, 10.0f);
	v[5].set(-3.0f, -1.0f, 10.0f);
	v[6].set(-1.0f, -1.0f, 10.0f);
	for(int i = 0; i < 7; i++)
		geo->colors[i] = i < 3 ? RED : GREEN;
	Material *m0 = Material::create();
	Material *m1 = Material::create();
	geo->matList.appendMaterial(m0);
	geo->matList.appendMaterial(m1);
	m0->destroy();
	m1->destroy();
	static const uint16 tris[3][4] = { { 0, 1, 2, 0 }, { 3, 4, 5, 1 }, { 3, 5, 6, 1 } };
	for(int i = 0; i < 3; i++){
		geo->triangles[i].v[0] = tris[i][0];
		geo->triangles[i].v[1] = tris[i][1];
		geo->triangles[i].v[2] = tris[i][2];
		geo->triangles[i].matId = tris[i][3];
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil;
	if(h){
		ok &= Expect(h->numMeshes == 2 && h->inst[1].offset == 8 && h->inst[1].numIndex == 6,
		             "%u meshes, mesh 1 at offset %u with %u indices, expected 2, 8 and 6",
		             h->numMeshes, h->numMeshes > 1 ? h->inst[1].offset : 0, h->numMeshes > 1 ? h->inst[1].numIndex : 0);
		Image *img = RenderOne(atomic);
		if(img){
			ok &= Near(img, ScreenX(2.0f, 10.0f), ScreenY(-1.0f/3.0f, 10.0f), RED, 0);
			ok &= Near(img, ScreenX(-2.0f, 10.0f), ScreenY(0.0f, 10.0f), GREEN, 0);
			img->destroy();
		}else
			ok = false;
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckMidPassDepthClear(void)
{
	const char *name = "a depth clear with the pass open lets farther geometry through";
	WorldBegin(GREY);
	RenderSquare(2.0f, 20.0f, RED);
	RGBA c = GREY;
	saved.camera->clear(&c, Camera::CLEARZ);
	RenderSquare(8.0f, 40.0f, BLUE);
	WorldEnd();
	bool ok = true;
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 320, 240, BLUE, 0);
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static void
DrawLit(Atomic *atomic, WorldLights *lights, bool useLightBits)
{
	metal::InstanceDataHeader *header = Instanced(atomic);
	if(header == nil)
		return;
	uint32 flags = atomic->geometry->flags;
	metal::setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 bits = metal::setLights(lights);
	if(!useLightBits)
		bits = 0;
	metal::setupVertexInput(header);
	metal::InstanceData *inst = header->inst;
	for(uint32 n = 0; n < header->numMeshes; n++, inst++){
		Material *m = inst->material;
		metal::setMaterial(flags, m->color, m->surfaceProps);
		metal::setTexture(0, m->texture);
		SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		metal::defaultShader->use(metal::shaderVariant(bits & metal::VSLIGHT_MASK, metal::getAlphaTest()));
		metal::drawInst(header, inst);
	}
	metal::teardownVertexInput(header);
}

static void
ClearLights(void)
{
	WorldLights none = {};
	metal::setLights(&none);
}

static void
Gl3DynamicLight(const WorldLights *wl, V3d V, V3d N, float out[3])
{
	int n = 0;
	out[0] = out[1] = out[2] = 0.0f;
	for(int i = 0; i < wl->numDirectionals && n < 8; i++, n++){
		Light *l = wl->directionals[i];
		V3d at = l->getFrame()->getLTM()->at;
		float d = fmaxf(0.0f, -dot(N, at));
		out[0] += d*l->color.red;
		out[1] += d*l->color.green;
		out[2] += d*l->color.blue;
	}
	for(int i = 0; i < wl->numLocals && n < 8; i++, n++){
		Light *l = wl->locals[i];
		Matrix *ltm = l->getFrame()->getLTM();
		V3d dir = sub(V, ltm->pos);
		float dist = length(dir);
		float atten = fmaxf(0.0f, 1.0f - dist/l->radius);
		dir = scale(dir, 1.0f/dist);
		float d = fmaxf(0.0f, -dot(N, dir));
		if(l->getType() == Light::POINT){
			out[0] += d*l->color.red*atten;
			out[1] += d*l->color.green*atten;
			out[2] += d*l->color.blue*atten;
			continue;
		}
		float pcos = dot(dir, ltm->at);
		float ccos = -l->minusCosAngle;
		float falloff = (pcos - ccos)/(1.0f - ccos);
		if(falloff < 0.0f)
			d = 0.0f;
		d *= fmaxf(falloff, l->getType() == Light::SOFTSPOT ? 0.0f : 1.0f);
		out[0] = d*l->color.red*atten;
		out[1] = d*l->color.green*atten;
		out[2] = d*l->color.blue*atten;
		return;
	}
}

static RGBA
Gl3LitColour(const WorldLights *wl, V3d V, V3d N, const SurfaceProperties &surf)
{
	float dyn[3];
	Gl3DynamicLight(wl, V, N, dyn);
	float amb[3] = { wl->ambient.red, wl->ambient.green, wl->ambient.blue };
	uint8 c[3];
	for(int i = 0; i < 3; i++){
		float v = amb[i]*surf.ambient + dyn[i]*surf.diffuse;
		v = fminf(fmaxf(v, 0.0f), 1.0f);
		c[i] = (uint8)floorf(v*255.0f + 0.5f);
	}
	RGBA col = { c[0], c[1], c[2], 255 };
	return col;
}

static Light*
MakeLight(int32 type, float r, float g, float b)
{
	Light *l = Light::create(type);
	l->setFrame(Frame::create());
	l->setColor(r, g, b);
	return l;
}

static void
DestroyLight(Light *l)
{
	Frame *f = l->getFrame();
	l->destroy();
	f->destroy();
}

static Geometry*
LitQuad(const SurfaceProperties &surf)
{
	Material *mat = Material::create();
	mat->surfaceProps = surf;
	Geometry *geo = QuadGeometry(Geometry::NORMALS | Geometry::LIGHT | Geometry::POSITIONS,
	                             1.0f, -1.0f, -1.0f, 1.0f, 10.0f, nil, nil, mat);
	mat->destroy();
	return geo;
}

static bool
LitResult(RGBA want, int tol, uint32 variant, uint32 wantVariant, const char *what)
{
	Image *img = ReadCamera();
	if(img == nil)
		return false;
	bool ok = Near(img, 320, 240, want, tol);
	img->destroy();
	if(variant != wantVariant){
		Detail("  variant %u, expected %u\n", variant, wantVariant);
		ok = false;
	}
	if(!ok)
		Detail("  in case %s\n", what);
	return ok;
}

static bool
SetLightsCase(Atomic *atomic, WorldLights *wl, bool useLightBits, RGBA want, uint32 wantVariant, const char *what)
{
	WorldBegin(GREY);
	DrawLit(atomic, wl, useLightBits);
	uint32 variant = metal::currentVariant;
	WorldEnd();
	return LitResult(want, 1, variant, wantVariant, what);
}

static bool
LitCase(Atomic *atomic, Light **lights, int numLights, RGBA want, int tol, uint32 wantVariant, const char *what)
{
	for(int i = 0; i < numLights; i++)
		saved.world->addLight(lights[i]);
	WorldBegin(GREY);
	atomic->render();
	uint32 variant = metal::currentVariant;
	WorldEnd();
	for(int i = 0; i < numLights; i++)
		saved.world->removeLight(lights[i]);
	return LitResult(want, tol, variant, wantVariant, what);
}

static bool
CheckLitColours(void)
{
	const char *name = "world lights drawn through the render callback match gl3's formula computed on the CPU";
	static const SurfaceProperties surf = { 0.5f, 0.0f, 0.8f };
	Geometry *geo = LitQuad(surf);
	Atomic *atomic = MakeAtomic(geo);
	saved.world->addAtomic(atomic);
	Light *amb = MakeLight(Light::AMBIENT, 0.1f, 0.2f, 0.3f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.6f, 0.5f, 0.4f);
	Light *point = MakeLight(Light::POINT, 0.9f, 0.3f, 0.1f);
	Light *spot = MakeLight(Light::SPOT, 0.2f, 0.4f, 0.9f);
	Light *soft = MakeLight(Light::SOFTSPOT, 0.2f, 0.4f, 0.9f);
	V3d xAxis = { 1.0f, 0.0f, 0.0f }, lightPos = { 0.0f, 0.0f, 8.0f };
	dir->getFrame()->rotate(&xAxis, 30.0f);
	point->getFrame()->translate(&lightPos);
	spot->getFrame()->translate(&lightPos);
	soft->getFrame()->translate(&lightPos);
	point->radius = 5.0f;
	spot->radius = 5.0f;
	soft->radius = 5.0f;
	spot->setAngle(3.14159265f/4.0f);
	soft->setAngle(3.14159265f/4.0f);

	RGBAf ambient = { 0.1f, 0.2f, 0.3f, 1.0f };
	Light *dirs[1] = { dir };
	Light *locals[1];
	WorldLights wl = {};
	wl.numAmbients = 1;
	wl.ambient = ambient;
	wl.directionals = dirs;
	wl.locals = locals;
	V3d V = { 1.0f, 1.0f, 10.0f }, N = { 0.0f, 0.0f, -1.0f };
	bool ok = true;

	Light *set[3] = { amb, dir, nil };

	wl.numDirectionals = 1;
	wl.numLocals = 0;
	ok &= LitCase(atomic, set, 2, Gl3LitColour(&wl, V, N, surf), 1, metal::VARIANT_DIRECTIONALS, "directional");
	WorldLights ambientOnly = wl;
	ambientOnly.numDirectionals = 0;
	ok &= SetLightsCase(atomic, &wl, false, Gl3LitColour(&ambientOnly, V, N, surf), 0, "directional drawn with variant 0");

	wl.numDirectionals = 0;
	wl.numLocals = 1;
	locals[0] = set[1] = point;
	ok &= LitCase(atomic, set, 2, Gl3LitColour(&wl, V, N, surf), 1, metal::VARIANT_POINTLIGHTS, "point");
	locals[0] = set[1] = spot;
	ok &= LitCase(atomic, set, 2, Gl3LitColour(&wl, V, N, surf), 1, metal::VARIANT_SPOTLIGHTS, "spot");
	locals[0] = set[1] = soft;
	ok &= LitCase(atomic, set, 2, Gl3LitColour(&wl, V, N, surf), 1, metal::VARIANT_SPOTLIGHTS, "soft spot");
	wl.numDirectionals = 1;
	locals[0] = spot;
	set[1] = dir;
	set[2] = spot;
	ok &= LitCase(atomic, set, 3, Gl3LitColour(&wl, V, N, surf), 1,
	              metal::VARIANT_DIRECTIONALS | metal::VARIANT_SPOTLIGHTS, "directional then spot, which drops the directional");

	saved.world->removeAtomic(atomic);
	DestroyAtomic(atomic);
	geo->destroy();

	Material *mat = Material::create();
	mat->surfaceProps = surf;
	geo = QuadGeometry(Geometry::NORMALS | Geometry::LIGHT | Geometry::POSITIONS,
	                   1.0f, -1.0f, -1.0f, 1.0f, -10.0f, nil, nil, mat);
	mat->destroy();
	for(int i = 0; i < 4; i++)
		geo->morphTargets[0].normals[i].set(0.0f, 0.0f, 1.0f);
	atomic = MakeAtomic(geo);
	V3d yAxis = { 0.0f, 1.0f, 0.0f };
	atomic->getFrame()->rotate(&yAxis, 180.0f);
	wl.numDirectionals = 1;
	wl.numLocals = 0;
	set[1] = dir;
	ok &= LitCase(atomic, set, 2, Gl3LitColour(&wl, V, N, surf), 1, metal::VARIANT_DIRECTIONALS,
	              "directional on a turned atomic with model normals +z");
	DestroyAtomic(atomic);
	geo->destroy();

	ClearLights();
	DestroyLight(amb);
	DestroyLight(dir);
	DestroyLight(point);
	DestroyLight(spot);
	DestroyLight(soft);
	return Report(ok, name);
}

static bool
CheckUnlitGeometryGetsNoLights(void)
{
	const char *name = "a geometry without LIGHT drawn after a lit one gets no lights and zero ambient from the render callback";
	static const RGBA solid = { 200, 40, 10, 255 };
	static const SurfaceProperties surf = { 1.0f, 0.0f, 1.0f };
	Light *amb = MakeLight(Light::AMBIENT, 0.5f, 0.5f, 0.5f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.6f, 0.5f, 0.4f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	Geometry *litGeo = LitQuad(surf);
	Atomic *lit = MakeAtomic(litGeo);
	V3d left = { 4.0f, 0.0f, 0.0f };
	lit->getFrame()->translate(&left);
	bool ok = true;
	WorldBegin(GREY);
	metal::StateStats s = metal::getStateStats();
	lit->render();
	uint32 litVariant = metal::currentVariant;
	RenderSquare(2.0f, 10.0f, solid);
	uint32 variant = metal::currentVariant;
	uint32 lightUploads = metal::getStateStats().blockUploads[metal::BUFFER_LIGHTS] - s.blockUploads[metal::BUFFER_LIGHTS];
	WorldEnd();
	ok &= Expect(litVariant == metal::VARIANT_DIRECTIONALS, "the lit quad drew with variant %u, expected %u",
	             litVariant, metal::VARIANT_DIRECTIONALS);
	ok &= Expect(variant == 0, "variant %u, expected 0", variant);
	ok &= Expect(lightUploads == 2, "the lights block was uploaded %u times, expected 2", lightUploads);
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 320, 240, solid, 0);
		img->destroy();
	}else
		ok = false;
	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	DestroyAtomic(lit);
	litGeo->destroy();
	ClearLights();
	DestroyLight(amb);
	DestroyLight(dir);
	return Report(ok, name);
}

static Geometry*
TwoQuads(bool alphaFirst)
{
	Geometry *geo = Geometry::create(8, 4, Geometry::PRELIT | Geometry::POSITIONS);
	V3d *v = geo->morphTargets[0].vertices;
	v[0].set(4.0f, 2.0f, 10.0f);
	v[1].set(0.5f, 2.0f, 10.0f);
	v[2].set(0.5f, -2.0f, 10.0f);
	v[3].set(4.0f, -2.0f, 10.0f);
	v[4].set(-0.5f, 2.0f, 10.0f);
	v[5].set(-4.0f, 2.0f, 10.0f);
	v[6].set(-4.0f, -2.0f, 10.0f);
	v[7].set(-0.5f, -2.0f, 10.0f);
	static const RGBA clear = { 255, 0, 0, 0 };
	for(int i = 0; i < 8; i++)
		geo->colors[i] = i < 4 ? RED : clear;
	Material *m0 = Material::create();
	Material *m1 = Material::create();
	geo->matList.appendMaterial(m0);
	geo->matList.appendMaterial(m1);
	m0->destroy();
	m1->destroy();
	static const uint16 tris[4][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 4, 5, 6 }, { 4, 6, 7 } };
	for(int i = 0; i < 4; i++){
		geo->triangles[i].v[0] = tris[i][0];
		geo->triangles[i].v[1] = tris[i][1];
		geo->triangles[i].v[2] = tris[i][2];
		geo->triangles[i].matId = (i < 2) == alphaFirst ? 1 : 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
CheckVariantChosenPerDraw(void)
{
	const char *name = "each mesh draws with the alpha test variant its own state asks for";
	bool ok = true;
	for(int alphaFirst = 0; alphaFirst < 2; alphaFirst++){
		Geometry *geo = TwoQuads(alphaFirst != 0);
		WorldBegin(GREY);
		RenderGeometry(geo);
		uint32 variant = metal::currentVariant;
		RenderSquare(10.0f, 20.0f, BLUE);
		WorldEnd();
		geo->destroy();
		uint32 want = alphaFirst ? 0 : metal::VARIANT_ALPHATEST;
		bool caseOk = Expect(variant == want, "the last mesh drew with variant %u, expected %u", variant, want);
		Image *img = ReadCamera();
		if(img){
			caseOk &= Near(img, ScreenX(2.25f, 10.0f), 240, RED, 0);
			caseOk &= Near(img, ScreenX(-2.25f, 10.0f), 240, BLUE, 0);
			img->destroy();
		}else
			caseOk = false;
		if(!caseOk)
			Detail("  with the transparent mesh %s\n", alphaFirst ? "first" : "second");
		ok &= caseOk;
	}
	return Report(ok, name);
}

static bool
UploadsWere(const metal::StateStats &before, uint32 index, uint32 want, const char *step)
{
	uint32 got = metal::getStateStats().blockUploads[index] - before.blockUploads[index];
	return Expect(got == want, "%s: block %u uploaded %u times, expected %u", step, index, got, want);
}

static bool
CheckSecondFrameUploads(void)
{
	const char *name = "a second frame with nothing changed uploads the object, lights and material blocks once each";
	static const SurfaceProperties surf = { 0.5f, 0.0f, 0.8f };
	Geometry *geo = LitQuad(surf);
	Atomic *atomic = MakeAtomic(geo);
	Light *amb = MakeLight(Light::AMBIENT, 0.1f, 0.2f, 0.3f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.6f, 0.5f, 0.4f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	bool ok = true;

	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	WorldBegin(GREY);
	metal::StateStats s = metal::getStateStats();
	atomic->render();
	atomic->render();
	uint32 variant = metal::currentVariant;
	ok &= UploadsWere(s, metal::BUFFER_OBJECT, 1, "default render callback");
	ok &= UploadsWere(s, metal::BUFFER_LIGHTS, 1, "default render callback");
	ok &= UploadsWere(s, metal::BUFFER_MATERIAL, 1, "default render callback");
	ok &= Expect(variant == metal::VARIANT_DIRECTIONALS, "variant %u, expected %u", variant, metal::VARIANT_DIRECTIONALS);
	WorldEnd();

	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	ClearLights();
	DestroyLight(amb);
	DestroyLight(dir);
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
RingProbe(metal::RingSpace *space)
{
	return metal::ringAlloc(1, 256, space) != 0;
}

static bool
CheckRingBytesPerFrame(void)
{
	const char *name = "ring bytes for four atomics and two materials match their block uploads";
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	Material *mats[2] = { Material::create(), Material::create() };
	mats[0]->color = RED;
	mats[1]->color = GREEN;
	Geometry *geos[2];
	for(int i = 0; i < 2; i++){
		geos[i] = QuadGeometry(Geometry::PRELIT | Geometry::MODULATE | Geometry::POSITIONS,
		                       0.5f, -0.5f, -0.5f, 0.5f, 10.0f, white, nil, mats[i]);
		mats[i]->destroy();
	}
	Atomic *atomics[5];
	for(int i = 0; i < 5; i++){
		atomics[i] = MakeAtomic(geos[i < 4 ? i/2 : 1]);
		V3d pos = { i < 4 ? 3.0f - 2.0f*i : 0.0f, i < 4 ? 0.0f : 2.0f, 0.0f };
		atomics[i]->getFrame()->translate(&pos);
	}
	bool ok = true;
	metal::RingSpace before, after;
	WorldBegin(GREY);
	metal::StateStats s0 = metal::getStateStats();
	atomics[4]->render();
	metal::StateStats s = metal::getStateStats();
	ok &= RingProbe(&before);
	for(int i = 0; i < 4; i++)
		atomics[i]->render();
	ok &= RingProbe(&after);
	metal::StateStats e = metal::getStateStats();
	WorldEnd();
	uint32 uploads = 0, frameUploads = 0;
	for(int i = 0; i < (int)nelem(e.blockUploads); i++){
		uploads += e.blockUploads[i] - s.blockUploads[i];
		frameUploads += e.blockUploads[i] - s0.blockUploads[i];
	}
	uint32 object = e.blockUploads[metal::BUFFER_OBJECT] - s.blockUploads[metal::BUFFER_OBJECT];
	uint32 material = e.blockUploads[metal::BUFFER_MATERIAL] - s.blockUploads[metal::BUFFER_MATERIAL];
	uint32 bytes = after.buffer == before.buffer ? after.offset - before.offset - 256 : ~0u;
	ok &= Expect(object == 4 && material == 2 && uploads == 6, "%u object and %u material of %u uploads, expected 4 and 2 of 6",
	             object, material, uploads);
	ok &= Expect(bytes == 6*256, "%u ring bytes, expected %u", bytes, 6*256);
	ok &= Expect(frameUploads <= 5 + 6, "%u uploads in the frame, expected at most 11", frameUploads);
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, ScreenX(3.0f, 10.0f), 240, RED, 0);
		ok &= Near(img, ScreenX(-3.0f, 10.0f), 240, GREEN, 0);
		img->destroy();
	}else
		ok = false;
	for(int i = 0; i < 5; i++)
		DestroyAtomic(atomics[i]);
	geos[0]->destroy();
	geos[1]->destroy();
	return Report(ok, name);
}

static RGBA
Rgb(uint8 r, uint8 g, uint8 b)
{
	RGBA c = { r, g, b, 255 };
	return c;
}

static RGBA
Rgba(uint8 r, uint8 g, uint8 b, uint8 a)
{
	RGBA c = { r, g, b, a };
	return c;
}

static const SurfaceProperties unitSurf = { 1.0f, 0.0f, 1.0f };

static Geometry*
LitGrid(void)
{
	Geometry *geo = Geometry::create(9, 8, Geometry::NORMALS | Geometry::LIGHT | Geometry::POSITIONS);
	for(int j = 0; j < 3; j++)
		for(int i = 0; i < 3; i++){
			geo->morphTargets[0].vertices[j*3+i].set(5.0f*(i-1), 5.0f*(j-1), 10.0f);
			geo->morphTargets[0].normals[j*3+i].set(0.0f, 0.0f, -1.0f);
		}
	Material *mat = Material::create();
	mat->surfaceProps = unitSurf;
	geo->matList.appendMaterial(mat);
	mat->destroy();
	Triangle *t = geo->triangles;
	for(int j = 0; j < 2; j++)
		for(int i = 0; i < 2; i++){
			uint16 a = j*3+i;
			t->v[0] = a; t->v[1] = a+1; t->v[2] = a+4; t->matId = 0; t++;
			t->v[0] = a; t->v[1] = a+4; t->v[2] = a+3; t->matId = 0; t++;
		}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static Light*
LocalLight(int32 type, float r, float g, float b)
{
	Light *l = MakeLight(type, r, g, b);
	V3d pos = { 0.0f, 0.0f, 8.0f };
	l->getFrame()->translate(&pos);
	l->radius = 4.0f;
	return l;
}

static bool
CheckWorldLightCases(void)
{
	const char *name = "ambient, directional, point and spot lights added to the world light a quad through the render callback";
	static const SurfaceProperties halfDiffuse = { 1.0f, 0.0f, 0.5f };
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.4f, 0.6f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	Light *point = LocalLight(Light::POINT, 1.0f, 0.0f, 0.0f);
	Light *spot = LocalLight(Light::SPOT, 0.0f, 0.0f, 1.0f);
	spot->setAngle(3.14159265f/4.0f);
	Light *set[9] = { amb, dir, spot };
	bool ok = true;

	Geometry *geo = LitQuad(unitSurf);
	Atomic *atomic = MakeAtomic(geo);
	ok &= LitCase(atomic, set, 1, Rgb(51, 102, 153), 1, 0, "L1 ambient");
	ok &= LitCase(atomic, set, 2, Rgb(179, 230, 255), 1, metal::VARIANT_DIRECTIONALS, "L2 ambient and directional");
	DestroyAtomic(atomic);
	geo->destroy();

	geo = LitQuad(halfDiffuse);
	atomic = MakeAtomic(geo);
	ok &= LitCase(atomic, set, 2, Rgb(115, 166, 217), 1, metal::VARIANT_DIRECTIONALS, "L2 with diffuse 0.5");
	DestroyAtomic(atomic);
	geo->destroy();

	Material *mat = Material::create();
	mat->surfaceProps = unitSurf;
	geo = QuadGeometry(Geometry::NORMALS | Geometry::LIGHT | Geometry::POSITIONS,
	                   1.0f, -1.0f, -1.0f, 1.0f, -10.0f, nil, nil, mat);
	for(int i = 0; i < 4; i++)
		geo->morphTargets[0].normals[i].set(0.0f, 0.0f, 1.0f);
	atomic = MakeAtomic(geo);
	V3d yAxis = { 0.0f, 1.0f, 0.0f };
	atomic->getFrame()->rotate(&yAxis, 180.0f);
	ok &= LitCase(atomic, set, 2, Rgb(179, 230, 255), 1, metal::VARIANT_DIRECTIONALS, "L2 normal transformed by the world matrix");
	DestroyAtomic(atomic);
	geo->destroy();

	geo = QuadGeometry(Geometry::LIGHT | Geometry::POSITIONS, 1.0f, -1.0f, -1.0f, 1.0f, 10.0f, nil, nil, mat);
	mat->destroy();
	atomic = MakeAtomic(geo);
	ok &= LitCase(atomic, set, 2, Rgb(51, 102, 153), 1, 0, "L3 directional without normals");
	DestroyAtomic(atomic);
	geo->destroy();

	geo = LitGrid();
	atomic = MakeAtomic(geo);
	saved.world->addAtomic(atomic);
	Light *pointSet[2] = { amb, point };
	ok &= LitCase(atomic, pointSet, 2, Rgb(179, 102, 153), 2, metal::VARIANT_POINTLIGHTS, "L4 point light");
	ok &= LitCase(atomic, set, 3, Rgb(51, 102, 255), 2, metal::VARIANT_DIRECTIONALS | metal::VARIANT_SPOTLIGHTS,
	              "L5 spot light, which drops the directional");
	saved.world->removeAtomic(atomic);
	DestroyAtomic(atomic);
	geo->destroy();

	Light *nine[9];
	for(int i = 0; i < 9; i++)
		nine[i] = MakeLight(Light::DIRECTIONAL, 0.1f, 0.1f, 0.1f);
	geo = LitQuad(unitSurf);
	atomic = MakeAtomic(geo);
	ok &= LitCase(atomic, nine, 9, Rgb(204, 204, 204), 1, metal::VARIANT_DIRECTIONALS, "L6 nine directionals, capped at eight");
	DestroyAtomic(atomic);
	geo->destroy();
	for(int i = 0; i < 9; i++)
		DestroyLight(nine[i]);

	ClearLights();
	DestroyLight(amb);
	DestroyLight(dir);
	DestroyLight(point);
	DestroyLight(spot);
	return Report(ok, name);
}

static bool
CheckFog(void)
{
	const char *name = "fog blends to the fog colour by clip w between the fog plane and the far plane";
	struct Case { float z; bool fog; uint8 want; int tol; };
	const Case cases[] = {
		{ 56.0f, true, 128, 1 },
		{ 20.0f, true, 230, 1 },
		{ 5.0f, true, 255, 0 },
		{ 56.0f, false, 255, 0 },
	};
	uint32 oldColour = GetRenderState(FOGCOLOR);
	float32 oldPlane = saved.camera->fogPlane;
	saved.camera->fogPlane = 11.0f;
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		WorldBegin(GREY);
		SetRenderState(FOGCOLOR, RWRGBAINT(0, 0, 0, 255));
		SetRenderState(FOGENABLE, c.fog);
		RenderSquare(0.2f*c.z, c.z, WHITE);
		SetRenderState(FOGENABLE, 0);
		WorldEnd();
		Image *img = ReadCamera();
		if(img == nil){
			ok = false;
			continue;
		}
		bool caseOk = Near(img, 320, 240, Rgb(c.want, c.want, c.want), c.tol);
		img->destroy();
		if(!caseOk)
			Detail("  at z %g with fog %s\n", c.z, c.fog ? "on" : "off");
		ok &= caseOk;
	}
	saved.camera->fogPlane = oldPlane;
	SetRenderState(FOGCOLOR, oldColour);
	return Report(ok, name);
}

static Geometry*
MaterialQuad(uint32 flags, RGBA colour)
{
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	Material *mat = Material::create();
	mat->color = colour;
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS | flags,
	                             2.0f, -2.0f, -2.0f, 2.0f, 10.0f, white, nil, mat);
	mat->destroy();
	return geo;
}

static bool
CheckMaterialColour(void)
{
	const char *name = "the material colour modulates prelit white only with MODULATE, and its alpha blends";
	static const RGBA orange = { 255, 128, 64, 255 };
	static const RGBA half = { 255, 255, 255, 128 };
	struct Case { uint32 flags; RGBA colour; RGBA want; int tol; const char *what; };
	const Case cases[] = {
		{ Geometry::MODULATE, orange, orange, 0, "orange with MODULATE" },
		{ 0, orange, WHITE, 0, "orange without MODULATE" },
		{ Geometry::MODULATE, half, Rgba(192, 192, 192, 191), 2, "white at material alpha 128" },
	};
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		Geometry *geo = MaterialQuad(c.flags, c.colour);
		WorldBegin(GREY);
		RenderGeometry(geo);
		WorldEnd();
		geo->destroy();
		Image *img = ReadCamera();
		if(img == nil){
			ok = false;
			continue;
		}
		bool caseOk = Near(img, 320, 240, c.want, c.tol);
		img->destroy();
		if(!caseOk)
			Detail("  in case %s\n", c.what);
		ok &= caseOk;
	}
	return Report(ok, name);
}

static Geometry*
CutoutQuad(uint8 leftAlpha)
{
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	RGBA texels[2] = { { 255, 0, 0, leftAlpha }, { 255, 0, 0, 255 } };
	Texture *tex = MakeTexture(2, 1, texels, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	if(tex == nil)
		return nil;
	Material *mat = Material::create();
	mat->setTexture(tex);
	tex->destroy();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             4.0f, -2.0f, -4.0f, 2.0f, 10.0f, white, uv, mat);
	mat->destroy();
	return geo;
}

static bool
CheckCutout(void)
{
	const char *name = "a texel failing the alpha test is discarded and writes no depth";
	Geometry *geo = CutoutQuad(0);
	if(geo == nil)
		return Report(false, name);
	WorldBegin(GREY);
	RenderGeometry(geo);
	RenderSquare(10.0f, 20.0f, BLUE);
	WorldEnd();
	geo->destroy();
	bool ok = true;
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 256, 240, BLUE, 0);
		ok &= Near(img, 384, 240, RED, 0);
		img->destroy();
	}else
		ok = false;
	return Report(ok, name);
}

static bool
CheckPS2AlphaTest(void)
{
	const char *name = "GSALPHATEST draws texels below its reference without depth, and restores the alpha test and depth write";
	Geometry *geo = CutoutQuad(64);
	if(geo == nil)
		return Report(false, name);
	bool ok = Expect(GetRenderState(GSALPHATESTREF) == 128, "GSALPHATESTREF is %u, expected 128", GetRenderState(GSALPHATESTREF));

	WorldBegin(GREY);
	RenderSquare(10.0f, 20.0f, BLUE);
	SetRenderState(GSALPHATEST, 1);
	uint32 func = GetRenderState(ALPHATESTFUNC), ref = GetRenderState(ALPHATESTREF);
	RenderGeometry(geo);
	uint32 variant = metal::currentVariant;
	ok &= Expect(GetRenderState(ALPHATESTFUNC) == func && GetRenderState(ALPHATESTREF) == ref &&
	             GetRenderState(ZWRITEENABLE) == 1 && GetRenderState(GSALPHATEST) == 1 &&
	             GetRenderState(GSALPHATESTREF) == 128,
	             "depth write on: state after the draw is func %u ref %u zwrite %u, expected %u %u 1",
	             GetRenderState(ALPHATESTFUNC), GetRenderState(ALPHATESTREF), GetRenderState(ZWRITEENABLE), func, ref);
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	ok &= Expect(variant == metal::VARIANT_ALPHATEST, "depth write on: variant %u, expected %u", variant, metal::VARIANT_ALPHATEST);
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 256, 240, Rgba(64, 0, 191, 207), 2);
		ok &= Near(img, 384, 240, RED, 0);
		img->destroy();
	}else
		ok = false;
	ok &= DepthNear(256, 240, DepthOf(20.0f), 1e-5f);
	ok &= DepthNear(384, 240, DepthOf(10.0f), 1e-5f);

	WorldBegin(GREY);
	SetRenderState(GSALPHATEST, 1);
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(ALPHATESTREF, 200);
	ref = GetRenderState(ALPHATESTREF);
	RenderGeometry(geo);
	ok &= Expect(GetRenderState(ALPHATESTFUNC) == func && GetRenderState(ALPHATESTREF) == ref &&
	             GetRenderState(ZWRITEENABLE) == 0,
	             "depth write off: state after the draw is func %u ref %u zwrite %u, expected %u %u 0",
	             GetRenderState(ALPHATESTFUNC), GetRenderState(ALPHATESTREF), GetRenderState(ZWRITEENABLE), func, ref);
	SetRenderState(ALPHATESTREF, 3);
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 256, 240, Rgba(160, 96, 96, 207), 2);
		ok &= Near(img, 384, 240, RED, 0);
		img->destroy();
	}else
		ok = false;
	ok &= DepthNear(384, 240, 1.0f, 0.0f);
	geo->destroy();
	return Report(ok, name);
}

static Geometry*
TwoBands(Material *top, Material *bottom, float uMax)
{
	Geometry *geo = Geometry::create(8, 4, Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS);
	static const float band[2][2] = { { 3.0f, 1.0f }, { -1.0f, -3.0f } };
	for(int b = 0; b < 2; b++){
		V3d *v = &geo->morphTargets[0].vertices[b*4];
		TexCoords *uv = &geo->texCoords[0][b*4];
		v[0].set(4.0f, band[b][0], 10.0f);
		v[1].set(-4.0f, band[b][0], 10.0f);
		v[2].set(-4.0f, band[b][1], 10.0f);
		v[3].set(4.0f, band[b][1], 10.0f);
		uv[0].u = 0.0f; uv[0].v = 0.0f;
		uv[1].u = uMax; uv[1].v = 0.0f;
		uv[2].u = uMax; uv[2].v = 1.0f;
		uv[3].u = 0.0f; uv[3].v = 1.0f;
		for(int i = 0; i < 4; i++)
			geo->colors[b*4+i] = WHITE;
		Triangle *t = &geo->triangles[b*2];
		t[0].v[0] = b*4; t[0].v[1] = b*4+1; t[0].v[2] = b*4+2; t[0].matId = b;
		t[1].v[0] = b*4; t[1].v[1] = b*4+2; t[1].v[2] = b*4+3; t[1].matId = b;
	}
	geo->matList.appendMaterial(top);
	geo->matList.appendMaterial(bottom);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
CheckSamplersPerMaterial(void)
{
	const char *name = "each material samples with its own texture's filter and addressing";
	static const RGBA texels[2] = { RED, GREEN };
	Texture *wrap = MakeTexture(2, 1, texels, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	Texture *clamp = MakeTexture(2, 1, texels, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *linear = MakeTexture(2, 1, texels, Texture::LINEAR, Texture::CLAMP, Texture::CLAMP);
	if(wrap == nil || clamp == nil || linear == nil)
		return Report(false, name);
	Material *mats[3] = { Material::create(), Material::create(), Material::create() };
	mats[0]->setTexture(wrap);
	mats[1]->setTexture(clamp);
	mats[2]->setTexture(linear);
	wrap->destroy();
	clamp->destroy();
	linear->destroy();

	Geometry *geo = TwoBands(mats[0], mats[1], 2.0f);

	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Geometry *lin = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             2.0f, -1.5f, -2.0f, 1.5f, 10.0f, white, uv, mats[2]);
	for(int i = 0; i < 3; i++)
		mats[i]->destroy();

	bool ok = true;
	WorldBegin(GREY);
	RenderGeometry(geo);
	WorldEnd();
	Image *img = ReadCamera();
	if(img){
		static const int xs[4] = { 224, 288, 352, 416 };
		const RGBA top[4] = { RED, GREEN, RED, GREEN };
		const RGBA bottom[4] = { RED, GREEN, GREEN, GREEN };
		for(int i = 0; i < 4; i++){
			ok &= Near(img, xs[i], 176, top[i], 0);
			ok &= Near(img, xs[i], 304, bottom[i], 0);
		}
		img->destroy();
	}else
		ok = false;

	WorldBegin(GREY);
	RenderGeometry(lin);
	WorldEnd();
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, Rgb(128, 128, 0), 3);
		img->destroy();
	}else
		ok = false;
	geo->destroy();
	lin->destroy();
	return Report(ok, name);
}

static bool
CheckFilterPerMaterial(void)
{
	const char *name = "two materials in one atomic that differ only in filter mode each sample with their own filter";
	static const RGBA texels[2] = { RED, GREEN };
	Texture *nearest = MakeTexture(2, 1, texels, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *linear = MakeTexture(2, 1, texels, Texture::LINEAR, Texture::CLAMP, Texture::CLAMP);
	if(nearest == nil || linear == nil)
		return Report(false, name);
	Material *top = Material::create();
	Material *bottom = Material::create();
	top->setTexture(nearest);
	bottom->setTexture(linear);
	nearest->destroy();
	linear->destroy();
	Geometry *geo = TwoBands(top, bottom, 1.0f);
	top->destroy();
	bottom->destroy();

	bool ok = true;
	WorldBegin(GREY);
	RenderGeometry(geo);
	WorldEnd();
	Image *img = ReadCamera();
	if(img){
		static const int xs[2] = { 288, 352 };
		for(int i = 0; i < 2; i++){
			float u = (xs[i] + 0.5f - 192.0f)/256.0f;
			float f = fminf(fmaxf(u*2.0f - 0.5f, 0.0f), 1.0f);
			RGBA mixed = Rgb((uint8)floorf(255.0f*(1.0f - f) + 0.5f), (uint8)floorf(255.0f*f + 0.5f), 0);
			ok &= Near(img, xs[i], 176, u < 0.5f ? RED : GREEN, 0);
			ok &= Near(img, xs[i], 304, mixed, 3);
		}
		img->destroy();
	}else
		ok = false;
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckRingAllocStartsFrame(void)
{
	const char *name = "a ring allocation before the first draw of a frame lands in that frame's slot";
	metal::RingSpace held = {}, fresh = {};
	bool ok = true;

	WorldBegin(GREY);
	RenderSquare(2.0f, 10.0f, RED);
	ok &= Expect(metal::ringAlloc(64, 256, &held) != 0, "the held frame's allocation failed");
	if(held.cpu)
		memset(held.cpu, 0x5A, 64);
	uint64 heldFrame = metal::getFrameId();
	metal::holdFrameForTest(0.5);
	WorldEnd();

	WorldBegin(GREY);
	uint64 before = metal::getFrameId();
	ok &= Expect(metal::ringAlloc(64, 256, &fresh) != 0, "the new frame's allocation failed");
	uint64 after = metal::getFrameId();
	if(fresh.cpu)
		memset(fresh.cpu, 0xA5, 64);
	RenderSquare(2.0f, 10.0f, BLUE);
	bool stillHeld = metal::getCompletedFrameId() < heldFrame;
	bool intact = held.cpu != nil;
	for(int i = 0; i < 64 && intact; i++)
		intact = held.cpu[i] == 0x5A;
	WorldEnd();

	ok &= Expect(before == heldFrame, "frame %llu had started before any draw, expected it to start later",
	             (unsigned long long)before);
	ok &= Expect(after == heldFrame + 1, "the allocation left the frame id at %llu, expected %llu",
	             (unsigned long long)after, (unsigned long long)heldFrame + 1);
	ok &= Expect(fresh.buffer != held.buffer && fresh.offset == 0,
	             "the allocation is at offset %u of %s buffer, expected offset 0 of the new frame's slot",
	             fresh.offset, fresh.buffer == held.buffer ? "the held frame's" : "another");
	ok &= Expect(stillHeld, "the held frame completed early, so the check proves nothing");
	ok &= Expect(intact, "the held frame's ring data changed");
	return Report(ok, name);
}

static bool
CheckStripRestartIndexCounted(void)
{
	const char *name = "a strip mesh using index 0xFFFF is counted at instancing and keeps all its indices";
	Geometry *geo = Geometry::create(65536, 1, Geometry::TRISTRIP | Geometry::POSITIONS);
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < 65536; i++)
		v[i].set(0.0f, 0.0f, 10.0f);
	v[65533].set(1.0f, 0.0f, 10.0f);
	v[65534].set(0.0f, 1.0f, 10.0f);
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	Triangle *t = geo->triangles;
	t->v[0] = 65533; t->v[1] = 65534; t->v[2] = 65535; t->matId = 0;
	geo->calculateBoundingSphere();
	geo->unlock();

	bool ok = true;
	Mesh *mesh = geo->meshHeader->getMeshes();
	bool uses = false;
	for(uint32 i = 0; i < mesh->numIndices; i++)
		uses |= mesh->indices[i] == 0xFFFF;
	ok &= Expect(geo->meshHeader->flags == 1 && uses, "the test geometry is not a strip using 0xFFFF");

	uint32 before = metal::getInstanceStats().stripRestartMeshes;
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	uint32 after = metal::getInstanceStats().stripRestartMeshes;
	ok &= Expect(after == before + 1, "%u strip meshes with 0xFFFF counted, expected 1", after - before);
	if(h)
		ok &= Expect(h->inst[0].numIndex == mesh->numIndices, "%u indices instanced, expected %u",
		             h->inst[0].numIndex, mesh->numIndices);
	else
		ok = false;
	DestroyAtomic(atomic);
	geo->destroy();

	Material *mat2 = Material::create();
	geo = QuadGeometry(Geometry::TRISTRIP | Geometry::POSITIONS, 2.0f, -1.0f, -2.0f, 1.5f, 10.0f, nil, nil, mat2);
	mat2->destroy();
	atomic = MakeAtomic(geo);
	Instanced(atomic);
	uint32 plain = metal::getInstanceStats().stripRestartMeshes;
	ok &= Expect(plain == after, "a strip without 0xFFFF was counted");
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckLitAtomicWithoutWorld(void)
{
	const char *name = "a lit atomic rendered by a camera outside any world gets no lights";
	static const RGBA prelight = { 10, 200, 30, 255 };
	RGBA col[4] = { prelight, prelight, prelight, prelight };
	Material *mat = Material::create();
	mat->surfaceProps = unitSurf;
	Geometry *geo = QuadGeometry(Geometry::NORMALS | Geometry::LIGHT | Geometry::PRELIT | Geometry::POSITIONS,
	                             1.0f, -1.0f, -1.0f, 1.0f, 10.0f, col, nil, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	Light *amb = MakeLight(Light::AMBIENT, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	bool ok = true;

	Image *img = RenderOne(atomic);
	if(img){
		ok &= Near(img, 320, 240, Rgb(138, 255, 158), 1);
		img->destroy();
	}else
		ok = false;

	saved.world->removeCamera(saved.camera);
	WorldBegin(GREY);
	ok &= Expect(engine->currentWorld == nil, "the camera is still in a world");
	atomic->render();
	uint32 variant = metal::currentVariant;
	WorldEnd();
	saved.world->addCamera(saved.camera);
	ok &= Expect(variant == 0, "variant %u, expected 0", variant);
	if((img = ReadCamera())){
		ok &= Near(img, 320, 240, prelight, 0);
		img->destroy();
	}else
		ok = false;

	saved.world->removeLight(amb);
	DestroyLight(amb);
	ClearLights();
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static const RGBA IMCOL = { 10, 200, 30, 255 };
static uint16 quadIndices[6] = { 0, 1, 2, 0, 2, 3 };

static void
Im3DVert(metal::Im3DVertex *v, float x, float y, float z, RGBA c, float u, float t)
{
	v->setX(x); v->setY(y); v->setZ(z);
	v->setNormalX(0.0f); v->setNormalY(0.0f); v->setNormalZ(-1.0f);
	v->setColor(c.red, c.green, c.blue, c.alpha);
	v->setU(u); v->setV(t);
}

static void
Im3DQuad(metal::Im3DVertex *v, float x0, float y0, float x1, float y1, float z, RGBA c)
{
	Im3DVert(&v[0], x0, y1, z, c, 0.0f, 0.0f);
	Im3DVert(&v[1], x1, y1, z, c, 1.0f, 0.0f);
	Im3DVert(&v[2], x1, y0, z, c, 1.0f, 1.0f);
	Im3DVert(&v[3], x0, y0, z, c, 0.0f, 1.0f);
}

static void
Im3DSquare(metal::Im3DVertex *v, float half, float z, RGBA c)
{
	Im3DQuad(v, half, -half, -half, half, z, c);
}

static void
DrawIm3DSquare(float half, float z, RGBA c, Matrix *world, uint32 flags)
{
	metal::Im3DVertex v[4];
	Im3DSquare(v, half, z, c);
	im3d::Transform(v, 4, world, flags);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndices, 6);
	im3d::End();
}

static float
WorldX(float sx, float z)
{
	return z*(1.0f - sx/320.0f);
}

static float
WorldY(float sy, float z)
{
	return 0.75f*z*(1.0f - sy/240.0f);
}

struct PixelWant
{
	int x, y;
	RGBA want;
	int tol;
};

static bool
Pixels(const PixelWant *p, int n, const char *what)
{
	Image *img = ReadCamera();
	if(img == nil){
		Detail("  in case %s\n", what);
		return false;
	}
	bool ok = true;
	for(int i = 0; i < n; i++)
		ok &= Near(img, p[i].x, p[i].y, p[i].want, p[i].tol);
	img->destroy();
	if(!ok)
		Detail("  in case %s\n", what);
	return ok;
}

static bool
CheckIm3DColourAndWorldMatrix(void)
{
	const char *name = "an im3d quad lands where the camera maths puts it, in its vertex colour, and a world matrix moves it";
	bool ok = true;

	WorldBegin(GREY);
	DrawIm3DSquare(2.0f, 10.0f, IMCOL, nil, 0);
	WorldEnd();
	const PixelWant plain[] = {
		{ 320, 240, IMCOL, 0 }, { 260, 240, IMCOL, 0 }, { 380, 240, IMCOL, 0 },
		{ 250, 240, GREY, 0 }, { 390, 240, GREY, 0 }, { 320, 170, GREY, 0 }, { 320, 180, IMCOL, 0 },
	};
	ok &= Pixels(plain, nelem(plain), "I1 identity");

	Matrix world;
	world.setIdentity();
	V3d shift = { 1.0f, 0.0f, 0.0f };
	world.translate(&shift, COMBINEREPLACE);
	WorldBegin(GREY);
	DrawIm3DSquare(2.0f, 10.0f, IMCOL, &world, 0);
	WorldEnd();
	const PixelWant moved[] = {
		{ 320, 240, IMCOL, 0 }, { 228, 240, IMCOL, 0 }, { 348, 240, IMCOL, 0 },
		{ 218, 240, GREY, 0 }, { 358, 240, GREY, 0 },
	};
	ok &= Pixels(moved, nelem(moved), "I1 translated by +1 in x");
	return Report(ok, name);
}

static bool
CheckIm3DTexture(void)
{
	const char *name = "an im3d quad with VERTEXUV samples the bound raster, and one without unbinds it";
	static const RGBA texels[4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
	Texture *tex = MakeTexture(2, 2, texels, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	if(tex == nil)
		return Report(false, name);
	bool ok = true;
	metal::Im3DVertex v[4];
	Im3DSquare(v, 2.0f, 10.0f, WHITE);

	WorldBegin(GREY);
	SetRenderStatePtr(TEXTURERASTER, tex->raster);
	SetRenderState(TEXTUREFILTER, Texture::NEAREST);
	SetRenderState(TEXTUREADDRESS, Texture::WRAP);
	im3d::Transform(v, 4, nil, im3d::VERTEXUV);
	ok &= Expect(GetRenderStatePtr(TEXTURERASTER) == tex->raster, "a transform with VERTEXUV unbound the raster");
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndices, 6);
	im3d::End();
	WorldEnd();
	const PixelWant quadrants[] = {
		{ 288, 208, texels[0], 0 }, { 352, 208, texels[1], 0 }, { 288, 272, texels[2], 0 }, { 352, 272, texels[3], 0 },
	};
	ok &= Pixels(quadrants, nelem(quadrants), "I2 texture quadrants");

	WorldBegin(GREY);
	SetRenderStatePtr(TEXTURERASTER, tex->raster);
	Im3DSquare(v, 2.0f, 10.0f, IMCOL);
	im3d::Transform(v, 4, nil, im3d::VERTEXXYZ | im3d::VERTEXRGBA);
	ok &= Expect(GetRenderStatePtr(TEXTURERASTER) == nil, "a transform without VERTEXUV left the raster bound");
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndices, 6);
	im3d::End();
	WorldEnd();
	const PixelWant untextured[] = { { 288, 208, IMCOL, 0 }, { 352, 272, IMCOL, 0 } };
	ok &= Pixels(untextured, nelem(untextured), "I7 without VERTEXUV");
	tex->destroy();
	return Report(ok, name);
}

static bool
CheckIm3DSeveralRenders(void)
{
	const char *name = "one im3d transform serves several render calls, also in the frame that reuses its ring slot";
	static uint16 upper[3] = { 0, 1, 2 };
	static uint16 lower[3] = { 0, 2, 3 };
	const PixelWant both[] = { { 368, 200, IMCOL, 0 }, { 272, 280, IMCOL, 0 } };
	const PixelWant lowerOnly[] = { { 368, 200, GREY, 0 }, { 272, 280, IMCOL, 0 }, { 320, 240, RED, 0 } };
	metal::Im3DVertex v[4];
	Im3DSquare(v, 2.0f, 10.0f, IMCOL);
	bool ok = true;

	WorldBegin(GREY);
	im3d::Transform(v, 4, nil, 0);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, upper, 3);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, lower, 3);
	WorldEnd();
	ok &= Pixels(both, nelem(both), "I3 two indexed calls");

	for(int i = 1; i < metal::getMaxFramesInFlight(); i++){
		WorldBegin(GREY);
		RenderSquare(0.5f, 10.0f, BLUE);
		WorldEnd();
	}
	WorldBegin(GREY);
	RenderSquare(0.5f, 10.0f, RED);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, lower, 3);
	im3d::End();
	WorldEnd();
	ok &= Pixels(lowerOnly, nelem(lowerOnly), "a render call without a new transform in the frame that reuses the transform's ring slot");
	return Report(ok, name);
}

static bool
CheckIm3DLighting(void)
{
	const char *name = "an im3d transform with LIGHTING lights its vertices from the world, and without a world from nothing";
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.4f, 0.6f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	bool ok = true;

	WorldBegin(GREY);
	DrawIm3DSquare(2.0f, 10.0f, BLACK, nil, im3d::LIGHTING);
	uint32 variant = metal::currentVariant;
	WorldEnd();
	ok &= Expect(variant == metal::VARIANT_DIRECTIONALS, "lit: variant %u, expected %u", variant, metal::VARIANT_DIRECTIONALS);
	const PixelWant lit[] = { { 320, 240, Rgb(179, 230, 255), 1 } };
	ok &= Pixels(lit, nelem(lit), "I4 ambient and directional");

	WorldBegin(GREY);
	DrawIm3DSquare(2.0f, 10.0f, IMCOL, nil, 0);
	WorldEnd();
	const PixelWant unlit[] = { { 320, 240, IMCOL, 0 } };
	ok &= Pixels(unlit, nelem(unlit), "an unlit transform after a lit one");

	saved.world->removeCamera(saved.camera);
	WorldBegin(GREY);
	DrawIm3DSquare(2.0f, 10.0f, IMCOL, nil, im3d::LIGHTING);
	variant = metal::currentVariant;
	WorldEnd();
	saved.world->addCamera(saved.camera);
	ok &= Expect(variant == 0, "without a world: variant %u, expected 0", variant);
	ok &= Pixels(unlit, nelem(unlit), "LIGHTING without a world");

	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	return Report(ok, name);
}

static const PixelWant quadFilled[] = {
	{ 300, 240, IMCOL, 0 }, { 340, 240, IMCOL, 0 }, { 320, 200, IMCOL, 0 }, { 320, 280, IMCOL, 0 },
	{ 270, 180, IMCOL, 0 }, { 370, 180, IMCOL, 0 }, { 270, 300, IMCOL, 0 }, { 370, 300, IMCOL, 0 },
};

static bool
CheckIm3DPrimitiveTypes(void)
{
	const char *name = "im3d draws every primitive type, indexed and not";
	metal::Im3DVertex quad[4], strip[4], list[6];
	Im3DSquare(quad, 2.0f, 10.0f, IMCOL);
	strip[0] = quad[0]; strip[1] = quad[1]; strip[2] = quad[3]; strip[3] = quad[2];
	for(int i = 0; i < 6; i++)
		list[i] = quad[quadIndices[i]];
	static uint16 fanIndices[4] = { 0, 1, 2, 3 };
	static uint16 stripIndices[4] = { 0, 1, 3, 2 };
	bool ok = true;

	struct Case { metal::Im3DVertex *v; int n; PrimitiveType type; uint16 *indices; int numIndices; const char *what; };
	const Case fills[] = {
		{ list, 6, PRIMTYPETRILIST, nil, 0, "TRILIST" },
		{ quad, 4, PRIMTYPETRILIST, quadIndices, 6, "indexed TRILIST" },
		{ strip, 4, PRIMTYPETRISTRIP, nil, 0, "TRISTRIP" },
		{ quad, 4, PRIMTYPETRISTRIP, stripIndices, 4, "indexed TRISTRIP" },
		{ quad, 4, PRIMTYPETRIFAN, nil, 0, "TRIFAN" },
		{ quad, 4, PRIMTYPETRIFAN, fanIndices, 4, "indexed TRIFAN" },
	};
	for(int i = 0; i < (int)nelem(fills); i++){
		const Case &c = fills[i];
		WorldBegin(GREY);
		im3d::Transform(c.v, c.n, nil, 0);
		if(c.indices)
			im3d::RenderIndexedPrimitive(c.type, c.indices, c.numIndices);
		else
			im3d::RenderPrimitive(c.type);
		im3d::End();
		WorldEnd();
		ok &= Pixels(quadFilled, nelem(quadFilled), c.what);
	}

	metal::Im3DVertex line[3];
	Im3DVert(&line[0], WorldX(100.5f, 10.0f), WorldY(240.5f, 10.0f), 10.0f, IMCOL, 0.0f, 0.0f);
	Im3DVert(&line[1], WorldX(300.5f, 10.0f), WorldY(240.5f, 10.0f), 10.0f, IMCOL, 0.0f, 0.0f);
	Im3DVert(&line[2], WorldX(300.5f, 10.0f), WorldY(400.5f, 10.0f), 10.0f, IMCOL, 0.0f, 0.0f);
	static uint16 lineIndices[3] = { 0, 1, 2 };
	const Case lines[] = {
		{ line, 3, PRIMTYPELINELIST, nil, 0, "LINELIST" },
		{ line, 3, PRIMTYPEPOLYLINE, nil, 0, "POLYLINE" },
		{ line, 3, PRIMTYPELINELIST, lineIndices, 2, "indexed LINELIST" },
		{ line, 3, PRIMTYPEPOLYLINE, lineIndices, 3, "indexed POLYLINE" },
	};
	for(int i = 0; i < (int)nelem(lines); i++){
		const Case &c = lines[i];
		bool poly = c.type == PRIMTYPEPOLYLINE;
		WorldBegin(GREY);
		im3d::Transform(c.v, c.n, nil, 0);
		if(c.indices)
			im3d::RenderIndexedPrimitive(c.type, c.indices, c.numIndices);
		else
			im3d::RenderPrimitive(c.type);
		im3d::End();
		WorldEnd();
		const PixelWant want[] = {
			{ 200, 240, IMCOL, 0 }, { 200, 250, GREY, 0 },
			{ 300, 320, poly ? IMCOL : GREY, 0 }, { 310, 320, GREY, 0 },
		};
		ok &= Pixels(want, nelem(want), c.what);
	}

	metal::Im3DVertex point[1];
	Im3DVert(&point[0], WorldX(320.5f, 10.0f), WorldY(240.5f, 10.0f), 10.0f, IMCOL, 0.0f, 0.0f);
	static uint16 pointIndex[1] = { 0 };
	for(int indexed = 0; indexed < 2; indexed++){
		WorldBegin(GREY);
		im3d::Transform(point, 1, nil, 0);
		if(indexed)
			im3d::RenderIndexedPrimitive(PRIMTYPEPOINTLIST, pointIndex, 1);
		else
			im3d::RenderPrimitive(PRIMTYPEPOINTLIST);
		im3d::End();
		WorldEnd();
		const PixelWant want[] = { { 320, 240, IMCOL, 0 }, { 322, 240, GREY, 0 }, { 320, 242, GREY, 0 } };
		ok &= Pixels(want, nelem(want), indexed ? "indexed POINTLIST" : "POINTLIST");
	}
	return Report(ok, name);
}

static bool
CheckIm3DWideFan(void)
{
	const char *name = "an im3d fan of more than 65536 vertices reaches its last vertices";
	const int n = 70000;
	metal::Im3DVertex *v = new metal::Im3DVertex[n];
	for(int i = 0; i < n; i++)
		Im3DVert(&v[i], 0.0f, 0.0f, 10.0f, IMCOL, 0.0f, 0.0f);
	Im3DVert(&v[n-2], -2.0f, 0.0f, 10.0f, IMCOL, 0.0f, 0.0f);
	Im3DVert(&v[n-1], -2.0f, -2.0f, 10.0f, IMCOL, 0.0f, 0.0f);
	WorldBegin(GREY);
	im3d::Transform(v, n, nil, 0);
	im3d::RenderPrimitive(PRIMTYPETRIFAN);
	im3d::End();
	WorldEnd();
	delete[] v;
	const PixelWant want[] = { { 370, 250, IMCOL, 0 }, { 330, 290, GREY, 0 } };
	return Report(Pixels(want, nelem(want), "fan of 70000"), name);
}

static bool
CheckIm3DTwoTransforms(void)
{
	const char *name = "a second im3d transform in a frame keeps the first one's vertices for its draws";
	metal::Im3DVertex left[4], right[4];
	Im3DQuad(left, 3.0f, -1.0f, 1.0f, 1.0f, 10.0f, RED);
	Im3DQuad(right, -1.0f, -1.0f, -3.0f, 1.0f, 10.0f, BLUE);
	WorldBegin(GREY);
	im3d::Transform(left, 4, nil, 0);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndices, 6);
	im3d::End();
	im3d::Transform(right, 4, nil, 0);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndices, 6);
	im3d::End();
	WorldEnd();
	const PixelWant want[] = { { 256, 240, RED, 0 }, { 384, 240, BLUE, 0 }, { 320, 240, GREY, 0 } };
	return Report(Pixels(want, nelem(want), "two transforms"), name);
}

static bool
CheckIm3DTransformBeforeFrame(void)
{
	const char *name = "an im3d transform made before the frame starts is drawn in that frame";
	Matrix world;
	world.setIdentity();
	V3d shift = { 1.0f, 0.0f, 0.0f };
	world.translate(&shift, COMBINEREPLACE);
	metal::Im3DVertex v[4];
	Im3DSquare(v, 2.0f, 10.0f, IMCOL);
	WorldBegin(GREY);
	RenderSquare(0.5f, 10.0f, RED);
	WorldEnd();
	im3d::Transform(v, 4, &world, 0);
	WorldBegin(GREY);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndices, 6);
	im3d::End();
	WorldEnd();
	const PixelWant want[] = { { 228, 240, IMCOL, 0 }, { 358, 240, GREY, 0 } };
	return Report(Pixels(want, nelem(want), "transform before the frame"), name);
}

static bool
CheckIm3DEndWithoutRender(void)
{
	const char *name = "an im3d transform ended without a render call draws nothing and the next one draws";
	metal::Im3DVertex v[4];
	Im3DSquare(v, 3.0f, 10.0f, RED);
	WorldBegin(GREY);
	im3d::Transform(v, 4, nil, 0);
	im3d::End();
	DrawIm3DSquare(1.0f, 10.0f, IMCOL, nil, 0);
	WorldEnd();
	const PixelWant want[] = { { 320, 240, IMCOL, 0 }, { 240, 240, GREY, 0 } };
	return Report(Pixels(want, nelem(want), "end without render"), name);
}

static bool
CheckIm3DDepth(void)
{
	const char *name = "im3d tests depth against earlier world geometry, writes depth, and blends ZERO ONE as depth only";
	bool ok = true;

	WorldBegin(GREY);
	RenderSquare(2.0f, 10.0f, RED);
	DrawIm3DSquare(8.0f, 20.0f, BLUE, nil, 0);
	DrawIm3DSquare(0.5f, 5.0f, GREEN, nil, 0);
	WorldEnd();
	const PixelWant world[] = { { 320, 240, GREEN, 0 }, { 270, 240, RED, 0 }, { 220, 240, BLUE, 0 } };
	ok &= Pixels(world, nelem(world), "against an atomic");

	WorldBegin(GREY);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDZERO);
	SetRenderState(DESTBLEND, BLENDONE);
	DrawIm3DSquare(2.0f, 10.0f, RED, nil, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(VERTEXALPHA, 0);
	DrawIm3DSquare(8.0f, 20.0f, BLUE, nil, 0);
	WorldEnd();
	const PixelWant mask[] = { { 320, 240, GREY, 0 }, { 240, 240, BLUE, 0 } };
	ok &= Pixels(mask, nelem(mask), "I6 ZERO ONE with depth write");
	ok &= DepthNear(320, 240, DepthOf(10.0f), 1e-5f);

	RGBA clear = { 255, 0, 0, 0 };
	WorldBegin(GREY);
	SetRenderState(VERTEXALPHA, 1);
	DrawIm3DSquare(2.0f, 10.0f, clear, nil, 0);
	uint32 variant = metal::currentVariant;
	SetRenderState(VERTEXALPHA, 0);
	WorldEnd();
	ok &= Expect(variant == metal::VARIANT_ALPHATEST, "alpha 0: variant %u, expected %u", variant, metal::VARIANT_ALPHATEST);
	ok &= DepthNear(320, 240, 1.0f, 0.0f);
	return Report(ok, name);
}

static bool
CheckIm3DFlags(void)
{
	const char *name = "im3d ignores ALLOPAQUE, NOCLIP, VERTEXXYZ and VERTEXRGBA as gl3 does";
	const uint32 flagSets[] = {
		0,
		im3d::EVERYTHING | im3d::NOCLIP | im3d::ALLOPAQUE,
		im3d::NOCLIP,
	};
	bool ok = true;
	for(int i = 0; i < (int)nelem(flagSets); i++){
		WorldBegin(GREY);
		DrawIm3DSquare(2.0f, 10.0f, IMCOL, nil, flagSets[i]);
		WorldEnd();
		const PixelWant want[] = { { 320, 240, IMCOL, 0 }, { 250, 240, GREY, 0 } };
		char what[64];
		snprintf(what, sizeof(what), "flags 0x%x", flagSets[i]);
		ok &= Pixels(want, nelem(want), what);
	}

	RGBA halfRed = { 255, 0, 0, 128 };
	WorldBegin(GREY);
	SetRenderState(VERTEXALPHA, 1);
	DrawIm3DSquare(2.0f, 10.0f, halfRed, nil, im3d::ALLOPAQUE | im3d::VERTEXRGBA);
	SetRenderState(VERTEXALPHA, 0);
	WorldEnd();
	const PixelWant blended[] = { { 320, 240, Rgba(192, 64, 64, 191), 2 } };
	ok &= Pixels(blended, nelem(blended), "ALLOPAQUE with vertex alpha 128");
	return Report(ok, name);
}

static bool
CheckIm3DFog(void)
{
	const char *name = "im3d primitives are fogged by clip w like atomics";
	uint32 oldColour = GetRenderState(FOGCOLOR);
	float32 oldPlane = saved.camera->fogPlane;
	saved.camera->fogPlane = 11.0f;
	bool ok = true;
	for(int fog = 1; fog >= 0; fog--){
		WorldBegin(GREY);
		SetRenderState(FOGCOLOR, RWRGBAINT(0, 0, 0, 255));
		SetRenderState(FOGENABLE, fog);
		DrawIm3DSquare(0.2f*56.0f, 56.0f, WHITE, nil, 0);
		SetRenderState(FOGENABLE, 0);
		WorldEnd();
		const PixelWant want[] = { { 320, 240, fog ? Rgb(128, 128, 128) : WHITE, fog ? 1 : 0 } };
		ok &= Pixels(want, nelem(want), fog ? "fog on at z 56" : "fog off at z 56");
	}
	saved.camera->fogPlane = oldPlane;
	SetRenderState(FOGCOLOR, oldColour);
	return Report(ok, name);
}

static Geometry*
MatFXQuad(uint32 flags, Texture *tex, RGBA colour)
{
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Material *mat = Material::create();
	mat->color = colour;
	mat->setTexture(tex);
	MatFX::setEffects(mat, MatFX::ENVMAP);
	Geometry *geo = QuadGeometry(flags | Geometry::TEXTURED | Geometry::POSITIONS,
	                             1.0f, -1.0f, -1.0f, 1.0f, 10.0f, white, uv, mat);
	mat->destroy();
	return geo;
}

static bool
CheckMatFXFallback(void)
{
	const char *name = "an atomic with material effects gets the Metal matfx pipeline and draws its base pass lit like the default pipeline";
	static const RGBA red[1] = { RED };
	Texture *tex = MakeTexture(1, 1, red, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	if(tex == nil)
		return Report(false, name);
	bool ok = true;

	Geometry *geo = MatFXQuad(Geometry::PRELIT, tex, WHITE);
	Atomic *atomic = MakeAtomic(geo);
	MatFX::enableEffects(atomic);
	ok &= Expect(atomic->pipeline != nil && atomic->pipeline == matFXGlobals.pipelines[PLATFORM_METAL] &&
	             atomic->pipeline != matFXGlobals.dummypipe, "the atomic did not get the Metal matfx pipeline");
	ok &= Expect(atomic->pipeline && atomic->pipeline->platform == PLATFORM_METAL, "the matfx pipeline is not a Metal pipeline");
	Image *img = RenderOne(atomic);
	if(img){
		ok &= Near(img, 320, 240, RED, 0);
		img->destroy();
	}else
		ok = false;
	DestroyAtomic(atomic);
	geo->destroy();

	static const RGBA whiteTexel[1] = { WHITE };
	Texture *white = MakeTexture(1, 1, whiteTexel, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.4f, 0.6f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	geo = MatFXQuad(Geometry::NORMALS | Geometry::LIGHT | Geometry::MODULATE, white, Rgb(200, 100, 50));
	RGBA got[2];
	for(int fx = 0; fx < 2; fx++){
		atomic = MakeAtomic(geo);
		if(fx)
			MatFX::enableEffects(atomic);
		img = RenderOne(atomic);
		DestroyAtomic(atomic);
		if(img == nil){
			ok = false;
			break;
		}
		uint8 *p = &img->pixels[240*img->stride + 320*4];
		got[fx] = Rgba(p[0], p[1], p[2], p[3]);
		img->destroy();
	}
	if(ok){
		ok &= Expect(got[0].red != GREY.red || got[0].green != GREY.green || got[0].blue != GREY.blue,
		             "the lit quad was not drawn by the default pipeline");
		ok &= Expect(memcmp(&got[0], &got[1], sizeof(RGBA)) == 0,
		             "lit through matfx %d,%d,%d,%d, through the default pipeline %d,%d,%d,%d",
		             got[1].red, got[1].green, got[1].blue, got[1].alpha,
		             got[0].red, got[0].green, got[0].blue, got[0].alpha);
	}
	geo->destroy();
	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	white->destroy();
	tex->destroy();
	return Report(ok, name);
}

static bool
CheckDrawsCounted(void)
{
	const char *name = "every mesh draw is counted, and none is dropped";
	metal::StateStats before = metal::getStateStats();
	WorldBegin(GREY);
	RenderSquare(1.0f, 10.0f, RED);
	RenderSquare(0.5f, 9.0f, GREEN);
	RenderSquare(0.25f, 8.0f, BLUE);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	bool ok = Expect(after.draws - before.draws == 3, "%u draws counted, expected 3", after.draws - before.draws);
	ok &= Expect(after.droppedDraws == before.droppedDraws, "%u draws dropped",
	             after.droppedDraws - before.droppedDraws);
	return Report(ok, name);
}

static void
DrawPatternQuad(uint32 flags, RGBA prelight, Texture *tex, uint8 matAlpha)
{
	RGBA col[4] = { prelight, prelight, prelight, prelight };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Material *mat = Material::create();
	mat->color.alpha = matAlpha;
	if(tex)
		mat->setTexture(tex);
	Geometry *geo = QuadGeometry(flags | Geometry::POSITIONS, 1.0f, -1.0f, -1.0f, 1.0f, 10.0f, col, uv, mat);
	mat->destroy();
	RenderGeometry(geo);
	geo->destroy();
}

static void
RenderIm2DQuad(void)
{
	metal::Im2DVertex v[4];
	float recipz = 1.0f/saved.camera->nearPlane;
	static const float xy[4][2] = { { 10.0f, 10.0f }, { 50.0f, 10.0f }, { 50.0f, 50.0f }, { 10.0f, 50.0f } };
	for(int i = 0; i < 4; i++){
		v[i].setScreenX(xy[i][0]);
		v[i].setScreenY(xy[i][1]);
		v[i].setScreenZ(im2d::GetNearZ());
		v[i].setCameraZ(saved.camera->nearPlane);
		v[i].setRecipCameraZ(recipz);
		v[i].setColor(0, 0, 0, 255);
		v[i].setU(0.0f, recipz);
		v[i].setV(0.0f, recipz);
	}
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
}

static void
DrawDepthOnlyIm2dMask(void)
{
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDZERO);
	SetRenderState(DESTBLEND, BLENDONE);
	SetRenderState(ZTESTENABLE, 0);
	RenderIm2DQuad();
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(VERTEXALPHA, 0);
}

static void
DrawShadowCameraQuads(Texture *tex)
{
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(SRCBLEND, BLENDINVDESTCOLOR);
	SetRenderState(DESTBLEND, BLENDZERO);
	RenderIm2DQuad();
	SetRenderStatePtr(TEXTURERASTER, tex->raster);
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDZERO);
	RenderIm2DQuad();
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(VERTEXALPHA, 0);
}

static void DrawSkinnedPatternMarkers(uint32 flags);
static void DrawEnvPatternQuad(uint32 flags, Texture *env, uint8 matAlpha);
static bool SameLayout(uint32 layout, const metal::AttribDesc *want, int32 numWant);

static uint32
FindSkinLayout(bool prelit)
{
	metal::InstAttrib want[metal::MAXINSTATTRIBS];
	metal::AttribDesc got[16];
	int32 n = metal::skinVertexLayout(true, prelit, 1, want);
	for(uint32 id = 1; metal::getVertexLayout(id, got, 16) > 0; id++)
		if(SameLayout(id, (const metal::AttribDesc*)want, n))
			return id;
	return 0;
}

static uint32
FindWorldLayout(bool normals, bool prelit)
{
	metal::InstAttrib want[metal::MAXINSTATTRIBS];
	metal::AttribDesc got[16];
	int32 n = metal::defaultVertexLayout(normals, prelit, 1, want);
	for(uint32 id = 1; metal::getVertexLayout(id, got, 16) > 0; id++)
		if(SameLayout(id, (const metal::AttribDesc*)want, n))
			return id;
	return 0;
}

static uint64
PrewarmKey(metal::Shader *sh,uint32 layout, uint32 variant, bool blend, uint32 src, uint32 dst)
{
	metal::PipelineDesc d = {};
	d.shader = sh->shaderId;
	d.variant = variant & sh->variantMask;
	d.vertexLayout = layout;
	d.blendEnable = blend;
	d.srcBlend = src;
	d.destBlend = dst;
	d.writeMask = 15;
	d.colorFormat = metal::COLORFMT_RGBA8;
	d.depthFormat = metal::DEPTHFMT_D32S8;
	d.sampleCount = 1;
	return metal::pipelineKey(d);
}

static bool
CheckGameKeysCached(void)
{
	uint32 ped = FindSkinLayout(true);
	uint32 actor = FindSkinLayout(false);
	bool ok = Expect(ped == 7 && actor == 8, "skin layouts %u and %u, expected 7 and 8", ped, actor);
	uint32 vehicle = FindWorldLayout(true, false);
	uint32 building = FindWorldLayout(false, true);
	uint32 water = FindWorldLayout(true, true);
	ok &= Expect(vehicle == 4 && building == 3 && water == 2, "world layouts %u, %u and %u, expected 4, 3 and 2",
	             vehicle, building, water);
	uint64 keys[16];
	int n = 0;
	const uint32 layouts[2] = { ped, actor };
	for(uint32 layout : layouts){
		keys[n++] = PrewarmKey(metal::skinShader, layout, 0, false, 0, 0);
		keys[n++] = PrewarmKey(metal::skinShader, layout, metal::VARIANT_ALPHATEST, true, BLENDSRCALPHA, BLENDINVSRCALPHA);
		keys[n++] = PrewarmKey(metal::skinShader, layout, metal::VARIANT_DIRECTIONALS, false, 0, 0);
		keys[n++] = PrewarmKey(metal::skinShader, layout, metal::VARIANT_DIRECTIONALS | metal::VARIANT_ALPHATEST, true,
		                       BLENDSRCALPHA, BLENDINVSRCALPHA);
	}
	keys[n++] = PrewarmKey(metal::im2dShader, metal::im2dVertexLayout, metal::VARIANT_ALPHATEST, true, BLENDINVDESTCOLOR, BLENDZERO);
	keys[n++] = PrewarmKey(metal::im2dShader, metal::im2dVertexLayout, metal::VARIANT_ALPHATEST, true, BLENDONE, BLENDZERO);
	keys[n++] = PrewarmKey(metal::im2dShader, metal::im2dVertexLayout, metal::VARIANT_ALPHATEST, true, BLENDZERO, BLENDSRCCOLOR);
	const uint32 envLayouts[3] = { vehicle, building, water };
	for(uint32 layout : envLayouts){
		keys[n++] = PrewarmKey(metal::matfxEnvShader, layout, metal::VARIANT_ALPHATEST, true, BLENDONE, BLENDINVSRCALPHA);
		if(layout != water)
			keys[n++] = PrewarmKey(metal::matfxEnvShader, layout, metal::VARIANT_DIRECTIONALS | metal::VARIANT_ALPHATEST, true,
			                       BLENDONE, BLENDINVSRCALPHA);
	}
	for(int i = 0; i < n; i++)
		ok &= Expect(metal::pipelineCached(keys[i]), "pipeline %016llx is not cached", (unsigned long long)keys[i]);
	return ok;
}

static bool
CheckGamePipelinesPrewarmed(void)
{
	const char *name = "the game's world, vehicle, ped, water, matfx, im3d, radar and shadow camera states draw without a pipeline built after init";
	static const RGBA cut[1] = { { 255, 0, 0, 128 } };
	Texture *alphaTex = MakeTexture(1, 1, cut, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	if(alphaTex == nil)
		return Report(false, name);
	const uint32 building = Geometry::PRELIT | Geometry::TEXTURED | Geometry::LIGHT;
	const uint32 vehicle = Geometry::NORMALS | Geometry::TEXTURED | Geometry::LIGHT;
	const uint32 water = Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED;
	const uint32 untexturedPrelit = Geometry::PRELIT | Geometry::LIGHT;
	const uint32 untexturedLit = Geometry::NORMALS | Geometry::LIGHT;
	const RGBA halfWhite = { 255, 255, 255, 128 };
	Light *amb = MakeLight(Light::AMBIENT, 0.3f, 0.3f, 0.3f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	bool cached = CheckGameKeysCached();
	metal::StateStats before = metal::getStateStats();

	WorldBegin(GREY);
	for(int lit = 1; lit >= 0; lit--){
		if(!lit)
			saved.world->removeLight(dir);
		SetRenderState(GSALPHATEST, 1);
		DrawPatternQuad(building, WHITE, nil, 255);
		DrawPatternQuad(building, WHITE, alphaTex, 255);
		SetRenderState(DESTBLEND, BLENDONE);
		DrawPatternQuad(building, WHITE, alphaTex, 255);
		SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
		DrawPatternQuad(vehicle, WHITE, nil, 255);
		DrawPatternQuad(vehicle, WHITE, nil, 128);
		DrawSkinnedPatternMarkers(Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED | Geometry::LIGHT);
		DrawSkinnedPatternMarkers(Geometry::NORMALS | Geometry::TEXTURED | Geometry::LIGHT);
		DrawPatternQuad(untexturedLit, WHITE, nil, 255);
		DrawPatternQuad(untexturedLit, WHITE, nil, 128);
		DrawPatternQuad(untexturedPrelit, WHITE, nil, 255);
		DrawPatternQuad(untexturedPrelit, halfWhite, nil, 255);
		DrawEnvPatternQuad(vehicle | Geometry::MODULATE, alphaTex, 255);
		DrawEnvPatternQuad(vehicle | Geometry::MODULATE, alphaTex, 128);
		DrawEnvPatternQuad(building, alphaTex, 255);
		DrawEnvPatternQuad(vehicle, nil, 255);
		SetRenderState(VERTEXALPHA, 0);
		SetRenderState(GSALPHATEST, 0);
	}
	DrawShadowCameraQuads(alphaTex);
	DrawPatternQuad(water, WHITE, nil, 255);
	DrawPatternQuad(water, WHITE, alphaTex, 255);
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDZERO);
	DrawPatternQuad(water, WHITE, alphaTex, 255);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);

	Geometry *geo = MatFXQuad(Geometry::NORMALS | Geometry::LIGHT, alphaTex, WHITE);
	Atomic *atomic = MakeAtomic(geo);
	MatFX::enableEffects(atomic);
	atomic->render();
	DestroyAtomic(atomic);
	geo->destroy();

	uint32 fog = GetRenderState(FOGENABLE);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(FOGENABLE, 1);
	DrawEnvPatternQuad(water | Geometry::MODULATE, alphaTex, 255);
	SetRenderState(FOGENABLE, fog);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(VERTEXALPHA, 0);

	static const int32 im3dBlends[][2] = {
		{ BLENDSRCALPHA, BLENDINVSRCALPHA },
		{ BLENDONE, BLENDONE },
		{ BLENDZERO, BLENDONE },
		{ BLENDZERO, BLENDINVSRCCOLOR },
		{ BLENDONE, BLENDZERO },
	};
	DrawIm3DSquare(1.0f, 10.0f, IMCOL, nil, 0);
	SetRenderState(VERTEXALPHA, 1);
	for(int i = 0; i < (int)nelem(im3dBlends); i++){
		SetRenderState(SRCBLEND, im3dBlends[i][0]);
		SetRenderState(DESTBLEND, im3dBlends[i][1]);
		DrawIm3DSquare(1.0f, 10.0f, IMCOL, nil, 0);
	}
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(VERTEXALPHA, 0);
	DrawDepthOnlyIm2dMask();
	WorldEnd();

	metal::StateStats after = metal::getStateStats();
	bool ok = cached;
	ok &= Expect(after.pipelinesLate == before.pipelinesLate, "%u pipelines built after init",
	                 after.pipelinesLate - before.pipelinesLate);
	ok &= Expect(after.pipelineFailures == before.pipelineFailures, "%u pipelines failed",
	             after.pipelineFailures - before.pipelineFailures);
	saved.world->removeLight(amb);
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	alphaTex->destroy();
	return Report(ok, name);
}

static bool
CheckDrawVariant(void)
{
	const char *name = "drawVariant gives the variant of a draw's lights and the current alpha test";
	WorldBegin(GREY);
	SetRenderState(VERTEXALPHA, 0);
	uint32 dir = metal::drawVariant(metal::VSLIGHT_AMBIENT | metal::VSLIGHT_DIRECT);
	uint32 local = metal::drawVariant(metal::VSLIGHT_POINT | metal::VSLIGHT_SPOT);
	uint32 amb = metal::drawVariant(metal::VSLIGHT_AMBIENT);
	SetRenderState(VERTEXALPHA, 1);
	uint32 alpha = metal::drawVariant(metal::VSLIGHT_DIRECT);
	SetRenderState(VERTEXALPHA, 0);
	WorldEnd();
	bool ok = Expect(dir == metal::VARIANT_DIRECTIONALS, "ambient and directional gave %u", dir);
	ok &= Expect(local == (metal::VARIANT_POINTLIGHTS | metal::VARIANT_SPOTLIGHTS), "point and spot gave %u", local);
	ok &= Expect(amb == 0, "ambient only gave %u", amb);
	ok &= Expect(alpha == (metal::VARIANT_DIRECTIONALS | metal::VARIANT_ALPHATEST),
	             "directional with vertex alpha gave %u", alpha);
	return Report(ok, name);
}

static metal::Shader *hostShader;
static Texture *hostStage1;

static void
HostRenderCB(Atomic *atomic, metal::InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	metal::setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = metal::lightingCB(atomic);

	metal::setupVertexInput(header);
	if(hostStage1)
		metal::setTexture(1, hostStage1);

	metal::InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	while(n--){
		m = inst->material;
		metal::setMaterial(flags, m->color, m->surfaceProps);
		metal::setTexture(0, m->texture);
		SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		hostShader->use(metal::drawVariant(vsBits));
		metal::drawInst(header, inst);
		inst++;
	}
	if(hostStage1)
		metal::setTexture(1, nil);
	metal::teardownVertexInput(header);
}

static void
HostSkinRenderCB(Atomic *atomic, metal::InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	metal::setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = metal::lightingCB(atomic);

	metal::setupVertexInput(header);

	metal::InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	metal::uploadSkinMatrices(atomic);

	while(n--){
		m = inst->material;
		metal::setMaterial(flags, m->color, m->surfaceProps);
		metal::setTexture(0, m->texture);
		SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		hostShader->use(metal::drawVariant(vsBits));
		metal::drawInst(header, inst);
		inst++;
	}
	metal::teardownVertexInput(header);
}

// leaves the blend state to the caller
static void
SecondPassRenderCB(Atomic *atomic, metal::InstanceDataHeader *header)
{
	metal::setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = metal::lightingCB(atomic);
	metal::setupVertexInput(header);
	metal::InstanceData *inst = header->inst;
	for(uint32 n = 0; n < header->numMeshes; n++, inst++){
		hostShader->use(metal::drawVariant(vsBits));
		metal::drawInst(header, inst);
	}
	metal::teardownVertexInput(header);
}

static metal::ObjPipeline*
HostPipe(void (*instanceCB)(Geometry*, metal::InstanceDataHeader*, bool32),
         void (*renderCB)(Atomic*, metal::InstanceDataHeader*))
{
	metal::ObjPipeline *pipe = metal::ObjPipeline::create();
	pipe->instanceCB = instanceCB;
	pipe->uninstanceCB = metal::defaultUninstanceCB;
	pipe->renderCB = renderCB;
	return pipe;
}

static bool
ProbeCentre(Atomic *atomic, RGBA want, int tol, RGBA *got, const char *what)
{
	Image *img = RenderOne(atomic);
	if(img == nil)
		return false;
	uint8 *p = &img->pixels[240*img->stride + 320*4];
	got->red = p[0];
	got->green = p[1];
	got->blue = p[2];
	got->alpha = p[3];
	bool ok = Near(img, 320, 240, want, tol);
	img->destroy();
	if(!ok)
		Detail("  in case %s\n", what);
	return ok;
}

static bool
SamePixel(RGBA a, RGBA b, int tol, const char *what)
{
	return Expect(abs(a.red - b.red) <= tol && abs(a.green - b.green) <= tol && abs(a.blue - b.blue) <= tol &&
	              abs(a.alpha - b.alpha) <= tol, "%s: engine %d,%d,%d,%d, host %d,%d,%d,%d", what,
	              a.red, a.green, a.blue, a.alpha, b.red, b.green, b.blue, b.alpha);
}

static bool
CheckHostShadersOnEngineSources(void)
{
	const char *name = "host shaders built from the exported default and skin sources draw like the engine's pipelines";
	static const SurfaceProperties surf = { 1.0f, 0.0f, 1.0f };
	static const uint8 indices[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
	static const float weights[4][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };
	const RGBA want = { 153, 133, 112, 255 };
	metal::Shader *defSh = CreateHostDefaultShader();
	metal::Shader *skinSh = CreateHostSkinShader();
	if(defSh == nil || skinSh == nil){
		Detail("  a host shader could not be built\n");
		if(defSh)
			defSh->destroy();
		if(skinSh)
			skinSh->destroy();
		return Report(false, name);
	}
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.2f, 0.2f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.4f, 0.32f, 0.24f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	metal::ObjPipeline *defPipe = HostPipe(metal::defaultInstanceCB, HostRenderCB);
	metal::ObjPipeline *skinPipe = HostPipe(metal::skinInstanceCB, HostSkinRenderCB);
	RGBA engine, host;
	metal::AttribDesc attribs[metal::MAXVERTEXATTRIBS];
	int32 n = metal::defaultVertexAttribs(1, 0, 0, attribs);
	bool ok = Expect(metal::prewarmShader(defSh, attribs, n, metal::VARIANT_DIRECTIONALS, 0, 0, 0, 1) == 1,
	                 "the host default shader was not prewarmed");
	n = metal::skinVertexAttribs(1, 0, 0, attribs);
	ok &= Expect(metal::prewarmShader(skinSh, attribs, n, metal::VARIANT_DIRECTIONALS, 0, 0, 0, 1) == 1,
	             "the host skin shader was not prewarmed");
	uint32 late = 0, lateBefore;

	Geometry *geo = LitQuad(surf);
	Atomic *atomic = MakeAtomic(geo);
	ok &= ProbeCentre(atomic, want, 1, &engine, "engine default pipeline");
	hostShader = defSh;
	atomic->pipeline = defPipe;
	lateBefore = metal::getStateStats().pipelinesLate;
	ok &= ProbeCentre(atomic, want, 1, &host, "host default pipeline");
	late += metal::getStateStats().pipelinesLate - lateBefore;
	ok &= SamePixel(engine, host, 1, "default");
	DestroyAtomic(atomic);
	geo->destroy();

	geo = LitQuad(surf);
	AttachSkin(geo, 1, nil, indices, weights);
	atomic = SkinAtomic(geo);
	ok &= ProbeCentre(atomic, want, 1, &engine, "engine skin pipeline");
	hostShader = skinSh;
	atomic->pipeline = skinPipe;
	lateBefore = metal::getStateStats().pipelinesLate;
	ok &= ProbeCentre(atomic, want, 1, &host, "host skin pipeline");
	late += metal::getStateStats().pipelinesLate - lateBefore;
	ok &= SamePixel(engine, host, 1, "skin");
	DestroySkinAtomic(atomic);
	geo->destroy();
	ok &= Expect(late == 0, "the host draws built %u pipelines after init", late);

	hostShader = nil;
	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	defPipe->destroy();
	skinPipe->destroy();
	defSh->destroy();
	skinSh->destroy();
	return Report(ok, name);
}

static bool
CheckHostPrewarm3D(void)
{
	const char *name = "a host shader prewarmed with a public vertex layout draws two uv sets without a late pipeline";
	metal::AttribDesc a[metal::MAXVERTEXATTRIBS], s[metal::MAXVERTEXATTRIBS];
	int32 n = metal::defaultVertexAttribs(0, 1, 2, a);
	bool ok = Expect(n == 4, "default layout has %d attributes, expected 4", n);
	if(n == 4){
		ok &= Expect(a[0].index == metal::ATTRIB_POS && a[0].offset == 0, "position %u at %u", a[0].index, a[0].offset);
		ok &= Expect(a[1].index == metal::ATTRIB_COLOR && a[1].offset == 12 && a[1].format == metal::ATTRIBFMT_UCHAR4_NORM,
		             "colour %u at %u format %d", a[1].index, a[1].offset, a[1].format);
		ok &= Expect(a[2].index == metal::ATTRIB_TEXCOORDS0 && a[2].offset == 16, "texcoords0 %u at %u", a[2].index, a[2].offset);
		ok &= Expect(a[3].index == metal::ATTRIB_TEXCOORDS1 && a[3].offset == 24, "texcoords1 %u at %u", a[3].index, a[3].offset);
		for(int32 i = 0; i < n; i++)
			ok &= Expect(a[i].stride == 32, "attribute %d stride %u", i, a[i].stride);
	}
	int32 ns = metal::skinVertexAttribs(1, 1, 1, s);
	ok &= Expect(ns == 6, "skin layout has %d attributes, expected 6", ns);
	for(int32 i = 0; i < ns && i < metal::MAXVERTEXATTRIBS; i++){
		ok &= Expect(s[i].stride == 56, "skin attribute %d stride %u", i, s[i].stride);
		if(s[i].index == metal::ATTRIB_WEIGHTS)
			ok &= Expect(s[i].offset == 36, "weights at %u", s[i].offset);
		if(s[i].index == metal::ATTRIB_INDICES)
			ok &= Expect(s[i].offset == 52, "indices at %u", s[i].offset);
	}

	metal::Shader *sh = CreateTwoUVShader();
	if(sh == nil){
		Detail("  the two-uv shader could not be built\n");
		return Report(false, name);
	}
	ok &= Expect(sh->textureStages == -1, "texture stages %d before the first pipeline", sh->textureStages);
	metal::StateStats s0 = metal::getStateStats();
	bool32 built = metal::prewarmShader(sh, a, n, 0, 0, 0, 0, 1);
	metal::StateStats s1 = metal::getStateStats();
	ok &= Expect(built == 1, "prewarmShader returned %d", built);
	ok &= Expect(s1.pipelinesHost == s0.pipelinesHost + 1, "host pipelines +%u", s1.pipelinesHost - s0.pipelinesHost);
	ok &= Expect(s1.pipelinesAtInit == s0.pipelinesAtInit && s1.pipelinesLate == s0.pipelinesLate,
	             "init +%u, late +%u", s1.pipelinesAtInit - s0.pipelinesAtInit, s1.pipelinesLate - s0.pipelinesLate);
	ok &= Expect(sh->textureStages == 3, "texture stages %d, expected 3", sh->textureStages);
	ok &= Expect(metal::prewarmShader(nil, a, n, 0, 0, 0, 0, 1) == 0, "a nil shader was prewarmed");
	ok &= Expect(metal::prewarmShader(sh, nil, 0, 0, 0, 0, 0, 1) == 0, "a nil layout was prewarmed");
	if(n == 4){
		metal::AttribDesc bad[4];
		Error err;
		getError(&err);
		memcpy(bad, a, sizeof(bad));
		bad[1].format = metal::ATTRIBFMT_UCHAR4_NORM + 3;
		ok &= Expect(metal::prewarmShader(sh, bad, n, 0, 0, 0, 0, 1) == 0, "a bad attribute format was prewarmed");
		getError(&err);
		ok &= Expect(err.code == ERR_GENERAL, "bad format: error code %u, expected ERR_GENERAL", err.code);
		memcpy(bad, a, sizeof(bad));
		bad[2].index = 31;
		ok &= Expect(metal::prewarmShader(sh, bad, n, 0, 0, 0, 0, 1) == 0, "attribute index 31 was prewarmed");
		getError(&err);
		ok &= Expect(err.code == ERR_GENERAL, "bad index: error code %u, expected ERR_GENERAL", err.code);
		metal::StateStats sb = metal::getStateStats();
		ok &= Expect(sb.pipelinesHost == s1.pipelinesHost && sb.pipelinesLate == s1.pipelinesLate &&
		             sb.pipelineFailures == s1.pipelineFailures, "bad attributes built host +%u, late +%u, failed +%u",
		             sb.pipelinesHost - s1.pipelinesHost, sb.pipelinesLate - s1.pipelinesLate,
		             sb.pipelineFailures - s1.pipelineFailures);
	}

	static const RGBA base = { 200, 100, 50, 255 };
	static const RGBA e4[4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 } };
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	Texture *t0 = MakeTexture(1, 1, &base, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *t1 = MakeTexture(2, 2, e4, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	if(t0 == nil || t1 == nil){
		if(t0)
			t0->destroy();
		if(t1)
			t1->destroy();
		sh->destroy();
		return Report(false, name);
	}
	Material *mat = Material::create();
	mat->setTexture(t0);
	t0->destroy();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED2 | Geometry::POSITIONS,
	                             1.0f, -1.0f, -1.0f, 1.0f, 10.0f, white, nil, mat);
	mat->destroy();
	for(int i = 0; i < 4; i++){
		geo->texCoords[0][i].u = 0.25f;
		geo->texCoords[0][i].v = 0.75f;
		geo->texCoords[1][i].u = 0.75f;
		geo->texCoords[1][i].v = 0.25f;
	}
	Atomic *atomic = MakeAtomic(geo);
	metal::ObjPipeline *pipe = HostPipe(metal::defaultInstanceCB, HostRenderCB);
	atomic->pipeline = pipe;
	hostShader = sh;
	hostStage1 = t1;
	RGBA got;
	ok &= ProbeCentre(atomic, makeRGBA(0, 100, 0, 255), 2, &got, "two uv sets");
	hostStage1 = nil;
	hostShader = nil;
	metal::InstanceDataHeader *header = (metal::InstanceDataHeader*)geo->instData;
	uint32 layout = metal::registerVertexLayout(a, n);
	ok &= Expect(header && header->vertexLayout == layout, "instanced layout %u, prewarmed %u",
	             header ? header->vertexLayout : 0, layout);
	metal::StateStats s2 = metal::getStateStats();
	ok &= Expect(s2.pipelinesLate == s0.pipelinesLate, "%u pipelines built after init", s2.pipelinesLate - s0.pipelinesLate);

	DestroyAtomic(atomic);
	geo->destroy();
	pipe->destroy();
	t1->destroy();
	sh->destroy();
	return Report(ok, name);
}

static bool
CheckEqualDepthSecondPass(void)
{
	const char *name = "a second pass with another vertex shader lands on the first pass's depth";
	static const RGBA reds[4] = { RED, RED, RED, RED };
	metal::Shader *sh = CreateSecondPassShader();
	if(sh == nil){
		Detail("  the second pass shader could not be built\n");
		return Report(false, name);
	}
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::NORMALS | Geometry::LIGHT | Geometry::POSITIONS,
	                             2.0f, -2.0f, -2.0f, 2.0f, 0.0f, reds, nil, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	V3d yAxis = { 0.0f, 1.0f, 0.0f }, pos = { 0.0f, 0.0f, 10.0f };
	atomic->getFrame()->rotate(&yAxis, 60.0f, COMBINEREPLACE);
	atomic->getFrame()->translate(&pos, COMBINEPOSTCONCAT);
	metal::ObjPipeline *pipe = HostPipe(metal::defaultInstanceCB, SecondPassRenderCB);
	Light *dir = MakeLight(Light::DIRECTIONAL, 1.0f, 0.0f, 0.0f);
	saved.world->addLight(dir);

	WorldBegin(GREY);
	atomic->render();
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDONE);
	hostShader = sh;
	atomic->pipeline = pipe;
	atomic->render();
	hostShader = nil;
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	WorldEnd();

	bool ok = false;
	Image *img = ReadCamera();
	if(img){
		int red = 0, missed = 0;
		for(int y = 0; y < img->height; y++)
			for(int x = 0; x < img->width; x++){
				uint8 *p = &img->pixels[y*img->stride + x*4];
				if(p[0] >= 250 && p[1] <= 5){
					red++;
					if(p[2] < 60)
						missed++;
				}
			}
		img->destroy();
		Detail("  red %d, missed by the second pass %d\n", red, missed);
		ok = red > 2000 && missed == 0;
	}
	saved.world->removeLight(dir);
	DestroyLight(dir);
	ClearLights();
	DestroyAtomic(atomic);
	geo->destroy();
	pipe->destroy();
	sh->destroy();
	return Report(ok, name);
}

int
RunWorldChecks(Camera *camera)
{
	failures = 0;
	WorldOpen(camera);
	CheckGamePipelinesPrewarmed();
	CheckInstanceTwoMaterials();
	CheckInstanceStrip();
	CheckReinstance();
	CheckDestroyNativeData();
	CheckAxesAndWorldMatrix();
	CheckRotatedWorldMatrix();
	CheckDepthRange();
	CheckDepthOrderAndWrite();
	CheckWindingAndCulling();
	CheckVertexColours();
	CheckTextureOrigin();
	CheckStripIsNotList();
	CheckTwoMeshes();
	CheckMidPassDepthClear();
	CheckLitColours();
	CheckUnlitGeometryGetsNoLights();
	CheckVariantChosenPerDraw();
	CheckSecondFrameUploads();
	CheckRingBytesPerFrame();
	CheckWorldLightCases();
	CheckFog();
	CheckMaterialColour();
	CheckCutout();
	CheckPS2AlphaTest();
	CheckSamplersPerMaterial();
	CheckFilterPerMaterial();
	CheckRingAllocStartsFrame();
	CheckStripRestartIndexCounted();
	CheckIm3DColourAndWorldMatrix();
	CheckIm3DTexture();
	CheckIm3DSeveralRenders();
	CheckIm3DPrimitiveTypes();
	CheckIm3DWideFan();
	CheckIm3DTwoTransforms();
	CheckIm3DTransformBeforeFrame();
	CheckIm3DEndWithoutRender();
	CheckIm3DDepth();
	CheckIm3DFlags();
	CheckIm3DFog();
	CheckIm3DLighting();
	CheckLitAtomicWithoutWorld();
	CheckMatFXFallback();
	CheckDrawsCounted();
	CheckDrawVariant();
	CheckHostShadersOnEngineSources();
	CheckHostPrewarm3D();
	CheckEqualDepthSecondPass();
	WorldClose();
	return failures;
}

static bool
SameLayout(uint32 layout, const metal::AttribDesc *want, int32 numWant)
{
	metal::AttribDesc got[16];
	int32 n = metal::getVertexLayout(layout, got, 16);
	return n == numWant && n > 0 && memcmp(got, want, n*sizeof(metal::AttribDesc)) == 0;
}

static bool
CheckGeometryAcrossRestart(bool (*restart)(void))
{
	const char *name = "instanced geometry keeps its vertex layout and buffers across a device restart";
	static const RGBA col[4] = {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 10, 20, 30, 255 }, { 0, 0, 255, 255 } };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.25f, 0.75f }, { 0.0f, 1.0f } };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             2.0f, -1.0f, -2.0f, 1.5f, 10.0f, col, uv, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	DestroyAtomic(atomic);
	bool ok = h != nil && h->numAttribs > 0 && h->numAttribs <= 16;
	uint32 layout = 0;
	metal::AttribDesc attribs[16];
	int32 numAttribs = 0;
	uint8 vbytes[96], ibytes[12];
	if(ok){
		layout = h->vertexLayout;
		numAttribs = h->numAttribs;
		memcpy(attribs, h->attribDesc, numAttribs*sizeof(metal::AttribDesc));
		ok &= Expect(SameLayout(layout, attribs, numAttribs), "layout %u does not resolve before the restart", layout);
		ok &= Expect(BufferBytes(h->mtlVertexBuffer, 0, vbytes, sizeof(vbytes)) &&
		             BufferBytes(h->mtlIndexBuffer, 0, ibytes, sizeof(ibytes)),
		             "the buffers could not be read before the restart");
	}
	if(ok && !restart()){
		Detail("  the restart failed\n");
		ok = false;
	}
	if(ok){
		ok &= Expect(SameLayout(layout, attribs, numAttribs), "layout %u no longer resolves to the geometry's layout", layout);
		atomic = MakeAtomic(geo);
		metal::InstanceDataHeader *h2 = Instanced(atomic);
		DestroyAtomic(atomic);
		ok &= Expect(h2 == h && h->vertexLayout == layout, "the instance header or its layout id changed");
		uint8 vafter[96], iafter[12];
		ok &= Expect(BufferOnCurrentDevice(h->mtlVertexBuffer) && BufferOnCurrentDevice(h->mtlIndexBuffer),
		             "the buffers do not belong to the device in use");
		ok &= Expect(BufferBytes(h->mtlVertexBuffer, 0, vafter, sizeof(vafter)) &&
		             memcmp(vafter, vbytes, sizeof(vbytes)) == 0, "the vertex buffer bytes changed");
		ok &= Expect(BufferBytes(h->mtlIndexBuffer, 0, iafter, sizeof(iafter)) &&
		             memcmp(iafter, ibytes, sizeof(ibytes)) == 0, "the index buffer bytes changed");

		Material *mat2 = Material::create();
		Geometry *geo2 = QuadGeometry(Geometry::NORMALS | Geometry::POSITIONS,
		                              2.0f, -1.0f, -2.0f, 1.5f, 10.0f, nil, nil, mat2);
		mat2->destroy();
		atomic = MakeAtomic(geo2);
		metal::InstanceDataHeader *h3 = Instanced(atomic);
		DestroyAtomic(atomic);
		if(h3){
			ok &= Expect(h3->vertexLayout != 0 && h3->vertexLayout != layout,
			             "the new layout id %u collides with %u", h3->vertexLayout, layout);
			ok &= Expect(SameLayout(h3->vertexLayout, h3->attribDesc, h3->numAttribs), "the new layout does not resolve");
			ok &= Expect(SameLayout(layout, attribs, numAttribs), "layout %u changed after a new registration", layout);
		}else
			ok = false;
		geo2->destroy();
	}
	geo->destroy();
	return Report(ok, name);
}

static Atomic*
SkinnedAtomic(Geometry *geo)
{
	Skin *skin = rwNewT(Skin, 1, MEMDUR_EVENT | ID_SKIN);
	skin->init(1, 1, geo->numVertices);
	skin->usedBones[0] = 0;
	Matrix identity;
	identity.setIdentity();
	RawMatrix raw;
	convMatrix(&raw, &identity);
	memcpy(skin->inverseMatrices, &raw, 64);
	for(int i = 0; i < geo->numVertices; i++){
		memset(&skin->indices[i*4], 0, 4);
		skin->weights[i*4] = 1.0f;
		skin->weights[i*4+1] = skin->weights[i*4+2] = skin->weights[i*4+3] = 0.0f;
	}
	Skin::set(geo, skin);
	Atomic *atomic = MakeAtomic(geo);
	Skin::setPipeline(atomic, 1);
	return atomic;
}

static HAnimHierarchy *MakeHierarchy(int32 numNodes, const Matrix *bones, int32 flags);
static Matrix Translation(float x, float y, float z);

static const RGBA YELLOW = { 255, 255, 0, 255 };

struct EnvGlobals
{
	bool32 flipU, applyLight, useMatColor;
	RGBA colour;
};

static EnvGlobals
SetEnvGlobals(bool32 flipU, bool32 applyLight, bool32 useMatColor, RGBA colour)
{
	EnvGlobals prev = { MatFX::envMapFlipU, MatFX::envMapApplyLight, MatFX::envMapUseMatColor, MatFX::envMapColor };
	MatFX::envMapFlipU = flipU;
	MatFX::envMapApplyLight = applyLight;
	MatFX::envMapUseMatColor = useMatColor;
	MatFX::envMapColor = colour;
	return prev;
}

static void
RestoreEnvGlobals(const EnvGlobals &g)
{
	MatFX::envMapFlipU = g.flipU;
	MatFX::envMapApplyLight = g.applyLight;
	MatFX::envMapUseMatColor = g.useMatColor;
	MatFX::envMapColor = g.colour;
}

static Texture*
Tex1(RGBA c)
{
	return MakeTexture(1, 1, &c, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
}

static bool
CheckPipelinesAcrossRestart(bool (*restart)(void), Camera *(*camera)(void))
{
	const char *name = "matfx and skinned atomics keep a live Metal pipeline across a restart, and both draw again with their env pass";
	static const RGBA red[4] = { RED, RED, RED, RED };
	Material *mat = Material::create();
	MatFX::setEffects(mat, MatFX::ENVMAP);
	MatFX *fx = MatFX::get(mat);
	fx->setEnvCoefficient(1.0f);
	fx->setEnvFrame(nil);
	EnvGlobals g = SetEnvGlobals(0, 0, 0, WHITE);
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS, 1.0f, -1.0f, -1.0f, 1.0f, 10.0f, red, nil, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	MatFX::enableEffects(atomic);
	Geometry *skinned = SolidQuad(3.0f, -1.0f, 2.0f, 1.0f, 10.0f, BLUE);
	Atomic *skinAtomic = SkinnedAtomic(skinned);
	Matrix bone = Translation(0.0f, -1.0f, 0.0f);
	Skin::setHierarchy(skinAtomic, MakeHierarchy(1, &bone, 0));
	rw::ObjPipeline *defaultPipe = engine->driver[PLATFORM_METAL]->defaultPipeline;
	rw::ObjPipeline *savedMatfx = matFXGlobals.pipelines[PLATFORM_METAL], *savedSkin = skinGlobals.pipelines[PLATFORM_METAL];
	bool ok = true;

	for(int pass = 0; pass < 2; pass++){
		const char *when = pass ? "after" : "before";
		if(pass && !restart()){
			Detail("  the restart failed\n");
			ok = false;
			break;
		}
		ok &= Expect(atomic->pipeline && atomic->pipeline->pluginID == ID_MATFX &&
		             atomic->pipeline->platform == PLATFORM_METAL &&
		             atomic->pipeline == matFXGlobals.pipelines[PLATFORM_METAL],
		             "%s the restart the matfx atomic's pipeline is not the live Metal matfx pipeline", when);
		ok &= Expect(skinAtomic->pipeline && skinAtomic->pipeline->pluginID == ID_SKIN &&
		             skinAtomic->pipeline->platform == PLATFORM_METAL &&
		             skinAtomic->pipeline == skinGlobals.pipelines[PLATFORM_METAL],
		             "%s the restart the skinned atomic's pipeline is not the live Metal skin pipeline", when);
		ok &= Expect(engine->driver[PLATFORM_METAL]->defaultPipeline == defaultPipe,
		             "%s the restart the default pipeline was replaced", when);
		ok &= Expect(!pass || atomic->pipeline == savedMatfx,
		             "after the restart the matfx atomic's pipeline is not the one saved before it");
		ok &= Expect(!pass || skinAtomic->pipeline == savedSkin,
		             "after the restart the skinned atomic's pipeline is not the one saved before it");
		Texture *green = Tex1(GREEN);
		fx->setEnvTexture(green);
		if(green)
			green->destroy();
		WorldOpen(camera());
		uint32 before = metal::getStateStats().skinnedUnrouted;
		WorldBegin(GREY);
		atomic->render();
		skinAtomic->render();
		WorldEnd();
		uint32 unrouted = metal::getStateStats().skinnedUnrouted - before;
		ok &= Expect(unrouted == 0, "%s the restart %u skinned renders counted as unrouted, expected 0", when, unrouted);
		ok &= Expect(!pass || (metal::matfxEnvShader && metal::matfxEnvShader->shaderId == 5),
		             "after the restart the matfx env shader is missing or its id is not 5");
		const PixelWant want[] = { { 320, 240, YELLOW, 0 }, { 240, 272, BLUE, 0 }, { 240, 224, GREY, 0 } };
		ok &= Pixels(want, nelem(want), when);
		fx->setEnvTexture(nil);
		WorldClose();
	}
	RestoreEnvGlobals(g);
	DestroySkinAtomic(skinAtomic);
	skinned->destroy();
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

int
RunRestartWorldChecks(bool (*restart)(void), Camera *(*camera)(void))
{
	failures = 0;
	CheckGeometryAcrossRestart(restart);
	CheckPipelinesAcrossRestart(restart, camera);
	return failures;
}

Skin*
AttachSkin(Geometry *geo, int32 numBones, const Matrix *invBind,
           const uint8 (*indices)[4], const float (*weights)[4])
{
	Skin *skin = rwNewT(Skin, 1, MEMDUR_EVENT | ID_SKIN);
	skin->init(numBones, numBones, geo->numVertices);
	Matrix identity;
	identity.setIdentity();
	for(int32 i = 0; i < numBones; i++){
		skin->usedBones[i] = i;
		memcpy(skin->inverseMatrices + 16*i, invBind ? &invBind[i] : &identity, 64);
	}
	for(int32 i = 0; i < geo->numVertices; i++){
		memcpy(&skin->indices[i*4], indices[i], 4);
		memcpy(&skin->weights[i*4], weights[i], 16);
	}
	skin->findNumWeights(geo->numVertices);
	Skin::set(geo, skin);
	return skin;
}

Atomic*
SkinAtomic(Geometry *geo)
{
	Atomic *atomic = MakeAtomic(geo);
	Skin::setPipeline(atomic, 1);
	return atomic;
}

static const uint8 skinQuadIndices[4][4] = { { 0, 3, 1, 2 }, { 1, 2, 1, 2 }, { 2, 1, 1, 2 }, { 3, 0, 1, 2 } };
static const float skinQuadWeights[4][4] = {
	{ 0.5f, 0.25f, 0.125f, 0.125f }, { 0.5f, 0.25f, 0.125f, 0.125f },
	{ 0.5f, 0.25f, 0.125f, 0.125f }, { 0.5f, 0.25f, 0.125f, 0.125f } };

static Geometry*
SkinQuad(void)
{
	static const RGBA col[4] = {
		{ 255, 0, 0, 128 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             2.0f, -1.0f, -2.0f, 1.5f, 10.0f, col, uv, mat);
	mat->destroy();
	AttachSkin(geo, 4, nil, skinQuadIndices, skinQuadWeights);
	return geo;
}

static bool
SkinBytes(metal::InstanceDataHeader *h, uint32 stride, int32 v, float weights[4], uint8 indices[4])
{
	return BufferBytes(h->mtlVertexBuffer, stride*v + stride-20, weights, 16) &&
	       BufferBytes(h->mtlVertexBuffer, stride*v + stride-4, indices, 4);
}

static bool
CheckSkinInstanceLayout(void)
{
	const char *name = "a skinned geometry is instanced with gl3's skin layout, weights and bone indices, and no vertex alpha";
	Geometry *geo = SkinQuad();
	Atomic *atomic = SkinAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil;
	if(h){
		metal::InstAttrib want[metal::MAXINSTATTRIBS];
		int32 n = metal::skinVertexLayout(true, true, 1, want);
		ok &= Expect(h->numAttribs == 6 && n == 6, "%d attributes, expected 6", h->numAttribs);
		ok &= Expect(h->numAttribs == n && h->attribDesc && memcmp(h->attribDesc, want, n*sizeof(metal::AttribDesc)) == 0,
		             "the attributes are not skinVertexLayout(true, true, 1)");
		ok &= Expect(SameLayout(h->vertexLayout, (const metal::AttribDesc*)want, n),
		             "layout %u does not resolve to skinVertexLayout(true, true, 1)", h->vertexLayout);
		for(int32 v = 0; v < 4; v++){
			float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			uint8 idx[4] = { 0, 0, 0, 0 };
			bool read = BufferBytes(h->mtlVertexBuffer, 56*v + 36, w, 16) &&
			            BufferBytes(h->mtlVertexBuffer, 56*v + 52, idx, 4);
			ok &= Expect(read && memcmp(w, skinQuadWeights[v], 16) == 0,
			             "vertex %d weights %g,%g,%g,%g, expected 0.5,0.25,0.125,0.125", v, w[0], w[1], w[2], w[3]);
			ok &= Expect(read && memcmp(idx, skinQuadIndices[v], 4) == 0,
			             "vertex %d indices %d,%d,%d,%d, expected %d,%d,%d,%d", v, idx[0], idx[1], idx[2], idx[3],
			             skinQuadIndices[v][0], skinQuadIndices[v][1], skinQuadIndices[v][2], skinQuadIndices[v][3]);
		}
		ok &= Expect(h->numMeshes > 0 && h->inst[0].vertexAlpha == 0,
		             "mesh 0 vertexAlpha is %d, expected 0", h->numMeshes > 0 ? h->inst[0].vertexAlpha : -1);
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinReinstanceKeepsWeights(void)
{
	const char *name = "a relocked skinned geometry gets a new vertex buffer and keeps its weights and bone indices";
	Geometry *geo = SkinQuad();
	Atomic *atomic = SkinAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil && h->numAttribs > 0 && h->attribDesc && h->attribDesc[0].stride == 56;
	if(h && !ok)
		Detail("  %d attributes, stride %u, expected the 56-byte skin layout\n",
		       h->numAttribs, h->numAttribs > 0 && h->attribDesc ? h->attribDesc[0].stride : 0);
	if(ok){
		float pos[3];
		ok &= Expect(BufferBytes(h->mtlVertexBuffer, 2*56, pos, 12), "vertex 2 position could not be read");
		void *before = h->mtlVertexBuffer;
		geo->lock(Geometry::LOCKVERTICES);
		geo->morphTargets[0].vertices[2].x += 0.5f;
		geo->unlock();
		metal::InstanceDataHeader *h2 = Instanced(atomic);
		ok &= Expect(h2 == h, "the instance header was replaced");
		if(ok){
			float pos2[3];
			ok &= Expect(h->mtlVertexBuffer != nil && h->mtlVertexBuffer != before, "the vertex buffer was not replaced");
			ok &= Expect(BufferBytes(h->mtlVertexBuffer, 2*56, pos2, 12) && memcmp(pos, pos2, 12) != 0 &&
			             pos2[0] == pos[0] + 0.5f, "vertex 2 x is %g, expected %g", pos2[0], pos[0] + 0.5f);
			for(int32 v = 0; v < 4; v++){
				float w2[4];
				uint8 idx2[4];
				ok &= Expect(SkinBytes(h, 56, v, w2, idx2) && memcmp(w2, skinQuadWeights[v], 16) == 0 &&
				             memcmp(idx2, skinQuadIndices[v], 4) == 0,
				             "vertex %d weights or indices differ from the skin after reinstance", v);
			}
		}
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinPipelineWithoutSkin(void)
{
	const char *name = "a geometry without skin data on the skin pipeline is instanced with the default layout";
	static const RGBA col[4] = { RED, RED, RED, RED };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS, 2.0f, -1.0f, -2.0f, 1.5f, 10.0f, col, nil, mat);
	mat->destroy();
	Atomic *atomic = SkinAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = Expect(atomic->pipeline == skinGlobals.pipelines[PLATFORM_METAL], "the atomic is not on the skin pipeline");
	ok &= h != nil;
	if(h){
		metal::InstAttrib want[metal::MAXINSTATTRIBS];
		int32 n = metal::defaultVertexLayout(false, true, 0, want);
		ok &= Expect(h->numAttribs == n && h->attribDesc && memcmp(h->attribDesc, want, n*sizeof(metal::AttribDesc)) == 0,
		             "%d attributes, expected defaultVertexLayout(false, true, 0) with %d", h->numAttribs, n);
		ok &= Expect(SameLayout(h->vertexLayout, (const metal::AttribDesc*)want, n),
		             "layout %u does not resolve to defaultVertexLayout(false, true, 0)", h->vertexLayout);
		for(int32 i = 0; i < h->numAttribs && h->attribDesc; i++)
			ok &= Expect(h->attribDesc[i].index != metal::ATTRIB_WEIGHTS && h->attribDesc[i].index != metal::ATTRIB_INDICES,
			             "attribute %d has index %u", i, h->attribDesc[i].index);
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinLayoutPositionsOnly(void)
{
	const char *name = "a positions-only skinned geometry has weights at 12 and indices at 28 in a 32-byte vertex";
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::POSITIONS, 2.0f, -1.0f, -2.0f, 1.5f, 10.0f, nil, nil, mat);
	mat->destroy();
	AttachSkin(geo, 4, nil, skinQuadIndices, skinQuadWeights);
	Atomic *atomic = SkinAtomic(geo);
	metal::InstanceDataHeader *h = Instanced(atomic);
	bool ok = h != nil;
	if(h){
		static const uint32 index[3] = { metal::ATTRIB_POS, metal::ATTRIB_WEIGHTS, metal::ATTRIB_INDICES };
		static const int32 format[3] = { metal::ATTRIBFMT_FLOAT3, metal::ATTRIBFMT_FLOAT4, metal::ATTRIBFMT_UCHAR4 };
		static const uint32 offset[3] = { 0, 12, 28 };
		ok &= Expect(h->numAttribs == 3 && h->attribDesc, "%d attributes, expected 3", h->numAttribs);
		for(int32 i = 0; ok && i < 3; i++){
			metal::AttribDesc *a = &h->attribDesc[i];
			ok &= Expect(a->index == index[i] && a->format == format[i] && a->offset == offset[i] && a->stride == 32,
			             "attribute %d is {%u, %d, stride %u, offset %u}, expected {%u, %d, stride 32, offset %u}",
			             i, a->index, a->format, a->stride, a->offset, index[i], format[i], offset[i]);
		}
		for(int32 v = 0; ok && v < 4; v++){
			float w[4];
			uint8 idx[4];
			ok &= Expect(SkinBytes(h, 32, v, w, idx) && memcmp(w, skinQuadWeights[v], 16) == 0 &&
			             memcmp(idx, skinQuadIndices[v], 4) == 0, "vertex %d weights or indices differ", v);
		}
	}
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static HAnimHierarchy*
MakeHierarchy(int32 numNodes, const Matrix *bones, int32 flags)
{
	HAnimHierarchy *hier = HAnimHierarchy::create(numNodes, nil, nil, flags, 36);
	memcpy(hier->matrices, bones, numNodes*sizeof(Matrix));
	return hier;
}

void
DestroySkinAtomic(Atomic *atomic)
{
	HAnimHierarchy *hier = Skin::getHierarchy(atomic);
	Skin::setHierarchy(atomic, nil);
	if(hier)
		hier->destroy();
	DestroyAtomic(atomic);
}

struct Marker { float cx, cy, half, z; RGBA colour; uint8 idx[4]; float w[4]; };

static Geometry*
Markers(uint32 flags, const Marker *m, int n, const uint8 *matAlpha = nil)
{
	Geometry *geo = Geometry::create(4*n, 2*n, flags | Geometry::POSITIONS);
	int numMats = matAlpha ? n : 1;
	uint8 (*indices)[4] = new uint8[4*n][4];
	float (*weights)[4] = new float[4*n][4];
	int32 numBones = 1;
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < n; i++){
		v[4*i+0].set(m[i].cx + m[i].half, m[i].cy + m[i].half, m[i].z);
		v[4*i+1].set(m[i].cx - m[i].half, m[i].cy + m[i].half, m[i].z);
		v[4*i+2].set(m[i].cx - m[i].half, m[i].cy - m[i].half, m[i].z);
		v[4*i+3].set(m[i].cx + m[i].half, m[i].cy - m[i].half, m[i].z);
		for(int j = 0; j < 4; j++){
			if(flags & Geometry::NORMALS)
				geo->morphTargets[0].normals[4*i+j].set(0.0f, 0.0f, -1.0f);
			if(geo->colors)
				geo->colors[4*i+j] = m[i].colour;
			memcpy(indices[4*i+j], m[i].idx, 4);
			memcpy(weights[4*i+j], m[i].w, 16);
			if(m[i].idx[j] + 1 > numBones)
				numBones = m[i].idx[j] + 1;
		}
		Triangle *t = &geo->triangles[2*i];
		t[0].v[0] = 4*i; t[0].v[1] = 4*i+1; t[0].v[2] = 4*i+2; t[0].matId = i % numMats;
		t[1].v[0] = 4*i; t[1].v[1] = 4*i+2; t[1].v[2] = 4*i+3; t[1].matId = i % numMats;
	}
	for(int i = 0; i < numMats; i++){
		Material *mat = Material::create();
		if(matAlpha)
			mat->color.alpha = matAlpha[i];
		geo->matList.appendMaterial(mat);
		mat->destroy();
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	AttachSkin(geo, numBones, nil, indices, weights);
	delete[] indices;
	delete[] weights;
	return geo;
}

static void
SetInvBind(Geometry *geo, int32 bone, const Matrix &m)
{
	memcpy(Skin::get(geo)->inverseMatrices + 16*bone, &m, 64);
}

static Matrix
Translation(float x, float y, float z)
{
	Matrix m;
	V3d t = { x, y, z };
	m.setIdentity();
	m.translate(&t, COMBINEREPLACE);
	return m;
}

static Matrix
RotationThenTranslation(V3d axis, float angle, float x, float y, float z)
{
	Matrix m;
	V3d t = { x, y, z };
	m.setIdentity();
	m.rotate(&axis, angle, COMBINEPOSTCONCAT);
	m.translate(&t, COMBINEPOSTCONCAT);
	return m;
}

static Matrix
Identity(void)
{
	Matrix m;
	m.setIdentity();
	return m;
}

static bool
CheckSkinNilHierarchy(void)
{
	const char *name = "a skinned atomic without a hierarchy draws with identity bones and uploads the skin block once";
	const Marker marker = { 2.0f, 0.0f, 0.5f, 10.0f, RED, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	Geometry *geo = Markers(Geometry::PRELIT, &marker, 1);
	SetInvBind(geo, 1, Translation(5.0f, 0.0f, 0.0f));
	Atomic *atomic = SkinAtomic(geo);
	uint32 before = metal::getStateStats().blockUploads[metal::BUFFER_SKIN];
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	uint32 uploads = metal::getStateStats().blockUploads[metal::BUFFER_SKIN] - before;
	const PixelWant want[] = { { 256, 240, RED, 0 }, { 96, 240, GREY, 0 } };
	bool ok = Pixels(want, nelem(want), "nil hierarchy");
	ok &= Expect(uploads == 1, "%u skin block uploads, expected 1", uploads);
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckBoneTransformAndOrder(void)
{
	const char *name = "bone matrices are invBind then the hierarchy matrix, not transposed and not reversed";
	const V3d zAxis = { 0.0f, 0.0f, 1.0f };
	const Marker m[5] = {
		{ 2.0f, 0.0f, 0.5f, 10.0f, RED, { 0, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ -1.75f, 0.0f, 0.25f, 10.0f, GREEN, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ -1.25f, 0.0f, 0.25f, 10.0f, GREEN, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ -0.75f, 0.0f, 0.25f, 10.0f, GREEN, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ -0.25f, 0.0f, 0.25f, 10.0f, GREEN, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
	};
	Geometry *geo = Markers(Geometry::PRELIT, m, 5);
	Matrix invBind1 = Translation(2.0f, 0.0f, 0.0f);
	SetInvBind(geo, 1, invBind1);
	Matrix bones[2] = { Identity(), RotationThenTranslation(zAxis, 90.0f, -1.0f, 1.0f, 0.0f) };
	Atomic *atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, bones, 0));

	V3d corners[2] = { { -2.0f, -0.25f, 10.0f }, { 0.0f, 0.25f, 10.0f } }, tmp[2], out[2];
	V3d::transformPoints(tmp, corners, 2, &invBind1);
	V3d::transformPoints(out, tmp, 2, &bones[1]);
	float x0 = fminf(out[0].x, out[1].x), x1 = fmaxf(out[0].x, out[1].x);
	float y0 = fminf(out[0].y, out[1].y), y1 = fmaxf(out[0].y, out[1].y);
	bool ok = Expect(fabsf(x0 + 1.25f) < 1e-5f && fabsf(x1 + 0.75f) < 1e-5f && fabsf(y0 - 1.0f) < 1e-5f &&
	                 fabsf(y1 - 3.0f) < 1e-5f && fabsf(out[0].z - 10.0f) < 1e-5f && fabsf(out[1].z - 10.0f) < 1e-5f,
	                 "bar B lands at x %g..%g y %g..%g z %g,%g, expected -1.25..-0.75, 1..3, 10",
	                 x0, x1, y0, y1, out[0].z, out[1].z);

	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	const PixelWant want[] = {
		{ 256, 240, RED, 0 }, { 352, 176, GREEN, 0 }, { 352, 240, GREY, 0 }, { 288, 240, GREY, 0 }, { 320, 208, GREY, 0 } };
	ok &= Pixels(want, nelem(want), "bone transform and order");
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckWeightsBlend(void)
{
	const char *name = "all four weights blend the bone positions";
	const Marker half = { -2.0f, 0.0f, 0.5f, 10.0f, RED, { 0, 1, 0, 0 }, { 0.5f, 0.5f, 0.0f, 0.0f } };
	Geometry *geo = Markers(Geometry::PRELIT, &half, 1);
	Matrix halfBones[2] = { Identity(), Translation(0.0f, 2.0f, 0.0f) };
	Atomic *atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, halfBones, 0));
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	const PixelWant wantHalf[] = { { 384, 208, RED, 0 }, { 384, 240, GREY, 0 }, { 384, 176, GREY, 0 } };
	bool ok = Pixels(wantHalf, nelem(wantHalf), "halfway");
	DestroySkinAtomic(atomic);
	geo->destroy();

	const Marker four = { 2.0f, -1.0f, 0.5f, 10.0f, RED, { 3, 2, 1, 0 }, { 0.25f, 0.25f, 0.25f, 0.25f } };
	geo = Markers(Geometry::PRELIT, &four, 1);
	Matrix fourBones[4] = { Translation(4.0f, 0.0f, 0.0f), Translation(0.0f, 4.0f, 0.0f), Identity(), Identity() };
	atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(4, fourBones, 0));
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	const PixelWant wantFour[] = { { 224, 240, RED, 0 }, { 256, 229, GREY, 0 } };
	ok &= Pixels(wantFour, nelem(wantFour), "four weights");
	DestroySkinAtomic(atomic);
	geo->destroy();

	const Marker scaled = { -2.0f, 0.0f, 0.5f, 10.0f, RED, { 0, 1, 0, 0 }, { 0.5f, 0.25f, 0.0f, 0.0f } };
	geo = Markers(Geometry::PRELIT, &scaled, 1);
	Geometry *wall = SolidQuad(-1.0f, -1.0f, -3.0f, 1.0f, 9.0f, GREEN);
	Matrix scaledBones[2] = { Identity(), Translation(0.0f, 0.0f, 2.0f) };
	atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, scaledBones, 0));
	WorldBegin(GREY);
	RenderGeometry(wall);
	atomic->render();
	WorldEnd();
	const PixelWant wantScaled[] = { { 384, 240, RED, 0 } };
	ok &= Pixels(wantScaled, nelem(wantScaled), "weights summing to 0.75");
	ok &= DepthNear(384, 240, DepthOf(8.0f), 2e-5f);
	DestroySkinAtomic(atomic);
	wall->destroy();
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinnedNormalsBlend(void)
{
	const char *name = "skinned normals blend by weight and are not renormalised before lighting";
	const V3d yAxis = { 0.0f, 1.0f, 0.0f };
	const Marker marker = { 0.0f, 0.0f, 0.5f, 10.0f, WHITE, { 0, 1, 0, 0 }, { 0.5f, 0.5f, 0.0f, 0.0f } };
	Geometry *geo = Markers(Geometry::NORMALS | Geometry::LIGHT, &marker, 1);
	SurfaceProperties surf = { 1.0f, 0.0f, 1.0f };
	geo->matList.materials[0]->surfaceProps = surf;
	SetInvBind(geo, 1, Translation(0.0f, 0.0f, -10.0f));
	Matrix bones[2] = { Identity(), RotationThenTranslation(yAxis, 90.0f, 0.0f, 0.0f, 10.0f) };
	Atomic *atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, bones, 0));
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.4f, 0.6f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	const PixelWant want[] = { { 320, 240, Rgb(115, 166, 217), 2 } };
	bool ok = Pixels(want, nelem(want), "blended normal");
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinnedFog(void)
{
	const char *name = "skinned geometry takes fog from clip z, as gl3's skin shader does";
	uint32 oldColour = GetRenderState(FOGCOLOR);
	float32 oldPlane = saved.camera->fogPlane;
	saved.camera->fogPlane = 11.0f;
	Geometry *plain = SolidQuad(4.0f, -2.0f, 8.0f, 2.0f, 56.0f, WHITE);
	const Marker marker = { -6.0f, 0.0f, 2.0f, 56.0f, WHITE, { 0, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	Geometry *geo = Markers(Geometry::PRELIT, &marker, 1);
	Atomic *atomic = SkinAtomic(geo);
	WorldBegin(GREY);
	SetRenderState(FOGCOLOR, RWRGBAINT(0, 0, 0, 255));
	SetRenderState(FOGENABLE, 1);
	RenderGeometry(plain);
	atomic->render();
	SetRenderState(FOGENABLE, 0);
	WorldEnd();
	const PixelWant want[] = { { 286, 240, Rgb(128, 128, 128), 1 }, { 354, 240, Rgb(130, 130, 130), 1 } };
	bool ok = Pixels(want, nelem(want), "fog");
	saved.camera->fogPlane = oldPlane;
	SetRenderState(FOGCOLOR, oldColour);
	DestroySkinAtomic(atomic);
	geo->destroy();
	plain->destroy();
	return Report(ok, name);
}

static bool
CheckSkinPipelineWithoutSkinDraws(void)
{
	const char *name = "a geometry without skin data on the skin pipeline draws with the default shader";
	static const RGBA col[4] = { BLUE, BLUE, BLUE, BLUE };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS, 2.0f, -1.0f, -2.0f, 1.5f, 10.0f, col, nil, mat);
	mat->destroy();
	Atomic *atomic = SkinAtomic(geo);
	WorldBegin(GREY);
	atomic->render();
	metal::Shader *shader = metal::currentShader;
	WorldEnd();
	const PixelWant want[] = { { 320, 240, BLUE, 0 } };
	bool ok = Pixels(want, nelem(want), "no skin data");
	ok &= Expect(shader == metal::defaultShader, "the draw used shader %p, expected the default shader %p",
	             (void*)shader, (void*)metal::defaultShader);
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinnedOnDefaultPipelineUnrouted(void)
{
	const char *name = "skinned geometry on the default pipeline draws unskinned and is counted; on the skin pipeline it is not";
	const Marker marker = { 2.0f, 0.0f, 0.5f, 10.0f, RED, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	Matrix bones[2] = { Identity(), Translation(0.0f, 2.0f, 0.0f) };
	Geometry *geo = Markers(Geometry::PRELIT, &marker, 1);
	Atomic *atomic = MakeAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, bones, 0));
	uint32 before = metal::getStateStats().skinnedUnrouted;
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	uint32 unrouted = metal::getStateStats().skinnedUnrouted - before;
	bool ok = Expect(atomic->pipeline == nil, "the atomic has a pipeline");
	ok &= Expect(unrouted == 1, "%u unrouted skinned renders, expected 1", unrouted);
	const PixelWant wantDefault[] = { { 256, 240, RED, 0 }, { 256, 176, GREY, 0 } };
	ok &= Pixels(wantDefault, nelem(wantDefault), "default pipeline");
	DestroySkinAtomic(atomic);
	geo->destroy();

	geo = Markers(Geometry::PRELIT, &marker, 1);
	atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, bones, 0));
	before = metal::getStateStats().skinnedUnrouted;
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	unrouted = metal::getStateStats().skinnedUnrouted - before;
	ok &= Expect(unrouted == 0, "%u unrouted skinned renders on the skin pipeline, expected 0", unrouted);
	const PixelWant wantSkin[] = { { 256, 176, RED, 0 }, { 256, 240, GREY, 0 } };
	ok &= Pixels(wantSkin, nelem(wantSkin), "skin pipeline");
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckBoneCountMismatch(void)
{
	const char *name = "hierarchies with more or fewer nodes than the skin, or over 64 bones, draw clamped and report once";
	Error err;
	getError(&err);
	const Marker marker = { 2.0f, 0.0f, 0.5f, 10.0f, RED, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	Matrix bones[3] = { Identity(), Translation(0.0f, -2.0f, 0.0f), Translation(9.0f, 9.0f, 0.0f) };
	Geometry *geo = Markers(Geometry::PRELIT, &marker, 1);
	Atomic *atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(3, bones, 0));
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	getError(&err);
	bool ok = Expect(err.code == ERR_GENERAL, "more nodes: error code %u, expected ERR_GENERAL", err.code);
	const PixelWant wantMore[] = { { 256, 304, RED, 0 }, { 256, 240, GREY, 0 } };
	ok &= Expect(Skin::get(geo)->numBones == 2, "the skin has %d bones, expected 2", Skin::get(geo)->numBones);
	ok &= Pixels(wantMore, nelem(wantMore), "more nodes than bones");
	DestroySkinAtomic(atomic);
	geo->destroy();

	const Marker fewer = { 2.0f, 0.0f, 0.5f, 10.0f, RED, { 1, 2, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	Matrix fewerBones[2] = { Identity(), Translation(0.0f, 2.0f, 0.0f) };
	geo = Markers(Geometry::PRELIT, &fewer, 1);
	atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(2, fewerBones, 0));
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	getError(&err);
	ok &= Expect(err.code == 0, "fewer nodes: reported again with code %u", err.code);
	const PixelWant wantFewer[] = { { 256, 176, RED, 0 }, { 256, 240, GREY, 0 } };
	ok &= Expect(Skin::get(geo)->numBones == 3, "the skin has %d bones, expected 3", Skin::get(geo)->numBones);
	ok &= Pixels(wantFewer, nelem(wantFewer), "fewer nodes than bones");
	DestroySkinAtomic(atomic);
	geo->destroy();

	const Marker many[2] = {
		{ 2.0f, 0.0f, 0.5f, 10.0f, RED, { 63, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ -2.0f, 0.0f, 0.5f, 10.0f, GREEN, { 69, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } } };
	Matrix manyBones[70];
	for(int i = 0; i < 70; i++)
		manyBones[i] = Translation(0.0f, 2.0f, 0.0f);
	manyBones[63] = Translation(-1.0f, -2.0f, 0.0f);
	geo = Markers(Geometry::PRELIT, many, 2);
	atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(70, manyBones, 0));
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	getError(&err);
	ok &= Expect(err.code == 0, "70 bones: reported again with code %u", err.code);
	const PixelWant wantMany[] = { { 288, 304, RED, 0 }, { 256, 240, GREY, 0 }, { 416, 304, GREEN, 0 }, { 384, 240, GREY, 0 } };
	ok &= Expect(Skin::get(geo)->numBones == 70, "the skin has %d bones, expected 70", Skin::get(geo)->numBones);
	ok &= Pixels(wantMany, nelem(wantMany), "70 bones");
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static void
ReadLTMs(Frame **f, Matrix *out, int n, const Matrix *invRoot)
{
	for(int i = 0; i < n; i++)
		if(invRoot)
			Matrix::mult(&out[i], f[i+1]->getLTM(), invRoot);
		else
			out[i] = *f[i+1]->getLTM();
}

static bool
CheckPedLikeHierarchy(void)
{
	const char *name = "a ped-like hierarchy below the atomic frame skins in bind pose and posed, in world and local space";
	const V3d zAxis = { 0.0f, 0.0f, 1.0f };
	const Marker m[2] = {
		{ 1.0f, 1.0f, 0.25f, 10.0f, RED, { 2, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ 0.0f, -1.0f, 0.25f, 10.0f, GREEN, { 3, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } } };
	Geometry *geo = Markers(Geometry::PRELIT, m, 2);
	Atomic *atomic = SkinAtomic(geo);
	Frame *f[5];
	f[0] = atomic->getFrame();
	V3d atomicPos = { 1.0f, 0.0f, 0.0f };
	f[0]->translate(&atomicPos, COMBINEREPLACE);
	static const int parent[5] = { -1, 0, 1, 2, 1 };
	static const V3d local[5] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, -1, 0 } };
	for(int i = 1; i < 5; i++){
		f[i] = Frame::create();
		f[i]->translate(&local[i], COMBINEREPLACE);
		f[parent[i]]->addChild(f[i], 1);
	}
	Matrix invRoot, ltms[4];
	Matrix::invert(&invRoot, f[0]->getLTM());
	ReadLTMs(f, ltms, 4, &invRoot);
	for(int i = 0; i < 4; i++){
		Matrix inv;
		Matrix::invert(&inv, &ltms[i]);
		SetInvBind(geo, i, inv);
	}
	HAnimHierarchy *hier = HAnimHierarchy::create(4, nil, nil, 0, 36);
	Skin::setHierarchy(atomic, hier);
	ReadLTMs(f, hier->matrices, 4, nil);
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	const PixelWant wantBind[] = { { 256, 208, RED, 0 }, { 288, 272, GREEN, 0 }, { 288, 176, GREY, 0 } };
	bool ok = Pixels(wantBind, nelem(wantBind), "bind pose");

	Matrix pose = RotationThenTranslation(zAxis, 90.0f, 0.0f, 1.0f, 0.0f);
	f[2]->transform(&pose, COMBINEREPLACE);
	ReadLTMs(f, hier->matrices, 4, nil);
	const PixelWant wantPose[] = { { 288, 176, RED, 0 }, { 256, 208, GREY, 0 }, { 288, 272, GREEN, 0 } };
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	ok &= Pixels(wantPose, nelem(wantPose), "posed, world-space matrices");

	hier->flags = HAnimHierarchy::LOCALSPACEMATRICES;
	Matrix::invert(&invRoot, f[0]->getLTM());
	ReadLTMs(f, hier->matrices, 4, &invRoot);
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	ok &= Pixels(wantPose, nelem(wantPose), "posed, local-space matrices");

	for(int i = 4; i > 0; i--)
		f[i]->destroy();
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinVariantPerDraw(void)
{
	const char *name = "a skinned draw picks the skin shader and its light and alpha test variant per draw";
	const Marker marker = { 2.0f, 0.0f, 0.5f, 10.0f, WHITE, { 0, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	const uint8 opaque = 255, half = 128;
	Geometry *lit = Markers(Geometry::NORMALS | Geometry::LIGHT, &marker, 1, &opaque);
	Geometry *translucent = Markers(Geometry::NORMALS | Geometry::LIGHT, &marker, 1, &half);
	Geometry *unlit = Markers(Geometry::NORMALS, &marker, 1);
	Atomic *atomics[3] = { SkinAtomic(lit), SkinAtomic(translucent), SkinAtomic(unlit) };
	static const uint32 want[3] = { metal::VARIANT_DIRECTIONALS, metal::VARIANT_DIRECTIONALS | metal::VARIANT_ALPHATEST, 0 };
	static const char *what[3] = { "lit", "lit with material alpha 128", "without LIGHT" };
	Light *amb = MakeLight(Light::AMBIENT, 0.3f, 0.3f, 0.3f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	bool ok = true;
	WorldBegin(GREY);
	for(int i = 0; i < 3; i++){
		atomics[i]->render();
		ok &= Expect(metal::currentShader == metal::skinShader && metal::currentVariant == want[i],
		             "%s: shader %p variant %u, expected the skin shader %p and variant %u", what[i],
		             (void*)metal::currentShader, metal::currentVariant, (void*)metal::skinShader, want[i]);
	}
	RenderSquare(0.5f, 10.0f, RED);
	ok &= Expect(metal::currentShader == metal::defaultShader, "a following default draw used shader %p, expected %p",
	             (void*)metal::currentShader, (void*)metal::defaultShader);
	WorldEnd();
	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	for(int i = 0; i < 3; i++)
		DestroySkinAtomic(atomics[i]);
	lit->destroy();
	translucent->destroy();
	unlit->destroy();
	return Report(ok, name);
}

static bool
CheckBoneBlockOncePerAtomic(void)
{
	const char *name = "the skin block is uploaded once per atomic and pose, and bound for every pass";
	const Marker m[2] = {
		{ 2.0f, 0.0f, 0.5f, 10.0f, RED, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
		{ -2.0f, 0.0f, 0.5f, 10.0f, GREEN, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } } };
	const uint8 alphas[2] = { 255, 128 };
	Geometry *geo = Markers(Geometry::PRELIT, m, 2, alphas);
	Matrix poseA[2] = { Identity(), Translation(0.0f, 1.0f, 0.0f) };
	Matrix poseB[2] = { Identity(), Translation(0.0f, -1.0f, 0.0f) };
	Atomic *first = SkinAtomic(geo);
	Atomic *second = SkinAtomic(geo);
	Skin::setHierarchy(first, MakeHierarchy(2, poseA, 0));
	Skin::setHierarchy(second, MakeHierarchy(2, poseB, 0));
	const int32 skin = metal::BUFFER_SKIN;
	metal::StateStats s0 = metal::getStateStats();
	WorldBegin(GREY);
	SetRenderState(GSALPHATEST, 1);
	first->render();
	metal::StateStats s1 = metal::getStateStats();
	first->render();
	metal::StateStats s2 = metal::getStateStats();
	second->render();
	metal::StateStats s3 = metal::getStateStats();
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	WorldBegin(GREY);
	SetRenderState(GSALPHATEST, 1);
	first->render();
	SetRenderState(GSALPHATEST, 0);
	metal::StateStats s4 = metal::getStateStats();
	WorldEnd();
	bool ok = Expect(s1.blockUploads[skin] - s0.blockUploads[skin] == 1 && s1.vertexBlockBinds[skin] - s0.vertexBlockBinds[skin] == 3,
	                 "one render: %u uploads and %u binds, expected 1 and 3", s1.blockUploads[skin] - s0.blockUploads[skin],
	                 s1.vertexBlockBinds[skin] - s0.vertexBlockBinds[skin]);
	ok &= Expect(s2.blockUploads[skin] - s0.blockUploads[skin] == 1 && s2.vertexBlockBinds[skin] - s0.vertexBlockBinds[skin] == 6,
	             "the same atomic again: %u uploads and %u binds in total, expected 1 and 6",
	             s2.blockUploads[skin] - s0.blockUploads[skin], s2.vertexBlockBinds[skin] - s0.vertexBlockBinds[skin]);
	ok &= Expect(s3.blockUploads[skin] - s0.blockUploads[skin] == 2,
	             "a second atomic with another pose: %u uploads in total, expected 2", s3.blockUploads[skin] - s0.blockUploads[skin]);
	ok &= Expect(s4.blockUploads[skin] - s3.blockUploads[skin] == 1,
	             "the next frame: %u uploads, expected 1", s4.blockUploads[skin] - s3.blockUploads[skin]);
	DestroySkinAtomic(first);
	DestroySkinAtomic(second);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckSkinnedWithAttachment(void)
{
	const char *name = "a skinned atomic, an unskinned one and a weapon on a bone's matrix draw together in one frame";
	const Marker marker = { 0.0f, 0.0f, 0.5f, 10.0f, RED, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	Geometry *plain = SolidQuad(2.5f, -0.5f, 1.5f, 0.5f, 10.0f, GREEN);
	Geometry *weapon = SolidQuad(0.25f, 0.75f, -0.25f, 1.25f, 10.0f, BLUE);
	Geometry *geo = Markers(Geometry::PRELIT, &marker, 1);
	Matrix bones[2] = { Identity(), Translation(-2.0f, 0.0f, 0.0f) };
	Atomic *ped = SkinAtomic(geo);
	HAnimHierarchy *hier = MakeHierarchy(2, bones, 0);
	Skin::setHierarchy(ped, hier);
	Atomic *plainAtomic = MakeAtomic(plain);
	Atomic *weaponAtomic = MakeAtomic(weapon);
	weaponAtomic->getFrame()->transform(&hier->matrices[1], COMBINEREPLACE);
	uint32 binds = metal::getStateStats().vertexBlockBinds[metal::BUFFER_SKIN];
	WorldBegin(GREY);
	plainAtomic->render();
	ped->render();
	weaponAtomic->render();
	metal::Shader *last = metal::currentShader;
	WorldEnd();
	binds = metal::getStateStats().vertexBlockBinds[metal::BUFFER_SKIN] - binds;
	const PixelWant want[] = { { 256, 240, GREEN, 0 }, { 384, 240, RED, 0 }, { 384, 208, BLUE, 0 } };
	bool ok = Pixels(want, nelem(want), "attachment");
	ok &= Expect(binds == 1, "%u skin block binds, expected 1", binds);
	ok &= Expect(last == metal::defaultShader, "the last draw used shader %p, expected the default shader %p",
	             (void*)last, (void*)metal::defaultShader);
	DestroyAtomic(weaponAtomic);
	DestroyAtomic(plainAtomic);
	DestroySkinAtomic(ped);
	geo->destroy();
	weapon->destroy();
	plain->destroy();
	return Report(ok, name);
}

static bool
CheckSkinnedPS2AlphaTest(void)
{
	const char *name = "GSALPHATEST on a skinned mesh draws texels below its reference without depth, and restores the states";
	Geometry *geo = CutoutQuad(64);
	if(geo == nil)
		return Report(false, name);
	static const uint8 idx[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
	static const float w[4][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };
	AttachSkin(geo, 1, nil, idx, w);
	Matrix bone = Translation(0.0f, 1.0f, 0.0f);
	Atomic *atomic = SkinAtomic(geo);
	Skin::setHierarchy(atomic, MakeHierarchy(1, &bone, 0));
	WorldBegin(GREY);
	RenderSquare(10.0f, 20.0f, BLUE);
	SetRenderState(GSALPHATEST, 1);
	uint32 func = GetRenderState(ALPHATESTFUNC), ref = GetRenderState(ALPHATESTREF);
	atomic->render();
	bool ok = Expect(GetRenderState(ALPHATESTFUNC) == func && GetRenderState(ALPHATESTREF) == ref &&
	                 GetRenderState(ZWRITEENABLE) == 1 && GetRenderState(GSALPHATEST) == 1 &&
	                 GetRenderState(GSALPHATESTREF) == 128,
	                 "state after the draw is func %u ref %u zwrite %u, expected %u %u 1",
	                 GetRenderState(ALPHATESTFUNC), GetRenderState(ALPHATESTREF), GetRenderState(ZWRITEENABLE), func, ref);
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	const PixelWant want[] = { { 256, 160, Rgba(64, 0, 191, 207), 2 }, { 384, 160, RED, 0 }, { 256, 288, BLUE, 0 } };
	ok &= Pixels(want, nelem(want), "skinned PS2 alpha test");
	ok &= DepthNear(256, 160, DepthOf(20.0f), 1e-5f);
	ok &= DepthNear(384, 160, DepthOf(10.0f), 1e-5f);
	DestroySkinAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static void
DrawSkinnedPatternMarkers(uint32 flags)
{
	const Marker marker = { 0.0f, 0.0f, 1.0f, 10.0f, WHITE, { 0, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
	const uint8 alphas[2] = { 255, 128 };
	for(int i = 0; i < 2; i++){
		Geometry *geo = Markers(flags, &marker, 1, &alphas[i]);
		Atomic *atomic = SkinAtomic(geo);
		atomic->render();
		DestroySkinAtomic(atomic);
		geo->destroy();
	}
}

static bool
CheckSkinShaderBlocks(void)
{
	const char *name = "the skin shader reads scene, object, material, state, skin and lights blocks at their C sizes";
	uint32 mismatches = metal::getStateStats().blockSizeMismatches;
	uint32 used = metal::checkShaderBlockSizes(metal::skinShader, metal::VARIANT_ALL);
	bool ok = Expect(used == 0xBE, "blocks 0x%x, expected 0xBE", used);
	ok &= Expect(metal::getStateStats().blockSizeMismatches == mismatches, "%u block size mismatches",
	             metal::getStateStats().blockSizeMismatches - mismatches);
	return Report(ok, name);
}

int
RunSkinChecks(Camera *camera)
{
	failures = 0;
	WorldOpen(camera);
	CheckSkinInstanceLayout();
	CheckSkinReinstanceKeepsWeights();
	CheckSkinPipelineWithoutSkin();
	CheckSkinLayoutPositionsOnly();
	CheckSkinNilHierarchy();
	CheckBoneTransformAndOrder();
	CheckWeightsBlend();
	CheckSkinnedNormalsBlend();
	CheckSkinnedFog();
	CheckSkinPipelineWithoutSkinDraws();
	CheckSkinnedOnDefaultPipelineUnrouted();
	CheckBoneCountMismatch();
	CheckPedLikeHierarchy();
	CheckSkinVariantPerDraw();
	CheckBoneBlockOncePerAtomic();
	CheckSkinnedWithAttachment();
	CheckSkinnedPS2AlphaTest();
	CheckSkinShaderBlocks();
	WorldClose();
	return failures;
}

static Frame*
RotatedFrame(V3d axis, float angle)
{
	Frame *frame = Frame::create();
	frame->rotate(&axis, angle, COMBINEREPLACE);
	frame->updateObjects();
	return frame;
}

static Matrix
RollCamera(Camera *camera, float angle)
{
	const V3d zAxis = { 0.0f, 0.0f, 1.0f };
	Frame *frame = camera->getFrame();
	Matrix prev = frame->matrix;
	frame->rotate(&zAxis, angle, COMBINEPRECONCAT);
	frame->updateObjects();
	return prev;
}

static void
RestoreCamera(Camera *camera, const Matrix &m)
{
	Frame *frame = camera->getFrame();
	frame->matrix = m;
	frame->updateObjects();
}

static void
EnvUV(const RawMatrix &m, V3d n, float *u, float *v)
{
	*u = m.right.x*n.x + m.up.x*n.y + m.at.x*n.z + m.pos.x;
	*v = m.right.y*n.x + m.up.y*n.y + m.at.y*n.z + m.pos.y;
}

static bool
CheckMatFXShaderBlocks(void)
{
	const char *name = "the matfx env shader has id 5 and reads scene, object, material, state, lights and matfx blocks at their C sizes";
	bool ok = Expect(metal::matfxEnvShader != nil, "no matfx env shader");
	if(!ok)
		return Report(ok, name);
	ok &= Expect(metal::matfxEnvShader->shaderId == 5, "shader id %u, expected 5", metal::matfxEnvShader->shaderId);
	uint32 mismatches = metal::getStateStats().blockSizeMismatches;
	uint32 used = metal::checkShaderBlockSizes(metal::matfxEnvShader, metal::VARIANT_ALL);
	ok &= Expect(used == 0x19E, "blocks 0x%x, expected 0x19E", used);
	ok &= Expect(metal::getStateStats().blockSizeMismatches == mismatches, "%u block size mismatches",
	             metal::getStateStats().blockSizeMismatches - mismatches);
	return Report(ok, name);
}

static bool
ExpectEnvUV(Frame *frame, bool32 flipU, V3d n, float u, float v, const char *label, RawMatrix *out)
{
	float gu, gv;
	RawMatrix m;
	MatFX::envMapFlipU = flipU;
	metal::matfxEnvMatrix(frame, &m);
	EnvUV(m, n, &gu, &gv);
	if(out)
		*out = m;
	return Expect(fabsf(gu - u) < 1e-5f && fabsf(gv - v) < 1e-5f, "%s: (%f, %f), expected (%f, %f)",
	              label, gu, gv, u, v);
}

static bool
CheckEnvMatrix(Camera *camera)
{
	const char *name = "the env matrix maps normals to env uvs through the inverse frame rotation, flipping u on request";
	EnvGlobals g = SetEnvGlobals(0, 0, 0, YELLOW);
	const V3d zAxis = { 0.0f, 0.0f, 1.0f };
	const V3d xAxis = { 1.0f, 0.0f, 0.0f };
	const V3d n = { 0.5f, 0.5f, -0.7071f };
	const V3d up = { 0.0f, 0.0f, 1.0f };
	const V3d side = { 1.0f, 0.0f, 0.0f };
	const V3d shift = { 100.0f, 0.0f, 0.0f };
	RawMatrix m;
	bool ok = true;

	Frame *identity = Frame::create();
	identity->updateObjects();
	ok &= ExpectEnvUV(identity, 0, n, 0.75f, 0.25f, "identity", nil);
	ok &= ExpectEnvUV(identity, 1, n, 0.25f, 0.25f, "identity, flipU", nil);
	identity->destroy();

	Frame *rz = RotatedFrame(zAxis, 90.0f);
	ok &= ExpectEnvUV(rz, 0, n, 0.75f, 0.75f, "R_z(90)", nil);
	rz->translate(&shift, COMBINEPOSTCONCAT);
	rz->updateObjects();
	ok &= ExpectEnvUV(rz, 0, n, 0.75f, 0.75f, "R_z(90) then T(100, 0, 0)", &m);
	ok &= Expect(m.pos.x == 0.5f && m.pos.y == 0.5f, "translated pos (%f, %f), expected (0.5, 0.5)", m.pos.x, m.pos.y);
	rz->destroy();

	Frame *rx = RotatedFrame(xAxis, 60.0f);
	ok &= ExpectEnvUV(rx, 1, up, 0.5f, 0.5f - 0.25f*sqrtf(3.0f), "R_x(60), flipU, +Z", nil);
	ok &= ExpectEnvUV(rx, 1, side, 0.0f, 0.5f, "R_x(60), flipU, +X", nil);
	rx->destroy();

	ok &= Expect(engine->currentCamera == camera, "current camera is not the test camera");
	Matrix cam = RollCamera(camera, 90.0f);
	ok &= ExpectEnvUV(nil, 0, n, 0.75f, 0.75f, "nil frame (camera rolled R_z(90))", nil);
	RestoreCamera(camera, cam);

	RestoreEnvGlobals(g);
	return Report(ok, name);
}

static Material*
EnvMaterial(Texture *base, Texture *env, float coef, bool32 fbAlpha, Frame *frame, RGBA colour)
{
	Material *mat = Material::create();
	mat->color = colour;
	mat->setTexture(base);
	MatFX::setEffects(mat, MatFX::ENVMAP);
	MatFX *fx = MatFX::get(mat);
	fx->setEnvTexture(env);
	fx->setEnvCoefficient(coef);
	fx->setEnvFBAlpha(fbAlpha);
	fx->setEnvFrame(frame);
	return mat;
}

static Geometry*
EnvQuad(uint32 flags, V3d normal, Material *mat, float half, float z)
{
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Geometry *geo = QuadGeometry(flags | Geometry::POSITIONS | Geometry::TEXTURED, half, -half, -half, half, z, white, uv, mat);
	if(geo && flags & Geometry::NORMALS)
		for(int i = 0; i < 4; i++)
			geo->morphTargets[0].normals[i] = normal;
	return geo;
}

static Atomic*
MatFXAtomic(Geometry *geo)
{
	Atomic *atomic = MakeAtomic(geo);
	MatFX::enableEffects(atomic);
	return atomic;
}

static const V3d envFacing = { 0.0f, 0.0f, -1.0f };

static void
DrawEnvPatternQuad(uint32 flags, Texture *env, uint8 matAlpha)
{
	RGBA colour = WHITE;
	colour.alpha = matAlpha;
	Material *mat = env ? EnvMaterial(nil, env, 1.0f, 0, nil, colour) : Material::create();
	mat->color = colour;
	Geometry *geo = EnvQuad(flags, envFacing, mat, 1.0f, 10.0f);
	mat->destroy();
	Atomic *atomic = MatFXAtomic(geo);
	atomic->render();
	DestroyAtomic(atomic);
	geo->destroy();
}

static bool
PixelCase(RGBA want, int tol, const char *what)
{
	Image *img = ReadCamera();
	if(img == nil)
		return false;
	bool ok = Near(img, 320, 240, want, tol);
	img->destroy();
	if(!ok)
		Detail("  in case %s\n", what);
	return ok;
}

static bool
CheckEnvCoordinates(Camera *camera)
{
	const char *name = "the env pass samples the env texture at the world normal through the inverse env frame, or the camera's";
	static const RGBA texels[4] = { RED, GREEN, BLUE, YELLOW };
	Texture *env = MakeTexture(2, 2, texels, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *base = Tex1(BLACK);
	if(env == nil || base == nil)
		return Report(false, name);
	const V3d zAxis = { 0.0f, 0.0f, 1.0f };
	const V3d n = { 0.5f, 0.5f, -0.7071f };
	const V3d nObject = { 0.5f, -0.5f, -0.7071f };
	const V3d shift = { 100.0f, 0.0f, 0.0f };
	Frame *identity = Frame::create();
	identity->updateObjects();
	Frame *rz = RotatedFrame(zAxis, 90.0f);
	Frame *rzt = RotatedFrame(zAxis, 90.0f);
	rzt->translate(&shift, COMBINEPOSTCONCAT);
	rzt->updateObjects();
	struct Case { Frame *env; bool atomicRolled, cameraRolled; V3d normal; bool32 flipU; RGBA want; const char *what; };
	const Case cases[] = {
		{ identity, false, false, n, 0, GREEN, "a: identity frames" },
		{ identity, false, false, n, 1, RED, "b: identity frames, flipU" },
		{ rz, false, false, n, 0, YELLOW, "c: env frame R_z(90)" },
		{ rzt, false, false, n, 0, YELLOW, "d: env frame R_z(90) then T(100, 0, 0)" },
		{ identity, true, false, nObject, 0, GREEN, "e: atomic frame R_z(90)" },
		{ nil, false, true, n, 0, YELLOW, "f: nil env frame, camera rolled R_z(90)" },
	};
	EnvGlobals g = SetEnvGlobals(0, 0, 1, WHITE);
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		MatFX::envMapFlipU = c.flipU;
		Material *mat = EnvMaterial(base, env, 1.0f, 0, c.env, WHITE);
		Geometry *geo = EnvQuad(Geometry::NORMALS | Geometry::PRELIT | Geometry::MODULATE, c.normal, mat, 1.0f, 10.0f);
		mat->destroy();
		Atomic *atomic = MatFXAtomic(geo);
		if(c.atomicRolled){
			atomic->getFrame()->rotate(&zAxis, 90.0f);
			atomic->getFrame()->updateObjects();
		}
		Matrix cam;
		if(c.cameraRolled)
			cam = RollCamera(camera, 90.0f);
		WorldBegin(GREY);
		atomic->render();
		WorldEnd();
		if(c.cameraRolled)
			RestoreCamera(camera, cam);
		DestroyAtomic(atomic);
		geo->destroy();
		ok &= PixelCase(c.want, 1, c.what);
	}
	RestoreEnvGlobals(g);
	identity->destroy();
	rz->destroy();
	rzt->destroy();
	env->destroy();
	base->destroy();
	return Report(ok, name);
}

static bool
CheckEnvColour(void)
{
	const char *name = "the env colour follows applyLight, useMatColor, envMapColor and the coefficient; no coefficient or env texture draws the base";
	static const RGBA grey64 = { 64, 64, 64, 255 };
	static const RGBA orange = { 255, 128, 0, 255 };
	struct Case { bool32 applyLight, useMatColor; RGBA envColour, material; float coef; bool envTex, base; RGBA want; const char *what; };
	const Case cases[] = {
		{ 1, 1, WHITE, WHITE, 0.5f, true, false, Rgb(77, 153, 230), "applyLight, useMatColor (the game's settings)" },
		{ 0, 1, WHITE, WHITE, 0.5f, true, false, Rgb(179, 230, 255), "useMatColor" },
		{ 0, 0, grey64, WHITE, 0.5f, true, false, Rgb(83, 134, 185), "envMapColor (64, 64, 64)" },
		{ 1, 1, WHITE, orange, 1.0f, true, false, Rgb(102, 102, 0), "applyLight, useMatColor, orange material" },
		{ 1, 1, WHITE, WHITE, 0.0f, true, true, Rgb(51, 102, 153), "coefficient 0" },
		{ 1, 1, WHITE, WHITE, 1.0f, false, true, Rgb(51, 102, 153), "no env texture" },
	};
	Texture *white = Tex1(WHITE);
	if(white == nil)
		return Report(false, name);
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.4f, 0.6f);
	saved.world->addLight(amb);
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		EnvGlobals g = SetEnvGlobals(0, c.applyLight, c.useMatColor, c.envColour);
		Material *mat = EnvMaterial(white, c.envTex ? white : nil, c.coef, 0, nil, c.material);
		Geometry *geo = EnvQuad(Geometry::NORMALS | Geometry::LIGHT | Geometry::MODULATE, envFacing, mat, 1.0f, 10.0f);
		mat->destroy();
		Atomic *atomic = MatFXAtomic(geo);
		WorldBegin(GREY);
		atomic->render();
		metal::Shader *shader = metal::currentShader;
		WorldEnd();
		RestoreEnvGlobals(g);
		DestroyAtomic(atomic);
		geo->destroy();
		bool caseOk = PixelCase(c.want, 2, c.what);
		metal::Shader *wantShader = c.base ? metal::defaultShader : metal::matfxEnvShader;
		if(shader != wantShader){
			Detail("  in case %s the %s shader drew, expected the %s shader\n", c.what,
			       shader == metal::defaultShader ? "default" : shader == metal::matfxEnvShader ? "env" : "other",
			       c.base ? "default" : "env");
			caseOk = false;
		}
		ok &= caseOk;
	}
	saved.world->removeLight(amb);
	DestroyLight(amb);
	ClearLights();
	white->destroy();
	return Report(ok, name);
}

static bool
CheckEnvFrameBufferAlpha(void)
{
	const char *name = "the env pass blends ONE/INVSRCALPHA, scales by alpha only with fbAlpha, and leaves SRCBLEND at SRCALPHA";
	Texture *white = Tex1(WHITE), *blue = Tex1(BLUE);
	if(white == nil || blue == nil)
		return Report(false, name);
	EnvGlobals g = SetEnvGlobals(0, 0, 0, WHITE);
	bool ok = true;
	for(int fba = 0; fba < 2; fba++){
		Material *mat = EnvMaterial(white, blue, 1.0f, fba, nil, Rgba(255, 0, 0, 128));
		Geometry *geo = EnvQuad(Geometry::PRELIT | Geometry::MODULATE, envFacing, mat, 1.0f, 10.0f);
		mat->destroy();
		Atomic *atomic = MatFXAtomic(geo);
		WorldBegin(BLACK);
		SetRenderState(SRCBLEND, BLENDONE);
		atomic->render();
		uint32 src = GetRenderState(SRCBLEND), va = GetRenderState(VERTEXALPHA);
		WorldEnd();
		DestroyAtomic(atomic);
		geo->destroy();
		const char *what = fba ? "fbAlpha 1" : "fbAlpha 0";
		ok &= PixelCase(fba ? Rgba(128, 0, 128, 255) : Rgba(128, 0, 255, 255), 2, what);
		ok &= Expect(src == BLENDSRCALPHA && va == 1, "%s: SRCBLEND %u and VERTEXALPHA %u after the draw, expected %u and 1",
		             what, src, va, BLENDSRCALPHA);
	}
	RestoreEnvGlobals(g);
	white->destroy();
	blue->destroy();
	return Report(ok, name);
}

static bool
CheckEnvFog(void)
{
	const char *name = "fog fades the env pass towards black";
	Texture *black = Tex1(BLACK), *white = Tex1(WHITE);
	if(black == nil || white == nil)
		return Report(false, name);
	uint32 oldColour = GetRenderState(FOGCOLOR);
	float32 oldPlane = saved.camera->fogPlane;
	saved.camera->fogPlane = 11.0f;
	EnvGlobals g = SetEnvGlobals(0, 0, 1, WHITE);
	Material *mat = EnvMaterial(black, white, 1.0f, 0, nil, WHITE);
	Geometry *geo = EnvQuad(Geometry::NORMALS | Geometry::PRELIT, envFacing, mat, 11.2f, 56.0f);
	mat->destroy();
	Atomic *atomic = MatFXAtomic(geo);
	bool ok = true;
	for(int fog = 1; fog >= 0; fog--){
		WorldBegin(GREY);
		SetRenderState(FOGCOLOR, RWRGBAINT(0, 0, 0, 255));
		SetRenderState(FOGENABLE, fog);
		atomic->render();
		SetRenderState(FOGENABLE, 0);
		WorldEnd();
		ok &= PixelCase(fog ? Rgb(128, 128, 128) : WHITE, fog ? 1 : 0, fog ? "fog on" : "fog off");
	}
	DestroyAtomic(atomic);
	geo->destroy();
	RestoreEnvGlobals(g);
	saved.camera->fogPlane = oldPlane;
	SetRenderState(FOGCOLOR, oldColour);
	black->destroy();
	white->destroy();
	return Report(ok, name);
}

static bool
CheckOtherEffectsDrawBase(void)
{
	const char *name = "materials without an env map effect draw their base pass on the matfx pipeline, as on the default pipeline";
	Texture *red = Tex1(RED), *white = Tex1(WHITE), *green = Tex1(GREEN);
	if(red == nil || white == nil || green == nil)
		return Report(false, name);
	Matrix uvBase = Translation(0.5f, 0.0f, 0.0f);
	struct Case { int32 type; const char *what; };
	const Case cases[] = {
		{ -1, "no MatFX" },
		{ MatFX::NOTHING, "NOTHING" },
		{ MatFX::BUMPMAP, "BUMPMAP" },
		{ MatFX::DUAL, "DUAL" },
		{ MatFX::UVTRANSFORM, "UVTRANSFORM" },
		{ MatFX::DUALUVTRANSFORM, "DUALUVTRANSFORM" },
		{ MatFX::BUMPENVMAP, "BUMPENVMAP" },
	};
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		Material *mat = Material::create();
		mat->setTexture(red);
		if(c.type >= 0){
			MatFX::setEffects(mat, c.type);
			MatFX *fx = MatFX::get(mat);
			if(c.type == MatFX::BUMPMAP || c.type == MatFX::BUMPENVMAP){
				fx->setBumpTexture(white);
				fx->setBumpCoefficient(1.0f);
				// streamed bump maps carry a bumped texture, which overlaps Env::tex in the union
				fx->fx[0].bump.bumpedTex = white;
				white->addRef();
			}
			if(c.type == MatFX::DUAL || c.type == MatFX::DUALUVTRANSFORM){
				fx->setDualTexture(green);
				fx->setDualSrcBlend(BLENDONE);
				fx->setDualDestBlend(BLENDONE);
			}
			if(c.type == MatFX::UVTRANSFORM || c.type == MatFX::DUALUVTRANSFORM)
				fx->setUVTransformMatrices(&uvBase, nil);
			if(c.type == MatFX::BUMPENVMAP){
				fx->setEnvTexture(green);
				fx->setEnvCoefficient(1.0f);
			}
		}
		Geometry *geo = EnvQuad(Geometry::PRELIT, envFacing, mat, 1.0f, 10.0f);
		mat->destroy();
		RGBA got[2];
		bool caseOk = true;
		for(int fx = 0; fx < 2; fx++){
			Atomic *atomic = fx ? MatFXAtomic(geo) : MakeAtomic(geo);
			WorldBegin(GREY);
			atomic->render();
			metal::Shader *shader = metal::currentShader;
			WorldEnd();
			DestroyAtomic(atomic);
			caseOk &= Expect(shader == metal::defaultShader, "the %s pipeline drew with the %s shader",
			                 fx ? "matfx" : "default", shader == metal::matfxEnvShader ? "env" : "wrong");
			Image *img = ReadCamera();
			if(img == nil){
				caseOk = false;
				break;
			}
			uint8 *p = &img->pixels[240*img->stride + 320*4];
			got[fx] = Rgba(p[0], p[1], p[2], p[3]);
			caseOk &= Near(img, 320, 240, RED, 0);
			img->destroy();
		}
		if(caseOk)
			caseOk &= Expect(memcmp(&got[0], &got[1], sizeof(RGBA)) == 0, "the pipelines drew different pixels");
		if(!caseOk)
			Detail("  in case %s\n", c.what);
		ok &= caseOk;
		geo->destroy();
	}
	red->destroy();
	white->destroy();
	green->destroy();
	return Report(ok, name);
}

static bool
CheckEnvPerMesh(void)
{
	const char *name = "each mesh of a matfx atomic draws its own env texture, and a mesh without effects draws its base";
	Texture *black = Tex1(BLACK), *green = Tex1(GREEN), *blue = Tex1(BLUE);
	if(black == nil || green == nil || blue == nil)
		return Report(false, name);
	EnvGlobals g = SetEnvGlobals(0, 0, 1, WHITE);
	Material *top = EnvMaterial(black, green, 1.0f, 0, nil, WHITE);
	Material *bottom = EnvMaterial(black, blue, 1.0f, 0, nil, WHITE);
	Material *plain = Material::create();
	plain->setTexture(black);
	Geometry *geos[2] = { TwoBands(top, bottom, 1.0f), TwoBands(top, plain, 1.0f) };
	top->destroy();
	bottom->destroy();
	plain->destroy();
	const RGBA wantBottom[2] = { BLUE, BLACK };
	bool ok = true;
	for(int i = 0; i < 2; i++){
		Atomic *atomic = MatFXAtomic(geos[i]);
		Image *img = RenderOne(atomic);
		DestroyAtomic(atomic);
		if(img == nil){
			ok = false;
			geos[i]->destroy();
			continue;
		}
		bool caseOk = Near(img, 320, 176, GREEN, 1);
		caseOk &= Near(img, 320, 304, wantBottom[i], 1);
		img->destroy();
		if(!caseOk)
			Detail("  with the bottom material %s\n", i ? "without effects" : "on env BLUE");
		ok &= caseOk;
		geos[i]->destroy();
	}
	RestoreEnvGlobals(g);
	black->destroy();
	green->destroy();
	blue->destroy();
	return Report(ok, name);
}

static bool
CheckEnvVariantPerDraw(void)
{
	const char *name = "the env pass draws with the env shader and the light variant of its atomic, and the next default draw switches back";
	Texture *white = Tex1(WHITE);
	if(white == nil)
		return Report(false, name);
	Light *amb = MakeLight(Light::AMBIENT, 0.2f, 0.2f, 0.2f);
	Light *dir = MakeLight(Light::DIRECTIONAL, 0.5f, 0.5f, 0.5f);
	saved.world->addLight(amb);
	saved.world->addLight(dir);
	EnvGlobals g = SetEnvGlobals(0, 1, 1, WHITE);
	bool ok = true;
	for(int lit = 1; lit >= 0; lit--){
		Material *mat = EnvMaterial(white, white, 1.0f, 0, nil, WHITE);
		uint32 flags = Geometry::NORMALS | Geometry::MODULATE | (lit ? Geometry::LIGHT : 0);
		Geometry *geo = EnvQuad(flags, envFacing, mat, 1.0f, 10.0f);
		mat->destroy();
		Atomic *atomic = MatFXAtomic(geo);
		WorldBegin(GREY);
		atomic->render();
		metal::Shader *shader = metal::currentShader;
		uint32 variant = metal::currentVariant;
		RenderSquare(0.5f, 20.0f, BLUE);
		metal::Shader *after = metal::currentShader;
		WorldEnd();
		DestroyAtomic(atomic);
		geo->destroy();
		uint32 want = lit ? metal::VARIANT_DIRECTIONALS | metal::VARIANT_ALPHATEST : metal::VARIANT_ALPHATEST;
		const char *what = lit ? "with LIGHT" : "without LIGHT";
		ok &= Expect(shader == metal::matfxEnvShader, "%s: the env mesh did not draw with the env shader", what);
		ok &= Expect(variant == want, "%s: variant %u, expected %u", what, variant, want);
		ok &= Expect(after == metal::defaultShader, "%s: the following default draw did not use the default shader", what);
	}
	RestoreEnvGlobals(g);
	saved.world->removeLight(amb);
	saved.world->removeLight(dir);
	DestroyLight(amb);
	DestroyLight(dir);
	ClearLights();
	white->destroy();
	return Report(ok, name);
}

static bool
CheckEnvPS2AlphaTest(void)
{
	const char *name = "GSALPHATEST draws the env pass in two passes, blending ONE/INVSRCALPHA, and restores the alpha test and depth write";
	Geometry *geo = CutoutQuad(64);
	Texture *black = Tex1(BLACK);
	if(geo == nil || black == nil)
		return Report(false, name);
	Material *mat = geo->matList.materials[0];
	MatFX::setEffects(mat, MatFX::ENVMAP);
	MatFX::get(mat)->setEnvTexture(black);
	MatFX::get(mat)->setEnvCoefficient(1.0f);
	black->destroy();
	EnvGlobals g = SetEnvGlobals(0, 0, 1, WHITE);
	Atomic *atomic = MatFXAtomic(geo);

	WorldBegin(GREY);
	RenderSquare(10.0f, 20.0f, BLUE);
	SetRenderState(GSALPHATEST, 1);
	uint32 func = GetRenderState(ALPHATESTFUNC), ref = GetRenderState(ALPHATESTREF);
	atomic->render();
	metal::Shader *shader = metal::currentShader;
	bool ok = Expect(GetRenderState(ALPHATESTFUNC) == func && GetRenderState(ALPHATESTREF) == ref &&
	                 GetRenderState(ZWRITEENABLE) == 1,
	                 "state after the draw is func %u ref %u zwrite %u, expected %u %u 1",
	                 GetRenderState(ALPHATESTFUNC), GetRenderState(ALPHATESTREF), GetRenderState(ZWRITEENABLE), func, ref);
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	ok &= Expect(shader == metal::matfxEnvShader, "the cutout did not draw with the env shader");
	Image *img = ReadCamera();
	if(img){
		ok &= Near(img, 256, 240, Rgba(64, 0, 191, 255), 2);
		ok &= Near(img, 384, 240, RED, 0);
		img->destroy();
	}else
		ok = false;
	ok &= DepthNear(256, 240, DepthOf(20.0f), 1e-5f);
	ok &= DepthNear(384, 240, DepthOf(10.0f), 1e-5f);
	RestoreEnvGlobals(g);
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static uint32
MatFXStat(const uint32 *after, const uint32 *before)
{
	return after[metal::BUFFER_MATFX] - before[metal::BUFFER_MATFX];
}

static bool
CheckEnvBlockUploads(void)
{
	const char *name = "the matfx block uploads only when an env mesh's constants change, and binds only for env draws";
	Texture *white = Tex1(WHITE);
	if(white == nil)
		return Report(false, name);
	EnvGlobals g = SetEnvGlobals(0, 0, 1, WHITE);
	Frame *frame = Frame::create();
	frame->updateObjects();
	const float coefs[3] = { 1.0f, 1.0f, 0.5f };
	Geometry *geos[3];
	Atomic *atomics[3];
	for(int i = 0; i < 3; i++){
		Material *mat = EnvMaterial(white, white, coefs[i], 0, frame, WHITE);
		geos[i] = EnvQuad(Geometry::PRELIT, envFacing, mat, 1.0f, 10.0f);
		mat->destroy();
		atomics[i] = MatFXAtomic(geos[i]);
	}
	WorldBegin(GREY);
	metal::StateStats s = metal::getStateStats();
	for(int i = 0; i < 3; i++)
		atomics[i]->render();
	metal::StateStats e = metal::getStateStats();
	RenderSquare(0.5f, 20.0f, BLUE);
	metal::StateStats d = metal::getStateStats();
	WorldEnd();
	WorldBegin(GREY);
	metal::StateStats s2 = metal::getStateStats();
	atomics[0]->render();
	metal::StateStats e2 = metal::getStateStats();
	WorldEnd();

	uint32 uploads = MatFXStat(e.blockUploads, s.blockUploads);
	uint32 vbinds = MatFXStat(e.vertexBlockBinds, s.vertexBlockBinds);
	uint32 fbinds = MatFXStat(e.fragmentBlockBinds, s.fragmentBlockBinds);
	bool ok = Expect(uploads == 2 && vbinds == 3 && fbinds == 3,
	                 "three env draws: %u uploads, %u vertex and %u fragment binds, expected 2, 3 and 3", uploads, vbinds, fbinds);
	vbinds = MatFXStat(d.vertexBlockBinds, e.vertexBlockBinds);
	fbinds = MatFXStat(d.fragmentBlockBinds, e.fragmentBlockBinds);
	ok &= Expect(vbinds == 0 && fbinds == 0, "a default draw bound the matfx block %u and %u times, expected 0", vbinds, fbinds);
	uploads = MatFXStat(e2.blockUploads, s2.blockUploads);
	ok &= Expect(uploads == 1, "the next frame: %u uploads, expected 1", uploads);

	for(int i = 0; i < 3; i++){
		DestroyAtomic(atomics[i]);
		geos[i]->destroy();
	}
	frame->destroy();
	RestoreEnvGlobals(g);
	white->destroy();
	return Report(ok, name);
}

static bool
CheckMatfxRingCost(void)
{
	const char *name = "twelve matfx atomics with two env materials each upload the matfx block 24 times within 2 KB of ring per atomic";
	static const RGBA grey64 = { 64, 64, 64, 255 };
	Texture *white = Tex1(WHITE);
	if(white == nil)
		return Report(false, name);
	EnvGlobals g = SetEnvGlobals(1, 1, 1, grey64);
	Material *top = EnvMaterial(white, white, 1.0f, 0, nil, RED);
	Material *bottom = EnvMaterial(white, white, 1.0f, 0, nil, GREEN);
	Geometry *geo = TwoBands(top, bottom, 1.0f);
	top->destroy();
	bottom->destroy();
	geo->flags |= Geometry::MODULATE;
	Atomic *atomics[12];
	for(int i = 0; i < 12; i++){
		atomics[i] = MatFXAtomic(geo);
		V3d pos = { 0.5f*i - 3.0f, 0.0f, 0.0f };
		atomics[i]->getFrame()->translate(&pos);
	}
	metal::RingSpace probe;
	WorldBegin(GREY);
	bool ok = RingProbe(&probe);
	metal::StateStats s = metal::getStateStats();
	for(int i = 0; i < 12; i++)
		atomics[i]->render();
	metal::StateStats e = metal::getStateStats();
	WorldEnd();
	uint32 uploads = MatFXStat(e.blockUploads, s.blockUploads);
	uint32 bytes = e.frameRingBytes - s.frameRingBytes;
	Detail("  %u ring bytes for 12 atomics, %u per vehicle atomic; %u matfx uploads\n", bytes, bytes/12, uploads);
	ok &= Expect(uploads == 24, "%u matfx uploads, expected 24", uploads);
	ok &= Expect(bytes <= 24576, "%u ring bytes, expected at most 24576", bytes);
	for(int i = 0; i < 12; i++)
		DestroyAtomic(atomics[i]);
	geo->destroy();
	RestoreEnvGlobals(g);
	white->destroy();
	return Report(ok, name);
}

static bool
CheckMatfxRingCostPS2AlphaTest(void)
{
	const char *name = "twelve matfx atomics under the PS2 alpha test upload the matfx block 24 times within 4 KB of ring per atomic";
	static const RGBA grey64 = { 64, 64, 64, 255 };
	Texture *white = Tex1(WHITE);
	if(white == nil)
		return Report(false, name);
	EnvGlobals g = SetEnvGlobals(1, 1, 1, grey64);
	Material *top = EnvMaterial(white, white, 1.0f, 0, nil, RED);
	Material *bottom = EnvMaterial(white, white, 1.0f, 0, nil, GREEN);
	Geometry *geo = TwoBands(top, bottom, 1.0f);
	top->destroy();
	bottom->destroy();
	geo->flags |= Geometry::MODULATE;
	Atomic *atomics[12];
	for(int i = 0; i < 12; i++){
		atomics[i] = MatFXAtomic(geo);
		V3d pos = { 0.5f*i - 3.0f, 0.0f, 0.0f };
		atomics[i]->getFrame()->translate(&pos);
	}
	metal::RingSpace probe;
	WorldBegin(GREY);
	SetRenderState(GSALPHATEST, 1);
	bool ok = RingProbe(&probe);
	metal::StateStats s = metal::getStateStats();
	for(int i = 0; i < 12; i++)
		atomics[i]->render();
	metal::StateStats e = metal::getStateStats();
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	uint32 uploads = MatFXStat(e.blockUploads, s.blockUploads);
	uint32 bytes = e.frameRingBytes - s.frameRingBytes;
	Detail("  %u ring bytes for 12 atomics, %u per vehicle atomic; %u matfx uploads\n", bytes, bytes/12, uploads);
	ok &= Expect(uploads == 24, "%u matfx uploads, expected 24", uploads);
	ok &= Expect(bytes <= 49152, "%u ring bytes, expected at most 49152", bytes);
	for(int i = 0; i < 12; i++)
		DestroyAtomic(atomics[i]);
	geo->destroy();
	RestoreEnvGlobals(g);
	white->destroy();
	return Report(ok, name);
}

static bool
CheckEnvSampler(void)
{
	const char *name = "the env texture samples with its own filter and addressing";
	static const RGBA redGreen[2] = { RED, GREEN };
	static const RGBA blackWhite[2] = { BLACK, WHITE };
	Texture *wrap = MakeTexture(2, 1, redGreen, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	Texture *clamp = MakeTexture(2, 1, redGreen, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *linear = MakeTexture(2, 1, blackWhite, Texture::LINEAR, Texture::CLAMP, Texture::CLAMP);
	Texture *black = Tex1(BLACK);
	if(wrap == nil || clamp == nil || linear == nil || black == nil)
		return Report(false, name);
	const V3d side = { 1.5f, 0.0f, -0.1f };
	struct Case { Texture *env; V3d normal; RGBA want; int tol; const char *what; };
	const Case cases[] = {
		{ wrap, side, RED, 0, "nearest WRAP at u 1.25" },
		{ clamp, side, GREEN, 0, "nearest CLAMP at u 1.25" },
		{ linear, envFacing, Rgb(128, 128, 128), 2, "LINEAR CLAMP at u 0.5" },
	};
	Frame *identity = Frame::create();
	identity->updateObjects();
	EnvGlobals g = SetEnvGlobals(0, 0, 1, WHITE);
	bool ok = true;
	for(int i = 0; i < (int)nelem(cases); i++){
		const Case &c = cases[i];
		Material *mat = EnvMaterial(black, c.env, 1.0f, 0, identity, WHITE);
		Geometry *geo = EnvQuad(Geometry::NORMALS | Geometry::PRELIT, c.normal, mat, 1.0f, 10.0f);
		mat->destroy();
		Atomic *atomic = MatFXAtomic(geo);
		WorldBegin(GREY);
		atomic->render();
		WorldEnd();
		DestroyAtomic(atomic);
		geo->destroy();
		ok &= PixelCase(c.want, c.tol, c.what);
	}
	RestoreEnvGlobals(g);
	identity->destroy();
	wrap->destroy();
	clamp->destroy();
	linear->destroy();
	black->destroy();
	return Report(ok, name);
}

static bool
CheckStageOneFeedbackDropped(void)
{
	const char *name = "an env draw sampling its own target on stage 1 is dropped";
	Raster *ct = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	Texture *env = ct ? Texture::create(ct) : nil;
	Texture *white = Tex1(WHITE);
	if(env == nil || white == nil){
		if(env){
			env->raster = nil;
			env->destroy();
		}
		if(ct)
			ct->destroy();
		if(white)
			white->destroy();
		return Report(false, name);
	}
	Material *mat = EnvMaterial(white, env, 1.0f, 0, nil, WHITE);
	Geometry *geo = EnvQuad(Geometry::NORMALS | Geometry::PRELIT, envFacing, mat, 1.0f, 10.0f);
	mat->destroy();
	Atomic *atomic = MatFXAtomic(geo);
	Camera *camera = saved.camera;
	Raster *fb = camera->frameBuffer, *zb = camera->zBuffer;
	camera->frameBuffer = ct;
	camera->zBuffer = nil;
	uint32 feedback = metal::getStateStats().feedbackDraws;
	RGBA blue = BLUE;
	camera->clear(&blue, Camera::CLEARIMAGE);
	camera->beginUpdate();
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(FOGENABLE, 0);
	SetRenderState(CULLMODE, CULLNONE);
	SetRenderStatePtr(TEXTURERASTER, nil);
	atomic->render();
	camera->endUpdate();
	feedback = metal::getStateStats().feedbackDraws - feedback;
	camera->frameBuffer = fb;
	camera->zBuffer = zb;
	metal::setTexture(0, nil);
	metal::setTexture(1, nil);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	bool ok = true;
	if(feedback != 1){
		Detail("  %u feedback draws counted, expected 1\n", feedback);
		ok = false;
	}
	metal::resolveRasterTarget(ct);
	Image *img = ct->toImage();
	if(img == nil || img->depth != 32){
		Detail("  toImage gave %p\n", img);
		ok = false;
	}else{
		bool blue = true;
		for(int i = 0; blue && i < 16*16; i++)
			blue = Near(img, i%16, i/16, BLUE, 0);
		ok &= blue;
	}
	if(img)
		img->destroy();
	DestroyAtomic(atomic);
	geo->destroy();
	env->raster = nil;
	env->destroy();
	ct->destroy();
	white->destroy();
	return Report(ok, name);
}

int
RunMatFXChecks(Camera *camera)
{
	failures = 0;
	WorldOpen(camera);
	CheckMatFXShaderBlocks();
	CheckEnvMatrix(camera);
	CheckEnvCoordinates(camera);
	CheckEnvColour();
	CheckEnvFrameBufferAlpha();
	CheckEnvFog();
	CheckOtherEffectsDrawBase();
	CheckEnvPerMesh();
	CheckEnvVariantPerDraw();
	CheckEnvPS2AlphaTest();
	CheckEnvBlockUploads();
	CheckMatfxRingCost();
	CheckMatfxRingCostPS2AlphaTest();
	CheckEnvSampler();
	CheckStageOneFeedbackDropped();
	WorldClose();
	return failures;
}
