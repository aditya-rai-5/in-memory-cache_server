#include "include/net/acceptor.h"
#include "include/net/event_loop.h"

int main()
{
     Acceptor acceptor(9000);
     EventLoop loop(acceptor);
     loop.run();
}
