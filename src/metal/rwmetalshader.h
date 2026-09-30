#ifdef RW_METAL

namespace rw {
namespace metal {

enum ShaderVariant
{
	VARIANT_ALPHATEST = 1,
	VARIANT_DIRECTIONALS = 2,
	VARIANT_POINTLIGHTS = 4,
	VARIANT_SPOTLIGHTS = 8,
	VARIANT_ALL = 15,
	NUMVARIANTS = 16
};

enum BufferIndices
{
	BUFFER_VERTEX = 0,
	BUFFER_SCENE,
	BUFFER_OBJECT,
	BUFFER_MATERIAL,
	BUFFER_STATE,
	BUFFER_SKIN,
	BUFFER_CUSTOM,
	BUFFER_LIGHTS,
	BUFFER_MATFX,
	BUFFER_DEFAULTATTRIBS = 30
};

enum { MAXCUSTOMCONSTANTS = 1024 };

struct Shader
{
	void *library;
	char *vertexName;
	char *fragmentName;
	uint32 shaderId;
	uint32 variantMask;
	void *vertexFns[NUMVARIANTS];
	void *fragmentFns[NUMVARIANTS];

	static Shader *create(const char **src, const char *vs, const char *fs, uint32 variantMask = VARIANT_ALPHATEST);
	bool32 getFunctions(uint32 variant, void **vs, void **fs);
	void use(uint32 variant = VARIANT_ALPHATEST);
	void destroy(void);
};

extern Shader *currentShader;
extern uint32 currentVariant;

}
}

#endif
