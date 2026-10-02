const char *reactive_metal_src =
"fragment float\n"
"reactiveFS(FragmentIn in [[stage_in]],\n"
"           texture2d<float> tex0 [[texture(0)]],\n"
"           sampler tex0Sampler [[sampler(0)]])\n"
"{\n"
"	return in.color.a*tex0.sample(tex0Sampler, in.tex0).a;\n"
"}\n"
;
