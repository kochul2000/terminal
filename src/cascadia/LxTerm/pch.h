// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

// Matches TerminalCore's pch: til has to come after the C++/WinRT headers.
#define BLOCK_TIL
#include <LibraryIncludes.h>
#include "winrt/Windows.Foundation.h"

#include "winrt/Microsoft.Terminal.Core.h"

#include <til.h>
#include <til/u8u16convert.h>
