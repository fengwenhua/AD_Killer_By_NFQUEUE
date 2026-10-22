#pragma once
// Test-only forced include: every compiled translation unit uses one disposable
// data root. Never include this header in a production build.
#include "../../src/common.h"
#ifndef ADK_AUDIT_DATA_DIR
#error "The audit runner must provide a disposable data directory"
#endif
#undef DATA_DIR
#define DATA_DIR ADK_AUDIT_DATA_DIR
