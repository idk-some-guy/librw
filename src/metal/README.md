# Metal backend

`rw::metal` renders through Apple's Metal API. It sits beside the gl3 and d3d9 backends, is compiled when `RW_METAL` is defined, and gets its window from a host table: a GLFW host, or a native Cocoa host.

## Platform

- Apple silicon Macs, macOS 14 or later. The build targets `arm64-apple-macos14`.
- GLFW 3 (Homebrew `glfw`) for the GLFW window host, with `LIBRW_GLFW` defined. The Cocoa host needs only the system frameworks.
- Xcode command line tools. Shaders are compiled at start-up; no offline shader tools are needed.
- Build with premake (see "Build"). The CMake files have no Metal platform. Two platforms: `macosx-arm64-metal` with the GLFW host, and `macosx-arm64-metal-cocoa` with the Cocoa host and no GLFW. Premake warns that `--gfxlib` is ignored if it names another library. A host can pass its own window host instead (see "Window hosts").

## Window hosts

The device owns no window. `EngineOpenParams::host` points to a `MetalHost` table (`rwmetal.h`) of thirteen functions that open the window system, describe the displays and create the surface the backend presents to. When `host` is `nil` the device uses `metal::glfwHost`, which exists only when `LIBRW_GLFW` is defined and writes the `GLFWwindow` it creates through `EngineOpenParams::window`. Without `LIBRW_GLFW`, a nil `host` selects `metal::cocoaHost` when `LIBRW_COCOA` is defined; without either, the device reports "no window host".

The library and every host must be built with the same `LIBRW_GLFW` and `LIBRW_COCOA` settings. The `window` and `cocoaWindow` fields of `EngineOpenParams` exist only under them, so the layout of the struct depends on them.

The device calls the table from the engine's device requests and from `showRaster`, so every call comes from the render thread, the thread that drives the engine. The GLFW host needs that to be the main thread, as GLFW does. The Cocoa host needs the main thread too, as AppKit does, and fails `open` on any other. A host is a singleton: it keeps one window system and one surface in its own state, and the device uses one host at a time.

### Entries

| Entry | Called | Must |
|---|---|---|
| `open(params)` | `Engine::open`, before any other entry except `close` | Start the window system and read `width`, `height`, `windowtitle` and `hidden`. Report a failure with `RWERROR` and return 0. |
| `close()` | `Engine::close`, and `Engine::open` when `getModes(0)` lists nothing | Shut the window system down. `Engine::close` calls it even when `open` failed or never ran, so it must tolerate both. |
| `numDisplays()` | `Engine::open`, `Engine::setSubSystem`, `Engine::getSubSystemInfo` | Return the number of displays, 0 when there are none. It must work before `open` succeeds and after `close`. |
| `displayName(i)` | `Engine::getSubSystemInfo`, with `i` below the last `numDisplays()` | Return the display's name, never `nil`. Like `numDisplays`, it must work after `close`. |
| `displayMode(i, &mode)` | not called by the device | Fill the display's current mode with `flags` 0; return 0 for an unknown display. |
| `getModes(i, &n)` | `Engine::open`, display 0 only | Return the mode list and its length. After a successful `open` the list for display 0 is never empty. |
| `createSurface(display, mode, windowed, hidden)` | `Engine::start` | Create the surface and return its `CAMetalLayer` as a `void *`, or report with `RWERROR` and return `nil`. See "Surfaces". |
| `destroySurface()` | `Engine::stop` | Release the surface. The device has already finished its GPU work and dropped the layer. |
| `drawableSize(&w, &h)` | `showRaster`, after `pollSizeChange()` returns true | The surface size in pixels, 0 by 0 when unknown. |
| `backingScale()` | `showRaster`, after `pollSizeChange()` returns true | The scale from points to pixels, 0 when unknown. |
| `refreshRate()` | `showRaster`, on a frame that sleeps without a drawable | The display's refresh rate in Hz, 0 when unknown (the device then uses 60). |
| `visible()` | `showRaster`, every frame with a surface | See "Hidden surfaces". |
| `pollSizeChange()` | `showRaster`, every frame with a surface | Return true once after the size or scale changed, counting from `createSurface`. The device then reads `drawableSize` and `backingScale` and updates the layer. |

Across the table, 0 means none or unknown: a count, a size, a scale or a rate of 0, and a mode `refresh` of 0.

### Modes and displays

The host owns the mode arrays and the display names. A list stays valid until the next `getModes` or `close`, and a name until the next call; the device copies both at once. Entry 0 of a list is the windowed mode, the display's current mode with `flags` 0. Every other entry carries `VIDEOMODEEXCLUSIVE`.

Displays are indexes, and display 0 is the main display. They are not stable: both hosts read the display list again on every call, so connecting or removing a display can reorder them. The device takes its video modes from display 0 at `Engine::open`. The subsystem chosen with `Engine::setSubSystem` only picks the display a fullscreen surface goes to; `Engine::open` resets that fullscreen target to display 0, while the current subsystem that `Engine::getCurrentSubSystem` reports keeps its value.

### Surfaces

`createSurface` gets the chosen display and `mode`, an index into the list the host returned last, whatever display is passed. `windowed` repeats the mode's flag: it is true when the mode lacks `VIDEOMODEEXCLUSIVE`. `hidden` is the open parameter. A windowed or hidden surface takes its size from the open parameters' `width` and `height`; a fullscreen one takes the mode's size on the given display.

The host returns the layer without transferring ownership. It keeps the layer attached to its view, with `contentsScale` and `drawableSize` set, until `destroySurface`. The device sets the Metal device, pixel format, `framebufferOnly`, drawable count and `displaySyncEnabled`, and afterwards writes `contentsScale` and `drawableSize` itself when `pollSizeChange` reports a change.

### Hidden surfaces

`visible()` is false while the surface is minimised or hidden, for example with cmd-H. A surface created with `hidden` set counts as visible while it is not minimised: it renders and presents. The GLFW host does not consider occlusion, so a window behind others counts as visible; the Cocoa host does.

While the surface is not visible the device acquires no drawable and composites nothing. The camera still renders, and `showRaster` still finishes and counts the frame, which the statistics line reports under "frames without drawable". A frame shown with `FLIPWAITVSYNCH` then sleeps one refresh, and any other frame does not sleep. A visible surface that gets no drawable sleeps one refresh whatever the flags. While the application is hidden, macOS can stretch these sleeps well past one refresh.

### The Cocoa host

Define `LIBRW_COCOA` (the `macosx-arm64-metal-cocoa` platform does), link `AppKit`, `QuartzCore` and `Metal`, and pass `&metal::cocoaHost` as `host`, or leave `host` nil in a build without `LIBRW_GLFW`. The host writes the `NSWindow *` it creates, unretained, through `EngineOpenParams::cocoaWindow`, and writes `nil` there when it closes the window.

- Application. If `NSApp` exists when `open` runs, the host uses it as it is. Otherwise it creates it and sets the activation policy: accessory for a hidden surface, regular for a shown one, which it also launches and activates when it shows the window. Create `NSApp` yourself before `Engine::open` to keep its policy, menu and delegate your own.
- Events and input are the application's. The host runs no event loop and handles no keyboard, mouse or pad. Pump events once per frame on the main thread, for example with `nextEventMatchingMask:untilDate:inMode:dequeue:` and `sendEvent:` in an `@autoreleasepool`; without it, occlusion, backing-scale and fullscreen changes never arrive. The host observes the window through notifications and leaves its delegate to the application.
- Autorelease pools. The device calls every host entry inside an `@autoreleasepool`. An application that calls AppKit itself without a run loop (no `[NSApp run]`) provides its own pools around those calls, or the objects AppKit autoreleases there, windows included, are never released.
- Displays are `NSScreen.screens`, display 0 being the screen with the menu bar, named by `localizedName`. Modes come from `CGDisplayCopyAllDisplayModes`, in pixels: the current mode first, then one entry per usable size up to the display's native size, sorted by width then height, each at its highest refresh rate (0 when the display reports none), depth 32. The native size is always listed.
- Surface. One window, titled, closable, miniaturisable and resizable, whose content view is layer-backed with a `CAMetalLayer`. The layer's `contentsGravity` is `kCAGravityResizeAspect` over a black background. A windowed or hidden surface has the view's size in pixels as its drawable size. A fullscreen surface (an exclusive mode, not hidden) has the mode's size, capped at the native size, as its drawable size; the host enters the window's native fullscreen with `toggleFullScreen:` and the layer scales the drawable to the screen with black bars where the aspect differs. There is no display mode switch.
- Fullscreen timing. `toggleFullScreen:` is asynchronous and `createSurface` does not wait for it; the drawable has the mode's size from the start. Activation is a request that macOS may decline. When the window leaves its fullscreen Space, the drawable returns to the view's size in pixels; a windowed surface that the user takes fullscreen keeps following the view.
- Visibility. `visible()` is false while the window is miniaturised or fully occluded, which includes another Space, cmd-H and a sleeping display. A surface created hidden is never ordered in and counts as visible unless miniaturised.
- Size changes. `pollSizeChange()` is true once after the view resized, its backing scale or screen changed, or the window entered or left fullscreen, and whenever the drawable size or scale differs from the last report.
- Teardown. `destroySurface` orders the window out and closes it. AppKit releases the window-server window only at the next event pump after that.

## Scope

The backend draws what the gl3 backend draws:

- im2d, including a two-uv variant for host shaders, and im3d;
- the default pipeline with ambient, directional, point and spot lights (up to eight);
- skinned geometry (up to 64 bones) and matfx environment maps;
- camera rasters, camera textures, sub-raster cameras and depth rasters, with frame-buffer copies into camera textures;
- native D3D8, D3D9 and Xbox textures, including DXT, paletted formats and mip maps;
- readback of camera rasters and depth, and screenshots;
- multisampling;
- engine restarts (`Engine::stop` and `Engine::start`) with a new video mode or sample count.

It was compared with the gl3 backend inside a host application. The differences that remain are listed under "Differences from gl3".

## Design

- The camera raster is an offscreen `RGBA8Unorm` texture. Only a composite pass in `showRaster` writes the drawable (`BGRA8Unorm`). Depth is `Depth32Float_Stencil8`.
- The engine's OpenGL maths stays: projection matrices map depth to -1..1, and every vertex function in `shaders/` converts depth once, as its last step, with `MetalDepth`.
- Textures are stored top-down, camera textures included. Shaders sample `v` as given, with no `1 - v`.
- Texture addressing is what the application sets. When the addressing render state changes while a texture is bound, the gl3 backend applies it only if the texture already has that mode (`setAddressU` and `setAddressV` in `src/gl/gl3device.cpp`), so the texture keeps the mode it had. Define `RW_METAL_GL3_ADDRESSING` to reproduce the gl3 behaviour, for comparisons.
- Shaders are hand-written Metal Shading Language in `shaders/*.metal`, embedded as strings in `shaders/*_metal.inc` (rebuilt by `make -C src/metal/shaders` after a `.metal` file changes) and compiled at start-up with `preserveInvariance` on. Lighting and alpha-test variants are function constants.
- Vertex positions are `[[position, invariant]]`, so passes that must land on the same depth get the same result when they use the same expression (see the host contract).
- Pipeline states are built when the engine starts: 51 engine states at one sample, and each of them again at the engine's sample count when multisampling is on. A state built later logs `created after init`.
- The default, matfx and skin object pipelines live for the whole process. Loaded atomics keep pointing at them across an engine restart. Shaders, pipeline states and uniform buffers are rebuilt at every start.
- Headers are plain C++. Metal objects are held as `void *` and used only in `.mm` files compiled with ARC.

### Sample counts

- `Engine::setMultiSamplingLevels(n)` after `Engine::open` and before `Engine::start` asks for `n` samples. The backend uses the largest power of two that is not above `n`, the device maximum, or 8. `Engine::getMaxMultiSamplingLevels()` returns the device maximum; `Engine::getMultiSamplingLevels()` returns the count in use.
- If the clear pipelines cannot be built at a count, the maximum drops to the previous count and a line says so.
- Camera rasters are multisampled at that count and resolved at the end of every pass, so everything that reads them (copies, readback, the composite, sampling) sees the resolved texture. Camera textures and their depth rasters stay at one sample. A depth raster gets a multisampled twin the first time it is attached beside a multisampled colour raster; its depth is resolved only when read back.
- Every engine and host prewarm runs at one sample and at the count in use.

## Host pipeline contract

A host can add its own object pipelines and shaders without changing the backend. `defaultRenderCB` in `metalrender.cpp` and the matfx and skin pipelines are the reference.

1. **Shader source.** `Shader::create(src, vsName, fsName, variantMask)` joins a nil-terminated array of strings and compiles it as one library. Start the array with `header_metal_src`, which declares the attribute and buffer indices, the function constants, the `Scene`, `Object`, `Lights`, `Material` and `State` blocks, `VertexOut`, `FragmentIn`, and the helpers `DoDynamicLight`, `DoFog`, `MetalDepth` and `DoAlphaTest`. Add `default_metal_src` (`defaultVS`), `skin_metal_src` (`Skin` block, `skinVS`) or `simple_metal_src` (`simpleFS`) to reuse those functions, and do not declare those names again. Name the material block parameter `material` and the state block parameter `state`: the `surf*` and `u_fog*` macros use those names. Host shader ids start after the engine's five.

2. **Buffer and attribute indices.**

   | Buffer | Index | Set by |
   |---|---|---|
   | vertex | 0 | instancing |
   | `Scene` | 1 | camera |
   | `Object` | 2 | `setWorldMatrix` |
   | `Material` | 3 | `setMaterial` |
   | `State` | 4 | render states (alpha reference, fog) |
   | `Skin` | 5 | `uploadSkinMatrices` |
   | custom | 6 | `setCustomConstants` |
   | `Lights` | 7 | `setLights` through `lightingCB` |
   | `MatFX` | 8 | matfx pipeline |

   Attributes: position 0, normal 1, colour 2, weights 3, indices 4, texture coordinates 5 onward (`ATTRIB_TEXCOORDS0` to `ATTRIB_TEXCOORDS7`).

   A pipeline binds only the blocks its functions read, found by reflection. A block shorter than the shader's declaration is bound at the shader's size with zeros after the data. If an engine block's size differs from the shader's, the backend logs `uniform block <n> is <a> bytes, shader expects <b>` and counts a block mismatch.

3. **Custom block.** `setCustomConstants(data, size)` copies up to 1024 bytes (`MAXCUSTOMCONSTANTS`) into buffer 6. It is bound only for pipelines that read buffer 6, zero-padded to the shader's size, and stays set until it is replaced or cleared with `setCustomConstants(nil, 0)`. Every call marks the block for upload, and the next draw that reads it copies it into a 256-byte-aligned slice of the uniform ring, so set it once per atomic or when it changes. A shader whose custom block is declared larger than 1024 bytes cannot build its pipelines: the error is reported once per pipeline key and its draws are dropped.

4. **Variants.** Variant bits are function constants: alpha test is bit 0 (`VARIANT_ALPHATEST`), directional lights bit 1, point lights bit 2, spot lights bit 3. `variantMask` in `Shader::create` limits which bits a shader builds; others are cleared. Select the variant per mesh with `shader->use(drawVariant(vsBits))`, where `vsBits` comes from `lightingCB(atomic)`, right before `drawInst` and after the mesh's render states are set, because `drawVariant` reads the current alpha-test state. Alpha test is on exactly when alpha blending is on, as in gl3.

5. **Positions.** Any draw that must produce the same depth as another pass (a second pass with an equal depth test, a decal over geometry) writes its position with the engine's expression, keeps `[[position, invariant]]` on the output, and converts depth last:

   ```metal
   float4 V = object.world * float4(in.pos, 1.0);
   out.position = scene.proj * scene.view * V;
   // lighting, fog, texture coordinates
   out.position = MetalDepth(out.position);
   ```

   `skinVS` computes `V` from blended bone positions, so skinned and unskinned draws are not guaranteed to match each other.

6. **Vertex layouts.** `defaultInstanceCB` and `skinInstanceCB` build the layouts that `defaultVertexAttribs(normals, prelit, numTexCoordSets, out)` and `skinVertexAttribs(...)` describe. `out` needs `MAXVERTEXATTRIBS` (13) entries. Pass the same descriptions to `prewarmShader`.

7. **Prewarm.** After every `Engine::start`, call `prewarmShader(shader, attribs, numAttribs, variant, blend, srcBlend, destBlend, depth)` for each layout, variant and blend state the host draws. `srcBlend` and `destBlend` take `rw::BlendFunction` values; `depth` selects a D32S8 depth attachment. The backend assumes RGBA8 colour, which every camera raster and camera texture uses, and builds each state at one sample and at the count in use. Im2d override shaders use `prewarmIm2DShader(shader, uv2, blend, srcBlend, destBlend, depth)`. Prewarmed states count as `host pipelines`. A state built during drawing logs `rw::metal: pipeline <key> created after init` and counts as a `late pipeline`: add the missing case to the prewarm list.

8. **Engine restart.** `Engine::stop` clears the pipeline-state cache, so host prewarms are lost. Destroy host shaders before stopping the engine; create them and prewarm again after `Engine::start`. Keep host `ObjPipeline`s for the life of the process when loaded atomics hold them across a restart, as the engine does with its own; destroy only the shaders. (The gl3 backend destroys its matfx and skin pipelines at close, which leaves loaded atomics with freed pipelines after a restart.)

9. **Textures.** `setTexture(n, tex)` binds stage `n` (0 to 7) to `texture(n)` and `sampler(n)`, with the texture's filter and addressing. All textures are top-down, including camera textures: drop any `1.0 - v` a GLSL version of the shader has for render targets.

10. **Feedback.** A draw that samples the raster it renders into, or the depth raster it renders with, is dropped and reported once. To read the current frame, copy it into a camera texture first and sample that.

11. **Im2d override.** Set `im2dOverrideShader` to draw im2d through a host shader, and set it back to nil afterwards. `im2DRenderIndexedPrimitiveUV2` with `Im2DVertexUV2` vertices needs an override shader; without one the draw is dropped and counted. `im2d_metal_src` and `im2d_uv2_metal_src` export the engine's im2d vertex functions.

A minimal host pipeline:

```cpp
static rw::metal::Shader *glowShader;

static void
glowRenderCB(rw::Atomic *atomic, rw::metal::InstanceDataHeader *header)
{
	using namespace rw;
	uint32 flags = atomic->geometry->flags;
	metal::setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = metal::lightingCB(atomic);
	metal::setCustomConstants(&glowParams, sizeof(glowParams));
	metal::setupVertexInput(header);
	metal::InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++, inst++){
		Material *m = inst->material;
		metal::setMaterial(flags, m->color, m->surfaceProps);
		metal::setTexture(0, m->texture);
		SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		glowShader->use(metal::drawVariant(vsBits));
		metal::drawInst(header, inst);
	}
	metal::teardownVertexInput(header);
	metal::setCustomConstants(nil, 0);
}

void
glowStart(void)
{
	using namespace rw;
	const char *src[] = { metal::header_metal_src, glow_metal_src, nil };
	metal::AttribDesc attribs[metal::MAXVERTEXATTRIBS];
	glowShader = metal::Shader::create(src, "glowVS", "glowFS",
		metal::VARIANT_ALPHATEST | metal::VARIANT_DIRECTIONALS);
	if(glowShader == nil)
		return;
	int32 n = metal::defaultVertexAttribs(1, 0, 1, attribs);
	metal::prewarmShader(glowShader, attribs, n, 0, 0, 0, 0, 1);
	metal::prewarmShader(glowShader, attribs, n, metal::VARIANT_DIRECTIONALS, 0, 0, 0, 1);
	metal::prewarmShader(glowShader, attribs, n, metal::VARIANT_ALPHATEST, 1, BLENDSRCALPHA, BLENDINVSRCALPHA, 1);
}

void
glowStop(void)
{
	if(glowShader)
		glowShader->destroy();
	glowShader = nil;
}
```

`glowParams` (the host's custom block) and `glow_metal_src` (its shader source, defining `glowVS` and `glowFS`) belong to the host. Create the `ObjPipeline` once per process (`metal::ObjPipeline::create()`, with `defaultInstanceCB`, `defaultUninstanceCB` and `glowRenderCB`), call `glowStart` after every `Engine::start`, and `glowStop` before every `Engine::stop`.

## Log lines

The backend writes these lines to stderr, each starting with `rw::metal:`. Errors such as a pipeline that fails to build, an oversized custom block or a texture that cannot be created go through librw's error mechanism (`RWERROR`) instead.

- `prewarm <n> pipelines in <t> ms` at every start.
- `pipeline <key> created after init` for each state built during drawing.
- `draw dropped, <cause> (shader <vs>/<fs>)` once per cause and shader, or `draw dropped, <cause> (shader none)` when no shader is set. Causes: no render encoder is open; no shader is set; its pipeline failed to build; no ring space for its uniform blocks; no render target is set; it samples the raster it renders into; a two-uv im2d draw has no override shader or layout.
- `uniform block <n> is <a> bytes, shader expects <b>`, `shader <vs>/<fs>: <compiler warnings>`, `command buffer error: <text>`, `clear pipelines at <n> samples failed; using at most <m> samples`.

### Statistics line

```
rw::metal: stats frames %u draws/frame %.1f ring peak %u ring grows %u late pipelines %u host pipelines %u skinned unrouted %u strip restarts %u staged uploads %u direct uploads %u mipmap blits %u gpu waits %u block mismatches %u dropped draws %u passes/frame %.1f copies/frame %.1f custom uploads/frame %.1f material uploads/frame %.1f frames without drawable %u drawable wait max %.1f ms host prewarm %.1f ms samples %u
```

The backend prints it at every `Engine::stop`, and every `n` seconds while frames are shown after the host calls `metal::setStatsInterval(n)`. The default interval is 0, which prints only the line at stop. Counters start again from zero at every `Engine::start`.

| Field | Meaning | Counted over |
|---|---|---|
| frames | frames shown | since start |
| draws/frame | draws per frame | interval |
| ring peak | largest uniform-ring use in one frame, KB | interval |
| ring grows | times the uniform ring grew | since start |
| late pipelines | states built during drawing | since start |
| host pipelines | states built by host prewarms | since start |
| skinned unrouted | skinned geometry drawn by a pipeline without skinning | since start |
| strip restarts | meshes instanced with strip restarts | since start |
| staged, direct uploads | texture uploads staged for textures in use, and written directly | since start |
| mipmap blits | mip chains generated on the GPU | since start |
| gpu waits | CPU waits for GPU work on a texture | since start |
| block mismatches | uniform block size mismatches | since start |
| dropped draws | draws dropped for any cause | since start |
| passes/frame, copies/frame | render passes and blit copies per frame | interval |
| custom, material uploads/frame | custom and material block uploads per frame | interval |
| frames without drawable | frames shown without a drawable | interval |
| drawable wait max | longest wait for a drawable | interval |
| host prewarm | time spent in host prewarms | since start |
| samples | sample count in use | now |

## Build

Install the prerequisites:

```
xcode-select --install
brew install premake glfw
```

The build scripts are tested with premake 5.0.0-beta8, which warns that `gmake2` was renamed to `gmake`; the warning is harmless. From the librw root:

```
premake5 gmake2
make -C build config=release_macosx-arm64-metal librw
```

The library is `lib/macosx-arm64-metal/Release/librw.a`. A host that builds librw itself defines `RW_METAL` and, for the GLFW host, `LIBRW_GLFW`, compiles `src/metal/*.mm` as Objective-C++ with `-fobjc-arc`, leaves `src/metal` out of other platforms, targets macOS 14, and links `Metal`, `QuartzCore`, `Cocoa` and `glfw`. Set `HOMEBREW_PREFIX` if Homebrew is not in `/opt/homebrew`. For the Cocoa host define `LIBRW_COCOA` instead of `LIBRW_GLFW` and link `AppKit` instead of `Cocoa` and `glfw`; build it with `make -C build config=release_macosx-arm64-metal-cocoa librw`, which writes `lib/macosx-arm64-metal-cocoa/Release/librw.a`.

## Tests

```
bash tests/metal/build.sh
bash tests/metal/run.sh
METAL_SMOKE_ASAN=1 bash tests/metal/build.sh && METAL_SMOKE_ASAN=1 bash tests/metal/run.sh
METAL_SMOKE_HOST=cocoa bash tests/metal/build.sh && METAL_SMOKE_HOST=cocoa bash tests/metal/run.sh
```

- `tests/metal/pure/` holds seven tests of the backend's pure rules (triangle fans, formats, instancing layouts, pipeline and state keys, sample counts, pass tracking, host mode lists, drawable sizes). They need no GPU.
- `tests/metal/smoke/` drives the whole backend on the GPU with a hidden window and reads pixels back. It needs a logged-in GUI session on a Mac with a GPU and a display. It runs under the Metal validation layer; `run.sh` fails if the layer does not load or reports an error. With the GLFW host it prints `PASS` or `FAIL` for each of its 273 checks; with `METAL_SMOKE_HOST=cocoa` it opens the Cocoa host instead, runs the same 273 checks and 7 more for the Cocoa window (280), and skips the pure tests. Both end with `all tests passed`, and both print the same 273 check names for the checks they share. The Cocoa window is never ordered in: nothing appears on screen.
- The Cocoa run covers only the hidden window. Showing a window, entering and leaving fullscreen and activating the application need a run with a shown window, and a Retina display needs a run on a display with a backing scale above 1.
- The ASan build instruments the test code, not the library. `METAL_SMOKE_ASAN=1` combines with either host.
- A host can add its own checks to the same run: compile `tests/metal/smoke/*.cpp` and `*.mm` except `no_host_checks.cpp`, and define `RunHostChecks(camera)` and `RunRestartHostChecks(restart, camera)` (`host_checks.h`). The driver calls the first after its render-target checks and the second after its restart render-target checks, before the multisampling checks, with the engine running.

## Changes to shared librw code

Against upstream `aap/librw` master. These fix every backend:

- `src/d3d/d3d.cpp`: `rasterToImage` wrote four bytes per palette index for paletted rasters; it now copies the index.
- `src/raster.cpp`: `Raster::convertTexToCurrentPlatform` left the source raster locked when `toImage` failed, and leaked the `Image` of every mip level above 0.
- `src/d3d/d3d8.cpp`, `d3d9.cpp`, `xbox.cpp`: native textures keep the mip level count the file gives, instead of the full chain, on every platform that reads them into `RasterLevels` (all but native D3D9).

These apply only when `RW_METAL` is defined:

- `src/raster.cpp`: conversion of native D3D8, D3D9 and Xbox textures to Metal, with DXT kept compressed; the alpha flag of a converted uncompressed texture comes from its level-0 texels.
- Platform registration in `rw.h`, `src/rwbase.h`, `base.cpp`, `charset.cpp`, `engine.cpp`, `geoplg.cpp`, `matfx.cpp`, `skin.cpp`, `texture.cpp`.

`src/CMakeLists.txt` exports `LIBRW_GLFW` as a public definition for GLFW builds, as `LIBRW_SDL2` already was. `premake5.lua` adds the `macosx-arm64-metal` and `macosx-arm64-metal-cocoa` platforms, leaves `src/metal` out of other platforms and the tool projects out of macOS, and adds the Metal test projects.

## Known defects outside the Metal backend

- `src/gl/gl3matfx.cpp` `matfxClose` and `src/gl/gl3skin.cpp` `skinClose` destroy the gl3 matfx and skin pipelines that loaded atomics still hold. After an engine restart those atomics use freed memory. The Metal backend keeps its pipelines for the process.
- `src/skin.cpp` `skinClose` clears `matFXGlobals.pipelines` slots where `skinGlobals.pipelines` was meant. Shared by all backends; not changed here.

## Differences from gl3

- Texture addressing follows the application (see Design). `RW_METAL_GL3_ADDRESSING` reproduces gl3.
- The alpha flag of a converted texture comes from mip level 0; gl3 uses the last level. An opaque converted C8888 texture stays C8888 on Metal, where gl3 makes it C888.
- Native textures keep the file's mip level count.
- The maximum sample count is the device's (4 on current Apple silicon GPUs); a larger request runs at the maximum.
- Skinned and unskinned positions are not guaranteed invariant with each other.
- The periodic statistics line appears only when the host sets an interval.

## Known limitations

- A shader whose custom block exceeds 1024 bytes reports once per pipeline key and its draws are dropped.
- Staged texture uploads split the current render pass.
- Geometry instanced before a device exists has no index buffer; its draws are skipped.
- A few vertex descriptors registered during drawing are autoreleased outside a pool and leak once each.
- A skinned atomic whose hierarchy has fewer nodes than the skin has bones draws with the bone count clamped and is reported once; the unmatched bones keep stale matrices, as on gl3.
- Writing to a multisampled camera raster through `lock` reaches only its single-sample texture.
- A depth raster used with colour rasters of different sample counts is not kept consistent between them.
- A multisampled depth raster keeps an extra single-sample texture (about 40 MB at 3840x2160).
- A new render target's pending clear runs as a pass of its own.
