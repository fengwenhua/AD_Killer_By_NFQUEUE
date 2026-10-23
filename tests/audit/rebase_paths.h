#pragma once
// Test-only forced include: every compiled translation unit uses one disposable
// data root. Never include this header in a production build.
#define DATA_FILE ADK_AUDIT_ORIGINAL_DATA_FILE
#include "../../src/common.h"
#undef DATA_FILE
#ifndef ADK_AUDIT_DATA_DIR
#error "The audit runner must provide a disposable data directory"
#endif
#undef DATA_DIR
#define DATA_DIR ADK_AUDIT_DATA_DIR
#undef PROC_NFQUEUE_FILE
#define PROC_NFQUEUE_FILE (DATA_DIR "/queue.fixture")
// common.h's inline helper is expanded when included, not when called. Rebase
// it too; rebasing only the file macros would leave DATA_FILE() on real /data.
inline std::string DATA_FILE(const char *name) {
    return std::string(DATA_DIR) + "/" + name;
}
