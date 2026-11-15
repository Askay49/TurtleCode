#pragma once
#include <git2.h>

namespace GitRuntime {
inline int ensureInitialized() {
    // Keep one process-lifetime reference. Short-lived workspace scans and Git
    // operations still balance their own references, but cannot tear down the
    // SSL/global state while another worker (or Qt's TLS backend) is using it.
    // Deliberately avoid static-destructor shutdown: worker-pool destruction
    // order is not guaranteed across translation units.
    static const int initialized=git_libgit2_init();
    return initialized;
}
}
