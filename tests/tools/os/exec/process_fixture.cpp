#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "argv") {
        for (int i = 2; i < argc; ++i)
            std::cout << '[' << argv[i] << ']';
        std::cerr << "diagnostic";
        return 2;
    }
    if (mode == "stdin") {
        char c;
        return std::cin.get(c) ? 1 : 0;
    }
    if (mode == "signal") {
        raise(SIGTERM);
        return 1;
    }
    if (mode == "utf-cut")
        std::cout << std::string(8191, 'x') << "\xe2\x82\xac";
    if (mode == "replacement-volume")
        std::cout << std::string(8192, char(0xff));
    if (mode == "alternating")
        for (int i = 0; i < 512; ++i) {
            std::cout << std::string(4096, 'o');
            std::cerr << std::string(4096, 'e');
        }
    if (mode == "pid")
        std::cout << getpid();
    if (mode == "volume") {
        std::cerr << std::string(3 * 1024 * 1024, 'e');
        std::cout << std::string(3 * 1024 * 1024, 'o');
    }
    if (mode == "bytes")
        std::cout << std::string(std::stoi(argv[2]), 'x');
    if (mode == "lines")
        for (int i = 0; i < std::stoi(argv[2]); ++i)
            std::cout << "x\n";
    if (mode == "invalid") {
        const char bytes[] = {'a', char(0xff), char(0xe2), char(0x82)};
        std::cout.write(bytes, sizeof(bytes));
    }
    if (mode == "descendant") {
        const auto child = fork();
        if (child < 0)
            return 1;
        if (child == 0) {
            for (;;)
                pause();
        }
        std::cout << child << std::endl;
        return 0;
    }
    if (mode == "wait" || mode == "closed") {
        std::cout << getpid() << std::endl;
        if (mode == "closed") {
            close(1);
            close(2);
        }
        if (argc > 2) {
            std::string temporary = std::string(argv[2]) + ".tmp";
            {
                std::ofstream ready(temporary);
                ready << getpid();
            }
            if (std::rename(temporary.c_str(), argv[2]) != 0)
                return 1;
        }
        for (;;)
            pause();
    }

    if (mode == "context") {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd)))
            return 1;
        std::cout << cwd;
        const char *value = getenv("OCTOPUS_EXEC_TEST");
        if (value)
            std::cerr << value;
    }
    return 0;
}
