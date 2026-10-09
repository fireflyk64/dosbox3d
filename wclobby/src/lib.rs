//! wclobby: C ABI over the lobbylink Rust client for DOSBox's Wing
//! Commander multiplayer transport (see include/wclobby.h for the
//! contract and src/cpu/wcnet_lobby.cpp for the C++ side).
//!
//! One background thread owns the tokio runtime and the `P2PGame`.  It
//! demultiplexes the game's single event stream into per-player inboxes
//! and link states under one mutex + condvar, which is what the C side
//! blocks on.  Sends are queued to the same thread, which awaits them in
//! order (the client serializes reliable sends per peer anyway).
//!
//! Logical connections: every slot carries a generation counter that
//! changes on link up, link death and hangups.  A zero-length reliable
//! message is the hangup marker (the game protocol never sends empty
//! blobs).

use std::collections::VecDeque;
use std::ffi::{c_char, CStr, CString};
use std::os::raw::c_int;
use std::ptr;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use bytes::Bytes;
use p2p_lobby_client::{
    ConnectOptions, CreateOptions, Event, MessageKind, P2PGame, PlayerLeftReason,
};

const LOG_ERROR: c_int = 0;
const LOG_INFO: c_int = 1;
const LOG_EVENT: c_int = 2;

const PEER_ABSENT: c_int = 0;
const PEER_DOWN: c_int = 1;
const PEER_UP: c_int = 2;
const PEER_GONE: c_int = 3;

/// Upper bound on one queued send before the net loop gives up waiting
/// for it (the job itself stays queued inside the client).
const SEND_TIMEOUT: Duration = Duration::from_secs(5);
/// How long close() waits for the leave message to go out.
const CLOSE_TIMEOUT: Duration = Duration::from_secs(3);

// ---------------------------------------------------------------------------
// Logging

type LogFn = unsafe extern "C" fn(c_int, *const c_char);
static LOGGER: Mutex<Option<LogFn>> = Mutex::new(None);

fn log(level: c_int, msg: impl AsRef<str>) {
    let msg = msg.as_ref();
    let cb = *LOGGER.lock().unwrap_or_else(|e| e.into_inner());
    match cb {
        Some(f) => {
            if let Ok(c) = CString::new(msg) {
                // SAFETY: the C side registered a valid callback.
                unsafe { f(level, c.as_ptr()) }
            }
        }
        None => eprintln!("wclobby: {msg}"),
    }
}

// ---------------------------------------------------------------------------
// Shared state

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Link {
    /// No usable channel (fresh slot, or a link being rebuilt).
    Down,
    /// Channel open.
    Up,
    /// Was up; ICE reported "disconnected".  Usually recovers; same generation.
    Flaky,
    /// Channel failed/closed.
    Gone,
}

struct Peer {
    occupied: bool,
    link: Link,
    gen: u32,
    /// The C side holds a stream for the current generation.
    attached: bool,
    inbox: VecDeque<Bytes>,
    /// Best-effort datagrams, the newest `BEST_EFFORT_KEPT` of them.
    be_inbox: VecDeque<Bytes>,
}

const BEST_EFFORT_KEPT: usize = 64;

impl Peer {
    fn new() -> Self {
        Peer {
            occupied: false,
            link: Link::Down,
            gen: 0,
            attached: false,
            inbox: VecDeque::new(),
            be_inbox: VecDeque::new(),
        }
    }
    fn live(&self) -> bool {
        self.occupied && matches!(self.link, Link::Up | Link::Flaky)
    }
    fn state_code(&self) -> c_int {
        if !self.occupied {
            PEER_ABSENT
        } else {
            match self.link {
                Link::Down => PEER_DOWN,
                Link::Up | Link::Flaky => PEER_UP,
                Link::Gone => PEER_GONE,
            }
        }
    }
    /// New generation: nothing the C side holds for this slot is valid any more.
    fn new_generation(&mut self) {
        self.gen = self.gen.wrapping_add(1);
        self.attached = false;
        self.inbox.clear();
        self.be_inbox.clear();
    }
}

struct State {
    closed: bool,
    /// The signaling WebSocket is up (new players can still join).
    signaling: bool,
    peers: Vec<Peer>,
}

impl State {
    fn peer(&mut self, id: u16) -> Option<&mut Peer> {
        self.peers.get_mut(id as usize)
    }

    fn link_up(&mut self, id: u16) {
        let Some(p) = self.peer(id) else { return };
        match p.link {
            Link::Up => {}
            Link::Flaky => {
                p.link = Link::Up;
                log(LOG_INFO, format!("player {id}: link recovered"));
            }
            Link::Down | Link::Gone => {
                p.link = Link::Up;
                p.new_generation();
                log(LOG_INFO, format!("player {id}: link up"));
            }
        }
    }

    fn link_gone(&mut self, id: u16, why: &str) {
        let Some(p) = self.peer(id) else { return };
        if p.link != Link::Gone {
            let was_live = matches!(p.link, Link::Up | Link::Flaky);
            p.link = Link::Gone;
            p.new_generation();
            if was_live {
                log(LOG_INFO, format!("player {id}: link lost ({why})"));
            }
        }
    }

    fn link_forming(&mut self, id: u16) {
        // A fresh peer connection is negotiating.  Any live link we
        // still believe in belonged to a previous connection.
        let Some(p) = self.peer(id) else { return };
        if matches!(p.link, Link::Up | Link::Flaky) {
            self.link_gone(id, "rebuilt");
        }
        if let Some(p) = self.peer(id) {
            p.link = Link::Down;
        }
    }

    fn player_present(&mut self, id: u16, what: &str) {
        self.link_forming(id);
        if let Some(p) = self.peer(id) {
            p.occupied = true;
            log(LOG_INFO, format!("player {id} {what}"));
        }
    }

    fn player_absent(&mut self, id: u16, why: &str) {
        self.link_gone(id, why);
        if let Some(p) = self.peer(id) {
            p.occupied = false;
            log(LOG_INFO, format!("player {id} left ({why})"));
        }
    }

    fn deliver(&mut self, from: u16, data: Bytes) {
        if data.is_empty() {
            // Hangup marker from the peer.
            if let Some(p) = self.peer(from) {
                if p.attached {
                    log(LOG_INFO, format!("player {from} hung up"));
                } else {
                    log(LOG_EVENT, format!("player {from} hung up before we accepted"));
                }
                p.new_generation();
            }
            return;
        }
        // Data can only flow over an open channel; if we have not heard
        // "connected" yet, this is as good as that.
        self.link_up(from);
        if let Some(p) = self.peer(from) {
            p.inbox.push_back(data);
        }
    }

    fn deliver_best_effort(&mut self, from: u16, data: Bytes) {
        if data.is_empty() {
            return;
        }
        self.link_up(from);
        if let Some(p) = self.peer(from) {
            if p.be_inbox.len() >= BEST_EFFORT_KEPT {
                p.be_inbox.pop_front();
            }
            p.be_inbox.push_back(data);
        }
    }

    fn close_all(&mut self) {
        self.closed = true;
        for id in 0..self.peers.len() {
            self.link_gone(id as u16, "lobby closed");
        }
    }
}

struct Shared {
    state: Mutex<State>,
    cv: Condvar,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn update(&self, f: impl FnOnce(&mut State)) {
        let mut st = self.lock();
        f(&mut st);
        drop(st);
        self.cv.notify_all();
    }

    /// Runs `check` until it yields a value or the timeout expires.
    fn wait_until<T>(
        &self,
        timeout_ms: i32,
        mut check: impl FnMut(&mut State) -> Option<T>,
    ) -> Option<T> {
        let deadline = if timeout_ms < 0 {
            None
        } else {
            Some(Instant::now() + Duration::from_millis(timeout_ms as u64))
        };
        let mut st = self.lock();
        loop {
            if let Some(v) = check(&mut st) {
                return Some(v);
            }
            match deadline {
                None => st = self.cv.wait(st).unwrap_or_else(|e| e.into_inner()),
                Some(d) => {
                    let now = Instant::now();
                    if now >= d {
                        return None;
                    }
                    st = self
                        .cv
                        .wait_timeout(st, d - now)
                        .unwrap_or_else(|e| e.into_inner())
                        .0;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The net thread

enum Outbound {
    Send { to: u16, data: Bytes },
    SendBestEffort { to: u16, data: Bytes },
    Close,
}

pub struct Hub {
    shared: Arc<Shared>,
    out_tx: tokio::sync::mpsc::UnboundedSender<Outbound>,
    done_rx: std::sync::mpsc::Receiver<()>,
    self_id: u16,
    max_players: u16,
}

type Ready = Result<(u16, u16), String>;

fn handle_event(shared: &Shared, ev: Event) {
    match ev {
        Event::Message { from, kind: MessageKind::Reliable, data } => {
            shared.update(|st| st.deliver(from, data));
        }
        Event::Message { from, kind: MessageKind::BestEffort, data } => {
            shared.update(|st| st.deliver_best_effort(from, data));
        }
        Event::PlayerJoined { player_id } => {
            shared.update(|st| st.player_present(player_id, "joined"));
        }
        Event::PlayerRejoined { player_id, .. } => {
            shared.update(|st| st.player_present(player_id, "rejoined"));
        }
        Event::PlayerReplaced { player_id } => {
            shared.update(|st| st.player_present(player_id, "was replaced"));
        }
        Event::PlayerLeft { player_id, reason } => match reason {
            PlayerLeftReason::ExplicitLeave => {
                shared.update(|st| st.player_absent(player_id, "left the room"));
            }
            // Only the player's signaling socket is gone (idle proxy
            // timeout, laptop lid, ...).  The lobby keeps the slot and the
            // WebRTC link is unaffected, so the game connection stays up;
            // if the process really died the link reports failed/closed.
            PlayerLeftReason::Disconnected => {
                log(LOG_INFO, format!("player {player_id} lost its lobby connection; game link unaffected"));
            }
        },
        Event::Started => log(LOG_EVENT, "room started"),
        Event::PeerState { player_id, state } => {
            log(LOG_EVENT, format!("player {player_id}: peer connection {state}"));
            shared.update(|st| match state.as_str() {
                "connected" => st.link_up(player_id),
                "disconnected" => {
                    if let Some(p) = st.peer(player_id) {
                        if p.link == Link::Up {
                            p.link = Link::Flaky;
                        }
                    }
                }
                "failed" | "closed" => st.link_gone(player_id, &state),
                _ => st.link_forming(player_id),
            });
        }
        Event::CandidatePair { player_id, local, remote } => {
            log(LOG_INFO, format!("player {player_id}: path {local}/{remote}"));
        }
        Event::LobbyError { code, message } => {
            log(LOG_ERROR, format!("lobby error {code}: {message}"));
        }
        Event::SignalingClosed { code, message } => {
            let fatal = matches!(code.as_str(), "replaced" | "session-superseded" | "room-expired");
            if fatal {
                log(LOG_ERROR, format!("lobby session over ({code}: {message})"));
                shared.update(|st| st.close_all());
            } else {
                log(LOG_ERROR, format!("lobby signaling lost ({code}: {message}); existing links continue, nobody new can join"));
                shared.update(|st| st.signaling = false);
            }
        }
    }
}

fn net_thread(
    opts: ConnectOptions,
    shared: Arc<Shared>,
    ready_tx: std::sync::mpsc::Sender<Ready>,
    mut out_rx: tokio::sync::mpsc::UnboundedReceiver<Outbound>,
) {
    let rt = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .thread_name("wclobby-rt")
        .enable_all()
        .build()
    {
        Ok(rt) => rt,
        Err(e) => {
            let _ = ready_tx.send(Err(format!("runtime: {e}")));
            return;
        }
    };
    rt.block_on(async move {
        let mut game = match P2PGame::connect(opts).await {
            Ok(g) => g,
            Err(e) => {
                let _ = ready_tx.send(Err(format!("{}: {}", e.code, e.message)));
                return;
            }
        };
        shared.update(|st| {
            st.peers = (0..game.max_players()).map(|_| Peer::new()).collect();
            for p in game.players() {
                if p.occupied {
                    st.peers[p.id as usize].occupied = true;
                }
            }
        });
        let _ = ready_tx.send(Ok((game.self_id(), game.max_players())));

        loop {
            tokio::select! {
                ev = game.next_event() => match ev {
                    None => {
                        log(LOG_ERROR, "lobby event stream ended");
                        break;
                    }
                    Some(ev) => handle_event(&shared, ev),
                },
                out = out_rx.recv() => match out {
                    None | Some(Outbound::Close) => break,
                    Some(Outbound::SendBestEffort { to, data }) => {
                        // (Dropped when the channel is not open or full: the contract.)
                        let _ = game.send_best_effort(to, data).await;
                    }
                    Some(Outbound::Send { to, data }) => {
                        match tokio::time::timeout(SEND_TIMEOUT, game.send_reliable(to, data)).await {
                            Ok(Ok(())) => {}
                            Ok(Err(e)) => {
                                log(LOG_ERROR, format!("send to player {to} failed: {e}"));
                                shared.update(|st| st.link_gone(to, "send failed"));
                            }
                            Err(_) => log(LOG_ERROR, format!("send to player {to} is stalled")),
                        }
                    }
                },
            }
        }
        let _ = tokio::time::timeout(CLOSE_TIMEOUT, game.close()).await;
        shared.update(|st| {
            st.signaling = false;
            st.close_all();
        });
    });
}

fn spawn_hub(opts: ConnectOptions) -> Result<Hub, String> {
    let shared = Arc::new(Shared {
        state: Mutex::new(State { closed: false, signaling: true, peers: Vec::new() }),
        cv: Condvar::new(),
    });
    let (ready_tx, ready_rx) = std::sync::mpsc::channel::<Ready>();
    let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
    let (out_tx, out_rx) = tokio::sync::mpsc::unbounded_channel();
    let thread_shared = shared.clone();
    std::thread::Builder::new()
        .name("wclobby-net".into())
        .spawn(move || {
            net_thread(opts, thread_shared, ready_tx, out_rx);
            let _ = done_tx.send(());
        })
        .map_err(|e| format!("cannot start net thread: {e}"))?;
    match ready_rx.recv() {
        Ok(Ok((self_id, max_players))) => Ok(Hub { shared, out_tx, done_rx, self_id, max_players }),
        Ok(Err(e)) => Err(e),
        Err(_) => Err("net thread exited before joining".to_string()),
    }
}

// ---------------------------------------------------------------------------
// C API

unsafe fn c_str<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        CStr::from_ptr(p).to_str().ok()
    }
}

fn write_err(err: *mut c_char, err_len: usize, msg: &str) {
    if err.is_null() || err_len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(err_len - 1);
    // SAFETY: the caller promises err points at err_len writable bytes.
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
}

#[no_mangle]
pub extern "C" fn wclobby_set_logger(f: Option<LogFn>) {
    *LOGGER.lock().unwrap_or_else(|e| e.into_inner()) = f;
}

/// # Safety
/// `opts` must point at a valid options struct with NUL-terminated strings;
/// `err` must be NULL or point at `err_len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn wclobby_connect(
    opts: *const wclobby_options,
    err: *mut c_char,
    err_len: usize,
) -> *mut Hub {
    if opts.is_null() {
        write_err(err, err_len, "invalid-options: NULL options");
        return ptr::null_mut();
    }
    let o = &*opts;
    let (Some(server), Some(code)) = (c_str(o.server), c_str(o.code)) else {
        write_err(err, err_len, "invalid-options: server and code are required");
        return ptr::null_mut();
    };
    let connect = ConnectOptions {
        server: server.to_string(),
        code: code.to_string(),
        create: (o.create_max_players > 0).then(|| CreateOptions::new(o.create_max_players)),
        force_relay: o.force_relay != 0,
        origin: c_str(o.origin).map(str::to_string),
        storage_path: c_str(o.token_path).map(std::path::PathBuf::from),
        disable_keepalive: o.no_keepalive != 0,
        ..Default::default()
    };
    log(LOG_INFO, format!("joining room {code} at {server}"));
    match spawn_hub(connect) {
        Ok(hub) => {
            log(LOG_INFO, format!("joined room {code} as player {} of {}", hub.self_id, hub.max_players));
            Box::into_raw(Box::new(hub))
        }
        Err(e) => {
            log(LOG_ERROR, format!("joining room {code} failed: {e}"));
            write_err(err, err_len, &e);
            ptr::null_mut()
        }
    }
}

#[repr(C)]
pub struct wclobby_options {
    pub server: *const c_char,
    pub code: *const c_char,
    pub origin: *const c_char,
    pub create_max_players: u16,
    pub force_relay: c_int,
    pub token_path: *const c_char,
    pub no_keepalive: c_int,
}

/// # Safety
/// `h` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn wclobby_signaling_alive(h: *const Hub) -> c_int {
    let st = (*h).shared.lock();
    (st.signaling && !st.closed) as c_int
}

/// # Safety
/// `h` must come from wclobby_connect and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn wclobby_close(h: *mut Hub) {
    if h.is_null() {
        return;
    }
    let hub = Box::from_raw(h);
    let _ = hub.out_tx.send(Outbound::Close);
    // Bounded wait for the leave message; the thread is detached otherwise.
    let _ = hub.done_rx.recv_timeout(CLOSE_TIMEOUT + SEND_TIMEOUT);
    hub.shared.update(|st| st.close_all());
}

/// # Safety
/// `h` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn wclobby_self_id(h: *const Hub) -> u16 {
    (*h).self_id
}

/// # Safety
/// `h` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn wclobby_max_players(h: *const Hub) -> u16 {
    (*h).max_players
}

/// # Safety
/// `h` must be a live handle; `gen_out` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn wclobby_peer_state(h: *const Hub, player: u16, gen_out: *mut u32) -> c_int {
    let mut st = (*h).shared.lock();
    match st.peer(player) {
        Some(p) => {
            if !gen_out.is_null() {
                *gen_out = p.gen;
            }
            p.state_code()
        }
        None => PEER_ABSENT,
    }
}

/// # Safety
/// `h` must be a live handle; `gen_out` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn wclobby_open(
    h: *mut Hub,
    player: u16,
    timeout_ms: i32,
    gen_out: *mut u32,
) -> c_int {
    let hub = &*h;
    let result = hub.shared.wait_until(timeout_ms, |st| {
        if st.closed {
            return Some(Err(()));
        }
        let p = st.peer(player)?;
        if !p.occupied {
            return Some(Err(()));
        }
        if p.live() {
            p.attached = true;
            return Some(Ok(p.gen));
        }
        None
    });
    match result {
        Some(Ok(gen)) => {
            if !gen_out.is_null() {
                *gen_out = gen;
            }
            1
        }
        Some(Err(())) => -1,
        None => 0,
    }
}

/// # Safety
/// `h` must be a live handle; the out pointers NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn wclobby_accept(
    h: *mut Hub,
    timeout_ms: i32,
    player_out: *mut u16,
    gen_out: *mut u32,
) -> c_int {
    let hub = &*h;
    let result = hub.shared.wait_until(timeout_ms, |st| {
        if st.closed {
            return Some(Err(()));
        }
        for (id, p) in st.peers.iter_mut().enumerate() {
            if p.live() && !p.attached && !p.inbox.is_empty() {
                p.attached = true;
                return Some(Ok((id as u16, p.gen)));
            }
        }
        None
    });
    match result {
        Some(Ok((id, gen))) => {
            if !player_out.is_null() {
                *player_out = id;
            }
            if !gen_out.is_null() {
                *gen_out = gen;
            }
            1
        }
        Some(Err(())) => -1,
        None => 0,
    }
}

/// # Safety
/// `h` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn wclobby_is_open(h: *const Hub, player: u16, gen: u32) -> c_int {
    let mut st = (*h).shared.lock();
    if st.closed {
        return 0;
    }
    match st.peer(player) {
        Some(p) if p.gen == gen && p.live() && p.attached => 1,
        _ => 0,
    }
}

/// # Safety
/// `h` must be a live handle; `data` must point at `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn wclobby_send(
    h: *mut Hub,
    to: u16,
    gen: u32,
    data: *const u8,
    len: usize,
) -> c_int {
    let hub = &*h;
    if len == 0 {
        // Empty is the hangup marker; the game never sends it.
        return -1;
    }
    let bytes = Bytes::copy_from_slice(std::slice::from_raw_parts(data, len));
    {
        let mut st = hub.shared.lock();
        if st.closed {
            return -1;
        }
        match st.peer(to) {
            Some(p) if p.gen == gen && p.live() => {}
            _ => return -1,
        }
    }
    if hub.out_tx.send(Outbound::Send { to, data: bytes }).is_err() {
        return -1;
    }
    0
}

/// # Safety
/// `h` must be a live handle; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn wclobby_recv(
    h: *mut Hub,
    from: u16,
    gen: u32,
    timeout_ms: i32,
    out: *mut wclobby_buf,
) -> c_int {
    let hub = &*h;
    let result = hub.shared.wait_until(timeout_ms, |st| {
        if st.closed {
            return Some(Err(()));
        }
        let p = st.peer(from)?;
        if p.gen != gen {
            return Some(Err(()));
        }
        if let Some(data) = p.inbox.pop_front() {
            return Some(Ok(data));
        }
        if !p.live() {
            return Some(Err(()));
        }
        None
    });
    match result {
        Some(Ok(data)) => {
            let boxed: Box<[u8]> = data.to_vec().into_boxed_slice();
            let len = boxed.len();
            let raw = Box::into_raw(boxed) as *mut u8;
            (*out).data = raw;
            (*out).len = len;
            1
        }
        Some(Err(())) => -1,
        None => 0,
    }
}

/// # Safety
/// `h` must be a live handle; `data` must point at `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn wclobby_send_best_effort(
    h: *mut Hub,
    to: u16,
    gen: u32,
    data: *const u8,
    len: usize,
) -> c_int {
    let hub = &*h;
    if len == 0 {
        return -1;
    }
    let bytes = Bytes::copy_from_slice(std::slice::from_raw_parts(data, len));
    {
        let mut st = hub.shared.lock();
        if st.closed {
            return -1;
        }
        match st.peer(to) {
            Some(p) if p.gen == gen && p.live() => {}
            _ => return -1,
        }
    }
    if hub.out_tx.send(Outbound::SendBestEffort { to, data: bytes }).is_err() {
        return -1;
    }
    0
}

/// # Safety
/// `h` must be a live handle; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn wclobby_recv_best_effort(
    h: *mut Hub,
    from: u16,
    gen: u32,
    out: *mut wclobby_buf,
) -> c_int {
    let hub = &*h;
    let mut st = hub.shared.lock();
    if st.closed {
        return -1;
    }
    let Some(p) = st.peer(from) else { return -1 };
    if p.gen != gen {
        return -1;
    }
    match p.be_inbox.pop_front() {
        Some(data) => {
            let boxed: Box<[u8]> = data.to_vec().into_boxed_slice();
            let len = boxed.len();
            let raw = Box::into_raw(boxed) as *mut u8;
            (*out).data = raw;
            (*out).len = len;
            1
        }
        None => {
            if p.live() {
                0
            } else {
                -1
            }
        }
    }
}

#[repr(C)]
pub struct wclobby_buf {
    pub data: *mut u8,
    pub len: usize,
}

/// # Safety
/// `buf` must have been filled by wclobby_recv and not freed before.
#[no_mangle]
pub unsafe extern "C" fn wclobby_buf_free(buf: *mut wclobby_buf) {
    if buf.is_null() || (*buf).data.is_null() {
        return;
    }
    drop(Box::from_raw(ptr::slice_from_raw_parts_mut((*buf).data, (*buf).len)));
    (*buf).data = ptr::null_mut();
    (*buf).len = 0;
}

/// # Safety
/// `h` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn wclobby_hangup(h: *mut Hub, player: u16, gen: u32) {
    let hub = &*h;
    let tell_peer = {
        let mut st = hub.shared.lock();
        match st.peer(player) {
            Some(p) if p.gen == gen && p.attached => {
                let live = p.live();
                p.new_generation();
                log(LOG_INFO, format!("hanging up on player {player}"));
                live
            }
            _ => false,
        }
    };
    hub.shared.cv.notify_all();
    if tell_peer {
        let _ = hub.out_tx.send(Outbound::Send { to: player, data: Bytes::new() });
    }
}
