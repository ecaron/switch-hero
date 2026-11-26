#pragma once
// A small FTP server for copying songs onto the console over the network.
//
// It shares exactly one folder, the song library: every path a client sends
// is resolved against a virtual root that is the songs folder, and ".." stops
// at that root the way it stops at "/" on a real server, so nothing outside
// the folder can be named, listed, read or written.
//
// Logins are anonymous for now: any user name and password are accepted. Only
// passive mode (PASV/EPSV) is offered, which is what every current client uses
// and what works through a home router.
#include "song.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fret {
// The virtual path (always starting with "/") that `arg` names from the
// working directory `cwd`. "." and empty parts are dropped and ".." removes
// the previous part but never goes above "/". Backslashes count as
// separators, since some Windows clients send them.
inline std::string ftpResolve(const std::string &cwd, const std::string &arg) {
    const std::string joined = !arg.empty() && (arg[0] == '/' || arg[0] == '\\') ? arg : cwd + "/" + arg;
    std::vector<std::string> parts;
    std::string part;
    auto flush = [&] {
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".")
            parts.push_back(part);
        part.clear();
    };
    for (char c : joined) {
        if (c == '/' || c == '\\')
            flush();
        else
            part += c;
    }
    flush();
    std::string out;
    for (const auto &p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

class FtpServer {
  public:
    struct Status {
        bool running = false;
        std::string address; // the console's address on the network, empty when offline
        uint16_t port = 0;
        int clients = 0;
        uint64_t received = 0, sent = 0; // file bytes
        int changes = 0;                 // files and folders added, removed or renamed
        std::string last;                // the latest thing a client did, for the screen
        std::string error;
    };
    // `root` is the only folder shared. Networking must already be up.
    explicit FtpServer(fs::path root);
    ~FtpServer();
    FtpServer(const FtpServer &) = delete;
    FtpServer &operator=(const FtpServer &) = delete;
    // Listens on `port` (0 picks any free port, for tests). Throws when the
    // port cannot be opened.
    void start(uint16_t port = DefaultPort);
    void stop();
    Status status() const;
    static constexpr uint16_t DefaultPort = 5000;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace fret
