static float4
deringPixel(texture2d<float> graded, float4 v, uint2 gid, uint2 outSize)
{
	int2 last = int2(graded.get_width(), graded.get_height()) - 1;
	int2 c = int2(deringTexel(int(gid.x), int(outSize.x), int(graded.get_width())),
		deringTexel(int(gid.y), int(outSize.y), int(graded.get_height())));
	float3 lo = graded.read(uint2(c)).rgb;
	float3 hi = lo;
	for(int dy = -1; dy <= 1; dy++)
		for(int dx = -1; dx <= 1; dx++){
			float3 t = graded.read(uint2(clamp(c + int2(dx, dy), int2(0), last))).rgb;
			lo = min(lo, t);
			hi = max(hi, t);
		}
	return float4(deringClamp(v.r, lo.r, hi.r), deringClamp(v.g, lo.g, hi.g), deringClamp(v.b, lo.b, hi.b), v.a);
}

kernel void
deringInPlace(texture2d<float> graded [[texture(0)]],
              texture2d<float, access::read_write> img [[texture(1)]],
              uint2 gid [[thread_position_in_grid]])
{
	uint2 size = uint2(img.get_width(), img.get_height());
	if(gid.x >= size.x || gid.y >= size.y)
		return;
	img.write(deringPixel(graded, img.read(gid), gid, size), gid);
}

kernel void
deringCopy(texture2d<float> graded [[texture(0)]],
           texture2d<float, access::read> src [[texture(1)]],
           texture2d<float, access::write> dst [[texture(2)]],
           uint2 gid [[thread_position_in_grid]])
{
	uint2 size = uint2(dst.get_width(), dst.get_height());
	if(gid.x >= size.x || gid.y >= size.y)
		return;
	dst.write(deringPixel(graded, src.read(gid), gid, size), gid);
}
