/* ---------------------------------------------------------------------------
 * burn_webui.cpp - a small local web UI for the burning library.
 *
 * Why a web UI: it needs no GUI toolchain, behaves identically on Windows,
 * Linux and macOS, and can be driven by a test script, so the UI itself is
 * covered by the automated suite instead of only by hand.
 *
 * It binds to 127.0.0.1 only. The UI can burn discs, so it must never be
 * reachable from the network.
 *
 * The HTTP layer is deliberately minimal: one thread per connection, a
 * hand-rolled router, and JSON written by hand. There is no dependency to
 * install and nothing that can pull in a web framework at build time.
 * ------------------------------------------------------------------------- */
#include "burning_utility.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <direct.h>
    #define PATHSEP '\\'
    #define MKDIR(p) _mkdir(p)
    #define CLOSE_SOCKET closesocket
    /* SOCKET is UINT_PTR on Windows: keeping it in an int would truncate the
     * handle on a 64-bit build. */
    #define SOCKET_TYPE SOCKET
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <dirent.h>
    #include <unistd.h>
    #define PATHSEP '/'
    #define MKDIR(p) mkdir((p), 0777)
    #define CLOSE_SOCKET ::close
    #define SOCKET_TYPE int
#endif

namespace {

/* ==========================================================================
 * JSON helpers
 * ======================================================================== */
std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

std::string JsonString(const std::string& s) {
    return "\"" + JsonEscape(s) + "\"";
}

/* ==========================================================================
 * Minimal URL/form decoding
 * ======================================================================== */
int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = HexVal(s[i + 1]), lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

/* A parsed request: method, path and decoded form fields. */
struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> fields;
    std::string body;
};

std::map<std::string, std::string> ParseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string pair = query.substr(pos, amp == std::string::npos
                                                     ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            out[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string Field(const Request& r, const char* name, const std::string& fallback = "") {
    std::map<std::string, std::string>::const_iterator it = r.fields.find(name);
    return it == r.fields.end() ? fallback : it->second;
}

int IntField(const Request& r, const char* name, int fallback = 0) {
    const std::string v = Field(r, name);
    if (v.empty()) return fallback;
    return std::atoi(v.c_str());
}

/* ==========================================================================
 * Filesystem browsing for the UI
 * ======================================================================== */
bool IsDirectory(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

std::string ParentPath(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    if (slash == 0) return std::string(1, PATHSEP);   /* root */
    return path.substr(0, slash);
}

#ifdef _WIN32
std::string CurrentDir() {
    char buf[MAX_PATH];
    if (!_getcwd(buf, sizeof(buf))) return "";
    return buf;
}
#else
std::string CurrentDir() {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return "";
    return buf;
}
#endif

/* List a directory as a JSON array of {name, isDir, path}. */
std::string ListDirJson(const std::string& path) {
    std::string out = "[";
    bool first = true;
#ifdef _WIN32
    /* Avoid <filesystem>: it needs a newer toolchain than the project targets. */
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA((path + "\\*").c_str(), &data);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = data.cFileName;
            if (name == "." || name == "..") continue;
            const std::string full = path + "\\" + name;
            if (!first) out += ",";
            first = false;
            out += "{\"name\":" + JsonString(name) +
                   ",\"isDir\":" + (IsDirectory(full) ? "true" : "false") +
                   ",\"path\":" + JsonString(full) + "}";
        } while (FindNextFileA(h, &data));
        FindClose(h);
    }
#else
    DIR* dir = opendir(path.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = path + "/" + name;
            if (!first) out += ",";
            first = false;
            out += "{\"name\":" + JsonString(name) +
                   ",\"isDir\":" + (IsDirectory(full) ? "true" : "false") +
                   ",\"path\":" + JsonString(full) + "}";
        }
        closedir(dir);
    }
#endif
    out += "]";
    return out;
}

/* ==========================================================================
 * Job tracking for the UI
 *
 * A job started from the UI runs in its own thread; the browser polls
 * /api/job?id=N for its progress and can cancel it.
 * ======================================================================== */
struct UiJob {
    BurnJobHandle handle = BURN_INVALID_JOB_HANDLE;
    std::atomic<bool> finished{false};
    std::string label;
    bool reported = false;   /* the client has already seen the final state */
};

std::map<int, UiJob*> g_jobs;
std::mutex g_jobsMutex;
int g_nextJobId = 1;

int RegisterJob(BurnJobHandle handle, const std::string& label) {
    UiJob* job = new UiJob();
    job->handle = handle;
    job->label = label;
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    const int id = g_nextJobId++;
    g_jobs[id] = job;
    return id;
}

/* ==========================================================================
 * API handlers
 * ======================================================================== */
std::string HandleDevices() {
    DeviceInfo* devices = nullptr;
    int count = 0;
    std::string out = "{\"devices\":[";
    if (ListBurningDevices(&devices, &count) == BURN_SUCCESS) {
        for (int i = 0; i < count; ++i) {
            if (i) out += ",";
            out += "{\"path\":" + JsonString(devices[i].devicePath) +
                   ",\"name\":" + JsonString(devices[i].deviceName) +
                   ",\"media\":" + JsonString(devices[i].mediaType) +
                   ",\"writable\":" + (devices[i].isWritable ? "true" : "false") +
                   "}";
        }
        FreeDeviceList(devices, count);
    }
    out += "]}";
    return out;
}

std::string HandleCapabilities() {
    struct { int id; const char* name; } caps[] = {
        { BURN_CAP_IMAGE_BURN,      "image_burn" },
        { BURN_CAP_CREATE_IMAGE,    "create_image" },
        { BURN_CAP_AUDIO_TAO,       "audio_tao" },
        { BURN_CAP_ERASE,           "erase" },
        { BURN_CAP_MULTISESSION,    "multisession" },
        { BURN_CAP_UDF,             "udf" },
        { BURN_CAP_BOOT_IMAGE,      "boot_image" },
        { BURN_CAP_READBACK_VERIFY, "readback_verify" },
        { BURN_CAP_RAW_BURN,        "raw_burn" },
        { BURN_CAP_RAW_IMAGE,       "raw_image" },
        { BURN_CAP_TRAY_CONTROL,    "tray_control" },
        { BURN_CAP_DRIVE_INFO,      "drive_info" },
    };
    std::string out = "{\"apiVersion\":" + std::to_string(Burn_GetApiVersion());
    out += ",\"platform\":" + JsonString(GetPlatformInfo());
    out += ",\"capabilities\":{";
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
        if (i) out += ",";
        out += JsonString(caps[i].name) + ":" +
               (Burn_GetCapability(caps[i].id) ? "true" : "false");
    }
    out += "}}";
    return out;
}

/* Build the options struct for a job started from the UI. */
struct JobRequest {
    int mode = 0;
    std::string sourcePath;
    std::string outputPath;
    std::string devicePath;
    std::string discType = "CD-R";
    std::string volumeLabel = "DISC";
    std::string bootImagePath;
    std::string mediaCatalogNumber;
    int speed = 0;
    int verifyLevel = 0;
    int fileSystems = 0;
    int interchangeLevel = 0;
    int leaveOpen = 0;
    int eraseFull = 0;
    int startingTrackNumber = 0;
    std::vector<std::string> audioTracks;
};

JobRequest ParseJobRequest(const Request& r) {
    JobRequest j;
    j.mode = IntField(r, "mode", 0);
    j.sourcePath = Field(r, "source");
    j.outputPath = Field(r, "output");
    j.devicePath = Field(r, "device");
    j.discType = Field(r, "discType", "CD-R");
    j.volumeLabel = Field(r, "label", "DISC");
    j.bootImagePath = Field(r, "boot");
    j.mediaCatalogNumber = Field(r, "mcn");
    j.speed = IntField(r, "speed", 0);
    j.verifyLevel = IntField(r, "verify", 0);
    j.fileSystems = IntField(r, "fileSystems", 0);
    j.interchangeLevel = IntField(r, "isoLevel", 0);
    j.leaveOpen = IntField(r, "leaveOpen", 0);
    j.eraseFull = IntField(r, "eraseFull", 0);
    j.startingTrackNumber = IntField(r, "startTrack", 0);
    const std::string tracks = Field(r, "tracks");
    size_t pos = 0;
    while (pos <= tracks.size() && !tracks.empty()) {
        const size_t comma = tracks.find('|', pos);
        const std::string one = tracks.substr(pos, comma == std::string::npos
                                                      ? std::string::npos : comma - pos);
        if (!one.empty()) j.audioTracks.push_back(one);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return j;
}

/* Start a job described by the request and return its id. */
std::string HandleStart(const Request& r) {
    const JobRequest j = ParseJobRequest(r);

    BurnJobOptions3 o;
    std::memset(&o, 0, sizeof(o));
    o.size = (int)sizeof(o);
    o.mode = j.mode;
    o.sourcePath = j.sourcePath.empty() ? nullptr : j.sourcePath.c_str();
    o.outputPath = j.outputPath.empty() ? nullptr : j.outputPath.c_str();
    o.devicePath = j.devicePath.empty() ? nullptr : j.devicePath.c_str();
    o.discType = j.discType.c_str();
    o.volumeLabel = j.volumeLabel.c_str();
    o.bootImagePath = j.bootImagePath.empty() ? nullptr : j.bootImagePath.c_str();
    o.mediaCatalogNumber = j.mediaCatalogNumber.empty() ? nullptr
                                                       : j.mediaCatalogNumber.c_str();
    o.speed = j.speed;
    o.verifyLevel = j.verifyLevel;
    o.fileSystems = j.fileSystems;
    o.interchangeLevel = j.interchangeLevel;
    o.leaveOpen = j.leaveOpen;
    o.eraseFull = j.eraseFull;
    o.startingTrackNumber = j.startingTrackNumber;

    std::vector<BurnAudioTrackC> trackSpecs;
    for (size_t i = 0; i < j.audioTracks.size(); ++i) {
        BurnAudioTrackC spec;
        std::memset(&spec, 0, sizeof(spec));
        const std::string& p = j.audioTracks[i];
        const size_t copy = p.size() < sizeof(spec.audioPath) - 1 ? p.size()
                                                                  : sizeof(spec.audioPath) - 1;
        std::memcpy(spec.audioPath, p.c_str(), copy);
        spec.preemphasis = 0;
        trackSpecs.push_back(spec);
    }
    if (!trackSpecs.empty()) {
        o.audioTracks = trackSpecs.data();
        o.audioTrackCount = (int)trackSpecs.size();
    }

    const BurnJobHandle handle =
        Burn_Start(reinterpret_cast<const BurnJobOptions*>(&o));
    if (handle == BURN_INVALID_JOB_HANDLE) {
        return "{\"ok\":false,\"error\":" + JsonString(GetLastErrorMessage()) + "}";
    }

    const int id = RegisterJob(handle, j.mode == BURN_JOB_MODE_ERASE ? "erase" : "burn");
    std::string out = "{\"ok\":true,\"id\":" + std::to_string(id) + "}";
    return out;
}

std::string HandleJobStatus(const Request& r) {
    const int id = IntField(r, "id", 0);
    BurnJobHandle handle = BURN_INVALID_JOB_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        std::map<int, UiJob*>::iterator it = g_jobs.find(id);
        if (it == g_jobs.end()) return "{\"error\":\"no such job\"}";
        handle = it->second->handle;
    }
    BurnJobStatus st;
    std::memset(&st, 0, sizeof(st));
    st.size = (int)sizeof(st);
    if (Burn_GetStatus(handle, &st) != BURN_SUCCESS) {
        return "{\"error\":" + JsonString(GetLastErrorMessage()) + "}";
    }
    std::string out = "{\"state\":" + std::to_string((int)st.state) +
                      ",\"percent\":" + std::to_string(st.percent) +
                      ",\"phase\":" + JsonString(st.phase) +
                      ",\"message\":" + JsonString(st.message) +
                      ",\"error\":" + JsonString(st.errorMessage) +
                      ",\"errorCode\":" + std::to_string(st.errorCode) +
                      ",\"elapsedMs\":" + std::to_string((unsigned long long)st.elapsedMs) + "}";
    /* The job record is kept until the client has seen the final state.
     * Deleting it right here would make the very poll that reports
     * "completed" answer "no such job" instead. */
    if (st.state == BURN_JOB_COMPLETED || st.state == BURN_JOB_FAILED ||
        st.state == BURN_JOB_CANCELLED) {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        std::map<int, UiJob*>::iterator it = g_jobs.find(id);
        if (it != g_jobs.end()) {
            if (it->second->reported) {
                /* The client already saw the final state: release now. */
                Burn_Release(handle);
                delete it->second;
                g_jobs.erase(it);
            } else {
                it->second->reported = true;
            }
        }
    }
    return out;
}

std::string HandleCancel(const Request& r) {
    const int id = IntField(r, "id", 0);
    BurnJobHandle handle = BURN_INVALID_JOB_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        std::map<int, UiJob*>::iterator it = g_jobs.find(id);
        if (it == g_jobs.end()) return "{\"error\":\"no such job\"}";
        handle = it->second->handle;
    }
    const int rc = Burn_Cancel(handle);
    return std::string("{\"ok\":") + (rc == BURN_SUCCESS ? "true" : "false") +
           ",\"error\":" + JsonString(GetLastErrorMessage()) + "}";
}

std::string HandleDriveInfo(const Request& r) {
    const std::string device = Field(r, "device");
    const char* info = Burn_GetDriveInformation(device.c_str());
    if (!info || !*info) {
        return "{\"error\":" + JsonString(GetLastErrorMessage()) + "}";
    }
    return "{\"info\":" + JsonString(info) + "}";
}

std::string HandleSessionBlock(const Request& r) {
    const std::string device = Field(r, "device");
    const long long block = Burn_GetNextSessionBlock(device.c_str());
    if (block < 0) {
        return "{\"ok\":false,\"error\":" + JsonString(GetLastErrorMessage()) + "}";
    }
    return "{\"ok\":true,\"block\":" + std::to_string(block) + "}";
}

std::string HandleTray(const Request& r, bool eject) {
    const std::string device = Field(r, "device");
    const int rc = eject ? BurnEjectTray(device.c_str()) : BurnCloseTray(device.c_str());
    return std::string("{\"ok\":") + (rc == BURN_SUCCESS ? "true" : "false") +
           ",\"error\":" + JsonString(GetLastErrorMessage()) + "}";
}

std::string HandleBrowse(const Request& r) {
    std::string path = Field(r, "path");
    if (path.empty()) path = CurrentDir();
    if (!IsDirectory(path)) {
        return "{\"error\":" + JsonString("Not a directory: " + path) + "}";
    }
    return "{\"path\":" + JsonString(path) +
           ",\"parent\":" + JsonString(ParentPath(path)) +
           ",\"entries\":" + ListDirJson(path) + "}";
}

/* ==========================================================================
 * The page
 * ======================================================================== */
const char* kPage =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>BurningUtility 控制台</title>\n"
"<style>\n"
"  :root { --bg:#0f1115; --panel:#171a21; --line:#262b36; --fg:#e6e9ef;\n"
"          --muted:#98a1b3; --accent:#4c8dff; --ok:#3fb950; --warn:#d29922;\n"
"          --err:#f85149; }\n"
"  * { box-sizing:border-box; }\n"
"  body { margin:0; background:var(--bg); color:var(--fg);\n"
"         font:14px/1.5 -apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,\n"
"         \"Helvetica Neue\",Arial,\"Microsoft YaHei\",sans-serif; }\n"
"  header { padding:16px 24px; border-bottom:1px solid var(--line);\n"
"           display:flex; align-items:center; gap:16px; flex-wrap:wrap; }\n"
"  header h1 { font-size:17px; margin:0; font-weight:600; }\n"
"  .meta { color:var(--muted); font-size:12px; }\n"
"  main { padding:20px 24px; max-width:1180px; }\n"
"  .grid { display:grid; gap:16px; grid-template-columns:repeat(auto-fit,minmax(340px,1fr)); }\n"
"  section { background:var(--panel); border:1px solid var(--line); border-radius:10px;\n"
"            padding:16px 18px; }\n"
"  h2 { font-size:14px; margin:0 0 12px; font-weight:600; letter-spacing:.02em; }\n"
"  label { display:block; margin:10px 0 4px; color:var(--muted); font-size:12px; }\n"
"  input,select,button,textarea { width:100%; padding:8px 10px; border-radius:6px;\n"
"    border:1px solid var(--line); background:#0d1016; color:var(--fg); font:inherit; }\n"
"  input:focus,select:focus { outline:2px solid var(--accent); outline-offset:-1px; }\n"
"  button { background:var(--accent); color:#fff; border:none; font-weight:600;\n"
"           cursor:pointer; margin-top:12px; }\n"
"  button:hover { filter:brightness(1.08); }\n"
"  button.ghost { background:#222733; color:var(--fg); border:1px solid var(--line);\n"
"                 font-weight:500; margin-top:8px; }\n"
"  button.danger { background:var(--err); }\n"
"  .row { display:flex; gap:10px; }\n"
"  .row > * { flex:1; }\n"
"  .caps { display:flex; flex-wrap:wrap; gap:6px; margin-top:10px; }\n"
"  .cap { font-size:11px; padding:2px 8px; border-radius:999px;\n"
"         border:1px solid var(--line); color:var(--muted); }\n"
"  .cap.on { color:var(--ok); border-color:#1d4429; background:#0f2417; }\n"
"  .cap.off { color:#6b7280; }\n"
"  table { width:100%; border-collapse:collapse; margin-top:8px; }\n"
"  th,td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line);\n"
"           font-size:12px; }\n"
"  th { color:var(--muted); font-weight:500; }\n"
"  .files { max-height:230px; overflow:auto; border:1px solid var(--line);\n"
"           border-radius:6px; margin-top:8px; }\n"
"  .files div { padding:4px 9px; cursor:pointer; font-size:12px; }\n"
"  .files div:hover { background:#1e2430; }\n"
"  .files .dir { color:var(--accent); }\n"
"  .bar { height:8px; background:#0d1016; border-radius:999px; overflow:hidden;\n"
"         border:1px solid var(--line); margin-top:10px; }\n"
"  .bar > i { display:block; height:100%; background:var(--accent); width:0;\n"
"             transition:width .25s; }\n"
"  .log { background:#0d1016; border:1px solid var(--line); border-radius:6px;\n"
"         padding:9px 11px; margin-top:10px; max-height:230px; overflow:auto;\n"
"         font-family:ui-monospace,Consolas,monospace; font-size:11.5px;\n"
"         white-space:pre-wrap; }\n"
"  .ok { color:var(--ok); } .warn { color:var(--warn); } .err { color:var(--err); }\n"
"  .muted { color:var(--muted); }\n"
"  .pill { display:inline-block; padding:1px 7px; border-radius:999px;\n"
"          font-size:11px; border:1px solid var(--line); }\n"
"  .pill.w { color:var(--ok); border-color:#1d4429; }\n"
"  .pill.r { color:var(--err); border-color:#4a1f1f; }\n"
"  .tabs { display:flex; gap:6px; margin-bottom:14px; flex-wrap:wrap; }\n"
"  .tabs button { margin:0; width:auto; background:#1b2029; color:var(--muted);\n"
"                font-weight:500; padding:6px 14px; border:1px solid var(--line); }\n"
"  .tabs button.on { background:var(--accent); color:#fff; }\n"
"  .tabpane { display:none; } .tabpane.on { display:block; }\n"
"  .note { font-size:12px; color:var(--muted); margin-top:8px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <h1>BurningUtility 控制台</h1>\n"
"  <span class=\"meta\" id=\"hdr-meta\">读取中...</span>\n"
"</header>\n"
"<main>\n"
"  <div class=\"tabs\">\n"
"    <button class=\"on\" data-tab=\"t-data\">数据盘</button>\n"
"    <button data-tab=\"t-audio\">音频 CD</button>\n"
"    <button data-tab=\"t-iso\">镜像制作</button>\n"
"    <button data-tab=\"t-raw\">RAW</button>\n"
"    <button data-tab=\"t-maint\">维护 / 多会话</button>\n"
"  </div>\n"
"\n"
"  <div class=\"grid\">\n"
"    <section>\n"
"      <h2>设备与能力</h2>\n"
"      <label>刻录设备</label>\n"
"      <select id=\"device\"></select>\n"
"      <div class=\"caps\" id=\"caps\"></div>\n"
"      <div id=\"devinfo\"></div>\n"
"    </section>\n"
"\n"
"    <section>\n"
"      <h2>通用选项</h2>\n"
"      <div class=\"row\">\n"
"        <div><label>介质类型</label>\n"
"          <select id=\"discType\">\n"
"            <option>CD-R</option><option>CD-RW</option><option>DVD-R</option>\n"
"            <option>DVD-RW</option><option>DVD+R</option><option>DVD+RW</option>\n"
"          </select></div>\n"
"        <div><label>速度（0 = 默认）</label>\n"
"          <input id=\"speed\" type=\"number\" min=\"0\" max=\"16\" value=\"0\"></div>\n"
"      </div>\n"
"      <label>校验等级</label>\n"
"      <select id=\"verify\">\n"
"        <option value=\"0\">不校验</option>\n"
"        <option value=\"1\">快速校验</option>\n"
"        <option value=\"2\">完整校验（逐扇区）</option>\n"
"      </select>\n"
"      <div class=\"log\" id=\"log\">就绪。</div>\n"
"    </section>\n"
"  </div>\n"
"\n"
"  <div class=\"tabpane on\" id=\"t-data\">\n"
"    <div class=\"grid\" style=\"margin-top:16px\">\n"
"      <section>\n"
"        <h2>刻录镜像 / 目录</h2>\n"
"        <label>镜像文件（.iso）或源目录</label>\n"
"        <input id=\"dataSource\" placeholder=\"E:\\images\\movie.iso\">\n"
"        <label>参考镜像（用于回读校验，可选）</label>\n"
"        <input id=\"dataVerifyImage\" placeholder=\"留空则不校验\">\n"
"        <label><input type=\"checkbox\" id=\"dataLeaveOpen\" style=\"width:auto;margin-right:6px\">保留会话可追加（不封盘）</label>\n"
"        <button id=\"btnBurnData\">开始刻录</button>\n"
"      </section>\n"
"      <section>\n"
"        <h2>说明</h2>\n"
"        <p class=\"note\">「保留会话可追加」用于多会话：勾选后本次不封盘，\n"
"        之后可在「维护 / 多会话」页追加新会话。</p>\n"
"        <p class=\"note\">校验等级为快速或完整时，刻录后会自动读回光盘\n"
"        与镜像逐扇区比对。</p>\n"
"      </section>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"tabpane\" id=\"t-audio\">\n"
"    <div class=\"grid\" style=\"margin-top:16px\">\n"
"      <section>\n"
"        <h2>音频 CD（CD-DA）</h2>\n"
"        <label>音频文件（44.1kHz / 16bit / 立体声 WAV，每行一个）</label>\n"
"        <textarea id=\"audioTracks\" rows=\"6\" style=\"font-family:ui-monospace,monospace\"></textarea>\n"
"        <label><input type=\"checkbox\" id=\"audioFinalize\" style=\"width:auto;margin-right:6px\" checked>写入目录表并封盘</label>\n"
"        <button id=\"btnBurnAudio\">刻录音频 CD</button>\n"
"      </section>\n"
"      <section>\n"
"        <h2>说明</h2>\n"
"        <p class=\"note\">每条轨道至少 4 秒；过短会被写入器补静音或拒绝。</p>\n"
"        <p class=\"note\">不封盘时可在之后追加数据会话（多会话 CD）。</p>\n"
"      </section>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"tabpane\" id=\"t-iso\">\n"
"    <div class=\"grid\" style=\"margin-top:16px\">\n"
"      <section>\n"
"        <h2>生成 ISO 镜像</h2>\n"
"        <label>源目录</label>\n"
"        <input id=\"isoSource\" placeholder=\"D:\\data\">\n"
"        <label>输出镜像路径</label>\n"
"        <input id=\"isoOutput\" placeholder=\"D:\\out\\disc.iso\">\n"
"        <div class=\"row\">\n"
"          <div><label>卷标</label><input id=\"isoLabel\" value=\"DISC\"></div>\n"
"          <div><label>ISO 级别</label>\n"
"            <select id=\"isoLevel\"><option value=\"0\">默认</option>\n"
"            <option value=\"1\">1</option><option value=\"2\">2</option>\n"
"            <option value=\"3\">3</option></select></div>\n"
"        </div>\n"
"        <label>文件系统</label>\n"
"        <select id=\"isoFs\">\n"
"          <option value=\"0\">ISO9660 + Joliet</option>\n"
"          <option value=\"1\">仅 ISO9660</option>\n"
"          <option value=\"2\">ISO9660 + Joliet + UDF</option>\n"
"          <option value=\"4\">仅 UDF</option>\n"
"        </select>\n"
"        <label>El Torito 引导镜像（可选）</label>\n"
"        <input id=\"isoBoot\" placeholder=\"D:\\boot\\efi.img\">\n"
"        <button id=\"btnCreateIso\">生成镜像</button>\n"
"      </section>\n"
"      <section>\n"
"        <h2>浏览源目录</h2>\n"
"        <div class=\"row\">\n"
"          <div><input id=\"browsePath\" placeholder=\"目录路径\"></div>\n"
"          <div style=\"flex:0 0 90px\"><button class=\"ghost\" id=\"btnBrowse\">浏览</button></div>\n"
"        </div>\n"
"        <div class=\"files\" id=\"fileList\"></div>\n"
"        <p class=\"note\">双击目录进入；双击文件填入上方对应输入框。</p>\n"
"      </section>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"tabpane\" id=\"t-raw\">\n"
"    <div class=\"grid\" style=\"margin-top:16px\">\n"
"      <section>\n"
"        <h2>RAW 镜像</h2>\n"
"        <label>音频文件（每行一个，用于生成 RAW 轨）</label>\n"
"        <textarea id=\"rawTracks\" rows=\"5\" style=\"font-family:ui-monospace,monospace\"></textarea>\n"
"        <label>输出 RAW 镜像路径</label>\n"
"        <input id=\"rawOutput\" placeholder=\"D:\\out\\disc.raw\">\n"
"        <div class=\"row\">\n"
"          <div><label>MCN（13 位数字，可选）</label><input id=\"rawMcn\"></div>\n"
"          <div><label>起始轨号</label><input id=\"rawStart\" type=\"number\" min=\"0\" max=\"99\" value=\"0\"></div>\n"
"        </div>\n"
"        <button id=\"btnCreateRaw\">生成 RAW 镜像</button>\n"
"        <button class=\"ghost\" id=\"btnBurnRaw\">把 RAW 镜像写入光盘</button>\n"
"      </section>\n"
"      <section>\n"
"        <h2>说明</h2>\n"
"        <p class=\"note\">RAW 镜像由本程序生成时，与「写入光盘」配套使用。</p>\n"
"        <p class=\"note\">Windows 与 Linux 生成的 RAW 镜像格式不同，\n"
"        不能跨平台互换烧录。</p>\n"
"      </section>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class=\"tabpane\" id=\"t-maint\">\n"
"    <div class=\"grid\" style=\"margin-top:16px\">\n"
"      <section>\n"
"        <h2>擦除 / 格式化</h2>\n"
"        <label><input type=\"checkbox\" id=\"eraseFull\" style=\"width:auto;margin-right:6px\">全擦除（较慢）</label>\n"
"        <button class=\"danger\" id=\"btnErase\">擦除光盘</button>\n"
"        <h2 style=\"margin-top:18px\">托盘</h2>\n"
"        <div class=\"row\">\n"
"          <div><button class=\"ghost\" id=\"btnEject\">弹出</button></div>\n"
"          <div><button class=\"ghost\" id=\"btnClose\">收回</button></div>\n"
"        </div>\n"
"      </section>\n"
"      <section>\n"
"        <h2>多会话</h2>\n"
"        <label>下一会话起始块</label>\n"
"        <div class=\"row\">\n"
"          <div><input id=\"sessionBlock\" readonly placeholder=\"点击查询\"></div>\n"
"          <div style=\"flex:0 0 90px\"><button class=\"ghost\" id=\"btnQuerySession\">查询</button></div>\n"
"        </div>\n"
"        <label>追加内容目录</label>\n"
"        <input id=\"appendSource\" placeholder=\"D:\\data\">\n"
"        <label>会话卷标</label>\n"
"        <input id=\"appendLabel\" value=\"SESSION2\">\n"
"        <button id=\"btnAppend\">追加会话</button>\n"
"        <p class=\"note\">追加前请先用「保留会话可追加」方式刻过一张盘。</p>\n"
"      </section>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <section style=\"margin-top:16px\">\n"
"    <h2>任务进度</h2>\n"
"    <div class=\"bar\"><i id=\"bar\"></i></div>\n"
"    <div class=\"muted\" id=\"status\" style=\"margin-top:6px;font-size:12px\">无任务</div>\n"
"    <button class=\"ghost\" id=\"btnCancel\">取消当前任务</button>\n"
"  </section>\n"
"</main>\n"
"<script>\n"
"const $ = (id) => document.getElementById(id);\n"
"const logEl = $('log');\n"
"function log(msg, cls) {\n"
"  const line = document.createElement('div');\n"
"  if (cls) line.className = cls;\n"
"  line.textContent = msg;\n"
"  logEl.appendChild(line);\n"
"  logEl.scrollTop = logEl.scrollHeight;\n"
"}\n"
"async function api(path, params) {\n"
"  const q = new URLSearchParams(params || {}).toString();\n"
"  const res = await fetch(path + (q ? '?' + q : ''));\n"
"  return await res.json();\n"
"}\n"
"/* ---- tabs ---- */\n"
"document.querySelectorAll('.tabs button').forEach(b => {\n"
"  b.onclick = () => {\n"
"    document.querySelectorAll('.tabs button').forEach(x => x.classList.remove('on'));\n"
"    document.querySelectorAll('.tabpane').forEach(x => x.classList.remove('on'));\n"
"    b.classList.add('on');\n"
"    $(b.dataset.tab).classList.add('on');\n"
"  };\n"
"});\n"
"/* ---- capabilities and devices ---- */\n"
"let CAPS = {};\n"
"async function refresh() {\n"
"  const caps = await api('/api/capabilities');\n"
"  CAPS = caps.capabilities || {};\n"
"  $('hdr-meta').textContent = 'API v' + caps.apiVersion + ' · ' + caps.platform;\n"
"  const box = $('caps'); box.innerHTML = '';\n"
"  const names = { image_burn:'数据盘刻录', create_image:'镜像生成', audio_tao:'音频 CD',\n"
"    erase:'擦除', multisession:'多会话', udf:'UDF', boot_image:'El Torito 引导',\n"
"    readback_verify:'回读校验', raw_burn:'RAW 写入', raw_image:'RAW 镜像',\n"
"    tray_control:'托盘控制', drive_info:'设备信息' };\n"
"  for (const k in names) {\n"
"    const s = document.createElement('span');\n"
"    s.className = 'cap ' + (CAPS[k] ? 'on' : 'off');\n"
"    s.textContent = names[k] + (CAPS[k] ? ' ✓' : ' ✗');\n"
"    box.appendChild(s);\n"
"  }\n"
"  const dev = await api('/api/devices');\n"
"  const sel = $('device'); const prev = sel.value;\n"
"  sel.innerHTML = '';\n"
"  if (!dev.devices || !dev.devices.length) {\n"
"    const o = document.createElement('option'); o.textContent = '(未找到刻录设备)'; o.value='';\n"
"    sel.appendChild(o);\n"
"    log('未找到刻录设备。可在「镜像制作」页生成镜像。', 'warn');\n"
"  } else {\n"
"    dev.devices.forEach(d => {\n"
"      const o = document.createElement('option');\n"
"      o.value = d.path;\n"
"      o.textContent = d.path + ' — ' + (d.name || '') + (d.writable ? '' : ' [只读/不可写]');\n"
"      sel.appendChild(o);\n"
"    });\n"
"    if (prev) sel.value = prev;\n"
"  }\n"
"  updateFsOptions();\n"
"}\n"
"/* The UDF choice must follow the backend's capability. */\n"
"function updateFsOptions() {\n"
"  const sel = $('isoFs');\n"
"  for (const opt of sel.options) {\n"
"    const v = parseInt(opt.value, 10);\n"
"    const needsUdf = (v === 2 || v === 4);\n"
"    opt.disabled = needsUdf && !CAPS.udf;\n"
"    if (needsUdf && !CAPS.udf) opt.textContent = opt.textContent.replace('（本后端不支持）','') + '（本后端不支持）';\n"
"  }\n"
"}\n"
"async function showDriveInfo() {\n"
"  const dev = $('device').value;\n"
"  if (!dev) { $('devinfo').innerHTML = ''; return; }\n"
"  const r = await api('/api/driveinfo', { device: dev });\n"
"  const box = $('devinfo'); box.innerHTML = '';\n"
"  if (r.error) { box.innerHTML = '<p class=\"note err\">' + r.error + '</p>'; return; }\n"
"  const pre = document.createElement('div');\n"
"  pre.className = 'log';\n"
"  pre.style.maxHeight = '180px';\n"
"  pre.textContent = r.info;\n"
"  box.appendChild(pre);\n"
"}\n"
"$('device').onchange = showDriveInfo;\n"
"/* ---- job runner ---- */\n"
"let pollTimer = null, currentJob = 0;\n"
"async function runJob(params, label) {\n"
"  log('▶ ' + label);\n"
"  const r = await api('/api/start', params);\n"
"  if (!r.ok) { log('✗ ' + r.error, 'err'); return; }\n"
"  currentJob = r.id;\n"
"  $('status').textContent = label + ' 进行中...';\n"
"  if (pollTimer) clearInterval(pollTimer);\n"
"  pollTimer = setInterval(async () => {\n"
"    const s = await api('/api/job', { id: currentJob });\n"
"    if (s.error) { clearInterval(pollTimer); pollTimer = null; log('✗ ' + s.error, 'err'); return; }\n"
"    $('bar').style.width = s.percent + '%';\n"
"    $('status').textContent = label + ' · ' + s.percent + '% · ' + (s.phase || s.message || '');\n"
"    if (s.state === 2) { clearInterval(pollTimer); pollTimer = null;\n"
"      $('bar').style.width = '100%'; log('✔ 完成', 'ok'); }\n"
"    else if (s.state === 3) { clearInterval(pollTimer); pollTimer = null;\n"
"      log('✗ 失败: ' + (s.error || '未知错误'), 'err'); }\n"
"    else if (s.state === 4) { clearInterval(pollTimer); pollTimer = null;\n"
"      log('⚠ 已取消', 'warn'); }\n"
"  }, 400);\n"
"}\n"
"function common() {\n"
"  return { device: $('device').value, discType: $('discType').value,\n"
"           speed: $('speed').value, verify: $('verify').value };\n"
"}\n"
"$('btnBurnData').onclick = () => {\n"
"  const p = common();\n"
"  p.mode = 0; p.source = $('dataSource').value;\n"
"  p.leaveOpen = $('dataLeaveOpen').checked ? 1 : 0;\n"
"  runJob(p, '刻录数据盘');\n"
"};\n"
"$('btnBurnAudio').onclick = () => {\n"
"  const p = common();\n"
"  p.mode = 4; p.tracks = $('audioTracks').value.split('\\n').map(s => s.trim()).filter(Boolean).join('|');\n"
"  runJob(p, '刻录音频 CD');\n"
"};\n"
"$('btnCreateIso').onclick = () => {\n"
"  const p = { mode: 2, source: $('isoSource').value, output: $('isoOutput').value,\n"
"              label: $('isoLabel').value, fileSystems: $('isoFs').value,\n"
"              isoLevel: $('isoLevel').value, boot: $('isoBoot').value };\n"
"  runJob(p, '生成 ISO 镜像');\n"
"};\n"
"$('btnCreateRaw').onclick = () => {\n"
"  const p = { mode: 8, tracks: $('rawTracks').value.split('\\n').map(s => s.trim()).filter(Boolean).join('|'),\n"
"              output: $('rawOutput').value, mcn: $('rawMcn').value,\n"
"              startTrack: $('rawStart').value };\n"
"  runJob(p, '生成 RAW 镜像');\n"
"};\n"
"$('btnBurnRaw').onclick = () => {\n"
"  const p = common();\n"
"  p.mode = 9; p.source = $('rawOutput').value;\n"
"  runJob(p, '刻录 RAW 镜像');\n"
"};\n"
"$('btnErase').onclick = () => {\n"
"  const p = common();\n"
"  p.mode = 5; p.eraseFull = $('eraseFull').checked ? 1 : 0;\n"
"  runJob(p, '擦除光盘');\n"
"};\n"
"$('btnAppend').onclick = () => {\n"
"  const p = common();\n"
"  p.mode = 6; p.source = $('appendSource').value; p.label = $('appendLabel').value;\n"
"  runJob(p, '追加会话');\n"
"};\n"
"$('btnCancel').onclick = async () => {\n"
"  if (!currentJob) { log('没有正在进行的任务', 'warn'); return; }\n"
"  const r = await api('/api/cancel', { id: currentJob });\n"
"  log(r.ok ? '已请求取消' : '取消失败: ' + r.error, r.ok ? 'warn' : 'err');\n"
"};\n"
"$('btnEject').onclick = async () => {\n"
"  const r = await api('/api/tray', { device: $('device').value, action: 'eject' });\n"
"  log(r.ok ? '已弹出' : '弹出失败: ' + r.error, r.ok ? 'ok' : 'err');\n"
"};\n"
"$('btnClose').onclick = async () => {\n"
"  const r = await api('/api/tray', { device: $('device').value, action: 'close' });\n"
"  log(r.ok ? '已收回' : '收回失败: ' + r.error, r.ok ? 'ok' : 'err');\n"
"};\n"
"$('btnQuerySession').onclick = async () => {\n"
"  const r = await api('/api/session', { device: $('device').value });\n"
"  if (r.ok) { $('sessionBlock').value = r.block; log('下一会话起始块: ' + r.block, 'ok'); }\n"
"  else log('查询失败: ' + r.error, 'warn');\n"
"};\n"
"/* ---- file browser ---- */\n"
"async function browse(path) {\n"
"  const r = await api('/api/browse', { path: path || $('browsePath').value });\n"
"  const list = $('fileList'); list.innerHTML = '';\n"
"  if (r.error) { list.innerHTML = '<div class=\"err\">' + r.error + '</div>'; return; }\n"
"  $('browsePath').value = r.path;\n"
"  const add = (name, path, isDir) => {\n"
"    const d = document.createElement('div');\n"
"    if (isDir) d.className = 'dir';\n"
"    d.textContent = (isDir ? '📁 ' : '📄 ') + name;\n"
"    d.ondblclick = () => {\n"
"      if (isDir) { browse(path); }\n"
"      else if (/\\.wav$/i.test(name)) { $('audioTracks').value += (\\n ? '\\\\n' : '') + path; }\n"
"      else if (/\\.iso$/i.test(name)) { $('dataSource').value = path; }\n"
"      else if (/\\.(img|bin)$/i.test(name)) { $('isoBoot').value = path; }\n"
"      else if (/\\.raw$/i.test(name)) { $('rawOutput').value = path; }\n"
"      else { $('isoSource').value = path; }\n"
"    };\n"
"    list.appendChild(d);\n"
"  };\n"
"  if (r.parent) add('..', r.parent, true);\n"
"  r.entries.forEach(e => add(e.name, e.path, e.isDir));\n"
"}\n"
"$('btnBrowse').onclick = () => browse();\n"
"/* ---- start ---- */\n"
"refresh().then(showDriveInfo);\n"
"setInterval(() => { if (!pollTimer) refresh(); }, 5000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ==========================================================================
 * HTTP plumbing
 * ======================================================================== */
void SendAll(SOCKET_TYPE sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int n = ::send(sock, data.data() + sent, (int)(data.size() - sent), 0);
#else
        const ssize_t n = ::send(sock, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

void SendResponse(SOCKET_TYPE sock, int code, const std::string& contentType,
                  const std::string& body) {
    const char* reason = (code == 200) ? "OK" : "Not Found";
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + std::to_string((unsigned long long)body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += "Connection: close\r\n\r\n";
    SendAll(sock, head);
    SendAll(sock, body);
}

/* Set by --shutdown-after: the server stops once this many requests have
 * been served, so a test can end it without waiting for a timeout. */
std::atomic<int> g_requestLimit{0};
std::atomic<int> g_servedRequests{0};

/* The socket handle is passed by value; on Windows SOCKET is a 64-bit
 * handle, so the parameter must not be an int. */
void HandleConnection(SOCKET_TYPE sock) {
    /* Read the request head; the body is not used by this UI (all calls are
     * GET with query parameters). */
    std::string raw;
    char buf[8192];
    for (;;) {
#ifdef _WIN32
        const int n = ::recv(sock, buf, (int)sizeof(buf), 0);
#else
        const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (raw.size() > 65536) break;
    }

    Request req;
    const size_t sp = raw.find(' ');
    if (sp == std::string::npos) { CLOSE_SOCKET(sock); return; }
    req.method = raw.substr(0, sp);
    const size_t sp2 = raw.find(' ', sp + 1);
    if (sp2 == std::string::npos) { CLOSE_SOCKET(sock); return; }
    req.path = raw.substr(sp + 1, sp2 - sp - 1);

    const size_t q = req.path.find('?');
    if (q != std::string::npos) {
        req.fields = ParseQuery(req.path.substr(q + 1));
        req.path = req.path.substr(0, q);
    }

    std::string body;
    const char* type = "text/plain; charset=utf-8";
    if (req.path == "/" || req.path == "/index.html") {
        body = kPage;
        type = "text/html; charset=utf-8";
    } else if (req.path == "/api/capabilities") {
        body = HandleCapabilities();
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/devices") {
        body = HandleDevices();
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/driveinfo") {
        body = HandleDriveInfo(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/session") {
        body = HandleSessionBlock(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/browse") {
        body = HandleBrowse(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/tray") {
        body = HandleTray(req, Field(req, "action") == "eject");
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/start") {
        body = HandleStart(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/job") {
        body = HandleJobStatus(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/cancel") {
        body = HandleCancel(req);
        type = "application/json; charset=utf-8";
    } else {
        SendResponse(sock, 404, "text/plain; charset=utf-8", "not found");
        CLOSE_SOCKET(sock);
        return;
    }
    SendResponse(sock, 200, type, body);
    CLOSE_SOCKET(sock);

    /* Test hook: stop after the configured number of requests. */
    if (g_requestLimit.load() > 0 &&
        ++g_servedRequests >= g_requestLimit.load()) {
        std::printf("  request limit reached, shutting down\n");
        std::fflush(stdout);
#ifdef _WIN32
        ::closesocket(sock);
        WSACleanup();
#endif
        std::exit(0);
    }
}

} /* namespace */

/* ==========================================================================
 * Entry point
 * ======================================================================== */
int main(int argc, char* argv[]) {
    int port = 8712;
    bool openBrowser = true;
    int autoExitSeconds = 0;   /* >0: shut down on its own, for the test */
    int autoExitAfterRequests = 0;  /* >0: shut down after N requests */
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--no-browser") {
            openBrowser = false;
        } else if (a == "--exit-after" && i + 1 < argc) {
            /* Used by the automated test so the server never outlives it. */
            autoExitSeconds = std::atoi(argv[++i]);
        } else if (a == "--shutdown-after" && i + 1 < argc) {
            /* Stop the server once the given number of requests has been
             * served. This is how the test ends it without waiting for a
             * timeout. */
            autoExitAfterRequests = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            std::printf("burn_webui [--port <n>] [--no-browser] [--exit-after <s>]\n"
                        "  Starts a local web console for the burning library.\n"
                        "  It binds to 127.0.0.1 only.\n"
                        "  --exit-after stops the server after the given seconds.\n");
            return 0;
        }
    }
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "Invalid port\n");
        return 2;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    const SOCKET_TYPE sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::fprintf(stderr, "socket() failed\n");
        return 1;
    }
    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    /* Loopback only: this program can destroy media. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::fprintf(stderr, "Cannot bind 127.0.0.1:%d (already in use?)\n", port);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    if (::listen(sock, 8) != 0) {
        std::fprintf(stderr, "listen() failed\n");
        return 1;
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::printf("BurningUtility console\n");
    std::printf("  open: %s\n", url.c_str());
    std::printf("  press Ctrl+C to stop\n");
    std::fflush(stdout);

    if (openBrowser) {
#ifdef _WIN32
        /* ShellExecute with the default browser; no user data is involved. */
        ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
        std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
        if (std::system(cmd.c_str()) != 0) {
            std::printf("  (could not open a browser; open the URL manually)\n");
        }
#endif
    }

    if (autoExitAfterRequests > 0) g_requestLimit.store(autoExitAfterRequests);

    /* Optional self-termination, so an automated run never leaves a server
     * behind. */
    if (autoExitSeconds > 0) {
        std::thread([&]() {
            for (int i = 0; i < autoExitSeconds * 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
#ifdef _WIN32
            ::closesocket(sock);
#endif
            std::exit(0);
        }).detach();
    }

    for (;;) {
        sockaddr_in peer;
#ifdef _WIN32
        int plen = sizeof(peer);
#else
        socklen_t plen = sizeof(peer);
#endif
        const SOCKET_TYPE client = ::accept(sock, (sockaddr*)&peer, &plen);
        if (client < 0) continue;
        /* One thread per request: the UI has very low request volume and
         * this keeps the handler code free of locking. */
        std::thread(HandleConnection, client).detach();
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
