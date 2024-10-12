#pragma once

#include <variant>
#include <utility>
#include <cstdint>
#include <future>

#include <aapis/orchestrator/v1/orchestrator.pb.h>

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

struct JobResult
{
    aapis::orchestrator::v1::JobStatus resultStatus;
    std::vector<std::string>           outputs;
};

using FutureJobResult = std::future<JobResult>;

struct JobsListResult
{
    // ^^^^ TODO
};

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
