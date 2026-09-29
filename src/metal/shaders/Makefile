all: header_metal.inc im2d_metal.inc simple_metal.inc default_metal.inc im3d_metal.inc

header_metal.inc: header.metal
	(echo 'const char *header_metal_src =';\
	 sed 's/..*/"&\\n"/' header.metal;\
	 echo ';') >header_metal.inc

im2d_metal.inc: im2d.metal
	(echo 'const char *im2d_metal_src =';\
	 sed 's/..*/"&\\n"/' im2d.metal;\
	 echo ';') >im2d_metal.inc

simple_metal.inc: simple.metal
	(echo 'const char *simple_metal_src =';\
	 sed 's/..*/"&\\n"/' simple.metal;\
	 echo ';') >simple_metal.inc

default_metal.inc: default.metal
	(echo 'const char *default_metal_src =';\
	 sed 's/..*/"&\\n"/' default.metal;\
	 echo ';') >default_metal.inc

im3d_metal.inc: im3d.metal
	(echo 'const char *im3d_metal_src =';\
	 sed 's/..*/"&\\n"/' im3d.metal;\
	 echo ';') >im3d_metal.inc
