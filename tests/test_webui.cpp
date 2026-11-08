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

    /* ---- the page's script must be syntactically valid ----
     * A syntax error in the embedded script stops the whole block from
     * running, which leaves the device list and the capability chips empty
     * while every API call still works. That failure is invisible to the
     * HTTP level checks, so the script is checked here.
     *
     * The check is structural rather than a full JavaScript parser: it
     * verifies that the parts which are easy to break while embedding the
     * script in a C string literal are intact. */
    {
        const size_t scriptStart = page.find("<script>");
        const size_t scriptEnd = page.find("</script>");
        CHECK(scriptStart != std::string::npos);
        CHECK(scriptEnd != std::string::npos);
        if (scriptStart != std::string::npos && scriptEnd != std::string::npos) {
            const std::string script = page.substr(scriptStart + 8,
                                                     scriptEnd - scriptStart - 8);
            std::printf("  script length: %zu bytes\n", script.size());

            /* Balanced braces and parentheses: an unbalanced pair is the
             * usual result of a mangled escape. Comments and string
             * literals are skipped, so a bracket inside them does not
             * count. */
            int braces = 0, parens = 0;
            {
                bool inS = false, inT = false, inLC = false, inBC = false;
                char q = 0;
                for (size_t i = 0; i < script.size(); ++i) {
                    const char c = script[i];
                    const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';
                    if (inLC) { if (c == '\n') inLC = false; continue; }
                    if (inBC) { if (c == '*' && next == '/') { inBC = false; ++i; } continue; }
                    if (inT) {
                        if (c == '\\') { ++i; continue; }
                        if (c == '`') inT = false;
                        continue;
                    }
                    if (inS) {
                        if (c == '\\') { ++i; continue; }
                        if (c == q) inS = false;
                        continue;
                    }
                    if (c == '/' && next == '/') { inLC = true; ++i; continue; }
                    if (c == '/' && next == '*') { inBC = true; ++i; continue; }
                    if (c == '`') { inT = true; continue; }
                    if (c == '\'' || c == '"') { inS = true; q = c; continue; }
                    if (c == '{') ++braces;
                    else if (c == '}') --braces;
                    else if (c == '(') ++parens;
                    else if (c == ')') --parens;
                }
            }
            CHECK(braces == 0);
            CHECK(parens == 0);
            std::printf("  braces=%d parens=%d\n", braces, parens);

            /* A literal newline inside a string literal breaks the script.
             * This is what a wrongly escaped \\n in the C++ source produces:
             * the byte pair backslash+n is fine, a raw newline is not.
             *
             * Comments are skipped: an apostrophe inside a comment (as in
             * "the backend's capability") is not a string delimiter. Template
             * literals may legitimately span lines, so they are tracked
             * separately. */
            bool inString = false;
            bool inTemplate = false;
            bool inLineComment = false;
            bool inBlockComment = false;
            bool rawNewlineInString = false;
            char quote = 0;
            for (size_t i = 0; i < script.size(); ++i) {
                const char c = script[i];
                const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';

                if (inLineComment) {
                    if (c == '\n') inLineComment = false;
                    continue;
                }
                if (inBlockComment) {
                    if (c == '*' && next == '/') { inBlockComment = false; ++i; }
                    continue;
                }
                if (inTemplate) {
                    if (c == '\\') { ++i; continue; }
                    if (c == '`') inTemplate = false;
                    continue;
                }
                if (inString) {
                    if (c == '\\') { ++i; continue; }   /* skip the escaped one */
                    if (c == quote) { inString = false; continue; }
                    if (c == '\n') { rawNewlineInString = true; break; }
                    continue;
                }
                /* Not inside a string: look for a comment start. */
                if (c == '/' && next == '/') { inLineComment = true; ++i; continue; }
                if (c == '/' && next == '*') { inBlockComment = true; ++i; continue; }
                if (c == '`') {
                    inTemplate = true;
                } else if (c == '\'' || c == '"') {
                    inString = true;
                    quote = c;
                }
            }
            CHECK(!rawNewlineInString);
            if (rawNewlineInString) {
                std::printf("  a raw newline sits inside a string literal: "
                            "the script cannot parse\n");
            }

            /* A backslash outside any string is a syntax error in
             * JavaScript: the escape sequences only exist inside
             * literals. This is the shape a wrongly escaped \\n in the
             * C++ source takes - it lands in the script as a bare token
             * such as "value += (\n ? ...)" - and it stops the whole
             * script block from running.
             *
             * Regular expression literals are skipped: a backslash
             * inside /\.wav$/ is perfectly valid. */
            {
                bool inS = false, inT = false, inLC = false, inBC = false, inRe = false;
                char q = 0;
                bool strayBackslash = false;
                int strayAt = -1;
                for (size_t i = 0; i < script.size(); ++i) {
                    const char c = script[i];
                    const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';
                    if (inLC) { if (c == '\n') inLC = false; continue; }
                    if (inBC) { if (c == '*' && next == '/') { inBC = false; ++i; } continue; }
                    if (inT) {
                        if (c == '\\') { ++i; continue; }
                        if (c == '`') inT = false;
                        continue;
                    }
                    if (inRe) {
                        if (c == '\\') { ++i; continue; }
                        if (c == '[') { /* character class: ] does not end it */ }
                        else if (c == '/') { inRe = false; }
                        continue;
                    }
                    if (inS) {
                        if (c == '\\') { ++i; continue; }
                        if (c == q) inS = false;
                        continue;
                    }
                    if (c == '/' && next == '/') { inLC = true; ++i; continue; }
                    if (c == '/' && next == '*') { inBC = true; ++i; continue; }
                    if (c == '`') { inT = true; continue; }
                    if (c == '\'' || c == '"') { inS = true; q = c; continue; }
                    /* A slash starts a regular expression when the previous
                     * significant token cannot end an expression; the
                     * script only uses /.../ right after "(" or "," or "=",
                     * so the simple rule below is enough here. */
                    if (c == '/') { inRe = true; continue; }
                    if (c == '\\') { strayBackslash = true; strayAt = (int)i; break; }
                }
                CHECK(!strayBackslash);
                if (strayBackslash) {
                    const size_t from = (size_t)strayAt > 40 ? (size_t)strayAt - 40 : 0;
                    std::printf("  a backslash outside a string at %d: %s\n", strayAt,
                                script.substr(from, 90).c_str());
                }
            }

            /* The script must contain the calls the page depends on. */
            CHECK(Contains(script, "function showTab("));
            CHECK(Contains(script, "refresh()"));
            CHECK(Contains(script, "/api/capabilities"));
            CHECK(Contains(script, "/api/devices"));
        }
    }

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
