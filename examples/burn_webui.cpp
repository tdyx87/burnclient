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
            /* "media" is the legacy device class ("CD/DVD"). "mediaType" is
             * the physical disc actually in the drive, e.g. "DVD+RW". */
            const char* disc = Burn_GetMediaType(devices[i].devicePath);
            const int discCode = Burn_GetMediaTypeCode(devices[i].devicePath);
            /* Media readiness is a separate question from disc type: a real
             * recorder with an empty tray enumerates fine but cannot burn. */
            const int state = Burn_GetMediaState(devices[i].devicePath);
            const char* reason = Burn_GetMediaStateText(devices[i].devicePath);
            const bool ready = state > 0 &&
                               (state & BURN_MEDIA_STATE_READY) != 0;
            out += "{\"path\":" + JsonString(devices[i].devicePath) +
                   ",\"name\":" + JsonString(devices[i].deviceName) +
                   ",\"media\":" + JsonString(devices[i].mediaType) +
                   ",\"writable\":" + (devices[i].isWritable ? "true" : "false") +
                   ",\"ready\":" + (ready ? "true" : "false") +
                   ",\"state\":" + std::to_string(state) +
                   ",\"reason\":" + JsonString(reason ? reason : "") +
                   ",\"mediaType\":" + JsonString(disc ? disc : "") +
                   ",\"mediaCode\":" + std::to_string(discCode) +
                   "}";
        }
        FreeDeviceList(devices, count);
    }
    out += "]}";
    return out;
}

/* Structured query for the disc currently in one drive. */
std::string HandleMedia(const Request& r) {
    const std::string device = Field(r, "device");
    if (device.empty()) {
        return "{\"ok\":false,\"error\":\"device is required\"}";
    }
    const char* name = Burn_GetMediaType(device.c_str());
    const int code = Burn_GetMediaTypeCode(device.c_str());
    const int state = Burn_GetMediaState(device.c_str());
    const char* reason = Burn_GetMediaStateText(device.c_str());
    const bool ready = state > 0 && (state & BURN_MEDIA_STATE_READY) != 0;
    std::string out = "{\"ok\":true,\"device\":" + JsonString(device) +
                      ",\"type\":" + JsonString(name ? name : "") +
                      ",\"code\":" + std::to_string(code) +
                      ",\"state\":" + std::to_string(state) +
                      ",\"ready\":" + (ready ? "true" : "false") +
                      ",\"reason\":" + JsonString(reason ? reason : "");
    if (!name || !*name) {
        out += ",\"note\":" + JsonString(GetLastErrorMessage());
    }
    out += "}";
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
    /* The data-disc pane accepts a folder as well as an image file: files
     * dropped onto the page are staged into a directory on this machine.
     * Mode IMAGE would hand that directory to the writer as though it were
     * an ISO, and Windows fails with 0x80070005 when asked to open a
     * directory as a stream. Normalise it here so every client gets the
     * ISO built from the folder first, not just our own page. */
    if (o.mode == BURN_JOB_MODE_IMAGE && !j.sourcePath.empty() &&
        IsDirectory(j.sourcePath)) {
        o.mode = BURN_JOB_MODE_DIRECTORY;
    }
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

    /* Reject a burn with no usable disc before starting a job. The library
     * also preflights this in BurnImageFile(), but answering here gives the
     * browser a clear reason instead of a job that fails a second later.
     * Read-only modes (CREATE_IMAGE, SELFTEST, VERIFY, CREATE_RAW) and the
     * erase mode have different media requirements and are left alone. */
    if (!j.devicePath.empty() &&
        (o.mode == BURN_JOB_MODE_IMAGE || o.mode == BURN_JOB_MODE_DIRECTORY ||
         o.mode == BURN_JOB_MODE_APPEND)) {
        const int state = Burn_GetMediaState(j.devicePath.c_str());
        if (state >= 0 &&
            !(state & BURN_MEDIA_STATE_QUERY_UNSUPPORTED) &&
            !(state & BURN_MEDIA_STATE_READY)) {
            const char* reason = Burn_GetMediaStateText(j.devicePath.c_str());
            const std::string message =
                "Device " + j.devicePath + " cannot be used for burning right now. " +
                (reason && *reason ? reason : "No writable disc detected.");
            return "{\"ok\":false,\"error\":" + JsonString(message) +
                   ",\"state\":" + std::to_string(state) + "}";
        }
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
 * Upload staging
 *
 * A browser never reveals the real path of a file the user dropped on the
 * page, so "drag files here and burn them" can only work by copying those
 * bytes to this machine. The server writes them into a per-session staging
 * directory and hands that directory back; the burn job then uses it as the
 * source folder.
 * ======================================================================== */

/* Keep only characters that are safe in a file name. */
static std::string SanitizeComponent(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out += (char)c;
        } else if (c >= 0x80) {
            out += (char)c;   /* keep UTF-8 file names intact */
        }
    }
    while (!out.empty() && out[0] == '.') out.erase(out.begin());
    if (out.size() > 120) out = out.substr(out.size() - 120);
    if (out.empty()) out = "file";
    return out;
}

static std::string StagingRoot() {
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMP");
#else
    /* POSIX prefers TMPDIR; TMP/TEMP are honoured too because the Windows
     * spelling is what a cross-platform caller usually sets. */
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
#endif
    if (!tmp || !*tmp) tmp = ".";
    return std::string(tmp) + PATHSEP + "burn_webui_stage";
}

/* Create a directory and its parents, ignoring "already exists". */
static bool EnsureDir(const std::string& path) {
    if (IsDirectory(path)) return true;
    const std::string parent = ParentPath(path);
    if (!parent.empty() && parent != path && !IsDirectory(parent)) {
        EnsureDir(parent);
    }
#ifdef _WIN32
    const int rc = _mkdir(path.c_str());
#else
    const int rc = mkdir(path.c_str(), 0777);
#endif
    return rc == 0 || IsDirectory(path);
}

/* Delete a directory tree; used when the user clears the list. */
static void RemoveTree(const std::string& path) {
    if (!IsDirectory(path)) {
        std::remove(path.c_str());
        return;
    }
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA((path + "\\*").c_str(), &data);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = data.cFileName;
            if (name == "." || name == "..") continue;
            const std::string full = path + "\\" + name;
            if (IsDirectory(full)) RemoveTree(full);
            else DeleteFileA(full.c_str());
        } while (FindNextFileA(h, &data));
        FindClose(h);
    }
    RemoveDirectoryA(path.c_str());
#else
    DIR* dir = opendir(path.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = path + "/" + name;
            if (IsDirectory(full)) RemoveTree(full);
            else std::remove(full.c_str());
        }
        closedir(dir);
    }
    rmdir(path.c_str());
#endif
}

/* Write the request body into the session staging directory. */
std::string HandleUpload(const Request& r) {
    const std::string rawSid = Field(r, "sid");
    if (rawSid.empty()) {
        return "{\"ok\":false,\"error\":" +
               JsonString("Missing upload session id") + "}";
    }
    std::string sid = SanitizeComponent(rawSid);
    if (sid.empty()) {
        return "{\"ok\":false,\"error\":" +
               JsonString("Invalid upload session id") + "}";
    }
    std::string name = Field(r, "name");
    if (name.empty()) name = "file.bin";
    /* Only the base name, never a path. */
    {
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
    }
    name = SanitizeComponent(name);

    const std::string dir = StagingRoot() + PATHSEP + sid;
    if (!EnsureDir(dir)) {
        return "{\"ok\":false,\"error\":" +
               JsonString("Cannot create the staging directory: " + dir) + "}";
    }

    /* Prefix with a counter so two files with the same name both survive. */
    static std::mutex counterMutex;
    static unsigned long long counter = 0;
    unsigned long long serial = 0;
    {
        std::lock_guard<std::mutex> lock(counterMutex);
        serial = ++counter;
    }
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "%04llu_", serial);
    const std::string target = dir + PATHSEP + prefix + name;

    FILE* f = std::fopen(target.c_str(), "wb");
    if (!f) {
        return "{\"ok\":false,\"error\":" +
               JsonString("Cannot write " + target) + "}";
    }
    const size_t written = r.body.empty()
                               ? 0
                               : std::fwrite(r.body.data(), 1, r.body.size(), f);
    const int closeErr = (std::fclose(f) != 0);
    if (written != r.body.size() || closeErr) {
        std::remove(target.c_str());
        return "{\"ok\":false,\"error\":" +
               JsonString("Short write (disk full?) for " + target) + "}";
    }

    return "{\"ok\":true,\"dir\":" + JsonString(dir) +
           ",\"path\":" + JsonString(target) +
           ",\"name\":" + JsonString(name) +
           ",\"size\":" + std::to_string((unsigned long long)r.body.size()) + "}";
}

std::string HandleStageClear(const Request& r) {
    const std::string rawSid = Field(r, "sid");
    if (rawSid.empty()) {
        return "{\"ok\":false,\"error\":" + JsonString("Missing session id") + "}";
    }
    const std::string sid = SanitizeComponent(rawSid);
    if (sid.empty()) {
        return "{\"ok\":false,\"error\":" + JsonString("Invalid session id") + "}";
    }
    RemoveTree(StagingRoot() + PATHSEP + sid);
    return "{\"ok\":true}";
}

/* ==========================================================================
 * The page
 *
 * It is shipped as a real HTML file next to this source, so that the script
 * inside it can be checked by ordinary JavaScript tooling instead of only by
 * the C++ compiler. Keeping it in a file also means no escaping mistake can
 * corrupt it: an earlier version embedded the page as a C string literal and
 * a wrongly escaped newline silently broke the whole script block.
 *
 * The file is found next to the executable (the installed SDK layout) or via
 * the BURN_WEBUI_PAGE_DIR environment variable (the build tree).
 * ======================================================================== */
namespace {
const char* const kPageFile = "console.html";
}

/* The directory the running executable lives in, or an empty string. */
static std::string ExecutableDir() {
#ifdef _WIN32
    char exePath[MAX_PATH] = {0};
    const DWORD len = ::GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    std::string path(exePath, len);
#else
    /* /proc/self/exe resolves the real path of the running binary. */
    char buf[4096];
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';
    std::string path(buf);
#endif
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return "";
    return path.substr(0, slash);
}

/* Read a whole file into a string. */
static bool ReadWholeFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

static std::string ReadPageFile() {
    /* 1) next to the executable: the installed layout puts the page in the
     *    same directory as the binary. */
    const std::string dir = ExecutableDir();
    if (!dir.empty()) {
        std::vector<std::string> candidates;
        candidates.push_back(dir + PATHSEP + kPageFile);
        candidates.push_back(dir + PATHSEP + ".." + PATHSEP + "examples" + PATHSEP + kPageFile);
        candidates.push_back(dir + PATHSEP + ".." + PATHSEP + ".." + PATHSEP + "examples" +
                             PATHSEP + kPageFile);
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::string content;
            if (ReadWholeFile(candidates[i], content)) return content;
        }
    }
    /* 2) the source tree, for running from the build directory */
    const char* envDir = std::getenv("BURN_WEBUI_PAGE_DIR");
    if (envDir && *envDir) {
        std::string content;
        const std::string p = std::string(envDir) + PATHSEP + kPageFile;
        if (ReadWholeFile(p, content)) return content;
    }
    return "";
}

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

/* Case-insensitive header lookup on the raw request head. */
static bool FindHeader(const std::string& head, const char* name, std::string& out) {
    const size_t nameLen = std::strlen(name);
    for (size_t i = 0; i + nameLen < head.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < nameLen && match; ++k) {
            char a = head[i + k];
            char b = name[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) match = false;
        }
        if (!match) continue;
        /* Must be at the start of a line. */
        if (i != 0 && head[i - 1] != '\n') continue;
        const size_t colon = head.find(':', i + nameLen);
        if (colon == std::string::npos) continue;
        size_t start = colon + 1;
        while (start < head.size() && (head[start] == ' ' || head[start] == '\t')) ++start;
        size_t end = head.find("\r\n", start);
        if (end == std::string::npos) end = head.size();
        out = head.substr(start, end - start);
        return true;
    }
    return false;
}

/* The socket handle is passed by value; on Windows SOCKET is a 64-bit
 * handle, so the parameter must not be an int. */
void HandleConnection(SOCKET_TYPE sock) {
    /* Read the request head, then the body when Content-Length is present.
     * GET calls carry their parameters in the query; uploads POST raw bytes. */
    std::string raw;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    for (;;) {
#ifdef _WIN32
        const int n = ::recv(sock, buf, (int)sizeof(buf), 0);
#else
        const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (raw.size() > 65536) break;
    }
    if (headerEnd == std::string::npos) { CLOSE_SOCKET(sock); return; }

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

    /* Finish reading the body if the client announced its length. */
    {
        std::string lengthHeader;
        size_t wanted = 0;
        if (FindHeader(raw.substr(0, headerEnd), "content-length", lengthHeader)) {
            wanted = (size_t)std::strtoull(lengthHeader.c_str(), nullptr, 10);
        }
        std::string bodyBytes = raw.substr(headerEnd + 4);
        while (bodyBytes.size() < wanted) {
#ifdef _WIN32
            const int n = ::recv(sock, buf, (int)sizeof(buf), 0);
#else
            const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            bodyBytes.append(buf, (size_t)n);
        }
        if (bodyBytes.size() > wanted) bodyBytes.resize(wanted);
        req.body = bodyBytes;
    }

    std::string body;
    const char* type = "text/plain; charset=utf-8";
    if (req.path == "/" || req.path == "/index.html") {
        body = ReadPageFile();
        type = "text/html; charset=utf-8";
        if (body.empty()) {
            /* The page file was not found: say so plainly instead of
             * serving an empty page that looks broken. */
            body = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                   "<title>光盘刻录工具</title></head><body style=\"font:16px sans-serif;"
                   "padding:40px;background:#0d1117;color:#e8edf5\">"
                   "<h1>界面文件缺失</h1>"
                   "<p>找不到 <code>console.html</code>。它应当与 burn_webui 可执行文件"
                   "放在同一目录，或放在 examples 目录中。</p>"
                   "<p>设置环境变量 <code>BURN_WEBUI_PAGE_DIR</code> 指向它所在的目录即可。</p>"
                   "</body></html>";
        }
    } else if (req.path == "/api/capabilities") {
        body = HandleCapabilities();
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/devices") {
        body = HandleDevices();
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/driveinfo") {
        body = HandleDriveInfo(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/media") {
        body = HandleMedia(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/session") {
        body = HandleSessionBlock(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/browse") {
        body = HandleBrowse(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/upload") {
        body = HandleUpload(req);
        type = "application/json; charset=utf-8";
    } else if (req.path == "/api/stage/clear") {
        body = HandleStageClear(req);
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
