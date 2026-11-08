/* Automated test for the local web console.
 *
 * The console is exercised the way a browser would: the test starts the
 * server, performs real HTTP requests against it and checks the answers.
 * That covers the parts that can go wrong without a drive: the server must
 * come up, answer every endpoint, report capabilities truthfully, drive a
 * job to completion and refuse a request that must fail.
 *
 * The console binds to 127.0.0.1 only; the test connects to the same
 * loopback address. */
#include "../src/burning_utility.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <direct.h>
    #define CLOSE_SOCKET closesocket
    /* SOCKET is UINT_PTR on Windows: an int would truncate the handle. */
    #define SOCKET_TYPE SOCKET
    #define MKDIR(p) _mkdir(p)
    #define RMDIR(p) _rmdir(p)
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #include <signal.h>
    #define CLOSE_SOCKET ::close
    #define SOCKET_TYPE int
    #define MKDIR(p) mkdir((p), 0777)
    #define RMDIR(p) rmdir(p)
#endif

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("CHECK FAILED: %s (line %d)\n", #expr, __LINE__);      \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------------ */
/* A very small HTTP client: enough to talk to the console. */
/* Raw HTTP: returns the status line plus the body. */
static std::string HttpGetRaw(int port, const std::string& path) {
    const SOCKET_TYPE sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSE_SOCKET(sock);
        return "";
    }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                            "Connection: close\r\n\r\n";
    ::send(sock, req.data(), (int)req.size(), 0);
    std::string out;
    char buf[8192];
    for (;;) {
        const int n = ::recv(sock, buf, (int)sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    CLOSE_SOCKET(sock);
    return out;
}

static std::string HttpGet(int port, const std::string& path) {
    const std::string raw = HttpGetRaw(port, path);
    const size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return "";
    return raw.substr(sep + 4);
}

static bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

/* Pick a port that is free right now. */
static int FindFreePort() {
    const SOCKET_TYPE sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 18712;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 18712;
    if (::bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
        sockaddr_in bound;
        socklen_t len = sizeof(bound);
        if (::getsockname(sock, (sockaddr*)&bound, &len) == 0)
            port = ntohs(bound.sin_port);
    }
    CLOSE_SOCKET(sock);
    return port;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    /* The server is killed with a signal; do not let that kill the test. */
    signal(SIGPIPE, SIG_IGN);
#endif

    const char* exePath = (argc > 1) ? argv[1] : "burn_webui";
    const int port = FindFreePort();
    long serverPid = 0;   /* POSIX only */

    std::printf("=== web console test ===\n");
    std::printf("  server: %s  port: %d\n", exePath, port);

    /* Launch the console. On Windows the child is started directly so the test
     * knows its PID and can stop exactly that process; killing by image name
     * would also kill a console the user has open. */
    std::printf("  launching...\n");
    std::fflush(stdout);
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    std::string winCmd = std::string("\"") + exePath + "\" --port " +
                         std::to_string(port) + " --no-browser --exit-after 120";
    std::vector<char> cmdBuf(winCmd.begin(), winCmd.end());
    cmdBuf.push_back('\0');
    if (::CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL,
                         &si, &pi) == 0) {
        std::printf("  CreateProcess failed (%lu)\n", (unsigned long)::GetLastError());
        return 1;
    }
    ::CloseHandle(pi.hThread);
#else
    /* Start the server in the background and remember its PID. Killing it by
     * name would also match this test, whose command line mentions the
     * server binary. */
    const std::string cmd = std::string("\"") + exePath + "\" --port " +
                            std::to_string(port) + " --no-browser --exit-after 120 & echo $!";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[64] = {0};
        if (::fgets(buf, sizeof(buf), pipe)) {
            serverPid = (long)std::atoi(buf);
        }
        ::pclose(pipe);
    }
#endif

    /* Wait for the server to accept connections. */
    std::string caps;
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        caps = HttpGet(port, "/api/capabilities");
        if (!caps.empty()) break;
    }
    CHECK(!caps.empty());
    if (caps.empty()) {
        std::printf("the console never answered; aborting\n");
        return 1;
    }

    /* ---- capabilities ---- */
    std::printf("\n=== capabilities ===\n");
    std::printf("  %s\n", caps.c_str());
    CHECK(Contains(caps, "\"apiVersion\":3"));
    CHECK(Contains(caps, "\"platform\":"));
    CHECK(Contains(caps, "\"image_burn\":true"));
    CHECK(Contains(caps, "\"create_image\":true"));

    /* ---- the page itself ---- */
    std::printf("\n=== page ===\n");
    const std::string page = HttpGet(port, "/");
    CHECK(!page.empty());
    CHECK(Contains(page, "<!DOCTYPE html>"));
    CHECK(Contains(page, "BurningUtility"));
    /* The page must not embed an absolute path of the build machine. */
    CHECK(!Contains(page, "F:\\\\"));
    std::printf("  page length: %zu bytes\n", page.size());

    /* ---- devices ---- */
    std::printf("\n=== devices ===\n");
    const std::string devices = HttpGet(port, "/api/devices");
    std::printf("  %s\n", devices.c_str());
    CHECK(!devices.empty());
    CHECK(Contains(devices, "\"devices\":["));

    /* ---- directory browsing ---- */
    std::printf("\n=== browse ===\n");
    const std::string browse = HttpGet(port, "/api/browse?path=/");
    std::printf("  %s\n", browse.c_str());
    CHECK(!browse.empty());
    CHECK(Contains(browse, "\"entries\":["));
    CHECK(Contains(browse, "\"parent\":"));

    /* A path that is not a directory must be refused, not crash the server. */
    const std::string bad = HttpGet(port, "/api/browse?path=/definitely/not/here");
    CHECK(!bad.empty());
    CHECK(Contains(bad, "\"error\":"));
    std::printf("  bad path -> %s\n", bad.c_str());

    /* ---- a job that needs no drive: build an ISO ----
     * The source must be a small directory of its own: pointing the job at
     * the working directory would pack the output image into itself. */
    std::printf("\n=== job: create image ===\n");
    const std::string srcDir = "ui_src";
    MKDIR(srcDir.c_str());
    {
        FILE* f = nullptr;
        const std::string file = srcDir + "/payload.txt";
        f = std::fopen(file.c_str(), "wb");
        if (f) {
            std::fputs("web console test payload", f);
            std::fclose(f);
        }
    }
    const std::string start = HttpGet(port, "/api/start?mode=2&source=" + srcDir +
                                              "&output=ui_test.iso&label=UITEST"
                                              "&fileSystems=0&isoLevel=0");
    std::printf("  start -> %s\n", start.c_str());
    CHECK(Contains(start, "\"ok\":true"));
    const size_t idPos = start.find("\"id\":");
    CHECK(idPos != std::string::npos);
    int jobId = -1;
    if (idPos != std::string::npos) jobId = std::atoi(start.c_str() + idPos + 5);

    std::string status;
    for (int i = 0; i < 80; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        status = HttpGet(port, std::string("/api/job?id=") + std::to_string(jobId));
        if (Contains(status, "\"state\":2") || Contains(status, "\"state\":3") ||
            Contains(status, "\"state\":4")) {
            break;
        }
    }
    std::printf("  final -> %s\n", status.c_str());
    CHECK(Contains(status, "\"state\":2"));   /* completed */
    CHECK(Contains(status, "\"percent\":100"));
    CHECK(Contains(status, "\"errorCode\":0"));
    /* A successful job carries an empty error message, not a missing one. */
    CHECK(Contains(status, "\"error\":\"\""));
    std::remove("ui_test.iso");
    std::remove((srcDir + "/payload.txt").c_str());
    RMDIR(srcDir.c_str());

    /* ---- a request that must fail ---- */
    std::printf("\n=== job: invalid request ===\n");
    const std::string bad2 = HttpGet(port, "/api/start?mode=4&device=x");  /* no tracks */
    std::printf("  start -> %s\n", bad2.c_str());
    CHECK(Contains(bad2, "\"ok\":false"));
    CHECK(Contains(bad2, "\"error\":"));

    /* ---- cancelling an unknown job ---- */
    const std::string cancel = HttpGet(port, "/api/cancel?id=999999");
    std::printf("  cancel unknown -> %s\n", cancel.c_str());
    CHECK(Contains(cancel, "\"error\":"));

    /* ---- an unknown endpoint must answer 404 ---- */
    const std::string missing = HttpGetRaw(port, "/api/nope");
    std::printf("  unknown endpoint -> %s\n",
                missing.substr(0, missing.find("\r\n")).c_str());
    CHECK(Contains(missing, "404"));

    /* ---- the server is still alive ---- */
    const std::string again = HttpGet(port, "/api/capabilities");
    CHECK(!again.empty());
    std::printf("\n=== server survived every request ===\n");

    /* ---- stop the server we started ----
     * The console is a long-running program, so the test has to end it;
     * otherwise the test runner waits for it. */
#ifdef _WIN32
    if (pi.hProcess) {
        ::TerminateProcess(pi.hProcess, 0);
        ::CloseHandle(pi.hProcess);
    }
#else
    if (serverPid > 0) {
        ::kill((pid_t)serverPid, SIGTERM);
        /* Give it a moment, then make sure it is gone. */
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (::kill((pid_t)serverPid, 0) != 0) break;
        }
        ::kill((pid_t)serverPid, SIGKILL);
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

#ifdef _WIN32
    WSACleanup();
#endif
    if (g_failures) {
        std::printf("\n%d CHECK(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\n=== web console test: SUCCESS ===\n");
    return 0;
}
