#pragma once
#include "engine.h"
#include "process_identity.h"
#include <string>

namespace apply_state {
struct Applied {
    EngineConfig config;
    std::string generation;
    uint64_t black = 0, white = 0, updated_at = 0;
};
struct Record {
    bool valid = false;
    std::string desired, applied, attempt, last_revision, error;
    bool submitted = false;
    ProcessIdentity owner;
    Applied actual;
};
struct View {
    Record record;
    std::string phase = "unverified";
    bool owner_verified = false;
    bool actual_verified = false;
};

bool valid_revision(const std::string &revision);
Record read(const std::string &root); // bounded, read-only; never creates files/locks
View view(const Record &record, const ProcessIdentity &live, bool running,
          const std::string &expected = "");
// Call dirty BEFORE replacing a routing input, while holding its writer lock.
// A crash between save and submission must never leave an old green ACK.
bool dirty(const std::string &root, std::string &error);
// Call queue while holding the apps -> editable -> state input locks.
bool queue(const std::string &root, std::string &revision, std::string &error);
// Capture uses an existing submitted request; direct SIGHUP/initial startup
// creates one only when no submitted request is pending. Dirty inputs wait
// for the saving CLI to submit (startup may reconcile a crashed writer).
bool capture(const std::string &root, bool startup, const ProcessIdentity &owner,
             std::string &revision, std::string &error);
bool current(const std::string &root, const ProcessIdentity &owner, const std::string &revision);
bool finish(const std::string &root, const ProcessIdentity &owner, const std::string &revision,
            const Applied *actual, const std::string &failure, std::string &error);
bool pending(const Record &record, const ProcessIdentity &owner);
}
