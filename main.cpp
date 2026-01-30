#include "include/common/logger.h"

int main()
{
    logging::Logger::instance().init(
        "logs/server.log",
        logging::LogLevel::DEBUG,
        10 * 1024 * 1024,
        3,
        8192,
        logging::QueuePolicy::BLOCK);

    LOG_INFO("Server starting");
    LOG_DEBUG("Listening on port 6379");
    LOG_WARN("High memory usage");
    LOG_ERROR("Connection failed");

    return 0;
}
