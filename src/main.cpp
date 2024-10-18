#include <boost/program_options.hpp>
#include <iostream>
#include <mscpp/ServiceFactory.h>
#include "orchestrator/JobQueue.h"

int main(int argc, char* argv[])
{
    uint32_t port_number = 4444;
    uint32_t num_threads = 4;

    boost::program_options::options_description args_desc("Options");
    // clang-format off
    args_desc.add_options()
        ("help,h", "print usage")
        ("port,p", boost::program_options::value<uint32_t>(), "Port to serve requests on")
        ("num-allowed-threads,n", boost::program_options::value<uint32_t>(), "Number of concurrent threads to leverage");
    // clang-format on

    boost::program_options::variables_map vm;
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, args_desc), vm);
    boost::program_options::notify(vm);

    if (vm.count("help"))
    {
        std::cout << args_desc << std::endl;
        return 0;
    }

    if (vm.count("port"))
    {
        port_number = vm["port"].as<uint32_t>();
    }
    if (vm.count("num-allowed-threads"))
    {
        num_threads = vm["num-allowed-threads"].as<uint32_t>();
    }

    services::ServiceFactory<orchestrator::job_queue::JobQueue> factory;

    // TODO initialization

    return 0;
}
