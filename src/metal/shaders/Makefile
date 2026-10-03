all: header_metal.inc im2d_metal.inc im2d_uv2_metal.inc simple_metal.inc default_metal.inc im3d_metal.inc skin_metal.inc matfx_env_metal.inc frame_metal.inc grade_metal.inc emit_metal.inc output_metal.inc bloom_metal.inc bloomh_metal.inc dering_metal.inc deringh_metal.inc fxaa_metal.inc aah_metal.inc prepassh_metal.inc temporalh_metal.inc shadowh_metal.inc prepass_metal.inc prepass_skin_metal.inc motion_metal.inc reactive_metal.inc taa_metal.inc raytrace_metal.inc shadow_metal.inc

header_metal.inc: header.metal ../metalrelit.h
	(echo 'const char *header_metal_src =';\
	 sed -n '1,2p' header.metal | sed 's/..*/"&\\n"/';\
	 sed 's/..*/"&\\n"/' ../metalrelit.h;\
	 sed -n '3,$$p' header.metal | sed 's/..*/"&\\n"/';\
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

prepassh_metal.inc: ../metalprepass.h
	(echo 'const char *prepassh_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metalprepass.h;\
	 echo ';') >prepassh_metal.inc

temporalh_metal.inc: ../metaltemporal.h
	(echo 'const char *temporalh_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metaltemporal.h;\
	 echo ';') >temporalh_metal.inc

shadowh_metal.inc: ../metalshadow.h
	(echo 'const char *shadowh_metal_src =';\
	 sed 's/..*/"&\\n"/' ../metalshadow.h;\
	 echo ';') >shadowh_metal.inc

prepass_metal.inc: prepass.metal
	(echo 'const char *prepass_metal_src =';\
	 sed 's/..*/"&\\n"/' prepass.metal;\
	 echo ';') >prepass_metal.inc

prepass_skin_metal.inc: prepass_skin.metal
	(echo 'const char *prepass_skin_metal_src =';\
	 sed 's/..*/"&\\n"/' prepass_skin.metal;\
	 echo ';') >prepass_skin_metal.inc

motion_metal.inc: motion.metal
	(echo 'const char *motion_metal_src =';\
	 sed 's/..*/"&\\n"/' motion.metal;\
	 echo ';') >motion_metal.inc

reactive_metal.inc: reactive.metal
	(echo 'const char *reactive_metal_src =';\
	 sed 's/..*/"&\\n"/' reactive.metal;\
	 echo ';') >reactive_metal.inc

taa_metal.inc: taa.metal
	(echo 'const char *taa_metal_src =';\
	 sed 's/..*/"&\\n"/' taa.metal;\
	 echo ';') >taa_metal.inc

raytrace_metal.inc: raytrace.metal
	(echo 'const char *raytrace_metal_src =';\
	 sed 's/..*/"&\\n"/' raytrace.metal;\
	 echo ';') >raytrace_metal.inc

shadow_metal.inc: shadow.metal
	(echo 'const char *shadow_metal_src =';\
	 sed 's/..*/"&\\n"/' shadow.metal;\
	 echo ';') >shadow_metal.inc
