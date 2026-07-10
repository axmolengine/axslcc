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

## Shader Compilation Architecture

### Pipeline

axslcc compiles HLSL/GLSL shaders through a multi-stage pipeline:

```
HLSL/GLSL source
       │
       ├── (--dxbc path, Windows only, HLSL input)
       │       │
       │       ├── d3d11  →  glslang → SPIR-V → SPIRV-Cross → FXC → DXBC bytecode
       │       │
       │       ├── d3d12 sm51 →  FXC (raw HLSL) → DXBC bytecode
       │       │
       │       └── d3d12 sm60 →  DXC (raw HLSL) → DXIL bytecode
       │
       └── (source output path, all platforms)
               │
               ▼
          ┌─────────┐
          │ glslang  │  ── preprocessor (resolves #include, #define)
          └────┬────┘
               │
               ▼
          ┌─────────┐
          │  SPIR-V  │  ── intermediate binary IR
          └────┬────┘
               │
               ▼
          ┌──────────────┐
          │  SPIRV-Cross  │  ── cross-compile to target language
          └────┬─────────┘
               │
               ├── (reflection)  SPIRV-Cross reflection data  ← unified for all modes
               │
               └── .sc container with text source (HLSL/GLSL/MSL/ESSL)

d3d11 --dxbc is routed through SPIRV-Cross first to down-convert SM 5.1 syntax
to SM 5.0 compatible HLSL before FXC compilation. d3d12 --dxbc bypasses SPIRV-Cross
entirely for raw native compilation speed.
```

`--dxbc` bytecode compilation bypasses glslang and SPIRV-Cross entirely, feeding raw HLSL directly to FXC or DXC. All `#include` resolution is handled by the respective compiler's preprocessor.

### Reflection

Reflection data (vertex inputs, constant buffers, textures) is generated via SPIRV-Cross from the glslang-produced SPIR-V intermediate, ensuring consistent results across all output modes (source text and bytecode).

### Target Types

| Target    | Source Output (no `--dxbc`) | Bytecode Output (`--dxbc`) |
|-----------|----------------------------|---------------------------|
| `d3d11`   | HLSL text (SM 5.0)         | DXBC via FXC (SM 5.0)     |
| `d3d12`   | HLSL text (SM 5.1)         | DXBC SM 5.1 (FXC) + DXIL SM 6.0 (DXC) |
| `essl-300`| GLSL ES text               | —                          |
| `glsl-330`| GLSL text                  | —                          |
| `spirv-100`| SPIR-V binary              | —                          |
| `msl-*`   | Metal Shading Language      | —                          |

### d3d12 Bytecode Expansion

When `--dxbc` is specified with `--target=d3d12`, axslcc produces **two** entries in the .sc container:

1. **SM 5.1 (FXC)**: `profile_ver = 51 | SC_PROFILE_BINARY` — DXBC for backward compatibility with older D3D12 drivers
2. **SM 6.0 (DXC)**: `profile_ver = 60 | SC_PROFILE_BINARY` — DXIL for modern D3D12 drivers

### .sc Container Binary Flag

The `.sc` container stores a per-target `profile_ver` field. Bit 31 (`SC_PROFILE_BINARY = 0x80000000`) indicates whether the code chunk contains bytecode:

| `profile_ver` | Content |
|---------------|---------|
| `51` | HLSL source text (SM 5.1) |
| `51 \| SC_PROFILE_BINARY` | DXBC bytecode (SM 5.1) |
| `60 \| SC_PROFILE_BINARY` | DXIL bytecode (SM 6.0) |

The RHI runtime reads this flag to determine if the data should be passed to `CreateVertexShader`/`CreatePixelShader` (bytecode) or compiled at runtime (source).

### Reflection

Reflection data (vertex inputs, constant buffers, textures) is generated via SPIRV-Cross and embedded in the `REFL` chunk of the .sc container when `--reflect` is specified. Reflection always uses the SPIRV-Cross path regardless of source/bytecode output mode.

### Compiler Backends

| Backend | File | Purpose |
|---------|------|---------|
| glslang | `spirv_compiler.cpp` | HLSL/GLSL → SPIR-V (preprocessor, parser, validator) |
| SPIRV-Cross | `cross_compiler.cpp` | SPIR-V → target language (HLSL/GLSL/MSL/ESSL) |
| SPIRV-Cross | `reflection.cpp` | SPIR-V → .sc reflection data (UBO/texture/input bindings) |
| FXC | `fxc_compiler.cpp` | HLSL → DXBC (SM 5.0 / 5.1) — `--dxbc` d3d11/d3d12 bytecode |
| DXC | `dxc_compiler.cpp` | HLSL → DXIL (SM 6.0) — `--dxbc` d3d12 bytecode |
