// ananas-engine: the headless slicing engine. Reads one JSON-RPC 2.0 request per line on stdin and writes
// one response (or notification) per line on the protocol channel. Logs go to stderr.
//
//   ananas-engine --profiles <resources/profiles> [--vendor <name>] [--log-level <0-5>]
//
// The protocol channel is a duplicate of the original stdout; fd 1 is then pointed at stderr, so a library
// that prints to stdout can never corrupt a response.

// nanosvg is header-only and libslic3r's SVG import needs its implementation in exactly one translation
// unit; it has to come before any header that includes nanosvg.h without the macro (its include guard
// would suppress the implementation). Same pattern as the profile validator and the test mains.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include "Engine.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/trivial.hpp>
#include <boost/program_options.hpp>

#include <cstdio>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace po = boost::program_options;

namespace {

void log_to_stderr()
{
    namespace logging = boost::log;
    namespace sinks   = boost::log::sinks;
    namespace expr    = boost::log::expressions;

    auto backend = boost::make_shared<sinks::text_ostream_backend>();
    backend->add_stream(boost::shared_ptr<std::ostream>(&std::clog, boost::null_deleter()));
    backend->auto_flush(true);
    auto sink = boost::make_shared<sinks::synchronous_sink<sinks::text_ostream_backend>>(backend);
    sink->set_formatter([](const logging::record_view& record, logging::formatting_ostream& stream) {
        stream << "[" << record[logging::trivial::severity] << "] " << record[expr::smessage];
    });
    logging::core::get()->remove_all_sinks();
    logging::core::get()->add_sink(sink);
}

FILE* open_protocol_channel()
{
#ifndef _WIN32
    const int fd = dup(STDOUT_FILENO);
    if (fd >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0)
        if (FILE* channel = fdopen(fd, "w"))
            return channel;
#endif
    return stdout;
}

void send(FILE* channel, const nlohmann::json& message)
{
    const std::string line = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    fwrite(line.data(), 1, line.size(), channel);
    fflush(channel);
}

} // namespace

int main(int argc, char* argv[])
{
    po::options_description options("ananas-engine");
    options.add_options()("help,h", "help")("profiles,p", po::value<std::string>(), "resources/profiles folder (required)")(
        "vendor,v", po::value<std::string>()->default_value(""), "load only this vendor (and the filament library)")(
        "log-level,l", po::value<int>()->default_value(2), "log level, 2 = warning");
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, options), vm);
        if (vm.count("help")) {
            std::cerr << options << "\n";
            return 0;
        }
        po::notify(vm);
    } catch (const po::error& ex) {
        std::cerr << "error: " << ex.what() << "\n" << options << "\n";
        return 2;
    }
    if (!vm.count("profiles")) {
        std::cerr << "error: --profiles is required\n";
        return 2;
    }

    log_to_stderr();
    Slic3r::set_logging_level(vm["log-level"].as<int>());
    FILE* channel = open_protocol_channel();

    std::unique_ptr<Slic3r::engine::Engine> engine;
    try {
        engine = std::make_unique<Slic3r::engine::Engine>(vm["profiles"].as<std::string>(), vm["vendor"].as<std::string>());
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }

    const auto notify = [channel](const nlohmann::json& message) { send(channel, message); };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        nlohmann::json request = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (request.is_discarded()) {
            send(channel, { { "jsonrpc", "2.0" },
                            { "id", nullptr },
                            { "error", { { "code", static_cast<int>(Slic3r::engine::ErrorCode::ParseError) }, { "message", "invalid JSON" } } } });
            continue;
        }
        const nlohmann::json response = engine->handle(request, notify);
        if (!response.is_null())
            send(channel, response);
    }
    return 0;
}
