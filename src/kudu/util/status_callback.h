// Copyright (c) 2014, Cloudera, inc.
#ifndef KUDU_UTIL_STATUS_CALLBACK_H
#define KUDU_UTIL_STATUS_CALLBACK_H

#include "kudu/gutil/callback_forward.h"

namespace kudu {

class Status;

// A callback which takes a Status. This is typically used for functions which
// produce asynchronous results and may fail.
typedef Callback<void(const Status& status)> StatusCallback;

} // namespace kudu

#endif
