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

BOOST_AUTO_TEST_CASE(TestJobQueueInit)
{
    using namespace orchestrator;
    LOG(info) << "INIT";
    using namespace orchestrator::job_executor;
    using namespace orchestrator::job_database;
    using namespace orchestrator::job_queue;

    services::ServiceFactory<JobDatabase, JobExecutor, JobQueue> factory;

    std::this_thread::sleep_for(std::chrono::seconds(10));
    LOG(debug) << "STOPPING";
    factory.stop();
}

BOOST_AUTO_TEST_CASE(TestJobQueueInsertionIds)
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
        LOG(debug) << "hm"; // ^^^^
        BOOST_ASSERT(factory.get<JobQueue>()->sendInput(std::move(pushInput)));
        LOG(debug) << "lets";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        LOG(debug) << "wait";
        auto pushResult = pushFuture.get();
        BOOST_ASSERT(std::holds_alternative<result::JobIdResult>(pushResult));
        int64_t newId = std::get<result::JobIdResult>(pushResult).id;
        BOOST_CHECK_NE(newId, prevId);
        prevId = newId;
    }

    factory.stop();
}

BOOST_AUTO_TEST_SUITE_END()