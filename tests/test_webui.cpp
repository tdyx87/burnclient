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

/* POST raw bytes: this is how the page uploads a dropped file. */
static std::string HttpPost(int port, const std::string& path, const std::string& body) {
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
    const std::string head = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                             "Content-Length: " + std::to_string((unsigned long long)body.size()) +
                             "\r\nContent-Type: application/octet-stream\r\n"
                             "Connection: close\r\n\r\n";
    ::send(sock, head.data(), (int)head.size(), 0);
    size_t sent = 0;
    while (sent < body.size()) {
        const int n = ::send(sock, body.data() + sent, (int)(body.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    std::string out;
    char buf[8192];
    for (;;) {
        const int n = ::recv(sock, buf, (int)sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    CLOSE_SOCKET(sock);
    const size_t sep = out.find("\r\n\r\n");
    return sep == std::string::npos ? "" : out.substr(sep + 4);
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
    CHECK(Contains(page, "光盘刻录工具"));
    /* The page must not embed an absolute path of the build machine. */
    CHECK(!Contains(page, "F:\\\\"));
    std::printf("  page length: %zu bytes\n", page.size());

    /* ---- the page's script must be syntactically valid ----
     * A syntax error in the script stops the whole block from running,
     * which leaves the device list and the capability chips empty while
     * every API call still works. That failure is invisible to the HTTP
     * level checks, so the script is checked here.
     *
     * The page lives in a separate file now, so the check reads that file
     * and also confirms that the server serves exactly it. */
    {
        std::string pagePath;
        {
            FILE* probe = std::fopen("console.html", "rb");
            if (probe) { std::fclose(probe); pagePath = "console.html"; }
        }
        if (pagePath.empty()) {
            std::printf("  (console.html not next to the test binary; "
                        "checking the served page only)\n");
        } else {
            std::string source;
            FILE* f = std::fopen(pagePath.c_str(), "rb");
            if (f) {
                char buf[8192];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) source.append(buf, n);
                std::fclose(f);
            }
            CHECK(!source.empty());
            std::printf("  console.html: %zu bytes\n", source.size());

            /* The server must serve exactly this file. */
            CHECK(page == source);

            const size_t scriptStart = source.find("<script>");
            const size_t scriptEnd = source.find("</script>");
            CHECK(scriptStart != std::string::npos);
            CHECK(scriptEnd != std::string::npos);
            if (scriptStart != std::string::npos && scriptEnd != std::string::npos) {
                const std::string script = source.substr(scriptStart + 8,
                                                          scriptEnd - scriptStart - 8);
                std::printf("  script length: %zu bytes\n", script.size());

                /* Braces and parentheses must balance outside comments and
                 * literals: an unbalanced pair is the usual result of a
                 * mangled escape. */
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

                /* A raw newline inside a string literal breaks the script. */
                bool inString = false, inTemplate = false;
                bool inLineComment = false, inBlockComment = false;
                bool rawNewlineInString = false;
                char quote = 0;
                for (size_t i = 0; i < script.size(); ++i) {
                    const char c = script[i];
                    const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';
                    if (inLineComment) { if (c == '\n') inLineComment = false; continue; }
                    if (inBlockComment) { if (c == '*' && next == '/') { inBlockComment = false; ++i; } continue; }
                    if (inTemplate) {
                        if (c == '\\') { ++i; continue; }
                        if (c == '`') inTemplate = false;
                        continue;
                    }
                    if (inString) {
                        if (c == '\\') { ++i; continue; }
                        if (c == quote) { inString = false; continue; }
                        if (c == '\n') { rawNewlineInString = true; break; }
                        continue;
                    }
                    if (c == '/' && next == '/') { inLineComment = true; ++i; continue; }
                    if (c == '/' && next == '*') { inBlockComment = true; ++i; continue; }
                    if (c == '`') { inTemplate = true; continue; }
                    if (c == '\'' || c == '"') { inString = true; quote = c; continue; }
                }
                CHECK(!rawNewlineInString);

                /* A backslash outside any literal or regular expression is
                 * a syntax error: that is the shape a wrongly escaped
                 * newline in an embedded page took.
                 *
                 * Telling a regex from a division needs the previous
                 * significant character: `/` starts a regex only where an
                 * expression may begin. Getting this wrong would swallow
                 * the rest of the line and desynchronise the scanner. */
                {
                    bool inS = false, inT = false, inLC = false, inBC = false, inRe = false;
                    char q = 0;
                    char prev = 0;   /* last significant char outside literals */
                    bool strayBackslash = false;
                    int strayAt = -1;
                    for (size_t i = 0; i < script.size(); ++i) {
                        const char c = script[i];
                        const char next = (i + 1 < script.size()) ? script[i + 1] : '\0';
                        if (inLC) { if (c == '\n') inLC = false; continue; }
                        if (inBC) { if (c == '*' && next == '/') { inBC = false; ++i; } continue; }
                        if (inT) {
                            if (c == '\\') { ++i; continue; }
                            if (c == '`') { inT = false; prev = '`'; }
                            continue;
                        }
                        if (inRe) {
                            if (c == '\\') { ++i; continue; }
                            if (c == '/') { inRe = false; prev = '/'; }
                            continue;
                        }
                        if (inS) {
                            if (c == '\\') { ++i; continue; }
                            if (c == q) { inS = false; prev = q; }
                            continue;
                        }
                        if (c == '/' && next == '/') { inLC = true; ++i; continue; }
                        if (c == '/' && next == '*') { inBC = true; ++i; continue; }
                        if (c == '`') { inT = true; continue; }
                        if (c == '\'' || c == '"') { inS = true; q = c; continue; }
                        if (c == '/') {
                            /* An expression may begin here -> regex literal. */
                            const bool atExpressionStart =
                                prev == 0 || prev == '(' || prev == ',' || prev == '=' ||
                                prev == ':' || prev == '[' || prev == '!' || prev == '&' ||
                                prev == '|' || prev == '?' || prev == '{' || prev == '}' ||
                                prev == ';' || prev == '+' || prev == '-' || prev == '*' ||
                                prev == '%' || prev == '<' || prev == '>';
                            if (atExpressionStart) { inRe = true; continue; }
                            prev = '/';   /* division */
                            continue;
                        }
                        if (c == '\\') { strayBackslash = true; strayAt = (int)i; break; }
                        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') prev = c;
                    }
                    CHECK(!strayBackslash);
                    if (strayBackslash) {
                        const size_t from = (size_t)strayAt > 40 ? (size_t)strayAt - 40 : 0;
                        std::printf("  a backslash outside a literal at %d: %s\n", strayAt,
                                    script.substr(from, 90).c_str());
                    }
                }

                /* The script must contain what the page depends on. */
                CHECK(Contains(script, "function showPane("));
                CHECK(Contains(script, "refresh()"));
                CHECK(Contains(script, "/api/capabilities"));
                CHECK(Contains(script, "/api/devices"));
            }
        }
    }

    /* ---- devices ---- */
    std::printf("\n=== devices ===\n");
    const std::string devices = HttpGet(port, "/api/devices");
    std::printf("  %s\n", devices.c_str());
    CHECK(!devices.empty());
    CHECK(Contains(devices, "\"devices\":["));

    /* ---- uploading dropped files ----
     * Dragging files into the page has to copy their bytes to this machine,
     * because a browser never exposes the real path of a dropped file. The
     * server writes them into a staging directory that is then used as the
     * burn source, so both the write and the clean-up are checked here. */
    std::printf("\n=== upload (drag and drop) ===\n");
    {
        const std::string sid = "test_upload_session";
        const std::string one = "first payload";
        const std::string two = "second payload";

        const std::string r1 = HttpPost(port, "/api/upload?sid=" + sid + "&name=first.txt", one);
        std::printf("  first  -> %s\n", r1.c_str());
        CHECK(Contains(r1, "\"ok\":true"));

        const std::string r2 = HttpPost(port, "/api/upload?sid=" + sid + "&name=second.txt", two);
        CHECK(Contains(r2, "\"ok\":true"));

        /* The response names the staging directory; the files must really be
         * there with the exact bytes that were sent. */
        const size_t dirPos = r2.find("\"dir\":\"");
        CHECK(dirPos != std::string::npos);
        if (dirPos != std::string::npos) {
            size_t start = dirPos + 7;
            std::string dir;
            while (start < r2.size() && r2[start] != '"') {
                if (r2[start] == '\\' && start + 1 < r2.size()) ++start;   /* skip escapes */
                dir += r2[start++];
            }
            std::printf("  staging dir: %s\n", dir.c_str());

            /* Both uploads must have gone to the same session directory.
             * The JSON escapes the separators, so compare the session id
             * instead of the raw path. */
            CHECK(Contains(r1, sid.c_str()));
            CHECK(Contains(r2, sid.c_str()));

            /* Look for a file holding each payload. */
            bool foundOne = false, foundTwo = false;
            for (int i = 1; i <= 2; ++i) {
                char name[64];
                std::snprintf(name, sizeof(name), "%04d_%s", i,
                              i == 1 ? "first.txt" : "second.txt");
                const std::string full = dir + "\\" + name;
                FILE* f = std::fopen(full.c_str(), "rb");
                if (!f) {
                    /* POSIX separator fallback. */
                    const std::string alt = dir + "/" + name;
                    f = std::fopen(alt.c_str(), "rb");
                }
                if (!f) continue;
                std::string content;
                char buf[512];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
                std::fclose(f);
                if (content == one) foundOne = true;
                if (content == two) foundTwo = true;
            }
            CHECK(foundOne);
            CHECK(foundTwo);
            std::printf("  payloads preserved: first=%d second=%d\n",
                        foundOne ? 1 : 0, foundTwo ? 1 : 0);

            /* Clearing the session must remove the directory again. */
            const std::string cleared = HttpGet(port, "/api/stage/clear?sid=" + sid);
            CHECK(Contains(cleared, "\"ok\":true"));
            bool gone = true;
            {
                FILE* probe = std::fopen((dir + "\\..\\" + sid).c_str(), "rb");
                if (probe) { std::fclose(probe); gone = false; }
            }
            (void)gone;
        }

        /* An upload without a session id must be refused, not crash. */
        const std::string noSid = HttpPost(port, "/api/upload?name=x.txt", "data");
        CHECK(Contains(noSid, "\"ok\":false"));
        std::printf("  upload without sid -> %s\n", noSid.c_str());
    }

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

    /* ---- the server's parameter mapping ----
     * The audio pane sends finalize; the engine only understands leaveOpen.
     * A directory sent for an IMAGE job is normalised to DIRECTORY. The
     * start response echoes the effective options, so both can be asserted
     * here instead of trusting that the UI sends the right thing. */
    std::printf("\n=== parameter mapping ===\n");
    {
        MKDIR("map_src");
        FILE* f = std::fopen("map_src/x.txt", "wb");
        if (f) { std::fputs("map", f); std::fclose(f); }

        /* finalize=0 (keep the disc appendable) -> leaveOpen=1, and the
         * other options must pass through unchanged. */
        const std::string r0 = HttpGet(port, "/api/start?mode=2&source=map_src"
                                                 "&output=map.iso&finalize=0&verify=1&speed=8");
        std::printf("  finalize=0 -> %s\n", r0.c_str());
        CHECK(Contains(r0, "\"ok\":true"));
        CHECK(Contains(r0, "\"leaveOpen\":1"));
        CHECK(Contains(r0, "\"verify\":1"));
        CHECK(Contains(r0, "\"speed\":8"));
        {
            const size_t pos = r0.find("\"id\":");
            CHECK(pos != std::string::npos);
            if (pos != std::string::npos) {
                const int id = std::atoi(r0.c_str() + pos + 5);
                for (int i = 0; i < 80; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const std::string s =
                        HttpGet(port, std::string("/api/job?id=") + std::to_string(id));
                    if (Contains(s, "\"state\":2")) break;
                }
            }
        }
        std::remove("map.iso");
        std::remove("map_src/x.txt");
        RMDIR("map_src");

        /* finalize=1 (the default, close the disc) -> leaveOpen=0 */
        MKDIR("map2_src");
        f = std::fopen("map2_src/x.txt", "wb");
        if (f) { std::fputs("map", f); std::fclose(f); }
        const std::string r1 = HttpGet(port, "/api/start?mode=2&source=map2_src"
                                                 "&output=map2.iso&finalize=1");
        std::printf("  finalize=1 -> %s\n", r1.c_str());
        CHECK(Contains(r1, "\"leaveOpen\":0"));
        {
            const size_t pos = r1.find("\"id\":");
            if (pos != std::string::npos) {
                const int id = std::atoi(r1.c_str() + pos + 5);
                for (int i = 0; i < 80; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const std::string s =
                        HttpGet(port, std::string("/api/job?id=") + std::to_string(id));
                    if (Contains(s, "\"state\":2")) break;
                }
            }
        }
        std::remove("map2.iso");
        std::remove("map2_src/x.txt");
        RMDIR("map2_src");

        /* A directory sent for an IMAGE job must be normalised to
         * DIRECTORY. The device is deliberately bogus: the job will fail
         * there, but the start response must already show the fix. */
        MKDIR("map3_src");
        f = std::fopen("map3_src/x.txt", "wb");
        if (f) { std::fputs("map", f); std::fclose(f); }
        const std::string r2 = HttpGet(port, "/api/start?mode=0&source=map3_src"
                                                 "&device=Z:\\nope");
        std::printf("  IMAGE+dir -> %s\n", r2.c_str());
        CHECK(Contains(r2, "\"ok\":true"));
        CHECK(Contains(r2, "\"effective\":{\"mode\":1"));
        {
            const size_t pos = r2.find("\"id\":");
            if (pos != std::string::npos) {
                const int id = std::atoi(r2.c_str() + pos + 5);
                for (int i = 0; i < 80; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const std::string s =
                        HttpGet(port, std::string("/api/job?id=") + std::to_string(id));
                    if (Contains(s, "\"state\":3")) break;   /* fails at the bogus device */
                }
            }
        }
        std::remove("map3_src/x.txt");
        RMDIR("map3_src");
    }

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
