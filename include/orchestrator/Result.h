#include <variant>
#include <utility>

namespace orchestrator
{

namespace result
{

template<typename T>
struct Success
{
    Success(const T& obj) : mObj(obj) {}
    T&& unwrap()
    {
        return std::move(mObj);
    }
    bool success()
    {
        return true;
    }
    bool failure()
    {
        return false;
    }
    T mObj;
};

template<typename T>
struct Failure
{
    Failure(const T& obj) : mObj(obj) {}
    T&& unwrap()
    {
        return std::move(mObj);
    }
    bool success()
    {
        return false;
    }
    bool failure()
    {
        return true;
    }
    T mObj;
};

template<typename T1, typename T2>
using Result = std::variant<Success<T1>, Failure<T2>>;

} // end namespace result

} // end namespace orchestrator
