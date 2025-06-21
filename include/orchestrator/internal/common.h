#pragma once
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/attributes/scoped_attribute.hpp>
#include <boost/log/attributes/mutable_constant.hpp>
#include <boost/log/attributes/current_thread_id.hpp>
#include <boost/log/sinks/async_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/support/date_time.hpp>

#include <boost/make_shared.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

namespace orchestrator
{

// Define severity levels (optional if using BOOST_LOG_TRIVIAL only)
enum severity_level
{
    trace,
    debug,
    info,
    warning,
    error,
    fatal
};

// Pretty print severity_level
std::ostream& operator<<(std::ostream& strm, severity_level lvl)
{
    static const char* strings[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    if (static_cast<std::size_t>(lvl) < sizeof(strings) / sizeof(*strings))
        strm << strings[lvl];
    else
        strm << static_cast<int>(lvl);
    return strm;
}

void init_logging()
{
    namespace logging = boost::log;
    namespace sinks   = boost::log::sinks;
    namespace expr    = boost::log::expressions;
    namespace attrs   = boost::log::attributes;

    using text_sink = sinks::asynchronous_sink<sinks::text_ostream_backend>;

    auto sink = boost::make_shared<text_sink>();
    sink->locked_backend()->add_stream(boost::make_shared<std::ostream>(std::clog.rdbuf()));

    sink->set_formatter(
        expr::stream << "[" << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S")
                     << "] [" << expr::attr<boost::log::trivial::severity_level>("Severity") << "] [line "
                     << expr::attr<unsigned int>("Line") << "] [" << expr::attr<std::string>("Function") << "] "
                     << expr::smessage);

    logging::core::get()->add_sink(sink);
    logging::add_common_attributes();

    // Predeclare line and function as scoped/mutable tags
    logging::core::get()->add_global_attribute("Line", attrs::mutable_constant<unsigned int>(0));
    logging::core::get()->add_global_attribute("Function", attrs::mutable_constant<std::string>(""));
}

// Logging macro that injects line and function into current thread context
#define LOG(sev)                                                                                                       \
    BOOST_LOG_SCOPED_THREAD_TAG("Line", __LINE__);                                                                     \
    BOOST_LOG_SCOPED_THREAD_TAG("Function", BOOST_CURRENT_FUNCTION);                                                   \
    BOOST_LOG_TRIVIAL(sev)

} // namespace orchestrator
