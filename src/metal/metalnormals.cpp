#include <math.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "metalnormals.h"

namespace rw {
namespace metal {

uint32_t
stripToList(const uint16_t *s, uint32_t n, uint32_t *out)
{
	uint32_t k = 0, start = 0;
	for(uint32_t i = 0; i < n; i++){
		if(s[i] == 0xFFFF){
			start = i + 1;
			continue;
		}
		if(i < start + 2)
			continue;
		uint32_t a = s[i-2], b = s[i-1], c = s[i];
		if(a == b || b == c || a == c)
			continue;
		if((i - start) & 1){
			out[k++] = b;
			out[k++] = a;
		}else{
			out[k++] = a;
			out[k++] = b;
		}
		out[k++] = c;
	}
	return k;
}

uint32_t
listToList(const uint16_t *l, uint32_t n, uint32_t *out)
{
	uint32_t k = 0;
	for(uint32_t i = 0; i + 2 < n; i += 3){
		if(l[i] == l[i+1] || l[i+1] == l[i+2] || l[i] == l[i+2])
			continue;
		out[k++] = l[i];
		out[k++] = l[i+1];
		out[k++] = l[i+2];
	}
	return k;
}

static void
position(const uint8_t *v, uint32_t stride, uint32_t posOffset, uint32_t i, float p[3])
{
	memcpy(p, v + (size_t)i*stride + posOffset, 3*sizeof(float));
}

const float normalSmoothAngle = 50.0f;
const NormalRule defaultNormalRule = { cosf(normalSmoothAngle*3.14159265f/180.0f), 1.0f/1024.0f, 0.01f };

struct Face
{
	float f[3];
	float u[3];
	float area;
	uint32_t v[3];
};

struct Cell
{
	int64_t x, y, z;
	bool operator<(const Cell &o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
	bool operator==(const Cell &o) const { return x == o.x && y == o.y && z == o.z; }
};

static float
dot3(const float *a, const float *b)
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static bool
finite3(const float *p)
{
	return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

static uint32_t
root(std::vector<uint32_t> &parent, uint32_t a)
{
	while(parent[a] != a)
		a = parent[a] = parent[parent[a]];
	return a;
}

void
deriveNormals(const uint8_t *v, uint32_t stride, uint32_t posOffset, uint32_t numVertices,
              const uint32_t *list, uint32_t numIndices, float *n, const NormalRule *rule, NormalCounts *counts)
{
	const NormalRule &r = rule ? *rule : defaultNormalRule;
	NormalCounts c = {};
	memset(n, 0, (size_t)numVertices*3*sizeof(float));
	if(counts)
		*counts = c;
	if(numVertices == 0)
		return;

	std::vector<Face> faces;
	faces.reserve(numIndices/3);
	std::vector<uint32_t> first(numVertices + 1, 0);
	for(uint32_t t = 0; t + 2 < numIndices; t += 3){
		Face f;
		f.v[0] = list[t];
		f.v[1] = list[t+1];
		f.v[2] = list[t+2];
		if(f.v[0] >= numVertices || f.v[1] >= numVertices || f.v[2] >= numVertices)
			continue;
		float a[3], b[3], p[3];
		position(v, stride, posOffset, f.v[0], a);
		position(v, stride, posOffset, f.v[1], b);
		position(v, stride, posOffset, f.v[2], p);
		float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		float e2[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
		f.f[0] = e1[1]*e2[2] - e1[2]*e2[1];
		f.f[1] = e1[2]*e2[0] - e1[0]*e2[2];
		f.f[2] = e1[0]*e2[1] - e1[1]*e2[0];
		float l = sqrtf(dot3(f.f, f.f));
		if(!std::isfinite(l) || !finite3(f.f) || l < 1e-12f)
			continue;
		f.area = l;
		for(int k = 0; k < 3; k++){
			f.u[k] = f.f[k]/l;
			first[f.v[k] + 1]++;
		}
		faces.push_back(f);
	}
	for(uint32_t i = 0; i < numVertices; i++)
		first[i + 1] += first[i];
	std::vector<uint32_t> incident(faces.size()*3), fill(first.begin(), first.end() - 1);
	for(uint32_t t = 0; t < faces.size(); t++)
		for(int k = 0; k < 3; k++)
			incident[fill[faces[t].v[k]]++] = t;

	std::vector<Cell> cells(numVertices);
	std::vector<uint32_t> order(numVertices), group(numVertices), groupStart;
	for(uint32_t i = 0; i < numVertices; i++){
		float p[3];
		position(v, stride, posOffset, i, p);
		order[i] = i;
		if(finite3(p)){
			cells[i].x = (int64_t)floor((double)p[0]/r.quantum + 0.5);
			cells[i].y = (int64_t)floor((double)p[1]/r.quantum + 0.5);
			cells[i].z = (int64_t)floor((double)p[2]/r.quantum + 0.5);
		}else{
			cells[i].x = INT64_MAX;
			cells[i].y = INT64_MAX;
			cells[i].z = i;
		}
	}
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b){
		return cells[a] < cells[b] || (cells[a] == cells[b] && a < b); });
	for(uint32_t i = 0; i < numVertices; i++){
		if(i == 0 || !(cells[order[i]] == cells[order[i-1]]))
			groupStart.push_back(i);
		group[order[i]] = (uint32_t)groupStart.size() - 1;
	}
	groupStart.push_back(numVertices);

	std::vector<uint32_t> stamp(faces.size(), UINT32_MAX), local(faces.size());
	std::vector<uint32_t> members, parent, size;
	std::vector<uint8_t> votes;
	std::vector<float> sums;
	for(uint32_t g = 0; g + 1 < groupStart.size(); g++){
		members.clear();
		for(uint32_t gi = groupStart[g]; gi < groupStart[g + 1]; gi++){
			uint32_t w = order[gi];
			for(uint32_t j = first[w]; j < first[w + 1]; j++){
				uint32_t t = incident[j];
				if(stamp[t] == g)
					continue;
				stamp[t] = g;
				local[t] = (uint32_t)members.size();
				members.push_back(t);
			}
		}
		uint32_t k = (uint32_t)members.size();
		float largest = 0.0f;
		for(uint32_t a = 0; a < k; a++)
			largest = std::max(largest, faces[members[a]].area);
		parent.resize(k);
		votes.resize(k);
		for(uint32_t a = 0; a < k; a++){
			parent[a] = a;
			votes[a] = faces[members[a]].area >= r.sliverArea*largest;
		}
		for(uint32_t a = 0; a < k; a++)
			for(uint32_t b = a + 1; b < k; b++){
				if(!votes[a] || !votes[b] || dot3(faces[members[a]].u, faces[members[b]].u) < r.smoothCos)
					continue;
				uint32_t ra = root(parent, a), rb = root(parent, b);
				if(ra != rb)
					parent[ra < rb ? rb : ra] = ra < rb ? ra : rb;
			}
		sums.assign((size_t)k*3, 0.0f);
		size.assign(k, 0);
		for(uint32_t a = 0; a < k; a++){
			if(!votes[a])
				continue;
			uint32_t ra = root(parent, a);
			for(int x = 0; x < 3; x++)
				sums[(size_t)ra*3 + x] += faces[members[a]].f[x];
			size[ra]++;
		}
		for(uint32_t gi = groupStart[g]; gi < groupStart[g + 1]; gi++){
			uint32_t i = order[gi], b = first[i], e = first[i + 1];
			if(b == e)
				continue;
			c.vertices++;
			uint32_t cluster = UINT32_MAX, own = 0;
			bool ambiguous = false;
			for(uint32_t j = b; j < e; j++){
				uint32_t a = local[incident[j]];
				if(!votes[a])
					continue;
				own++;
				uint32_t ra = root(parent, a);
				if(cluster == UINT32_MAX)
					cluster = ra;
				else if(ra != cluster)
					ambiguous = true;
			}
			for(uint32_t a = 0; own == 0 && a < k; a++){
				if(!votes[a])
					continue;
				uint32_t ra = root(parent, a);
				if(cluster == UINT32_MAX)
					cluster = ra;
				else if(ra != cluster)
					ambiguous = true;
			}
			if(ambiguous || cluster == UINT32_MAX){
				c.ambiguous++;
				continue;
			}
			const float *sum = &sums[(size_t)cluster*3];
			float l = sqrtf(dot3(sum, sum));
			if(!(l > 0.0f)){
				c.ambiguous++;
				continue;
			}
			for(int x = 0; x < 3; x++)
				n[(size_t)i*3 + x] = sum[x]/l;
			if(size[cluster] > own)
				c.welded++;
		}
	}

	if(counts == nullptr)
		return;
	const float flatCos = 0.99985f;
	for(const Face &f : faces){
		int flat = 0, zero = 0;
		for(int k = 0; k < 3; k++){
			const float *p = &n[(size_t)f.v[k]*3];
			if(p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f){
				zero++;
				flat++;
			}else if(dot3(p, f.u) > flatCos)
				flat++;
		}
		if(flat == 3)
			c.facesFlat++;
		else if(zero == 0)
			c.facesSmooth++;
		else
			c.facesMixed++;
	}
	*counts = c;
}

void
packNormals(const float *n, uint32_t numVertices, int8_t *packed)
{
	memset(packed, 0, (size_t)numVertices*4);
	for(uint32_t i = 0; i < numVertices; i++){
		const float *v = &n[(size_t)i*3];
		int8_t *p = &packed[(size_t)i*4];
		if(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f)
			continue;
		float base[3], best = -2.0f;
		for(int k = 0; k < 3; k++)
			base[k] = floorf(v[k]*127.0f);
		for(int mask = 0; mask < 8; mask++){
			float c[3];
			for(int k = 0; k < 3; k++)
				c[k] = std::min(127.0f, std::max(-127.0f, base[k] + ((mask >> k) & 1)));
			float l = sqrtf(dot3(c, c));
			if(l == 0.0f || dot3(c, v)/l <= best)
				continue;
			best = dot3(c, v)/l;
			for(int k = 0; k < 3; k++)
				p[k] = (int8_t)c[k];
		}
		p[3] = 127;
	}
}

}
}
