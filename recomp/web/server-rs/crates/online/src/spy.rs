//! The OpenSpy relay (`--spy-relay`): ports of the virtual host whose game-side bytes go to a real OpenSpy server
//! on the internet, for the parts of the game's Online mode this server does not stand in for.  The service is a
//! pipe in both directions and nothing more: what the game sends goes upstream, what the upstream sends comes
//! back; nothing of it is read here (the rest of this crate speaks the lobby's protocol instead).
//!
//! One relayed connection has its pump on a short-lived OS thread of its own: the host's thread never waits for
//! the internet.  The pump resolves the name (3 s at most: a resolver that never answers used to leave the game's
//! search half-open for the whole session) and connects (5 s at most; a failed connect RSTs the game's
//! connection, its line once per host:port).  A connection that is not connected by ACK_DEADLINE is refused by the
//! drain, so the game's wait for our SYNACK always ends in a definite answer.  Then the pump reads and writes the
//! one stream nonblocking, in TCP_CHUNK
//! pieces.  The ends are mapped: the upstream's close is the game's FIN, the game's FIN a shutdown of the
//! upstream's write side (the upstream sends what it still has, then closes, as GameSpy's servers do).
//!
//! The two sides meet in queues: the game's bytes wait for the pump (64 chunks; a queue that runs over ends the
//! connection: the game reconnects, as it did when EA's servers were slow), the upstream's for the host's thread,
//! which takes them on the relay's drain timer (10 ms, while a relayed connection is open; timers.rs
//! `Action::SpyDrain`) and writes them into the connections (`conn_write`, so the vhost's bytes_out counts them).
//! There are no per-byte lines.

use std::collections::{HashMap, HashSet};
use std::io::ErrorKind::{Interrupted, WouldBlock};
use std::io::{Read, Write};
use std::net::{Shutdown, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::mpsc::{Receiver, SyncSender, TryRecvError, TrySendError, sync_channel};
use std::time::{Duration, Instant};

use crate::core::Core;
use crate::timers::{Action, TimerId};
use crate::vhost::{ConnId, Svc, SYNACK, TCP_CHUNK, lan_frame};
use crate::VHOST_VIP;

/// the pump's connect, at most (a dead OpenSpy: the game's connection goes, it does not hang)
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
/// name resolution, at most: `to_socket_addrs` has no timeout of its own, and a resolver that never answers used to
/// leave the pump (and the game's half-open connection) waiting for the rest of the session
const RESOLVE_TIMEOUT: Duration = Duration::from_secs(3);
/// the game's own wait for our SYNACK (net_lan.c SPY_CONNECT_TMO, 6 s). Report the failure ourselves first, so the
/// game takes its "cannot connect" path instead of being left mid-search.
const ACK_DEADLINE: Duration = Duration::from_secs(5);
/// the queue from the host's thread to a pump, in chunks of TCP_CHUNK (the GameSpy traffic is messages of a few
/// hundred bytes)
const QUEUE: usize = 64;
/// the pump's idle wait before it looks at both directions again (its two directions on the one nonblocking
/// stream, so the host's thread is never in its loop)
const IDLE: Duration = Duration::from_millis(2);
/// how often the host's thread takes what the upstreams sent, in ms of the timer clock, while a relayed
/// connection is open
const SPY_EVERY: f64 = 10.0;

/// the relay's table: the port the game opens, the upstream's "host:port"
pub type SpyTable = Vec<(u16, String)>;

/// the relay's own state: its table, the lines it has logged once, its drain timer
pub struct Spy {
    table: HashMap<u16, String>,
    /// the ports "no upstream" was logged for (once per port)
    unknown: HashSet<u16>,
    /// the upstreams "connect failed" was logged for (once per host:port)
    failed: HashSet<String>,
    timer: Option<TimerId>,
}

impl Spy {
    pub fn new(table: SpyTable) -> Spy {
        Spy { table: table.into_iter().collect(), unknown: HashSet::new(), failed: HashSet::new(), timer: None }
    }

    /// whether the relay is on (a table was given)
    pub fn on(&self) -> bool {
        !self.table.is_empty()
    }
}

/// one relayed connection: the queues between the host's thread and its pump
pub struct SpyConn {
    /// the game's bytes upstream, and its FIN; a full queue ends the connection (`spy_data`)
    up: SyncSender<Up>,
    /// the upstream's bytes, its failed connect and its close, taken by the drain (`spy_drain`)
    down: Receiver<Down>,
    /// the game's FIN, handed to the pump once its queue has room (a full queue at the FIN's own first try)
    fin: bool,
    /// Do not tell the game its TCP connect succeeded until the internet side is connected.
    pub ready: bool,
    /// `ready` must happen by then: after it the connection is refused (RST) rather than left half-open
    deadline: Instant,
    /// the upstream "host:port" this connection was opened for (the line names it)
    to: String,
}

/// what the host's thread hands a pump
enum Up {
    /// the game's bytes
    Data(Vec<u8>),
    /// the game closed its side: the upstream's write side goes, its answers still come
    Fin,
}

/// what a pump hands the host's thread
enum Down {
    Connected,
    /// the upstream's bytes
    Data(Vec<u8>),
    /// the upstream (or its name) refused us: the game's connection goes, the line once per host:port
    Failed(String, String),
    /// the upstream closed its side: the game's FIN
    Closed,
}

impl Core {
    /// a connection to a relayed port: its pump on a thread of its own, the drain timer if this is the first
    pub fn spy_connect(&mut self, c: ConnId) {
        let up = self.vh.conns.get(&c).and_then(|k| self.spy.table.get(&k.lport)).cloned();
        let Some(up) = up else {
            // the table makes the listeners, so a listener without its entry cannot be SYNed to; if it ever is,
            // the connection goes as one to a port with no listener does
            return self.conn_destroy(c, "spy-relay: no upstream for its port");
        };
        let to = up.clone();
        let (up_tx, up_rx) = sync_channel::<Up>(QUEUE);
        let (down_tx, down_rx) = sync_channel::<Down>(QUEUE);
        match std::thread::Builder::new().name("spy-relay".into()).spawn(move || pump(up_rx, down_tx, up)) {
            Ok(_) => {
                self.vh.spys.insert(c, SpyConn {
                    up: up_tx,
                    down: down_rx,
                    fin: false,
                    ready: false,
                    to,
                    deadline: Instant::now() + ACK_DEADLINE,
                });
                self.set_svc(c, Svc::Spy);
            }
            // no thread for it (out of memory): the game's connection goes, as for a failed connect
            Err(_) => return self.conn_destroy(c, "spy-relay: no thread for the upstream"),
        }
        if self.spy.timer.is_none() {
            self.spy.timer = Some(self.set_timeout(SPY_EVERY, Action::SpyDrain));
        }
    }

    /// the game's bytes upstream; a full queue: the game outsends what the upstream takes, the connection goes
    pub fn spy_data(&mut self, c: ConnId, chunk: &[u8]) {
        let sent = self.vh.spys.get(&c).map(|s| s.up.try_send(Up::Data(chunk.to_vec())));
        match sent {
            Some(Ok(())) | None | Some(Err(TrySendError::Disconnected(_))) => {}
            // the pump is gone: the upstream's close is on its way to the game as a FIN
            Some(Err(TrySendError::Full(_))) => self.conn_destroy(c, "spy-relay: the game outsent the upstream"),
        }
    }

    /// the game closed its side: the pump shuts the upstream's write side; the game's own FIN waits for the
    /// upstream's close
    pub fn spy_end(&mut self, c: ConnId) {
        if let Some(s) = self.vh.spys.get_mut(&c) {
            s.fin = true;
        }
    }

    /// a SYN to a port with no listener, the relay on: one line per port, once, so an operator sees which other
    /// ports the game asks for (the refusal itself is the vhost's)
    pub fn spy_no_upstream(&mut self, port: u16) {
        if self.spy.on() && self.spy.unknown.insert(port) {
            (self.log)(&format!("spy-relay: no upstream for port {port}"), None);
        }
    }

    /// what the upstreams sent since the last drain: into the game's connections; a pump's end is the upstream's
    /// close (the game's FIN) or, with its word of why, a failed connect (the game's RST)
    pub fn spy_drain(&mut self) {
        self.spy.timer = None;
        // taken first, all of it: the writes and the closes borrow the host's whole state again
        let mut got: Vec<(ConnId, Vec<Down>)> = Vec::new();
        let mut late: Vec<ConnId> = Vec::new();
        let now = Instant::now();
        for (id, s) in self.vh.spys.iter_mut() {
            if !s.ready && now >= s.deadline {
                late.push(*id);
                continue;
            }
            // a FIN the pump's queue had no room for, tried again here
            if s.fin && s.up.try_send(Up::Fin).is_ok() {
                s.fin = false;
            }
            let mut msgs = Vec::new();
            loop {
                match s.down.try_recv() {
                    Ok(m) => msgs.push(m),
                    Err(TryRecvError::Empty) => break,
                    // the pump is gone: the upstream closed (or the connection's own end took it)
                    Err(TryRecvError::Disconnected) => {
                        msgs.push(Down::Closed);
                        break;
                    }
                }
            }
            if !msgs.is_empty() {
                got.push((*id, msgs));
            }
        }
        for id in late {
            self.conn_destroy(id, "spy-relay: the upstream did not connect in time");
        }
        for (id, msgs) in got {
            for m in msgs {
                match m {
                    Down::Connected => {
                        if let Some(s) = self.vh.spys.get_mut(&id) {
                            s.ready = true;
                            // the one line that says the relay reached OpenSpy: search cannot work without it
                            (self.log)(&format!("spy-relay: {} connected (SYNACK to the game)", s.to), None);
                        }
                        if let Some(conn) = self.vh.conns.get(&id) {
                            self.out(conn.vip, lan_frame(4, VHOST_VIP, conn.vip, conn.lport, conn.rport, SYNACK, conn.cid, &[]));
                        }
                    }
                    Down::Data(d) => {
                        self.conn_write(id, &d);
                    }
                    Down::Failed(to, err) => {
                        if self.spy.failed.insert(to.clone()) {
                            (self.log)(&format!("spy-relay: {to} connect failed: {err}"), None);
                        }
                        self.conn_destroy(id, "spy-relay: the upstream refused the connection");
                    }
                    Down::Closed => self.conn_end(id),
                }
            }
        }
        // the next drain, while a relayed connection is open
        if !self.vh.spys.is_empty() {
            self.spy.timer = Some(self.set_timeout(SPY_EVERY, Action::SpyDrain));
        }
    }
}

/// one connection's pump: the game's bytes upstream, the upstream's bytes back, the ends mapped (the module's
/// text).  The name is resolved and the connect made here, so the host's thread waits for neither.  A down queue
/// that would not take (the game's side gone) or an upstream that broke ends the pump: the host hears the queue's
/// end as the upstream's close.
fn pump(up: Receiver<Up>, down: SyncSender<Down>, to: String) {
    let mut s = match connect(&to) {
        Ok(s) => s,
        Err(e) => {
            let _ = down.try_send(Down::Failed(to, e));
            return;
        }
    };
    // the pump never blocks: the one stream is read and written nonblocking, both directions served in turn
    if s.set_nonblocking(true).is_err() {
        let _ = down.try_send(Down::Failed(to, "the stream would not go nonblocking".into()));
        return;
    }
    // no Nagle: the GameSpy servers' answers are small, and a held-back answer is the game's lag
    let _ = s.set_nodelay(true);
    if down.try_send(Down::Connected).is_err() {
        return;
    }
    let mut buf = [0u8; TCP_CHUNK];
    let mut pending: Vec<u8> = Vec::new();
    let mut fin = false;
    loop {
        let mut busy = false;
        // the upstream's bytes to the game; its close (a read of 0) or a failure ends the pump
        match s.read(&mut buf) {
            Ok(0) => return,
            Ok(n) => {
                if down.try_send(Down::Data(buf[..n].to_vec())).is_err() {
                    return;
                }
                busy = true;
            }
            Err(e) if e.kind() == WouldBlock || e.kind() == Interrupted => {}
            Err(_) => return,
        }
        // the game's next bytes, and its FIN (after it, the upstream's answers still come, its writes do not)
        if pending.is_empty() {
            match up.try_recv() {
                Ok(Up::Data(d)) if !fin => {
                    pending = d;
                    busy = true;
                }
                Ok(Up::Data(_)) => {}
                Ok(Up::Fin) => {
                    let _ = s.shutdown(Shutdown::Write);
                    fin = true;
                    busy = true;
                }
                Err(TryRecvError::Disconnected) => return,
                Err(TryRecvError::Empty) => {}
            }
        }
        // the game's bytes upstream, as much of the chunk as the upstream takes now
        while !pending.is_empty() {
            match s.write(&pending) {
                Ok(0) => return,
                Ok(n) => {
                    pending.drain(..n);
                    busy = true;
                }
                Err(e) if e.kind() == WouldBlock || e.kind() == Interrupted => break,
                Err(_) => return,
            }
        }
        if !busy {
            std::thread::sleep(IDLE);
        }
    }
}

/// the upstream's stream, or why not: try all resolved addresses within one connect deadline.
/// A name can advertise IPv6 before IPv4 even when this host can reach only IPv4.
fn connect(to: &str) -> Result<TcpStream, String> {
    let mut addresses = resolve(to)?;
    if addresses.is_empty() {
        return Err("no address for its name".into());
    }
    addresses.sort_by_key(|addr| !addr.is_ipv4());
    let start = Instant::now();
    let mut last = String::new();
    for addr in addresses {
        let Some(remaining) = CONNECT_TIMEOUT.checked_sub(start.elapsed()) else { break };
        if remaining.is_zero() { break }
        match TcpStream::connect_timeout(&addr, remaining) {
            Ok(stream) => return Ok(stream),
            Err(e) => last = e.to_string(),
        }
    }
    Err(if last.is_empty() { "connect timed out".into() } else { last })
}

/// the upstream's addresses, with the resolver bounded: `to_socket_addrs` may block without end, and the pump is the
/// only thing that can tell the game the connect failed.
fn resolve(to: &str) -> Result<Vec<SocketAddr>, String> {
    let (tx, rx) = std::sync::mpsc::channel();
    let name = to.to_string();
    std::thread::Builder::new()
        .name("spy-dns".into())
        .spawn(move || {
            let _ = tx.send(name.to_socket_addrs().map(|i| i.collect::<Vec<_>>()).map_err(|e| e.to_string()));
        })
        .map_err(|e| e.to_string())?;
    match rx.recv_timeout(RESOLVE_TIMEOUT) {
        Ok(r) => r,
        Err(_) => Err(format!("name resolution took longer than {} s", RESOLVE_TIMEOUT.as_secs())),
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::rc::Rc;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::time::Duration;

    use nfsu2_util::{FrameCtx, SendResult};
    use parking_lot::Mutex;

    use crate::core::{Core, CoreOptions};
    use crate::vhost::{DATA, FIN, RST, SYN, SYNACK, lan_frame};
    use crate::{Clocks, VHOST_VIP};

    /// one of the host's frames out, as far as the relay's tests read it
    struct F {
        op: u8,
        cid: u32,
        data: Vec<u8>,
    }

    fn parse(f: &[u8]) -> F {
        F { op: f[16], cid: u32::from_le_bytes([f[20], f[21], f[22], f[23]]), data: f[24..].to_vec() }
    }

    fn ip(s: &str) -> u32 {
        s.split('.').fold(0u32, |a, b| (a << 8) | b.parse::<u32>().unwrap())
    }

    /// a host with the relay's table, its frames out kept, its lines kept
    fn host(table: super::SpyTable) -> (Core, Rc<Cell<f64>>, Arc<Mutex<Vec<F>>>, Rc<std::cell::RefCell<Vec<String>>>) {
        let t = Rc::new(Cell::new(0.0));
        let (t2, t3) = (t.clone(), t.clone());
        let lines = Rc::new(std::cell::RefCell::new(Vec::new()));
        let l2 = lines.clone();
        let mut core = Core::new(CoreOptions {
            state_dir: None,
            log: Rc::new(move |s: &str, _: Option<&str>| l2.borrow_mut().push(s.to_string())),
            tap: None,
            clocks: Rc::new(Clocks { now: Box::new(move || t2.get()), timer: Box::new(move || t3.get()) }),
            limits: Default::default(),
            buddy_limits: Default::default(),
            vhost: Default::default(),
            spy: table,
            spy_udp_ports: None,
        });
        let out: Arc<Mutex<Vec<F>>> = Arc::default();
        let o2 = out.clone();
        core.attach(Arc::new(move |_, f, _| {
            o2.lock().push(parse(&f));
            SendResult::Sent
        }));
        (core, t, out, lines)
    }

    fn frame(h: &mut Core, from: u32, sport: u16, op: u8, dport: u16, cid: u32, data: &[u8]) {
        h.frame(from, &lan_frame(4, from, VHOST_VIP, sport, dport, op, cid, data), &FrameCtx { group: format!("vip {from}").into(), site: None, cause: None, slot: None });
    }

    /// the timer clock moves and what is due runs, as real time would (the relay's drain among the timers), until
    /// `f` holds or a moment has passed: what the pumps do happens on their own threads, in real time
    fn drain_until(h: &mut Core, t: &Cell<f64>, f: impl Fn(&Core) -> bool) -> bool {
        for _ in 0..100 {
            t.set(t.get() + 20.0);
            h.run_due(t.get());
            if f(h) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        false
    }

    /// waits for what needs no drain (a pump's own work), or fails after a moment
    fn wait_for(f: impl Fn() -> bool) {
        for _ in 0..100 {
            if f() {
                return;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        panic!("timed out");
    }

    /// an upstream on localhost that takes one message, answers, and then waits for the game's half of the close
    fn upstream() -> (u16, Arc<Mutex<Vec<u8>>>, Arc<AtomicBool>) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let got: Arc<Mutex<Vec<u8>>> = Arc::default();
        let fin = Arc::new(AtomicBool::new(false));
        let (got2, fin2) = (got.clone(), fin.clone());
        std::thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            let mut buf = [0u8; 4096];
            loop {
                match s.read(&mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        got2.lock().extend_from_slice(&buf[..n]);
                        break;
                    }
                }
            }
            let _ = s.write_all(b"hi there\n");
            // the game's FIN comes as the end of the stream
            loop {
                match s.read(&mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {}
                }
            }
            fin2.store(true, Ordering::SeqCst);
        });
        (port, got, fin)
    }

    // a connection relayed to a fake upstream on localhost: the game's hello gets there, the upstream's answer
    // comes back counted in the connection's bytes, the game's FIN shuts the upstream's write side, and the
    // upstream's close comes back as the game's FIN
    #[test]
    fn relays_both_ways_and_both_ends() {
        let (port, got, fin) = upstream();
        let (mut h, t, out, _lines) = host(vec![(29900, format!("127.0.0.1:{port}"))]);
        let a = ip("10.77.0.2");
        frame(&mut h, a, 50000, SYN, 29900, 7, &[]);
        assert!(out.lock().is_empty(), "the game must not see an upstream that has not connected");
        frame(&mut h, a, 50000, DATA, 29900, 7, b"early");
        assert_eq!(h.vh.conns[&1].bytes_in, 0, "data before the SYNACK must not enter the stream");
        assert!(drain_until(&mut h, &t, |_| out.lock().iter().any(|f| f.op == SYNACK)), "the upstream connect ACKs the game");
        assert_eq!(out.lock()[0].op, SYNACK);
        frame(&mut h, a, 50000, DATA, 29900, 7, b"hello");
        wait_for(|| !got.lock().is_empty());
        assert_eq!(got.lock().as_slice(), &b"hello"[..]);
        // the answer comes back on the drain, counted as the connection's bytes out
        assert!(drain_until(&mut h, &t, |h| h.vh.conns.get(&1).is_some_and(|c| c.bytes_out == 9)), "the upstream's answer reached the game");
        let data: Vec<u8> = out.lock().iter().filter(|f| f.op == DATA).flat_map(|f| f.data.iter().copied()).collect();
        assert_eq!(data.as_slice(), &b"hi there\n"[..]);
        let conn = &h.vh.conns[&1];
        assert_eq!((conn.bytes_in, conn.bytes_out), (5, 9));
        // the game's FIN goes to the pump on the drain: the upstream sees the end of its stream
        frame(&mut h, a, 50000, FIN, 29900, 7, &[]);
        assert!(drain_until(&mut h, &t, |_| fin.load(Ordering::SeqCst)), "the game's FIN reached the upstream");
        // the upstream's close comes back as the game's FIN, and the connection goes once both sides closed
        assert!(drain_until(&mut h, &t, |h| h.vh.conns.is_empty()), "the upstream's close came back as the game's FIN");
        assert_eq!(out.lock().last().unwrap().op, FIN);
    }

    // a SYN to a port the table has no upstream for: RST, and the line once per port, and no line for a port of
    // the server's own services
    #[test]
    fn a_port_without_an_upstream_is_refused_once_logged() {
        let (port, _got, _fin) = upstream();
        let (mut h, _t, out, lines) = host(vec![(29900, format!("127.0.0.1:{port}"))]);
        let a = ip("10.77.0.2");
        frame(&mut h, a, 50000, SYN, 29901, 7, &[]);
        assert_eq!(out.lock()[0].op, RST);
        frame(&mut h, a, 50001, SYN, 29901, 8, &[]);
        assert_eq!(out.lock().len(), 2, "still refused, not logged again");
        frame(&mut h, a, 50002, SYN, 20920, 9, &[]);
        assert_eq!(lines.borrow().len(), 1);
        assert_eq!(lines.borrow()[0], "spy-relay: no upstream for port 29901");
    }

    // an upstream that cannot be connected to: the game's connection goes with a RST, the line once per host:port
    #[test]
    fn a_failed_connect_rsts_once_logged() {
        // a port nobody listens on (the listener taken back): localhost refuses at once
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let dead = l.local_addr().unwrap().port();
        drop(l);
        let (mut h, t, out, lines) = host(vec![(29900, format!("127.0.0.1:{dead}"))]);
        let a = ip("10.77.0.2");
        frame(&mut h, a, 50000, SYN, 29900, 7, &[]);
        assert!(out.lock().is_empty(), "a failed upstream must never be ACKed");
        assert!(drain_until(&mut h, &t, |_| out.lock().iter().any(|f| f.cid == 7 && f.op == RST)), "the failed connect RSTs the game's connection");
        assert!(!out.lock().iter().any(|f| f.op == SYNACK));
        assert_eq!(lines.borrow().len(), 1);
        assert!(lines.borrow()[0].starts_with(&format!("spy-relay: 127.0.0.1:{dead} connect failed:")), "{:?}", lines.borrow());
        // the game may try once more: the same RST, the line stays at one
        frame(&mut h, a, 50001, SYN, 29900, 8, &[]);
        assert!(drain_until(&mut h, &t, |_| out.lock().iter().any(|f| f.cid == 8 && f.op == RST)), "the second try RSTs too");
        assert_eq!(lines.borrow().len(), 1, "the failure is logged once per host:port");
    }
}
