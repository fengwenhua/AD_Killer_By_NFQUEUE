#pragma once
#if defined(__linux__)
#include_next <sys/prctl.h>
#else
// Only allows engine.cpp to compile for isolated macOS audit probes. The real
// run_daemon entry point is not called by the engine probe.
#define PR_SET_NAME 15
inline int prctl(int, ...) { return 0; }
#endif
