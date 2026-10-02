struct TaaParams
{
	uint2 size;
	uint valid;
	uint pad;
	float2 jitter;
	float2 pad2;
};

static TMColor
tc(float3 c)
{
	TMColor r = { c.r, c.g, c.b };
	return r;
}

static float3
catmullRom(texture2d<float, access::sample> t, sampler s, float2 uv, float2 size)
{
	float2 pos = uv*size;
	float2 c = floor(pos - 0.5) + 0.5;
	float2 f = pos - c;
	float2 w0 = f*(-0.5 + f*(1.0 - 0.5*f));
	float2 w1 = 1.0 + f*f*(-2.5 + 1.5*f);
	float2 w2 = f*(0.5 + f*(2.0 - 1.5*f));
	float2 w3 = f*f*(-0.5 + 0.5*f);
	float2 w12 = w1 + w2;
	float2 t0 = (c - 1.0)/size, t3 = (c + 2.0)/size, t12 = (c + w2/w12)/size;
	float3 r = t.sample(s, float2(t12.x, t0.y)).rgb*(w12.x*w0.y) + t.sample(s, float2(t0.x, t12.y)).rgb*(w0.x*w12.y) +
		t.sample(s, t12).rgb*(w12.x*w12.y) + t.sample(s, float2(t3.x, t12.y)).rgb*(w3.x*w12.y) +
		t.sample(s, float2(t12.x, t3.y)).rgb*(w12.x*w3.y);
	return r/(w12.x*w0.y + w0.x*w12.y + w12.x*w12.y + w3.x*w12.y + w12.x*w3.y);
}

kernel void
taaResolve(texture2d<float, access::sample> cur [[texture(0)]],
           texture2d<float, access::sample> hist [[texture(1)]],
           texture2d<float, access::read> motion [[texture(2)]],
           texture2d<float, access::read> reactive [[texture(3)]],
           texture2d<float, access::write> out [[texture(4)]],
           texture2d<float, access::write> histOut [[texture(5)]],
           depth2d<float, access::read> depth [[texture(6)]],
           constant TaaParams &p [[buffer(0)]],
           uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	constexpr sampler lin(filter::linear, address::clamp_to_edge, coord::normalized);
	TMColor mn = { 1e9, 1e9, 1e9 }, mx = { -1e9, -1e9, -1e9 };
	uint2 nearest = gid;
	float nearestZ = depth.read(gid);
	for(int dy = -1; dy <= 1; dy++)
		for(int dx = -1; dx <= 1; dx++){
			uint2 q = uint2(clamp(int2(gid) + int2(dx, dy), int2(0), int2(p.size) - 1));
			float z = depth.read(q);
			if(z < nearestZ){
				nearestZ = z;
				nearest = q;
			}
			if((dx != 0 || dy != 0) && reactive.read(q).r >= 0.5)
				continue;
			TMColor y = tmToYCoCg(tmCompress(tc(cur.read(q).rgb)));
			mn = tmMinC(mn, y);
			mx = tmMaxC(mx, y);
		}
	TMUv cu = tmUnjitterUv(float(gid.x), float(gid.y), p.jitter.x, p.jitter.y, float(p.size.x), float(p.size.y));
	float4 c0 = cur.sample(lin, float2(cu.u, cu.v));
	TMColor cc = tmCompress(tc(c0.rgb));
	float2 uv = (float2(gid) + 0.5)/float2(p.size);
	float2 huv = uv + motion.read(nearest).xy;
	bool valid = p.valid != 0 && tmHistoryValid(huv.x, huv.y);
	float w = tmHistoryWeight(reactive.read(gid).r, valid);
	TMColor h = cc;
	if(valid)
		h = tmFromYCoCg(tmClipAabb(tmToYCoCg(tmCompress(tc(catmullRom(hist, lin, huv, float2(p.size))))), mn, mx));
	TMColor r = tmExpand(tmMix(cc, h, w));
	float4 o = float4(r.r, r.g, r.b, c0.a);
	out.write(o, gid);
	histOut.write(o, gid);
}
