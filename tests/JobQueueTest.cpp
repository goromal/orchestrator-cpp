#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#pragma GCC diagnostic pop

#include <mscpp/ServiceFactory.h>
#include "orchestrator/JobQueue.h"

TEST_CASE("TestJQInit")
{
    using namespace orchestrator;
    LOG_WARN("INIT");
    using namespace orchestrator::job_executor;
    using namespace orchestrator::job_database;
    using namespace orchestrator::job_queue;

    services::ServiceFactory<JobDatabase, JobExecutor, JobQueue> factory;

    std::this_thread::sleep_for(
        std::chrono::seconds(10)); // ^^^^ TODO replace with while loop waiting for certain state

    LOG_WARN("STOPPING");
    factory.stop();
}

TEST_CASE("TestJQStore")
{
    using namespace orchestrator;
    using namespace aapis::orchestrator::v1;

    job_queue::Store store;

    Job job1, job2, job3, job4, job5, job6;

    job1.priority = 1;
    int64_t id1   = store.addAndRegisterNewJob(job1, false);
    REQUIRE(job1.status == JobStatus::JOB_STATUS_QUEUED);

    job2.priority = 0;
    int64_t id2   = store.addAndRegisterNewJob(job2, false);
    REQUIRE(id1 != id2);
    REQUIRE(job2.status == JobStatus::JOB_STATUS_QUEUED);

    job3.priority = 0;
    job3.independentBlockers.push_back(id1);
    job3.relevantBlockers.push_back(id2);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);
    REQUIRE(id2 != id3);
    REQUIRE(job3.status == JobStatus::JOB_STATUS_BLOCKED);

    job4.priority = 1;
    job4.relevantBlockers.push_back(id1);
    int64_t id4 = store.addAndRegisterNewJob(job4, false);
    REQUIRE(id3 != id4);
    REQUIRE(job4.status == JobStatus::JOB_STATUS_BLOCKED);

    job5.priority = 5;
    int64_t id5   = store.addAndRegisterNewJob(job5, false);
    REQUIRE(id4 != id5);
    REQUIRE(job5.status == JobStatus::JOB_STATUS_QUEUED);

    job6.priority = 0;
    job6.independentBlockers.push_back(id5);
    int64_t id6 = store.addAndRegisterNewJob(job6, false);
    REQUIRE(id5 != id6);
    REQUIRE(job6.status == JobStatus::JOB_STATUS_BLOCKED);

    store.sortJobs();

    // ^^^^ TODO check jobs order, then sort and check order again...use queryInput to get order

    store.pauseJobs();
    REQUIRE(job1.status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(job2.status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(job3.status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(job4.status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(job5.status == JobStatus::JOB_STATUS_PAUSED);
    REQUIRE(job6.status == JobStatus::JOB_STATUS_PAUSED);

    store.unpauseJobs();
    REQUIRE(job1.status == JobStatus::JOB_STATUS_QUEUED);
    REQUIRE(job2.status == JobStatus::JOB_STATUS_QUEUED);
    REQUIRE(job3.status == JobStatus::JOB_STATUS_BLOCKED);
    REQUIRE(job4.status == JobStatus::JOB_STATUS_BLOCKED);
    REQUIRE(job5.status == JobStatus::JOB_STATUS_QUEUED);
    REQUIRE(job6.status == JobStatus::JOB_STATUS_BLOCKED);
}

TEST_CASE("TestJQInsertionIds")
{
    using namespace orchestrator;
    using namespace orchestrator::job_executor;
    using namespace orchestrator::job_database;
    using namespace orchestrator::job_queue;

    services::ServiceFactory<JobDatabase, JobExecutor, JobQueue> factory;

    std::this_thread::sleep_for(std::chrono::seconds(6));
    LOG_DEBUG("TEST 2");
    // static constexpr uint32_t numInsertions = 1000;
    static constexpr uint32_t numInsertions = 10;
    int64_t                   prevId        = 0;
    for (uint32_t i = 0; i < numInsertions; i++)
    {
        auto pushInput  = PushInput();
        pushInput.job   = Job();
        auto pushFuture = pushInput.getFuture();
        LOG_DEBUG("hm");
        REQUIRE(factory.get<JobQueue>()->sendInput(std::move(pushInput)));
        LOG_DEBUG("lets");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        LOG_DEBUG("wait");
        auto pushResult = pushFuture.get();
        LOG_DEBUG("assert check incoming");
        REQUIRE(std::holds_alternative<result::JobIdResult>(pushResult));
        LOG_DEBUG("made it!"); // ^^^^ ?
        int64_t newId = std::get<result::JobIdResult>(pushResult).id;
        REQUIRE(newId != prevId);
        prevId = newId;
    }

    factory.stop();
}

TEST_CASE("TestJQInitHeartbeat") {}
TEST_CASE("TestJQInitPush") {}
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
