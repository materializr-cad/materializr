// MeshDispatch decides whether a body is meshed in the frame, handed to the
// worker, or left waiting on a request already in flight.
#include "viewport/MeshDispatch.h"

#include <gtest/gtest.h>

using materializr::MeshDispatch;
using materializr::MeshPath;
using materializr::MeshRequest;

namespace {
int a = 0, b = 0; // stand-ins for TShape addresses
const MeshRequest reqA{&a, 0.1f, 0.3f};
const MeshRequest reqB{&b, 0.1f, 0.3f};
const MeshRequest reqAFine{&a, 0.05f, 0.3f};
} // namespace

TEST(MeshDispatch, OnlyABodyThatMeshedSlowlyGoesToTheWorker) {
    MeshDispatch d;
    EXPECT_EQ(d.decide(1, reqA), MeshPath::InFrame); // never meshed: no timing yet
    d.meshedInFrame(1, MeshDispatch::kAsyncMeshMs - 1.0);
    EXPECT_EQ(d.decide(1, reqA), MeshPath::InFrame);
    d.meshedInFrame(1, MeshDispatch::kAsyncMeshMs);
    EXPECT_EQ(d.decide(1, reqA), MeshPath::Worker);
    d.meshedInFrame(1, 2.0); // an edit made it light again
    EXPECT_EQ(d.decide(1, reqA), MeshPath::InFrame);
}

TEST(MeshDispatch, ARequestInFlightIsWaitedFor) {
    MeshDispatch d;
    d.meshedInFrame(1, 50.0);
    d.requested(1, reqA);
    EXPECT_TRUE(d.anyPending());
    EXPECT_EQ(d.decide(1, reqA), MeshPath::Pending);
    EXPECT_EQ(d.decide(1, reqB), MeshPath::Worker);     // the body was edited again
    EXPECT_EQ(d.decide(1, reqAFine), MeshPath::Worker); // or the quality changed
    d.finished(1, reqA, 50.0, true);
    EXPECT_FALSE(d.anyPending());
}

TEST(MeshDispatch, AnAnsweredRequestIsNeverSentAgain) {
    // Regression: a result with every face bare lands nothing, the pre-mesh
    // tag can never cover it, landing marks the body dirty and the next
    // rebuild asked the worker for the same shape again, forever.
    MeshDispatch d;
    d.meshedInFrame(1, 50.0);
    d.requested(1, reqA);
    d.finished(1, reqA, 50.0, true);
    for (int rebuild = 0; rebuild < 3; ++rebuild)
        EXPECT_EQ(d.decide(1, reqA), MeshPath::InFrame) << "rebuild " << rebuild;
    EXPECT_FALSE(d.anyPending());
    // A new shape (an edit) or a new quality is a new question.
    EXPECT_EQ(d.decide(1, reqB), MeshPath::Worker);
    EXPECT_EQ(d.decide(1, reqAFine), MeshPath::Worker);
}

TEST(MeshDispatch, AStaleResultClearsItsRequestAndNothingElse) {
    MeshDispatch d;
    d.meshedInFrame(1, 50.0);
    d.requested(1, reqA);
    d.finished(1, reqA, 5.0, false); // the body had moved on before it landed
    EXPECT_FALSE(d.anyPending());
    EXPECT_EQ(d.decide(1, reqA), MeshPath::Worker); // not answered, timing unchanged
}

TEST(MeshDispatch, AnInFrameMeshForgetsTheAnsweredRequest) {
    // A recycled TShape address can make a new shape look already answered;
    // the in-frame mesh that follows must clear that, so the body goes back
    // to the worker on its next edit instead of stalling every frame.
    MeshDispatch d;
    d.meshedInFrame(1, 50.0);
    d.requested(1, reqA);
    d.finished(1, reqA, 50.0, true);
    EXPECT_EQ(d.decide(1, reqA), MeshPath::InFrame);
    d.meshedInFrame(1, 50.0);
    EXPECT_EQ(d.decide(1, reqA), MeshPath::Worker);
}

TEST(MeshDispatch, AForgottenBodyStartsOver) {
    MeshDispatch d;
    d.meshedInFrame(1, 50.0);
    d.requested(1, reqA);
    d.forget(1);
    EXPECT_FALSE(d.anyPending());
    EXPECT_EQ(d.decide(1, reqA), MeshPath::InFrame);
}
