#include "include/net/accepter.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <chrono>
#include <thread>

int main()
{
    try
    {
        Acceptor acceptor(9000);

        while (true)
        {
            sockaddr_in client{};
            socklen_t len = sizeof(client);

            int client_fd = ::accept4(
                acceptor.fd(),
                (sockaddr *)&client,
                &len,
                SOCK_NONBLOCK);

            if (client_fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("accept4() failed");
            }

            std::cout << "Accepted fd=" << client_fd << "\n";
            ::close(client_fd);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
