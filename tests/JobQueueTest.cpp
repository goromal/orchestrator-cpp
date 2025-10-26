#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#pragma GCC diagnostic pop

#include <mscpp/ServiceFactory.h>
#include "orchestrator/JobQueue.h"

TEST_CASE("TestJQStore")
{
    using namespace orchestrator;
    using namespace orchestrator::job_queue;
    using namespace aapis::orchestrator::v1;

    Store store;

    Job job1, job2, job3, job4, job5, job6;

    job1.priority = 1;
    int64_t id1   = store.addAndRegisterNewJob(job1, false);

    job2.priority = 0;
    int64_t id2   = store.addAndRegisterNewJob(job2, false);
    REQUIRE(id1 != id2);

    job3.priority = 0;
    job3.independentBlockers.push_back(id1);
    job3.relevantBlockers.push_back(id2);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);
    REQUIRE(id2 != id3);

    job4.priority = 1;
    job4.relevantBlockers.push_back(id1);
    int64_t id4 = store.addAndRegisterNewJob(job4, false);
    REQUIRE(id3 != id4);

    job5.priority = 5;
    int64_t id5   = store.addAndRegisterNewJob(job5, false);
    REQUIRE(id4 != id5);

    job6.priority = 0;
    job6.independentBlockers.push_back(id5);
    int64_t id6 = store.addAndRegisterNewJob(job6, false);
    REQUIRE(id5 != id6);

    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_QUEUED; })(
        store.query(QueryInput::GetQueuedJobWithId{id1})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_QUEUED; })(
        store.query(QueryInput::GetQueuedJobWithId{id2})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_BLOCKED; })(
        store.query(QueryInput::GetQueuedJobWithId{id3})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_BLOCKED; })(
        store.query(QueryInput::GetQueuedJobWithId{id4})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_QUEUED; })(
        store.query(QueryInput::GetQueuedJobWithId{id5})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_BLOCKED; })(
        store.query(QueryInput::GetQueuedJobWithId{id6})));

    auto jobs = store.query(QueryInput::GetAllQueuedJobs{});
    REQUIRE(jobs[0].id == id2);
    REQUIRE(jobs[1].id == id1);
    REQUIRE(jobs[2].id == id3);
    REQUIRE(jobs[3].id == id4);
    REQUIRE(jobs[4].id == id5);
    REQUIRE(jobs[5].id == id6);

    store.sortJobs();

    jobs = store.query(QueryInput::GetAllQueuedJobs{});
    REQUIRE(jobs[0].id == id2);
    REQUIRE(jobs[1].id == id1);
    REQUIRE(jobs[2].id == id3);
    REQUIRE(jobs[3].id == id4);
    REQUIRE(jobs[4].id == id5);
    REQUIRE(jobs[5].id == id6);

    store.pauseJobs();
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_PAUSED; })(
        store.query(QueryInput::GetQueuedJobWithId{id1})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_PAUSED; })(
        store.query(QueryInput::GetQueuedJobWithId{id2})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_PAUSED; })(
        store.query(QueryInput::GetQueuedJobWithId{id3})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_PAUSED; })(
        store.query(QueryInput::GetQueuedJobWithId{id4})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_PAUSED; })(
        store.query(QueryInput::GetQueuedJobWithId{id5})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_PAUSED; })(
        store.query(QueryInput::GetQueuedJobWithId{id6})));

    store.unpauseJobs();
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_QUEUED; })(
        store.query(QueryInput::GetQueuedJobWithId{id1})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_QUEUED; })(
        store.query(QueryInput::GetQueuedJobWithId{id2})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_BLOCKED; })(
        store.query(QueryInput::GetQueuedJobWithId{id3})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_BLOCKED; })(
        store.query(QueryInput::GetQueuedJobWithId{id4})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_QUEUED; })(
        store.query(QueryInput::GetQueuedJobWithId{id5})));
    REQUIRE(([&](const auto& v) { return !v.empty() && v.front().status == JobStatus::JOB_STATUS_BLOCKED; })(
        store.query(QueryInput::GetQueuedJobWithId{id6})));
}

TEST_CASE("TestJQInsertionIds")
{
    using namespace orchestrator;
    using namespace orchestrator::job_executor;
    using namespace orchestrator::job_database;
    using namespace orchestrator::job_queue;

    services::ServiceFactory<JobDatabase, JobExecutor, JobQueue> factory;

    std::this_thread::sleep_for(std::chrono::seconds(2));

    static constexpr uint32_t numInsertions = 1000;
    int64_t                   prevId        = 0;

    for (uint32_t i = 0; i < numInsertions; i++)
    {
        auto pushInput  = PushInput();
        pushInput.job   = Job();
        auto pushFuture = pushInput.getFuture();
        REQUIRE(factory.get<JobQueue>()->sendInput(std::move(pushInput)));
        auto pushResult = pushFuture.get();
        REQUIRE(std::holds_alternative<result::JobIdResult>(pushResult));
        int64_t newId = std::get<result::JobIdResult>(pushResult).id;
        REQUIRE(newId != prevId);
        prevId = newId;
    }

    factory.stop();
}

TEST_CASE("TestJQInitPush")
{
    using namespace orchestrator;

    job_queue::Store     store;
    job_queue::Container container(__handle_later{});
    job_queue::PushInput input;
    job_queue::InitState state;

    REQUIRE(state.step(store, container, input) == job_queue::InitState::index());
    REQUIRE(store.pendingJobs.empty());
}

TEST_CASE("TestJQInitQuery") {}
TEST_CASE("TestJQInitTogglePause") {}
TEST_CASE("TestJQInitDump") {}

TEST_CASE("TestJQInitWaitHeartbeat") {}
TEST_CASE("TestJQInitWaitPush") {}
TEST_CASE("TestJQInitWaitQuery") {}
TEST_CASE("TestJQInitWaitTogglePause") {}
TEST_CASE("TestJQInitWaitDump") {}

TEST_CASE("TestJQInitFinalWaitHeartbeat") {}
TEST_CASE("TestJQInitFinalWaitPush") {}
TEST_CASE("TestJQInitFinalWaitQuery") {}
TEST_CASE("TestJQInitFinalWaitTogglePause") {}
TEST_CASE("TestJQInitFinalWaitDump") {}

TEST_CASE("TestJQRunningHeartbeat") {}
TEST_CASE("TestJQRunningPush") {}
TEST_CASE("TestJQRunningQuery") {}
TEST_CASE("TestJQRunningTogglePause") {}
TEST_CASE("TestJQRunningDump") {}

TEST_CASE("TestJQPausedHeartbeat") {}
TEST_CASE("TestJQPausedPush") {}
TEST_CASE("TestJQPausedQuery") {}
TEST_CASE("TestJQPausedTogglePause") {}
TEST_CASE("TestJQPausedDump") {}
// ^^^^ TODO unit test for each of the 25 state functions, add the docstrings as you go

// ^^^^ TODO "integration-level" test
