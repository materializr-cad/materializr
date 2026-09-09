#pragma once

#include "PreviewDispatch.h"

namespace materializr {

// What a push/pull preview frame asked for. Two frames with the same key
// would produce the same document, so a job for one answers the other.
struct PushPullKey {
    double distance = 0.0;
    bool symmetric = false;
    bool operator==(const PushPullKey& o) const
    {
        return distance == o.distance && symmetric == o.symmetric;
    }
};

// The push/pull gesture's dispatch (PreviewDispatch). In async mode the
// gesture draws the ghost tool volume every frame while the boolean runs on
// the worker (PushPullPreview). The heavy path, a ghost with no boolean
// until commit, lives in PushPullState::heavyPreview and is decided before
// the first frame.
using PushPullDispatch = PreviewDispatch<PushPullKey>;

} // namespace materializr
