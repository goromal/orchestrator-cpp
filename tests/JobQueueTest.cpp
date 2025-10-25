#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE JobQueueTests

#include <boost/test/unit_test.hpp>
#include <mscpp/ServiceFactory.h>
#include "orchestrator/JobQueue.h"

BOOST_AUTO_TEST_SUITE(TestJobQueue)

struct GlobalFixture
{
    GlobalFixture()
    {
        orchestrator::logger::init();
    }
    ~GlobalFixture() {}
};

BOOST_GLOBAL_FIXTURE(GlobalFixture);

BOOST_AUTO_TEST_CASE(TestJQInit)
{
    using namespace orchestrator;
    LOG(info) << "INIT";
    using namespace orchestrator::job_executor;
    using namespace orchestrator::job_database;
    using namespace orchestrator::job_queue;

    services::ServiceFactory<JobDatabase, JobExecutor, JobQueue> factory;

    std::this_thread::sleep_for(
        std::chrono::seconds(10)); // ^^^^ TODO replace with while loop waiting for certain state

    LOG(debug) << "STOPPING";
    factory.stop();
}

BOOST_AUTO_TEST_CASE(TestJQStore)
{
    using namespace orchestrator;
    using namespace aapis::orchestrator::v1;

    job_queue::Store store;

    Job job1, job2, job3, job4, job5, job6;

    job1.priority = 1;
    int64_t id1   = store.addAndRegisterNewJob(job1, false);
    BOOST_ASSERT(job1.status == JobStatus::JOB_STATUS_QUEUED);

    job2.priority = 0;
    int64_t id2   = store.addAndRegisterNewJob(job2, false);
    BOOST_CHECK_NE(id1, id2);
    BOOST_ASSERT(job2.status == JobStatus::JOB_STATUS_QUEUED);

    job3.priority = 0;
    job3.independentBlockers.push_back(id1);
    job3.relevantBlockers.push_back(id2);
    int64_t id3 = store.addAndRegisterNewJob(job3, false);
    BOOST_CHECK_NE(id2, id3);
    BOOST_ASSERT(job3.status == JobStatus::JOB_STATUS_BLOCKED);

    job4.priority = 1;
    job4.relevantBlockers.push_back(id1);
    int64_t id4 = store.addAndRegisterNewJob(job4, false);
    BOOST_CHECK_NE(id3, id4);
    BOOST_ASSERT(job4.status == JobStatus::JOB_STATUS_BLOCKED);

    job5.priority = 5;
    int64_t id5   = store.addAndRegisterNewJob(job5, false);
    BOOST_CHECK_NE(id4, id5);
    BOOST_ASSERT(job5.status == JobStatus::JOB_STATUS_QUEUED);

    job6.priority = 0;
    job6.independentBlockers.push_back(id5);
    int64_t id6 = store.addAndRegisterNewJob(job6, false);
    BOOST_CHECK_NE(id5, id6);
    BOOST_ASSERT(job6.status == JobStatus::JOB_STATUS_BLOCKED);

    store.sortJobs();

    // ^^^^ TODO check jobs order, then sort and check order again...use queryInput to get order

    store.pauseJobs();
    BOOST_ASSERT(job1.status == JobStatus::JOB_STATUS_PAUSED);
    BOOST_ASSERT(job2.status == JobStatus::JOB_STATUS_PAUSED);
    BOOST_ASSERT(job3.status == JobStatus::JOB_STATUS_PAUSED);
    BOOST_ASSERT(job4.status == JobStatus::JOB_STATUS_PAUSED);
    BOOST_ASSERT(job5.status == JobStatus::JOB_STATUS_PAUSED);
    BOOST_ASSERT(job6.status == JobStatus::JOB_STATUS_PAUSED);

    store.unpauseJobs();
    BOOST_ASSERT(job1.status == JobStatus::JOB_STATUS_QUEUED);
    BOOST_ASSERT(job2.status == JobStatus::JOB_STATUS_QUEUED);
    BOOST_ASSERT(job3.status == JobStatus::JOB_STATUS_BLOCKED);
    BOOST_ASSERT(job4.status == JobStatus::JOB_STATUS_BLOCKED);
    BOOST_ASSERT(job5.status == JobStatus::JOB_STATUS_QUEUED);
    BOOST_ASSERT(job6.status == JobStatus::JOB_STATUS_BLOCKED);
}

BOOST_AUTO_TEST_CASE(TestJQInsertionIds)
{
    using namespace orchestrator;
    using namespace orchestrator::job_executor;
    using namespace orchestrator::job_database;
    using namespace orchestrator::job_queue;

    services::ServiceFactory<JobDatabase, JobExecutor, JobQueue> factory;

    std::this_thread::sleep_for(std::chrono::seconds(6));
    LOG(debug) << "TEST 2";
    // static constexpr uint32_t numInsertions = 1000;
    static constexpr uint32_t numInsertions = 10;
    int64_t                   prevId        = 0;
    for (uint32_t i = 0; i < numInsertions; i++)
    {
        auto pushInput  = PushInput();
        pushInput.job   = Job();
        auto pushFuture = pushInput.getFuture();
        LOG(debug) << "hm";
        BOOST_ASSERT(factory.get<JobQueue>()->sendInput(std::move(pushInput)));
        LOG(debug) << "lets";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        LOG(debug) << "wait";
        auto pushResult = pushFuture.get();
        LOG(debug) << "assert check incoming";
        BOOST_ASSERT(std::holds_alternative<result::JobIdResult>(pushResult));
        LOG(debug) << "made it!"; // ^^^^ ?
        int64_t newId = std::get<result::JobIdResult>(pushResult).id;
        BOOST_CHECK_NE(newId, prevId);
        prevId = newId;
    }

    factory.stop();
}

BOOST_AUTO_TEST_CASE(TestJQInitHeartbeat) {}
BOOST_AUTO_TEST_CASE(TestJQInitPush) {}
BOOST_AUTO_TEST_CASE(TestJQInitQuery) {}
BOOST_AUTO_TEST_CASE(TestJQInitTogglePause) {}
BOOST_AUTO_TEST_CASE(TestJQInitDump) {}

BOOST_AUTO_TEST_CASE(TestJQInitWaitHeartbeat) {}
BOOST_AUTO_TEST_CASE(TestJQInitWaitPush) {}
BOOST_AUTO_TEST_CASE(TestJQInitWaitQuery) {}
BOOST_AUTO_TEST_CASE(TestJQInitWaitTogglePause) {}
BOOST_AUTO_TEST_CASE(TestJQInitWaitDump) {}

BOOST_AUTO_TEST_CASE(TestJQInitFinalWaitHeartbeat) {}
BOOST_AUTO_TEST_CASE(TestJQInitFinalWaitPush) {}
BOOST_AUTO_TEST_CASE(TestJQInitFinalWaitQuery) {}
BOOST_AUTO_TEST_CASE(TestJQInitFinalWaitTogglePause) {}
BOOST_AUTO_TEST_CASE(TestJQInitFinalWaitDump) {}

BOOST_AUTO_TEST_CASE(TestJQRunningHeartbeat) {}
BOOST_AUTO_TEST_CASE(TestJQRunningPush) {}
BOOST_AUTO_TEST_CASE(TestJQRunningQuery) {}
BOOST_AUTO_TEST_CASE(TestJQRunningTogglePause) {}
BOOST_AUTO_TEST_CASE(TestJQRunningDump) {}

BOOST_AUTO_TEST_CASE(TestJQPausedHeartbeat) {}
BOOST_AUTO_TEST_CASE(TestJQPausedPush) {}
BOOST_AUTO_TEST_CASE(TestJQPausedQuery) {}
BOOST_AUTO_TEST_CASE(TestJQPausedTogglePause) {}
BOOST_AUTO_TEST_CASE(TestJQPausedDump) {}
// ^^^^ TODO unit test for each of the 25 state functions, add the docstrings as you go

// ^^^^ TODO "integration-level" test

BOOST_AUTO_TEST_SUITE_END()