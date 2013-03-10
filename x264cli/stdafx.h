// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#include <stdio.h>
#include <tchar.h>

#include <windows.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

#include <stdlib.h>
#include <memory.h>
#include <string.h>

#define X264_VERSION " r1900M 60ef1f8"
extern "C" {
#include "stdint.h"
#include "x264.h"
}
