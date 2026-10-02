const char *deringh_metal_src =
"#ifndef RW_METAL_METALDERING_H\n"
"#define RW_METAL_METALDERING_H\n"

"#ifdef __METAL_VERSION__\n"
"#define DERING_ISNAN(v) isnan(v)\n"
"#else\n"
"#define DERING_ISNAN(v) ((v) != (v))\n"
"#endif\n"

"static inline float\n"
"deringClamp(float v, float lo, float hi)\n"
"{\n"
"	if(DERING_ISNAN(v))\n"
"		return lo;\n"
"	return v < lo ? lo : v > hi ? hi : v;\n"
"}\n"

"static inline int\n"
"deringTexel(int x, int outSpan, int inSpan)\n"
"{\n"
"	int t = (int)(((float)x + 0.5f)*(float)inSpan/(float)outSpan);\n"
"	return t < 0 ? 0 : t >= inSpan ? inSpan - 1 : t;\n"
"}\n"

"#endif\n"
;
