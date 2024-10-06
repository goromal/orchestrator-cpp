#include <boost/test/unit_test.hpp>
#include <mscpp/ServiceFactory.h>
#include "orchestrator/JobQueue.h"

BOOST_AUTO_TEST_SUITE(TestJobQueue)

BOOST_AUTO_TEST_CASE(TestJobQueueInsertionIds)
{
    services::ServiceFactory<orchestrator::job_queue::JobQueue> factory;

    // static constexpr uint32_t numInsertions = 1000;
    // int64_t                   prevId        = 0;
    // for (uint32_t i = 0; i < numInsertions; i++)
    // {
    //     auto    jobPtr = std::make_unique<orchestrator::Job>();
    //     int64_t jobId  = factory.get<orchestrator::job_queue::JobQueue>()->push(std::move(jobPtr));
    //     BOOST_CHECK_NE(jobId, prevId);
    //     prevId = jobId;
    // }
}

BOOST_AUTO_TEST_SUITE_END()