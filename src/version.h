/****************************************************************************
 Copyright (c) 2026 Simdsoft Limited.

 https://axmol.dev/

 SPDX-License-Identifier: MIT
 ****************************************************************************/
#pragma once

#define AXSLCC_MAJOR 3
#define AXSLCC_MINOR 99
#define AXSLCC_PATCH 2

#define _AXSLCCSTR(R)      #R
#define _AXSLCCMACROSTR(R) _AXSLCCSTR(R)

#define AXSLCC_VERSION_STRING _AXSLCCMACROSTR(AXSLCC_MAJOR) "." _AXSLCCMACROSTR(AXSLCC_MINOR) "." _AXSLCCMACROSTR(AXSLCC_PATCH)
