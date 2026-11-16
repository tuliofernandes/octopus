#pragma once

#include <string>

namespace octopus::tool {

struct Result;

struct ActivityStart {
    std::string call_id, identity, label;
};

/**
 * Borrowed only for a synchronous execute call. Event/result references
 * must not be retained. Notifications surround handler execution only;
 * the executor isolates observer exceptions from operational results.
 */
class ActivityObserver {
  public:
    virtual ~ActivityObserver() = default;
    virtual void started(const ActivityStart &) = 0;
    virtual void finished(const ActivityStart &, const Result &) = 0;
};

} /* namespace octopus::tool */
