// Copyright (c) 2014, Cloudera, inc.

#ifndef KUDU_MONITORED_TASK_H
#define KUDU_MONITORED_TASK_H

#include <string>

#include "kudu/gutil/ref_counted.h"
#include "kudu/util/monotime.h"
#include "kudu/util/task_executor.h"

namespace kudu {

class MonitoredTask : public RefCountedThreadSafe<MonitoredTask>,
                      public Task {
  public:
    enum State {
      kStatePreparing,
      kStateRunning,
      kStateComplete,
      kStateFailed,
      kStateAborted,
    };

    // Task State
    virtual State state() const = 0;

    // Task Type Identifier
    virtual std::string type_name() const = 0;

    // Task description
    virtual std::string description() const = 0;

    // Task start time, may be !Initialized()
    virtual MonoTime start_timestamp() const = 0;

    // Task completion time, may be !Initialized()
    virtual MonoTime completion_timestamp() const = 0;
};

} // namespace kudu

#endif
