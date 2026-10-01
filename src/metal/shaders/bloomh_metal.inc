const char *bloomh_metal_src =
"#ifndef RW_METAL_METALBLOOM_H\n"
"#define RW_METAL_METALBLOOM_H\n"

"#define BLOOM_LEVELS 6\n"

"static inline float\n"
"bloomKarisWeight(float r, float g, float b)\n"
"{\n"
"	return 1.0f/(1.0f + 0.2126f*r + 0.7152f*g + 0.0722f*b);\n"
"}\n"

"static inline float\n"
"bloomRadius(float softness)\n"
"{\n"
"	float s = softness < 0.0f ? 0.0f : softness > 3.0f ? 3.0f : softness;\n"
"	return 0.5f + 0.5f*s;\n"
"}\n"

"static inline int\n"
"bloomLevelSpan(int span, int level)\n"
"{\n"
"	int n = span >> (level + 1);\n"
"	return n < 1 ? 1 : n;\n"
"}\n"

"#endif\n"
;
