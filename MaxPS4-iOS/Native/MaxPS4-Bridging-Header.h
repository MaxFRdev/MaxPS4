#pragma once
#include "maxps4_fex_runtime.h"
#include "PM4MetalDrawBridge.h"
#include "PM4GameMetalBridge.h"
#include "PS4PKGBridge.h"
#include "MaxPS4CrashLog.h"

// Existing passive sysctl probe; debugger presence is not JIT permission.
int32_t maxps4_native_debugger_attached(void)
    __attribute__((swift_name("maxps4FEXDebuggerStatus()")));


#include "OrbisAnalysisBridge.h"
#include "OrbisLinkerBridge.h"
