#pragma once
#include "exclusive_file_lock.h"
#include <string>

// One global order for coherent routing/rule inputs. The journal lock is
// terminal and may be taken under these locks, never in the opposite order.
class ConfigInputLock {
public:
    explicit ConfigInputLock(const std::string &root)
        : apps_((root + "/.apps.lock").c_str(), true, 1000),
          editable_((root + "/.editable.lock").c_str(), true, 1000),
          state_((root + "/.state.lock").c_str(), true, 1000) {}
    bool ok() const { return apps_.ok() && editable_.ok() && state_.ok(); }
private:
    ExclusiveFileLock apps_, editable_, state_;
};
