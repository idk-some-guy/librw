#include <cstdio>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#include "metalnormals.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestStrip(void)
{
	uint16_t s[] = { 0, 1, 2, 3 };
	uint32_t out[18];
	CHECK(stripToList(s, 4, out) == 6);
	CHECK(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 2 && out[4] == 1 && out[5] == 3);
	uint16_t joined[] = { 0, 1, 2, 2, 5, 5, 6, 7 };
	uint32_t n = stripToList(joined, 8, out);
	CHECK(n == 6);
	CHECK(out[3] == 6 && out[4] == 5 && out[5] == 7);
	CHECK(stripToList(s, 2, out) == 0);
	uint16_t list[] = { 0, 1, 2, 3, 3, 4 };
	CHECK(listToList(list, 6, out) == 3 && out[2] == 2);
}

static void
TestStripRestart(void)
{
	uint16_t s[] = { 0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7 };
	uint32_t out[21];
	uint32_t n = stripToList(s, 9, out);
	CHECK(n == 12);
	CHECK(out[6] == 4 && out[7] == 5 && out[8] == 6);
	CHECK(out[9] == 6 && out[10] == 5 && out[11] == 7);
	bool restart = false;
	for(uint32_t i = 0; i < n; i++)
		restart |= out[i] == 0xFFFF;
	CHECK(!restart);
	uint16_t edge[] = { 0xFFFF, 0, 1, 2, 0xFFFF };
	CHECK(stripToList(edge, 5, out) == 3 && out[0] == 0 && out[1] == 1 && out[2] == 2);
}

static void
TestMeshList(void)
{
	std::vector<uint32_t> list = { 9, 9, 9 };
	uint16_t s[] = { 0, 1, 2, 3, 0xFFFF, 4, 5, 6 };
	CHECK(appendMeshList(s, 8, true, &list) == 9);
	CHECK(list.size() == 12 && list[0] == 9 && list[3] == 0 && list[6] == 2 && list[7] == 1 && list[8] == 3);
	CHECK(list[9] == 4 && list[10] == 5 && list[11] == 6);
	uint16_t l[] = { 0, 1, 2, 3, 3, 4, 5, 6 };
	CHECK(appendMeshList(l, 8, false, &list) == 3);
	CHECK(list.size() == 15 && list[12] == 0 && list[14] == 2);
	CHECK(appendMeshList(s, 2, true, &list) == 0 && list.size() == 15);
	uint16_t big[600];
	for(int i = 0; i < 600; i++)
		big[i] = (uint16_t)i;
	CHECK(appendMeshList(big, 600, true, &list) == 598*3 && list.size() == 15 + 598*3);
}

struct V { float x, y, z; float u, v; };

static const float PI = 3.14159265f;

static bool
Is(const float *n, float x, float y, float z, float tol)
{
	return Near(n[0], x, tol) && Near(n[1], y, tol) && Near(n[2], z, tol);
}

static bool
Zero(const float *n)
{
	return n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f;
}

static float
Dot(const float *n, float x, float y, float z)
{
	return n[0]*x + n[1]*y + n[2]*z;
}

struct Mesh
{
	std::vector<float> p;
	std::vector<uint32_t> list;
};

static uint32_t
AddV(Mesh *m, float x, float y, float z)
{
	m->p.push_back(x);
	m->p.push_back(y);
	m->p.push_back(z);
	return (uint32_t)(m->p.size()/3 - 1);
}

static void
Tri(Mesh *m, uint32_t a, uint32_t b, uint32_t c)
{
	m->list.push_back(a);
	m->list.push_back(b);
	m->list.push_back(c);
}

static std::vector<float>
Derive(const Mesh &m, NormalCounts *c)
{
	std::vector<float> n(m.p.size(), 7.0f);
	deriveNormals((const uint8_t*)m.p.data(), 12, 0, (uint32_t)(m.p.size()/3), m.list.data(), (uint32_t)m.list.size(),
		n.data(), nullptr, c);
	return n;
}

static void
Cylinder(Mesh *m, uint32_t sides, bool split, bool cap)
{
	uint32_t cols = split ? sides + 1 : sides;
	for(uint32_t ring = 0; ring < 2; ring++)
		for(uint32_t j = 0; j < cols; j++){
			float a = 2.0f*PI*(j % sides)/sides;
			AddV(m, cosf(a), sinf(a), ring*2.0f);
		}
	for(uint32_t j = 0; j < sides; j++){
		uint32_t j1 = split ? j + 1 : (j + 1) % sides;
		Tri(m, j, j1, cols + j1);
		Tri(m, j, cols + j1, cols + j);
	}
	if(cap){
		uint32_t c = AddV(m, 0.0f, 0.0f, 2.0f);
		for(uint32_t j = 0; j < sides; j++)
			Tri(m, c, cols + j, cols + (split ? j + 1 : (j + 1) % sides));
	}
}

static void
Box(Mesh *m, bool shared)
{
	for(int f = 0; f < 6; f++){
		int a = f/2;
		float s = f & 1 ? -1.0f : 1.0f;
		float u[3] = { 0, 0, 0 }, v[3] = { 0, 0, 0 };
		u[(a + 1) % 3] = 1.0f;
		v[(a + 2) % 3] = 1.0f;
		if(s < 0)
			std::swap(u, v);
		float cu[4] = { -1, 1, 1, -1 }, cv[4] = { -1, -1, 1, 1 };
		uint32_t id[4];
		for(int i = 0; i < 4; i++){
			float p[3];
			for(int k = 0; k < 3; k++)
				p[k] = (k == a ? s : 0.0f) + cu[i]*u[k] + cv[i]*v[k];
			id[i] = (uint32_t)-1;
			if(shared)
				for(uint32_t w = 0; w < m->p.size()/3; w++)
					if(m->p[w*3] == p[0] && m->p[w*3+1] == p[1] && m->p[w*3+2] == p[2])
						id[i] = w;
			if(id[i] == (uint32_t)-1)
				id[i] = AddV(m, p[0], p[1], p[2]);
		}
		Tri(m, id[0], id[1], id[2]);
		Tri(m, id[0], id[2], id[3]);
	}
}

static void
TestQuadNormals(void)
{
	V v[4] = { { -1, 1, 10, 0, 0 }, { 1, 1, 10, 0, 0 }, { 1, -1, 10, 0, 0 }, { -1, -1, 10, 0, 0 } };
	uint32_t list[6] = { 0, 1, 2, 0, 2, 3 };
	float n[12];
	deriveNormals((const uint8_t*)v, sizeof(V), 0, 4, list, 6, n);
	for(int i = 0; i < 4; i++)
		CHECK(Near(n[i*3+0], 0.0f, 1e-6f) && Near(n[i*3+1], 0.0f, 1e-6f) && Near(n[i*3+2], -1.0f, 1e-6f));
}

static void
TestRidge(void)
{
	float p[6][3] = { { -1, -1, 10 }, { -1, 1, 10 }, { 0, -1, 9 }, { 0, 1, 9 }, { 1, -1, 10 }, { 1, 1, 10 } };
	uint32_t list[12] = { 0, 1, 3, 0, 3, 2, 2, 3, 4, 3, 5, 4 };
	float n[18];
	NormalCounts c;
	deriveNormals((const uint8_t*)p, 12, 0, 6, list, 12, n, nullptr, &c);
	CHECK(Zero(&n[2*3]) && Zero(&n[3*3]));
	CHECK(Is(&n[0*3], -0.70710678f, 0.0f, -0.70710678f, 1e-5f) && Is(&n[1*3], -0.70710678f, 0.0f, -0.70710678f, 1e-5f));
	CHECK(Is(&n[4*3], 0.70710678f, 0.0f, -0.70710678f, 1e-5f) && Is(&n[5*3], 0.70710678f, 0.0f, -0.70710678f, 1e-5f));
	CHECK(c.ambiguous == 2 && c.facesFlat == 4 && c.facesSmooth == 0 && c.facesMixed == 0);
}

static void
TestRidgeSplit(void)
{
	float p[8][3] = { { -1, -1, 10 }, { -1, 1, 10 }, { 0, -1, 9 }, { 0, 1, 9 }, { 0, -1, 9 }, { 0, 1, 9 }, { 1, -1, 10 }, { 1, 1, 10 } };
	uint32_t list[12] = { 0, 1, 3, 0, 3, 2, 4, 5, 6, 5, 7, 6 };
	float n[24];
	NormalCounts c;
	deriveNormals((const uint8_t*)p, 12, 0, 8, list, 12, n, nullptr, &c);
	CHECK(Is(&n[2*3], -0.70710678f, 0.0f, -0.70710678f, 1e-5f) && Is(&n[3*3], -0.70710678f, 0.0f, -0.70710678f, 1e-5f));
	CHECK(Is(&n[4*3], 0.70710678f, 0.0f, -0.70710678f, 1e-5f) && Is(&n[5*3], 0.70710678f, 0.0f, -0.70710678f, 1e-5f));
	CHECK(c.ambiguous == 0 && c.welded == 0 && c.facesFlat == 4);
}

static void
TestShallowRidge(void)
{
	float h = 10.0f - tanf(10.0f*PI/180.0f);
	float p[6][3] = { { -1, -1, 10 }, { -1, 1, 10 }, { 0, -1, h }, { 0, 1, h }, { 1, -1, 10 }, { 1, 1, 10 } };
	uint32_t list[12] = { 0, 1, 3, 0, 3, 2, 2, 3, 4, 3, 5, 4 };
	float n[18];
	deriveNormals((const uint8_t*)p, 12, 0, 6, list, 12, n);
	CHECK(Is(&n[2*3], 0.0f, 0.0f, -1.0f, 1e-5f) && Is(&n[3*3], 0.0f, 0.0f, -1.0f, 1e-5f));
}

static void
TestAreaWeight(void)
{
	float c30 = cosf(30.0f*PI/180.0f), s30 = 0.5f;
	float p[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 3*c30, 0, -3*s30 } };
	uint32_t list[6] = { 0, 1, 2, 0, 3, 2 };
	float n[12];
	deriveNormals((const uint8_t*)p, 12, 0, 4, list, 6, n);
	float x = 3*s30, z = 1.0f + 3*c30, l = sqrtf(x*x + z*z);
	CHECK(Is(&n[0*3], x/l, 0.0f, z/l, 1e-5f));
	uint32_t none[3] = { 0, 1, 2 };
	float m[12];
	for(int i = 0; i < 12; i++)
		m[i] = 7.0f;
	deriveNormals((const uint8_t*)p, 12, 0, 4, none, 3, m);
	CHECK(m[3*3+0] == 0.0f && m[3*3+1] == 0.0f && m[3*3+2] == 0.0f);
}

static void
TestCylinderSeam(void)
{
	Mesh whole, split;
	uint32_t sides = 8;
	Cylinder(&whole, sides, false, false);
	Cylinder(&split, sides, true, false);
	NormalCounts cw, cs;
	std::vector<float> nw = Derive(whole, &cw), ns = Derive(split, &cs);
	const float *ref = &nw[0];
	CHECK(Is(&ns[0], ref[0], ref[1], ref[2], 1e-6f) && Is(&ns[sides*3], ref[0], ref[1], ref[2], 1e-6f));
	CHECK(fabsf(ref[2]) < 1e-6f && ref[0] > cosf(10.0f*PI/180.0f));
	uint32_t top = sides + 1;
	CHECK(Is(&ns[(top + sides)*3], ns[top*3], ns[top*3+1], ns[top*3+2], 1e-6f) && Is(&ns[top*3], nw[sides*3], nw[sides*3+1],
		nw[sides*3+2], 1e-6f));
	for(uint32_t j = 1; j < sides; j++){
		float a = 2.0f*PI*j/sides;
		CHECK(Dot(&ns[j*3], cosf(a), sinf(a), 0.0f) > cosf(10.0f*PI/180.0f));
	}
	CHECK(cw.welded == 0 && cs.welded == 4 && cs.ambiguous == 0 && cs.facesSmooth == 2*sides && cs.facesFlat == 0);
}

static void
TestCylinderCap(void)
{
	Mesh m;
	uint32_t sides = 8;
	Cylinder(&m, sides, false, true);
	NormalCounts c;
	std::vector<float> n = Derive(m, &c);
	for(uint32_t j = 0; j < sides; j++){
		float a = 2.0f*PI*j/sides;
		CHECK(Zero(&n[(sides + j)*3]));
		CHECK(Dot(&n[j*3], cosf(a), sinf(a), 0.0f) > cosf(10.0f*PI/180.0f));
	}
	CHECK(Is(&n[2*sides*3], 0.0f, 0.0f, 1.0f, 1e-6f));
	CHECK(c.ambiguous == sides && c.facesMixed == 2*sides && c.facesFlat == sides && c.facesSmooth == 0);
}

static void
TestBoxShared(void)
{
	Mesh m;
	Box(&m, true);
	CHECK(m.p.size() == 8*3);
	NormalCounts c;
	std::vector<float> n = Derive(m, &c);
	for(uint32_t i = 0; i < 8; i++)
		CHECK(Zero(&n[i*3]));
	CHECK(c.vertices == 8 && c.ambiguous == 8 && c.facesFlat == 12 && c.facesMixed == 0);
}

static void
TestBoxSplit(void)
{
	Mesh m;
	Box(&m, false);
	CHECK(m.p.size() == 24*3);
	NormalCounts c;
	std::vector<float> n = Derive(m, &c);
	for(uint32_t f = 0; f < 6; f++){
		float want[3] = { 0, 0, 0 };
		want[f/2] = f & 1 ? -1.0f : 1.0f;
		for(uint32_t i = 0; i < 4; i++)
			CHECK(Is(&n[(f*4 + i)*3], want[0], want[1], want[2], 1e-6f));
	}
	CHECK(c.ambiguous == 0 && c.welded == 0 && c.facesFlat == 12);
}

static void
TestFlatGrid(void)
{
	Mesh m;
	uint32_t id[4][5];
	for(int y = 0; y < 4; y++)
		for(int x = 0; x < 5; x++)
			id[y][x] = AddV(&m, x < 3 ? (float)x : (float)(x - 1), (float)y, 2.0f);
	for(int y = 0; y < 3; y++)
		for(int x = 0; x < 4; x++){
			if(x == 2)
				continue;
			Tri(&m, id[y][x], id[y][x+1], id[y+1][x+1]);
			Tri(&m, id[y][x], id[y+1][x+1], id[y+1][x]);
		}
	NormalCounts c;
	std::vector<float> n = Derive(m, &c);
	for(uint32_t i = 0; i < 20; i++)
		CHECK(Is(&n[i*3], 0.0f, 0.0f, 1.0f, 1e-6f));
	CHECK(c.ambiguous == 0 && c.facesFlat == 18 && c.facesSmooth == 0 && c.facesMixed == 0 && c.welded == 8);
}

static void
TestFan(void)
{
	struct { int sides; float h; bool smooth; } cones[3] = { { 8, 0.1763f, true }, { 8, 3.0f, true }, { 4, 3.0f, false } };
	for(int k = 0; k < 3; k++){
		Mesh m;
		int sides = cones[k].sides;
		uint32_t centre = AddV(&m, 0.0f, 0.0f, cones[k].h);
		for(int j = 0; j < sides; j++)
			AddV(&m, cosf(2.0f*PI*j/sides), sinf(2.0f*PI*j/sides), 0.0f);
		for(int j = 0; j < sides; j++)
			Tri(&m, centre, 1 + j, 1 + (j + 1) % sides);
		std::vector<float> n = Derive(m, nullptr);
		if(cones[k].smooth)
			CHECK(Is(&n[0], 0.0f, 0.0f, 1.0f, 1e-6f));
		else
			CHECK(Zero(&n[0]));
		for(int j = 0; j < sides; j++)
			CHECK(cones[k].smooth ? !Zero(&n[(1 + j)*3]) && Dot(&n[(1 + j)*3], cosf(2.0f*PI*j/sides), sinf(2.0f*PI*j/sides), 0.0f) > 0.0f :
				Zero(&n[(1 + j)*3]));
	}
}

static void
TestChainedSeam(void)
{
	Mesh m;
	for(int k = -1; k <= 1; k++){
		float a = k*40.0f*PI/180.0f;
		uint32_t p = AddV(&m, 0.0f, 0.0f, 0.0f);
		uint32_t t = AddV(&m, cosf(a), 0.0f, -sinf(a));
		uint32_t y = AddV(&m, 0.0f, 1.0f, 0.0f);
		Tri(&m, p, t, y);
	}
	NormalCounts c;
	std::vector<float> n = Derive(m, &c);
	for(int k = 0; k < 3; k++)
		CHECK(Is(&n[k*3*3], 0.0f, 0.0f, 1.0f, 1e-6f));
	CHECK(c.ambiguous == 0);
}

static void
TestWeldQuantum(void)
{
	float q = defaultNormalRule.quantum;
	for(int far = 0; far < 2; far++){
		float d = far ? 4.0f*q : 0.25f*q;
		float h = tanf(15.0f*PI/180.0f);
		float p[6][3] = { { -1, 0.25f, 0 }, { -1, 1.25f, 0 }, { 0, 0.25f, h }, { 0, 1.25f, h },
			{ d, 0.25f, h }, { d, 1.25f, h } };
		Mesh m;
		for(int i = 0; i < 6; i++)
			AddV(&m, p[i][0], p[i][1], p[i][2]);
		AddV(&m, 1, 0.25f, 0);
		AddV(&m, 1, 1.25f, 0);
		Tri(&m, 0, 2, 3); Tri(&m, 0, 3, 1);
		Tri(&m, 4, 6, 7); Tri(&m, 4, 7, 5);
		NormalCounts c;
		std::vector<float> n = Derive(m, &c);
		if(far)
			CHECK(c.welded == 0 && fabsf(n[2*3+0] - n[4*3+0]) > 0.4f);
		else
			CHECK(c.welded == 4 && Is(&n[2*3], n[4*3], n[4*3+1], n[4*3+2], 1e-6f) && fabsf(n[2*3]) < 0.1f);
	}
}

static void
TestSliverOnce(void)
{
	float d = 0.1f*defaultNormalRule.quantum, e = sqrtf(d);
	float p[6][3] = { { 0, 0, 0 }, { e, 0, 0 }, { 0, e, 0 }, { 0, 0, 0 }, { d, 0, 0 }, { 0, -1, -0.5f } };
	uint32_t list[6] = { 0, 1, 2, 3, 5, 4 };
	float n[18];
	deriveNormals((const uint8_t*)p, 12, 0, 6, list, 6, n);
	float l = sqrtf(4.25f);
	CHECK(Is(&n[0], 0.0f, -0.5f/l, 2.0f/l, 1e-5f));
}

static void
TestNonFinite(void)
{
	float p[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }, { NAN, 2, 0 } };
	uint32_t list[9] = { 0, 1, 2, 0, 2, 3, 3, 2, 4 };
	float n[15];
	NormalCounts c;
	deriveNormals((const uint8_t*)p, 12, 0, 5, list, 9, n, nullptr, &c);
	bool finite = true;
	for(int i = 0; i < 15; i++)
		finite &= std::isfinite(n[i]);
	CHECK(finite);
	CHECK(Zero(&n[4*3]) && Is(&n[2*3], 0.0f, 0.0f, 1.0f, 1e-6f) && Is(&n[3*3], 0.0f, 0.0f, 1.0f, 1e-6f));
}

static void
Prism(Mesh *m, int sides, float noise)
{
	float phi = 0.0f, face[16];
	for(int j = 0; j < sides; j++){
		face[j] = phi;
		phi += 2.0f*PI/sides + (j & 1 ? -noise : noise);
	}
	for(int ring = 0; ring < 2; ring++)
		for(int j = 0; j < sides; j++){
			float a0 = face[(j + sides - 1) % sides], a1 = face[j];
			if(j == 0)
				a0 -= 2.0f*PI;
			float a = 0.5f*(a0 + a1), r = 1.0f/cosf(0.5f*(a1 - a0));
			AddV(m, r*cosf(a), r*sinf(a), ring*2.0f);
		}
	for(int j = 0; j < sides; j++){
		uint32_t j1 = (j + 1) % sides;
		Tri(m, j, j1, sides + j1);
		Tri(m, j, sides + j1, sides + j);
	}
}

static void
Roof(Mesh *m, float pitch, float noise)
{
	float a0 = pitch + noise, a1 = pitch;
	AddV(m, -cosf(a0), -1, -sinf(a0));
	AddV(m, -cosf(a0), 1, -sinf(a0));
	AddV(m, 0, -1, 0);
	AddV(m, 0, 1, 0);
	AddV(m, cosf(a1), -1, -sinf(a1));
	AddV(m, cosf(a1), 1, -sinf(a1));
	Tri(m, 0, 2, 3); Tri(m, 0, 3, 1);
	Tri(m, 2, 4, 5); Tri(m, 2, 5, 3);
}

static bool
SameNormals(const std::vector<float> &a, const std::vector<float> &b)
{
	for(size_t i = 0; i + 2 < a.size(); i += 3){
		if(Zero(&a[i]) != Zero(&b[i]))
			return false;
		if(!Zero(&a[i]) && Dot(&a[i], b[i], b[i+1], b[i+2]) < cosf(0.5f*PI/180.0f))
			return false;
	}
	return true;
}

static void
TestAngleMargin(void)
{
	float noise = 0.1f*PI/180.0f;
	for(int sides = 6; sides <= 8; sides += 2){
		Mesh exact, plus, minus;
		Prism(&exact, sides, 0.0f);
		Prism(&plus, sides, noise);
		Prism(&minus, sides, -noise);
		std::vector<float> n = Derive(exact, nullptr);
		CHECK(SameNormals(n, Derive(plus, nullptr)) && SameNormals(n, Derive(minus, nullptr)));
		bool zero = true, smooth = true;
		for(size_t i = 0; i < n.size(); i += 3){
			zero &= Zero(&n[i]);
			smooth &= !Zero(&n[i]);
		}
		CHECK(sides == 8 ? smooth : zero);
	}
	Mesh exact, plus, minus;
	Roof(&exact, 30.0f*PI/180.0f, 0.0f);
	Roof(&plus, 30.0f*PI/180.0f, noise);
	Roof(&minus, 30.0f*PI/180.0f, -noise);
	std::vector<float> n = Derive(exact, nullptr);
	CHECK(SameNormals(n, Derive(plus, nullptr)) && SameNormals(n, Derive(minus, nullptr)));
	CHECK(Zero(&n[2*3]) && Zero(&n[3*3]) && !Zero(&n[0]) && !Zero(&n[4*3]));
	CHECK(Near(acosf(defaultNormalRule.smoothCos)*180.0f/PI, normalSmoothAngle, 1e-3f) && normalSmoothAngle == 50.0f);
}

static void
TestSliverVote(void)
{
	Mesh m;
	float d = 1e-3f;
	uint32_t a = AddV(&m, 0, 0, 0), b = AddV(&m, 0, 1, 0), c = AddV(&m, 0, 0, 1), e = AddV(&m, 0, 1, 1);
	uint32_t f = AddV(&m, 1, 0, 0), g = AddV(&m, 1, 1, 0), s = AddV(&m, d, 0.5f, d);
	Tri(&m, a, b, e); Tri(&m, a, e, c);
	Tri(&m, a, g, f); Tri(&m, a, b, g);
	Tri(&m, a, b, s);
	NormalCounts cnt;
	std::vector<float> n = Derive(m, &cnt);
	CHECK(Zero(&n[a*3]) && Zero(&n[b*3]));
	CHECK(Is(&n[c*3], 1, 0, 0, 1e-6f) && Is(&n[e*3], 1, 0, 0, 1e-6f));
	CHECK(Is(&n[f*3], 0, 0, -1, 1e-6f) && Is(&n[g*3], 0, 0, -1, 1e-6f));
	CHECK(Is(&n[s*3], 0.70710678f, 0, -0.70710678f, 1e-5f));
	CHECK(defaultNormalRule.sliverArea > 0.0f && defaultNormalRule.sliverArea < 0.1f);
}

static float
PackedAngle(const float *n, const int8_t *p)
{
	float d[3] = { p[0]/127.0f, p[1]/127.0f, p[2]/127.0f };
	float l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
	return acosf(fminf(1.0f, Dot(n, d[0]/l, d[1]/l, d[2]/l)))*180.0f/PI;
}

static void
TestPack(void)
{
	float n[6] = { 0, 0, 0, 0, 0, -1 };
	int8_t p[8];
	packNormals(n, 2, p);
	CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0);
	CHECK(p[4] == 0 && p[5] == 0 && p[6] == -127 && p[7] == 127);
	float worst = 0.0f;
	uint32_t seed = 1;
	for(int i = 0; i < 20000; i++){
		float v[3];
		for(int k = 0; k < 3; k++){
			seed = seed*1664525u + 1013904223u;
			v[k] = (seed >> 8)/8388608.0f - 1.0f;
		}
		float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
		if(l < 1e-3f)
			continue;
		for(int k = 0; k < 3; k++)
			v[k] /= l;
		packNormals(v, 1, p);
		CHECK(p[3] == 127);
		worst = fmaxf(worst, PackedAngle(v, p));
	}
	CHECK(worst < 0.33f);
}

static void
TestOutOfRange(void)
{
	float p[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, -3, 0 } };
	uint32_t list[6] = { 0, 1, 2, 0, 3, 4 };
	float n[15];
	for(int i = 0; i < 15; i++)
		n[i] = 7.0f;
	deriveNormals((const uint8_t*)p, 12, 0, 4, list, 6, n);
	CHECK(Near(n[0*3+2], 1.0f, 1e-6f) && n[0*3+0] == 0.0f);
	CHECK(n[4*3+0] == 7.0f && n[4*3+1] == 7.0f && n[4*3+2] == 7.0f);
}

int
main(void)
{
	TestStrip();
	TestStripRestart();
	TestMeshList();
	TestQuadNormals();
	TestRidge();
	TestRidgeSplit();
	TestShallowRidge();
	TestAreaWeight();
	TestCylinderSeam();
	TestCylinderCap();
	TestBoxShared();
	TestBoxSplit();
	TestFlatGrid();
	TestFan();
	TestChainedSeam();
	TestWeldQuantum();
	TestSliverOnce();
	TestNonFinite();
	TestOutOfRange();
	TestAngleMargin();
	TestSliverVote();
	TestPack();
	if(failures == 0)
		printf("all normals tests passed\n");
	return failures != 0;
}
