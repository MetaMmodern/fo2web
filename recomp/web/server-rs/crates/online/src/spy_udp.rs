//! Bounded, worker-thread UDP relay for OpenSpy NAT and public peers.
//!
//! DNS and socket operations belong to the workers; the Core thread only uses
//! nonblocking channel operations and forwards replies during its timer tick.

use std::collections::{HashMap, VecDeque};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, ToSocketAddrs, UdpSocket};
use std::sync::mpsc::{self, Receiver, SyncSender, TryRecvError};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use crate::core::Core;
use crate::timers::{Action, TimerId};

pub const NAT1_VIP: u32 = 0x0a4d1ffe;
pub const NAT2_VIP: u32 = 0x0a4d1ffd;

const NAT_PORT: u16 = 27901;
const MAX_DATAGRAM: usize = 1464;
const MAX_SESSIONS: usize = 256;
const MAX_SESSIONS_PER_VIP: usize = 4;
const QUEUE_DEPTH: usize = 64;
const IDLE_TIMEOUT: Duration = Duration::from_secs(60);
const POLL_INTERVAL: Duration = Duration::from_millis(2);
const EGRESS_BYTES_PER_SECOND: f64 = 32.0 * 1024.0;
const EGRESS_PACKETS_PER_SECOND: f64 = 200.0;
const CONTACTS_MAX: usize = 128;
const MAX_PEER_MAP: usize = 16;

// Opt-in and globally bounded: network addresses are sensitive. No payloads.
fn trace(args: std::fmt::Arguments<'_>) {
    static COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    if std::env::var_os("FO2_SPY_UDP_TRACE").is_some()
        && COUNT.fetch_add(1, Ordering::Relaxed) < 32
    {
        eprintln!("spy-udp-trace: {args}");
    }
}

#[derive(Clone, Copy, Hash, PartialEq, Eq)]
struct SessionKey {
    guest_vip: u32,
    guest_port: u16,
}

#[derive(Clone, Copy)]
enum Target {
    Nat(usize),
    Public(SocketAddrV4),
}

struct Outbound {
    target: Target,
    packet: Vec<u8>,
}

type Inbound = (u32, u16, Vec<u8>);

struct Session {
    outbound: SyncSender<Outbound>,
    inbound: Receiver<Inbound>,
    last_activity: Instant,
    stop: Arc<AtomicBool>,
}

impl Drop for Session {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

pub struct SpyUdp {
    enabled: bool,
    targets: [String; 4],
    /// Operator-pinned public (advertised) to private (actual LAN) endpoints.
    /// Parsed once from `FO2_SPY_UDP_PEER_MAP`; empty unless explicitly set.
    peer_map: HashMap<SocketAddrV4, SocketAddrV4>,
    #[cfg(test)]
    public_target: Option<SocketAddrV4>,
    sessions: HashMap<SessionKey, Session>,
    /// (destination, port) pairs already reported, so the log has one line each
    logged: std::collections::HashSet<(u32, u16)>,
    timer: Option<TimerId>,
}

impl SpyUdp {
    pub fn new(enabled: bool) -> Self {
        Self {
            enabled,
            targets: [
                "natneg1.openspy.net:27901".into(),
                "natneg2.openspy.net:27901".into(),
                // the relay VIP's own qr query port (10.77.0.1:27900)
                "flatout2pc.master.openspy.net:27900".into(),
                // a NAT host the DNS mapping sent to the relay VIP (10.77.0.1:27901)
                "natneg1.openspy.net:27901".into(),
            ],
            peer_map: peer_map_from_env(),
            #[cfg(test)]
            public_target: None,
            sessions: HashMap::new(),
            logged: std::collections::HashSet::new(),
            timer: None,
        }
    }
}

/// a datagram's first bytes, escaped, for the relay's bounded trace
fn preview(p: &[u8]) -> String {
    let mut s = String::new();
    for &b in p.iter().take(48) {
        match b {
            0x20..=0x7e => match b {
                b'\\' => s.push_str("\\\\"),
                _ => s.push(b as char),
            },
            b'\r' => s.push_str("\\r"),
            b'\n' => s.push_str("\\n"),
            _ => s.push_str(&format!("\\x{b:02x}")),
        }
    }
    if p.len() > 48 { s.push('…'); }
    s
}

fn public_ip_allowed(ip: Ipv4Addr) -> bool {
    let [a, b, c, _] = ip.octets();
    !(
        a == 0 || a == 10 || a == 127 || a >= 224
        || (a == 100 && (64..=127).contains(&b))
        || (a == 169 && b == 254)
        || (a == 172 && (16..=31).contains(&b))
        || (a == 192 && (b == 0 || (b == 88 && c == 99) || b == 168))
        || (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100)))
        || (a == 203 && b == 0 && c == 113)
    )
}

/// The actual side of an operator mapping must be RFC1918 unicast. The
/// switch's virtual 10.77.0.0/19 network is excluded so a mapping cannot
/// redirect a public peer into the relay's own addresses.
fn actual_private_ip_allowed(ip: Ipv4Addr) -> bool {
    let [a, b, c, _] = ip.octets();
    if ip.is_unspecified() || ip.is_loopback() || ip.is_multicast() || ip.is_broadcast() {
        return false;
    }
    let rfc1918 = a == 10 || (a == 172 && (16..=31).contains(&b)) || (a == 192 && b == 168);
    let relay_virtual = a == 10 && b == 77 && c < 32;
    rfc1918 && !relay_virtual
}

fn peer_mapping_allowed(public: SocketAddrV4, actual: SocketAddrV4) -> bool {
    public_ip_allowed(*public.ip())
        && public.port() != 0
        && actual.port() != 0
        && actual_private_ip_allowed(*actual.ip())
}

/// Parse the operator's opt-in map. Any malformed entry, duplicate public or
/// actual endpoint, or more than `MAX_PEER_MAP` entries rejects the whole map:
/// a partial map is harder to reason about than public-only behavior.
fn parse_peer_map(raw: &str) -> HashMap<SocketAddrV4, SocketAddrV4> {
    let mut map = HashMap::new();
    if raw.trim().is_empty() {
        return map;
    }
    let mut count = 0usize;
    for entry in raw.split(',') {
        count += 1;
        if count > MAX_PEER_MAP {
            return HashMap::new();
        }
        let Some((public, actual)) = entry.trim().split_once('=') else {
            return HashMap::new();
        };
        let (Ok(public), Ok(actual)) = (
            public.trim().parse::<SocketAddrV4>(),
            actual.trim().parse::<SocketAddrV4>(),
        ) else {
            return HashMap::new();
        };
        if !peer_mapping_allowed(public, actual)
            || map.contains_key(&public)
            || map.values().any(|existing| *existing == actual)
        {
            return HashMap::new();
        }
        map.insert(public, actual);
    }
    map
}

fn peer_map_from_env() -> HashMap<SocketAddrV4, SocketAddrV4> {
    match std::env::var("FO2_SPY_UDP_PEER_MAP") {
        Ok(raw) => parse_peer_map(&raw),
        Err(_) => HashMap::new(),
    }
}

fn worker(
    targets: [String; 4],
    peer_map: HashMap<SocketAddrV4, SocketAddrV4>,
    outbound: Receiver<Outbound>,
    inbound: SyncSender<Inbound>,
    public_target: Option<SocketAddrV4>,
    stop: Arc<AtomicBool>,
) {
    let socket = match UdpSocket::bind("0.0.0.0:0") {
        Ok(socket) => socket,
        Err(error) => { trace(format_args!("bind failed: {error}")); return; }
    };
    if socket.set_nonblocking(true).is_err() {
        return;
    }

    // Only endpoints actually sent to are allowed to reply. NAT endpoints
    // retain their guest-facing alias even though their real IP is different;
    // a mapped public peer also writes the advertised public port back.
    let mut contacted = HashMap::<SocketAddrV4, (u32, Option<u16>, Instant)>::new();
    let mut contact_order = VecDeque::new();
    let mut nat_addresses: [Option<SocketAddrV4>; 4] = [None, None, None, None];
    let mut buffer = [0u8; MAX_DATAGRAM];
    let mut pending: Option<Outbound> = None;
    let mut tokens = EGRESS_BYTES_PER_SECOND;
    let mut packet_tokens = EGRESS_PACKETS_PER_SECOND;
    let mut ingress_tokens = EGRESS_BYTES_PER_SECOND;
    let mut ingress_packet_tokens = EGRESS_PACKETS_PER_SECOND;
    let mut last_refill = Instant::now();
    let mut sent_traces = 0;
    loop {
        if stop.load(Ordering::Relaxed) { return; }
        let now = Instant::now();
        tokens = (tokens + now.duration_since(last_refill).as_secs_f64() * EGRESS_BYTES_PER_SECOND)
            .min(EGRESS_BYTES_PER_SECOND);
        packet_tokens = (packet_tokens + now.duration_since(last_refill).as_secs_f64() * EGRESS_PACKETS_PER_SECOND)
            .min(EGRESS_PACKETS_PER_SECOND);
        ingress_tokens = (ingress_tokens + now.duration_since(last_refill).as_secs_f64() * EGRESS_BYTES_PER_SECOND)
            .min(EGRESS_BYTES_PER_SECOND);
        ingress_packet_tokens = (ingress_packet_tokens + now.duration_since(last_refill).as_secs_f64() * EGRESS_PACKETS_PER_SECOND)
            .min(EGRESS_PACKETS_PER_SECOND);
        last_refill = now;
        for _ in 0..QUEUE_DEPTH {
            if pending.is_none() {
                match outbound.try_recv() {
                    Ok(packet) => pending = Some(packet),
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => return,
                }
            }
            let item = pending.as_ref().expect("queued datagram");
            if tokens < item.packet.len() as f64 || packet_tokens < 1.0 {
                break;
            }
            let (remote, guest_source, guest_port) = match item.target {
                Target::Public(original) => {
                    // An explicit mapping wins over the test's fallback target.
                    let mapped = peer_map.get(&original).copied();
                    let remote = mapped.or(public_target).unwrap_or(original);
                    (remote, u32::from(*original.ip()), mapped.map(|_| original.port()))
                }
                Target::Nat(index) => {
                    let address = nat_addresses[index].or_else(|| {
                        // DNS is deliberately confined to this worker, never Core.
                        let resolved = targets[index].to_socket_addrs().ok()?
                            .find_map(|addr| match addr {
                                SocketAddr::V4(addr) => Some(addr),
                                SocketAddr::V6(_) => None,
                            });
                        nat_addresses[index] = resolved;
                        resolved
                    });
                    let Some(remote) = address else {
                        pending = None;
                        continue;
                    };
                    let guest = match index {
                        0 => NAT1_VIP,
                        1 => NAT2_VIP,
                        _ => crate::VHOST_VIP,
                    };
                    (remote, guest, None)
                }
            };
            match socket.send_to(&item.packet, remote) {
                Ok(size) if size == item.packet.len() => {
                    if sent_traces < 3 {
                        trace(format_args!("sent {size} bytes from {:?} to {remote}", socket.local_addr()));
                        sent_traces += 1;
                    }
                    tokens -= size as f64;
                    packet_tokens -= 1.0;
                    if !contacted.contains_key(&remote) {
                        if contact_order.len() >= CONTACTS_MAX {
                            if let Some(old) = contact_order.pop_front() { contacted.remove(&old); }
                        }
                        contact_order.push_back(remote);
                    }
                    contacted.insert(remote, (guest_source, guest_port, now));
                    pending = None;
                }
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock || error.kind() == std::io::ErrorKind::Interrupted => break,
                result => { trace(format_args!("send to {remote} failed or short: {result:?}")); pending = None; },
            }
        }

        for _ in 0..QUEUE_DEPTH {
            match socket.recv_from(&mut buffer) {
                Ok((size, SocketAddr::V4(source))) => {
                    trace(format_args!("received {size} bytes from {source}, contacted={}", contacted.contains_key(&source)));
                    if let Some(&(guest_source, guest_port, sent_at)) = contacted.get(&source) {
                        if now.duration_since(sent_at) >= IDLE_TIMEOUT || ingress_tokens < size as f64 || ingress_packet_tokens < 1.0 {
                            continue;
                        }
                        ingress_tokens -= size as f64;
                        ingress_packet_tokens -= 1.0;
                        let reply_port = guest_port.unwrap_or(source.port());
                        match inbound.try_send((guest_source, reply_port, buffer[..size].to_vec())) {
                            Ok(()) | Err(mpsc::TrySendError::Full(_)) => {},
                            Err(mpsc::TrySendError::Disconnected(_)) => return,
                        }
                    }
                }
                Ok(_) => {},
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(_) => return,
            }
        }
        thread::sleep(POLL_INTERVAL);
    }
}

impl Core {
    pub fn spy_udp_send(
        &mut self,
        from: u32,
        sport: u16,
        dest: u32,
        dport: u16,
        cid: u32,
        payload: &[u8],
    ) -> bool {
        let target = match (dest, dport) {
            // note each distinct endpoint once: the join's UDP has no other trace
            (NAT1_VIP, NAT_PORT) if cid == 0 => Target::Nat(0),
            (NAT2_VIP, NAT_PORT) if cid == 0 => Target::Nat(1),
            (crate::VHOST_VIP, 27900) if cid == 0 => Target::Nat(2),
            (crate::VHOST_VIP, NAT_PORT) if cid == 0 => Target::Nat(3),
            (crate::VHOST_VIP, port) if cid != 0 && port != 0 && self.spy_udp.enabled => {
                let ip = Ipv4Addr::from(cid);
                if !public_ip_allowed(ip) {
                    return true;
                }
                Target::Public(SocketAddrV4::new(ip, dport))
            }
            _ => return false,
        };

        {
            let describe = match target {
                Target::Nat(i) => format!("nat{}", i + 1),
                Target::Public(a) => a.to_string(),
            };
            let seen = (dest, dport);
            if self.spy_udp.logged.insert(seen) {
                (self.log)(
                    &format!(
                        "spy-udp: {from:08x}:{sport} -> {dest:08x}:{dport} via {describe} ({} bytes) [{preview}]",
                        payload.len(),
                        preview = preview(payload)
                    ),
                    None,
                );
            }
        }

        let start_timer = {
            let spy = &mut self.spy_udp;
            // Disabled and oversized datagrams are deliberately dropped. Both
            // destinations remain claimed, so they cannot fall through to LAN.
            if !spy.enabled || payload.len() > MAX_DATAGRAM {
                return true;
            }

            let key = SessionKey { guest_vip: from, guest_port: sport };
            let now = Instant::now();
            spy.sessions
                .retain(|_, session| now.duration_since(session.last_activity) < IDLE_TIMEOUT);

            if let Some(session) = spy.sessions.get_mut(&key) {
                if session.outbound.try_send(Outbound { target, packet: payload.to_vec() }).is_ok() {
                    session.last_activity = now;
                } else {
                    trace(format_args!("outbound queue unavailable for {from:08x}:{sport}"));
                }
                return true;
            }

            if spy.sessions.len() >= MAX_SESSIONS
                || spy
                    .sessions
                    .keys()
                    .filter(|key| key.guest_vip == from)
                    .count()
                    >= MAX_SESSIONS_PER_VIP
            {
                trace(format_args!("session limit reached for {from:08x}:{sport}"));
                return true;
            }

            let (outbound_tx, outbound_rx) = mpsc::sync_channel(QUEUE_DEPTH);
            let (inbound_tx, inbound_rx) = mpsc::sync_channel(QUEUE_DEPTH);
            let targets = spy.targets.clone();
            let peer_map = spy.peer_map.clone();
            #[cfg(test)]
            let public_target = spy.public_target;
            #[cfg(not(test))]
            let public_target = None;
            let stop = Arc::new(AtomicBool::new(false));
            let worker_stop = stop.clone();
            if thread::Builder::new()
                .name(format!("spy-udp-{from:08x}-{sport}"))
                .spawn(move || worker(targets, peer_map, outbound_rx, inbound_tx, public_target, worker_stop))
                .is_err()
            {
                trace(format_args!("worker spawn failed for {from:08x}:{sport}"));
                return true;
            }

            let _ = outbound_tx.try_send(Outbound { target, packet: payload.to_vec() });
            spy.sessions.insert(
                key,
                Session {
                    outbound: outbound_tx,
                    inbound: inbound_rx,
                    last_activity: now,
                    stop,
                },
            );
            spy.timer.is_none()
        };
        if start_timer {
            let timer = self.set_timeout(10.0, Action::SpyUdpDrain);
            self.spy_udp.timer = Some(timer);
        }
        true
    }

    pub fn spy_udp_drain(&mut self) {
        let mut replies = Vec::new();
        let mut replies_logged = 0u32;
        let mut reply_lines: Vec<String> = Vec::new();
        {
            let spy = &mut self.spy_udp;
            spy.timer = None;
            let now = Instant::now();
            spy.sessions.retain(|key, session| {
                if now.duration_since(session.last_activity) >= IDLE_TIMEOUT {
                    return false;
                }
                for _ in 0..QUEUE_DEPTH {
                    match session.inbound.try_recv() {
                        Ok((source_ip, source_port, packet)) => {
                            session.last_activity = now;
                            replies_logged = replies_logged.saturating_add(1);
                            if reply_lines.len() < 16 {
                                reply_lines.push(format!(
                                    "spy-udp: reply {source_ip:08x}:{source_port} ({} bytes) [{}]",
                                    packet.len(),
                                    preview(&packet)
                                ));
                            }
                            replies.push((key.guest_vip, crate::vhost::lan_frame(
                                3,
                                source_ip,
                                key.guest_vip,
                                source_port,
                                key.guest_port,
                                0,
                                0,
                                &packet,
                            )));
                        }
                        Err(TryRecvError::Empty) => return true,
                        Err(TryRecvError::Disconnected) => return false,
                    }
                }
                true
            });
        }
        for (vip, frame) in replies {
            self.out(vip, frame);
        }
        for line in reply_lines {
            (self.log)(&line, None);
        }
        if replies_logged > 0 {
            static reported: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            let n = reported.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            if n < 8 {
                (self.log)(&format!("spy-udp: {replies_logged} reply packet(s) relayed to the game"), None);
            }
        }
        if !self.spy_udp.sessions.is_empty() {
            let timer = self.set_timeout(10.0, Action::SpyUdpDrain);
            self.spy_udp.timer = Some(timer);
        }
    }

    pub fn spy_udp_peer_gone(&mut self, vip: u32) {
        self.spy_udp.sessions
            .retain(|key, _| key.guest_vip != vip);
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::rc::Rc;
    use std::sync::Arc;

    use nfsu2_util::{FrameCtx, SendResult};
    use parking_lot::Mutex;

    use super::*;
    use crate::core::CoreOptions;
    use crate::vhost::lan_frame;
    use crate::Clocks;

    fn v4(endpoint: &str) -> SocketAddrV4 {
        endpoint.parse().expect("IPv4 endpoint")
    }

    #[test]
    fn peer_map_parses_bounded_operator_pairs() {
        let map = parse_peer_map("212.47.146.210:5571=192.168.100.30:23756");
        assert_eq!(map.len(), 1);
        let expected = v4("192.168.100.30:23756");
        assert_eq!(map.get(&v4("212.47.146.210:5571")), Some(&expected));

        let map = parse_peer_map(
            " 1.2.3.4:1000=10.1.2.3:2000, 5.6.7.8:3000=172.16.0.5:4000, \
             9.9.9.9:5000=10.77.32.1:6000 ",
        );
        assert_eq!(map.len(), 3);
        let expected = v4("10.77.32.1:6000");
        assert_eq!(map.get(&v4("9.9.9.9:5000")), Some(&expected));

        assert!(parse_peer_map("").is_empty());
        assert!(parse_peer_map("   ").is_empty());
    }

    #[test]
    fn peer_map_rejects_invalid_or_ambiguous_pairs() {
        for raw in [
            "garbage",
            "212.47.146.210=192.168.100.30:23756",
            "212.47.146.210:0=192.168.100.30:23756",
            "212.47.146.210:5571=192.168.100.30:0",
            "10.1.2.3:5571=192.168.100.30:23756",
            "127.0.0.1:5571=192.168.100.30:23756",
            "212.47.146.210:5571=127.0.0.1:23756",
            "212.47.146.210:5571=0.0.0.0:23756",
            "212.47.146.210:5571=224.0.0.1:23756",
            "212.47.146.210:5571=255.255.255.255:23756",
            "212.47.146.210:5571=8.8.8.8:23756",
            "212.47.146.210:5571=10.77.0.1:23756",
            "212.47.146.210:5571=10.77.31.255:23756",
            "212.47.146.210:5571=192.168.100.30:23756,",
            "212.47.146.210:5571=192.168.100.30:23756,\
             212.47.146.210:5571=192.168.100.31:23756",
            "212.47.146.210:5571=192.168.100.30:23756,\
             212.47.146.211:5571=192.168.100.30:23756",
        ] {
            assert!(parse_peer_map(raw).is_empty(), "accepted {raw:?}");
        }
    }

    #[test]
    fn peer_map_is_capped_at_sixteen_pairs() {
        let sixteen = (1..=16)
            .map(|i| format!("1.2.3.{i}:1000=10.0.0.{i}:2000"))
            .collect::<Vec<_>>()
            .join(",");
        assert_eq!(parse_peer_map(&sixteen).len(), 16);

        let seventeen = (1..=17)
            .map(|i| format!("1.2.3.{i}:1000=10.0.0.{i}:2000"))
            .collect::<Vec<_>>()
            .join(",");
        assert!(parse_peer_map(&seventeen).is_empty());
    }

    #[test]
    fn mapped_public_peer_replies_from_advertised_endpoint() {
        let actual = UdpSocket::bind("127.0.0.1:0").unwrap();
        actual.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let actual_addr = match actual.local_addr().unwrap() {
            SocketAddr::V4(addr) => addr,
            _ => unreachable!(),
        };
        let received = thread::spawn(move || {
            let mut buf = [0u8; 100];
            let (n, from) = actual.recv_from(&mut buf).unwrap();
            assert_eq!(&buf[..n], b"mapped join");
            actual.send_to(b"mapped reply", from).unwrap();
        });

        let decoy = UdpSocket::bind("127.0.0.1:0").unwrap();
        decoy.set_nonblocking(true).unwrap();
        let decoy_addr = match decoy.local_addr().unwrap() {
            SocketAddr::V4(addr) => addr,
            _ => unreachable!(),
        };

        let t = Rc::new(Cell::new(0.0));
        let (a, b) = (t.clone(), t.clone());
        let mut core = Core::new(CoreOptions {
            state_dir: None,
            log: Rc::new(|_, _| {}),
            tap: None,
            clocks: Rc::new(Clocks { now: Box::new(move || a.get()), timer: Box::new(move || b.get()) }),
            limits: Default::default(),
            buddy_limits: Default::default(),
            vhost: Default::default(),
            spy: vec![(28910, "unused:28910".into())],
        });
        let public_ip = Ipv4Addr::new(212, 47, 146, 210);
        let advertised = SocketAddrV4::new(public_ip, 5571);
        core.spy_udp.peer_map.insert(advertised, actual_addr);
        core.spy_udp.public_target = Some(decoy_addr);

        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let out = sent.clone();
        core.attach(Arc::new(move |_, frame, _| { out.lock().push(frame.to_vec()); SendResult::Sent }));

        let vip = 0x0a4d_0002;
        let guest_port = 50123;
        assert!(core.spy_udp_send(
            vip,
            guest_port,
            crate::VHOST_VIP,
            advertised.port(),
            u32::from(public_ip),
            b"mapped join",
        ));
        assert_eq!(core.spy_udp.sessions.len(), 1);

        for _ in 0..100 {
            core.spy_udp_drain();
            if !sent.lock().is_empty() { break; }
            thread::sleep(Duration::from_millis(5));
        }
        received.join().unwrap();
        let frames = sent.lock();
        assert_eq!(frames.len(), 1);
        let frame = &frames[0];
        assert_eq!(&frame[4..8], &u32::from(public_ip).to_be_bytes());
        assert_eq!(&frame[8..12], &vip.to_be_bytes());
        assert_eq!(&frame[12..14], &advertised.port().to_be_bytes());
        assert_eq!(&frame[14..16], &guest_port.to_be_bytes());
        assert_eq!(&frame[24..], b"mapped reply");
        drop(frames);

        let mut buf = [0u8; 64];
        assert!(matches!(
            decoy.recv_from(&mut buf),
            Err(ref error) if error.kind() == std::io::ErrorKind::WouldBlock
        ));
    }

    #[test]
    fn natneg_round_trip_and_peer_cleanup() {
        let server = UdpSocket::bind("127.0.0.1:0").unwrap();
        server.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let remote = server.local_addr().unwrap();
        let received = thread::spawn(move || {
            let mut buf = [0u8; 100];
            let (n, from) = server.recv_from(&mut buf).unwrap();
            assert_eq!(&buf[..n], b"nat challenge");
            server.send_to(b"nat answer", from).unwrap();
        });
        let t = Rc::new(Cell::new(0.0));
        let (a, b) = (t.clone(), t.clone());
        let mut core = Core::new(CoreOptions {
            state_dir: None,
            log: Rc::new(|_, _| {}),
            tap: None,
            clocks: Rc::new(Clocks { now: Box::new(move || a.get()), timer: Box::new(move || b.get()) }),
            limits: Default::default(),
            buddy_limits: Default::default(),
            vhost: Default::default(),
            spy: vec![(28910, "unused:28910".into())],
        });
        core.spy_udp.targets[0] = remote.to_string();
        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let out = sent.clone();
        core.attach(Arc::new(move |_, frame, _| { out.lock().push(frame.to_vec()); SendResult::Sent }));
        let vip = 0x0a4d_0002;
        core.frame(vip, &lan_frame(3, vip, NAT1_VIP, 50123, NAT_PORT, 0, 0, b"nat challenge"),
                   &FrameCtx { group: "test".into(), site: None, cause: None, slot: None });
        assert_eq!(core.spy_udp.sessions.len(), 1);
        for _ in 0..100 {
            t.set(t.get() + 20.0);
            core.run_due(t.get());
            if !sent.lock().is_empty() { break }
            thread::sleep(Duration::from_millis(5));
        }
        received.join().unwrap();
        let frames = sent.lock();
        assert_eq!(frames.len(), 1);
        let frame = &frames[0];
        assert_eq!(frame[0], 3);
        assert_eq!(&frame[4..8], &NAT1_VIP.to_be_bytes());
        assert_eq!(&frame[8..12], &vip.to_be_bytes());
        assert_eq!(&frame[12..14], &remote.port().to_be_bytes());
        assert_eq!(&frame[14..16], &50123u16.to_be_bytes());
        assert_eq!(&frame[24..], b"nat answer");
        drop(frames);
        core.peer_gone(vip);
        assert!(core.spy_udp.sessions.is_empty());
    }

    #[test]
    fn nat_aliases_and_public_peer_share_one_guest_socket() {
        let nat1 = UdpSocket::bind("127.0.0.1:0").unwrap();
        let nat2 = UdpSocket::bind("127.0.0.1:0").unwrap();
        let public = UdpSocket::bind("127.0.0.1:0").unwrap();
        for socket in [&nat1, &nat2, &public] {
            socket.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        }

        let t = Rc::new(Cell::new(0.0));
        let (a, b) = (t.clone(), t.clone());
        let mut core = Core::new(CoreOptions {
            state_dir: None,
            log: Rc::new(|_, _| {}),
            tap: None,
            clocks: Rc::new(Clocks { now: Box::new(move || a.get()), timer: Box::new(move || b.get()) }),
            limits: Default::default(),
            buddy_limits: Default::default(),
            vhost: Default::default(),
            spy: vec![(28910, "unused:28910".into())],
        });
        let n1 = nat1.local_addr().unwrap().to_string();
        let n2 = nat2.local_addr().unwrap().to_string();
        core.spy_udp.targets = [n1.clone(), n2.clone(), n1, n2];
        core.spy_udp.peer_map.clear();
        core.spy_udp.public_target = Some(match public.local_addr().unwrap() {
            SocketAddr::V4(addr) => addr,
            _ => unreachable!(),
        });
        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let out = sent.clone();
        core.attach(Arc::new(move |_, frame, _| { out.lock().push(frame.to_vec()); SendResult::Sent }));

        let vip = 0x0a4d_0002;
        let port = 50123;
        let public_ip = 0x0102_0304;
        assert!(core.spy_udp_send(vip, port, NAT1_VIP, NAT_PORT, 0, b"one"));
        assert!(core.spy_udp_send(vip, port, NAT2_VIP, NAT_PORT, 0, b"two"));
        core.frame(vip, &lan_frame(3, vip, crate::VHOST_VIP, port, 12345, 0, public_ip, b"three"),
                   &FrameCtx { group: "test".into(), site: None, cause: None, slot: None });
        assert_eq!(core.spy_udp.sessions.len(), 1);

        let mut ports = Vec::new();
        for (socket, expected) in [(&nat1, b"one".as_slice()), (&nat2, b"two".as_slice()), (&public, b"three".as_slice())] {
            let mut buf = [0u8; MAX_DATAGRAM];
            let (size, source) = socket.recv_from(&mut buf).unwrap();
            assert_eq!(&buf[..size], expected);
            ports.push(source.port());
            socket.send_to(expected, source).unwrap();
        }
        assert_eq!(ports[0], ports[1]);
        assert_eq!(ports[1], ports[2]);
        let stranger = UdpSocket::bind("127.0.0.1:0").unwrap();
        stranger.send_to(b"unsolicited", ("127.0.0.1", ports[0])).unwrap();

        for _ in 0..100 {
            core.spy_udp_drain();
            if sent.lock().len() == 3 { break; }
            thread::sleep(Duration::from_millis(5));
        }
        let frames = sent.lock();
        assert_eq!(frames.len(), 3);
        assert!(frames.iter().all(|frame| &frame[24..] != b"unsolicited"));
        for (payload, source_ip, source_port) in [
            (b"one".as_slice(), NAT1_VIP, nat1.local_addr().unwrap().port()),
            (b"two".as_slice(), NAT2_VIP, nat2.local_addr().unwrap().port()),
            (b"three".as_slice(), public_ip, public.local_addr().unwrap().port()),
        ] {
            let frame = frames.iter().find(|frame| &frame[24..] == payload).unwrap();
            assert_eq!(&frame[4..8], &source_ip.to_be_bytes());
            assert_eq!(&frame[12..14], &source_port.to_be_bytes());
            assert_eq!(&frame[14..16], &port.to_be_bytes());
        }
        drop(frames);
        assert!(!core.spy_udp_send(vip, port, crate::VHOST_VIP, 12345, 0, b"ordinary vhost datagram"));
        for private in [0x7f00_0001, 0x0a00_0001, 0xc0a8_0101, 0xa9fe_0101, 0xe000_0001] {
            assert!(core.spy_udp_send(vip, port, crate::VHOST_VIP, 12345, private, b"blocked"));
        }
        assert_eq!(core.spy_udp.sessions.len(), 1, "invalid destinations do not open sockets");
    }
}
