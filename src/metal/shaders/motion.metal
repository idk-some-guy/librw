struct FarMotion
{
	float4x4 invViewProj;
	float4x4 viewProj;
	float4x4 prevViewProj;
	uint2 size;
};

static float2
farMotionAt(uint2 gid, constant FarMotion &p)
{
	float2 uv = (float2(gid) + 0.5)/float2(p.size);
	float4 w = p.invViewProj * float4(uv.x*2.0 - 1.0, 1.0 - uv.y*2.0, 1.0, 1.0);
	float4 P = float4(w.xyz/w.w, 1.0);
	float4 c = p.viewProj * P;
	float4 q = p.prevViewProj * P;
	PPVec2 m = ppMotion(c.x, c.y, c.w, q.x, q.y, q.w);
	return float2(m.x, m.y);
}

kernel void
farMotion(depth2d<float, access::read> depth [[texture(0)]],
          texture2d<float, access::write> motion [[texture(1)]],
          constant FarMotion &p [[buffer(0)]],
          uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y || depth.read(gid) < 1.0)
		return;
	motion.write(float4(farMotionAt(gid, p), 0.0, 0.0), gid);
}

kernel void
prepassResolve(depth2d<float, access::read> depth [[texture(0)]],
               texture2d<float, access::write> motion [[texture(1)]],
               texture2d_ms<float, access::read> normalMS [[texture(2)]],
               texture2d_ms<float, access::read> motionMS [[texture(3)]],
               texture2d<float, access::write> normal [[texture(4)]],
               constant FarMotion &p [[buffer(0)]],
               uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	normal.write(normalMS.read(gid, 0), gid);
	float2 m = depth.read(gid) < 1.0 ? motionMS.read(gid, 0).xy : farMotionAt(gid, p);
	motion.write(float4(m, 0.0, 0.0), gid);
}
