# Essential Model

What each component is given, what it must do with it, what it must remember, and
what it hands on. Everything here follows the protocols as described in
[`NASDAQ_SPEC.md`](NASDAQ_SPEC.md); how the implementation gets there (and where it
differs) belongs in the code or in [Accidental Complexity](#accidental-complexity).

Following *Out of the Tar Pit*, **Essential State** separates the state the
component has to be told (input that can't be recomputed) from state that's **derived**
from that input and kept anyway, because recomputing it from the day's history on
every message would be absurd.

## Pipeline

```
Client -> Inbound Gateway -> Matching Engine -> Outbound Gateway -> Client
                                    |
                                    +-> Market Data Feed -> Subscribers
```

## Client

### Inputs
- Run parameters from the user: how many sessions to open, how many Enter Orders to
  keep in flight per session, the order mix, the seed, and how long to run.
- Per session, from the exchange (SoupBinTCP):
  - Login Accepted (session, next sequence number) or Login Rejected (reason)
  - Sequenced Data: OUCH Order Accepted, Replaced, Canceled, Executed, Rejected
  - Server Heartbeat, End of Session
- Connection loss.

### Responsibilities
- Open each session with a Login Request. On a reconnect, ask for the next sequence
  number it hasn't seen yet.
- Generate Enter, Cancel and Replace Orders. Every new UserRefNum on a session is
  strictly greater than the last one.
- Keep up to the configured number of Enter Orders unanswered. Only Enters count,
  because each one is guaranteed an answer (Accepted or Rejected). Cancels and Replaces
  can legitimately go unanswered.
- Track which of its orders are live, and their open shares, from the responses, so
  that it only cancels or replaces orders that might still be live.
- Send a Client Heartbeat after 1 s of silence, and treat about 15 s without hearing
  from the exchange as a lost connection.
- Measure round-trip latency and throughput.

### Essential State
- Input: the run parameters.
- Derived, per session:
  - the last UserRefNum used
  - the next expected sequence number
  - live orders (UserRefNum -> open shares, side, price)
  - send time of each Enter that hasn't been answered yet

### Outputs
- To the Inbound Gateway: Login Request, Unsequenced Data (OUCH Enter, Replace or
  Cancel), Client Heartbeat, Logout Request.
- To the user: latency percentiles, throughput, counts of each response type.

## Inbound Gateway

### Inputs
- From each client connection: a byte stream of SoupBinTCP packets (Login Request,
  Unsequenced Data, Client Heartbeat, Logout Request, Debug).
- Connection open / connection lost.
- Configuration: valid credentials, the session name, the tradable symbols, the port
  options (cancel on disconnect, default Firm / CustomerType).

### Responsibilities
- Split the stream into logical SoupBinTCP packets.
- **Login**:
  - check the credentials (reject `A`)
  - check that the requested session and sequence number can be served (reject `S`)
  - allow only one connection per session
  - close a connection that doesn't log in within the timeout
- Identify the client: the session a message arrived on *is* the client. OUCH carries
  no client identity.
- Ignore Unsequenced Data before login. Close on Logout Request, and on about 15 s of
  silence.
- Decode OUCH, then apply the per-port UserRefNum rules:
  - An Enter whose UserRefNum isn't greater than the last one is a duplicate: ignore it.
    Any other Enter uses up its UserRefNum.
  - A Replace whose new UserRefNum has already been used is ignored, and doesn't use it
    up.
- Validate against the in-scope subset of the spec:
  - An invalid Enter gets Order Rejected, with its Appendix D reason.
  - An invalid Replace becomes a cancel of the original order.
  - An out-of-scope option counts as invalid unless it's at its default value.
- Map the symbol to its book. An unknown symbol is rejected.
- Assign each accepted Enter or valid Replace a new day-unique Order Reference Number.
- On connection loss, when cancel on disconnect is enabled, ask for every open order on
  the session to be canceled. A Logout Request doesn't trigger this.

### Essential State
- Input: the configuration.
- Derived:
  - username -> session
  - which sessions have a live connection
  - per session, the last UserRefNum used
  - the next Order Reference Number
  - per connection, the bytes of a packet that has only partly arrived

### Outputs
- To the Matching Engine, one request per valid order message, keyed by
  (session, UserRefNum):
  - **New**: Order Reference Number, book, side, quantity, price or market, Day/IOC
  - **Cancel**: the order, and its new intended size (including shares already
    executed)
  - **Replace**: the original order, the new UserRefNum, its Order Reference Number,
    quantity (including shares already executed), price, Day/IOC, and side if one was
    given
  - **Cancel all** for a session (cancel on disconnect)
- To the Outbound Gateway:
  - login results (Accepted with the sequence number to resume from, or Rejected with a
    reason)
  - connection closes
  - gateway rejects (Order Rejected with a reason)
  - the fields of each Enter or Replace that the engine has no use for but the
    responses must echo: ClOrdID, Symbol, Capacity, Cross Type, Display, the
    Intermarket Sweep flag, Firm, CustomerType, GroupID

## Matching Engine

### Inputs
- New, Cancel, Replace and Cancel-all requests from the Inbound Gateway, in arrival
  order.
- Configuration: the books (one per symbol).

### Responsibilities
- **New**: the order trades first, as the aggressor, in price-time priority, and each
  fill is at the resting order's price. A Day limit order then rests with whatever is
  left. Whatever an IOC or market order didn't fill is canceled (reason `I`).
- **Cancel**:
  - Reduce the open shares to `intended size - executed`, keeping time priority. Reducing
    to zero removes the order.
  - Ignore a cancel that wouldn't remove any shares, or that names an order that's
    unknown or dead.
- **Replace**:
  - Ignore the replace if the original is unknown or dead.
  - If the side given doesn't match the original's, or the order has already executed
    the whole intended size, cancel the original.
  - Otherwise remove the original, then enter the replacement under its new UserRefNum
    and Order Reference Number. The replacement carries over the shares already
    executed, loses time priority, and trades like any incoming order.
- **Cancel all**: cancel every open order of the session (reason `U`).
- Give every execution a day-unique Match Number, shared by both sides.
- Timestamp every event.

### Essential State
- Input: the configuration.
- Derived (all of it could be replayed from the day's requests):
  - each book: the resting orders per price level in time order, plus best bid and ask
  - each live order: (session, UserRefNum), Order Reference Number, side, price, open
    shares, shares executed so far
  - the next Match Number

### Outputs
- To the Outbound Gateway, per session:
  - Accepted (with Order State)
  - Replaced (the original and the replacement)
  - Canceled (shares removed, reason)
  - Executed (shares, price, liquidity flag `A` or `R`, Match Number)
  - Rejected (Processing Error)
- To the Market Data Feed, every visible change to a book, as ITCH: Add Order, Order
  Executed, Order Cancel, Order Delete, Order Replace.

## Outbound Gateway

### Inputs
- Engine events, per session.
- From the Inbound Gateway: login results, connection closes, gateway rejects, and each
  order's echo fields.
- Configuration: the session name.

### Responsibilities
- Turn each event into its OUCH message, filling in what the engine doesn't carry
  (UserRefNum, ClOrdID, Symbol, Capacity, ...) from the order's echo fields. A replaced
  order inherits the fields a replace can't change.
- Number every Sequenced Data message per session, from 1 for the day, and keep the
  session's stream so a re-login can be served from any sequence number.
- Send Login Accepted followed by replay from the requested sequence number, or Login
  Rejected followed by a close. Then deliver Sequenced Data live.
- Keep delivering a session's messages to its log while it's disconnected: its orders
  can still trade or be canceled.
- Send a Server Heartbeat after 1 s of silence. At the end of the day, send End of
  Session and close.

### Essential State
- Input: the configuration.
- Derived:
  - per session, the day's Sequenced Data stream: the record replay is served from
  - per session, the connection currently being served, and how far it has been sent
  - per live order, its echo fields, until the order is dead

### Outputs
- To the Client: Login Accepted / Rejected, Sequenced Data (OUCH Order Accepted,
  Replaced, Canceled, Executed, Rejected), Server Heartbeat, End of Session.

## Market Data Feed

### Inputs
- ITCH book-change messages from the Matching Engine, in order.
- Configuration: the symbol directory (symbol -> stock locate, reference data), the
  MoldUDP64 session name, the multicast group.
- Retransmission requests from subscribers.

### Responsibilities
- At the start of the day, publish a Stock Directory message for every symbol, before
  any order messages.
- Number every message from 1 for the day, and pack messages into MoldUDP64 downstream
  packets sent to the multicast group.
- Send a heartbeat (Message Count `0`) while idle, and End of Session (Message Count
  `0xFFFF`) at the end of the day.
- Answer retransmission requests by unicasting the requested messages to whoever asked.

### Essential State
- Input: the configuration.
- Derived:
  - the day's message stream, kept so retransmission requests can be answered
  - the next sequence number

### Outputs
- To subscribers: MoldUDP64 packets carrying ITCH (Stock Directory, Add Order, Order
  Executed, Order Cancel, Order Delete, Order Replace), plus heartbeats and End of
  Session.
- To one subscriber: the retransmissions it asked for.

## Accidental Complexity

Everything below exists for speed, or because of the platform. None of it is needed to
meet the spec.

- **Kernel bypass**: DPDK, a TCP stack in user space, the tap interface, RPS, the queue
  assignments.
- **Threads**: separate inbound and outbound gateway threads, the matching thread, core
  pinning, `SCHED_FIFO`. The gateway split creates state the two halves must share
  (notes, which sessions are connected, how much replay is retained).
- **SPSC rings** between the components: their capacities, batching, and dropping
  market data when its ring is full.
- **Fixed-capacity stores**: the order pool (and rejecting with a Processing Error when
  it's full), a dense price ladder over a configured price band, the bounded in-memory
  replay ring that stands in for a full-day journal, and the maximum number of sessions.
- **Engine representation**: prices kept in cents rather than OUCH's four decimals, and
  orders found by (session, UserRefNum).
- **Duplicated derived state**: the outbound gateway's per-order echo record exists so
  the engine doesn't have to carry fields it doesn't use. The book's best bid and ask
  are cached.
- **Time**: the TSC clock and its calibration, latency stamps, histograms.
