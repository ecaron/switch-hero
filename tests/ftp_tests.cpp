// FTP mode: the server is driven over real sockets, the way a client drives
// it. Most of these checks are about the one promise it makes - that nothing
// outside the songs folder can be reached - and the rest about the commands
// FileZilla, WinSCP and the command-line clients actually send.
#include "ftp.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

using namespace fret;
int checks = 0;
void check(bool value, const std::string &message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}

int connectTo(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0)
        throw std::runtime_error("cannot connect");
    return fd;
}

struct Client {
    int fd;
    std::string pending;
    explicit Client(uint16_t port) : fd(connectTo(port)) { reply(); }
    ~Client() { close(fd); }
    std::string line() {
        for (;;) {
            const size_t eol = pending.find("\r\n");
            if (eol != std::string::npos) {
                std::string l = pending.substr(0, eol);
                pending.erase(0, eol + 2);
                return l;
            }
            char buf[512];
            const ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n <= 0)
                throw std::runtime_error("control connection closed");
            pending.append(buf, size_t(n));
        }
    }
    // A whole reply, multi-line ones included; returns its last line.
    std::string reply() {
        std::string l = line();
        if (l.size() > 3 && l[3] == '-') {
            const std::string end = l.substr(0, 3) + " ";
            while (l.compare(0, 4, end) != 0)
                l = line();
        }
        return l;
    }
    std::string cmd(const std::string &c) {
        const std::string out = c + "\r\n";
        send(fd, out.data(), out.size(), 0);
        return reply();
    }
    int pasv() {
        const std::string r = cmd("EPSV");
        check(r.compare(0, 3, "229") == 0, "EPSV: " + r);
        const size_t a = r.find("|||"), b = r.find('|', a + 3);
        return connectTo(uint16_t(std::stoi(r.substr(a + 3, b - a - 3))));
    }
    // A transfer command with its data: returns the final reply and what came back.
    std::string transfer(const std::string &c, std::string *data, const std::string &upload = "") {
        const int d = pasv();
        const std::string first = cmd(c);
        if (first[0] != '1') {
            close(d);
            return first;
        }
        if (!upload.empty())
            send(d, upload.data(), upload.size(), 0);
        if (data) {
            char buf[4096];
            ssize_t n;
            while ((n = recv(d, buf, sizeof buf, 0)) > 0)
                data->append(buf, size_t(n));
        }
        close(d);
        return reply();
    }
};
bool starts(const std::string &s, const char *code) { return s.compare(0, 3, code) == 0; }
std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    try {
        // Path resolution: ".." stops at the root like "/" on a real server.
        check(ftpResolve("/", "") == "/", "empty is the current folder");
        check(ftpResolve("/", "..") == "/", ".. at the root stays at the root");
        check(ftpResolve("/a/b", "../../../../etc/passwd") == "/etc/passwd", "climbing stops at the root");
        check(ftpResolve("/a", "b/./c//d") == "/a/b/c/d", "dots and doubled slashes");
        check(ftpResolve("/a", "/x/../y") == "/y", "absolute paths start from the root");
        check(ftpResolve("/a", "..\\..\\x") == "/x", "backslashes are separators");
        check(ftpResolve("/", "...") == "/...", "three dots is an ordinary name");

        const fs::path base = fs::temp_directory_path() / ("switch-hero-ftp-" + std::to_string(getpid()));
        fs::remove_all(base);
        const fs::path root = base / "songs";
        fs::create_directories(root);
        std::ofstream(base / "secret.txt") << "outside";
        fs::create_directory_symlink(base, root / "escape-link");

        FtpServer server(root);
        server.start(0);
        const uint16_t port = server.status().port;
        check(server.status().running && port != 0, "server listening");
        {
            Client c(port);
            check(starts(c.cmd("LIST"), "530"), "nothing before login");
            check(starts(c.cmd("USER anonymous"), "331"), "anonymous user");
            check(starts(c.cmd("PASS guest@"), "230"), "any password");
            check(c.cmd("PWD") == "257 \"/\" is the current folder.", "starts at the root");
            check(starts(c.cmd("CWD .."), "250") && c.cmd("PWD").find("\"/\"") != std::string::npos,
                  "cannot go above the root");
            check(starts(c.cmd("FEAT"), "211"), "features listed");
            check(starts(c.cmd("TYPE I"), "200"), "binary");

            // Copying a song in, the way a client does: folder first, then files.
            check(starts(c.cmd("MKD Artist - Song"), "257"), "make a song folder");
            check(starts(c.cmd("CWD Artist - Song"), "250"), "enter it");
            std::string payload(300000, 'x');
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] = char('a' + i % 26);
            check(starts(c.transfer("STOR notes.chart", nullptr, payload), "226"), "upload");
            check(slurp(root / "Artist - Song" / "notes.chart") == payload, "uploaded bytes on disk");
            check(c.cmd("SIZE notes.chart") == "213 " + std::to_string(payload.size()), "size");
            check(starts(c.cmd("MDTM notes.chart"), "213"), "modification time");
            std::string listing, back;
            check(starts(c.transfer("LIST -la", &listing), "226"), "list");
            check(listing.find("notes.chart") != std::string::npos && listing.find("-rw-r--r--") == 0,
                  "listing shows the file: " + listing);
            check(starts(c.transfer("RETR notes.chart", &back), "226") && back == payload, "download");
            check(server.status().received == payload.size() && server.status().sent == payload.size(),
                  "byte counters");

            // Escapes. Every one of these must stay inside the songs folder.
            std::string leaked;
            check(starts(c.transfer("RETR ../../secret.txt", &leaked), "550") && leaked.empty(),
                  ".. cannot read outside");
            check(starts(c.transfer("RETR /../secret.txt", &leaked), "550") && leaked.empty(),
                  "/.. cannot read outside");
            check(starts(c.transfer("STOR ../../../planted.txt", nullptr, "bad"), "226"), "clamped upload accepted");
            check(!fs::exists(base / "planted.txt") && fs::exists(root / "planted.txt"),
                  "a .. upload lands inside the songs folder");
            check(starts(c.cmd("CWD /escape-link"), "550"), "a symlink out is not a folder here");
            check(starts(c.transfer("RETR /escape-link/secret.txt", &leaked), "550") && leaked.empty(),
                  "nothing is read through a symlink out");
            check(starts(c.transfer("LIST /escape-link", &leaked), "550"), "nothing is listed through a symlink out");
            check(starts(c.transfer("STOR /escape-link/planted.txt", nullptr, "bad"), "553") &&
                      !fs::exists(base / "planted.txt"),
                  "nothing is written through a symlink out");
            check(starts(c.cmd("RMD /"), "550") && starts(c.cmd("DELE /"), "550") && starts(c.cmd("RNFR /"), "550"),
                  "the root itself cannot be removed or renamed");
            check(slurp(base / "secret.txt") == "outside", "outside file untouched");

            // Tidying up, as a client would.
            check(starts(c.cmd("RNFR notes.chart"), "350") && starts(c.cmd("RNTO song.chart"), "250"), "rename");
            check(fs::exists(root / "Artist - Song" / "song.chart"), "renamed on disk");
            check(starts(c.cmd("CDUP"), "250"), "up a level");
            check(starts(c.cmd("RMD Artist - Song"), "550"), "a folder with files in is not removed");
            check(starts(c.cmd("DELE Artist - Song/song.chart"), "250"), "delete file");
            check(starts(c.cmd("RMD Artist - Song"), "250") && !fs::exists(root / "Artist - Song"), "remove folder");
            check(starts(c.cmd("PORT 127,0,0,1,4,1"), "502"), "active mode refused politely");
            check(server.status().changes >= 5, "changes counted");
            check(starts(c.cmd("QUIT"), "221"), "goodbye");
        }
        {
            // Stopping with a client still connected returns promptly.
            Client c(port);
            c.cmd("USER a"), c.cmd("PASS b");
            const auto t = std::chrono::steady_clock::now();
            server.stop();
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
            check(took < 2 && !server.status().running, "stop is prompt");
        }
        server.start(0);
        check(server.status().running && server.status().changes == 0, "restarts fresh");
        server.stop();
        fs::remove_all(base);
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    std::cout << "FTP: " << checks << " checks passed\n";
    return 0;
}
