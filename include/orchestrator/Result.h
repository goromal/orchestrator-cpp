#pragma once

#include <variant>
#include <utility>
#include <cstdint>
#include <future>

#include <aapis/orchestrator/v1/orchestrator.pb.h>

#include "orchestrator/Job.h"

namespace orchestrator
{

namespace result
{

struct EmptyResult
{
};

struct BooleanResult
{
    bool result;
};

struct JobIdResult
{
    int64_t id;
};

struct JobIdsListResult
{
    std::vector<int64_t> ids;
};

// The result of job execution: a status and either outputs or new spawned jobs
struct JobResult
{
    aapis::orchestrator::v1::JobStatus                       resultStatus;
    std::variant<std::vector<std::string>, std::vector<Job>> outputs;
};

using FutureJobResult = std::future<JobResult>;

struct JobsListResult
{
    std::vector<Job> jobs;
};

using JobQueueDataResult = std::pair<result::JobsListResult, result::JobsListResult>;

using FutureJobQueueDataResult = std::future<std::variant<services::ErrorResult, JobQueueDataResult>>;

// template<typename T>
// struct Success
// {
//     Success(const T& obj) : mObj(obj) {}
//     T&& unwrap()
//     {
//         return std::move(mObj);
//     }
//     bool success()
//     {
//         return true;
//     }
//     bool failure()
//     {
//         return false;
//     }
//     T mObj;
// };

// template<typename T>
// struct Failure
// {
//     Failure(const T& obj) : mObj(obj) {}
//     T&& unwrap()
//     {
//         return std::move(mObj);
//     }
//     bool success()
//     {
//         return false;
//     }
//     bool failure()
//     {
//         return true;
//     }
//     T mObj;
// };

// template<typename T1, typename T2>
// using Result = std::variant<Success<T1>, Failure<T2>>;

} // end namespace result

} // end namespace orchestrator
