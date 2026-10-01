newoption {
	trigger		= "gfxlib",
	value       = "LIBRARY",
	description = "Choose a particular development library",
	default		= "glfw",
	allowed		= {
		{ "glfw",	"GLFW" },
		{ "sdl2",	"SDL2" },
		{ "sdl3",	"SDL3" },
	},
}

newoption {
	trigger     = "glfwdir64",
	value       = "PATH",
	description = "Directory of glfw",
	default     = "../glfw-3.3.4.bin.WIN64",
}

newoption {
	trigger     = "glfwdir32",
	value       = "PATH",
	description = "Directory of glfw",
	default     = "../glfw-3.3.4.bin.WIN32",
}

newoption {
	trigger     = "sdl2dir",
	value       = "PATH",
	description = "Directory of sdl2",
	default     = "../SDL2-2.32.10",
}

newoption {
	trigger     = "sdl3dir",
	value       = "PATH",
	description = "Directory of sdl3",
	default     = "../SDL3-3.2.22",
}

newoption {
	trigger     = "freesce",
	description = "ps2: build against freesce with its own ee-gcc 2.9, instead of the SCE SDK"
}

newoption {
	trigger     = "metal-asan",
	description = "Build the Metal smoke test with AddressSanitizer",
}

local HomebrewPrefix = os.getenv("HOMEBREW_PREFIX")
if HomebrewPrefix == nil or HomebrewPrefix == "" then
	HomebrewPrefix = "/opt/homebrew"
end

workspace "librw"
	location "build"
	language "C++"

	configurations { "Release", "Debug" }
	filter { "system:windows" }
		configurations { "ReleaseStatic" }
		platforms { "win-x86-null", "win-x86-gl3", "win-x86-d3d9",
			"win-amd64-null", "win-amd64-gl3", "win-amd64-d3d9" }
	filter { "system:linux" }
		platforms { "linux-x86-null", "linux-x86-gl3",
		"linux-amd64-null", "linux-amd64-gl3",
		"linux-arm-null", "linux-arm-gl3",
		"ps2" }
		if _OPTIONS["gfxlib"] == "sdl2" then
			includedirs { "/usr/include/SDL2" }
		end
	filter { "system:macosx" }
		platforms { "macosx-arm64-metal", "macosx-arm64-metal-cocoa" }
	filter {}

	filter "configurations:Debug"
		defines { "DEBUG" }
		symbols "On"
	filter "configurations:Release*"
		defines { "NDEBUG" }
		optimize "On"
--	filter "configurations:ReleaseStatic"
--		staticruntime("On")

	filter { "platforms:*null" }
		defines { "RW_NULL" }
	filter { "platforms:*gl3" }
		defines { "RW_GL3" }
		if _OPTIONS["gfxlib"] == "sdl2" then
			defines { "LIBRW_SDL2" }
		elseif _OPTIONS["gfxlib"] == "sdl3" then
			defines { "LIBRW_SDL3" }
		elseif _OPTIONS["gfxlib"] == "glfw" then
			defines { "LIBRW_GLFW" }
		end
	filter { "platforms:*d3d9" }
		defines { "RW_D3D9" }
	filter { "platforms:ps2" }
		defines { "RW_PS2" }
		toolset "gcc"
		optimize "Off"
		if _OPTIONS["freesce"] then
			-- freesce, by the xtc convention: two roots, two axes.
			-- FREESCE is the SDK vintage -- a tree root with
			-- ee/include and ee/lib under it, which is how an
			-- install and a git worktree are both laid out -- and
			-- FREESCE_GCC is the compiler root (an env var read by
			-- the tools/freesce wrappers), which is not
			-- SDK-versioned and so does not derive from it. Both
			-- default to /usr/local/freesce. A premake-time choice
			-- because the 2.9 and 3.2 C++ ABIs don't link: one
			-- flavor per generated tree. The wrappers also strip
			-- premake's gcc-3-style dependency flags, which the
			-- 2.9 driver rejects.
			gccprefix '../tools/freesce/ee-'
			buildoptions { "-fno-common", "-fno-exceptions", "-mno-abicalls", "-G0" }
			makesettings [[
FREESCE ?= /usr/local/freesce
FREESCE_GCC ?= /usr/local/freesce/ee/gcc
export FREESCE_GCC
]]
			includedirs { "$(FREESCE)/ee/include" }
		else
			gccprefix 'ee-'
			buildoptions { "-nostdlib", "-fno-common" }
			includedirs { "$(PS2SDK)/ee/include", "$(PS2SDK)/common/include" }
		end
	filter { "platforms:*metal or *metal-cocoa" }
		defines { "RW_METAL" }
		cppdialect "gnu++14"
		buildoptions { "-target", "arm64-apple-macos14" }
		linkoptions { "-target", "arm64-apple-macos14" }
	filter { "platforms:*metal" }
		if os.istarget("macosx") and _OPTIONS["gfxlib"] ~= "glfw" then
			premake.warn("--gfxlib=%s is ignored: the Metal platform always uses GLFW", _OPTIONS["gfxlib"])
		end
		defines { "LIBRW_GLFW" }
		includedirs { path.join(HomebrewPrefix, "include") }
		libdirs { path.join(HomebrewPrefix, "lib") }
	filter { "platforms:*metal-cocoa" }
		defines { "LIBRW_COCOA" }

	filter { "platforms:*amd64*" }
		architecture "x86_64"
	filter { "platforms:*x86*" }
		architecture "x86"
	filter { "platforms:*arm-*" }
		architecture "ARM"
	filter { "platforms:*arm64*" }
		architecture "ARM64"

	filter { "platforms:win*" }
		system "windows"
	filter { "platforms:linux*" }
		system "linux"
	filter { "platforms:macosx*" }
		system "macosx"

	filter { "platforms:win*gl3" }
		includedirs { path.join(_OPTIONS["sdl2dir"], "include") }
	filter { "platforms:win-x86-gl3" }
		includedirs { path.join(_OPTIONS["glfwdir32"], "include") }
	filter { "platforms:win-amd64-gl3" }
		includedirs { path.join(_OPTIONS["glfwdir64"], "include") }

	filter "action:vs*"
		buildoptions { "/wd4996", "/wd4244" }

	filter { "platforms:win*gl3", "action:not vs*" }
		if _OPTIONS["gfxlib"] == "sdl2" then
			includedirs { "/mingw/include/SDL2" } -- TODO: Detect this properly
		end

	filter {}

	Libdir = "lib/%{cfg.platform}/%{cfg.buildcfg}"
	Bindir = "bin/%{cfg.platform}/%{cfg.buildcfg}"

function vucode()
	-- with --freesce, its own dvp-as by its root, not from PATH
	local dvpas = _OPTIONS["freesce"]
		and '$(or $(FREESCE_GCC),/usr/local/freesce/ee/gcc)/bin/ee-dvp-as'
		or 'ee-dvp-as'
	filter "files:**.dsm"
		buildmessage 'dvp-as %{file.name}'
		buildcommands {
			'cpp -x assembler-with-cpp "%{file.abspath}" | ' .. dvpas .. ' -I "%{file.directory}" -o "%{cfg.objdir}/%{file.basename}.o"'
		}
		buildoutputs { '%{cfg.objdir}/%{file.basename}.o' }
	filter {}
end

project "librw"
	kind "StaticLib"
	targetname "rw"
	targetdir (Libdir)
	defines { "LODEPNG_NO_COMPILE_CPP" }
	files { "src/*.*" }
	files { "src/*/*.*" }
	filter { "platforms:*gl3" }
		files { "src/gl/glad/*.*" }
        vucode()
        filter { "platforms:ps2" }
                files { "src/ps2/vu1/*.dsm" }
	filter { "platforms:not *metal", "platforms:not *metal-cocoa" }
		removefiles { "src/metal/**" }
	filter { "files:**.mm" }
		compileas "Objective-C++"
		buildoptions { "-fobjc-arc" }
	filter {}


project "dumprwtree"
	kind "ConsoleApp"
	targetdir (Bindir)
	removeplatforms { "*gl3", "*d3d9", "ps2" }
	files { "tools/dumprwtree/*" }
	includedirs { "." }
	libdirs { Libdir }
	links { "librw" }
	removeplatforms { "macosx*" }

function findlibs()
	filter { "platforms:linux*gl3" }
		links { "GL" }
		if _OPTIONS["gfxlib"] == "glfw" then
			links { "glfw" }
		elseif _OPTIONS["gfxlib"] == "sdl2" then
			links { "SDL2" }
		elseif _OPTIONS["gfxlib"] == "sdl3" then
			links { "SDL3" }
		end
	filter { "platforms:win-amd64-gl3" }
		libdirs { path.join(_OPTIONS["glfwdir64"], "lib-vc2015") }
		libdirs { path.join(_OPTIONS["sdl2dir"], "lib/x64") }
		libdirs { path.join(_OPTIONS["sdl3dir"], "lib/x64") }
	filter { "platforms:win-x86-gl3" }
		libdirs { path.join(_OPTIONS["glfwdir32"], "lib-vc2015") }
		libdirs { path.join(_OPTIONS["sdl2dir"], "lib/x86") }
		libdirs { path.join(_OPTIONS["sdl3dir"], "lib/x86") }
	filter { "platforms:win*gl3" }
		links { "opengl32" }
		if _OPTIONS["gfxlib"] == "glfw" then
			links { "glfw3" }
		elseif _OPTIONS["gfxlib"] == "sdl2" then
			links { "SDL2" }
		elseif _OPTIONS["gfxlib"] == "sdl3" then
			links { "SDL3" }
		end
	filter { "platforms:*d3d9" }
		links { "gdi32", "d3d9" }
	filter { "platforms:*d3d9", "action:vs*" }
		links { "Xinput9_1_0" }
	filter {}
end

function skeleton()
	files { "skeleton/*.cpp", "skeleton/*.h" }
	files { "skeleton/imgui/*.cpp", "skeleton/imgui/*.h" }
	includedirs { "skeleton" }
end

function skeltool(dir)
	targetdir (Bindir)
	files { path.join("tools", dir, "*.cpp"),
	        path.join("tools", dir, "*.h") }
	vpaths {
		{["src"] = { path.join("tools", dir, "*") }},
		{["skeleton"] = { "skeleton/*" }},
	}
	skeleton()
	debugdir ( path.join("tools", dir) )
	includedirs { "." }
	libdirs { Libdir }
	links { "librw" }
	findlibs()
end

project "playground"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("playground")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" } -- for now
	removeplatforms { "macosx*" }

project "imguitest"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("imguitest")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

project "lights"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("lights")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

project "subrast"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("subrast")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

project "camera"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("camera")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

project "im2d"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("im2d")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

project "im3d"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("im3d")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

project "demoreel"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("demoreel")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" } -- for now
	removeplatforms { "macosx*" }

project "clumpview"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("clumpview")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" } -- has its own Makefile
	removeplatforms { "macosx*" }

project "ska2anm"
	kind "ConsoleApp"
	characterset ("MBCS")
	targetdir (Bindir)
	files { path.join("tools/ska2anm", "*.cpp"),
	        path.join("tools/ska2anm", "*.h") }
	debugdir ( path.join("tools/ska2nm") )
	includedirs { "." }
	libdirs { Libdir }
	links { "librw" }
	findlibs()
	removeplatforms { "*gl3", "*d3d9", "*ps2" }
	removeplatforms { "macosx*" }

--project "ps2test"
--	kind "ConsoleApp"
--	targetdir (Bindir)
--	vucode()
--	removeplatforms { "*gl3", "*d3d9", "*null" }
--	targetextension '.elf'
--	includedirs { "." }
--	files { "tools/ps2test/*.cpp",
--	        "tools/ps2test/vu/*.dsm",
--	        "tools/ps2test/*.h" }
--	libdirs { "$(PS2SDK)/ee/lib" }
--	links { "librw" }

--project "ps2rastertest"
--	kind "ConsoleApp"
--	targetdir (Bindir)
--	removeplatforms { "*gl3", "*d3d9" }
--	files { "tools/ps2rastertest/*.cpp" }
--	includedirs { "." }
--	libdirs { Libdir }
--	links { "librw" }

project "hopalong"
	kind "WindowedApp"
	characterset ("MBCS")
	skeltool("hopalong")
	entrypoint("WinMainCRTStartup")
	removeplatforms { "*null" }
	removeplatforms { "ps2" }
	removeplatforms { "macosx*" }

if os.istarget("macosx") then
	function metalpuretest(name, source)
		project(name)
			kind "ConsoleApp"
			targetdir (Bindir)
			optimize "Off"
			undefines { "NDEBUG" }
			buildoptions { "-Wall", "-Wextra" }
			includedirs { "src/metal" }
			files { path.join("tests/metal/pure", name .. ".cpp") }
			if source then
				files { path.join("src/metal", source) }
			end
	end

	metalpuretest("metal_fan_test", "metalfan.cpp")
	metalpuretest("metal_format_test", "metalformat.cpp")
	metalpuretest("metal_inst_test", "metalinst.cpp")
	metalpuretest("metal_keys_test", "metalkeys.cpp")
	metalpuretest("metal_pass_test", "metalpass.cpp")
	metalpuretest("metal_modes_test", nil)
	metalpuretest("metal_drawable_test", nil)

	project "metal_smoke"
		kind "ConsoleApp"
		targetdir (Bindir)
		optimize "Off"
		symbols "On"
		buildoptions { "-O1", "-Wall" }
		includedirs { "." }
		files { "tests/metal/smoke/*.cpp", "tests/metal/smoke/*.h", "tests/metal/smoke/*.mm" }
		libdirs { Libdir }
		filter { "platforms:*metal" }
			links { "librw", "glfw", "Metal.framework", "QuartzCore.framework", "Cocoa.framework" }
		filter { "platforms:*metal-cocoa" }
			links { "librw", "Metal.framework", "QuartzCore.framework", "AppKit.framework" }
		filter {}
		if _OPTIONS["metal-asan"] then
			targetsuffix "_asan"
			objdir "build/obj-asan"
			buildoptions { "-fsanitize=address", "-fno-omit-frame-pointer" }
			linkoptions { "-fsanitize=address" }
		end
		filter { "files:**.mm" }
			compileas "Objective-C++"
			buildoptions { "-fobjc-arc" }
		filter {}
end
