fragment float
reactiveFS(FragmentIn in [[stage_in]],
           texture2d<float> tex0 [[texture(0)]],
           sampler tex0Sampler [[sampler(0)]])
{
	return in.color.a*tex0.sample(tex0Sampler, in.tex0).a;
}
