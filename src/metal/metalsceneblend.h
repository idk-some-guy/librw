#ifndef RW_METAL_METALSCENEBLEND_H
#define RW_METAL_METALSCENEBLEND_H

#include <cctype>
#include <cstring>
#include <string>

namespace rw {
namespace metal {

static const char sceneBlendPrelude[] =
	"#ifndef BUFFER_EMISSIVE\n"
	"#define BUFFER_EMISSIVE 9\n"
	"#endif\n"
	"constant uint rwSceneSrcFactor [[function_constant(4)]];\n"
	"constant uint rwSceneDestFactor [[function_constant(5)]];\n"
	"\n"
	"static float4\n"
	"rwSceneFactor(uint f, float4 s, float4 d)\n"
	"{\n"
	"\tswitch(f){\n"
	"\tcase 0: return float4(0.0);\n"
	"\tcase 2: return s;\n"
	"\tcase 3: return 1.0 - s;\n"
	"\tcase 4: return float4(s.a);\n"
	"\tcase 5: return float4(1.0 - s.a);\n"
	"\tcase 6: return d;\n"
	"\tcase 7: return 1.0 - d;\n"
	"\tcase 8: return float4(d.a);\n"
	"\tcase 9: return float4(1.0 - d.a);\n"
	"\tcase 10: return float4(float3(min(s.a, 1.0 - d.a)), 1.0);\n"
	"\t}\n"
	"\treturn float4(1.0);\n"
	"}\n"
	"\n"
	"static float4\n"
	"rwSceneOut(float4 src, float4 dst, float emissive)\n"
	"{\n"
	"\tif(emissive > 1.0)\n"
	"\t\treturn src*rwSceneFactor(rwSceneSrcFactor, src, dst) + dst*rwSceneFactor(rwSceneDestFactor, src, dst);\n"
	"\tfloat4 excess = max(dst - 1.0, 0.0);\n"
	"\tsrc = saturate(src);\n"
	"\tdst = saturate(dst);\n"
	"\tfloat4 df = rwSceneFactor(rwSceneDestFactor, src, dst);\n"
	"\treturn min(src*rwSceneFactor(rwSceneSrcFactor, src, dst) + dst*df, 1.0) + excess*df;\n"
	"}\n"
	"\n"
	"static float4\n"
	"rwSceneOpaque(float4 src, float emissive)\n"
	"{\n"
	"\treturn emissive > 1.0 ? src : saturate(src);\n"
	"}\n"
	"\n";

static inline bool
sceneIdentChar(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static inline bool
sceneWordAt(const std::string &c, size_t i, const char *w)
{
	size_t n = strlen(w);
	return c.compare(i, n, w) == 0 && (i == 0 || !sceneIdentChar(c[i-1])) &&
		(i+n >= c.size() || !sceneIdentChar(c[i+n]));
}

static inline size_t
sceneSkipSpace(const std::string &c, size_t i)
{
	while(i < c.size() && isspace((unsigned char)c[i]))
		i++;
	return i;
}

static inline size_t
sceneMatchClose(const std::string &c, size_t open, char o, char cl)
{
	int depth = 0;
	for(size_t i = open; i < c.size(); i++)
		if(c[i] == o)
			depth++;
		else if(c[i] == cl && --depth == 0)
			return i;
	return std::string::npos;
}

static inline std::string
sceneBlankComments(const std::string &s)
{
	std::string c = s;
	size_t i = 0, n = s.size();
	while(i < n){
		if(s[i] == '/' && i+1 < n && s[i+1] == '/'){
			while(i < n && s[i] != '\n')
				c[i++] = ' ';
		}else if(s[i] == '/' && i+1 < n && s[i+1] == '*'){
			size_t e = s.find("*/", i+2);
			e = e == std::string::npos ? n : e+2;
			for(; i < e; i++)
				if(s[i] != '\n')
					c[i] = ' ';
		}else
			i++;
	}
	return c;
}

static inline std::string
sceneEmissiveParam(const std::string &params)
{
	size_t e = params.find("buffer(BUFFER_EMISSIVE)");
	if(e == std::string::npos)
		return "";
	size_t b = params.rfind("[[", e);
	if(b == std::string::npos)
		return "";
	size_t j = b;
	while(j > 0 && isspace((unsigned char)params[j-1]))
		j--;
	size_t k = j;
	while(k > 0 && sceneIdentChar(params[k-1]))
		k--;
	return params.substr(k, j-k);
}

static inline bool
sceneBlendSource(const std::string &src, const char *fs, std::string *out)
{
	const size_t npos = std::string::npos;
	std::string c = sceneBlankComments(src);
	size_t def = npos, open = npos, name = npos, pos = 0;

	while((pos = c.find("fragment", pos)) != npos){
		size_t i = sceneSkipSpace(c, pos+8);
		if(sceneWordAt(c, pos, "fragment") && sceneWordAt(c, i, "float4")){
			i = sceneSkipSpace(c, i+6);
			if(sceneWordAt(c, i, fs)){
				name = i;
				i = sceneSkipSpace(c, i+strlen(fs));
				if(i < c.size() && c[i] == '('){
					def = pos;
					open = i;
					break;
				}
			}
		}
		pos += 8;
	}
	if(def == npos)
		return false;
	size_t close = sceneMatchClose(c, open, '(', ')');
	if(close == npos)
		return false;
	size_t bodyOpen = sceneSkipSpace(c, close+1);
	if(bodyOpen >= c.size() || c[bodyOpen] != '{')
		return false;
	size_t bodyClose = sceneMatchClose(c, bodyOpen, '{', '}');
	if(bodyClose == npos)
		return false;

	std::string dstArg = ", float4 rwSceneDst [[color(0)]]", emissiveArg;
	std::string emissive = sceneEmissiveParam(c.substr(open+1, close-open-1));
	if(emissive.empty()){
		emissiveArg = ", constant float &rwSceneEmissive [[buffer(BUFFER_EMISSIVE)]]";
		emissive = "rwSceneEmissive";
	}

	std::string body, opaque;
	size_t last = bodyOpen;
	for(size_t i = bodyOpen; i < bodyClose; i++){
		if(!sceneWordAt(c, i, "return"))
			continue;
		size_t s = sceneSkipSpace(c, i+6), j;
		int depth = 0;
		for(j = s; j < bodyClose; j++){
			char ch = c[j];
			if(ch == '(' || ch == '[' || ch == '{')
				depth++;
			else if(ch == ')' || ch == ']' || ch == '}')
				depth--;
			else if(ch == ';' && depth == 0)
				break;
		}
		if(j >= bodyClose || j == s)
			return false;
		size_t e = j;
		while(e > s && isspace((unsigned char)c[e-1]))
			e--;
		body += src.substr(last, i-last);
		body += "return rwSceneOut((" + src.substr(s, e-s) + "), rwSceneDst, " + emissive + ")";
		opaque += src.substr(last, i-last);
		opaque += "return rwSceneOpaque((" + src.substr(s, e-s) + "), " + emissive + ")";
		last = j;
		i = j;
	}
	body += src.substr(last, bodyClose+1-last);
	opaque += src.substr(last, bodyClose+1-last);

	size_t line = src.rfind('\n', def);
	line = line == npos ? 0 : line+1;
	std::string head = src.substr(line, name-line);
	std::string params = src.substr(name+strlen(fs), close-name-strlen(fs));
	std::string mid = src.substr(close, bodyOpen-close);
	*out = src.substr(0, line) + sceneBlendPrelude +
		head + fs + params + dstArg + emissiveArg + mid + body + "\n" +
		head + fs + "_rwOpaque" + params + emissiveArg + mid + opaque + src.substr(bodyClose+1);
	return true;
}

}
}

#endif
