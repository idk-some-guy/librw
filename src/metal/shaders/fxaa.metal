static float
fxaaLuma(float3 c)
{
	return aaLuma(c.r, c.g, c.b);
}

static float
fxaaTap(texture2d<float> src, int2 p)
{
	int2 last = int2(src.get_width(), src.get_height()) - 1;
	return fxaaLuma(src.read(uint2(clamp(p, int2(0), last))).rgb);
}

static float
fxaaAt(texture2d<float> src, sampler smp, float2 uv)
{
	return fxaaLuma(src.sample(smp, uv, level(0)).rgb);
}

kernel void
fxaaKernel(texture2d<float> src [[texture(0)]],
           texture2d<float, access::write> dst [[texture(1)]],
           uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= dst.get_width() || gid.y >= dst.get_height())
		return;
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	int2 p = int2(gid);
	float2 texel = 1.0/float2(src.get_width(), src.get_height());
	float2 posM = (float2(gid) + 0.5)*texel;
	float3 rgbM = src.read(gid).rgb;
	float lumaM = fxaaLuma(rgbM);
	float lumaN = fxaaTap(src, p + int2(0, -1));
	float lumaS = fxaaTap(src, p + int2(0, 1));
	float lumaW = fxaaTap(src, p + int2(-1, 0));
	float lumaE = fxaaTap(src, p + int2(1, 0));
	float rangeMax = max(max(max(lumaN, lumaS), max(lumaW, lumaE)), lumaM);
	float rangeMin = min(min(min(lumaN, lumaS), min(lumaW, lumaE)), lumaM);
	if(!aaIsEdge(rangeMax, rangeMin)){
		dst.write(float4(rgbM, 1.0), gid);
		return;
	}
	float range = rangeMax - rangeMin;
	float lumaNW = fxaaTap(src, p + int2(-1, -1));
	float lumaNE = fxaaTap(src, p + int2(1, -1));
	float lumaSW = fxaaTap(src, p + int2(-1, 1));
	float lumaSE = fxaaTap(src, p + int2(1, 1));
	float edgeHorz = abs(lumaNW - 2.0*lumaW + lumaSW) + 2.0*abs(lumaN - 2.0*lumaM + lumaS) + abs(lumaNE - 2.0*lumaE + lumaSE);
	float edgeVert = abs(lumaNW - 2.0*lumaN + lumaNE) + 2.0*abs(lumaW - 2.0*lumaM + lumaE) + abs(lumaSW - 2.0*lumaS + lumaSE);
	bool horzSpan = edgeHorz >= edgeVert;
	float subpixA = (2.0*(lumaN + lumaS + lumaW + lumaE) + lumaNW + lumaNE + lumaSW + lumaSE)/12.0;
	float subpixH = aaSubpixBlend(subpixA, lumaM, range);
	float lumaNeg = horzSpan ? lumaN : lumaW;
	float lumaPos = horzSpan ? lumaS : lumaE;
	float gradientNeg = lumaNeg - lumaM;
	float gradientPos = lumaPos - lumaM;
	bool pairNeg = abs(gradientNeg) >= abs(gradientPos);
	float gradientScaled = max(abs(gradientNeg), abs(gradientPos))*0.25;
	float lengthSign = horzSpan ? texel.y : texel.x;
	if(pairNeg)
		lengthSign = -lengthSign;
	float lumaHalf = ((pairNeg ? lumaNeg : lumaPos) + lumaM)*0.5;
	bool lumaMLTZero = lumaM - lumaHalf < 0.0;
	float2 posB = posM;
	if(horzSpan)
		posB.y += lengthSign*0.5;
	else
		posB.x += lengthSign*0.5;
	float2 offNP = horzSpan ? float2(texel.x, 0.0) : float2(0.0, texel.y);
	float2 posN = posB - offNP*aaStep(0);
	float2 posP = posB + offNP*aaStep(0);
	float lumaEndN = fxaaAt(src, smp, posN) - lumaHalf;
	float lumaEndP = fxaaAt(src, smp, posP) - lumaHalf;
	bool doneN = abs(lumaEndN) >= gradientScaled;
	bool doneP = abs(lumaEndP) >= gradientScaled;
	for(int i = 1; i < FXAA_STEPS && !(doneN && doneP); i++){
		if(!doneN)
			posN -= offNP*aaStep(i);
		if(!doneP)
			posP += offNP*aaStep(i);
		if(i == FXAA_STEPS - 1)
			break;
		if(!doneN){
			lumaEndN = fxaaAt(src, smp, posN) - lumaHalf;
			doneN = abs(lumaEndN) >= gradientScaled;
		}
		if(!doneP){
			lumaEndP = fxaaAt(src, smp, posP) - lumaHalf;
			doneP = abs(lumaEndP) >= gradientScaled;
		}
	}
	float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
	float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
	bool directionN = dstN < dstP;
	float dist = min(dstN, dstP);
	bool goodSpan = ((directionN ? lumaEndN : lumaEndP) < 0.0) != lumaMLTZero;
	float pixelOffset = goodSpan ? 0.5 - dist/(dstN + dstP) : 0.0;
	float offset = max(pixelOffset, subpixH);
	if(horzSpan)
		posM.y += offset*lengthSign;
	else
		posM.x += offset*lengthSign;
	dst.write(float4(src.sample(smp, posM, level(0)).rgb, 1.0), gid);
}
