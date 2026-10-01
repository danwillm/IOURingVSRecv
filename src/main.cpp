#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <liburing.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kMagic = 0x55445042454E4348ULL; // "UDPBENCH"
constexpr std::uint32_t kVersion = 2;
constexpr std::uint16_t kDefaultDiscoveryPort = 39000;
constexpr std::uint16_t kDefaultBenchmarkPort = 39001;
constexpr std::size_t kDefaultPacketSize = 1400;
constexpr unsigned kDefaultQueueDepth = 256;
constexpr unsigned kDefaultSendBatch = 64;
constexpr int kDiscoveryWindowMs = 1200;

std::atomic_bool g_running{true};

void SignalHandler(int)
{
    g_running.store(false, std::memory_order_relaxed);
}

std::string ErrnoString(int error)
{
    return std::string(std::strerror(error));
}

[[noreturn]] void ThrowErrno(const std::string &what)
{
    throw std::runtime_error(what + ": " + ErrnoString(errno));
}

std::uint64_t HostToBe64(std::uint64_t value)
{
    return htobe64(value);
}

std::uint64_t Be64ToHost(std::uint64_t value)
{
    return be64toh(value);
}

enum class PacketType : std::uint32_t
{
    DiscoverRequest = 1,
    DiscoverReply = 2,
    Start = 3,
    Ready = 4,
    Data = 5,
    Stop = 6,
    Result = 7,
};

#pragma pack(push, 1)
struct PacketHeader
{
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t type;
    std::uint64_t session;
    std::uint64_t sequence;
};

struct DiscoveryPacket
{
    PacketHeader header;
    std::uint16_t benchmarkPort;
    std::uint16_t reserved;
    char hostname[64];
};

struct StartPacket
{
    PacketHeader header;
    std::uint32_t packetSize;
    std::uint32_t reserved;
};

struct ReadyPacket
{
    PacketHeader header;
    std::uint32_t receiveInflight;
    std::uint32_t reserved;
};

struct ResultPacket
{
    PacketHeader header;
    std::uint64_t packetsReceived;
    std::uint64_t highestSequencePlusOne;
    std::uint64_t bytesReceived;
    std::uint64_t elapsedNs;
    std::uint64_t userCpuNs;
    std::uint64_t systemCpuNs;
    std::uint64_t voluntaryContextSwitches;
    std::uint64_t involuntaryContextSwitches;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 32);

PacketHeader MakeHeader(PacketType type, std::uint64_t session, std::uint64_t sequence = 0)
{
    PacketHeader header{};
    header.magic = HostToBe64(kMagic);
    header.version = htonl(kVersion);
    header.type = htonl(static_cast<std::uint32_t>(type));
    header.session = HostToBe64(session);
    header.sequence = HostToBe64(sequence);
    return header;
}

bool IsValidHeader(const PacketHeader &header)
{
    return Be64ToHost(header.magic) == kMagic && ntohl(header.version) == kVersion;
}

PacketType GetPacketType(const PacketHeader &header)
{
    return static_cast<PacketType>(ntohl(header.type));
}

std::uint64_t GetSession(const PacketHeader &header)
{
    return Be64ToHost(header.session);
}

std::uint64_t GetSequence(const PacketHeader &header)
{
    return Be64ToHost(header.sequence);
}

std::string AddressToString(const sockaddr_in &address)
{
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr)
        return "<invalid>";

    std::ostringstream out;
    out << text << ':' << ntohs(address.sin_port);
    return out.str();
}

sockaddr_in ParseIPv4Target(const std::string &host, std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("target must currently be an IPv4 address: " + host);

    return address;
}

int CreateUdpSocket()
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        ThrowErrno("socket(AF_INET, SOCK_DGRAM)");
    return fd;
}

void SetSocketBuffer(int fd, int option, int requested)
{
    if (setsockopt(fd, SOL_SOCKET, option, &requested, sizeof(requested)) < 0)
        ThrowErrno("setsockopt socket buffer");
}

int GetSocketBuffer(int fd, int option)
{
    int value = 0;
    socklen_t length = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, option, &value, &length) < 0)
        ThrowErrno("getsockopt socket buffer");
    return value;
}

std::uint64_t TimevalToNs(const timeval &value)
{
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_usec) * 1'000ULL;
}

struct ProcessUsage
{
    std::uint64_t userNs = 0;
    std::uint64_t systemNs = 0;
    std::uint64_t voluntaryContextSwitches = 0;
    std::uint64_t involuntaryContextSwitches = 0;
};

ProcessUsage GetProcessUsage()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        ThrowErrno("getrusage(RUSAGE_SELF)");

    ProcessUsage result{};
    result.userNs = TimevalToNs(usage.ru_utime);
    result.systemNs = TimevalToNs(usage.ru_stime);
    result.voluntaryContextSwitches = static_cast<std::uint64_t>(std::max<long>(0, usage.ru_nvcsw));
    result.involuntaryContextSwitches = static_cast<std::uint64_t>(std::max<long>(0, usage.ru_nivcsw));
    return result;
}

std::uint64_t SafeDelta(std::uint64_t end, std::uint64_t start)
{
    return end >= start ? end - start : 0;
}

void SetCpuAffinity(int cpu)
{
    if (cpu < 0 || cpu >= CPU_SETSIZE)
        throw std::runtime_error("CPU index is outside CPU_SETSIZE");

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        ThrowErrno("sched_setaffinity");
}

struct Peer
{
    sockaddr_in address{};
    std::string hostname;
};

std::vector<sockaddr_in> GetBroadcastAddresses(std::uint16_t port)
{
    std::vector<sockaddr_in> addresses;
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
        ThrowErrno("getifaddrs");

    for (const ifaddrs *ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || ifa->ifa_broadaddr == nullptr)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_BROADCAST) == 0)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        sockaddr_in broadcast = *reinterpret_cast<const sockaddr_in *>(ifa->ifa_broadaddr);
        broadcast.sin_port = htons(port);
        addresses.push_back(broadcast);
    }

    freeifaddrs(interfaces);

    sockaddr_in limited{};
    limited.sin_family = AF_INET;
    limited.sin_port = htons(port);
    limited.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    addresses.push_back(limited);

    std::sort(addresses.begin(), addresses.end(), [](const sockaddr_in &a, const sockaddr_in &b) {
        return a.sin_addr.s_addr < b.sin_addr.s_addr;
    });
    addresses.erase(std::unique(addresses.begin(), addresses.end(), [](const sockaddr_in &a, const sockaddr_in &b) {
                        return a.sin_addr.s_addr == b.sin_addr.s_addr;
                    }),
                    addresses.end());
    return addresses;
}

std::vector<Peer> DiscoverPeers(std::uint16_t discoveryPort, int windowMs)
{
    const int fd = CreateUdpSocket();

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0)
    {
        close(fd);
        ThrowErrno("setsockopt(SO_BROADCAST)");
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) < 0)
    {
        close(fd);
        ThrowErrno("bind discovery client");
    }

    DiscoveryPacket request{};
    request.header = MakeHeader(PacketType::DiscoverRequest, 0);

    for (const sockaddr_in &broadcast : GetBroadcastAddresses(discoveryPort))
    {
        (void)sendto(fd,
                     &request,
                     sizeof(request),
                     0,
                     reinterpret_cast<const sockaddr *>(&broadcast),
                     sizeof(broadcast));
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(windowMs);
    std::vector<Peer> peers;
    std::set<std::string> seen;

    while (Clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd pollFd{};
        pollFd.fd = fd;
        pollFd.events = POLLIN;

        const int pollResult = poll(&pollFd, 1, static_cast<int>(std::max<std::int64_t>(1, remaining)));
        if (pollResult < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            ThrowErrno("poll discovery replies");
        }
        if (pollResult == 0)
            break;

        DiscoveryPacket reply{};
        sockaddr_in from{};
        socklen_t fromLength = sizeof(from);
        const ssize_t bytes = recvfrom(fd,
                                       &reply,
                                       sizeof(reply),
                                       0,
                                       reinterpret_cast<sockaddr *>(&from),
                                       &fromLength);
        if (bytes < static_cast<ssize_t>(sizeof(reply.header)) || !IsValidHeader(reply.header) ||
            GetPacketType(reply.header) != PacketType::DiscoverReply)
        {
            continue;
        }

        from.sin_port = reply.benchmarkPort;
        const std::string key = AddressToString(from);
        if (!seen.insert(key).second)
            continue;

        reply.hostname[sizeof(reply.hostname) - 1] = '\0';
        peers.push_back(Peer{from, reply.hostname});
    }

    close(fd);
    return peers;
}

class DiscoveryResponder
{
public:
    DiscoveryResponder(std::uint16_t discoveryPort, std::uint16_t benchmarkPort)
        : m_discoveryPort(discoveryPort), m_benchmarkPort(benchmarkPort)
    {
    }

    void Start()
    {
        m_running.store(true, std::memory_order_relaxed);
        m_thread = std::thread([this] { Run(); });
    }

    void Stop()
    {
        m_running.store(false, std::memory_order_relaxed);
        if (m_thread.joinable())
            m_thread.join();
    }

    ~DiscoveryResponder()
    {
        Stop();
    }

private:
    void Run()
    {
        try
        {
            const int fd = CreateUdpSocket();
            int one = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 50'000;
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(m_discoveryPort);
            if (bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) < 0)
            {
                const int error = errno;
                close(fd);
                throw std::runtime_error("bind discovery responder: " + ErrnoString(error));
            }

            while (m_running.load(std::memory_order_relaxed) && g_running.load(std::memory_order_relaxed))
            {
                DiscoveryPacket request{};
                sockaddr_in from{};
                socklen_t fromLength = sizeof(from);
                const ssize_t bytes = recvfrom(fd,
                                               &request,
                                               sizeof(request),
                                               0,
                                               reinterpret_cast<sockaddr *>(&from),
                                               &fromLength);
                if (bytes < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        continue;
                    break;
                }

                if (bytes < static_cast<ssize_t>(sizeof(request.header)) || !IsValidHeader(request.header) ||
                    GetPacketType(request.header) != PacketType::DiscoverRequest)
                {
                    continue;
                }

                DiscoveryPacket reply{};
                reply.header = MakeHeader(PacketType::DiscoverReply, 0);
                reply.benchmarkPort = htons(m_benchmarkPort);
                if (gethostname(reply.hostname, sizeof(reply.hostname) - 1) != 0)
                    std::strncpy(reply.hostname, "unknown", sizeof(reply.hostname) - 1);

                (void)sendto(fd,
                             &reply,
                             sizeof(reply),
                             0,
                             reinterpret_cast<const sockaddr *>(&from),
                             fromLength);
            }

            close(fd);
        }
        catch (const std::exception &e)
        {
            std::cerr << "discovery responder error: " << e.what() << '\n';
        }
    }

    std::uint16_t m_discoveryPort;
    std::uint16_t m_benchmarkPort;
    std::atomic_bool m_running{false};
    std::thread m_thread;
};

struct Stats
{
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t highestSequencePlusOne = 0;
    std::uint64_t outOfOrder = 0;
    Clock::time_point firstPacket{};
    Clock::time_point lastPacket{};
    Clock::time_point measurementEnd{};
    ProcessUsage usageStart{};
    ProcessUsage usageEnd{};
    bool started = false;
    bool finalized = false;
};

void ProcessDatagram(const std::byte *data,
                     std::size_t size,
                     std::uint64_t session,
                     Stats &stats,
                     bool &stop)
{
    if (size < sizeof(PacketHeader))
        return;

    PacketHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (!IsValidHeader(header) || GetSession(header) != session)
        return;

    const PacketType type = GetPacketType(header);
    if (type == PacketType::Data)
    {
        const auto now = Clock::now();
        if (!stats.started)
        {
            // Snapshot immediately before accounting the first data datagram. Discovery and
            // handshake work therefore do not contaminate the benchmark CPU interval.
            stats.usageStart = GetProcessUsage();
            stats.started = true;
            stats.firstPacket = now;
        }

        const std::uint64_t sequence = GetSequence(header);
        if (sequence + 1 < stats.highestSequencePlusOne)
            ++stats.outOfOrder;
        stats.highestSequencePlusOne = std::max(stats.highestSequencePlusOne, sequence + 1);
        ++stats.packets;
        stats.bytes += size;
        stats.lastPacket = now;
    }
    else if (type == PacketType::Stop)
    {
        stop = true;
    }
}

void FinalizeStats(Stats &stats)
{
    if (!stats.started || stats.finalized)
        return;

    stats.usageEnd = GetProcessUsage();
    stats.measurementEnd = Clock::now();
    stats.finalized = true;
}

void PrintInterval(const Stats &stats,
                   std::uint64_t previousBytes,
                   std::uint64_t previousPackets,
                   Clock::time_point previousTime,
                   Clock::time_point now)
{
    const double seconds = std::chrono::duration<double>(now - previousTime).count();
    if (seconds <= 0.0)
        return;

    const std::uint64_t deltaBytes = stats.bytes - previousBytes;
    const std::uint64_t deltaPackets = stats.packets - previousPackets;
    const double mbps = static_cast<double>(deltaBytes) * 8.0 / seconds / 1'000'000.0;
    const double pps = static_cast<double>(deltaPackets) / seconds;

    std::cout << "  " << std::fixed << std::setprecision(2) << mbps << " Mbit/s, "
              << std::setprecision(0) << pps << " packets/s\n";
}

void PrintFinalStats(const Stats &stats, std::string_view backend)
{
    if (!stats.started || stats.packets == 0 || !stats.finalized)
    {
        std::cout << "No data packets received.\n";
        return;
    }

    const double seconds = std::chrono::duration<double>(stats.measurementEnd - stats.firstPacket).count();
    const std::uint64_t expected = stats.highestSequencePlusOne;
    const std::uint64_t missing = expected > stats.packets ? expected - stats.packets : 0;
    const double lossPercent = expected > 0 ? (static_cast<double>(missing) * 100.0 / static_cast<double>(expected)) : 0.0;
    const double mbps = seconds > 0.0 ? static_cast<double>(stats.bytes) * 8.0 / seconds / 1'000'000.0 : 0.0;
    const double pps = seconds > 0.0 ? static_cast<double>(stats.packets) / seconds : 0.0;

    const std::uint64_t userNs = SafeDelta(stats.usageEnd.userNs, stats.usageStart.userNs);
    const std::uint64_t systemNs = SafeDelta(stats.usageEnd.systemNs, stats.usageStart.systemNs);
    const std::uint64_t totalNs = userNs + systemNs;
    const std::uint64_t voluntarySwitches = SafeDelta(stats.usageEnd.voluntaryContextSwitches,
                                                       stats.usageStart.voluntaryContextSwitches);
    const std::uint64_t involuntarySwitches = SafeDelta(stats.usageEnd.involuntaryContextSwitches,
                                                         stats.usageStart.involuntaryContextSwitches);

    const double userPercent = seconds > 0.0 ? (static_cast<double>(userNs) / 1e9) / seconds * 100.0 : 0.0;
    const double systemPercent = seconds > 0.0 ? (static_cast<double>(systemNs) / 1e9) / seconds * 100.0 : 0.0;
    const double totalPercent = seconds > 0.0 ? (static_cast<double>(totalNs) / 1e9) / seconds * 100.0 : 0.0;
    const double userNsPerPacket = stats.packets > 0 ? static_cast<double>(userNs) / static_cast<double>(stats.packets) : 0.0;
    const double systemNsPerPacket = stats.packets > 0 ? static_cast<double>(systemNs) / static_cast<double>(stats.packets) : 0.0;
    const double totalNsPerPacket = stats.packets > 0 ? static_cast<double>(totalNs) / static_cast<double>(stats.packets) : 0.0;

    std::cout << "\nFinal receiver result (" << backend << ")\n"
              << "  elapsed:          " << std::fixed << std::setprecision(3) << seconds << " s\n"
              << "  received:         " << stats.packets << " packets\n"
              << "  bytes:            " << stats.bytes << "\n"
              << "  throughput:       " << std::setprecision(2) << mbps << " Mbit/s\n"
              << "  packet rate:      " << std::setprecision(0) << pps << " packets/s\n"
              << "  missing(est):     " << missing << " / " << expected << " ("
              << std::setprecision(4) << lossPercent << "%)\n"
              << "  out-of-order:     " << stats.outOfOrder << "\n"
              << "  user CPU:         " << std::setprecision(2) << userPercent << "% of one CPU\n"
              << "  system CPU:       " << systemPercent << "% of one CPU\n"
              << "  total process CPU: " << totalPercent << "% of one CPU\n"
              << "  user ns/packet:   " << std::setprecision(1) << userNsPerPacket << " ns\n"
              << "  system ns/packet: " << systemNsPerPacket << " ns\n"
              << "  total ns/packet:  " << totalNsPerPacket << " ns\n"
              << "  voluntary csw:    " << voluntarySwitches << "\n"
              << "  involuntary csw:  " << involuntarySwitches << "\n\n";
}

Stats ReceiveWithRecv(int fd, std::size_t packetSize, std::uint64_t session, bool quiet)
{
    std::vector<std::byte> buffer(packetSize);
    Stats stats{};
    bool stop = false;

    auto lastReport = Clock::now();
    std::uint64_t lastBytes = 0;
    std::uint64_t lastPackets = 0;

    while (!stop && g_running.load(std::memory_order_relaxed))
    {
        const ssize_t bytes = recv(fd, buffer.data(), buffer.size(), 0);
        if (bytes < 0)
        {
            if (errno == EINTR)
                continue;
            ThrowErrno("recv");
        }

        ProcessDatagram(buffer.data(), static_cast<std::size_t>(bytes), session, stats, stop);

        const auto now = Clock::now();
        if (!quiet && stats.started && now - lastReport >= std::chrono::seconds(1))
        {
            PrintInterval(stats, lastBytes, lastPackets, lastReport, now);
            lastReport = now;
            lastBytes = stats.bytes;
            lastPackets = stats.packets;
        }
    }

    FinalizeStats(stats);
    return stats;
}

Stats ReceiveWithIoUringOneShot(int fd, std::size_t packetSize, std::uint64_t session, unsigned queueDepth, bool quiet)
{
    if (queueDepth == 0 || queueDepth > 4096)
        throw std::runtime_error("queue depth must be between 1 and 4096");

    io_uring ring{};
    const int initResult = io_uring_queue_init(queueDepth, &ring, 0);
    if (initResult < 0)
        throw std::runtime_error("io_uring_queue_init: " + ErrnoString(-initResult));

    struct RingGuard
    {
        io_uring *ring;
        ~RingGuard() { io_uring_queue_exit(ring); }
    } guard{&ring};

    std::vector<std::vector<std::byte>> buffers(queueDepth, std::vector<std::byte>(packetSize));

    auto queueReceive = [&](unsigned index) {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (sqe == nullptr)
            throw std::runtime_error("io_uring submission queue unexpectedly full");
        io_uring_prep_recv(sqe, fd, buffers[index].data(), buffers[index].size(), 0);
        io_uring_sqe_set_data64(sqe, index);
    };

    for (unsigned i = 0; i < queueDepth; ++i)
        queueReceive(i);

    const int submitResult = io_uring_submit(&ring);
    if (submitResult < 0)
        throw std::runtime_error("io_uring_submit: " + ErrnoString(-submitResult));

    Stats stats{};
    bool stopSeen = false;
    std::uint64_t outstanding = queueDepth;
    std::vector<io_uring_cqe *> completions(queueDepth);

    auto lastReport = Clock::now();
    std::uint64_t lastBytes = 0;
    std::uint64_t lastPackets = 0;

    while (g_running.load(std::memory_order_relaxed) && (!stopSeen || outstanding != 0))
    {
        io_uring_cqe *first = nullptr;
        const int waitResult = io_uring_wait_cqe(&ring, &first);
        if (waitResult < 0)
        {
            if (waitResult == -EINTR)
                continue;
            throw std::runtime_error("io_uring_wait_cqe: " + ErrnoString(-waitResult));
        }

        const unsigned count = io_uring_peek_batch_cqe(&ring, completions.data(), queueDepth);
        if (count == 0)
            continue;

        unsigned queuedResubmits = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            io_uring_cqe *cqe = completions[i];
            const std::uint64_t userData = io_uring_cqe_get_data64(cqe);
            if (userData >= queueDepth)
                throw std::runtime_error("io_uring completion contained invalid buffer index");
            if (outstanding == 0)
                throw std::runtime_error("io_uring outstanding receive accounting underflow");
            --outstanding;

            bool thisWasStop = false;
            if (cqe->res >= 0)
            {
                ProcessDatagram(buffers[static_cast<std::size_t>(userData)].data(),
                                static_cast<std::size_t>(cqe->res),
                                session,
                                stats,
                                thisWasStop);
                stopSeen = stopSeen || thisWasStop;
            }
            else if (cqe->res != -ECANCELED)
            {
                std::cerr << "io_uring recv completion error: " << ErrnoString(-cqe->res) << '\n';
            }

            io_uring_cqe_seen(&ring, cqe);

            // Before STOP has been consumed, keep queueDepth receives in flight. Once a STOP
            // completion is observed, do not add any more receives. We continue harvesting all
            // receives that were already submitted so a later CQE for an earlier data datagram
            // is still included in the result.
            if (!stopSeen && g_running.load(std::memory_order_relaxed))
            {
                queueReceive(static_cast<unsigned>(userData));
                ++outstanding;
                ++queuedResubmits;
            }
        }

        if (queuedResubmits != 0)
        {
            const int resubmit = io_uring_submit(&ring);
            if (resubmit < 0)
                throw std::runtime_error("io_uring_submit: " + ErrnoString(-resubmit));
        }

        const auto now = Clock::now();
        if (!quiet && stats.started && now - lastReport >= std::chrono::seconds(1))
        {
            PrintInterval(stats, lastBytes, lastPackets, lastReport, now);
            lastReport = now;
            lastBytes = stats.bytes;
            lastPackets = stats.packets;
        }
    }

    FinalizeStats(stats);
    return stats;
}


Stats ReceiveWithIoUringMultishot(int fd, std::size_t packetSize, std::uint64_t session, unsigned bufferCount, bool quiet)
{
    if (bufferCount < 2 || bufferCount > 32768 || (bufferCount & (bufferCount - 1)) != 0)
        throw std::runtime_error("io_uring multishot buffer count must be a power of two between 2 and 32768");

    // Multishot receive only needs a single receive SQE in flight. Keep the SQ reasonably
    // sized, while letting --queue-depth control the much more important provided-buffer pool.
    const unsigned ringEntries = std::clamp(bufferCount, 32U, 4096U);

    io_uring ring{};
    const int initResult = io_uring_queue_init(ringEntries, &ring, 0);
    if (initResult < 0)
        throw std::runtime_error("io_uring_queue_init: " + ErrnoString(-initResult));

    struct RingGuard
    {
        io_uring *ring;
        ~RingGuard() { io_uring_queue_exit(ring); }
    } ringGuard{&ring};

    constexpr int kBufferGroup = 1;
    int bufferRingError = 0;
    io_uring_buf_ring *bufferRing = io_uring_setup_buf_ring(&ring,
                                                            bufferCount,
                                                            kBufferGroup,
                                                            0,
                                                            &bufferRingError);
    if (bufferRing == nullptr)
    {
        throw std::runtime_error("io_uring_setup_buf_ring: " +
                                 ErrnoString(bufferRingError < 0 ? -bufferRingError : EINVAL));
    }

    struct BufferRingGuard
    {
        io_uring *ring;
        io_uring_buf_ring *bufferRing;
        unsigned count;
        int group;
        ~BufferRingGuard()
        {
            if (bufferRing != nullptr)
                (void)io_uring_free_buf_ring(ring, bufferRing, count, group);
        }
    } bufferRingGuard{&ring, bufferRing, bufferCount, kBufferGroup};

    std::vector<std::vector<std::byte>> buffers(bufferCount, std::vector<std::byte>(packetSize));
    const int bufferMask = io_uring_buf_ring_mask(bufferCount);

    for (unsigned i = 0; i < bufferCount; ++i)
    {
        io_uring_buf_ring_add(bufferRing,
                              buffers[i].data(),
                              static_cast<unsigned>(buffers[i].size()),
                              static_cast<unsigned short>(i),
                              bufferMask,
                              static_cast<int>(i));
    }
    io_uring_buf_ring_advance(bufferRing, static_cast<int>(bufferCount));

    constexpr std::uint64_t kRecvUserData = 0x525843564D554C54ULL; // "RXCVMULT"

    auto submitMultishotReceive = [&]() {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (sqe == nullptr)
            throw std::runtime_error("io_uring submission queue unexpectedly full");

        io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = kBufferGroup;
        io_uring_sqe_set_data64(sqe, kRecvUserData);

        const int submitResult = io_uring_submit(&ring);
        if (submitResult < 0)
            throw std::runtime_error("io_uring_submit multishot recv: " + ErrnoString(-submitResult));
    };

    submitMultishotReceive();

    Stats stats{};
    bool stopSeen = false;
    std::vector<io_uring_cqe *> completions(std::max(64U, ringEntries * 2U));

    auto lastReport = Clock::now();
    std::uint64_t lastBytes = 0;
    std::uint64_t lastPackets = 0;

    while (g_running.load(std::memory_order_relaxed) && !stopSeen)
    {
        io_uring_cqe *first = nullptr;
        const int waitResult = io_uring_wait_cqe(&ring, &first);
        if (waitResult < 0)
        {
            if (waitResult == -EINTR)
                continue;
            throw std::runtime_error("io_uring_wait_cqe: " + ErrnoString(-waitResult));
        }

        const unsigned count = io_uring_peek_batch_cqe(&ring,
                                                        completions.data(),
                                                        static_cast<unsigned>(completions.size()));
        if (count == 0)
            continue;

        unsigned recycledBuffers = 0;
        bool needRearm = false;

        for (unsigned i = 0; i < count; ++i)
        {
            io_uring_cqe *cqe = completions[i];
            if (io_uring_cqe_get_data64(cqe) != kRecvUserData)
                throw std::runtime_error("io_uring completion contained unexpected user_data");

            const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;

            if (cqe->res >= 0)
            {
                if ((cqe->flags & IORING_CQE_F_BUFFER) == 0)
                    throw std::runtime_error("multishot recv completion did not contain a provided buffer");

                const unsigned bufferId = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                if (bufferId >= bufferCount)
                    throw std::runtime_error("multishot recv completion contained invalid buffer id");

                bool thisWasStop = false;
                ProcessDatagram(buffers[bufferId].data(),
                                static_cast<std::size_t>(cqe->res),
                                session,
                                stats,
                                thisWasStop);
                stopSeen = stopSeen || thisWasStop;

                // Return each consumed buffer to the provided-buffer ring. Buffer additions are
                // batched and made visible with a single tail update after this CQ batch.
                io_uring_buf_ring_add(bufferRing,
                                      buffers[bufferId].data(),
                                      static_cast<unsigned>(buffers[bufferId].size()),
                                      static_cast<unsigned short>(bufferId),
                                      bufferMask,
                                      static_cast<int>(recycledBuffers));
                ++recycledBuffers;
            }
            else if (cqe->res == -ENOBUFS)
            {
                // The kernel consumed the available provided buffers before userspace recycled
                // them. The multishot request ends in this case; recycle what we have and re-arm.
            }
            else if (cqe->res != -ECANCELED)
            {
                throw std::runtime_error("io_uring multishot recv completion: " + ErrnoString(-cqe->res));
            }

            if (!more)
                needRearm = true;
        }

        if (recycledBuffers != 0)
            io_uring_buf_ring_advance(bufferRing, static_cast<int>(recycledBuffers));

        // Advance the CQ once per batch instead of once per packet.
        io_uring_cq_advance(&ring, count);

        if (!stopSeen && g_running.load(std::memory_order_relaxed) && needRearm)
            submitMultishotReceive();

        const auto now = Clock::now();
        if (!quiet && stats.started && now - lastReport >= std::chrono::seconds(1))
        {
            PrintInterval(stats, lastBytes, lastPackets, lastReport, now);
            lastReport = now;
            lastBytes = stats.bytes;
            lastPackets = stats.packets;
        }
    }

    FinalizeStats(stats);
    return stats;
}

struct ReceiverOptions
{
    std::string backend = "recv";
    std::uint16_t discoveryPort = kDefaultDiscoveryPort;
    std::uint16_t benchmarkPort = kDefaultBenchmarkPort;
    unsigned queueDepth = kDefaultQueueDepth;
    int socketBuffer = 16 * 1024 * 1024;
    std::optional<int> cpu;
    bool once = false;
    bool quiet = false;
};

int CreateBenchmarkReceiverSocket(const ReceiverOptions &options)
{
    const int fd = CreateUdpSocket();
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    SetSocketBuffer(fd, SO_RCVBUF, options.socketBuffer);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(options.benchmarkPort);
    if (bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) < 0)
    {
        const int error = errno;
        close(fd);
        throw std::runtime_error("bind benchmark receiver: " + ErrnoString(error));
    }
    return fd;
}

void SendResult(int fd, std::uint64_t session, const Stats &stats)
{
    ResultPacket result{};
    result.header = MakeHeader(PacketType::Result, session);
    result.packetsReceived = HostToBe64(stats.packets);
    result.highestSequencePlusOne = HostToBe64(stats.highestSequencePlusOne);
    result.bytesReceived = HostToBe64(stats.bytes);

    const auto elapsedNs = stats.started && stats.finalized
                               ? std::chrono::duration_cast<std::chrono::nanoseconds>(stats.measurementEnd - stats.firstPacket).count()
                               : 0;
    const std::uint64_t userNs = SafeDelta(stats.usageEnd.userNs, stats.usageStart.userNs);
    const std::uint64_t systemNs = SafeDelta(stats.usageEnd.systemNs, stats.usageStart.systemNs);
    const std::uint64_t voluntarySwitches = SafeDelta(stats.usageEnd.voluntaryContextSwitches,
                                                       stats.usageStart.voluntaryContextSwitches);
    const std::uint64_t involuntarySwitches = SafeDelta(stats.usageEnd.involuntaryContextSwitches,
                                                         stats.usageStart.involuntaryContextSwitches);

    result.elapsedNs = HostToBe64(static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsedNs)));
    result.userCpuNs = HostToBe64(userNs);
    result.systemCpuNs = HostToBe64(systemNs);
    result.voluntaryContextSwitches = HostToBe64(voluntarySwitches);
    result.involuntaryContextSwitches = HostToBe64(involuntarySwitches);

    for (int i = 0; i < 8; ++i)
    {
        (void)send(fd, &result, sizeof(result), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int RunReceiver(const ReceiverOptions &options)
{
    if (options.backend != "recv" && options.backend != "uring" && options.backend != "uring-oneshot")
        throw std::runtime_error("receiver backend must be 'recv', 'uring', or 'uring-oneshot'");

    if (options.cpu.has_value())
        SetCpuAffinity(*options.cpu);

    std::cout << "UDP benchmark receiver\n"
              << "  backend:        " << options.backend << '\n'
              << "  benchmark port: " << options.benchmarkPort << '\n'
              << "  discovery port: " << options.discoveryPort << '\n';
    if (options.backend == "uring")
        std::cout << "  buffer count:   " << options.queueDepth << '\n';
    else if (options.backend == "uring-oneshot")
        std::cout << "  queue depth:    " << options.queueDepth << '\n';
    if (options.cpu.has_value())
        std::cout << "  CPU affinity:   " << *options.cpu << '\n';
    if (options.once)
        std::cout << "  mode:           one benchmark then exit\n";
    if (options.quiet)
        std::cout << "  progress:       disabled\n";
    std::cout << "Press Ctrl-C to stop.\n\n";

    bool completedOne = false;
    while (g_running.load(std::memory_order_relaxed) && !completedOne)
    {
        DiscoveryResponder discovery(options.discoveryPort, options.benchmarkPort);
        discovery.Start();

        const int fd = CreateBenchmarkReceiverSocket(options);
        std::cout << "Waiting for sender... (SO_RCVBUF=" << GetSocketBuffer(fd, SO_RCVBUF) << ")\n";

        StartPacket start{};
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        std::uint64_t session = 0;
        std::size_t packetSize = 0;

        while (g_running.load(std::memory_order_relaxed))
        {
            const ssize_t bytes = recvfrom(fd,
                                           &start,
                                           sizeof(start),
                                           0,
                                           reinterpret_cast<sockaddr *>(&sender),
                                           &senderLength);
            if (bytes < 0)
            {
                if (errno == EINTR)
                    continue;
                const int error = errno;
                close(fd);
                throw std::runtime_error("recvfrom start packet: " + ErrnoString(error));
            }

            if (bytes < static_cast<ssize_t>(sizeof(StartPacket)) || !IsValidHeader(start.header) ||
                GetPacketType(start.header) != PacketType::Start)
            {
                continue;
            }

            session = GetSession(start.header);
            packetSize = ntohl(start.packetSize);
            if (packetSize < sizeof(PacketHeader) || packetSize > 65507)
            {
                std::cerr << "Ignoring invalid packet size " << packetSize << '\n';
                continue;
            }
            break;
        }

        if (!g_running.load(std::memory_order_relaxed))
        {
            discovery.Stop();
            close(fd);
            break;
        }

        if (connect(fd, reinterpret_cast<const sockaddr *>(&sender), senderLength) < 0)
        {
            const int error = errno;
            discovery.Stop();
            close(fd);
            throw std::runtime_error("connect receiver UDP socket: " + ErrnoString(error));
        }

        // Stop the discovery thread before the measured traffic starts. getrusage(RUSAGE_SELF)
        // covers all process threads, so leaving discovery active would contaminate CPU results.
        discovery.Stop();

        ReadyPacket ready{};
        ready.header = MakeHeader(PacketType::Ready, session);
        ready.receiveInflight = htonl(options.backend == "recv" ? 1U : options.queueDepth);
        for (int i = 0; i < 3; ++i)
            (void)send(fd, &ready, sizeof(ready), 0);

        std::cout << "Sender " << AddressToString(sender) << ", packet size " << packetSize << " bytes\n";

        Stats stats{};
        if (options.backend == "recv")
            stats = ReceiveWithRecv(fd, packetSize, session, options.quiet);
        else if (options.backend == "uring")
            stats = ReceiveWithIoUringMultishot(fd, packetSize, session, options.queueDepth, options.quiet);
        else
            stats = ReceiveWithIoUringOneShot(fd, packetSize, session, options.queueDepth, options.quiet);

        PrintFinalStats(stats, options.backend);
        SendResult(fd, session, stats);
        close(fd);

        completedOne = options.once;
    }

    return 0;
}

struct SenderOptions
{
    std::optional<std::string> target;
    std::uint16_t discoveryPort = kDefaultDiscoveryPort;
    std::uint16_t benchmarkPort = kDefaultBenchmarkPort;
    std::size_t packetSize = kDefaultPacketSize;
    double durationSeconds = 10.0;
    double packetsPerSecond = 0.0; // 0 = unlimited
    unsigned sendBatch = kDefaultSendBatch;
    int socketBuffer = 16 * 1024 * 1024;
    std::optional<int> cpu;
};

std::uint64_t MakeSessionId()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return static_cast<std::uint64_t>(now) ^ (static_cast<std::uint64_t>(getpid()) << 32U);
}

Peer ResolveTarget(const SenderOptions &options)
{
    if (options.target.has_value())
        return Peer{ParseIPv4Target(*options.target, options.benchmarkPort), *options.target};

    std::cout << "Discovering receivers...\n";
    const std::vector<Peer> peers = DiscoverPeers(options.discoveryPort, kDiscoveryWindowMs);
    if (peers.empty())
        throw std::runtime_error("no benchmark receivers found by UDP broadcast; use --target <IPv4> if broadcast is filtered");

    std::cout << "Found " << peers.size() << " receiver(s):\n";
    for (const Peer &peer : peers)
        std::cout << "  " << std::left << std::setw(24) << peer.hostname << " " << AddressToString(peer.address) << '\n';

    if (peers.size() != 1)
        throw std::runtime_error("multiple receivers found; select one with --target <IPv4>");

    return peers.front();
}

std::optional<unsigned> WaitForReady(int fd, std::uint64_t session, const StartPacket &start)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        (void)send(fd, &start, sizeof(start), 0);

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int result = poll(&pfd, 1, 250);
        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            ThrowErrno("poll waiting for READY");
        }
        if (result == 0)
            continue;

        ReadyPacket reply{};
        const ssize_t bytes = recv(fd, &reply, sizeof(reply), 0);
        if (bytes >= static_cast<ssize_t>(sizeof(reply)) && IsValidHeader(reply.header) &&
            GetPacketType(reply.header) == PacketType::Ready && GetSession(reply.header) == session)
        {
            const unsigned inflight = ntohl(reply.receiveInflight);
            return std::max(1U, inflight);
        }
    }
    return std::nullopt;
}

void PrintRemoteResult(const ResultPacket &result)
{
    const std::uint64_t packets = Be64ToHost(result.packetsReceived);
    const std::uint64_t expected = Be64ToHost(result.highestSequencePlusOne);
    const std::uint64_t bytes = Be64ToHost(result.bytesReceived);
    const std::uint64_t elapsedNs = Be64ToHost(result.elapsedNs);
    const std::uint64_t userNs = Be64ToHost(result.userCpuNs);
    const std::uint64_t systemNs = Be64ToHost(result.systemCpuNs);
    const std::uint64_t voluntarySwitches = Be64ToHost(result.voluntaryContextSwitches);
    const std::uint64_t involuntarySwitches = Be64ToHost(result.involuntaryContextSwitches);
    const std::uint64_t totalNs = userNs + systemNs;
    const std::uint64_t missing = expected > packets ? expected - packets : 0;
    const double seconds = static_cast<double>(elapsedNs) / 1e9;
    const double mbps = seconds > 0.0 ? static_cast<double>(bytes) * 8.0 / seconds / 1'000'000.0 : 0.0;
    const double pps = seconds > 0.0 ? static_cast<double>(packets) / seconds : 0.0;
    const double userPercent = seconds > 0.0 ? (static_cast<double>(userNs) / 1e9) / seconds * 100.0 : 0.0;
    const double systemPercent = seconds > 0.0 ? (static_cast<double>(systemNs) / 1e9) / seconds * 100.0 : 0.0;
    const double totalPercent = seconds > 0.0 ? (static_cast<double>(totalNs) / 1e9) / seconds * 100.0 : 0.0;
    const double userNsPerPacket = packets > 0 ? static_cast<double>(userNs) / static_cast<double>(packets) : 0.0;
    const double systemNsPerPacket = packets > 0 ? static_cast<double>(systemNs) / static_cast<double>(packets) : 0.0;

    std::cout << "Remote receiver result\n"
              << "  throughput:       " << std::fixed << std::setprecision(2) << mbps << " Mbit/s\n"
              << "  packet rate:      " << std::setprecision(0) << pps << " packets/s\n"
              << "  received:         " << packets << " packets\n"
              << "  missing(est):     " << missing << "\n"
              << "  user CPU:         " << std::setprecision(2) << userPercent << "% of one CPU\n"
              << "  system CPU:       " << systemPercent << "% of one CPU\n"
              << "  total process CPU: " << totalPercent << "% of one CPU\n"
              << "  user ns/packet:   " << std::setprecision(1) << userNsPerPacket << " ns\n"
              << "  system ns/packet: " << systemNsPerPacket << " ns\n"
              << "  voluntary csw:    " << voluntarySwitches << "\n"
              << "  involuntary csw:  " << involuntarySwitches << "\n";
}

int RunSender(const SenderOptions &options)
{
    if (options.packetSize < sizeof(PacketHeader) || options.packetSize > 65507)
        throw std::runtime_error("packet size must be between 32 and 65507 bytes");
    if (options.durationSeconds <= 0.0)
        throw std::runtime_error("duration must be greater than zero");
    if (options.packetsPerSecond < 0.0 || !std::isfinite(options.packetsPerSecond))
        throw std::runtime_error("pps must be zero (unlimited) or a positive finite value");
    if (options.sendBatch == 0 || options.sendBatch > 1024)
        throw std::runtime_error("send batch must be between 1 and 1024");

    if (options.cpu.has_value())
        SetCpuAffinity(*options.cpu);

    const Peer peer = ResolveTarget(options);
    const int fd = CreateUdpSocket();
    SetSocketBuffer(fd, SO_SNDBUF, options.socketBuffer);

    if (connect(fd, reinterpret_cast<const sockaddr *>(&peer.address), sizeof(peer.address)) < 0)
    {
        const int error = errno;
        close(fd);
        throw std::runtime_error("connect sender UDP socket: " + ErrnoString(error));
    }

    const std::uint64_t session = MakeSessionId();
    StartPacket start{};
    start.header = MakeHeader(PacketType::Start, session);
    start.packetSize = htonl(static_cast<std::uint32_t>(options.packetSize));

    std::cout << "Target: " << peer.hostname << " (" << AddressToString(peer.address) << ")\n"
              << "Packet size: " << options.packetSize << " bytes\n"
              << "Duration: " << options.durationSeconds << " s\n"
              << "sendmmsg batch: " << options.sendBatch << "\n";
    if (options.packetsPerSecond > 0.0)
        std::cout << "Target rate: " << std::fixed << std::setprecision(0) << options.packetsPerSecond << " packets/s\n";
    else
        std::cout << "Target rate: unlimited\n";
    if (options.cpu.has_value())
        std::cout << "CPU affinity: " << *options.cpu << "\n";
    std::cout << "SO_SNDBUF: " << GetSocketBuffer(fd, SO_SNDBUF) << "\n";

    const std::optional<unsigned> receiverInflight = WaitForReady(fd, session, start);
    if (!receiverInflight.has_value())
    {
        close(fd);
        throw std::runtime_error("receiver did not acknowledge START");
    }

    std::vector<std::vector<std::byte>> buffers(options.sendBatch, std::vector<std::byte>(options.packetSize));
    std::vector<iovec> iovecs(options.sendBatch);
    std::vector<mmsghdr> messages(options.sendBatch);

    for (unsigned i = 0; i < options.sendBatch; ++i)
    {
        iovecs[i].iov_base = buffers[i].data();
        iovecs[i].iov_len = buffers[i].size();
        messages[i].msg_hdr.msg_iov = &iovecs[i];
        messages[i].msg_hdr.msg_iovlen = 1;
    }

    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.durationSeconds));
    const std::uint64_t targetPackets = options.packetsPerSecond > 0.0
                                            ? static_cast<std::uint64_t>(std::floor(options.durationSeconds * options.packetsPerSecond))
                                            : std::numeric_limits<std::uint64_t>::max();
    std::uint64_t sequence = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t packetsSent = 0;
    std::uint64_t sendErrors = 0;

    while (g_running.load(std::memory_order_relaxed) && Clock::now() < deadline && sequence < targetPackets)
    {
        unsigned batchCount = options.sendBatch;
        if (options.packetsPerSecond > 0.0)
        {
            const std::uint64_t remaining = targetPackets - sequence;
            batchCount = static_cast<unsigned>(std::min<std::uint64_t>(remaining, options.sendBatch));

            // Pace the *end* of each sendmmsg batch. This prevents the sender from getting ahead
            // of the requested average PPS, while retaining sendmmsg batching. The packets within
            // a batch are intentionally a small burst; keep --send-batch identical between runs.
            const double scheduledSeconds = static_cast<double>(sequence + batchCount - 1U) / options.packetsPerSecond;
            const auto scheduled = begin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(scheduledSeconds));
            if (Clock::now() < scheduled)
                std::this_thread::sleep_until(scheduled);
        }

        for (unsigned i = 0; i < batchCount; ++i)
        {
            const PacketHeader header = MakeHeader(PacketType::Data, session, sequence + i);
            std::memcpy(buffers[i].data(), &header, sizeof(header));
            messages[i].msg_len = 0;
        }

        const int sent = sendmmsg(fd, messages.data(), batchCount, 0);
        if (sent < 0)
        {
            if (errno == EINTR)
                continue;
            ++sendErrors;
            if (errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            const int error = errno;
            close(fd);
            throw std::runtime_error("sendmmsg: " + ErrnoString(error));
        }

        packetsSent += static_cast<std::uint64_t>(sent);
        bytesSent += static_cast<std::uint64_t>(sent) * options.packetSize;
        sequence += static_cast<std::uint64_t>(sent);
    }

    const auto end = Clock::now();
    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double mbps = seconds > 0.0 ? static_cast<double>(bytesSent) * 8.0 / seconds / 1'000'000.0 : 0.0;
    const double pps = seconds > 0.0 ? static_cast<double>(packetsSent) / seconds : 0.0;

    PacketHeader stop = MakeHeader(PacketType::Stop, session, sequence);
    const unsigned initialStopCount = std::max(64U, *receiverInflight * 4U);
    for (unsigned i = 0; i < initialStopCount; ++i)
        (void)send(fd, &stop, sizeof(stop), 0);

    std::cout << "\nSender result\n"
              << "  elapsed:     " << std::fixed << std::setprecision(3) << seconds << " s\n"
              << "  sent:        " << packetsSent << " packets\n"
              << "  throughput:  " << std::setprecision(2) << mbps << " Mbit/s\n"
              << "  packet rate: " << std::setprecision(0) << pps << " packets/s\n"
              << "  send errors: " << sendErrors << "\n";

    const auto resultDeadline = Clock::now() + std::chrono::seconds(2);
    auto nextStopRetry = Clock::now() + std::chrono::milliseconds(20);
    while (Clock::now() < resultDeadline)
    {
        const auto now = Clock::now();
        const auto untilRetry = std::chrono::duration_cast<std::chrono::milliseconds>(nextStopRetry - now).count();
        const auto untilDeadline = std::chrono::duration_cast<std::chrono::milliseconds>(resultDeadline - now).count();
        const int waitMs = static_cast<int>(std::max<std::int64_t>(1, std::min(untilRetry, untilDeadline)));

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pollResult = poll(&pfd, 1, waitMs);
        if (pollResult < 0)
        {
            if (errno == EINTR)
                continue;
            ThrowErrno("poll waiting for RESULT");
        }

        if (pollResult > 0)
        {
            ResultPacket result{};
            const ssize_t bytes = recv(fd, &result, sizeof(result), 0);
            if (bytes >= static_cast<ssize_t>(sizeof(result)) && IsValidHeader(result.header) &&
                GetPacketType(result.header) == PacketType::Result && GetSession(result.header) == session)
            {
                PrintRemoteResult(result);
                break;
            }
        }

        if (Clock::now() >= nextStopRetry)
        {
            for (unsigned i = 0; i < std::max(16U, *receiverInflight); ++i)
                (void)send(fd, &stop, sizeof(stop), 0);
            nextStopRetry = Clock::now() + std::chrono::milliseconds(20);
        }
    }

    close(fd);
    return 0;
}

std::uint64_t ParseUnsigned(std::string_view text, const char *name)
{
    std::size_t consumed = 0;
    const std::string owned(text);
    const unsigned long long value = std::stoull(owned, &consumed, 10);
    if (consumed != owned.size())
        throw std::runtime_error(std::string("invalid ") + name + ": " + owned);
    return static_cast<std::uint64_t>(value);
}

double ParseDouble(std::string_view text, const char *name)
{
    std::size_t consumed = 0;
    const std::string owned(text);
    const double value = std::stod(owned, &consumed);
    if (consumed != owned.size() || !std::isfinite(value))
        throw std::runtime_error(std::string("invalid ") + name + ": " + owned);
    return value;
}

void PrintUsage(const char *argv0)
{
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " receiver [options]\n"
        << "  " << argv0 << " sender [options]\n"
        << "  " << argv0 << " discover [--discovery-port N]\n\n"
        << "Receiver options:\n"
        << "  --backend recv|uring|uring-oneshot\n"
        << "                              Receive implementation (default: recv)\n"
        << "  --port N                   Benchmark UDP port (default: 39001)\n"
        << "  --discovery-port N         Discovery UDP port (default: 39000)\n"
        << "  --queue-depth N            uring provided-buffer/queue count (default: 256)\n"
        << "  --socket-buffer BYTES      Requested SO_RCVBUF (default: 16777216)\n"
        << "  --cpu N                    Pin receiver process to CPU N\n"
        << "  --once                     Exit after one completed benchmark\n"
        << "  --quiet                    Disable once-per-second progress output\n\n"
        << "Sender options:\n"
        << "  --target IPv4              Receiver address; omit to use broadcast discovery\n"
        << "  --port N                   Benchmark UDP port for explicit target (default: 39001)\n"
        << "  --discovery-port N         Discovery UDP port (default: 39000)\n"
        << "  --duration SECONDS         Test duration (default: 10)\n"
        << "  --packet-size BYTES        UDP payload size (default: 1400)\n"
        << "  --pps N                    Offered packet rate; 0 means unlimited (default: 0)\n"
        << "  --send-batch N             sendmmsg batch size (default: 64)\n"
        << "  --socket-buffer BYTES      Requested SO_SNDBUF (default: 16777216)\n"
        << "  --cpu N                    Pin sender process to CPU N\n\n"
        << "Examples:\n"
        << "  " << argv0 << " receiver --backend recv --once --quiet --cpu 4\n"
        << "  " << argv0 << " sender --packet-size 64 --pps 400000 --duration 30\n"
        << "  " << argv0 << " receiver --backend uring --queue-depth 256 --once --quiet --cpu 4\n"
        << "  " << argv0 << " sender --packet-size 64 --pps 400000 --duration 30\n"
        << "  " << argv0 << " receiver --backend uring-oneshot --queue-depth 32 --once\n";
}

std::string_view RequireValue(int argc, char **argv, int &index, std::string_view option)
{
    if (index + 1 >= argc)
        throw std::runtime_error(std::string(option) + " requires a value");
    ++index;
    return argv[index];
}

std::uint16_t ParsePort(std::string_view text, const char *name)
{
    const std::uint64_t value = ParseUnsigned(text, name);
    if (value == 0 || value > 65535)
        throw std::runtime_error(std::string(name) + " must be between 1 and 65535");
    return static_cast<std::uint16_t>(value);
}

int RunDiscover(std::uint16_t discoveryPort)
{
    const std::vector<Peer> peers = DiscoverPeers(discoveryPort, kDiscoveryWindowMs);
    if (peers.empty())
    {
        std::cout << "No receivers found.\n";
        return 1;
    }

    for (const Peer &peer : peers)
        std::cout << std::left << std::setw(24) << peer.hostname << " " << AddressToString(peer.address) << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    struct sigaction action{};
    action.sa_handler = SignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // Deliberately do not use SA_RESTART: Ctrl-C must wake recv()/recvfrom().
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0)
    {
        std::cerr << "error: sigaction: " << ErrnoString(errno) << '\n';
        return 1;
    }

    try
    {
        if (argc < 2)
        {
            PrintUsage(argv[0]);
            return 1;
        }

        const std::string command = argv[1];
        if (command == "--help" || command == "-h" || command == "help")
        {
            PrintUsage(argv[0]);
            return 0;
        }

        if (command == "receiver")
        {
            ReceiverOptions options;
            for (int i = 2; i < argc; ++i)
            {
                const std::string_view arg = argv[i];
                if (arg == "--backend")
                    options.backend = RequireValue(argc, argv, i, arg);
                else if (arg == "--port")
                    options.benchmarkPort = ParsePort(RequireValue(argc, argv, i, arg), "port");
                else if (arg == "--discovery-port")
                    options.discoveryPort = ParsePort(RequireValue(argc, argv, i, arg), "discovery port");
                else if (arg == "--queue-depth")
                    options.queueDepth = static_cast<unsigned>(ParseUnsigned(RequireValue(argc, argv, i, arg), "queue depth"));
                else if (arg == "--socket-buffer")
                    options.socketBuffer = static_cast<int>(ParseUnsigned(RequireValue(argc, argv, i, arg), "socket buffer"));
                else if (arg == "--cpu")
                    options.cpu = static_cast<int>(ParseUnsigned(RequireValue(argc, argv, i, arg), "cpu"));
                else if (arg == "--once")
                    options.once = true;
                else if (arg == "--quiet")
                    options.quiet = true;
                else if (arg == "--help" || arg == "-h")
                {
                    PrintUsage(argv[0]);
                    return 0;
                }
                else
                    throw std::runtime_error("unknown receiver option: " + std::string(arg));
            }
            return RunReceiver(options);
        }

        if (command == "sender")
        {
            SenderOptions options;
            for (int i = 2; i < argc; ++i)
            {
                const std::string_view arg = argv[i];
                if (arg == "--target")
                    options.target = std::string(RequireValue(argc, argv, i, arg));
                else if (arg == "--port")
                    options.benchmarkPort = ParsePort(RequireValue(argc, argv, i, arg), "port");
                else if (arg == "--discovery-port")
                    options.discoveryPort = ParsePort(RequireValue(argc, argv, i, arg), "discovery port");
                else if (arg == "--duration")
                    options.durationSeconds = ParseDouble(RequireValue(argc, argv, i, arg), "duration");
                else if (arg == "--packet-size")
                    options.packetSize = static_cast<std::size_t>(ParseUnsigned(RequireValue(argc, argv, i, arg), "packet size"));
                else if (arg == "--pps")
                    options.packetsPerSecond = ParseDouble(RequireValue(argc, argv, i, arg), "pps");
                else if (arg == "--send-batch")
                    options.sendBatch = static_cast<unsigned>(ParseUnsigned(RequireValue(argc, argv, i, arg), "send batch"));
                else if (arg == "--socket-buffer")
                    options.socketBuffer = static_cast<int>(ParseUnsigned(RequireValue(argc, argv, i, arg), "socket buffer"));
                else if (arg == "--cpu")
                    options.cpu = static_cast<int>(ParseUnsigned(RequireValue(argc, argv, i, arg), "cpu"));
                else if (arg == "--help" || arg == "-h")
                {
                    PrintUsage(argv[0]);
                    return 0;
                }
                else
                    throw std::runtime_error("unknown sender option: " + std::string(arg));
            }
            return RunSender(options);
        }

        if (command == "discover")
        {
            std::uint16_t discoveryPort = kDefaultDiscoveryPort;
            for (int i = 2; i < argc; ++i)
            {
                const std::string_view arg = argv[i];
                if (arg == "--discovery-port")
                    discoveryPort = ParsePort(RequireValue(argc, argv, i, arg), "discovery port");
                else if (arg == "--help" || arg == "-h")
                {
                    PrintUsage(argv[0]);
                    return 0;
                }
                else
                    throw std::runtime_error("unknown discover option: " + std::string(arg));
            }
            return RunDiscover(discoveryPort);
        }

        PrintUsage(argv[0]);
        throw std::runtime_error("unknown command: " + command);
    }
    catch (const std::exception &e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
