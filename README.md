# axslcc

## Overview
**axslcc** is the complete rewrite of the Axmol shader compiler. Releases follow semantic versioning from **4.0.0**
onward: the **major** number is the `.sc` container ABI generation, **minor** adds features without changing the
container, and **patch** carries fixes. This document explains the compatibility and download policy across the version
lines and points to the legacy repository for users who need to build older releases locally.

## Deprecation Notice
- **All versions <= 3.6.0 are deprecated.**  
  These legacy releases are retained **only** to support building older versions of **axmol** and are no longer actively maintained.
- **Versions 3.99.0 – 3.99.2 are the transition line and are now superseded.**  
  They already contain the rewritten implementation, but the archives they produce belong to container generation 3.
- **Version 3.99.3 was withdrawn**: its release and tag have been removed, so it is not available to download. The
  header change it carried ships in 4.0.0 instead.
- **The supported line begins at >= 4.0.0**, container generation 4.

## Download and Usage
- If you need axslcc for **legacy axmol builds**, download the appropriate pre-3.6.0 release assets.
- For new development, integration, or production use, take a **4.0.0 or newer** release. Axmol runtimes from the 4.0.0
  line reject generation-3 archives outright, so **recompile every shader** when you switch: clear the compiler output
  folder and the `axslc` copy that sits next to the executable, then rebuild.

## Compatibility Summary

| axslcc version | Container generation | Status |
|----------------|----------------------|--------|
| <= 3.6.0       | legacy container     | Deprecated; kept for legacy axmol builds only |
| 3.99.0 – 3.99.2 | 3                  | Superseded; archives are not readable by 4.0.0+ runtimes |
| 3.99.3         | —                    | Withdrawn; release and tag removed |
| >= 4.0.0       | 4                    | Current line, semantic versioning |

### Archive versioning

Every `.sc` archive starts with the container magic (`SC_CHUNK`) followed by `sc_chunk`, whose `major` field is the ABI
generation; a runtime accepts an archive only when that field matches its own `SC_ABI_MAJOR`. The magic names the
container and does not change between generations, so the version fields carry the compatibility decision. From 4.0.0
the `{major, minor, patch, num_targets}` layout of `sc_chunk` is frozen, and `AXSLCC_MAJOR` is bound to `SC_ABI_MAJOR`
by a compile-time assertion, so a release cannot advertise a generation the container format does not back.

## Build Legacy Versions Locally
If you need to compile or inspect deprecated legacy versions locally, clone the legacy repository and follow its build instructions:

```bash
git clone https://github.com/axmolengine/axslcc-legacy
cd axslcc-legacy
# follow the repository README for build steps for the specific version you need
```

Use repository tags or release archives to check out the exact legacy version you want to build.

## Contributing and Contact
- Contributions to the rewritten project will be accepted in the new repository once the rewrite is published. Check the new project README for contribution guidelines.
- For issues specifically related to legacy builds, open issues in the legacy repository or consult its documentation.

**Note:** This repository and its legacy artifacts are intended to help users who must maintain or rebuild older axmol targets. For new work, use axslcc **4.0.0 or newer**.

## Command Line

```
axslcc [options] <input>
```

| Option | Description |
|--------|-------------|
| `-o <path>` | Output file or basename (default: input stem) |
| `-t <target>` | Output target, repeatable: `d3d11`, `d3d12`, `vk`, `mtl`, `gl`, `gles` |
| `-a` | Write an Axmol `.sc` archive with reflection data |
| `-S` | Keep HLSL source, don't compile to DXBC/DXIL (D3D targets only) |
| `-O<level>` | Optimization level: `0` (debug, default), `1` (size), `2` (speed), `3` (aggressive) |
| `-I <dir>` | Include directory (repeatable) |
| `-D <name>[=<val>]` | Preprocessor define (repeatable) |
| `--cvar <name>` | C variable name for embedded shader data |
| `--version` | Print version and exit |

Input is always HLSL, and the shader stage is detected from the filename: a
`_vs` / `_ps` / `_fs` / `_cs` suffix. Legacy `lang-profile` target specs
(e.g. `hlsl-50`, `spirv-100`, `msl-20000`, `glsl-330`, `essl-300`) are also
accepted by `-t`.

## Shader Compilation Architecture

### Pipeline

axslcc compiles HLSL shaders through a multi-stage pipeline. Every target is
first lowered to SPIR-V by DXC, then SPIRV-Cross cross-compiles that SPIR-V to
the requested language (or FXC/DXC turn the resulting HLSL into D3D bytecode):

```
HLSL source
        │
        ▼
   ┌──────────────────────────────────┐
   │ DXC → SPIR-V                      │  resolves #include / #define
   └────────────────┬─────────────────┘
                    │  SPIR-V (intermediate IR)
                    ▼
   ┌──────────────────────────────────┐
   │            SPIRV-Cross            │
   └────────────────┬─────────────────┘
                    │
                    ├── vk    → SPIR-V binary (runtime metadata stripped)
                    ├── gl    → GLSL text
                    ├── gles  → GLSL ES text
                    ├── mtl   → Metal Shading Language text
                    └── d3d11 / d3d12 → clean HLSL text
                                          │
                                          │ default: compile to bytecode
                                          ├── profile <= 51 → FXC → DXBC (Windows)
                                          ├── profile >= 60 → DXC → DXIL
                                          │
                                          └── -S: keep HLSL source text
```

Routing D3D targets through SPIRV-Cross first ensures Vulkan-specific
annotations like `[[vk::builtin("PointSize")]]` are stripped before the HLSL
output is fed to FXC/DXC, which do not recognize them.

### DXC frontend and Vulkan samplers

DXC is the only HLSL frontend, on Windows, Linux, and macOS, used through the
DXC library API. It is auto-downloaded as a pinned prebuilt distribution by
`3rdparty/dxc` at build time; the location can be overridden with the
`AXSLCC_DXC_ROOT` CMake cache variable. Release packages ship the matching
`dxcompiler` runtime next to `axslcc`.

Vulkan (`-t vk`) always uses separate sampled-image and sampler descriptors:

```bash
axslcc -t vk shader_ps.hlsl
```

DXC shifts sampler bindings into a separate Vulkan binding range and removes
unused `base.hlsli` presets. GLSL/ESSL targets always generate combined sampler
uniforms.

### D3D bytecode (default) vs. source (`-S`)

For D3D targets (`d3d11`, `d3d12`) axslcc compiles to bytecode by default: the
clean HLSL emitted by SPIRV-Cross is fed to FXC (SM <= 5.1, Windows only) or DXC
(SM >= 6.0). Pass `-S` to keep the HLSL source text instead of compiling it. FXC
is only built on Windows.

### d3d12 Bytecode Expansion

When bytecode is produced for `-t d3d12` (i.e. without `-S`, Windows only),
axslcc emits **two** entries in the `.sc` archive:

1. **SM 5.1 (FXC)**: `profile_ver = 51 | SC_BYTECODE_FLAG` — DXBC for backward compatibility with older D3D12 drivers
2. **SM 6.0 (DXC)**: `profile_ver = 60 | SC_BYTECODE_FLAG` — DXIL for modern D3D12 drivers

### .sc Archive Binary Flag

The `.sc` archive (`-a`) stores a per-target `profile_ver` field. Bit 31
(`SC_BYTECODE_FLAG = 0x80000000`) indicates whether the code chunk contains
bytecode:

| `profile_ver` | Content |
|---------------|---------|
| `51` | HLSL source text (SM 5.1) |
| `51 \| SC_BYTECODE_FLAG` | DXBC bytecode (SM 5.1) |
| `60 \| SC_BYTECODE_FLAG` | DXIL bytecode (SM 6.0) |

The RHI runtime reads this flag to decide whether the data is passed to
`CreateVertexShader`/`CreatePixelShader` (bytecode) or compiled at runtime
(source).

### Target Types

| Target   | Source Output (`-S`)   | Bytecode Output (default) |
|----------|------------------------|---------------------------|
| `d3d11`  | HLSL text (SM 5.0)     | DXBC via FXC (SM 5.0)     |
| `d3d12`  | HLSL text (SM 5.1)     | DXBC SM 5.1 (FXC) + DXIL SM 6.0 (DXC) |
| `vk`     | —                      | SPIR-V binary             |
| `gl`     | GLSL text (330)        | —                          |
| `gles`   | GLSL ES text (300)     | —                          |
| `mtl`    | Metal Shading Language | —                          |

### Reflection

Reflection data (vertex inputs, uniform/constant buffers, textures, samplers) is
generated via SPIRV-Cross from the DXC-produced SPIR-V and embedded in the
`REFL` chunk of the `.sc` archive when `-a` is specified. Reflection always uses
the SPIRV-Cross path regardless of source/bytecode output mode.

### Compiler Backends

| Stage | File | Purpose |
|-------|------|---------|
| DXC | `spirv_compiler.cpp` | HLSL source preparation (resource-layout injection) and the DXC SPIR-V front end |
| DXC | `dxc_compiler.cpp` | HLSL → SPIR-V; HLSL backend → DXIL (SM 6.0) |
| SPIRV-Tools | `spirv_compiler.cpp` | Dead-resource elimination, non-runtime metadata stripping, Vulkan validation |
| SPIRV-Cross | `cross_compiler.cpp` | SPIR-V → target language (HLSL/GLSL/MSL/ESSL) |
| SPIRV-Cross | `reflection.cpp` | SPIR-V → `.sc` reflection data (UBO/texture/sampler/input bindings) |
| FXC | `fxc_compiler.cpp` | HLSL → DXBC (SM <= 5.1) — Windows only |
