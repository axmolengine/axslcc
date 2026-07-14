# axslcc

## Overview
**axslcc** is being **completely rewritten**. The rewritten project will start at **version 3.99.0 and later**. This document explains compatibility and download policy during the transition and points to the legacy repository for users who need to build older releases locally.

## Deprecation Notice
- **All versions <= 3.6.0 are deprecated.**  
  These legacy releases are retained **only** to support building older versions of **axmol** and are no longer actively maintained.
- **The new implementation begins at >= 3.99.0.**  
  From 3.99.0 onward, axslcc is a full rewrite and will follow a new development and release process.

## Download and Usage
- If you need axslcc for **legacy axmol builds**, download the appropriate release assets from the legacy releases.
- For new development, integration, or production use, prefer the rewritten axslcc releases **version 3.99.0 and above** once they become available.

## Compatibility Summary
- **Versions <= 3.6.0**: Deprecated and kept only for legacy axmol build support.  
- **Versions 3.99.0 and above**: Rewritten implementation. Use these for current and future development.

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

**Note:** This repository and its legacy artifacts are intended to help users who must maintain or rebuild older axmol targets. For new work, migrate to the rewritten axslcc (version **>= 3.99.0**) when it becomes available.

## Command Line

```
axslcc [options] <input>
```

| Option | Description |
|--------|-------------|
| `-o <path>` | Output file or basename (default: input stem) |
| `-t <target>` | Output target, repeatable: `d3d11`, `d3d12`, `vk`, `mtl`, `gl`, `gles` |
| `-a` | Write an Axmol `.sc` archive with reflection data |
| `-x <lang>` | Input language: `hlsl`, `glsl` (default: `hlsl`) |
| `--hlsl-frontend <dxc\|glslang>` | HLSL frontend (default: `dxc`) |
| `--vulkan-samplers <separate\|combined>` | Vulkan descriptor model (default: `separate`) |
| `-S` | Keep HLSL source, don't compile to DXBC/DXIL (D3D targets only) |
| `-O<level>` | Optimization level: `0` (debug, default), `1` (size), `2` (speed), `3` (aggressive) |
| `-I <dir>` | Include directory (repeatable) |
| `-D <name>[=<val>]` | Preprocessor define (repeatable) |
| `--cvar <name>` | C variable name for embedded shader data |
| `--version` | Print version and exit |

The shader stage is detected from the filename: a `_vs` / `_ps` / `_cs` suffix
or a `.vert` / `.frag` / `.comp` extension. Legacy `lang-profile` target specs
(e.g. `hlsl-50`, `spirv-100`, `msl-20000`, `glsl-330`, `essl-300`) are also
accepted by `-t`.

## Shader Compilation Architecture

### Pipeline

axslcc compiles HLSL/GLSL shaders through a multi-stage pipeline. Every target
is first lowered to SPIR-V by a frontend, then SPIRV-Cross cross-compiles that
SPIR-V to the requested language (or FXC/DXC turn the resulting HLSL into D3D
bytecode):

```
HLSL / GLSL source
        │
        ▼
   ┌──────────────────────────────────┐
   │ Frontend → SPIR-V                 │  resolves #include / #define
   │   • HLSL: DXC (default) or        │
   │     glslang (--hlsl-frontend)     │
   │   • GLSL: glslang                 │
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

Routing D3D targets through SPIRV-Cross first ensures glslang/Vulkan-specific
annotations like `[[vk::builtin("PointSize")]]` are stripped before the HLSL
output is fed to FXC/DXC, which do not recognize them.

### HLSL frontend and Vulkan samplers

DXC is the default HLSL frontend on Windows, Linux, and macOS, used through the
DXC library API. It is auto-downloaded as a pinned prebuilt distribution by
`3rdparty/dxc` at build time; the location can be overridden with the
`AXSLCC_DXC_ROOT` CMake cache variable. Release packages ship the matching
`dxcompiler` runtime next to `axslcc`.

Use `--hlsl-frontend dxc|glslang` to select the frontend; the default is `dxc`.
`glslang` is a temporary migration path and cannot emit Vulkan separate samplers.

Vulkan (`-t vk`) defaults to separate sampled-image and sampler descriptors:

```bash
axslcc --vulkan-samplers separate -t vk shader_ps.hlsl
```

DXC shifts sampler bindings into a separate Vulkan binding range and removes
unused `base.hlsli` presets. `--vulkan-samplers combined` retains the
compatibility round-trip (SPIR-V → GLSL → SPIR-V) for texture-owned sampler
pipelines. GLSL/ESSL targets always generate combined sampler uniforms.

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
generated via SPIRV-Cross from the frontend-produced SPIR-V and embedded in the
`REFL` chunk of the `.sc` archive when `-a` is specified. Reflection always uses
the SPIRV-Cross path regardless of source/bytecode output mode.

### Compiler Backends

| Stage | File | Purpose |
|-------|------|---------|
| glslang | `spirv_compiler.cpp` | HLSL/GLSL → SPIR-V (preprocessor, parser, validator) |
| DXC | `dxc_compiler.cpp` | HLSL frontend → SPIR-V; HLSL backend → DXIL (SM 6.0) |
| SPIRV-Cross | `cross_compiler.cpp` | SPIR-V → target language (HLSL/GLSL/MSL/ESSL) |
| SPIRV-Cross | `reflection.cpp` | SPIR-V → `.sc` reflection data (UBO/texture/sampler/input bindings) |
| FXC | `fxc_compiler.cpp` | HLSL → DXBC (SM <= 5.1) — Windows only |
