const char *output_metal_src =
"#ifndef RW_METAL_METALOUTPUT_H\n"
"#define RW_METAL_METALOUTPUT_H\n"

"#ifdef __METAL_VERSION__\n"
"#define OUTPUT_MIN fmin\n"
"#define OUTPUT_MAX fmax\n"
"#define OUTPUT_POW pow\n"
"#else\n"
"#include <math.h>\n"
"#define OUTPUT_MIN fminf\n"
"#define OUTPUT_MAX fmaxf\n"
"#define OUTPUT_POW powf\n"
"#endif\n"

"#define SDR_SHOULDER_KNEE 1.0f\n"

"static inline float\n"
"sdrShoulder(float x, float m)\n"
"{\n"
"	if(!(m > 1.0f))\n"
"		return x;\n"
"	float w = (m - 1.0f)/(m - 1.0f + SDR_SHOULDER_KNEE);\n"
"	return OUTPUT_MIN(x + w*(1.0f - x), 1.0f);\n"
"}\n"

"#endif\n"
;
