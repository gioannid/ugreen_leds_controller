#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

static void send_line(int fd, const std::string& line) {
    std::string msg = line + "\n";
    write(fd, msg.data(), msg.size());
}

static std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n') break;
        line += c;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

int main(int argc, char* argv[]) {
    int fd = -1;
    if (argc > 1 && std::string(argv[1]) == "--tcp") {
        std::string ip = (argc > 2) ? argv[2] : "127.0.0.1";
        int port = (argc > 3) ? std::stoi(argv[3]) : 39001;

        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        std::cout << "Connecting to TCP " << ip << ":" << port << "...\n";
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "TCP connection failed.\n";
            return 1;
        }
    } else {
        std::string sock_path = (argc > 1) ? argv[1] : "/tmp/ugreen_leds.sock";
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

        std::cout << "Connecting to Unix socket " << sock_path << "...\n";
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "Unix socket connection failed.\n";
            return 1;
        }
    }

    std::cout << "Connected!\n";

    // 1. Send HELLO
    std::cout << "-> HELLO 1 1\n";
    send_line(fd, "HELLO 1 1");
    std::cout << "<- " << read_line(fd) << "\n";

    // 2. Query CAPS
    std::cout << "-> CAPS 2\n";
    send_line(fd, "CAPS 2");
    std::cout << "<- " << read_line(fd) << "\n";

    // 3. SET LED state
    std::cout << "-> SET 3 disk1 mode=on brightness=128 rgb=0,255,0\n";
    send_line(fd, "SET 3 disk1 mode=on brightness=128 rgb=0,255,0");
    std::cout << "<- " << read_line(fd) << "\n";

    // 4. FLUSH
    std::cout << "-> FLUSH 4\n";
    send_line(fd, "FLUSH 4");
    std::cout << "<- " << read_line(fd) << "\n";

    // 5. GET status
    std::cout << "-> GET 5 disk1 source=desired\n";
    send_line(fd, "GET 5 disk1 source=desired");
    std::cout << "<- " << read_line(fd) << "\n";

    // 6. RESET
    std::cout << "-> RESET 6 disk1\n";
    send_line(fd, "RESET 6 disk1");
    std::cout << "<- " << read_line(fd) << "\n";

    close(fd);
    std::cout << "Done.\n";
    return 0;
}
