#include "server.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string socket_name;
    std::string token;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--socket" && i + 1 < argc) socket_name = argv[++i];
        else if (argument == "--token" && i + 1 < argc) token = argv[++i];
        else {
            std::cerr << "syncerd: invalid arguments\n";
            return 2;
        }
    }
    if (socket_name.empty() || token.size() < 32) {
        std::cerr << "syncerd: --socket and --token are required\n";
        return 2;
    }
    try {
        return syncer::server::Server(std::move(socket_name), std::move(token)).run();
    } catch (const std::exception& error) {
        std::cerr << "syncerd: " << error.what() << '\n';
        return 5;
    }
}
