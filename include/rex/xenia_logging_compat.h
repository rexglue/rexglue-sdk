// Compatibility shim: Map legacy Xenia XELOG* macros to RexGlue REXLOG_* macros
// Used by Metal GPU backend files ported from Xenia

#pragma once

#include <rex/logging.h>

#ifndef XELOGE
#define XELOGE(...) REXLOG_ERROR(__VA_ARGS__)
#endif
#ifndef XELOGW
#define XELOGW(...) REXLOG_WARN(__VA_ARGS__)
#endif
#ifndef XELOGI
#define XELOGI(...) REXLOG_INFO(__VA_ARGS__)
#endif
#ifndef XELOGD
#define XELOGD(...) REXLOG_DEBUG(__VA_ARGS__)
#endif
#ifndef XELOGGPU
#define XELOGGPU(...) REXLOG_DEBUG(__VA_ARGS__)
#endif
