const char *emit_metal_src =
"fragment float4\n"
"emitFS(FragmentIn in [[stage_in]],\n"
"       constant State &state [[buffer(BUFFER_STATE)]],\n"
"       constant float &emissiveScale [[buffer(BUFFER_EMISSIVE)]],\n"
"       texture2d<float> tex0 [[texture(0)]],\n"
"       sampler tex0Sampler [[sampler(0)]])\n"
"{\n"
"	float4 color = in.color*tex0.sample(tex0Sampler, in.tex0);\n"
"	color.rgb = mix(state.fogColor.rgb, color.rgb, in.fog);\n"
"	DoAlphaTest(color.a, state);\n"
"	return float4(color.rgb*emissiveScale, color.a);\n"
"}\n"
;
