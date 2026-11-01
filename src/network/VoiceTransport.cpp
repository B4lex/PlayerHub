#include "network/VoiceTransport.h"
#include "network/ServerClient.h"
#include "network/TrafficMeter.h"
#include "runtime/Profiling.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace Network::VoiceTransport
{
    namespace
    {
        using namespace std::chrono_literals;
        using Clock = std::chrono::steady_clock;

        // The datagram layout of server/internal/voice.
        constexpr std::uint8_t kKindVoice = 1;
        constexpr std::uint8_t kKindProbe = 2;
        constexpr std::uint8_t kFlagEnd = 1;
        constexpr std::uint8_t kDirectionToServer = 1;
        constexpr std::uint8_t kDirectionToClient = 2;
        constexpr std::size_t kTagBytes = 16;
        constexpr std::size_t kNonceBytes = 12;
        constexpr std::size_t kClientHeaderBytes = 1 + 4 + 4;
        constexpr std::size_t kServerHeaderBytes = 1 + 4;
        constexpr std::size_t kVoiceHeaderBytes = 2 + 1;
        constexpr std::size_t kPeerVoiceHeaderBytes = 4 + kVoiceHeaderBytes;
        constexpr std::size_t kProbeBytes = 4;
        constexpr std::size_t kMaximumDatagramBytes
            = kClientHeaderBytes + kPeerVoiceHeaderBytes + Multiplayer::kMaximumVoiceFrameBytes + kTagBytes;
        // Counted with each datagram in the traffic rates: an IPv4 and a UDP header.
        constexpr std::size_t kDatagramOverheadBytes = 20 + 8;

        // Also how often the thread looks at its timers and at Stop.
        constexpr DWORD kReceiveTimeoutMilliseconds = 100;
        constexpr auto kUnconfirmedProbeInterval = 1s;
        // Within the shortest common NAT mapping lifetime for UDP, about 30 s.
        constexpr auto kKeepAliveInterval = 5s;
        constexpr auto kSilenceTimeout = 12s;

        std::uint32_t ReadU32(const std::uint8_t* a_bytes)
        {
            return static_cast<std::uint32_t>(a_bytes[0]) | static_cast<std::uint32_t>(a_bytes[1]) << 8
                 | static_cast<std::uint32_t>(a_bytes[2]) << 16 | static_cast<std::uint32_t>(a_bytes[3]) << 24;
        }

        void WriteU32(std::uint8_t* a_bytes, std::uint32_t a_value)
        {
            for (int i = 0; i < 4; ++i)
            {
                a_bytes[i] = static_cast<std::uint8_t>(a_value >> (8 * i));
            }
        }

        std::array<std::uint8_t, kNonceBytes> MakeNonce(std::uint8_t a_direction, std::uint32_t a_counter)
        {
            std::array<std::uint8_t, kNonceBytes> nonce{};
            nonce[0] = a_direction;
            WriteU32(nonce.data() + 4, a_counter);
            return nonce;
        }

        // AES-256-GCM through Windows CNG. A key handle is used by one thread at a time.
        class AesGcm
        {
        public:
            AesGcm() = default;
            AesGcm(const AesGcm&) = delete;
            AesGcm& operator=(const AesGcm&) = delete;

            ~AesGcm()
            {
                if (key)
                {
                    BCryptDestroyKey(key);
                }
                if (algorithm)
                {
                    BCryptCloseAlgorithmProvider(algorithm, 0);
                }
            }

            bool Initialize(std::span<const std::uint8_t> a_key)
            {
                if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)))
                {
                    return false;
                }
                if (!BCRYPT_SUCCESS(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                        sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
                {
                    return false;
                }
                return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0,
                    const_cast<PUCHAR>(a_key.data()), static_cast<ULONG>(a_key.size()), 0));
            }

            // Writes the ciphertext and then the tag to a_out, which holds a_plaintext.size() + kTagBytes.
            bool Seal(std::span<const std::uint8_t, kNonceBytes> a_nonce, std::span<const std::uint8_t> a_aad,
                std::span<const std::uint8_t> a_plaintext, std::uint8_t* a_out) const
            {
                BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
                BCRYPT_INIT_AUTH_MODE_INFO(info);
                info.pbNonce = const_cast<PUCHAR>(a_nonce.data());
                info.cbNonce = static_cast<ULONG>(a_nonce.size());
                info.pbAuthData = const_cast<PUCHAR>(a_aad.data());
                info.cbAuthData = static_cast<ULONG>(a_aad.size());
                info.pbTag = a_out + a_plaintext.size();
                info.cbTag = kTagBytes;
                ULONG written = 0;
                return BCRYPT_SUCCESS(BCryptEncrypt(key, const_cast<PUCHAR>(a_plaintext.data()),
                           static_cast<ULONG>(a_plaintext.size()), &info, nullptr, 0, a_out,
                           static_cast<ULONG>(a_plaintext.size()), &written, 0))
                    && written == a_plaintext.size();
            }

            // a_sealed is the ciphertext followed by the tag; a_out holds its size minus kTagBytes.
            bool Open(std::span<const std::uint8_t, kNonceBytes> a_nonce, std::span<const std::uint8_t> a_aad,
                std::span<const std::uint8_t> a_sealed, std::uint8_t* a_out) const
            {
                if (a_sealed.size() < kTagBytes)
                {
                    return false;
                }
                const auto size = a_sealed.size() - kTagBytes;
                BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
                BCRYPT_INIT_AUTH_MODE_INFO(info);
                info.pbNonce = const_cast<PUCHAR>(a_nonce.data());
                info.cbNonce = static_cast<ULONG>(a_nonce.size());
                info.pbAuthData = const_cast<PUCHAR>(a_aad.data());
                info.cbAuthData = static_cast<ULONG>(a_aad.size());
                info.pbTag = const_cast<PUCHAR>(a_sealed.data() + size);
                info.cbTag = kTagBytes;
                ULONG written = 0;
                return BCRYPT_SUCCESS(BCryptDecrypt(key, const_cast<PUCHAR>(a_sealed.data()), static_cast<ULONG>(size),
                           &info, nullptr, 0, a_out, static_cast<ULONG>(size), &written, 0))
                    && written == size;
            }

        private:
            BCRYPT_ALG_HANDLE algorithm{ nullptr };
            BCRYPT_KEY_HANDLE key{ nullptr };
        };

        // Accepts each counter once, and older ones only within the last 64, as the relay does.
        class ReplayWindow
        {
        public:
            bool Accept(std::uint32_t a_counter)
            {
                if (!seen)
                {
                    seen = true;
                    highest = a_counter;
                    bits = 1;
                    return true;
                }
                if (a_counter > highest)
                {
                    const auto shift = a_counter - highest;
                    bits = shift >= 64 ? 1 : (bits << shift) | 1;
                    highest = a_counter;
                    return true;
                }
                const auto age = highest - a_counter;
                if (age >= 64 || (bits & (std::uint64_t{ 1 } << age)) != 0)
                {
                    return false;
                }
                bits |= std::uint64_t{ 1 } << age;
                return true;
            }

        private:
            bool seen{ false };
            std::uint32_t highest{ 0 };
            std::uint64_t bits{ 0 };
        };

        struct Session
        {
            Multiplayer::VoiceAccess access;
            std::string host;
            std::atomic_bool stopping{ false };
            std::atomic<Route> route{ Route::kWebSocket };

            // The socket is set once by the session's thread before route can become kDatagrams.
            std::atomic<SOCKET> socket{ INVALID_SOCKET };
            std::mutex sendLock;
            AesGcm sealer;
            std::atomic_uint32_t counter{ 0 };

            // The session's thread only.
            AesGcm opener;
            ReplayWindow window;
        };

        std::mutex s_lock;
        std::shared_ptr<Session> s_session;
        std::atomic<Sink> s_sink{ nullptr };

        std::shared_ptr<Session> GetSession()
        {
            const std::scoped_lock lock{ s_lock };
            return s_session;
        }

        // Sends the route on the WebSocket only while a_session is current, so a stopping session's
        // thread never tells a newer connection about its own datagrams.
        void PublishRoute(Session& a_session, Route a_route)
        {
            const std::scoped_lock lock{ s_lock };
            if (s_session.get() != &a_session || a_session.stopping.load(std::memory_order_acquire))
            {
                return;
            }
            a_session.route.store(a_route, std::memory_order_release);
            PublishVoiceRoute(a_route == Route::kDatagrams);
        }

        std::string GetHost(std::string_view a_url)
        {
            const auto scheme = a_url.find("://");
            if (scheme == std::string_view::npos)
            {
                return {};
            }
            a_url.remove_prefix(scheme + 3);
            a_url = a_url.substr(0, a_url.find_first_of("/?#"));
            if (const auto at = a_url.rfind('@'); at != std::string_view::npos)
            {
                a_url.remove_prefix(at + 1);
            }
            if (a_url.starts_with('['))
            {
                const auto close = a_url.find(']');
                return close == std::string_view::npos ? std::string{} : std::string{ a_url.substr(1, close - 1) };
            }
            return std::string{ a_url.substr(0, a_url.find(':')) };
        }

        bool SendDatagram(Session& a_session, std::uint8_t a_kind, std::span<const std::uint8_t> a_plaintext)
        {
            const auto socket = a_session.socket.load(std::memory_order_acquire);
            if (socket == INVALID_SOCKET)
            {
                return false;
            }
            std::array<std::uint8_t, kMaximumDatagramBytes> datagram;
            const auto counter = a_session.counter.fetch_add(1, std::memory_order_acq_rel) + 1;
            datagram[0] = a_kind;
            WriteU32(datagram.data() + 1, a_session.access.id);
            WriteU32(datagram.data() + 5, counter);
            const auto nonce = MakeNonce(kDirectionToServer, counter);
            const std::scoped_lock lock{ a_session.sendLock };
            if (!a_session.sealer.Seal(nonce, std::span{ datagram.data(), kClientHeaderBytes }, a_plaintext,
                    datagram.data() + kClientHeaderBytes))
            {
                return false;
            }
            const auto size = static_cast<int>(kClientHeaderBytes + a_plaintext.size() + kTagBytes);
            if (::send(socket, reinterpret_cast<const char*>(datagram.data()), size, 0) != size)
            {
                return false;
            }
            CountSentBytes(static_cast<std::size_t>(size) + kDatagramOverheadBytes);
            return true;
        }

        SOCKET OpenSocket(const Session& a_session)
        {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_protocol = IPPROTO_UDP;
            addrinfo* results = nullptr;
            const auto port = std::to_string(a_session.access.port);
            if (::getaddrinfo(a_session.host.c_str(), port.c_str(), &hints, &results) != 0 || !results)
            {
                logs::warn("Voice could not resolve the relay's host; voice uses the WebSocket");
                return INVALID_SOCKET;
            }
            SOCKET result = INVALID_SOCKET;
            for (auto* address = results; address && result == INVALID_SOCKET; address = address->ai_next)
            {
                const auto candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
                if (candidate == INVALID_SOCKET)
                {
                    continue;
                }
                // Connecting a datagram socket only fixes its peer, so recv drops anyone else's.
                if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0)
                {
                    result = candidate;
                }
                else
                {
                    ::closesocket(candidate);
                }
            }
            ::freeaddrinfo(results);
            if (result == INVALID_SOCKET)
            {
                logs::warn("Voice could not open a datagram socket; voice uses the WebSocket");
                return INVALID_SOCKET;
            }
            const DWORD timeout = kReceiveTimeoutMilliseconds;
            ::setsockopt(result, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            return result;
        }

        // Returns whether the datagram was the relay's.
        bool Receive(Session& a_session, std::span<const std::uint8_t> a_datagram)
        {
            if (a_datagram.size() < kServerHeaderBytes + kTagBytes || a_datagram.size() > kMaximumDatagramBytes)
            {
                return false;
            }
            const auto kind = a_datagram[0];
            const auto counter = ReadU32(a_datagram.data() + 1);
            const auto nonce = MakeNonce(kDirectionToClient, counter);
            std::array<std::uint8_t, kMaximumDatagramBytes> plaintext;
            const auto sealed = a_datagram.subspan(kServerHeaderBytes);
            if (!a_session.opener.Open(nonce, a_datagram.first(kServerHeaderBytes), sealed, plaintext.data())
                || !a_session.window.Accept(counter))
            {
                return false;
            }
            const auto size = sealed.size() - kTagBytes;
            if (kind == kKindProbe)
            {
                return size == kProbeBytes;
            }
            if (kind != kKindVoice || size < kPeerVoiceHeaderBytes)
            {
                return false;
            }
            Multiplayer::VoiceFrame frame;
            frame.speaker = ReadU32(plaintext.data());
            frame.sequence = static_cast<std::uint16_t>(plaintext[4] | plaintext[5] << 8);
            const auto flags = plaintext[6];
            const auto opusSize = size - kPeerVoiceHeaderBytes;
            if (frame.speaker == 0 || (flags & ~kFlagEnd) != 0 || !Multiplayer::IsValidVoiceFrameSize(opusSize))
            {
                return false;
            }
            frame.end = (flags & kFlagEnd) != 0;
            frame.size = static_cast<std::uint16_t>(opusSize);
            std::copy_n(plaintext.data() + kPeerVoiceHeaderBytes, opusSize, frame.opus.begin());
            if (const auto sink = s_sink.load(std::memory_order_acquire))
            {
                sink(frame);
            }
            return true;
        }

        void Run(std::shared_ptr<Session> a_session)
        {
            PROFILE_THREAD_NAME("Voice datagrams");
            auto& session = *a_session;
            const auto socket = OpenSocket(session);
            if (socket == INVALID_SOCKET)
            {
                return;
            }
            session.socket.store(socket, std::memory_order_release);

            bool confirmed = false;
            auto nextProbe = Clock::now();
            auto lastHeard = Clock::now();
            std::uint32_t probeID = 0;
            std::array<std::uint8_t, kMaximumDatagramBytes + 1> buffer;
            while (!session.stopping.load(std::memory_order_acquire))
            {
                const auto now = Clock::now();
                if (now >= nextProbe)
                {
                    std::array<std::uint8_t, kProbeBytes> probe;
                    WriteU32(probe.data(), ++probeID);
                    SendDatagram(session, kKindProbe, probe);
                    nextProbe = now + (confirmed ? kKeepAliveInterval : kUnconfirmedProbeInterval);
                }
                if (confirmed && now - lastHeard > kSilenceTimeout)
                {
                    confirmed = false;
                    logs::warn("Voice datagrams stopped arriving; voice uses the WebSocket until they return");
                    PublishRoute(session, Route::kWebSocket);
                    nextProbe = now;
                }

                const auto received = ::recv(socket, reinterpret_cast<char*>(buffer.data()),
                    static_cast<int>(buffer.size()), 0);
                // A timeout, or an ICMP error reported for an earlier datagram (WSAECONNRESET).
                if (received <= 0)
                {
                    continue;
                }
                CountReceivedBytes(static_cast<std::size_t>(received) + kDatagramOverheadBytes);
                if (!Receive(session, std::span{ buffer.data(), static_cast<std::size_t>(received) }))
                {
                    continue;
                }
                lastHeard = Clock::now();
                if (!confirmed)
                {
                    confirmed = true;
                    nextProbe = lastHeard + kKeepAliveInterval;
                    logs::info("Voice datagrams reach the relay both ways; voice uses them");
                    PublishRoute(session, Route::kDatagrams);
                }
            }
            session.socket.store(INVALID_SOCKET, std::memory_order_release);
            {
                // Waits for a Send that loaded the socket before it was cleared.
                const std::scoped_lock lock{ session.sendLock };
                ::closesocket(socket);
            }
        }
    }

    void SetSink(Sink a_sink)
    {
        s_sink.store(a_sink, std::memory_order_release);
    }

    void Start(const Multiplayer::VoiceAccess& a_access, std::string_view a_webSocketUrl)
    {
        auto session = std::make_shared<Session>();
        session->access = a_access;
        session->host = a_access.host.empty() ? GetHost(a_webSocketUrl) : a_access.host;
        const bool datagrams = !session->host.empty() && session->sealer.Initialize(a_access.key)
                            && session->opener.Initialize(a_access.key);
        if (!datagrams)
        {
            logs::error("Voice could not set up the relay's key or host; voice uses the WebSocket");
        }
        std::shared_ptr<Session> previous;
        {
            const std::scoped_lock lock{ s_lock };
            previous = std::exchange(s_session, session);
        }
        if (previous)
        {
            previous->stopping.store(true, std::memory_order_release);
        }
        logs::info("Voice starts: radius={} relay host {}", a_access.radius,
            a_access.host.empty() ? "is the server's" : "is named by the server");
        if (datagrams)
        {
            // Detached: it ends within one receive timeout of Stop and holds its own reference.
            std::thread(Run, std::move(session)).detach();
        }
    }

    void Stop()
    {
        std::shared_ptr<Session> previous;
        {
            const std::scoped_lock lock{ s_lock };
            previous = std::exchange(s_session, nullptr);
        }
        if (previous)
        {
            previous->stopping.store(true, std::memory_order_release);
            logs::info("Voice stops with the connection");
        }
    }

    void DeliverFromWebSocket(const Multiplayer::VoiceFrame& a_frame)
    {
        if (const auto sink = s_sink.load(std::memory_order_acquire))
        {
            sink(a_frame);
        }
    }

    void Send(std::uint16_t a_sequence, bool a_end, std::span<const std::uint8_t> a_opus)
    {
        if (!Multiplayer::IsValidVoiceFrameSize(a_opus.size()))
        {
            return;
        }
        const auto session = GetSession();
        if (!session)
        {
            return;
        }
        if (session->route.load(std::memory_order_acquire) == Route::kDatagrams)
        {
            std::array<std::uint8_t, kVoiceHeaderBytes + Multiplayer::kMaximumVoiceFrameBytes> plaintext;
            plaintext[0] = static_cast<std::uint8_t>(a_sequence);
            plaintext[1] = static_cast<std::uint8_t>(a_sequence >> 8);
            plaintext[2] = a_end ? kFlagEnd : 0;
            std::ranges::copy(a_opus, plaintext.begin() + kVoiceHeaderBytes);
            if (SendDatagram(*session, kKindVoice, std::span{ plaintext.data(), kVoiceHeaderBytes + a_opus.size() }))
            {
                return;
            }
        }
        PublishVoiceFrame(a_sequence, a_end, a_opus);
    }

    Route GetRoute()
    {
        const auto session = GetSession();
        return session ? session->route.load(std::memory_order_acquire) : Route::kNone;
    }

    std::uint32_t GetRadius()
    {
        const auto session = GetSession();
        return session ? session->access.radius : 0;
    }
}
