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
/// A client of a hosted session is shown to the guest under this relay VIP with
/// a per-client port.  The guest cannot send to arbitrary private LAN peers
/// (the page drops those), so the relay translates the replies back.
pub const HOSTED_PEER_VIP: u32 = 0x0a4d1ff0;

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
        && COUNT.fetch_add(1, Ordering::Relaxed) < 128
    {
        eprintln!("spy-udp-trace: {args}");
    }
}

/// Hosting diagnostics keep their own small budget: the general trace is
/// consumed by the master and peer chatter before a player joins.
fn hosted_diag(args: std::fmt::Arguments<'_>) {
    static COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    if std::env::var_os("FO2_SPY_UDP_TRACE").is_some()
        && COUNT.fetch_add(1, Ordering::Relaxed) < 24
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
    /// A reply to a hosted session's client, by its synthetic relay port.
    HostedClient(u16),
    /// A reply to a hosted session's client by the address it joined from.
    HostedAddr(SocketAddrV4),
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
    /// A guest that is hosting a session (its QR status carried `localip0`).
    /// Shared across the guest's sockets: the game opens a separate one for its
    /// NAT-negotiation library, so `hosting` cannot live in a single worker.
    hosting: HashMap<u32, Arc<AtomicBool>>,
    /// The worker sockets' port range (`low-high`); None: bind ephemeral, as before.
    udp_ports: Option<(u16, u16)>,
    /// (destination, port) pairs already reported, so the log has one line each
    logged: std::collections::HashSet<(u32, u16)>,
    timer: Option<TimerId>,
}

impl SpyUdp {
    pub fn new(enabled: bool, udp_ports: Option<(u16, u16)>) -> Self {
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
            hosting: HashMap::new(),
            udp_ports,
            logged: std::collections::HashSet::new(),
            timer: None,
        }
    }

    /// The worker's externally reachable UDP socket.  When a range is set, bind the first free port of it so the
    /// firewall can allow exactly that range; the port is freed when the worker drops the socket.  None: every port
    /// of the range is taken.
    fn bind_udp_socket(&self) -> Option<UdpSocket> {
        let socket = match self.udp_ports {
            None => UdpSocket::bind("0.0.0.0:0").ok(),
            Some((low, high)) => (low..=high).find_map(|port| {
                UdpSocket::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port)).ok()
            }),
        };
        let socket = socket?;
        socket.set_nonblocking(true).ok()?;
        Some(socket)
    }
}

/// a datagram's first bytes, escaped, for the relay's bounded trace
fn preview_n(p: &[u8], max: usize) -> String {
    let mut s = String::new();
    for &b in p.iter().take(max) {
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
    if p.len() > max { s.push('…'); }
    s
}
fn preview(p: &[u8]) -> String { preview_n(p, 48) }

/// The host's LAN address, where a LAN player can reach a hosted session
/// without the router hairpinning the public endpoint.  FO2_SPY_LAN_IP wins;
/// otherwise the routing table's primary address (no packet is sent).
fn lan_ip() -> Option<Ipv4Addr> {
    if let Ok(v) = std::env::var("FO2_SPY_LAN_IP") {
        if let Ok(ip) = v.trim().parse::<Ipv4Addr>() {
            if !ip.is_unspecified() {
                return Some(ip);
            }
        }
    }
    let probe = UdpSocket::bind("0.0.0.0:0").ok()?;
    probe.connect("192.0.2.1:9").ok()?;
    match probe.local_addr().ok()? {
        SocketAddr::V4(a) if !a.ip().is_unspecified() && !a.ip().is_loopback() => Some(*a.ip()),
        _ => None,
    }
}

fn find_sub(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || hay.len() < needle.len() {
        return None;
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

/// Replace the NUL-terminated value of a GameSpy `\key\value` field.
fn replace_nul_field(packet: &[u8], key: &[u8], value: &[u8]) -> Vec<u8> {
    let mut needle = key.to_vec();
    needle.push(0);
    let Some(pos) = find_sub(packet, &needle) else {
        return packet.to_vec();
    };
    let start = pos + needle.len();
    let Some(end) = packet[start..].iter().position(|&b| b == 0).map(|n| start + n) else {
        return packet.to_vec();
    };
    let mut out = Vec::with_capacity(packet.len() - (end - start) + value.len());
    out.extend_from_slice(&packet[..start]);
    out.extend_from_slice(value);
    out.extend_from_slice(&packet[end..]);
    out
}

/// What a hosted session's QR status advertises: its own virtual address is
/// meaningless to a real player, so point the LAN fields at the relay's LAN
/// listener.  The public fields stay as they are (the master fills those).
fn rewrite_qr(packet: &[u8], lan: Option<(Ipv4Addr, u16)>) -> Vec<u8> {
    let Some((ip, port)) = lan else {
        return packet.to_vec();
    };
    if find_sub(packet, b"localip0\0").is_none() {
        return packet.to_vec();
    }
    let ip = ip.to_string();
    let port = port.to_string();
    let out = replace_nul_field(packet, b"localip0", ip.as_bytes());
    replace_nul_field(&out, b"localport", port.as_bytes())
}

/// The IPv4 a FlatOut join packet reports as the player's own address, or None.
/// The packet is 0x6b length-prefixed: cookie(4) at offset 2, "FO14" at 10,
/// and the player's reported IPv4 at 28. Used only to recognise that one player
/// reached a hosted session by two paths (its LAN address and its public NAT).
fn join_reported_ip(payload: &[u8]) -> Option<Ipv4Addr> {
    if payload.len() < 32 || payload[0] != 0x6b || &payload[10..14] != b"FO14" {
        return None;
    }
    Some(Ipv4Addr::new(payload[28], payload[29], payload[30], payload[31]))
}

/// Rewrite the address a joining player reports as its own to the relay
/// identity the host is shown (`vip:syn`).  Only the reported IPv4 (`28..32`)
/// and port (`44..46`) are touched: the trailing datachecksum covers game
/// data, not these fields.  Anything that is not a `0x6b` join is unchanged.
fn rewrite_join(packet: &[u8], vip: u32, syn: u16) -> Vec<u8> {
    if packet.len() < 46 || packet[0] != 0x6b || &packet[10..14] != b"FO14" {
        return packet.to_vec();
    }
    let mut out = packet.to_vec();
    out[28..32].copy_from_slice(&vip.to_be_bytes());
    out[44..46].copy_from_slice(&syn.to_be_bytes());
    out
}

/// The endpoint a hosted session's reply to a player's *reported* address must
/// reach.  The host answers the address the player joined from: the player's
/// own view of itself, carried in its join packet and often its private LAN
/// address (e.g. `192.168.100.30`).  The host cannot reach that directly, so
/// the relay translates it back to where the player really is — a public peer
/// that reported a private address, or a player on the relay's own LAN.  The
/// returned bool marks replies that must leave from the LAN listener.
fn reported_endpoint(
    addr: SocketAddrV4,
    public_by_ip: &HashMap<Ipv4Addr, SocketAddrV4>,
    lan_by_ip: &HashMap<Ipv4Addr, SocketAddrV4>,
) -> Option<(SocketAddrV4, bool)> {
    if let Some(&ep) = public_by_ip.get(&addr.ip()) {
        return Some((ep, false));
    }
    if let Some(&ep) = lan_by_ip.get(&addr.ip()) {
        return Some((ep, true));
    }
    None
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

/// One client of a session this browser hosts.  A private (LAN) client is
/// presented to the host under one relay identity, `HOSTED_PEER_VIP:<syn>`,
/// and every leg (join content, packet source, host replies) uses this record.
struct HostedClient {
    /// Where the client really is: the endpoint its datagrams arrive from.
    endpoint: SocketAddrV4,
    /// When it was last seen; the mapping expires after `IDLE_TIMEOUT`.
    seen: Instant,
    /// Replies must leave from the LAN listener instead of the main socket.
    via_lan: bool,
}

/// Give a client the next free synthetic port; the range is small and reused.
fn allocate_syn(hosted_clients: &HashMap<u16, HostedClient>, next_syn: &mut u16) -> u16 {
    while hosted_clients.contains_key(next_syn) {
        *next_syn = if *next_syn >= 49999 { 40000 } else { *next_syn + 1 };
    }
    let syn = *next_syn;
    *next_syn = if *next_syn >= 49999 { 40000 } else { *next_syn + 1 };
    syn
}

/// Drop mappings for clients that have been silent for `IDLE_TIMEOUT`, and
/// the reverse lookup they owned.
fn expire_hosted_clients(
    hosted_clients: &mut HashMap<u16, HostedClient>,
    client_by_endpoint: &mut HashMap<SocketAddrV4, u16>,
    now: Instant,
) {
    hosted_clients.retain(|_, client| {
        let live = now.duration_since(client.seen) < IDLE_TIMEOUT;
        if !live {
            client_by_endpoint.remove(&client.endpoint);
        }
        live
    });
}

/// Adopt an unfamiliar endpoint as a client of the hosted session under its
/// own synthetic port.  Bounded by `CONTACTS_MAX`; idle entries are expired
/// first.
fn register_client(
    hosted_clients: &mut HashMap<u16, HostedClient>,
    client_by_endpoint: &mut HashMap<SocketAddrV4, u16>,
    next_syn: &mut u16,
    endpoint: SocketAddrV4,
    via_lan: bool,
    now: Instant,
) -> Option<u16> {
    expire_hosted_clients(hosted_clients, client_by_endpoint, now);
    if hosted_clients.len() >= CONTACTS_MAX {
        return None;
    }
    let syn = allocate_syn(hosted_clients, next_syn);
    hosted_clients.insert(
        syn,
        HostedClient { endpoint, seen: now, via_lan },
    );
    client_by_endpoint.insert(endpoint, syn);
    Some(syn)
}

/// Deliver a hosted client's datagram to the game as if it came from its relay
/// identity, rewriting the join's reported address to that identity.  False
/// when the session's inbound channel is gone.
fn deliver_hosted(
    inbound: &SyncSender<Inbound>,
    ingress_tokens: &mut f64,
    ingress_packet_tokens: &mut f64,
    syn: u16,
    packet: &[u8],
) -> bool {
    if *ingress_tokens < packet.len() as f64 || *ingress_packet_tokens < 1.0 {
        return true;
    }
    *ingress_tokens -= packet.len() as f64;
    *ingress_packet_tokens -= 1.0;
    let packet = rewrite_join(packet, HOSTED_PEER_VIP, syn);
    match inbound.try_send((HOSTED_PEER_VIP, syn, packet)) {
        Ok(()) | Err(mpsc::TrySendError::Full(_)) => true,
        Err(mpsc::TrySendError::Disconnected(_)) => false,
    }
}

fn worker(
    socket: UdpSocket,
    targets: [String; 4],
    peer_map: HashMap<SocketAddrV4, SocketAddrV4>,
    outbound: Receiver<Outbound>,
    inbound: SyncSender<Inbound>,
    public_target: Option<SocketAddrV4>,
    stop: Arc<AtomicBool>,
    hosting: Arc<AtomicBool>,
) {
    // `socket` is bound (to a range port, or ephemeral) and non-blocking already.

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
    // A hosted session is a server: other players connect to it without the
    // host having contacted them first.  The QR status response carrying
    // `localip0` marks this guest as such a session, shared across its sockets
    // (the game's natneg library has its own).
    let mut hosted_clients = HashMap::<u16, HostedClient>::new();
    let mut client_by_endpoint = HashMap::<SocketAddrV4, u16>::new();
    let mut next_syn: u16 = 40000;
    // The relay's LAN address and a listener on it: a player on the same LAN
    // reaches the hosted session here, without the router hairpinning.
    let mut lan: Option<(Ipv4Addr, u16, UdpSocket)> = None;
    // A player that reached us by two paths (its public NAT address, and the
    // LAN listener) is matched by the IPv4 it reports in its join packet:
    // public endpoint -> reported IPv4, and reported IPv4 -> LAN endpoint.
    let mut public_reported = HashMap::<SocketAddrV4, Ipv4Addr>::new();
    let mut lan_by_ip = HashMap::<Ipv4Addr, SocketAddrV4>::new();
    // A public peer's reported private address -> its real endpoint, so the
    // host's reply to the reported address finds the player.
    let mut public_by_ip = HashMap::<Ipv4Addr, SocketAddrV4>::new();
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
            if !hosting.load(Ordering::Relaxed) && item.packet.windows(8).any(|w| w == b"localip0") {
                hosting.store(true, Ordering::Relaxed);
                trace(format_args!("hosting session: unsolicited players accepted"));
                // Advertise the relay's LAN address too, so a player on this
                // LAN connects here instead of the (unreachable) public one.
                if let Some(ip) = lan_ip() {
                    if let Ok(sock) = UdpSocket::bind(SocketAddrV4::new(ip, 0)) {
                        if sock.set_nonblocking(true).is_ok() {
                            if let Ok(SocketAddr::V4(addr)) = sock.local_addr() {
                                trace(format_args!("hosting LAN listener: {addr}"));
                                lan = Some((ip, addr.port(), sock));
                            }
                        }
                    }
                }
                if lan.is_none() {
                    trace(format_args!("hosting: no LAN address, LAN players cannot connect"));
                }
            }
            if tokens < item.packet.len() as f64 || packet_tokens < 1.0 {
                break;
            }
            let (remote, guest_source, guest_port, via_lan) = match item.target {
                Target::Public(original) => {
                    // An explicit mapping wins over the test's fallback target.
                    let mapped = peer_map.get(&original).copied();
                    // If this public endpoint is a player that also reached us
                    // on the LAN listener (same reported address), reply over
                    // the LAN: the public address is the shared router's WAN,
                    // which the relay cannot hairpin back to a LAN player.
                    if mapped.is_none() {
                        if let Some(rep) = public_reported.get(&original) {
                            if let Some(&lan_ep) = lan_by_ip.get(rep) {
                                for client in hosted_clients.values_mut() {
                                    if client.endpoint == lan_ep {
                                        client.seen = now;
                                    }
                                }
                                hosted_diag(format_args!(
                                    "redirecting host reply to {original} over the LAN listener to {lan_ep}"
                                ));
                                (lan_ep, u32::from(*original.ip()), None, true)
                            } else {
                                let remote = public_target.unwrap_or(original);
                                (remote, u32::from(*original.ip()), None, false)
                            }
                        } else {
                            let remote = public_target.unwrap_or(original);
                            (remote, u32::from(*original.ip()), None, false)
                        }
                    } else {
                        let remote = mapped.unwrap_or(original);
                        (remote, u32::from(*original.ip()), Some(original.port()), false)
                    }
                }
                Target::HostedClient(syn) => {
                    let Some(client) = hosted_clients.get_mut(&syn) else {
                        hosted_diag(format_args!("no client mapping for relay port {syn}"));
                        pending = None;
                        continue;
                    };
                    client.seen = now;
                    hosted_diag(format_args!(
                        "host reply {} bytes -> client {} (relay port {syn}, lan={})",
                        item.packet.len(),
                        client.endpoint,
                        client.via_lan
                    ));
                    (client.endpoint, HOSTED_PEER_VIP, Some(syn), client.via_lan)
                }
                Target::HostedAddr(addr) => {
                    // The game answered the address the player joined from
                    // (its own reported address, often its LAN one).  A known
                    // client keeps its one relay identity; otherwise translate
                    // the reported address back to the real endpoint.
                    let syn = client_by_endpoint.get(&addr).copied();
                    if let Some(syn) = syn {
                        let Some(client) = hosted_clients.get_mut(&syn) else {
                            pending = None;
                            continue;
                        };
                        client.seen = now;
                        hosted_diag(format_args!(
                            "host reply {} bytes -> client {addr} by address (relay port {syn}, lan={})",
                            item.packet.len(),
                            client.via_lan
                        ));
                        (client.endpoint, HOSTED_PEER_VIP, Some(syn), client.via_lan)
                    } else if let Some((ep, via_lan)) = reported_endpoint(addr, &public_by_ip, &lan_by_ip) {
                        if let Some(&syn) = client_by_endpoint.get(&ep) {
                            let Some(client) = hosted_clients.get_mut(&syn) else {
                                pending = None;
                                continue;
                            };
                            client.seen = now;
                            hosted_diag(format_args!(
                                "host reply {} bytes -> client {ep} by its reported {addr} (relay port {syn})",
                                item.packet.len()
                            ));
                            (client.endpoint, HOSTED_PEER_VIP, Some(syn), client.via_lan)
                        } else {
                            // No client record: the player reached us from a
                            // different address than it reported. Translate.
                            hosted_diag(format_args!(
                                "host reply {} bytes -> client {ep} by its reported {addr} (lan={via_lan})",
                                item.packet.len()
                            ));
                            (ep, u32::from(*ep.ip()), None, via_lan)
                        }
                    } else {
                        hosted_diag(format_args!("host reply to unknown player {addr} dropped"));
                        pending = None;
                        continue;
                    }
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
                    (remote, guest, None, false)
                }
            };
            // A hosted session's QR status is rewritten to point LAN players
            // at the relay's LAN listener.
            let rewritten = if item.packet.windows(8).any(|w| w == b"localip0") {
                rewrite_qr(&item.packet, lan.as_ref().map(|l| (l.0, l.1)))
            } else {
                item.packet.clone()
            };
            let tx = if via_lan { lan.as_ref().map(|l| &l.2) } else { None }.unwrap_or(&socket);
            match tx.send_to(&rewritten, remote) {
                Ok(size) if size == rewritten.len() => {
                    if sent_traces < 3 {
                        trace(format_args!("sent {size} bytes from {:?} to {remote}", tx.local_addr()));
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
                    let known = contacted.contains_key(&source);
                    // Record the player's reported address for every packet:
                    // the NAT-negotiation "connect" may have marked this public
                    // source as contacted before the join arrives, so we cannot
                    // rely on the !known branch alone.
                    if let Some(rep) = join_reported_ip(&buffer[..size]) {
                        public_reported.insert(source, rep);
                        public_by_ip.insert(rep, source);
                    }
                    trace(format_args!("received {size} bytes from {source}, contacted={known}"));
                    // Other players connect to a hosted session without it
                    // having contacted them.  A public peer is named by the
                    // address its own NAT negotiation reported — the host's
                    // natneg through OpenSpy already knows it — so it is
                    // presented under that public address and its replies are
                    // translated back.  Only a private (LAN) peer, which the
                    // page cannot address, is shown under a relay identity
                    // `HOSTED_PEER_VIP:<syn>`.
                    if hosting.load(Ordering::Relaxed) && !known {
                        if public_ip_allowed(*source.ip()) {
                            if contact_order.len() >= CONTACTS_MAX {
                                if let Some(old) = contact_order.pop_front() { contacted.remove(&old); }
                            }
                            contact_order.push_back(source);
                            contacted.insert(source, (u32::from(*source.ip()), None, now));
                            hosted_diag(format_args!("public peer {source} accepted for the hosted session"));
                        } else {
                            let Some(syn) = register_client(
                                &mut hosted_clients,
                                &mut client_by_endpoint,
                                &mut next_syn,
                                source,
                                false,
                                now,
                            ) else {
                                continue;
                            };
                            if contact_order.len() >= CONTACTS_MAX {
                                if let Some(old) = contact_order.pop_front() { contacted.remove(&old); }
                            }
                            contact_order.push_back(source);
                            contacted.insert(source, (HOSTED_PEER_VIP, Some(syn), now));
                            hosted_diag(format_args!(
                                "hosted client {source} -> relay {HOSTED_PEER_VIP:08x}:{syn}"
                            ));
                            if !deliver_hosted(
                                &inbound,
                                &mut ingress_tokens,
                                &mut ingress_packet_tokens,
                                syn,
                                &buffer[..size],
                            ) {
                                return;
                            }
                            continue;
                        }
                    }
                    // A known client keeps its relay identity even though a
                    // reply to its reported address may have refreshed
                    // `contacted` with its real one.
                    if hosting.load(Ordering::Relaxed) {
                        if let Some(&syn) = client_by_endpoint.get(&source) {
                            if let Some(client) = hosted_clients.get_mut(&syn) {
                                client.seen = now;
                            }
                            if !deliver_hosted(
                                &inbound,
                                &mut ingress_tokens,
                                &mut ingress_packet_tokens,
                                syn,
                                &buffer[..size],
                            ) {
                                return;
                            }
                            continue;
                        }
                    }
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
        // Players on the relay's own LAN reach the hosted session here.
        if let Some(lsock) = lan.as_ref().map(|l| &l.2) {
            for _ in 0..QUEUE_DEPTH {
                match lsock.recv_from(&mut buffer) {
                    Ok((size, SocketAddr::V4(source))) => {
                        let known = contacted.contains_key(&source);
                        // Record the reported address for every LAN-listener
                        // packet, not just the first from this client.
                        if let Some(rep) = join_reported_ip(&buffer[..size]) {
                            lan_by_ip.insert(rep, source);
                        }
                        trace(format_args!(
                            "received {size} bytes from {source} on the LAN listener, contacted={known}"
                        ));
                        // A player that already reached us by the other path
                        // keeps its identity; arriving here just means its
                        // replies must leave from the LAN listener.
                        if let Some(&syn) = client_by_endpoint.get(&source) {
                            if let Some(client) = hosted_clients.get_mut(&syn) {
                                client.seen = now;
                                client.via_lan = true;
                            }
                            if !deliver_hosted(
                                &inbound,
                                &mut ingress_tokens,
                                &mut ingress_packet_tokens,
                                syn,
                                &buffer[..size],
                            ) {
                                return;
                            }
                            continue;
                        }
                        if !known {
                            let Some(syn) = register_client(
                                &mut hosted_clients,
                                &mut client_by_endpoint,
                                &mut next_syn,
                                source,
                                true,
                                now,
                            ) else {
                                continue;
                            };
                            if contact_order.len() >= CONTACTS_MAX {
                                if let Some(old) = contact_order.pop_front() { contacted.remove(&old); }
                            }
                            contact_order.push_back(source);
                            contacted.insert(source, (HOSTED_PEER_VIP, Some(syn), now));
                            hosted_diag(format_args!(
                                "LAN player {source} -> relay {HOSTED_PEER_VIP:08x}:{syn}"
                            ));
                            if !deliver_hosted(
                                &inbound,
                                &mut ingress_tokens,
                                &mut ingress_packet_tokens,
                                syn,
                                &buffer[..size],
                            ) {
                                return;
                            }
                            continue;
                        }
                        if let Some(&(guest_source, guest_port, sent_at)) = contacted.get(&source) {
                            if now.duration_since(sent_at) >= IDLE_TIMEOUT
                                || ingress_tokens < size as f64
                                || ingress_packet_tokens < 1.0
                            {
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
                    Err(_) => break,
                }
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
        // A hosted session replying to a client the relay presented to it.
        // The reply may leave from a different local port than the one that
        // registered the session, so offer it to every session of this guest:
        // only the worker that holds this client mapping sends it.
        if dest == HOSTED_PEER_VIP && cid == 0 {
            let mut delivered = false;
            for (key, session) in self.spy_udp.sessions.iter_mut() {
                if key.guest_vip != from {
                    continue;
                }
                if session
                    .outbound
                    .try_send(Outbound {
                        target: Target::HostedClient(dport),
                        packet: payload.to_vec(),
                    })
                    .is_ok()
                {
                    delivered = true;
                }
            }
            hosted_diag(format_args!(
                "reply from {from:08x}:{sport} to relay {HOSTED_PEER_VIP:08x}:{dport} \
                 ({} bytes), delivered={delivered}",
                payload.len()
            ));
            return true;
        }
        let target = match (dest, dport) {
            // note each distinct endpoint once: the join's UDP has no other trace
            (NAT1_VIP, NAT_PORT) if cid == 0 => Target::Nat(0),
            (NAT2_VIP, NAT_PORT) if cid == 0 => Target::Nat(1),
            (crate::VHOST_VIP, 27900) if cid == 0 => Target::Nat(2),
            (crate::VHOST_VIP, NAT_PORT) if cid == 0 => Target::Nat(3),
            (crate::VHOST_VIP, port) if cid != 0 && port != 0 && self.spy_udp.enabled => {
                let ip = Ipv4Addr::from(cid);
                if public_ip_allowed(ip) {
                    Target::Public(SocketAddrV4::new(ip, dport))
                } else if self.spy_udp.sessions.keys().any(|k| k.guest_vip == from) {
                    // A hosted player may have joined from a private address;
                    // the worker sends it only if that address really joined.
                    Target::HostedAddr(SocketAddrV4::new(ip, dport))
                } else {
                    return true;
                }
            }
            _ => return false,
        };

        {
            let describe = match target {
                Target::Nat(i) => format!("nat{}", i + 1),
                Target::Public(a) => a.to_string(),
                Target::HostedClient(syn) => format!("client:{syn}"),
                Target::HostedAddr(addr) => format!("client:{addr}"),
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
            // Hosting: the QR status response is what the master publishes, so
            // its localip0/localport0/publicip/publicport fields decide whether
            // another player can reach this session. Preview a few of them.
            if dest == crate::VHOST_VIP && dport == 27900 && payload.len() >= 100 {
                static QR_TRACES: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                if QR_TRACES.fetch_add(1, Ordering::Relaxed) < 4 {
                    trace(format_args!(
                        "qr status from {from:08x}:{sport} ({} bytes) [{}]",
                        payload.len(),
                        preview_n(payload, 400)
                    ));
                }
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
            let hosting = spy.hosting.entry(from).or_insert_with(|| Arc::new(AtomicBool::new(false))).clone();
            let socket = match spy.bind_udp_socket() {
                Some(socket) => socket,
                None => {
                    // every port of the range is taken: drop the datagram rather than
                    // open an unbounded socket outside the firewall's allowance
                    trace(format_args!("spy-udp: no port free in the range for {from:08x}:{sport}"));
                    return true;
                }
            };
            if thread::Builder::new()
                .name(format!("spy-udp-{from:08x}-{sport}"))
                .spawn(move || worker(socket, targets, peer_map, outbound_rx, inbound_tx, public_target, worker_stop, hosting))
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
        self.spy_udp.hosting.remove(&vip);
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
    fn udp_port_range_allocation_binding_exhaustion_and_reuse() {
        // A free port, found by binding and dropping an ephemeral socket.
        let probe = UdpSocket::bind("0.0.0.0:0").unwrap();
        let port = probe.local_addr().unwrap().port();
        drop(probe);

        // A single-port range: the first bind takes it, the next finds none.
        let spy = SpyUdp::new(true, Some((port, port)));
        let first = spy.bind_udp_socket().expect("the port is free");
        assert_eq!(first.local_addr().unwrap().port(), port);
        assert!(spy.bind_udp_socket().is_none(), "the range is exhausted");

        // Dropping the socket frees the port for reuse.
        drop(first);
        let again = spy.bind_udp_socket().expect("the port is free again");
        assert_eq!(again.local_addr().unwrap().port(), port);
        drop(again);

        // Without a range the socket binds ephemerally (a nonzero port).
        let ephemeral = SpyUdp::new(true, None);
        let sock = ephemeral.bind_udp_socket().expect("ephemeral bind");
        assert_ne!(sock.local_addr().unwrap().port(), 0);
    }

    fn test_core(t: &Rc<Cell<f64>>) -> Core {
        let (a, b) = (t.clone(), t.clone());
        Core::new(CoreOptions {
            state_dir: None,
            log: Rc::new(|_, _| {}),
            tap: None,
            clocks: Rc::new(Clocks { now: Box::new(move || a.get()), timer: Box::new(move || b.get()) }),
            limits: Default::default(),
            buddy_limits: Default::default(),
            vhost: Default::default(),
            spy: vec![(28910, "unused:28910".into())],
            spy_udp_ports: None,
        })
    }

    /// Wait for the worker to relay a frame matching `predicate` (removing it
    /// from the collected output), or fail the test.
    fn wait_frame(
        core: &mut Core,
        sent: &Arc<Mutex<Vec<Vec<u8>>>>,
        mut predicate: impl FnMut(&[u8]) -> bool,
    ) -> Vec<u8> {
        for _ in 0..400 {
            core.spy_udp_drain();
            {
                let mut frames = sent.lock();
                if let Some(index) = frames.iter().position(|frame| predicate(frame)) {
                    return frames.remove(index);
                }
            }
            thread::sleep(Duration::from_millis(5));
        }
        panic!("expected a relayed frame");
    }

    #[test]
    fn qr_status_advertises_the_lan_listener() {
        let qr: &[u8] =
            b"\x03\x01localip0\x0010.77.0.2\x00localport\x0023756\x00gamename\x00flatout2pc\x00";
        let out = rewrite_qr(qr, Some((Ipv4Addr::new(192, 168, 100, 50), 41234)));
        assert_eq!(
            out,
            b"\x03\x01localip0\x00192.168.100.50\x00localport\x0041234\x00gamename\x00flatout2pc\x00"
        );
        assert_eq!(rewrite_qr(qr, None), qr);
        // A packet without the field is left alone.
        let other = b"\x09\x00\x00\x00\x00flatout2pc\x00";
        assert_eq!(rewrite_qr(other, Some((Ipv4Addr::new(192, 168, 100, 50), 41234))), other);
    }

    #[test]
    fn reported_endpoint_maps_private_addresses_back_to_the_peer() {
        let public = v4("84.53.216.57:8250");
        let lan = v4("192.168.100.30:23756");
        let mut public_by_ip = HashMap::new();
        public_by_ip.insert(Ipv4Addr::new(192, 168, 100, 30), public);
        let mut lan_by_ip = HashMap::new();
        lan_by_ip.insert(Ipv4Addr::new(192, 168, 100, 30), lan);

        // A public peer that reported a private address: answered at the public
        // endpoint, not via the LAN listener.
        let addr = v4("192.168.100.30:23756");
        assert_eq!(reported_endpoint(addr, &public_by_ip, &lan_by_ip), Some((public, false)));

        // Without a public mapping, the LAN listener endpoint wins.
        public_by_ip.clear();
        assert_eq!(reported_endpoint(addr, &public_by_ip, &lan_by_ip), Some((lan, true)));

        // An address nobody reported is unknown.
        lan_by_ip.clear();
        assert_eq!(reported_endpoint(addr, &public_by_ip, &lan_by_ip), None);
    }

    #[test]
    fn join_reported_ip_parses_the_players_address() {
        // A real 109-byte join packet from the capture: 0x6b length, cookie at
        // 2, "FO14" at 10, the player's reported 192.168.100.30 at 28.
        let mut p = vec![0u8; 109];
        p[0] = 0x6b;
        p[2..6].copy_from_slice(b"S3\xdf7");
        p[10..14].copy_from_slice(b"FO14");
        p[28..32].copy_from_slice(&[192, 168, 100, 30]);
        assert_eq!(join_reported_ip(&p), Some(Ipv4Addr::new(192, 168, 100, 30)));
        assert_eq!(join_reported_ip(&p[..20]), None);          // too short
        let mut bad = p.clone();
        bad[10..14].copy_from_slice(b"XXYY");
        assert_eq!(join_reported_ip(&bad), None);              // not a join packet
    }

    #[test]
    fn rewrite_join_only_touches_the_reported_address() {
        // A real 109-byte join: 0x6b length, cookie at 2, "FO14" at 10, the
        // player's reported 192.168.100.30 at 28, port 23756 (0x5ccc) at 44.
        let mut p = vec![0u8; 109];
        p[0] = 0x6b;
        p[2..6].copy_from_slice(&[0x53, 0x33, 0xdf, 0x37]);
        p[10..14].copy_from_slice(b"FO14");
        p[28..32].copy_from_slice(&[192, 168, 100, 30]);
        p[44..46].copy_from_slice(&0x5cccu16.to_be_bytes());
        for (i, b) in p[62..].iter_mut().enumerate() {
            *b = i as u8; // the datachecksum tail
        }

        let out = rewrite_join(&p, HOSTED_PEER_VIP, 40001);
        assert_eq!(out.len(), p.len());
        for i in 0..p.len() {
            if (28..32).contains(&i) || (44..46).contains(&i) {
                continue;
            }
            assert_eq!(out[i], p[i], "byte {i} changed");
        }
        assert_eq!(&out[28..32], &HOSTED_PEER_VIP.to_be_bytes());
        assert_eq!(&out[44..46], &40001u16.to_be_bytes());
        assert_eq!(&out[62..], &p[62..]);

        // Anything that is not a join is returned untouched.
        assert_eq!(rewrite_join(b"not a join", HOSTED_PEER_VIP, 40001), b"not a join");
        assert_eq!(rewrite_join(&p[..40], HOSTED_PEER_VIP, 40001), &p[..40]);
        let mut other = p.clone();
        other[10..14].copy_from_slice(b"XXYY");
        assert_eq!(rewrite_join(&other, HOSTED_PEER_VIP, 40001), other);
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
            spy_udp_ports: None,
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
    fn hosted_lan_client_is_presented_at_the_relay_vip() {
        // A player on the relay's own LAN connects to a hosted session.  The
        // page cannot send to a private address, so the relay must present the
        // client at HOSTED_PEER_VIP:<syn> and translate the host's reply back.
        let client = UdpSocket::bind("127.0.0.1:0").unwrap();
        client.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        // Stands in for the OpenSpy master: it receives the QR status and thus
        // learns the worker's address, which it hands to the test client.
        let master = UdpSocket::bind("127.0.0.1:0").unwrap();
        master.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
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
            spy_udp_ports: None,
        });
        core.spy_udp.targets[2] = master.local_addr().unwrap().to_string();
        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let out = sent.clone();
        core.attach(Arc::new(move |_, frame, _| { out.lock().push(frame.to_vec()); SendResult::Sent }));

        let vip = 0x0a4d_0002;
        let guest_port = 23756;
        // The QR status response marks the session as hosting and gives the
        // test master (and so the client) the worker's address.
        assert!(core.spy_udp_send(
            vip, guest_port, crate::VHOST_VIP, 27900, 0,
            b"\x03\xe4localip0\x0010.77.1.2\x00localport\x0023756\x00",
        ));
        let mut buf = [0u8; 256];
        let (n, worker) = master.recv_from(&mut buf).unwrap();
        assert!(n >= 8);

        // The client joins; the host must receive it from the relay VIP.
        client.send_to(b"join", worker).unwrap();
        let mut frame = None;
        for _ in 0..200 {
            core.spy_udp_drain();
            if let Some(f) = sent.lock().pop() { frame = Some(f); break; }
            thread::sleep(Duration::from_millis(5));
        }
        let frame = frame.expect("host receives the client's join");
        let syn = u16::from_be_bytes([frame[12], frame[13]]);
        assert_eq!(&frame[4..8], &HOSTED_PEER_VIP.to_be_bytes());
        assert_eq!(&frame[8..12], &vip.to_be_bytes());
        assert!(syn >= 40000);
        assert_eq!(u16::from_be_bytes([frame[14], frame[15]]), guest_port);
        assert_eq!(&frame[24..], b"join");

        // The host replies to the relay VIP; the real client must get it.
        assert!(core.spy_udp_send(vip, guest_port, HOSTED_PEER_VIP, syn, 0, b"reply"));
        let (n, from) = client.recv_from(&mut buf).unwrap();
        assert_eq!(from, worker);
        assert_eq!(&buf[..n], b"reply");

        // The game may instead answer the address the player joined from
        // (its own reported LAN address). That must reach the same client.
        let client_addr = match client.local_addr().unwrap() {
            SocketAddr::V4(addr) => addr,
            _ => unreachable!(),
        };
        assert!(core.spy_udp_send(
            vip, guest_port, crate::VHOST_VIP, client_addr.port(), u32::from(*client_addr.ip()),
            b"by-address",
        ));
        let (n, from) = client.recv_from(&mut buf).unwrap();
        assert_eq!(from, worker);
        assert_eq!(&buf[..n], b"by-address");
    }

    #[test]
    fn host_natneg_init_is_forwarded_to_openspy() {
        // A hosted session's natneg still reaches OpenSpy: both the host and
        // the guest negotiate there, and OpenSpy matches them.  The relay
        // rewrites the addresses the game then uses; it does not terminate
        // the negotiation (which would leave the guest with no peer).
        let natneg = UdpSocket::bind("127.0.0.1:0").unwrap();
        natneg.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let master = UdpSocket::bind("127.0.0.1:0").unwrap();
        master.set_read_timeout(Some(Duration::from_secs(2))).unwrap();

        let t = Rc::new(Cell::new(0.0));
        let mut core = test_core(&t);
        core.spy_udp.targets[0] = natneg.local_addr().unwrap().to_string();
        core.spy_udp.targets[2] = master.local_addr().unwrap().to_string();
        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let out = sent.clone();
        core.attach(Arc::new(move |_, frame, _| { out.lock().push(frame.to_vec()); SendResult::Sent }));

        let vip = 0x0a4d_0002;
        let guest_port = 23756;
        assert!(core.spy_udp_send(
            vip, guest_port, crate::VHOST_VIP, 27900, 0,
            b"\x03\xe4localip0\x0010.77.1.2\x00localport\x0023756\x00",
        ));
        let mut buf = [0u8; 256];
        let (n, _worker) = master.recv_from(&mut buf).unwrap();
        assert!(n >= 8);

        // The host's own NAT-negotiation INIT, observed in the captures, is
        // forwarded to the real natneg host.
        let init: &[u8] = b"\xfd\xfc\x1e\x66\x6a\xb2\x02\x00\x0c\x5b\x1b\xb9\x00\x01\x01\x0a\x4d\x01\x02\x00\x00flatout2pc\x00";
        assert!(core.spy_udp_send(vip, guest_port, NAT1_VIP, NAT_PORT, 0, init));
        let (n, _) = natneg.recv_from(&mut buf).unwrap();
        assert_eq!(&buf[..n], init);
    }

    #[test]
    fn hosted_guest_keeps_one_relay_identity() {
        // A private (LAN) guest of a hosted session is one syn: the join's
        // reported address and the packet source both use that identity, and
        // the host's reply to it reaches the real client.
        let client = UdpSocket::bind("127.0.0.1:0").unwrap();
        client.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let master = UdpSocket::bind("127.0.0.1:0").unwrap();
        master.set_read_timeout(Some(Duration::from_secs(2))).unwrap();

        let t = Rc::new(Cell::new(0.0));
        let mut core = test_core(&t);
        core.spy_udp.targets[2] = master.local_addr().unwrap().to_string();
        let sent = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let out = sent.clone();
        core.attach(Arc::new(move |_, frame, _| { out.lock().push(frame.to_vec()); SendResult::Sent }));

        let vip = 0x0a4d_0002;
        let guest_port = 23756;
        assert!(core.spy_udp_send(
            vip, guest_port, crate::VHOST_VIP, 27900, 0,
            b"\x03\xe4localip0\x0010.77.1.2\x00localport\x0023756\x00",
        ));
        let mut buf = [0u8; 1024];
        let (_, worker) = master.recv_from(&mut buf).unwrap();

        // A real 109-byte join from the guest.
        let mut join = vec![0u8; 109];
        join[0] = 0x6b;
        join[2..6].copy_from_slice(&0x53_33_df_37u32.to_be_bytes());
        join[10..14].copy_from_slice(b"FO14");
        join[28..32].copy_from_slice(&[192, 168, 100, 30]);
        join[44..46].copy_from_slice(&0x5cccu16.to_be_bytes());
        client.send_to(&join, worker).unwrap();

        let frame = wait_frame(&mut core, &sent, |frame| frame[24] == 0x6b);
        let syn = u16::from_be_bytes([frame[12], frame[13]]);
        assert!(syn >= 40000);
        assert_eq!(&frame[4..8], &HOSTED_PEER_VIP.to_be_bytes());
        assert_eq!(&frame[8..12], &vip.to_be_bytes());
        assert_eq!(u16::from_be_bytes([frame[14], frame[15]]), guest_port);
        // The join the host sees reports the relay identity, nothing else
        // changed (the datachecksum tail included).
        let payload = &frame[24..];
        assert_eq!(&payload[28..32], &HOSTED_PEER_VIP.to_be_bytes());
        assert_eq!(&payload[44..46], &syn.to_be_bytes());
        assert_eq!(&payload[..28], &join[..28]);
        assert_eq!(&payload[32..44], &join[32..44]);
        assert_eq!(&payload[46..], &join[46..]);

        // A second datagram keeps the same synthetic port.
        client.send_to(b"race", worker).unwrap();
        let frame = wait_frame(&mut core, &sent, |frame| &frame[24..] == b"race");
        assert_eq!(&frame[4..8], &HOSTED_PEER_VIP.to_be_bytes());
        assert_eq!(u16::from_be_bytes([frame[12], frame[13]]), syn);

        // The host's reply to the relay identity reaches the real client.
        assert!(core.spy_udp_send(vip, guest_port, HOSTED_PEER_VIP, syn, 0, b"reply"));
        let (n, from) = client.recv_from(&mut buf).unwrap();
        assert_eq!(from, worker);
        assert_eq!(&buf[..n], b"reply");
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
            spy_udp_ports: None,
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
            spy_udp_ports: None,
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
