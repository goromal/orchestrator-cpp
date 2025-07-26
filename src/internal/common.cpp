#include "orchestrator/internal/common.h"

namespace orchestrator::logger
{

logger_t& get();

void init()
{
    namespace logging = boost::log;
    namespace expr    = boost::log::expressions;
    namespace sinks   = boost::log::sinks;
    namespace attrs   = boost::log::attributes;

    using text_sink = sinks::asynchronous_sink<sinks::text_ostream_backend>;

    auto backend = boost::make_shared<sinks::text_ostream_backend>();
    backend->add_stream(boost::shared_ptr<std::ostream>(&std::clog, boost::null_deleter()));
    backend->auto_flush(true);

    auto sink = boost::make_shared<text_sink>(backend);

    sink->set_formatter(
        expr::stream << "[" << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S")
                     << "] [" << expr::attr<severity_level>("Severity") << "] " << expr::smessage);

    logging::core::get()->add_sink(sink);
    logging::add_common_attributes();
}

logger_t& get()
{
    static logger_t lg;
    return lg;
}

} // namespace orchestrator::logger
