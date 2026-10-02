const char *aah_metal_src =
"#ifndef RW_METAL_METALAA_H\n"
"#define RW_METAL_METALAA_H\n"

"#ifdef __METAL_VERSION__\n"
"#define AA_MIN fmin\n"
"#define AA_MAX fmax\n"
"#define AA_ABS fabs\n"
"#else\n"
"#include <math.h>\n"
"#define AA_MIN fminf\n"
"#define AA_MAX fmaxf\n"
"#define AA_ABS fabsf\n"
"#endif\n"

"#define FXAA_EDGE_THRESHOLD 0.125f\n"
"#define FXAA_EDGE_THRESHOLD_MIN 0.0312f\n"
"#define FXAA_SUBPIX 0.75f\n"
"#define FXAA_STEPS 12\n"

"static inline float\n"
"aaLuma(float r, float g, float b)\n"
"{\n"
"	return 0.299f*AA_MIN(AA_MAX(r, 0.0f), 1.0f) + 0.587f*AA_MIN(AA_MAX(g, 0.0f), 1.0f) +\n"
"		0.114f*AA_MIN(AA_MAX(b, 0.0f), 1.0f);\n"
"}\n"

"static inline bool\n"
"aaIsEdge(float lumaMax, float lumaMin)\n"
"{\n"
"	return lumaMax - lumaMin >= AA_MAX(FXAA_EDGE_THRESHOLD_MIN, lumaMax*FXAA_EDGE_THRESHOLD);\n"
"}\n"

"static inline float\n"
"aaStep(int i)\n"
"{\n"
"	return i < 5 ? 1.0f : i == 5 ? 1.5f : i < 10 ? 2.0f : i == 10 ? 4.0f : 8.0f;\n"
"}\n"

"static inline float\n"
"aaSubpixBlend(float average, float lumaM, float range)\n"
"{\n"
"	float c = AA_MIN(AA_MAX(AA_ABS(average - lumaM)/range, 0.0f), 1.0f);\n"
"	float d = (-2.0f*c + 3.0f)*c*c;\n"
"	return d*d*FXAA_SUBPIX;\n"
"}\n"

"#endif\n"
;
