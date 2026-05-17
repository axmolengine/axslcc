# axslcc

## Overview
**axslcc** is being **completely rewritten**. The rewritten project will start at **version 3.9.0 and later**. This document explains compatibility and download policy during the transition and points to the legacy repository for users who need to build older releases locally.

## Deprecation Notice
- **All versions <= 3.6.0 are deprecated.**  
  These legacy releases are retained **only** to support building older versions of **axmol** and are no longer actively maintained.
- **The new implementation begins at >= 3.9.0.**  
  From 3.9.0 onward, axslcc is a full rewrite and will follow a new development and release process.

## Download and Usage
- If you need axslcc for **legacy axmol builds**, download the appropriate release assets from the legacy releases.
- For new development, integration, or production use, prefer the rewritten axslcc releases **version 3.9.0 and above** once they become available.

## Compatibility Summary
- **Versions <= 3.6.0**: Deprecated and kept only for legacy axmol build support.  
- **Versions 3.9.0 and above**: Rewritten implementation. Use these for current and future development.

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

**Note:** This repository and its legacy artifacts are intended to help users who must maintain or rebuild older axmol targets. For new work, migrate to the rewritten axslcc (version **>= 3.9.0**) when it becomes available.