#include "harbor/gateway.hpp"

#include <cerrno>
#include <sys/socket.h>
#include "harbor/feed.hpp"

namespace harbor {

namespace {
constexpr std::uint64_t kOrderListener = 1;
constexpr std::uint64_t kSnapshotListener = 2;
constexpr std::size_t kMaxOutboundPerPass = 65'536;
}  // namespace

Gateway::Gateway(const GatewayOptions& options, InboundRing& inbound, OutboundRing& outbound, Seqs seqs)
    : opts_(options),
      inbound_(inbound),
      outbound_(outbound),
      seqs_(std::move(seqs)),
      packetizer_(options.feedSession),
      readBuf_(65'536) {
    orderListener_ = listenTcp(opts_.orderPort, orderPort_);
    snapshotListener_ = listenTcp(opts_.snapshotPort, snapshotPort_);
    poller_.add(orderListener_.get(), kOrderListener, true, false);
    poller_.add(snapshotListener_.get(), kSnapshotListener, true, false);
    if (opts_.feedPort != 0) {
        std::uint16_t unused = 0;
        feedSocket_ = openUdp(0, unused);
        feedTarget_ = makeAddr(opts_.feedHost, opts_.feedPort);
    }
}

void Gateway::runOnce(int timeoutMs) {
    if (!stopSent_ && stopRequested_.load(std::memory_order_acquire)) {
        InboundItem stop{};
        stop.kind = InboundItem::Kind::Stop;
        stopSent_ = inbound_.tryPush(stop);
    }
    routeOutbound();
    // Retry pushes that did not fit. Collect first: processing a frame can close
    // (and erase) its session.
    std::vector<std::uint64_t> blocked;
    for (auto& [token, s] : sessions_) {
        if (s.blocked) {
            blocked.push_back(token);
        }
    }
    for (std::uint64_t token : blocked) {
        auto it = sessions_.find(token);
        if (it != sessions_.end()) {
            processFrames(it->second);
        }
    }
    bool pending = false;
    for (auto& [token, s] : sessions_) {
        pending = pending || s.blocked.has_value() || s.outPos < s.out.size();
    }
    poller_.wait(events_, pending ? 0 : timeoutMs);
    for (const PollEvent& e : events_) {
        if (e.token == kOrderListener) {
            accept(orderListener_.get(), false);
            continue;
        }
        if (e.token == kSnapshotListener) {
            accept(snapshotListener_.get(), true);
            continue;
        }
        auto it = sessions_.find(e.token);
        if (it == sessions_.end()) {
            continue;  // closed earlier in this pass
        }
        if (e.readable || e.hangup) {
            readFrom(it->second);
        }
        it = sessions_.find(e.token);
        if (it != sessions_.end() && e.writable) {
            flush(it->second);
        }
    }
    routeOutbound();
    flushFeed();
    std::vector<std::uint64_t> done;
    for (auto& [token, s] : sessions_) {
        if (s.outPos < s.out.size()) {
            flush(s);
        }
        if (s.closing && s.outPos >= s.out.size()) {
            done.push_back(token);
        }
    }
    for (std::uint64_t token : done) {
        close(token);
    }
}

void Gateway::accept(int listenFd, bool snapshot) {
    for (;;) {
        int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0) {
            return;  // EAGAIN: no more pending connections (or a transient error)
        }
        setNonBlocking(fd);
        setNoDelay(fd);
        setNoSigPipe(fd);
        std::uint64_t token = nextToken_++;
        Session& s = sessions_[token];
        s.fd = Fd(fd);
        s.token = token;
        s.snapshot = snapshot;
        poller_.add(fd, token, true, false);
        sessionCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Gateway::readFrom(Session& s) {
    if (s.blocked || s.closing) {
        return;  // not reading until the inbound ring has room / the session is going away
    }
    std::uint64_t token = s.token;
    for (;;) {
        ssize_t n = ::recv(s.fd.get(), readBuf_.data(), readBuf_.size(), 0);
        if (n > 0) {
            s.reader.feed(readBuf_.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(token);  // peer closed or socket error
            return;
        }
        if (errno != EINTR) {
            break;
        }
    }
    processFrames(s);
}

void Gateway::processFrames(Session& s) {
    if (s.blocked) {
        if (!inbound_.tryPush(*s.blocked)) {
            return;
        }
        s.blocked.reset();
        updateInterest(s);
    }
    std::uint64_t token = s.token;
    wire::Frame f{};
    while (!s.closing && !s.blocked) {
        auto st = s.reader.next(f);
        if (st == wire::FrameReader::Status::NeedMore) {
            return;
        }
        if (st == wire::FrameReader::Status::Error) {
            close(token);  // malformed stream: nothing after it can be trusted
            return;
        }
        handleFrame(s, f);
        if (!sessions_.contains(token)) {
            return;
        }
    }
}

bool Gateway::pushOrBlock(Session& s, const InboundItem& item) {
    if (inbound_.tryPush(item)) {
        return true;
    }
    s.blocked = item;  // TCP flow control now pushes back on this client
    updateInterest(s);
    return false;
}

void Gateway::handleFrame(Session& s, const wire::Frame& f) {
    using wire::MsgType;
    if (s.snapshot) {
        if (f.type != MsgType::SnapshotRequest || s.snapshotAsked) {
            close(s.token);
            return;
        }
        s.snapshotAsked = true;
        InboundItem item{};
        item.kind = InboundItem::Kind::Snapshot;
        item.origin = s.token;
        pushOrBlock(s, item);
        return;
    }
    if (f.type == MsgType::Login) {
        wire::Login login{};
        if (s.client != 0 || !wire::decode(f.payload, login)) {
            close(s.token);
            return;
        }
        if (login.clientId == 0 || byClient_.contains(login.clientId)) {
            send(s, wire::LoginRejected{login.clientId == 0 ? wire::LoginRejected::Unknown
                                                            : wire::LoginRejected::DuplicateSession});
            s.closing = true;
            return;
        }
        s.client = login.clientId;
        byClient_[s.client] = s.token;
        auto [durable, feed] = seqs_();
        send(s, wire::LoginAccepted{durable, feed});
        return;
    }
    if (s.client == 0) {
        close(s.token);  // commands before login
        return;
    }
    InboundItem item{};
    item.kind = InboundItem::Kind::Command;
    item.origin = s.token;
    switch (f.type) {
        case MsgType::NewOrder: {
            wire::NewOrderMsg m{};
            if (!wire::decode(f.payload, m)) {
                close(s.token);
                return;
            }
            item.cmd = Command{Command::Kind::Add,
                               NewOrder{exchangeId(s.client, m.clientOrderId), s.client, m.side, m.price, m.qty, m.tif},
                               0, 0, 0};
            break;
        }
        case MsgType::Cancel: {
            wire::CancelMsg m{};
            if (!wire::decode(f.payload, m)) {
                close(s.token);
                return;
            }
            item.cmd = Command{Command::Kind::Cancel, {}, exchangeId(s.client, m.clientOrderId), 0, 0};
            break;
        }
        case MsgType::Replace: {
            wire::ReplaceMsg m{};
            if (!wire::decode(f.payload, m)) {
                close(s.token);
                return;
            }
            item.cmd = Command{Command::Kind::Replace, {}, exchangeId(s.client, m.clientOrderId), m.price, m.qty};
            break;
        }
        default:
            close(s.token);  // a server-to-client type, or a snapshot request on the order port
            return;
    }
    pushOrBlock(s, item);
}

Gateway::Session* Gateway::sessionFor(ClientId client) {
    auto it = byClient_.find(client);
    if (it == byClient_.end()) {
        return nullptr;
    }
    auto s = sessions_.find(it->second);
    return s == sessions_.end() ? nullptr : &s->second;
}

template <class M>
void Gateway::send(Session& s, const M& msg) {
    wire::encode(msg, s.out);
}

void Gateway::routeOutbound() {
    OutboundItem item{};
    for (std::size_t i = 0; i < kMaxOutboundPerPass && outbound_.tryPop(item); ++i) {
        route(item);
    }
}

void Gateway::route(const OutboundItem& item) {
    using Kind = OutboundItem::Kind;
    switch (item.kind) {
        case Kind::Response:
        case Kind::Execution: {
            // A response goes only to the session that sent the command; if it has
            // closed, a later session of the same client must not receive it. An
            // execution belongs to the order's owner, wherever they are connected now.
            Session* s = nullptr;
            if (item.kind == Kind::Response) {
                auto it = sessions_.find(item.origin);
                s = it == sessions_.end() ? nullptr : &it->second;
            } else {
                s = sessionFor(item.client);
            }
            if (s == nullptr || s->closing) {
                return;  // not connected: nothing to deliver to
            }
            if (item.kind == Kind::Response) {
                send(*s, item.response);
            } else {
                send(*s, item.execution);
            }
            if (s->out.size() - s->outPos > opts_.sendBufferLimit) {
                // Over the limit: give the socket a chance first, so a burst to a
                // client that is reading is not mistaken for a stalled one.
                std::uint64_t token = s->token;
                flush(*s);
                auto it = sessions_.find(token);
                if (it != sessions_.end() && it->second.out.size() - it->second.outPos > opts_.sendBufferLimit) {
                    close(token);  // a client that does not read must not stall everyone else
                }
            }
            return;
        }
        case Kind::Feed:
            publishFeed(item.event);
            return;
        case Kind::SnapshotHeader:
        case Kind::SnapshotOrder:
        case Kind::SnapshotEnd: {
            auto it = sessions_.find(item.origin);
            if (it == sessions_.end()) {
                return;  // the requester hung up
            }
            Session& s = it->second;
            if (item.kind == Kind::SnapshotHeader) {
                send(s, item.snapHeader);
            } else if (item.kind == Kind::SnapshotOrder) {
                send(s, item.snapOrder);
            } else {
                s.closing = true;
            }
            return;
        }
    }
}

void Gateway::publishFeed(const wire::FeedEvent& event) {
    if (!feedSocket_.valid()) {
        return;
    }
    if (!packetizer_.add(event)) {
        flushFeed();
        packetizer_.add(event);
    }
}

void Gateway::flushFeed() {
    if (!feedSocket_.valid() || packetizer_.empty()) {
        return;
    }
    auto packet = packetizer_.packet();
    // UDP is best effort by design; listeners detect any loss by sequence number.
    ::sendto(feedSocket_.get(), packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr*>(&feedTarget_),
             sizeof feedTarget_);
    packetizer_.clear();
}

void Gateway::flush(Session& s) {
    std::uint64_t token = s.token;
    while (s.outPos < s.out.size()) {
        ssize_t n = ::send(s.fd.get(), s.out.data() + s.outPos, s.out.size() - s.outPos, sendFlags());
        if (n > 0) {
            s.outPos += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!s.wantWrite) {
                s.wantWrite = true;
                updateInterest(s);
            }
            return;
        }
        close(token);
        return;
    }
    s.out.clear();
    s.outPos = 0;
    if (s.wantWrite) {
        s.wantWrite = false;
        updateInterest(s);
    }
}

void Gateway::updateInterest(Session& s) {
    poller_.update(s.fd.get(), s.token, !s.blocked.has_value(), s.wantWrite);
}

void Gateway::close(std::uint64_t token) {
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return;
    }
    Session& s = it->second;
    poller_.remove(s.fd.get());
    if (s.client != 0) {
        auto owner = byClient_.find(s.client);
        if (owner != byClient_.end() && owner->second == token) {
            byClient_.erase(owner);
        }
    }
    sessions_.erase(it);
    sessionCount_.fetch_sub(1, std::memory_order_relaxed);
}

}  // namespace harbor
