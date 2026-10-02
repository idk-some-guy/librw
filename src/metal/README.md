# Metal backend

`rw::metal` renders through Apple's Metal API. It sits beside the gl3 and d3d9 backends, is compiled when `RW_METAL` is defined, and gets its window from a host table: a GLFW host, or a native Cocoa host.

## Platform

- Apple silicon Macs, macOS 14 or later. The build targets `arm64-apple-macos14`.
- GLFW 3 (Homebrew `glfw`) for the GLFW window host, with `LIBRW_GLFW` defined. The Cocoa host needs only the system frameworks.
- Xcode command line tools. Shaders are compiled at start-up; no offline shader tools are needed.
- Build with premake (see "Build"). The CMake files have no Metal platform. Two platforms: `macosx-arm64-metal` with the GLFW host, and `macosx-arm64-metal-cocoa` with the Cocoa host and no GLFW. Premake warns that `--gfxlib` is ignored if it names another library. A host can pass its own window host instead (see "Window hosts").

## Window hosts

The device owns no window. `EngineOpenParams::host` points to a `MetalHost` table (`rwmetal.h`) of thirteen functions that open the window system, describe the displays and create the surface the backend presents to. When `host` is `nil` the device uses `metal::glfwHost`, which exists only when `LIBRW_GLFW` is defined and writes the `GLFWwindow` it creates through `EngineOpenParams::window`. Without `LIBRW_GLFW`, a nil `host` selects `metal::cocoaHost` when `LIBRW_COCOA` is defined; without either, the device reports "no window host".

A native macOS application, one that runs its own `NSApplication`, menus and input, uses the Cocoa host, and nothing in its build links GLFW. The GLFW host is for applications that already open their window with GLFW, and for the smoke test's default configuration. Both hosts run the same 385 checks; the Cocoa host adds seven of its own.

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
| `surfaceScreen()` | `showRaster`, when the backend re-reads the EDR headroom | Return the `NSScreen` of the surface's window as a `void *`, or `nil`. The device reads the screen's EDR headroom and colour space from it. A table without the entry (`nil`) means no screen. Once a screen has been reported, a `nil` return keeps its headroom and colour space until a screen is reported again. |

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

- Application. If `NSApp` exists before the host's first `open`, the host uses it as it is. Otherwise it creates it and sets the activation policy: accessory for a hidden surface, regular for a shown one, which it also launches and activates when it shows the window. Create `NSApp` yourself before `Engine::open` to keep its policy, menu and delegate your own.
- Events and input are the application's. The host runs no event loop and handles no keyboard, mouse or pad. Pump events once per frame on the main thread, for example with `nextEventMatchingMask:untilDate:inMode:dequeue:` and `sendEvent:` in an `@autoreleasepool`; without it, occlusion, backing-scale and fullscreen changes never arrive. The host observes the window through notifications and leaves its delegate to the application.
- Autorelease pools. The device calls every host entry inside an `@autoreleasepool`. An application that calls AppKit itself without a run loop (no `[NSApp run]`) provides its own pools around those calls, or the objects AppKit autoreleases there, windows included, are never released.
- Displays are `NSScreen.screens`, display 0 being the screen with the menu bar, named by `localizedName`. Modes come from `CGDisplayCopyAllDisplayModes`, in pixels: the current mode first, then one entry per usable size up to the display's native size, sorted by width then height, each at its highest refresh rate (0 when the display reports none), depth 32. The native size is always listed. The list is read at every `Engine::open`, so a display connected, removed or rearranged while the engine runs changes it only at the next open. A window that moves to another display keeps its surface, and the next `pollSizeChange()` reports the new size and scale.
- Surface. One window, titled, closable, miniaturisable and resizable, whose content view is layer-backed with a `CAMetalLayer`. The layer's `contentsGravity` is `kCAGravityResizeAspect` over a black background. A windowed or hidden surface has the view's size in pixels as its drawable size. A fullscreen surface (an exclusive mode, not hidden) has the mode's size, capped at the native size, as its drawable size; the host enters the window's native fullscreen with `toggleFullScreen:` and the layer scales the drawable to the screen with black bars where the aspect differs. There is no display mode switch.
- Fullscreen timing. `toggleFullScreen:` is asynchronous and `createSurface` does not wait for it; the drawable has the mode's size from the start. Activation is a request that macOS may decline; an application that is active but still not in fullscreen can send `toggleFullScreen:` once more. When the window leaves its fullscreen Space, the drawable returns to the view's size in pixels; a windowed surface that the user takes fullscreen keeps following the view. `destroySurface` does not leave the Space. An application that restarts the engine from fullscreen, for example to change the render size, first sends `toggleFullScreen:` and pumps events until `NSWindowDidExitFullScreenNotification` arrives, then stops the engine; a window closed inside its Space may leave an empty Space behind.
- Visibility. `visible()` is false while the window is miniaturised or fully occluded, which includes another Space, cmd-H and a sleeping display. A surface created hidden is never ordered in and counts as visible unless miniaturised.
- Size changes. `pollSizeChange()` is true once after the view resized, its backing scale or screen changed, or the window entered or left fullscreen, and whenever the drawable size or scale differs from the last report.
- Teardown. `destroySurface` orders the window out and closes it. AppKit releases the window-server window asynchronously once events are pumped after the close.

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

1. **Shader source.** `Shader::create(src, vsName, fsName, variantMask)` joins a nil-terminated array of strings and compiles it as one library. Start the array with `header_metal_src`, which declares the attribute and buffer indices, the function constants, the `Scene`, `Object`, `Lights`, `Material` and `State` blocks, `VertexOut`, `FragmentIn`, and the helpers `DoDynamicLight`, `DoFog`, `MetalDepth` and `DoAlphaTest`. It also declares what the modern tier's light term uses: the function constants `relit` (6), `relitPrelit` (7) and `relitShadow` (8), the defines `BUFFER_NORMALSTREAM`, `BUFFER_FACEINDICES`, `BUFFER_FACEVERTICES`, `BUFFER_FACELAYOUT`, `BUFFER_RELIT` and `TEXTURE_SHADOW`, the `Relit` and `FaceFetch` blocks, the `VertexOut` and `FragmentIn` members `sun` (under `relit`) and `classic1`, `relitNormal` and `relitPos` (under `relitPrelit`), and the functions `DoSunLight`, `RelitClassic`, `RelitSunPart`, `faceFetchPos`, `faceFetchNormal`, `RelitSurfaceNormal`, `RelitShadowFactor`, `RelitShadowSun` and `RelitFragment`. A host fragment shader that wants the relit path declares the seven parameters `simpleFS` declares after its sampler. After its first two lines it holds the contents of `metalrelit.h` (`RLColor`, `RLVec3`, `RLSplit`, the `relit*` functions and the `RL_MIN`, `RL_MAX`, `RL_SQRT` and `RL_ABS` macros). Add `default_metal_src` (`defaultVS`), `skin_metal_src` (`Skin` block, `skinVS`) or `simple_metal_src` (`simpleFS`) to reuse those functions, and do not declare those names again. Name the material block parameter `material` and the state block parameter `state`: the `surf*` and `u_fog*` macros use those names. Host shader ids start after the engine's five.

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

   Shaders that opt into the relit variants must not bind vertex or fragment buffers 10 to 17 or texture 8 themselves.

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

## Render tiers

The backend renders in one of three tiers. `classic` is the parity path and changes nothing. `enhanced` and `modern` render the host's scene into a scene target, grade, bloom, upscale and composite it as described under "The frame". In this version `modern` renders the enhanced frame; its own features come later.

`setRenderSettings` takes a `RenderSettings` struct: `tier`, `upscale` (native, 1.5x, 1.7x, 2x, 3x), `dynamicResolution` (stored as off; not implemented), `hdrOutput` (auto or off), `emitterPeakNits` (400 to 4000) and `softness` (0 to 3). The backend clamps out-of-range values. The default is the classic tier. `getSupportedTier` reports what the device can run: enhanced needs GPU family Apple 7, modern also needs Apple 9 and hardware ray tracing. A request above the device's tier falls back, logs `rw::metal: tier <requested> requested, using <tier>; <requested> <reason>` once, and `getFrameInfo().tierReason` tells the host why.

Settings written while a frame is open take effect when that frame ends, so a frame never changes tier halfway through.

Every pipeline is keyed by its colour format. The classic tier prewarms the camera format only. The first switch to enhanced or modern prewarms the scene format (RGBA16F) and the overlay format (8-bit, alpha blended as coverage) for the backend's own layouts and for every host pipeline prewarmed so far; a later switch builds only the host pipelines added since.

## The frame

In the enhanced and modern tiers the host brackets its scene: `beginScene(camera)` before the frame's first clear, and `endScene(params)` where the scene ends. Between them, every pass on the camera's raster and depth raster draws into the frame's scene target instead: an RGBA16F camera raster at the internal resolution, with the device's MSAA twin, resolved at every pass end as the camera raster is. Values above 1.0 survive in it. `endScene` runs the grade compute pass from the resolved scene into the frame's output texture. From then on the camera raster is cleared to transparent black and collects what the host draws after the grade: HUD, menus and fades, blended with alpha as coverage (`ONE, ONE_MINUS_SOURCE_ALPHA` for the alpha channel of alpha-blended draws, `ZERO, ONE` for additive ones). The composite draws `output * (1 - overlay.a) + overlay.rgb`. A frame without `beginScene` is a classic frame. A scene shown without `endScene` is ended with a passthrough grade, and the backend logs it once.

A copy of the camera raster (`Raster::renderFast`) during a scene reads the scene colour, clamped to 8 bits; after `endScene` it reads the composite. `Raster::toImage` on the camera raster always reads the camera raster itself: the overlay after `endScene`, the last classic content before it.

In the enhanced and modern tiers the scene renders at the output size divided by the upscale factor (rounded to the nearest pixel), and MetalFX spatial scales the graded image to the output. Sub-raster viewports, sub-rect clears and copies of the camera raster are scaled to match. A scaler that cannot be created leaves the frame at native size with one log line. The overlay stays at the output size and composites after the upscale. A changed `upscale` setting takes effect at the next `beginScene`, without a device restart.

The spatial scaler sharpens, which overshoots above bright edges and leaves a dark ring below the background around small emitters. A dering pass runs after the scaler and only when it ran: each output channel is clamped to the minimum and maximum of the 3x3 graded texels around the texel nearest to the output pixel's centre (`metaldering.h`, shared with the kernel), and a NaN becomes the minimum. Its library is compiled with fast math off, so the NaN test holds on the GPU. It works in place on the output texture when the device has tier 2 read-write texture support. Otherwise the scaler writes into a second output-size RGBA16F texture, made on first use and dropped with the scene targets, and the pass copies it into the output. If that texture cannot be created, the backend logs once, sends the upscaled frame out unclamped and does not try again until the scene targets are rebuilt.

The grade is a compute kernel. Its library is `#include <metal_stdlib>`, `using namespace metal;`, then the host's grade source, then the backend's kernel. The host's source defines `static float4 hostGrade(float4 c, constant float4 *params)`; `params` holds up to 16 `float4` constants from `GradeParams`. The kernel hands `hostGrade` the scene colour clamped to 0..1 and rounded to 8 bits, rounds the result to 8 bits as a classic render target would, and adds the part of the colour above 1.0 multiplied by the grade's own linear gain, measured as `hostGrade(0.25) * 4`. A blend into the RGBA16F scene gives the value an 8-bit target would hold, except for emitters. Pipelines that blend into the scene leave the colour attachment's fixed-function blending off. Their fragment function reads the destination as `[[color(0)]]` and blends with the pipeline's own source and destination factors. A draw whose emissive scale (the `float` at `BUFFER_EMISSIVE`, 1.0 unless a draw sets it higher) is 1.0 clamps its colour, the destination and the result to 1.0, as an 8-bit target does, then adds back the destination's part above 1.0 multiplied by the destination factor. A translucent draw over an emitter therefore blends against white and lets the emitter's excess through by its destination factor: an alpha-0 texel leaves the emitter unchanged and an opaque one covers it. A draw with a scale above 1.0 skips every clamp and can rise above white. Opaque scene pipelines use a second variant that reads no destination and clamps the colour to 0..1 unless the draw's scale is above 1.0. The backend builds both variants from any shader's source when a scene pipeline first needs it, host shaders included: it adds the arguments to the fragment function and passes each `return` through the blend or the clamp. A fragment function it cannot rewrite (one that does not return `float4`) logs one line and blends with fixed-function blending, whose results do not saturate. With no emitters and no bloom, the only remaining difference from the classic frame is fp16 rounding, which the 8-bit rounding before and after the grade keeps within one step. The smoke test checks this at one and four samples and reports how many pixels differ.

`setGradeShader(src)` installs the host's grade; `nil` restores the passthrough. Call it on the render thread, between frames, after `Engine::open`; before that it returns 0 and logs nothing. A source that fails to compile leaves the current grade in place, logs the compiler's message and returns 0. Setting the source the backend already has, by content, compiles nothing and returns the earlier result, so a host may set its grade every frame. Constants past `numConstants` read as zero.

### Emitters

`setEmissiveScale(s)` marks the following im2D draws as emitters while `s` is above 1.0. In a scene, an emitter writes `colour * s` into the RGBA16F scene with its usual blending, so it rises above white; draws in front of it cover it as they cover anything. With `s` at 1.0, the default, an emitter draws what a plain draw does. `beginScene` sets the scale back to 1.0, so a host that leaves it raised lights one frame at most. The scale has no effect outside a scene (the 8-bit camera raster would clip it) or with an override shader.

The SDR composite leaves every pixel whose channels are all at or below 1.0 unchanged. A pixel above white moves every channel toward 1.0 by `(m - 1) / (m - 1 + 1)`, where `m` is its brightest channel, so a bright emitter turns white-hot instead of clipping one channel.

### Bloom

With a positive `GradeParams::bloomWeight`, `endScene` builds a six-level pyramid from the RGBA16F scene colour before the grade. The first level is a 13-tap downsample with Karis-weighted groups, so one very bright pixel cannot flood the glow. Each level is upsampled with a tent filter whose reach is `0.5 + 0.5 x softness` texels of the level below. The grade mixes the result into the scene by the weight (`mix(scene, bloom, weight)`), so the grade colours the glow. There is no threshold: everything blooms a little, emitters most. A weight of 0 skips the pyramid.

The bloom and grade kernels compile in relaxed math (`MTLMathModeRelaxed`): lossy optimisations stay, infinities and NaN are honoured. Each kernel reads the scene through `clamp(v, 0, 65504)`, which the Metal specification defines through `fmin` and `fmax`, so a NaN becomes 0, an infinity 65504 and a negative value 0, and one invalid pixel cannot spread through the bloom pyramid, the grade or the temporal history. The host's grade source compiles in the same mode. The dering kernel compiles in safe math; the other frame kernels in fast math. Before macOS 15, where the math mode cannot be set, relaxed falls back to safe math, which costs about 11% on the bloom passes and 8% on the grade pass.

### Output

With `hdrOutput` auto, the enhanced or modern tier, and a screen whose potential EDR headroom (`maximumPotentialExtendedDynamicRangeColorComponentValue`) is above 1.0, the layer becomes `rgba16Float` with `wantsExtendedDynamicRangeContent` and the screen's own colour space, extended and linearised (extended linear sRGB when the screen has none). The composite then writes the sRGB curve below white, so the frame matches the SDR frame there. Above white it writes `1 + (peak - 1) * e * s / ((peak - 1) + e * s)` for an excess `e`, with `s` the slope of the sRGB curve at 1.0. `peak` is `emitterPeakNits / 100`, clamped to the screen's current headroom (`maximumExtendedDynamicRangeColorComponentValue`). The backend re-reads the headroom when the host calls `displayChanged()`, when the window changes screen or size, and every 64 presents, and logs `rw::metal: EDR output on|off ...` when the layer changes. Otherwise the layer stays `BGRA8Unorm`, untagged, and the composite applies the SDR shoulder.

macOS reports a current headroom of 1.0 until a layer on the screen asks for EDR, so the first EDR frames have a peak of 1.0. At that peak the composite applies the SDR shoulder and linearises the result, so emitters look as they do in SDR until the next re-read finds the raised headroom. Between a peak of 1.0 and 1.25 the composite mixes the two results by `smoothstep(1.0, 1.25, peak)`, so a saturated emitter does not jump from near white to its full colour when the headroom crosses 1.0. The inverse transfer is always the sRGB curve. That is right for sRGB and Display P3 screens; on a screen profiled with another curve the shadows shift against the untagged SDR layer. A classic frame on an EDR layer, such as a menu, is linearised with the sRGB curve. The layer changes format only in `showRaster`, before the next drawable is taken.

### Anti-aliasing

With `antiAliasing` FXAA in the enhanced and modern tiers, the grade writes into an RGBA16F texture the frame owns, and an FXAA 3.11 quality kernel (edge thresholds 0.125 and 0.0312, a 12-step edge walk, subpixel blend 0.75) writes the result at the internal resolution where the grade wrote before: the texture the spatial scaler reads, or the output at native size. Luma is taken from the graded colour clamped to 0..1, so emitters count as white for edge detection and keep their values. A pixel below the contrast threshold is copied unchanged. The texture is made the first time FXAA runs and dropped with the scene targets, so a frame with anti-aliasing off allocates nothing for it. If it cannot be created, the backend logs once and renders without anti-aliasing, reported as off, until the scene targets are rebuilt. MSAA resolves in the scene before the grade and combines with FXAA. `ANTIALIAS_TAA` is stored as given. In the modern tier it selects the temporal scaler at a non-native upscale factor and the TAA resolve at native (see "The modern tier"). In the enhanced tier, or without the prepass shaders or motion kernels, it runs as FXAA, with one log line when it is requested. A modern camera without a depth raster also runs it as FXAA, without the line. The FXAA texture is dropped as soon as neither FXAA nor TAA runs. The classic tier never runs the pass. `FrameInfo::antiAliasing` and the stats line report the method in use.

## The modern tier

`metalprepass.h` and `metaltemporal.h` hold the pure rules of the prepass (which draws enter it, the draw key, the octahedral normal encoding, motion as previous minus current UV) and of the temporal path (an 8-phase Halton jitter in pixels, the projection jitter, the YCoCg neighbourhood clip, the compressed blend, the history weight). They compile as C++ for the pure tests and as MSL for the kernels.

`metalshadow.h` holds the shadow pass's pure rules and compiles as C++ and as MSL. It has the ShadowQuality table (off; low: one ray per pixel at half resolution, 16-frame history; medium: one ray per pixel, 12 frames; high: two rays per pixel, 8 frames), the per-pixel sample hash, uniform sampling of the sun's disc, the ray origin rule, the accumulation step, the history clamp, the disocclusion test, and the nearest-first cap used for the instance list. The ray starts at the surface point moved toward the camera by `shBias(distance)`, with no term along the normal: the segment from a visible point to the camera is free space, while the stored normal, turned toward the eye, points into the object at a smooth mesh's silhouette. Before the accumulation step, the history is clamped to the minimum and maximum of the raw visibility over the 3x3 neighbourhood at the ray resolution; a history the clamp moved by more than 0.5 restarts at a length of 1, so a moving caster's shadow does not smear over a static receiver. Two gates decide when the pass exists and when it runs. `shSourceOn(quality, rtSupported, modern)` is true whenever ShadowQuality is not off on a modern tier whose device supports ray tracing. `rtSupported` is a capability fixed for the session, never whether the acceleration structures were built this frame, so the gate does not follow per-frame state. `shPassRuns` also needs the frame's prepass, a registered sun above the horizon and a sun scale above 0. `shSunUp` takes the z of the direction toward the sun, the negation of the latched sun direction.

`metalrelit.h` holds the light term's arithmetic (the weight's prelit and live shares, the two-colour hemisphere and its desaturation, the split of a clamped vertex colour into the colour with the sun and the sun's part, and a shadow factor applied to the sun's part only) and is spliced into the shared shader header, so host shaders can use it. Shaders compile with fast math, under which `relitClampWeight` does not catch a NaN weight: clamp the weight on the CPU before uploading it. `metalnormals.cpp` converts strips to lists (degenerate triangles dropped, odd triangles flipped as the GPU draws them, the parity reset at a 0xFFFF restart) and derives vertex normals for geometry without them. It groups vertices by position (a 1/1024 unit grid), joins the faces around each position whose normals lie within 50 degrees of each other into clusters (`normalSmoothAngle`; an octagon stays smooth, a hexagon and a roof pitched 25 degrees or more stay crisp, with a margin for the noise in exported positions), and gives a vertex the area-weighted sum of its cluster, so copies of a vertex split at a texture seam get the same normal. A face whose area is below 1% of the largest face at a position takes no part in the joining there, so a sliver along a crease cannot link the two sides; its corners still get the normal of their cluster. A vertex whose own faces fall into more than one cluster (a box corner shared by three faces, a ridge shared by two slopes) gets no normal. `packNormals` stores each vertex in four signed normalised bytes: the normal in xyz (the nearest of the eight surrounding lattice points, within 0.33 degrees of the float normal) and a validity of 1 in w, or all zero for a vertex without a normal. `relitFillNormal(n, valid, g, fallback)` in `metalrelit.h` is the consumer rule. With `v` the interpolated validity clamped to 0..1, it returns `normalize(normalize(n) * v + g * (1 - v))`, with `n` turned to the side of `g`, `g` normalised (or `fallback` when it is zero), and `g` returned whenever the sum is zero. A triangle whose corners are all valid gets its normalised interpolated normal with no geometric normal mixed in, however much interpolation shortened it; one whose corners are all invalid gets the geometric normal; between them it blends, and the blend is continuous because the weight of the normalised part falls to zero with the validity.

In the modern tier the host may bracket a second submission of its opaque draws with beginPrepass and endPrepass after it opens the scene. Inside the bracket, an instanced draw with blending off and depth test and write on is drawn by a generic prepass pipeline into the scene depth, an RG16Snorm octahedral view-space normal and an RG16Float motion target at the internal resolution; every other draw is skipped. Its key (instance, world matrix, first bone) is recorded. After the bracket, a scene draw with the same key, drawn under the view and projection the prepass began with, tests depth equal without writing it; any other draw tests as before. A draw inside the bracket under another view and projection is skipped. A frame keys up to 8192 prepass draws; past that, a draw still enters the prepass but is not keyed, and its forward draw keeps the normal depth test. A prepass key with no forward draw by the end of the scene is counted as an orphan and logged once. Without the bracket a modern frame draws as an enhanced one. The prepass takes the pending depth and stencil clear of the scene; the scene colour clear stays pending for the first forward pass. That holds only when nothing draws into the scene before `beginPrepass`. A scene draw before it (a sky background, say) opens the scene pass, and the bracket then ends that pass, so the scene colour is stored and resolved and the first forward pass loads it and the depth again: one more pass, and a store and reload of every scene attachment. Hosts draw such a background after `endPrepass`. Routing to the scene targets applies only in the scene phase, so a camera begin-update or a target change inside the bracket sends draws, unkeyed, to the raw main colour: hosts must not switch targets inside the bracket. The cleared normal (0,0) decodes to +z, so consumers classify sky by a depth of 1, not by the normal. Forward vertex shaders must compute the position as scene.proj * scene.view * (object.world * pos) followed by MetalDepth, with [[position, invariant]]; skinned shaders as skinVS does.

The motion target holds the previous UV minus the current UV (y down). Object motion uses the previous frame's world matrix and bone palette of the object named by setWorldMatrix's object argument (an atomic); a nil object is treated as static. Pixels the prepass did not cover get the motion of the far plane, so the sky moves with the camera. cameraCut() makes the next frame start without camera history, and so does a prepass frame that does not directly follow another prepass frame. The previous matrices are kept for up to 8192 objects and 256 bone palettes per frame in tables reserved once; an object past that, seen for the first time, or not drawn in the previous prepass frame has no object motion. The previous view-projection is the unjittered one, so the motion target carries no jitter. A point whose previous position lies behind the camera gets a motion of (2, 2), which puts its previous UV off screen so that consumers drop its history. An atomic's history leaves the tables when the atomic is destroyed, so a new atomic at the same address starts without it. An object drawn more than once in one prepass frame keeps only its last world matrix as the next frame's previous one.

At more than one sample, the multisampled normal and motion targets are stored and a kernel after the prepass writes sample 0 of each into the resolved targets, the sample the depth resolve takes, so the depth, normal and motion of a pixel describe the same surface. A prepass split into more than one pass loads its targets when it reopens, so what the earlier passes drew is kept. The kernel runs at the end of every prepass pass. A frame whose prepass ran this kernel after at least one prepass draw has a valid prepass (`FrameInfo::prepassValid`); in any other modern frame the normal and motion targets hold an older frame's contents, and temporal consumers treat that frame and the next one as a cut.

An instance without normals gets a derived normal stream, built from its own CPU copies of positions and indices by the rule above and uploaded as four bytes per vertex at vertex buffer 13 (`BUFFER_NORMALSTREAM`). Such a draw uses a derived vertex layout whose normal attribute reads that buffer as `char4` normalised; derived layouts for the engine's layouts and for every layout a host prewarms are built and prewarmed with the prepass pipelines. Classic and enhanced draws never build, bind or read it. Building runs on the render thread under a budget of 1 ms per modern frame. The first prepass after `cameraCut` has a budget of 8 ms, and so does every prepass after it until each instance that was in the queue at the end of that frame has left the queue; instances queued later wait behind them and then build under the 1 ms budget. An instance created or reinstanced while the modern tier is active is queued (4096 places, reserved once), and the queue is drained under the budget when the next prepass begins. A prepass draw whose stream is missing builds it if the budget allows; otherwise it queues the instance, when there is room, and draws with its own layout: the missing normal reads (0,0,0,1), the fill returns the geometric normal and the draw shades flat until its stream exists. A queued instance that is freed leaves the queue. The stream is dropped with the instance or when its vertices are reinstanced. `InstanceStats` counts builds, queued instances, live streams and their bytes. The prepass fragment shader writes `relitFillNormal` of the interpolated normal and validity. For a draw with a derived stream it takes the geometric normal from the triangle itself: it reads the triangle's three indices at `[[primitive_id]]` (strip positions count degenerate triangles, as Metal numbers them) and their positions from the instance's buffers, and transforms the two edges to view space. Screen-space derivatives would not do: the GPU shades two triangles that share an edge in one 2x2 quad, so `dfdx` across that edge mixes the two planes, which put normals up to 90 degrees off along every crease. Other draws (authored or skinned normals, or a derived draw whose stream is not built yet) still use derivatives. Derived and vertex normals are transformed by the world matrix, not its inverse transpose: exact under uniform scale, approximate in direction under non-uniform scale.

In modern scene draws the world's prelit colour can be reweighted and live light added. A shader opts in with `VARIANT_RELIT` in its variant mask, and with `VARIANT_RELITPRELIT` as well if it draws prelit geometry without normals (`defaultShader` and `matfxEnvShader` take both, `skinShader` the first). The two function constants are `relit` (6) and `relitPrelit` (7). The `Relit` block (96 bytes, buffer 17) holds the sun's direction and colour, the sky's top and bottom colours, the eye position, the prelit weight, the hemisphere scale and saturation and the sun scale. `setRelitParams` fills it; the weight is clamped to 0..1 on the CPU (a NaN weight counts as 1, and a weight within 1e-6 of 1 counts as exactly 1, so a host blend that rounds a step below 1 still takes the plain variant), and colours and scales below zero count as zero. The sun is the light registered with `setSunLight`: its colour and its frame's `at`. The host clears it with `setSunLight(nil)` before destroying the light. `RelitParams.sunDir` is stored for later passes; the light term reads the light's frame. The weight is 1 by default. At a clamped weight of exactly 1, prelit draws without normals take the shader's plain variant: no relit pipeline, no face fetch and no forward stream build, so the modern tier pays nothing for the light term and draws exactly the enhanced colour. There is then no live sun on prelit geometry, so a shadow has nothing to remove there; geometry with its own normals keeps its relit variant and its separable sun at every weight. A weight just below 1 draws within one step of the weight-1 frame.

`setRelitDraw(0)` keeps the draws that follow on the shader's plain variant, the enhanced colour, until it is set again; `beginScene` sets it back to on. The default render path keeps an atomic baked when its geometry lacks `Geometry::LIGHT`: `lightingCB(atomic)` records the flag for that object, and `setWorldMatrix` clears the record for the next. A host that builds its own light list decides for itself. A kept-baked draw is prepassed and keyed exactly as before.

The sun's colour and direction are taken from the registered light when the scene begins and when `setSunLight` is called, so every relit prelit draw in a scene sees the same sun even if the host changes the light's colour per object. The host's sun is read once at scene start: a host that rescales its light after that point lights its own draws with the rescaled value, while relit prelit draws keep the colour read at scene start.

An instanced draw of a layout with vertex colours and no normals is lit per pixel. The vertex stage passes the classic colour with the weight applied (the prelit colour times the weight, plus the ambient, clamped and multiplied by the material colour as before), the same colour at weight 1, the derived normal with its validity, and the world position. The fragment stage takes the triangle's own face normal, reading its three indices at `[[primitive_id]]` and their positions from the instance's buffers (index and vertex buffers at fragment slots 14 and 15, the 80-byte `FaceFetch` block at 16). It fills the derived normal with that face normal by the same code the prepass uses, so the two cannot drift. The two edges are crossed in object space and the result is transformed by the cofactor of the matrix (`relitCofactor`), which gives the transformed face normal for any matrix, mirrored and unevenly scaled ones included. Where the derived normal's interpolated validity is 1 the forward pass does not fetch: with the validity at 1 the fill takes the derived normal alone, and the face normal only chooses its sign, which the final turn toward the eye settles the same way. A pixel seen exactly edge-on still fetches. The forward pass never reads the prepass normal target: under multisampling that target holds sample 0, and blended and alpha-tested draws are not in it. The share the weight removes, `1 - w`, is replaced by a two-colour hemisphere (world +z is up; desaturated and scaled) and the sun. When the host marks draws two-sided with `setRelitTwoSided(1)`, the sun term uses the absolute cosine (`relitSunCos`), so a thin surface is lit from either side. The cull state does not decide it: a renderer that turns culling off globally would otherwise light the shadow side of every surface. `beginScene` clears the mark. The sum is clamped as the classic colour is, and the sun's part of the clamped result is kept apart and multiplied by the screen-space shadow factor at texture 8 (`frameShadowTexture`). The hemisphere is never shadowed. The read of the shadow factor sits behind the function constant `relitShadow` (8), variant bit `VARIANT_RELITSHADOW` (64). A shader that opts into `VARIANT_RELIT` gets the bit in its mask. Relit draws take it only while a shadow source exists; until the shadow pass exists only the test hook provides one, and it prewarms those pipelines when it does. A draw whose derived stream is not built yet is lit by its face normal alone. Stream builds from forward draws run under the same budget, which resets when a prepass begins, so a modern frame drawn without a prepass bracket builds nothing once the budget is spent and its draws light by the face normal. A mesh whose strips restart (index 0xFFFF) is recorded on the instance (`stripRestart`); its prepass draws use the plain prepass shader and its relit draws take no face normal, so it is lit from its stream where the stream is valid and keeps its weight-1 colour where it has no normal.

Where the face fetch cannot run (positions that are not float3, a restart strip), both passes use the derived normal where it is valid. The prepass fills the rest with the geometric normal from screen-space derivatives; the forward pass does not light with that fill and blends toward the weight-1 colour by the validity, so where the derived normal is invalid it draws the classic colour.

With multisampling alone, a lit crease between faces that share their vertices is as jagged as without multisampling: the GPU merges the two triangles' fragments in a pixel on the edge and shades it once, for the face that covers more of its samples (ties go to one face), so every sample gets one face's light. The temporal resolve (TAA at native or the temporal scaler) integrates it, because the jitter moves the edge across the pixel; the resolved crease sits about an eighth of a pixel toward the face that wins ties. Silhouettes and edges between faces built with their own vertices blend by coverage as usual.

Geometry with its own normals, skinned geometry included, keeps the enhanced colour at every weight. In modern scene draws, a directional light that is the registered sun carries 1 in the fourth component of its light parameters. The vertex stage computes the colour without the sun and passes the difference on, so the fragment stage can remove exactly the sun's part where the shadow factor is below 1. Shaders that do not opt in see no difference. A relit pipeline that fails to build falls back to the shader's plain variant with one log line. The relit pipelines are prewarmed with the modern tier; the classic and enhanced tiers never build, bind or use them.

With AntiAliasing `taa` at a non-native upscale factor, the modern tier creates a MetalFX temporal scaler instead of the spatial one and feeds it the graded image, the scene depth (sample 0 at more than one sample), the motion target and a reactive mask. With `fxaa` or `off` the spatial scaler runs, with FXAA when asked. If the temporal scaler cannot be created, or the motion target cannot be, or the camera has no depth raster, or the prepass shaders are missing, the spatial scaler runs, with one log line. FXAA and the dering clamp do not run behind the temporal scaler. `FrameInfo::upscaler` reports `UPSCALER_TEMPORAL` and `antiAliasing` reports `ANTIALIAS_TEMPORAL`.

While a temporal consumer runs (the temporal scaler, or the TAA resolve), the projection of the camera rendering to the main raster is offset by an 8-phase Halton jitter at the internal resolution, chosen once at `beginScene` and applied at every `beginUpdate` of that camera in the frame, so the prepass and the forward pass upload the same matrix and keyed draws still test equal. A camera on a sub-raster of the main raster is offset by the same internal pixels, scaled to its viewport. Other cameras, such as an environment map's, are not jittered. im2D drawn in the scene gets the same offset in its screen transform, so sprites projected on the CPU stay aligned with the 3D scene; im2D drawn after `endScene` (a HUD, menus) is not jittered. `FrameInfo::jitterX` and `jitterY` give the offset in internal pixels (y down) applied to the scene projection this frame. The scaler gets that offset unchanged and the motion target scaled by the internal size; the smoke checks measure both conventions on the image, so a sign change fails them. A camera updated before `beginScene` keeps an unjittered projection, and the scaler is then told (0, 0).

A frame is jittered only when a temporal consumer exists, no reset is pending at `beginScene`, the previous frame had a valid prepass and the previous frame's temporal encode (the scaler's, or the TAA resolve's) used its history. The scaler and the resolve share this rule and the jitter sequence. Every other frame has a jitter of (0, 0). Because the rule looks at the previous frame and at `beginScene`, four things the host does inside a frame cost one jittered frame that resets: a `cameraCut()` after `beginScene`, a frame where the host stops bracketing the prepass (the jitter was chosen before the frame knew), a prepass dropped mid-frame, and `setRenderSettings` called after `beginScene`. After a reset the first frame resets unjittered, the second keeps its history unjittered and the jitter starts on the third. A host that never brackets the prepass, or brackets it on alternate frames only, is never jittered. A `cameraCut()` called after `beginScene` comes too late for that frame's rule: the frame is jittered and resets. Behind the temporal scaler that frame shows its raw jitter as one shimmering frame; the TAA resolve reads the current frame through the unjitter, so there it shows no shimmer.

Unbracketed im2D drawn in the scene has no motion of its own: it takes the motion of whatever the prepass drew behind it, or the far-plane camera motion over the sky. Under a moving camera its history moves with the background, so it smears or ghosts. Sprites and light glows drawn in the scene must be bracketed with `beginReactive()` and `endReactive()` (emissive im2D is marked anyway), or drawn after `endScene`. Behind the temporal scaler a bracketed sprite still wobbles by about half a pixel from frame to frame, because the scaler shows the raw jittered current frame where the mask is set; the TAA resolve unjitters it and holds it still.

im2D and im3D draws in the scene between `beginReactive()` and `endReactive()`, and every emissive im2D draw, are recorded and replayed at the end of the scene into an R8 reactive mask at the internal resolution (texture alpha times vertex alpha, maximum blend, no depth test), which tells the scaler or the TAA resolve to trust the current frame there. The replay has no depth test, so a bracketed draw hidden behind opaque geometry still marks its pixels and costs the history there; a depth test against the resolved scene depth is a later option. A frame records up to 4096 such draws in storage reserved once; later ones are left out of the mask, with one log line. The bracket closes at `beginScene`. The replay pipelines are built at `Engine::start`, so the first temporal frame compiles nothing. Before macOS 14.4 the scaler has no reactive mask, and the replay is skipped while the scaler runs; the TAA resolve replays it on every macOS version.

At native resolution with AntiAliasing `taa`, the modern tier resolves the graded image against its own history with a compute kernel. The grade writes into the anti-aliasing texture; the kernel reads the current frame there at the pixel moved by the applied jitter (bilinear, so a still image does not move with the jitter), reads the history with a Catmull-Rom filter at the pixel plus the motion of the nearest surface in its 3x3 neighbourhood (smallest scene depth), so a silhouette pixel follows the object in front, drops it when that position falls outside the screen, clips it to the YCoCg range of the current 3x3 neighbourhood in a luma-compressed space, and blends in that space with weight 0.9 times one minus the reactive mask. The range is taken from the pixel itself and its neighbours whose reactive mask is below 0.5, so a pixel beside an emitter does not keep the emitter's history, while the faint fringe of a glow still leaves the range whole. Before expanding, the blended colour's compressed luma is held below 63/64 and the result is clamped to zero or more, because a point inside the YCoCg range can have negative RGB or a compressed luma of 1 or more when saturated colours of different hues share a neighbourhood. The result goes to the output and into the second of two RGBA16F history textures at the output size (about 132.7 MB at 3840x2160, made the first time TAA runs and dropped when it stops). The resolve runs on the graded image before the output shoulder and EDR mapping, so values above white are in the history. If the history cannot be created, the backend logs once and runs FXAA until the scene targets are rebuilt. The stats line says `aa taa`; at a non-native factor it says `aa temporal`, because the scaler does both jobs.

The temporal history (the scaler's or the resolve's) resets on `cameraCut()`, on a frame without a valid prepass and the frame after it, when the scene targets are rebuilt (a resize, a new upscale factor or sample count), and on any tier or settings change. `FrameInfo::temporalReset` tells whether the last frame reset it.

## Log lines

The backend writes these lines to stderr, each starting with `rw::metal:`. Errors such as a pipeline that fails to build, an oversized custom block or a texture that cannot be created go through librw's error mechanism (`RWERROR`) instead.

- `prewarm <n> pipelines in <t> ms` at every start.
- `pipeline <key> created after init` for each state built during drawing.
- `draw dropped, <cause> (shader <vs>/<fs>)` once per cause and shader, or `draw dropped, <cause> (shader none)` when no shader is set. Causes: no render encoder is open; no shader is set; its pipeline failed to build; no ring space for its uniform blocks; no render target is set; it samples the raster it renders into; a two-uv im2d draw has no override shader or layout.
- `uniform block <n> is <a> bytes, shader expects <b>`, `shader <vs>/<fs>: <compiler warnings>`, `command buffer error: <text>`, `clear pipelines at <n> samples failed; using at most <m> samples`.
- `dering shaders failed to compile: <error>`; the upscaled frame then goes out without the clamp.
- `dering copy texture for <w>x<h> could not be created; the upscaled frame goes out unclamped` once, on a device without tier 2 read-write textures.
- `anti-aliasing taa requested, using fxaa; taa needs depth and motion vectors` each time the setting becomes TAA outside the modern tier, or in it without the prepass shaders, the motion kernels or (at native) the TAA kernel.
- `TAA history for <w>x<h> could not be created; using FXAA` once.
- `TAA kernel failed to build: <error>` at start; TAA then runs as FXAA.
- `temporal scaler unavailable for <iw>x<ih> to <ow>x<oh>; using the spatial scaler` once, when MetalFX cannot make the temporal scaler.
- `temporal scaler unavailable for <iw>x<ih> to <ow>x<oh>; taa needs a depth raster; using the spatial scaler` once, and the same with `the prepass shaders`, when `taa` resolves to the spatial scaler for that reason.
- `the temporal scaler has no motion target; using the spatial scaler` when the prepass targets cannot be created for it, instead of the first line.
- `reactive shaders failed to compile: <error>` and `reactive pipeline <vs> failed: <error>`; the reactive mask then stays empty for those draws.
- `more than 4096 reactive draws in a frame; the rest are left out of the reactive mask` once.
- `FXAA kernel failed to build: <error>`; the frame then runs without anti-aliasing.
- `prepass shaders failed to build; modern frames draw without the prepass` at start.
- `shader <vs>/<fs> has no relit variant; it draws without the live light` once per shader, when a relit pipeline fails to build.
- `prepass targets for <w>x<h> could not be created; modern frames draw without the prepass` once.
- `motion kernels failed to build; modern frames draw without the prepass` at start.
- `<n> prepass draws had no forward draw this frame; their depth hides what lies behind them` once.
- `FXAA texture for <w>x<h> could not be created; rendering without anti-aliasing` once.
- `spatial scaler unavailable for <iw>x<ih> to <ow>x<oh>; rendering at native resolution` once, when MetalFX cannot make the scaler.
- `EDR output on|off, headroom <current> of <potential>, emitter peak <peak>` when the layer switches between EDR and SDR or the screen's colour space changes. See "Output".

### Statistics line

```
rw::metal: stats frames %u draws/frame %.1f ring peak %u ring grows %u late pipelines %u host pipelines %u skinned unrouted %u strip restarts %u staged uploads %u direct uploads %u mipmap blits %u gpu waits %u block mismatches %u dropped draws %u passes/frame %.1f copies/frame %.1f custom uploads/frame %.1f material uploads/frame %.1f frames without drawable %u drawable wait max %.1f ms host prewarm %.1f ms samples %u tier %s internal %dx%d output %dx%d upscaler %s edr %s headroom %.2f/%.2f peak %.2f bloom %.3f aa %s prepass %u equal %u unkeyed %u normals built %u pending %u live %u bytes %u relit weight %.2f relit draws/frame %.1f sun %s
```

The backend prints it at every `Engine::stop`, and every `n` seconds while frames are shown after the host calls `metal::setStatsInterval(n)`. The default interval is 0, which prints only the line at stop. Counters start again from zero at every `Engine::start`. The line ends with the frame: tier, internal and output size, upscaler (none, spatial or temporal), EDR state (on or off), the current and potential headroom, the emitter peak, the bloom weight and the anti-aliasing in use (off, fxaa, taa or temporal).

The line logged at shutdown starts `stats at exit, last scene frame` and carries the tier, sizes and counts of the last scene frame, taken while its targets were live.

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
| prepass | draws in the last frame's prepass | last frame |
| equal | forward draws in the last frame that matched a prepass key and tested depth equal; below prepass when the host's forward draws missed the prepass's camera state or skipped prepassed objects | last frame |
| unkeyed | prepass draws past the key capacity, drawn without a key | since start |
| normals built | derived normal streams built | since start |
| pending | instances queued for a derived normal stream | now |
| live, bytes | derived normal streams in memory and their size in bytes | now |
| relit weight | the prelit weight in use, after the clamp | now |
| relit draws/frame | forward draws on a relit variant per frame | interval |
| sun | on when a sun is registered and the sun scale is above 0 | now |

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

The library is `lib/macosx-arm64-metal/Release/librw.a`. A host that builds librw itself defines `RW_METAL` and, for the GLFW host, `LIBRW_GLFW`, compiles `src/metal/*.mm` as Objective-C++ with `-fobjc-arc`, leaves `src/metal` out of other platforms, targets macOS 14, and links `Metal`, `MetalFX`, `QuartzCore`, `Cocoa` and `glfw`. Set `HOMEBREW_PREFIX` if Homebrew is not in `/opt/homebrew`. For the Cocoa host define `LIBRW_COCOA` instead of `LIBRW_GLFW` and link `AppKit` instead of `Cocoa` and `glfw`; build it with `make -C build config=release_macosx-arm64-metal-cocoa librw`, which writes `lib/macosx-arm64-metal-cocoa/Release/librw.a`.

## Tests

```
bash tests/metal/build.sh
bash tests/metal/run.sh
METAL_SMOKE_ASAN=1 bash tests/metal/build.sh && METAL_SMOKE_ASAN=1 bash tests/metal/run.sh
METAL_SMOKE_HOST=cocoa bash tests/metal/build.sh && METAL_SMOKE_HOST=cocoa bash tests/metal/run.sh
```

- `tests/metal/pure/` holds nineteen tests of the backend's pure rules (triangle fans, formats, instancing layouts, pipeline and state keys, sample counts, pass tracking, host mode lists, drawable sizes, render settings, frame phases and scene routing, scene blend sources, the SDR shoulder, the bloom pyramid, the dering clamp, FXAA luma, edge and walk rules, the prepass keys and normal encoding, the temporal jitter, clip and history rules, the relit light term, strip conversion and derived normals, the shadow pass rules). They need no GPU.
- `tests/metal/smoke/` drives the whole backend on the GPU with a hidden window and reads pixels back. It needs a logged-in GUI session on a Mac with a GPU and a display. It runs under the Metal validation layer; `run.sh` fails if the layer does not load or reports an error. With the GLFW host it prints `PASS` or `FAIL` for each of its 435 checks; with `METAL_SMOKE_HOST=cocoa` it opens the Cocoa host instead, runs the same 435 checks and 7 more for the Cocoa window (442), and skips the pure tests. Both end with `all tests passed`, and both print the same 435 check names for the checks they share. The last check attributes every pipeline an engine start built during drawing, outside the host's checks, to the check that built it, and fails the run when a check outside a named list builds one or a listed check builds more than its bound (custom and override shaders, uncommon state combinations, cameras without a depth raster, pipelines rebuilt after a restart). It prints each start's count, the host's share included. The Cocoa window is never ordered in: nothing appears on screen.
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
