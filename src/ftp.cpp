#include "ftp.hpp"
#include "background.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#ifdef __SWITCH__
// libnx answers this from the network service with the console's IP address;
// newlib's header hides the declaration in strict C++ mode.
extern "C" long gethostid(void);
#endif

namespace fret {
namespace {
constexpr int MaxClients = 4;
constexpr size_t MaxLine = 4096;
constexpr int PollMs = 250;                // how often blocked loops look at the stop flag
constexpr int IdleSeconds = 600;           // a silent control connection is closed after this
constexpr int DataSeconds = 30;            // a transfer that stalls this long is abandoned
constexpr size_t TransferBuffer = 64 * 1024; // SD card writes are slow in small pieces

void closeSocket(int &fd) {
    if (fd >= 0)
        close(fd);
    fd = -1;
}

// The address other machines reach the console on, or empty when it has none.
std::string localAddress() {
    in_addr a{};
#ifdef __SWITCH__
    a.s_addr = in_addr_t(gethostid());
#else
    // Connecting a UDP socket sends nothing; it only picks the outgoing interface.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return {};
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(9);
    probe.sin_addr.s_addr = htonl(0xC0000201); // 192.0.2.1, a documentation address
    sockaddr_in mine{};
    socklen_t len = sizeof mine;
    if (connect(fd, reinterpret_cast<sockaddr *>(&probe), sizeof probe) == 0 &&
        getsockname(fd, reinterpret_cast<sockaddr *>(&mine), &len) == 0)
        a = mine.sin_addr;
    close(fd);
#endif
    const uint32_t host = ntohl(a.s_addr);
    if (host == 0 || (host >> 24) == 127)
        return {};
    char text[INET_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET, &a, text, sizeof text) ? text : std::string();
}

std::string listDate(time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char out[32];
    const time_t now = std::time(nullptr);
    // As ls does: the time for recent files, the year for old ones.
    const bool recent = t <= now + 3600 && now - t < 180L * 24 * 3600;
    std::strftime(out, sizeof out, recent ? "%b %e %H:%M" : "%b %e  %Y", &tm);
    return out;
}
} // namespace

struct FtpServer::Impl {
    fs::path root;
    std::atomic<bool> stopping{false};
    int listener = -1;
    uint16_t port = 0;
    std::string address;
    std::thread acceptor;

    struct Client {
        int fd = -1;
        std::thread thread;
        std::atomic<bool> done{false};
    };
    mutable std::mutex mutex; // clients, last, error
    std::vector<std::unique_ptr<Client>> clients;
    std::string last, error;
    std::atomic<uint64_t> received{0}, sent{0};
    std::atomic<int> changes{0};

    void note(const std::string &what) {
        std::lock_guard<std::mutex> lock(mutex);
        last = what;
    }

    // The real path behind a virtual one, or empty if it would leave the root
    // (only possible through a symlink on a desktop file system).
    fs::path real(const std::string &virt) const {
        fs::path p = root;
        if (virt.size() > 1)
            p /= fs::path(virt.substr(1));
#ifndef __SWITCH__
        std::error_code ec;
        const fs::path base = fs::weakly_canonical(root, ec);
        const fs::path full = fs::weakly_canonical(p, ec);
        if (ec)
            return {};
        if (std::mismatch(base.begin(), base.end(), full.begin(), full.end()).first != base.end())
            return {};
#endif
        return p;
    }

    void acceptLoop() {
        runInBackground();
        while (!stopping) {
            pollfd p{listener, POLLIN, 0};
            if (poll(&p, 1, PollMs) <= 0 || !(p.revents & POLLIN))
                continue;
            int fd = accept(listener, nullptr, nullptr);
            if (fd < 0)
                continue;
            std::lock_guard<std::mutex> lock(mutex);
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [](auto &c) {
                                             if (!c->done)
                                                 return false;
                                             c->thread.join();
                                             return true;
                                         }),
                          clients.end());
            if (int(clients.size()) >= MaxClients) {
                const char busy[] = "421 Too many connections, try again later.\r\n";
                send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL);
                close(fd);
                continue;
            }
            auto c = std::make_unique<Client>();
            c->fd = fd;
            Client *raw = c.get();
            c->thread = std::thread([this, raw] {
                runInBackground();
                Session(*this, raw->fd).run();
                raw->done = true;
            });
            clients.push_back(std::move(c));
        }
    }

    // One control connection.
    struct Session {
        Impl &server;
        int control;
        int passive = -1;
        std::string cwd = "/", renameFrom, buffer;
        bool user = false, loggedIn = false;
        Session(Impl &s, int fd) : server(s), control(fd) {}
        ~Session() {
            closeSocket(passive);
            closeSocket(control);
        }

        bool sendAll(int fd, const char *data, size_t size) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DataSeconds);
            while (size) {
                if (server.stopping || std::chrono::steady_clock::now() > deadline)
                    return false;
                pollfd p{fd, POLLOUT, 0};
                if (poll(&p, 1, PollMs) <= 0)
                    continue;
                const ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
                if (n <= 0)
                    return false;
                data += n, size -= size_t(n);
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DataSeconds);
            }
            return true;
        }
        bool reply(const std::string &text) { return sendAll(control, (text + "\r\n").c_str(), text.size() + 2); }

        // The next command line, or false when the client went away, idled out
        // or the server is stopping.
        bool readLine(std::string &line) {
            auto idleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(IdleSeconds);
            for (;;) {
                const size_t eol = buffer.find('\n');
                if (eol != std::string::npos) {
                    line = buffer.substr(0, eol);
                    buffer.erase(0, eol + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    return true;
                }
                if (buffer.size() > MaxLine || server.stopping || std::chrono::steady_clock::now() > idleUntil)
                    return false;
                pollfd p{control, POLLIN, 0};
                if (poll(&p, 1, PollMs) <= 0)
                    continue;
                char chunk[512];
                const ssize_t n = recv(control, chunk, sizeof chunk, 0);
                if (n <= 0)
                    return false;
                buffer.append(chunk, size_t(n));
            }
        }

        // Opens a listening socket on the address the client reached us on.
        bool openPassive(sockaddr_in &where) {
            closeSocket(passive);
            socklen_t len = sizeof where;
            if (getsockname(control, reinterpret_cast<sockaddr *>(&where), &len) != 0)
                return false;
            where.sin_port = 0;
            passive = socket(AF_INET, SOCK_STREAM, 0);
            if (passive < 0)
                return false;
            len = sizeof where;
            if (bind(passive, reinterpret_cast<sockaddr *>(&where), sizeof where) != 0 || listen(passive, 1) != 0 ||
                getsockname(passive, reinterpret_cast<sockaddr *>(&where), &len) != 0) {
                closeSocket(passive);
                return false;
            }
            return true;
        }
        // The data connection for a transfer, accepted on the passive socket.
        int acceptData() {
            if (passive < 0)
                return -1;
            int fd = -1;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(DataSeconds);
            while (!server.stopping && std::chrono::steady_clock::now() < until) {
                pollfd p{passive, POLLIN, 0};
                if (poll(&p, 1, PollMs) > 0 && (p.revents & POLLIN)) {
                    fd = accept(passive, nullptr, nullptr);
                    break;
                }
            }
            closeSocket(passive);
            return fd;
        }

        static std::string quoted(const std::string &path) {
            std::string out = "\"";
            for (char c : path)
                out += c == '"' ? std::string("\"\"") : std::string(1, c);
            return out + "\"";
        }

        void list(const std::string &arg, bool namesOnly) {
            // Options such as "-la" are ls flags, not a path.
            std::string target = arg;
            while (target.size() > 1 && target[0] == '-') {
                const size_t space = target.find(' ');
                target = space == std::string::npos ? std::string() : target.substr(space + 1);
            }
            const fs::path dir = server.real(ftpResolve(cwd, target));
            std::error_code ec;
            if (dir.empty() || !fs::is_directory(dir, ec))
                return void(reply("550 No such folder."));
            std::string text;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (namesOnly) {
                    text += name + "\r\n";
                    continue;
                }
                struct stat st{};
                if (stat(it->path().string().c_str(), &st) != 0)
                    continue;
                const bool folder = S_ISDIR(st.st_mode);
                char line[96];
                std::snprintf(line, sizeof line, "%s 1 switch switch %13llu %s ",
                              folder ? "drwxr-xr-x" : "-rw-r--r--",
                              folder ? 0ull : (unsigned long long)st.st_size, listDate(st.st_mtime).c_str());
                text += line + name + "\r\n";
            }
            reply("150 Here comes the listing.");
            int data = acceptData();
            if (data < 0)
                return void(reply("425 No data connection."));
            const bool ok = sendAll(data, text.data(), text.size());
            closeSocket(data);
            reply(ok ? "226 Listing sent." : "426 Listing interrupted.");
        }

        void retrieve(const std::string &arg) {
            const std::string virt = ftpResolve(cwd, arg);
            const fs::path p = server.real(virt);
            std::error_code ec;
            if (p.empty() || !fs::is_regular_file(p, ec))
                return void(reply("550 No such file."));
            FILE *f = std::fopen(p.string().c_str(), "rb");
            if (!f)
                return void(reply("550 Cannot open the file."));
            reply("150 Sending " + virt + ".");
            int data = acceptData();
            if (data < 0) {
                std::fclose(f);
                return void(reply("425 No data connection."));
            }
            std::vector<char> buf(TransferBuffer);
            bool ok = true;
            size_t n;
            while (ok && (n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
                ok = sendAll(data, buf.data(), n);
                if (ok)
                    server.sent += n;
            }
            ok = ok && !std::ferror(f);
            std::fclose(f);
            closeSocket(data);
            reply(ok ? "226 Transfer complete." : "426 Transfer interrupted.");
        }

        void store(const std::string &arg, bool append) {
            const std::string virt = ftpResolve(cwd, arg);
            const fs::path p = server.real(virt);
            std::error_code ec;
            if (p.empty() || virt == "/" || fs::is_directory(p, ec) || !fs::is_directory(p.parent_path(), ec))
                return void(reply("553 Cannot write there."));
            FILE *f = std::fopen(p.string().c_str(), append ? "ab" : "wb");
            if (!f)
                return void(reply("553 Cannot create the file."));
            reply("150 Ready to receive " + virt + ".");
            int data = acceptData();
            if (data < 0) {
                std::fclose(f);
                return void(reply("425 No data connection."));
            }
            std::vector<char> buf(TransferBuffer);
            bool ok = true;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DataSeconds);
            for (;;) {
                if (server.stopping || std::chrono::steady_clock::now() > deadline) {
                    ok = false;
                    break;
                }
                pollfd pf{data, POLLIN, 0};
                if (poll(&pf, 1, PollMs) <= 0)
                    continue;
                const ssize_t n = recv(data, buf.data(), buf.size(), 0);
                if (n == 0)
                    break; // the client closes the connection to end the file
                if (n < 0 || std::fwrite(buf.data(), 1, size_t(n), f) != size_t(n)) {
                    ok = false;
                    break;
                }
                server.received += uint64_t(n);
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DataSeconds);
            }
            ok = std::fclose(f) == 0 && ok;
            closeSocket(data);
            ++server.changes;
            server.note(virt);
            reply(ok ? "226 Transfer complete." : "451 Transfer failed; the file may be incomplete.");
        }

        void run() {
            reply("220 Switch Hero song library. Anonymous login.");
            std::string line;
            while (readLine(line)) {
                if (line.find('\0') != std::string::npos) {
                    reply("501 Bad characters in the command.");
                    continue;
                }
                const size_t space = line.find(' ');
                std::string cmd = line.substr(0, space), arg = space == std::string::npos ? "" : line.substr(space + 1);
                std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return char(std::toupper(c)); });
                if (!handle(cmd, arg))
                    return;
            }
        }

        // Returns false to close the connection.
        bool handle(const std::string &cmd, const std::string &arg) {
            if (cmd == "QUIT")
                return reply("221 Goodbye."), false;
            if (cmd == "USER")
                return user = true, reply("331 Anonymous login: any password will do.");
            if (cmd == "PASS") {
                if (!user)
                    return reply("503 Send USER first.");
                loggedIn = true;
                return reply("230 Logged in. Only the songs folder is shared.");
            }
            if (cmd == "NOOP")
                return reply("200 OK.");
            if (cmd == "SYST")
                return reply("215 UNIX Type: L8");
            if (cmd == "FEAT")
                return reply("211-Features:\r\n UTF8\r\n SIZE\r\n MDTM\r\n EPSV\r\n PASV\r\n211 End");
            if (cmd == "OPTS")
                return reply("200 OK.");
            if (!loggedIn)
                return reply("530 Log in first (any user name).");

            if (cmd == "PWD" || cmd == "XPWD")
                return reply("257 " + quoted(cwd) + " is the current folder.");
            if (cmd == "CWD" || cmd == "XCWD" || cmd == "CDUP" || cmd == "XCUP") {
                const std::string to = ftpResolve(cwd, cmd == "CDUP" || cmd == "XCUP" ? ".." : arg);
                const fs::path p = server.real(to);
                std::error_code ec;
                if (p.empty() || !fs::is_directory(p, ec))
                    return reply("550 No such folder.");
                cwd = to;
                return reply("250 Now in " + cwd + ".");
            }
            if (cmd == "TYPE" || cmd == "MODE" || cmd == "STRU")
                return reply("200 OK."); // always binary, stream, file
            if (cmd == "ALLO")
                return reply("202 No need.");
            if (cmd == "PASV") {
                sockaddr_in where{};
                if (!openPassive(where))
                    return reply("425 Cannot open a data port.");
                const uint32_t ip = ntohl(where.sin_addr.s_addr);
                const uint16_t p = ntohs(where.sin_port);
                char text[80];
                std::snprintf(text, sizeof text, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).", ip >> 24,
                              (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, p >> 8, p & 255);
                return reply(text);
            }
            if (cmd == "EPSV") {
                sockaddr_in where{};
                if (!openPassive(where))
                    return reply("425 Cannot open a data port.");
                return reply("229 Entering Extended Passive Mode (|||" + std::to_string(ntohs(where.sin_port)) + "|).");
            }
            if (cmd == "PORT" || cmd == "EPRT")
                return reply("502 Only passive mode is supported.");
            if (cmd == "LIST" || cmd == "NLST")
                return list(arg, cmd == "NLST"), true;
            if (cmd == "RETR")
                return retrieve(arg), true;
            if (cmd == "STOR" || cmd == "APPE")
                return store(arg, cmd == "APPE"), true;
            if (cmd == "SIZE" || cmd == "MDTM") {
                const fs::path p = server.real(ftpResolve(cwd, arg));
                struct stat st{};
                if (p.empty() || stat(p.string().c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                    return reply("550 No such file.");
                if (cmd == "SIZE")
                    return reply("213 " + std::to_string((unsigned long long)st.st_size));
                std::tm tm{};
                gmtime_r(&st.st_mtime, &tm);
                char text[32];
                std::strftime(text, sizeof text, "213 %Y%m%d%H%M%S", &tm);
                return reply(text);
            }
            if (cmd == "MKD" || cmd == "XMKD") {
                const std::string virt = ftpResolve(cwd, arg);
                const fs::path p = server.real(virt);
                std::error_code ec;
                if (p.empty() || virt == "/" || !fs::create_directory(p, ec))
                    return reply("550 Cannot create that folder.");
                ++server.changes;
                server.note(virt + "/");
                return reply("257 " + quoted(virt) + " created.");
            }
            if (cmd == "DELE" || cmd == "RMD" || cmd == "XRMD") {
                const std::string virt = ftpResolve(cwd, arg);
                const fs::path p = server.real(virt);
                std::error_code ec;
                const bool folder = fs::is_directory(p, ec);
                // fs::remove only takes empty folders; clients delete contents first.
                if (p.empty() || virt == "/" || folder != (cmd != "DELE") || !fs::remove(p, ec))
                    return reply(cmd == "DELE" ? "550 Cannot delete that file." : "550 Cannot remove that folder.");
                ++server.changes;
                server.note("removed " + virt);
                return reply("250 Removed.");
            }
            if (cmd == "RNFR") {
                const std::string virt = ftpResolve(cwd, arg);
                const fs::path p = server.real(virt);
                std::error_code ec;
                if (p.empty() || virt == "/" || !fs::exists(p, ec))
                    return reply("550 No such file or folder.");
                renameFrom = virt;
                return reply("350 Ready for the new name.");
            }
            if (cmd == "RNTO") {
                if (renameFrom.empty())
                    return reply("503 Send RNFR first.");
                const std::string to = ftpResolve(cwd, arg);
                const fs::path from = server.real(renameFrom), dest = server.real(to);
                renameFrom.clear();
                std::error_code ec;
                if (from.empty() || dest.empty() || to == "/" || fs::exists(dest, ec))
                    return reply("553 Cannot rename to that.");
                fs::rename(from, dest, ec);
                if (ec)
                    return reply("553 Cannot rename to that.");
                ++server.changes;
                server.note(to);
                return reply("250 Renamed.");
            }
            if (cmd == "ABOR")
                return reply("226 Nothing to abort.");
            return reply("502 Not supported.");
        }
    };
};

FtpServer::FtpServer(fs::path root) : impl(std::make_unique<Impl>()) { impl->root = std::move(root); }
FtpServer::~FtpServer() { stop(); }

void FtpServer::start(uint16_t port) {
    stop();
    Impl &s = *impl;
    s.stopping = false;
    s.received = 0, s.sent = 0, s.changes = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.last.clear(), s.error.clear();
    }
    std::error_code ec;
    fs::create_directories(s.root, ec);
    s.listener = socket(AF_INET, SOCK_STREAM, 0);
    if (s.listener < 0)
        throw std::runtime_error("Could not open a network port");
    int yes = 1;
    setsockopt(s.listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    socklen_t len = sizeof a;
    if (bind(s.listener, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(s.listener, MaxClients) != 0 ||
        getsockname(s.listener, reinterpret_cast<sockaddr *>(&a), &len) != 0) {
        closeSocket(s.listener);
        throw std::runtime_error("Port " + std::to_string(port) + " is already in use");
    }
    s.port = ntohs(a.sin_port);
    s.address = localAddress();
    s.acceptor = std::thread([&s] { s.acceptLoop(); });
}

void FtpServer::stop() {
    Impl &s = *impl;
    if (!s.acceptor.joinable())
        return;
    // Every loop polls with a short timeout and checks this, so sessions in
    // the middle of a transfer give up within a fraction of a second.
    s.stopping = true;
    s.acceptor.join();
    closeSocket(s.listener);
    std::vector<std::unique_ptr<Impl::Client>> clients;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        clients.swap(s.clients);
    }
    for (auto &c : clients)
        c->thread.join();
}

FtpServer::Status FtpServer::status() const {
    const Impl &s = *impl;
    Status out;
    out.running = s.acceptor.joinable() && !s.stopping;
    out.address = s.address;
    out.port = s.port;
    out.received = s.received, out.sent = s.sent, out.changes = s.changes;
    std::lock_guard<std::mutex> lock(s.mutex);
    for (const auto &c : s.clients)
        out.clients += c->done ? 0 : 1;
    out.last = s.last, out.error = s.error;
    return out;
}

} // namespace fret
