struct GradeFrame
{
	float bloomWeight;
	uint grade;
	float bloomNorm;
	float pad;
};

kernel void
gradeKernel(texture2d<float, access::read> scene [[texture(0)]],
            texture2d<float> bloom [[texture(2)]],
            texture2d<float, access::write> graded [[texture(3)]],
            constant GradeFrame &frame [[buffer(0)]],
            constant float4 *params [[buffer(1)]],
            uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= graded.get_width() || gid.y >= graded.get_height())
		return;
	float3 c = clamp(scene.read(gid).rgb, 0.0, 65504.0);
	if(frame.bloomWeight > 0.0){
		constexpr sampler smp(filter::linear, address::clamp_to_edge);
		float2 uv = (float2(gid) + 0.5)/float2(graded.get_width(), graded.get_height());
		c = mix(c, bloom.sample(smp, uv, level(0)).rgb*frame.bloomNorm, frame.bloomWeight);
	}
	float3 low = rint(clamp(c, 0.0, 1.0)*255.0)/255.0;
	float3 high = max(c - 1.0, 0.0);
	float3 gain = float3(1.0);
	if(frame.grade != 0){
		low = hostGrade(float4(low, 1.0), params).rgb;
		gain = hostGrade(float4(0.25), params).rgb*4.0;
	}
	low = rint(saturate(low)*255.0)/255.0;
	graded.write(float4(low + high*gain, 1.0), gid);
}
