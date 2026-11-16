#pragma once

#include <string>

namespace octopus::tool {

struct Result;

struct ActivityStart {
    std::string call_id, identity, label;
};

/* Borrowed synchronous observer; exceptions never change operational results. */
class ActivityObserver {
  public:
    virtual ~ActivityObserver() = default;
    virtual void started(const ActivityStart &) = 0;
    virtual void finished(const ActivityStart &, const Result &) = 0;
};

} /* namespace octopus::tool */
