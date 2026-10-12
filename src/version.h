/****************************************************************************
 Copyright (c) 2026 Simdsoft Limited.

 https://axmol.dev/

 SPDX-License-Identifier: MIT
 ****************************************************************************/
#pragma once

#include "axslc-spec.h"

#define AXSLCC_MAJOR 4
#define AXSLCC_MINOR 0
#define AXSLCC_PATCH 0

#define _AXSLCCSTR(R)      #R
#define _AXSLCCMACROSTR(R) _AXSLCCSTR(R)

#define AXSLCC_VERSION_STRING _AXSLCCMACROSTR(AXSLCC_MAJOR) "." _AXSLCCMACROSTR(AXSLCC_MINOR) "." _AXSLCCMACROSTR(AXSLCC_PATCH)

static_assert(AXSLCC_MAJOR == SC_ABI_MAJOR);
