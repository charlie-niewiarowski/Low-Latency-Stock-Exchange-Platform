# Nasdaq Protocol Notes

Notes on the parts of Nasdaq's protocols this project emulates:

| Layer | Protocol | Direction |
|---|---|---|
| Order entry session | SoupBinTCP 3.00 | client <-> exchange, TCP |
| Order entry | OUCH 5.0 | client <-> exchange, inside SoupBinTCP |
| Market data | ITCH 5.0 | exchange -> subscribers, inside MoldUDP64 |
| Market data transport | MoldUDP64 1.00 | exchange -> subscribers, UDP multicast |

These notes describe **Nasdaq's spec**, not this implementation. Parts of the spec this
project doesn't emulate are marked **Out of scope**. Where the implementation differs
from the spec, the difference is documented in the code, not here.

---

## Common Data Types

| Type | Encoding |
|---|---|
| Integer | Unsigned, big-endian binary, of the stated width |
| Alpha | ASCII, left-justified, right-padded with spaces |
| Price (OUCH) | 8-byte integer, 4 implied decimal places |
| Price (ITCH) | 4-byte integer, 4 implied decimal places |
| Timestamp (OUCH) | 8-byte integer, nanoseconds since midnight |
| Timestamp (ITCH) | 6-byte integer, nanoseconds since midnight |

Both OUCH and ITCH messages have no padding: every field sits at a fixed offset.

### Prices

- Limit prices go up to $199,999.9900.
- A price of $200,000.0000 or more (which includes the documented sentinel
  `0x7FFFFFFF`, $214,748.3647) is a **market order**.
- Tick size (Reg NMS Rule 612): $0.01 at or above $1.00, and $0.0001 below $1.00.

### Day-Unique Identifiers

| Identifier | Assigned by | Scope | Notes |
|---|---|---|---|
| UserRefNum | Client | Per OUCH port (session), per day | 4 bytes, strictly increasing; the order's identity in OUCH |
| ClOrdID | Client | — | 14-byte free-form label; **not** required to be unique |
| Order Reference Number | Exchange | Per day | Same number in OUCH responses and in ITCH |
| Match Number | Exchange | Per day | Shared by both sides of an execution; same in OUCH and ITCH |

OUCH messages don't carry the client's identity: it's whichever session the message
arrived on.

---

## SoupBinTCP 3.00

The session layer OUCH runs over. It gives each client a login, heartbeats, and a
sequenced, replayable stream from the server.

### Framing

Every logical packet:

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Packet Length | 0 | 2 | Integer | Bytes that follow: the type byte plus the payload |
| Packet Type | 2 | 1 | Alpha | |
| Payload | 3 | variable | | |

Logical packets don't line up with TCP segments: the byte stream can split or merge
them.

Numeric fields inside SoupBinTCP payloads (sequence numbers) are **ASCII decimal**,
left-padded with spaces, not binary.

### Client -> Server

| Type | Packet | Payload |
|---|---|---|
| `L` | Login Request | see below |
| `U` | Unsequenced Data | one OUCH inbound message |
| `R` | Client Heartbeat | none |
| `O` | Logout Request | none |
| `+` | Debug | free-form text; the receiver ignores it |

**Login Request (`L`)**

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Username | 0 | 6 | Alpha | Case-insensitive |
| Password | 6 | 10 | Alpha | Case-insensitive |
| Requested Session | 16 | 10 | Alpha | All spaces: the currently active session |
| Requested Sequence Number | 26 | 20 | Numeric (ASCII) | Next Sequenced Data message wanted; `0` = start from the most recent |

### Server -> Client

| Type | Packet | Payload |
|---|---|---|
| `A` | Login Accepted | see below |
| `J` | Login Rejected | 1-byte reject reason |
| `S` | Sequenced Data | one OUCH outbound message |
| `H` | Server Heartbeat | none |
| `Z` | End of Session | none |
| `+` | Debug | free-form text; the receiver ignores it |

**Login Accepted (`A`)**

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Session | 0 | 10 | Alpha | Left-padded with spaces |
| Sequence Number | 10 | 20 | Numeric (ASCII) | Number of the next Sequenced Data packet the server will send |

**Login Rejected reasons**

| Code | Meaning |
|---|---|
| `A` | Not authorized: bad username or password |
| `S` | Session not available: the requested session or sequence number can't be served |

### Behaviour

- **Login.** The client's first packet must be a Login Request. The server answers
  Login Accepted or Login Rejected; after a reject it closes the connection. The server
  also closes a connection that doesn't log in within a timeout (typically 30 s).
- **One connection per session.** A login for a session that already has a live
  connection is rejected.
- **Sequencing.** Sequenced Data packets are numbered implicitly: the first one after
  Login Accepted carries the sequence number given in Login Accepted, and each later one
  is one higher. The first message of the day is number 1.
- **Replay.** A client that reconnects asks for the next sequence number it hasn't seen,
  and the server resends every Sequenced Data message from there, then continues live.
  Replay covers the whole day.
- **Unsequenced Data** is only valid after Login Accepted. It isn't numbered, and isn't
  replayed: a client that loses its connection must work out from the replayed
  Sequenced Data which of its messages arrived.
- **Heartbeats.** Each side sends a heartbeat whenever it hasn't sent anything else for
  1 s. Either side treats roughly 15 s without receiving anything as a lost connection.
- **Logout.** On a Logout Request the server closes the connection immediately.
- **End of Session.** The server sends End of Session when the session's stream for the
  day is finished, then closes the connection.

---

## OUCH 5.0

Every OUCH message is one fixed-layout core followed by an **Optional Appendage**.

### Optional Appendage

| Field | Length | Type | Notes |
|---|---|---|---|
| Appendage Length | 2 | Integer | Total bytes of the TagValue elements that follow |
| TagValue elements | variable | | |

Each TagValue element:

| Field | Length | Type | Notes |
|---|---|---|---|
| Length | 1 | Integer | Bytes that follow: the OptionTag plus the value |
| OptionTag | 1 | Integer | Which option (Appendix A) |
| Value | variable | | Width fixed by the tag |

A tag that isn't present takes its default. On a Replace, an absent tag means "same as
the original order", not "reset to default".

### Inbound Messages (client -> exchange)

| Type | Message | Scope |
|---|---|---|
| `O` | Enter Order | In scope |
| `U` | Replace Order Request | In scope |
| `X` | Cancel Order Request | In scope |
| `M` | Modify Order Request | **Out of scope** |
| `C` | Mass Cancel Request | **Out of scope** |
| `D` | Disable Order Entry Request | **Out of scope** |
| `E` | Enable Order Entry Request | **Out of scope** |
| `Q` | Account Query Request | **Out of scope** |

#### Enter Order (`O`)

| Field | Offset | Length | Type | Values |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `O` |
| UserRefNum | 1 | 4 | Integer | Greater than every UserRefNum used before on this port today |
| Side | 5 | 1 | Alpha | `B` buy, `S` sell. **Out of scope:** `T` sell short, `E` sell short exempt |
| Quantity | 6 | 4 | Integer | Greater than 0 and less than 1,000,000 |
| Symbol | 10 | 8 | Alpha | |
| Price | 18 | 8 | Price | Limit price, or the market-order sentinel |
| Time In Force | 26 | 1 | Alpha | `0` Day, `3` IOC. **Out of scope:** `5` GTX, `6` GTT, `E` after hours |
| Display | 27 | 1 | Alpha | `Y` visible. **Out of scope:** `N` hidden, `A` attributable |
| Capacity | 28 | 1 | Alpha | `A` agency, `P` principal, `R` riskless, `O` other. Informational |
| Intermarket Sweep Eligibility | 29 | 1 | Alpha | `N`. **Out of scope:** `Y` (ISO) |
| Cross Type | 30 | 1 | Alpha | `N` continuous market. **Out of scope:** every cross type |
| ClOrdID | 31 | 14 | Alpha | Echoed back |
| Appendage Length | 45 | 2 | Integer | |
| Optional Appendage | 47 | variable | | See [Optional Appendage Fields](#optional-appendage-fields-appendix-a) |

#### Replace Order Request (`U`)

| Field | Offset | Length | Type | Values |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `U` |
| Orig UserRefNum | 1 | 4 | Integer | The order being replaced |
| UserRefNum | 5 | 4 | Integer | New UserRefNum for the replacement; same rule as Enter Order |
| Quantity | 9 | 4 | Integer | Total shares for the order, **including shares already executed** |
| Price | 13 | 8 | Price | Limit price; a replace can't make a market order |
| Time In Force | 21 | 1 | Alpha | As Enter Order |
| Display | 22 | 1 | Alpha | As Enter Order |
| Intermarket Sweep Eligibility | 23 | 1 | Alpha | As Enter Order |
| ClOrdID | 24 | 14 | Alpha | |
| Appendage Length | 38 | 2 | Integer | |
| Optional Appendage | 40 | variable | | |

The replacement keeps the original's Side, Symbol, Capacity, Cross Type, Firm and Group
ID. A Side tag (27) may be sent, but it must match the original's side.

#### Cancel Order Request (`X`)

| Field | Offset | Length | Type | Values |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `X` |
| UserRefNum | 1 | 4 | Integer | The order being canceled |
| Quantity | 5 | 4 | Integer | New intended order size, **including shares already executed**; `0` cancels everything left |
| Appendage Length | 9 | 2 | Integer | |
| Optional Appendage | 11 | variable | | |

### Outbound Messages (exchange -> client)

| Type | Message | Scope |
|---|---|---|
| `A` | Order Accepted | In scope |
| `U` | Order Replaced | In scope |
| `C` | Order Canceled | In scope |
| `E` | Order Executed | In scope |
| `J` | Order Rejected | In scope |
| `S` | System Event | **Out of scope** (no trading sessions) |
| `D` | AIQ Canceled | **Out of scope** (no self-match prevention) |
| `B` | Broken Trade | **Out of scope** |
| `P` | Cancel Pending | **Out of scope** (crosses) |
| `I` | Cancel Reject | **Out of scope** (crosses) |
| `T` | Order Priority Update | **Out of scope** (pegs, reserves) |
| `M` | Order Modified | **Out of scope** (Modify Order) |
| `R` | Order Restated | **Out of scope** (reserves) |
| `X` | Mass Cancel Response | **Out of scope** |
| `G` | Disable Order Entry Response | **Out of scope** |
| `K` | Enable Order Entry Response | **Out of scope** |
| `Q` | Account Query Response | **Out of scope** |

#### Order Accepted (`A`)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `A` |
| Timestamp | 1 | 8 | Timestamp | |
| UserRefNum | 9 | 4 | Integer | |
| Side | 13 | 1 | Alpha | |
| Quantity | 14 | 4 | Integer | |
| Symbol | 18 | 8 | Alpha | |
| Price | 26 | 8 | Price | |
| Time In Force | 34 | 1 | Alpha | |
| Display | 35 | 1 | Alpha | |
| Order Reference Number | 36 | 8 | Integer | Exchange-assigned |
| Capacity | 44 | 1 | Alpha | |
| Intermarket Sweep Eligibility | 45 | 1 | Alpha | |
| Cross Type | 46 | 1 | Alpha | |
| Order State | 47 | 1 | Alpha | `L` live, `D` dead |
| ClOrdID | 48 | 14 | Alpha | |
| Appendage Length | 62 | 2 | Integer | |
| Optional Appendage | 64 | variable | | The order's options |

#### Order Replaced (`U`)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `U` |
| Timestamp | 1 | 8 | Timestamp | |
| Orig UserRefNum | 9 | 4 | Integer | The order that was replaced; now dead |
| UserRefNum | 13 | 4 | Integer | The replacement |
| Side | 17 | 1 | Alpha | |
| Quantity | 18 | 4 | Integer | Shares open on the replacement |
| Symbol | 22 | 8 | Alpha | |
| Price | 30 | 8 | Price | |
| Time In Force | 38 | 1 | Alpha | |
| Display | 39 | 1 | Alpha | |
| Order Reference Number | 40 | 8 | Integer | New, exchange-assigned |
| Capacity | 48 | 1 | Alpha | |
| Intermarket Sweep Eligibility | 49 | 1 | Alpha | |
| Cross Type | 50 | 1 | Alpha | |
| Order State | 51 | 1 | Alpha | `L` live, `D` dead |
| ClOrdID | 52 | 14 | Alpha | |
| Appendage Length | 66 | 2 | Integer | |
| Optional Appendage | 68 | variable | | |

#### Order Canceled (`C`)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `C` |
| Timestamp | 1 | 8 | Timestamp | |
| UserRefNum | 9 | 4 | Integer | |
| Decrement Shares | 13 | 4 | Integer | Shares canceled by this message (incremental, not cumulative) |
| Reason | 17 | 1 | Alpha | See [Cancel Reasons](#cancel-reasons-appendix-b) |
| Appendage Length | 18 | 2 | Integer | |
| Optional Appendage | 20 | variable | | |

#### Order Executed (`E`)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `E` |
| Timestamp | 1 | 8 | Timestamp | |
| UserRefNum | 9 | 4 | Integer | |
| Quantity | 13 | 4 | Integer | Shares executed by this message (incremental) |
| Price | 17 | 8 | Price | Execution price |
| Liquidity Flag | 25 | 1 | Alpha | See [Liquidity Flags](#liquidity-flags-appendix-e) |
| Match Number | 26 | 8 | Integer | Shared with the other side of the trade |
| Appendage Length | 34 | 2 | Integer | |
| Optional Appendage | 36 | variable | | |

#### Order Rejected (`J`)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Type | 0 | 1 | Alpha | `J` |
| Timestamp | 1 | 8 | Timestamp | |
| UserRefNum | 9 | 4 | Integer | |
| Reason | 13 | 2 | Integer | See [Reject Reasons](#reject-reasons-appendix-d); numeric in OUCH 5.0 |
| ClOrdID | 15 | 14 | Alpha | |
| Appendage Length | 29 | 2 | Integer | |
| Optional Appendage | 31 | variable | | |

### Order Handling

**Enter Order**
- A UserRefNum that isn't greater than the last one used on the port is a duplicate,
  and the message is ignored.
- Otherwise the order is answered with Order Accepted or Order Rejected, and either way
  it uses up the UserRefNum.
- An accepted order trades first, as the aggressor, against the resting book in
  price-time priority. Each fill executes at the resting order's price.
- A Day limit order rests with whatever didn't fill. Whatever an IOC or market order
  doesn't fill is canceled (reason `I`).

**Cancel Order**
- Quantity is the new intended size of the whole order, counting shares already
  executed. The open shares drop to `Quantity - executed`, and `0` cancels everything
  left.
- A partial cancel keeps the order's time priority.
- The only response is Order Canceled, with the shares removed.
- A cancel that wouldn't remove any shares, or that names an order that's unknown or
  already dead, is ignored and gets no response.

**Replace Order**
- The order is canceled and re-entered under the new UserRefNum, with a new Order
  Reference Number. The replacement **loses time priority** and trades like any incoming
  order.
- Quantity counts shares already executed, the same way as on a cancel. If that leaves
  no open shares, the order is canceled instead.
- If the original is unknown or dead, or the new UserRefNum has already been used, the
  replace is ignored, and the new UserRefNum is **not** used up.
- If the replace is invalid (a bad price, quantity, time in force or option, or a side
  that doesn't match), the existing order is **canceled** instead (Order Canceled), and
  the new UserRefNum is not used up.
- Otherwise the response is Order Replaced, followed by any executions.

**Executions**
- Both sides of a trade get Order Executed, with the same Match Number.

### Optional Appendage Fields (Appendix A)

| Tag | Field | Width | On | Scope |
|---|---|---|---|---|
| 2 | Firm | 4 | Enter | In scope: the MPID to attribute; blank = the port's default |
| 4 | CustomerType | 1 | Enter | In scope: `R` retail, `N` not retail; blank = the port's default |
| 24 | GroupID | 2 | Enter | In scope: customer group; `0` = none |
| 27 | Side | 1 | Replace | In scope: must match the original order's side |
| 1 | SecondaryOrdRefNum | 8 | — | **Out of scope** |
| 3 | MinQty | 4 | Enter, Replace | **Out of scope** |
| 5 | MaxFloor | 4 | Enter, Replace | **Out of scope** (reserve orders) |
| 6 | PriceType | 1 | Enter, Replace | **Out of scope** beyond `L` limit (pegs, midpoint, ...) |
| 7 | PegOffset | 4 | Enter, Replace | **Out of scope** |
| 9 | DiscretionPrice | 8 | Enter, Replace | **Out of scope** |
| 10 | DiscretionPriceType | 1 | Enter, Replace | **Out of scope** |
| 11 | DiscretionPegOffset | 4 | Enter, Replace | **Out of scope** |
| 12 | PostOnly | 1 | Enter, Replace | **Out of scope** |
| 13 | RandomReserves | 4 | Enter, Replace | **Out of scope** |
| 14 | Route | 4 | Accepted | **Out of scope** (no routing) |
| 15 | ExpireTime | 4 | Enter, Replace | **Out of scope** |
| 16 | TradeNow | 1 | Enter, Replace | **Out of scope** |
| 17 | HandleInst | 1 | Enter, Replace | **Out of scope** |
| 18 | BBO Weight Indicator | 1 | Accepted, Replaced | **Out of scope** |
| 22 | DisplayQuantity | 4 | — | **Out of scope** |
| 23 | DisplayPrice | 8 | — | **Out of scope** |
| 25 | SharesLocated | 1 | Enter, Replace | **Out of scope** (short sales) |
| 26 | LocateBroker | 4 | Enter, Replace | **Out of scope** (short sales) |
| 28 | UserRefIdx | 1 | All | **Out of scope** (order-flow channels) |
| 29 | AIQ Strategy | 1 | Enter, Replace | **Out of scope** (self-match prevention) |
| 30 | AIQ Group ID | 2 | Enter, Replace | **Out of scope** |

An out-of-scope option is accepted only at its default value. An Enter Order that asks
for an out-of-scope feature is rejected, and a Replace that does is treated as invalid.

### Cancel Reasons (Appendix B)

| Code | Meaning | Scope |
|---|---|---|
| `U` | User requested | In scope |
| `I` | Immediate or cancel: the unfilled remainder of an IOC or market order | In scope |
| others | Timeout, supervisory, regulatory, self-match prevention, system, cross, halt, ... | **Out of scope** |

### Reject Reasons (Appendix D)

In-scope rejects (numeric codes):

| Code | Meaning | When |
|---|---|---|
| `0x0003` | Invalid Display | Display other than `Y` |
| `0x0004` | Invalid Max Floor | MaxFloor set |
| `0x0005` | Invalid Peg Type | PriceType other than `L`, or peg/discretion fields set |
| `0x0008` | ISO not allowed | Intermarket Sweep Eligibility `Y` |
| `0x0009` | Invalid Side | Side other than `B` / `S` |
| `0x000A` | Processing Error | The exchange can't accept the order right now |
| `0x000D` | Invalid Min Quantity | MinQty set |
| `0x000F` | Other | Unsupported time in force, capacity, or option |
| `0x0013` | Invalid Quantity | Quantity 0, or 1,000,000 or more |
| `0x0014` | Invalid Cross Order | Cross Type other than `N` |
| `0x001D` | Invalid Price | Off-tick, or above the maximum limit price |

An Enter Order for an unknown symbol is also rejected. The rest of Appendix D (halts,
risk limits, firm permissions, ...) is **out of scope**.

### Liquidity Flags (Appendix E)

| Code | Meaning | Scope |
|---|---|---|
| `A` | Added: the resting side of the trade | In scope |
| `R` | Removed: the incoming side of the trade | In scope |
| others | Hidden, midpoint, retail, cross, routed, ... | **Out of scope** |

### Port Options

| Option | Scope |
|---|---|
| Cancel on disconnect: when the session's connection is lost, its open orders are canceled. Best effort, and a Logout Request doesn't trigger it | In scope |
| Default Firm and CustomerType | In scope |
| Order-flow channels (UserRefIdx) | **Out of scope** |
| Risk / throttle settings | **Out of scope** |

---

## ITCH 5.0

A one-way feed of every visible change to the book, for every order (it isn't
aggregated by price level). Order Reference Numbers and Match Numbers are the same ones
OUCH reports.

### Common Header

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Alpha | |
| Stock Locate | 1 | 2 | Integer | The symbol's locate code, from Stock Directory |
| Tracking Number | 3 | 2 | Integer | Nasdaq internal tracking number |
| Timestamp | 5 | 6 | Timestamp | |

Every message has a fixed length, and ITCH has no optional fields.

### Messages

| Type | Message | Scope |
|---|---|---|
| `R` | Stock Directory | In scope |
| `A` | Add Order (no MPID attribution) | In scope |
| `E` | Order Executed | In scope |
| `X` | Order Cancel | In scope |
| `D` | Order Delete | In scope |
| `U` | Order Replace | In scope |
| `S` | System Event | **Out of scope** (no trading sessions) |
| `H` | Stock Trading Action | **Out of scope** (no halts) |
| `Y` | Reg SHO Restriction | **Out of scope** |
| `L` | Market Participant Position | **Out of scope** |
| `V` / `W` | MWCB Decline Level / Status | **Out of scope** |
| `K` | IPO Quoting Period Update | **Out of scope** |
| `J` | LULD Auction Collar | **Out of scope** |
| `h` | Operational Halt | **Out of scope** |
| `F` | Add Order with MPID Attribution | **Out of scope** (no attributable orders) |
| `C` | Order Executed With Price | **Out of scope** (only arises from hidden orders and crosses) |
| `P` | Trade (non-cross) | **Out of scope** (reports non-displayed executions; no hidden orders) |
| `Q` | Cross Trade | **Out of scope** |
| `B` | Broken Trade | **Out of scope** |
| `I` | NOII | **Out of scope** |
| `N` | RPII | **Out of scope** |
| `O` | Direct Listing with Capital Raise Price Discovery | **Out of scope** |

#### Stock Directory (`R`), 39 bytes

| Field | Offset | Length | Type |
|---|---|---|---|
| Header | 0 | 11 | |
| Stock | 11 | 8 | Alpha |
| Market Category | 19 | 1 | Alpha |
| Financial Status Indicator | 20 | 1 | Alpha |
| Round Lot Size | 21 | 4 | Integer |
| Round Lots Only | 25 | 1 | Alpha |
| Issue Classification | 26 | 1 | Alpha |
| Issue Sub-Type | 27 | 2 | Alpha |
| Authenticity | 29 | 1 | Alpha |
| Short Sale Threshold Indicator | 30 | 1 | Alpha |
| IPO Flag | 31 | 1 | Alpha |
| LULD Reference Price Tier | 32 | 1 | Alpha |
| ETP Flag | 33 | 1 | Alpha |
| ETP Leverage Factor | 34 | 4 | Integer |
| Inverse Indicator | 38 | 1 | Alpha |

Sent for each symbol at the start of the day, before any order messages, so subscribers
can map Stock Locate to symbol. Only Stock and the locate code in the header matter
here; the other fields are reference data.

#### Add Order (`A`), 36 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Header | 0 | 11 | | |
| Order Reference Number | 11 | 8 | Integer | |
| Buy/Sell Indicator | 19 | 1 | Alpha | `B` / `S` |
| Shares | 20 | 4 | Integer | |
| Stock | 24 | 8 | Alpha | |
| Price | 32 | 4 | Price | |

#### Order Executed (`E`), 31 bytes

| Field | Offset | Length | Type |
|---|---|---|---|
| Header | 0 | 11 | |
| Order Reference Number | 11 | 8 | Integer |
| Executed Shares | 19 | 4 | Integer |
| Match Number | 23 | 8 | Integer |

#### Order Cancel (`X`), 23 bytes

| Field | Offset | Length | Type |
|---|---|---|---|
| Header | 0 | 11 | |
| Order Reference Number | 11 | 8 | Integer |
| Canceled Shares | 19 | 4 | Integer |

#### Order Delete (`D`), 19 bytes

| Field | Offset | Length | Type |
|---|---|---|---|
| Header | 0 | 11 | |
| Order Reference Number | 11 | 8 | Integer |

#### Order Replace (`U`), 35 bytes

| Field | Offset | Length | Type |
|---|---|---|---|
| Header | 0 | 11 | |
| Original Order Reference Number | 11 | 8 | Integer |
| New Order Reference Number | 19 | 8 | Integer |
| Shares | 27 | 4 | Integer |
| Price | 31 | 4 | Price |

### Semantics

- **Add Order**: an order now rests on the book with that many shares. An incoming order
  that executes completely never appears as an Add.
- **Order Executed**: a resting order traded at its own displayed price. ITCH reports
  only the resting side; the incoming side isn't on the book.
- **Order Cancel**: a resting order lost some of its shares and keeps the rest (and its
  priority).
- **Order Delete**: a resting order is gone.
- **Order Replace**: the original order is gone, and the replacement rests at the back
  of its price level under a new Order Reference Number.
- A subscriber can rebuild every symbol's book from these messages alone.

---

## MoldUDP64 1.00

The transport that carries ITCH: sequenced UDP multicast, with a separate channel for
asking for missed packets again.

### Downstream Packet (exchange -> subscribers)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Session | 0 | 10 | Alpha | |
| Sequence Number | 10 | 8 | Integer | Sequence number of the first message in this packet |
| Message Count | 18 | 2 | Integer | Messages in this packet |
| Message Blocks | 20 | variable | | |

Each Message Block:

| Field | Length | Type |
|---|---|---|
| Message Length | 2 | Integer |
| Message Data | Message Length | one ITCH message |

- Messages are numbered from 1. Each message's sequence number is the packet's Sequence
  Number plus its position in the packet.
- **Heartbeat**: Message Count `0`, Sequence Number = the next message to be sent.
- **End of Session**: Message Count `0xFFFF`.

### Request Packet (subscriber -> retransmission server)

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Session | 0 | 10 | Alpha | |
| Sequence Number | 10 | 8 | Integer | First message wanted |
| Requested Message Count | 18 | 2 | Integer | How many messages |

The retransmission server sends the requested messages back as ordinary downstream
packets, unicast to whoever asked.

### Late Joiners

**Out of scope:** snapshot / Glimpse-style book recovery. A late joiner either replays
from message 1 via retransmission requests or starts with an incomplete book.
