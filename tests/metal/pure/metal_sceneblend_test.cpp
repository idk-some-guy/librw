#include <cstdio>
#include <string>
#include "metalsceneblend.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static const char *dstArg = ", float4 rwSceneDst [[color(0)]]";
static const char *emissiveArg = ", constant float &rwSceneEmissive [[buffer(BUFFER_EMISSIVE)]]";

static std::string
Rewrite(const std::string &src, const char *fs, bool *ok)
{
	std::string out = "untouched";
	*ok = sceneBlendSource(src, fs, &out);
	return out;
}

static void
TestSimple(void)
{
	std::string head = "vertex VertexOut\nfooVS(uint vid [[vertex_id]])\n{\n\tVertexOut o;\n\treturn o;\n}\n";
	std::string sig = "fragment float4\nmyFS(FragmentIn in [[stage_in]],\n     constant State &state [[buffer(BUFFER_STATE)]]";
	std::string body = ")\n{\n\tfloat4 c = in.color; // return early? no\n\tif(c.a < 0.5){\n\t\treturn c*2.0;\n\t}\n\treturn float4(c.rgb, 1.0);\n}\n";
	std::string expectBody = ")\n{\n\tfloat4 c = in.color; // return early? no\n\tif(c.a < 0.5){\n\t\treturn rwSceneOut((c*2.0), rwSceneDst, rwSceneEmissive);\n\t}\n"
		"\treturn rwSceneOut((float4(c.rgb, 1.0)), rwSceneDst, rwSceneEmissive);\n}\n";
	bool ok;
	std::string out = Rewrite(head + sig + body, "myFS", &ok);
	CHECK(ok);
	std::string opaqueSig = "fragment float4\nmyFS_rwOpaque(FragmentIn in [[stage_in]],\n     constant State &state [[buffer(BUFFER_STATE)]]";
	std::string opaqueBody = ")\n{\n\tfloat4 c = in.color; // return early? no\n\tif(c.a < 0.5){\n\t\treturn rwSceneOpaque((c*2.0), rwSceneEmissive);\n\t}\n"
		"\treturn rwSceneOpaque((float4(c.rgb, 1.0)), rwSceneEmissive);\n}\n";
	CHECK(out == head + sceneBlendPrelude + sig + dstArg + emissiveArg + expectBody + opaqueSig + emissiveArg + opaqueBody);
}

static void
TestOwnEmissive(void)
{
	std::string sig = "fragment float4\nemitFS(FragmentIn in [[stage_in]],\n       constant float &scale [[buffer(BUFFER_EMISSIVE)]]";
	std::string body = ")\n{\n\treturn float4(in.color.rgb*scale, in.color.a);\n}\n";
	bool ok;
	std::string out = Rewrite(sig + body, "emitFS", &ok);
	CHECK(ok);
	CHECK(out == sceneBlendPrelude + sig + dstArg + ")\n{\n\treturn rwSceneOut((float4(in.color.rgb*scale, in.color.a)), rwSceneDst, scale);\n}\n" +
		"fragment float4\nemitFS_rwOpaque(FragmentIn in [[stage_in]],\n       constant float &scale [[buffer(BUFFER_EMISSIVE)]]" +
		")\n{\n\treturn rwSceneOpaque((float4(in.color.rgb*scale, in.color.a)), scale);\n}\n");
}

static void
TestExactName(void)
{
	std::string a = "fragment float4\nmyFS2(FragmentIn in [[stage_in]])\n{\n\treturn in.color;\n}\n";
	std::string b = "fragment float4 myFS(FragmentIn in [[stage_in]])\n{\n\treturn in.color;\n}\n";
	bool ok;
	std::string out = Rewrite(a + "/* fragment float4 myFS( */\n" + b, "myFS", &ok);
	CHECK(ok);
	CHECK(out == a + "/* fragment float4 myFS( */\n" + sceneBlendPrelude +
		"fragment float4 myFS(FragmentIn in [[stage_in]]" + dstArg + emissiveArg + ")\n{\n\treturn rwSceneOut((in.color), rwSceneDst, rwSceneEmissive);\n}\n" +
		"fragment float4 myFS_rwOpaque(FragmentIn in [[stage_in]]" + emissiveArg + ")\n{\n\treturn rwSceneOpaque((in.color), rwSceneEmissive);\n}\n");
}

static void
TestReturnWord(void)
{
	std::string sig = "fragment float4\nmyFS(FragmentIn in [[stage_in]]";
	bool ok;
	std::string out = Rewrite(sig + ")\n{\n\tfloat4 returned = in.color;\n\treturn returned;\n}\n", "myFS", &ok);
	CHECK(ok);
	CHECK(out == sceneBlendPrelude + sig + dstArg + emissiveArg +
		")\n{\n\tfloat4 returned = in.color;\n\treturn rwSceneOut((returned), rwSceneDst, rwSceneEmissive);\n}\n" +
		"fragment float4\nmyFS_rwOpaque(FragmentIn in [[stage_in]]" + emissiveArg +
		")\n{\n\tfloat4 returned = in.color;\n\treturn rwSceneOpaque((returned), rwSceneEmissive);\n}\n");
}

static void
TestRefused(void)
{
	bool ok;
	std::string out = Rewrite("fragment float4\notherFS(FragmentIn in [[stage_in]])\n{\n\treturn in.color;\n}\n", "myFS", &ok);
	CHECK(!ok && out == "untouched");
	out = Rewrite("fragment half4\nmyFS(FragmentIn in [[stage_in]])\n{\n\treturn half4(in.color);\n}\n", "myFS", &ok);
	CHECK(!ok && out == "untouched");
	out = Rewrite("fragment float4\nmyFS(FragmentIn in [[stage_in]])\n{\n\treturn in.color;\n", "myFS", &ok);
	CHECK(!ok && out == "untouched");
	out = Rewrite("// fragment float4 myFS(FragmentIn in [[stage_in]]) { return 1; }\n", "myFS", &ok);
	CHECK(!ok && out == "untouched");
}

int
main(void)
{
	TestSimple();
	TestOwnEmissive();
	TestExactName();
	TestReturnWord();
	TestRefused();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all scene blend tests passed\n");
	return 0;
}
