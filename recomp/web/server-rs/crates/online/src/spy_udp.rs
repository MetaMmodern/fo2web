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
    targets: [String; 2],
    #[cfg(test)]
    public_target: Option<SocketAddrV4>,
    sessions: HashMap<SessionKey, Session>,
    timer: Option<TimerId>,
}

impl SpyUdp {
    pub fn new(enabled: bool) -> Self {
        Self {
            enabled,
            targets: ["natneg1.openspy.net:27901".into(), "natneg2.openspy.net:27901".into()],
            #[cfg(test)]
            public_target: None,
            sessions: HashMap::new(),
            timer: None,
        }
    }
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

fn worker(
    targets: [String; 2],
    outbound: Receiver<Outbound>,
    inbound: SyncSender<Inbound>,
    public_target: Option<SocketAddrV4>,
    stop: Arc<AtomicBool>,
) {
    let Ok(socket) = UdpSocket::bind("0.0.0.0:0") else {
        return;
    };
    if socket.set_nonblocking(true).is_err() {
        return;
    }

    // Only endpoints actually sent to are allowed to reply. NAT endpoints
    // retain their guest-facing alias even though their real IP is different.
    let mut contacted = HashMap::<SocketAddrV4, (u32, Instant)>::new();
    let mut contact_order = VecDeque::new();
    let mut nat_addresses: [Option<SocketAddrV4>; 2] = [None, None];
    let mut buffer = [0u8; MAX_DATAGRAM];
    let mut pending: Option<Outbound> = None;
    let mut tokens = EGRESS_BYTES_PER_SECOND;
    let mut packet_tokens = EGRESS_PACKETS_PER_SECOND;
    let mut ingress_tokens = EGRESS_BYTES_PER_SECOND;
    let mut ingress_packet_tokens = EGRESS_PACKETS_PER_SECOND;
    let mut last_refill = Instant::now();
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
            let (remote, guest_source) = match item.target {
                Target::Public(original) => {
                    let remote = public_target.unwrap_or(original);
                    (remote, u32::from(*original.ip()))
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
                    (remote, if index == 0 { NAT1_VIP } else { NAT2_VIP })
                }
            };
            match socket.send_to(&item.packet, remote) {
                Ok(size) if size == item.packet.len() => {
                    tokens -= size as f64;
                    packet_tokens -= 1.0;
                    if !contacted.contains_key(&remote) {
                        if contact_order.len() >= CONTACTS_MAX {
                            if let Some(old) = contact_order.pop_front() { contacted.remove(&old); }
                        }
                        contact_order.push_back(remote);
                    }
                    contacted.insert(remote, (guest_source, now));
                    pending = None;
                }
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock || error.kind() == std::io::ErrorKind::Interrupted => break,
                _ => pending = None,
            }
        }

        for _ in 0..QUEUE_DEPTH {
            match socket.recv_from(&mut buffer) {
                Ok((size, SocketAddr::V4(source))) => {
                    if let Some(&(guest_source, sent_at)) = contacted.get(&source) {
                        if now.duration_since(sent_at) >= IDLE_TIMEOUT || ingress_tokens < size as f64 || ingress_packet_tokens < 1.0 {
                            continue;
                        }
                        ingress_tokens -= size as f64;
                        ingress_packet_tokens -= 1.0;
                        match inbound.try_send((guest_source, source.port(), buffer[..size].to_vec())) {
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
            (NAT1_VIP, NAT_PORT) if cid == 0 => Target::Nat(0),
            (NAT2_VIP, NAT_PORT) if cid == 0 => Target::Nat(1),
            (crate::VHOST_VIP, port) if cid != 0 && port != 0 && self.spy_udp.enabled => {
                let ip = Ipv4Addr::from(cid);
                if !public_ip_allowed(ip) {
                    return true;
                }
                Target::Public(SocketAddrV4::new(ip, dport))
            }
            _ => return false,
        };

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
                return true;
            }

            let (outbound_tx, outbound_rx) = mpsc::sync_channel(QUEUE_DEPTH);
            let (inbound_tx, inbound_rx) = mpsc::sync_channel(QUEUE_DEPTH);
            let targets = spy.targets.clone();
            #[cfg(test)]
            let public_target = spy.public_target;
            #[cfg(not(test))]
            let public_target = None;
            let stop = Arc::new(AtomicBool::new(false));
            let worker_stop = stop.clone();
            if thread::Builder::new()
                .name(format!("spy-udp-{from:08x}-{sport}"))
                .spawn(move || worker(targets, outbound_rx, inbound_tx, public_target, worker_stop))
                .is_err()
            {
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
        core.spy_udp.targets = [nat1.local_addr().unwrap().to_string(), nat2.local_addr().unwrap().to_string()];
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
