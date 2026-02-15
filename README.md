<!-- style: font-family: Arial; font-size: small; -->

Thunderbolt Streaming Protocol (TBSP)



Develop a network protocol to accelerate data transfer for streaming over a macOS Thunderbolt bridge.

Raw Ethernet protocol (Layer 2, custom protocol)


- DriverKit system extension: Allocate DMA-capable memory and export it
- User-space library: Lock-free ring buffer, rdma_write() / rdma_read()
- Control plane: Handshake and memory exchange
- Layer 2 over Thunderbolt bridge
- EtherType: 0x88B5 (experimental use)
- Header: Sequence number, ACK, window size, timestamp
- Flow control: Sliding window
- Payload: Jumbo frames (MTU 9000 recommended)

Header definition

See [include/tbsp_common.h](include/tbsp_common.h):

- TBSP_ETHERTYPE, TBSP_VERSION, MAX_FRAME, WINDOW_SIZE, PAYLOAD_SIZE
- TBSPHeader: version, type (DATA=0, ACK=1), flags, stream_id, sequence, ack, payload_len, window, timestamp_ns
- now_ns() for monotonic timestamps

```mermaid
flowchart LR
  subgraph frame [TBSP Frame]
    ETH[Ethernet 14B]
    HDR[TBSPHeader]
    PL[Payload]
  end
  ETH --> HDR --> PL
```

---

Minimal sender

Blocking I/O, one frame at a time, sliding-window flow control.

- Source: [src/tbsp_sender.c](src/tbsp_sender.c)
- Usage: sudo ./sender &lt;interface&gt; [dest_mac]

---

Minimal receiver

Blocking I/O, in-order DATA processing, sends ACK per frame.

- Source: [src/tbsp_receiver.c](src/tbsp_receiver.c)
- Usage: sudo ./receiver &lt;interface&gt;

---

High-performance sender

Non-blocking, poll loop, batch send (e.g. 32 frames, 8192-byte payload).

- Source: [src/tbsp_sender_hp.c](src/tbsp_sender_hp.c)
- Usage: sudo ./sender_hp &lt;interface&gt;

---

High-performance receiver

Non-blocking, poll loop, ACK frames.

- Source: [src/tbsp_receiver_hp.c](src/tbsp_receiver_hp.c)
- Usage: sudo ./receiver_hp &lt;interface&gt;

---

Performance tips

- MTU: Use Jumbo Frames (e.g. ifconfig en5 mtu 9000)
- I/O: Non-blocking + polling
- Batching: Send multiple frames per loop (HP sender)
- Zero-copy: Use a ring buffer when integrating with an app
- CPU: Pin TX/RX threads to cores; separate send and receive paths
- ACKs: Aggregate ACKs (e.g. every N frames or time-based) to reduce overhead

---

Usage

Build

```bash
make
```

Produces: sender, receiver, sender_hp, receiver_hp.

### Run (root required)

Raw Ethernet on macOS requires root (BPF access).

Receiver first (e.g. on one machine or interface):

```bash
sudo ./receiver_hp en5
```

Sender (e.g. on the other Thunderbolt bridge end):

```bash
sudo ./sender_hp en5
```

For minimal tools:

```bash
sudo ./receiver en5
sudo ./sender en5
```

Note: Sender and receiver typically run on different hosts (Thunderbolt bridge) or use two different interfaces; one BPF device is bound to one interface at a time.

---

## Project layout

- TBSP/
  - README.md
  - Makefile
  - include/
    - tbsp_common.h (TBSP protocol header)
    - shared_region.h (Variant A reference)
  - src/
    - tbsp_sender.c
    - tbsp_receiver.c
    - tbsp_sender_hp.c
    - tbsp_receiver_hp.c
