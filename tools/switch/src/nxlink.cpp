// The log on a console. Sent by nxlink, the game sends stdout and stderr back to
// the computer that sent it and writes no log file. Started any other way, each line also goes to
// the system log manager (the lm service), which a development unit's Target Manager relays to the
// host and Atmosphère's lm can keep. An NRO started from the Homebrew Menu also writes the configured log file
// in its own directory, replaced at every start and capped in size; a worker thread writes it, since
// one SD card write can take tens of milliseconds. An installed application (a development unit's
// NSP) writes no file and removes one an earlier run left. libnx calls userAppInit and userAppExit
// around main.

#include <arpa/inet.h>
#include <switch.h>
#include <sys/iosupport.h>
#include <sys/reent.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "switch_main.hpp"
#include "switch_memory.hpp"

namespace {
int  g_socket = -1;
bool g_sockets = false;

constexpr const char *kLogFileName = KZ_SWITCH_LOG_FILE;
constexpr std::size_t kLogFileLimit = std::size_t{4} << 20;

// lm's ILogService::OpenLogger and ILogger::Log, and the packet ILogger::Log takes: a 0x18-byte
// header followed by chunks, each a ULEB128 key, a ULEB128 size and the data. A message may span
// packets; the first carries the head flag and the last the tail flag.
constexpr std::uint32_t kOpenLogger = 0;
constexpr std::uint32_t kLog = 0;

enum class LogFlag : std::uint8_t {
    Head = 1 << 0,
    Tail = 1 << 1,
    LittleEndian = 1 << 2,
};

enum class LogChunk : std::uint8_t {
    SessionBegin = 0,
    Text = 2,
    ModuleName = 6,
    ProcessName = 10,
};

constexpr std::uint8_t kSeverityInfo = 1;

struct LogPacketHeader {
    std::uint64_t process_id; // filled in by lm
    std::uint64_t thread_id;
    std::uint8_t  flags;
    std::uint8_t  padding;
    std::uint8_t  severity;
    std::uint8_t  verbosity;
    std::uint32_t payload_size;
};

static_assert(sizeof(LogPacketHeader) == 0x18);

constexpr std::size_t kPacketSize = 0x400;
// Leaves room in a packet for the chunk's key and size.
constexpr std::size_t kLineSize = 0x300;

struct LogSink {
    Mutex mutex = {};
    // lm, written synchronously a line at a time.
    Service                logger = {};
    bool                   lm = false;
    std::vector<std::byte> packet;
    std::vector<char>      line;
    // The log file, written by the worker from pending.
    FILE             *file = nullptr;
    Thread            writer = {};
    bool              writer_running = false;
    CondVar           wake = {};
    std::vector<char> pending;
    std::size_t       accepted = 0;
    bool              stopping = false;
};

Mutex    g_log_lifetime = {};
LogSink *g_log = nullptr;

std::size_t PutUleb128(std::span<std::byte> out, std::size_t value) {
    std::size_t size = 0;
    do {
        const auto low = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
        out[size++] = static_cast<std::byte>(low | (value != 0 ? 0x80 : 0));
    } while (value != 0);
    return size;
}

std::size_t PutChunk(std::span<std::byte> out, LogChunk key, std::span<const std::byte> data) {
    std::size_t size = PutUleb128(out, static_cast<std::size_t>(key));
    size += PutUleb128(out.subspan(size), data.size());
    std::ranges::copy(data, out.subspan(size).begin());
    return size + data.size();
}

std::span<const std::byte> Bytes(std::string_view text) { return std::as_bytes(std::span(text)); }

using Chunk = std::pair<LogChunk, std::span<const std::byte> >;

// One packet holding a whole message.
void SendPacket(std::span<const Chunk> chunks) {
    std::span<std::byte> payload = std::span(g_log->packet).subspan(sizeof(LogPacketHeader));
    std::size_t          size = 0;
    for (const auto &[key, data] : chunks) {
        size += PutChunk(payload.subspan(size), key, data);
    }
    std::uint64_t thread_id = 0;
    static_cast<void>(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE));
    constexpr auto        kFlags = static_cast<std::uint8_t>(static_cast<std::uint8_t>(LogFlag::Head) |
                                                             static_cast<std::uint8_t>(LogFlag::Tail) |
                                                             static_cast<std::uint8_t>(LogFlag::LittleEndian));
    const LogPacketHeader header = {
        .process_id = 0,
        .thread_id = thread_id,
        .flags = kFlags,
        .padding = 0,
        .severity = kSeverityInfo,
        .verbosity = 0,
        .payload_size = static_cast<std::uint32_t>(size),
    };
    std::memcpy(g_log->packet.data(), &header, sizeof(header));
    static_cast<void>(serviceDispatch(&g_log->logger, kLog,
                                      .buffer_attrs = {
                                          SfBufferAttr_In | SfBufferAttr_HipcAutoSelect
    },
                                      .buffers = {{g_log->packet.data(), sizeof(header) + size}}));
}

void SendLine() {
    if (g_log->line.empty()) {
        return;
    }
    const Chunk text[] = {
        {LogChunk::Text, std::as_bytes(std::span(g_log->line))}
    };
    SendPacket(text);
    g_log->line.clear();
}

void OpenLogManager() {
    Service service;
    if (R_FAILED(smGetService(&service, "lm"))) {
        return;
    }
    const std::uint64_t pid_placeholder = 0;
    const Result        result = serviceDispatchIn(&service, kOpenLogger, pid_placeholder, .in_send_pid = true,
                                                   .out_num_objects = 1, .out_objects = &g_log->logger);
    serviceClose(&service);
    if (R_FAILED(result)) {
        return;
    }
    g_log->lm = true;
    g_log->packet.resize(kPacketSize);
    g_log->line.reserve(kLineSize);
    constexpr std::byte session{1};
    const Chunk         begin[] = {
        {LogChunk::SessionBegin, std::span(&session, 1)},
        {LogChunk::ModuleName, Bytes(KZ_SWITCH_APPLICATION_NAME)},
        {LogChunk::ProcessName, Bytes(KZ_SWITCH_APPLICATION_NAME)},
    };
    SendPacket(begin);
}

void WriteFile(void *argument) {
    auto             *log = static_cast<LogSink *>(argument);
    std::vector<char> batch;
    mutexLock(&log->mutex);
    for (;;) {
        while (log->pending.empty() && !log->stopping) {
            condvarWait(&log->wake, &log->mutex);
        }
        if (log->pending.empty()) {
            break;
        }
        batch.swap(log->pending);
        mutexUnlock(&log->mutex);
        std::fwrite(batch.data(), 1, batch.size(), log->file);
        std::fflush(log->file);
        batch.clear();
        mutexLock(&log->mutex);
    }
    mutexUnlock(&log->mutex);
}

// The file takes the first kLogFileLimit bytes of a run, then a line saying it stopped.
void QueueFile(const char *data, std::size_t size) {
    if (g_log->accepted >= kLogFileLimit) {
        return;
    }
    const std::size_t taken = std::min(size, kLogFileLimit - g_log->accepted);
    g_log->pending.insert(g_log->pending.end(), data, data + taken);
    g_log->accepted += taken;
    if (g_log->accepted >= kLogFileLimit) {
        constexpr std::string_view kStopped = "\nlog: " KZ_SWITCH_LOG_FILE " reached its size limit; later lines go to lm only\n";
        g_log->pending.insert(g_log->pending.end(), kStopped.begin(), kStopped.end());
    }
    condvarWakeOne(&g_log->wake);
}

// stdout and stderr.
ssize_t WriteLog(struct _reent *, void *, const char *data, size_t size) {
    mutexLock(&g_log_lifetime);
    if (g_log == nullptr) {
        mutexUnlock(&g_log_lifetime);
        return static_cast<ssize_t>(size);
    }
    mutexLock(&g_log->mutex);
    if (g_log->writer_running) {
        QueueFile(data, size);
    }
    if (g_log->lm) {
        for (std::size_t i = 0; i < size; ++i) {
            g_log->line.push_back(data[i]);
            if (data[i] == '\n' || g_log->line.size() == kLineSize) {
                SendLine();
            }
        }
    }
    mutexUnlock(&g_log->mutex);
    mutexUnlock(&g_log_lifetime);
    return static_cast<ssize_t>(size);
}

const devoptab_t g_log_devoptab = {
    .name = "log",
    .write_r = WriteLog,
};

void OpenLogFile() {
    g_log->file = std::fopen(kLogFileName, "w");
    if (g_log->file == nullptr) {
        return;
    }
    constexpr int kLowestPriority = 0x3F;
    if (R_FAILED(threadCreate(&g_log->writer, WriteFile, g_log, nullptr, 0x4000, kLowestPriority, -2))) {
        std::fclose(g_log->file);
        g_log->file = nullptr;
        return;
    }
    g_log->writer_running = R_SUCCEEDED(threadStart(&g_log->writer));
    if (!g_log->writer_running) {
        threadClose(&g_log->writer);
        std::fclose(g_log->file);
        g_log->file = nullptr;
    }
}

void CloseLog();

void OpenLog() {
    mutexLock(&g_log_lifetime);
    if (g_log != nullptr) {
        mutexUnlock(&g_log_lifetime);
        return;
    }
    const bool close_registered = std::atexit(CloseLog) == 0;
    g_log = new (std::nothrow) LogSink;
    if (g_log == nullptr) {
        mutexUnlock(&g_log_lifetime);
        return;
    }
    mutexInit(&g_log->mutex);
    condvarInit(&g_log->wake);
    OpenLogManager();
    if (envIsNso()) {
        static_cast<void>(std::remove(kLogFileName));
    } else if (close_registered) {
        OpenLogFile();
    }
    mutexUnlock(&g_log_lifetime);
    devoptab_list[STD_OUT] = &g_log_devoptab;
    devoptab_list[STD_ERR] = &g_log_devoptab;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (!close_registered) {
        std::fprintf(stderr, "switch: file log cleanup registration failed; using lm only\n");
    }
}

void CloseLog() {
    mutexLock(&g_log_lifetime);
    LogSink *log = g_log;
    if (log == nullptr) {
        mutexUnlock(&g_log_lifetime);
        return;
    }
    mutexLock(&log->mutex);
    if (log->lm) {
        SendLine();
        serviceClose(&log->logger);
        log->lm = false;
    }
    const bool writer = log->writer_running;
    log->writer_running = false;
    log->stopping = true;
    condvarWakeAll(&log->wake);
    g_log = nullptr;
    mutexUnlock(&log->mutex);
    mutexUnlock(&g_log_lifetime);
    if (writer) {
        threadWaitForExit(&log->writer);
        threadClose(&log->writer);
        std::fclose(log->file);
        log->file = nullptr;
    }
    delete log;
}
} // namespace

void SwitchDisplaySize(int &width, int &height) {
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    width = handheld ? 1280 : 1920;
    height = handheld ? 720 : 1080;
}

void SwitchLogClocks() {
    const bool               handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    const ApmPerformanceMode mode = appletGetPerformanceMode();
    std::uint32_t            configuration = 0;
    if (R_SUCCEEDED(apmInitialize())) {
        if (R_FAILED(apmGetPerformanceConfiguration(mode, &configuration))) {
            configuration = 0;
        }
        apmExit();
    }
    std::fprintf(stderr, "switch: operation %s, performance mode %d, configuration 0x%08x\n",
                 handheld ? "handheld" : "console", static_cast<int>(mode), static_cast<unsigned>(configuration));
}

void SwitchShowMissingData() {
    consoleInit(nullptr);
    constexpr std::string_view message =
        KZ_SWITCH_DATA_INSTRUCTIONS "\n"
                                    "Press + to exit.\n";
    // stdout may already point to the log file or nxlink socket.
    devoptab_list[STD_OUT]->write_r(_REENT, nullptr, message.data(), message.size());
    consoleUpdate(nullptr);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
            break;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

extern "C" void userAppInit() {
    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) {
        g_sockets = true;
        g_socket = nxlinkStdio();
        if (g_socket >= 0) {
            return;
        }
    }
    OpenLog();
}

extern "C" void userAppExit() {
    SwitchRestoreMemoryGuards();
    if (g_socket >= 0) {
        close(g_socket);
        g_socket = -1;
    }
    if (g_sockets) {
        socketExit();
        g_sockets = false;
    }
    CloseLog();
}
