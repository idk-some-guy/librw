all: header_metal.inc im2d_metal.inc im2d_uv2_metal.inc simple_metal.inc default_metal.inc im3d_metal.inc skin_metal.inc matfx_env_metal.inc frame_metal.inc grade_metal.inc emit_metal.inc output_metal.inc bloom_metal.inc bloomh_metal.inc dering_metal.inc deringh_metal.inc fxaa_metal.inc aah_metal.inc

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

frame_metal.inc: frame.metal
	(echo 'const char *frame_metal_src =';\
	 sed 's/..*/"&\\n"/' frame.metal;\
	 echo ';') >frame_metal.inc

grade_metal.inc: grade.metal
	(echo 'const char *grade_metal_src =';\
	 sed 's/..*/"&\\n"/' grade.metal;\
	 echo ';') >grade_metal.inc

emit_metal.inc: emit.metal
	(echo 'const char *emit_metal_src =';\
	 sed 's/..*/"&\\n"/' emit.metal;\
	 echo ';') >emit_metal.inc

output_metal.inc: ../metaloutput.h
	(echo 'const char *output_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metaloutput.h;\
	 echo ';') >output_metal.inc

bloom_metal.inc: bloom.metal
	(echo 'const char *bloom_metal_src =';\
	 sed 's/..*/"&\\n"/' bloom.metal;\
	 echo ';') >bloom_metal.inc

bloomh_metal.inc: ../metalbloom.h
	(echo 'const char *bloomh_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metalbloom.h;\
	 echo ';') >bloomh_metal.inc

dering_metal.inc: dering.metal
	(echo 'const char *dering_metal_src =';\
	 sed 's/..*/"&\\n"/' dering.metal;\
	 echo ';') >dering_metal.inc

deringh_metal.inc: ../metaldering.h
	(echo 'const char *deringh_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metaldering.h;\
	 echo ';') >deringh_metal.inc

fxaa_metal.inc: fxaa.metal
	(echo 'const char *fxaa_metal_src =';\
	 sed 's/..*/"&\\n"/' fxaa.metal;\
	 echo ';') >fxaa_metal.inc

aah_metal.inc: ../metalaa.h
	(echo 'const char *aah_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metalaa.h;\
	 echo ';') >aah_metal.inc
