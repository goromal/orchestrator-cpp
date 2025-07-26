#pragma once
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/async_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/attributes/scoped_attribute.hpp>
#include <boost/log/attributes/current_thread_id.hpp>
#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/core/null_deleter.hpp>

#include <boost/make_shared.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

namespace orchestrator
{

// Severity levels
enum severity_level
{
    trace,
    debug,
    info,
    warning,
    error,
    fatal
};

// Output stream overload for severity
inline std::ostream& operator<<(std::ostream& os, severity_level lvl)
{
    static const char* strings[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    if (static_cast<std::size_t>(lvl) < sizeof(strings) / sizeof(*strings))
        os << strings[lvl];
    else
        os << static_cast<int>(lvl);
    return os;
}

#define LOG(sev)                                                                                                       \
    {                                                                                                                  \
        BOOST_LOG_SCOPED_THREAD_TAG("File", __FILE__);                                                                 \
    }                                                                                                                  \
    {                                                                                                                  \
        BOOST_LOG_SCOPED_THREAD_TAG("Line", __LINE__);                                                                 \
    }                                                                                                                  \
    BOOST_LOG_SEV(logger::get(), sev)

namespace logger
{

using logger_t = boost::log::sources::severity_logger_mt<severity_level>;

void      init();
logger_t& get();

} // namespace logger

} // namespace orchestrator
