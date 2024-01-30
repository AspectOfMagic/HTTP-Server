HTTP-Server
- Author of httpserver.c: Jason Wu
- Author of connection.h, debug.h, helper_funcs.a, helper_funcs.h, queue.h, request.h, response.h, and rwlock.h: Andrew Quinn

==========[Introduction]

The program, written entirely in c, is a multi-threaded HTTP server that handles basic HTTP get and put requests from clients. Compared to traditional one-threaded servers, it adds a thread-safe queue and some rwlocks based on the mutex lock to serve multiple clients simultaneously. In addition, the server ensures that its responses are in a coherent and atomic order of the client requests, meaning that an outside observer could not differentiate the behavior of this server from a server that uses only a single thread.

==========[Installation]

You will need Linux x86_64

==========[Usage]

$ ./httpserver [-t threads] \<port\>

- threads: number of worker threads to use. This argument is optional(defaulting to 4).
- port: required argument that specifies which port the server will be listening on.

==========[Todo]

- Allows user to specify a range of ports
- Allows user to specify the number of threads to support client requests
