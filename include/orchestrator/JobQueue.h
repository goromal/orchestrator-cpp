#pragma once

#include <string>
#include <mscpp/MicroService.h>
#include <mscpp/MicroServiceContainer.h>

namespace orchestrator
{

class JobQueue : public MicroService
{
public:
    using Container = MicroServiceContainer<>;

    JobQueue(const Container& container) : mContainer{container} {}
    ~JobQueue() {}

    std::string name() const override;

    // TODO

private:
    const Container mContainer;

    // TODO
};

} // end namespace orchestrator
