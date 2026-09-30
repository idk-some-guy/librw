all: header_metal.inc im2d_metal.inc im2d_uv2_metal.inc simple_metal.inc default_metal.inc im3d_metal.inc skin_metal.inc matfx_env_metal.inc

header_metal.inc: header.metal
	(echo 'const char *header_metal_src =';\
	 sed 's/..*/"&\\n"/' header.metal;\
	 echo ';') >header_metal.inc

im2d_metal.inc: im2d.metal
	(echo 'const char *im2d_metal_src =';\
	 sed 's/..*/"&\\n"/' im2d.metal;\
	 echo ';') >im2d_metal.inc

im2d_uv2_metal.inc: im2d_uv2.metal
	(echo 'const char *im2d_uv2_metal_src =';\
	 sed 's/..*/"&\\n"/' im2d_uv2.metal;\
	 echo ';') >im2d_uv2_metal.inc

simple_metal.inc: simple.metal
	(echo 'const char *simple_metal_src =';\
	 sed 's/..*/"&\\n"/' simple.metal;\
	 echo ';') >simple_metal.inc

default_metal.inc: default.metal
	(echo 'const char *default_metal_src =';\
	 sed 's/..*/"&\\n"/' default.metal;\
	 echo ';') >default_metal.inc

skin_metal.inc: skin.metal
	(echo 'const char *skin_metal_src =';\
	 sed 's/..*/"&\\n"/' skin.metal;\
	 echo ';') >skin_metal.inc

im3d_metal.inc: im3d.metal
	(echo 'const char *im3d_metal_src =';\
	 sed 's/..*/"&\\n"/' im3d.metal;\
	 echo ';') >im3d_metal.inc

matfx_env_metal.inc: matfx_env.metal
	(echo 'const char *matfx_env_metal_src =';\
	 sed 's/..*/"&\\n"/' matfx_env.metal;\
	 echo ';') >matfx_env_metal.inc
