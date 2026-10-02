struct PrepassSkinIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float3 normal [[attribute(ATTRIB_NORMAL)]];
	float4 weights [[attribute(ATTRIB_WEIGHTS)]];
	uchar4 indices [[attribute(ATTRIB_INDICES)]];
};

vertex PrepassOut
prepassSkinVS(PrepassSkinIn in [[stage_in]],
              constant Scene &scene [[buffer(BUFFER_SCENE)]],
              constant Object &object [[buffer(BUFFER_OBJECT)]],
              constant Skin &skin [[buffer(BUFFER_SKIN)]],
              constant PrepassFrame &pf [[buffer(BUFFER_PREPASSFRAME)]],
              constant PrepassObject &po [[buffer(BUFFER_PREPASSOBJECT)]],
              constant Skin &prevSkin [[buffer(BUFFER_PREVSKIN)]])
{
	PrepassOut out;
	float3 SkinVertex = float3(0.0);
	float3 SkinNormal = float3(0.0);
	float3 PrevVertex = float3(0.0);
	for(int i = 0; i < 4; i++){
		uint b = min(uint(in.indices[i]), uint(MAX_BONES-1));
		float4x4 bone = skin.bones[b];
		SkinVertex += (bone * float4(in.pos, 1.0)).xyz * in.weights[i];
		SkinNormal += (bone * float4(in.normal, 0.0)).xyz * in.weights[i];
		PrevVertex += (prevSkin.bones[b] * float4(in.pos, 1.0)).xyz * in.weights[i];
	}
	float4 V = object.world * float4(SkinVertex, 1.0);
	out.position = scene.proj * scene.view * V;
	out.viewNormal = (scene.view * float4((object.world * float4(SkinNormal, 0.0)).xyz, 0.0)).xyz;
	out.viewPos = (scene.view * V).xyz;
	out.curClip = pf.viewProj * V;
	out.prevClip = pf.prevViewProj * (po.prevWorld * float4(PrevVertex, 1.0));
	out.position = MetalDepth(out.position);
	return out;
}
