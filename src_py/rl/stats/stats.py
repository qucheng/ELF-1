# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

from collections import defaultdict
import time

class ValueStats(object):
    def __init__(self, name=None):
        self.name = name
        self.reset()

    def feed(self, v):
        self.summation += v
        if v > self.max_value:
            self.max_value = v
            self.max_idx = self.counter
        if v < self.min_value:
            self.min_value = v
            self.min_idx = self.counter

        self.counter += 1

    def summary(self, info=None):
        info = "" if info is None else info
        name = "" if self.name is None else self.name
        if self.counter > 0:
            try:
                return "%s%s[%d]: avg: %.5f, min: %.5f[%d], max: %.5f[%d]" % (
                    info, name, self.counter, self.summation / self.counter,
                    self.min_value, self.min_idx, self.max_value, self.max_idx
                )
            except BaseException:
                return "%s%s[Err]:" % (info, name)
        else:
            return "%s%s[0]" % (info, name)

    def reset(self):
        self.counter = 0
        self.summation = 0.0
        self.max_value = -1e38
        self.min_value = 1e38
        self.max_idx = None
        self.min_idx = None

class Stats:
    def __init__(self):
        self.stats = dict()
        self.last_start = None
        
    def __getitem__(self, key):
        if key not in self.stats:
            self.stats[key] = ValueStats(name=key)
        return self.stats[key]

    def first(self):
        key = next(iter(self.stats))
        return self.stats[key]

    def summary(self, info=None):
        curr = time.perf_counter()
        if self.last_start is not None:
            elapsed = curr - self.last_start
            time_str = "Time elasped: %f sec" % elapsed 
        else:
            time_str = ""
        self.last_start = curr
            
        return time_str + "\n" + "\n".join([ s.summary() for k, s in self.stats.items()])

    def reset(self):
        for k, s in self.stats.items():
            s.reset()
