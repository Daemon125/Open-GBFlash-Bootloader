# The update wire protocol

Byte-exact. This is what the bootloader answers and what an updater must send.

The protocol was reverse-engineered from FlashGBX's open-source host implementation,
cross-checked against an independent third-party project's documentation of it and
against a script published by the original hardware author, and then confirmed on
silicon by four complete firmware updates — **on one device, a PCB v1.3 with a CH579M.**
See [RELEASE-NOTES.md](RELEASE-NOTES.md) for exactly what that does and does not
establish.

---

## Transport

The bootloader enumerates as a **CH340 serial device, VID `1A86` PID `7523`**, with the
same descriptors the application presents — device descriptor, configuration descriptor
set and the canned CH340 vendor-reply table are all reproduced verbatim from the
application image, because the host's `ch341`/`ch34x` driver binds on exactly those
bytes.

| endpoint | type | wMaxPacketSize | use |
|---|---|---|---|
| `0x02` | bulk OUT | 32 | host -> device |
| `0x82` | bulk IN | 32 | device -> host |
| `0x81` | interrupt IN | 8 | declared, never driven |

`bMaxPacketSize0` is **8**, not 64. That is what the application reports and it is
reproduced as-is. Hosts open the port at 2 Mbaud. There is no line-coding negotiation
that matters; this is a vendor-class device, not CDC-ACM.

---

## Framing

All multi-byte fields on the wire are **big-endian**. (The boot-info record in flash is
little-endian. The flip at that boundary is a real trap; see [DESIGN.md](DESIGN.md).)

```
 offset  size  field
   0      4    intro marker   48 48 4A 4A     ("HHJJ")
   4      1    sender
   5      2    sequence number
   7      2    command
   9      2    payload length
  11    plen   payload
  11+n    4    outro marker   4A 4A 48 48     ("JJHH")
  15+n    1    pad 0x00       -- present iff the frame length so far is odd
```

The fixed part is **15 bytes**, so the pad is present exactly when the payload length is
**even**.

- **The pad belongs to the whole frame and sits after the outro**, not inside the
  payload and not before the outro.
- Every response the bootloader builds has an **odd** payload length (9, 3 or 1), so
  **no response is ever padded**. That is required, not incidental: no known host reads
  a pad byte, so an even response payload length would desynchronise the host by one byte
  on the following frame.
- Every frame a host sends has an **even** payload length (0, 4, or `chunk_len + 6` with
  `chunk_len` even), so **every host frame is padded**.

The bootloader does not consume the pad explicitly. It returns to hunting for the intro
marker the instant the outro completes, and the pad is swallowed harmlessly by the hunt
window. A host that omitted the pad would therefore still work — whereas explicitly
consuming one byte would cost the first byte of the next intro.

### Sender and sequence

`sender` is `0x00` in every response the bootloader emits. No host inspects it.

`sequence` and `command` are **echoed verbatim** in the response. Both known hosts check
both.

---

## CRC16

**CRC-16/MODBUS**: reflected polynomial `0xA001`, initial value `0xFFFF`, no final XOR,
no output reflection. Nibble-table form in this implementation.

Known-answer vectors:

| input | CRC |
|---|---|
| `""` (empty) | `0xFFFF` |
| `"\x00"` | `0x40BF` |
| `"\xFF"` | `0x00FF` |
| `"123456789"` | `0x4B37` |
| `"LFBG"` | `0xF387` |
| bytes `0x00..0xFF` | `0xDE6C` |
| `0xAA` × 511 | `0x3F2E` |

It appears in three places: over each data chunk, over the whole image at finalize, and
inside the boot-info record (once over the application payload, once over the record's
own first 12 bytes).

---

## Commands

Three, and only three.

| command | payload length | meaning |
|---|---|---|
| `0x0021` | 0 | initialise session |
| `0x0024` | `chunk_len + 6` | write one firmware data packet |
| `0x0023` | 4 | finalise |

Anything else is consumed whole and **answered with silence**. That is deliberate: a
future command's payload shape is not this bootloader's to guess, and a NAK to an unknown
command would be a lie.

---

### `0x21` — initialise session

**Request payload:** none. The whole frame is 16 bytes:

```
48 48 4A 4A  ss  00 01  00 21  00 00  4A 4A 48 48  00
|__intro__|  |   |seq|  |cmd|  |len|  |__outro__|  pad
           sender
```

**Response payload: exactly 9 bytes.**

| offset | size | value | meaning |
|---|---|---|---|
| `[0]` | 1 | `0x00` | never read by any known host |
| `[1:3]` | 2 | `0x0003` | **hard gate** — hosts refuse to proceed without it |
| `[3:5]` | 2 | `0x3E00` | `program_size` — parsed by every host and then referenced by none |
| `[5:7]` | 2 | `0x0000` | never read |
| `[7:9]` | 2 | `0x0200` | `page_size` — **drives the host's chunking** |

Full response frame, 24 bytes:

```
48 48 4A 4A  00  ss ss  00 21  00 09  00 00 03 3E 00 00 00 02 00  4A 4A 48 48
```

`page_size = 0x0200` is forced by the host's roughly 100 ms response window: one
512-byte sector per data packet is what fits inside it. A larger page would mean more
sectors erased and programmed per packet than the window allows.

`program_size` is **not** an image-size limit. No host checks the firmware length
against it. The size limit is enforced by the length gate at finalize.

This command is answered **statelessly and unconditionally**, and it begins (or
restarts) a session. No flash is touched. A repeated sequence number is never an error.

---

### `0x24` — write firmware data packet

**Request payload:**

| offset | size | field |
|---|---|---|
| `[0:2]` | u16 BE | `index`, **1-based** |
| `[2:4]` | u16 BE | `chunk_len` |
| `[4 : 4+chunk_len]` | | `chunk` |
| `[4+chunk_len : 6+chunk_len]` | u16 BE | CRC16 of `chunk` |

so `payload_len == chunk_len + 6`.

**There is no address on the wire.** The destination is derived:

```
dest = 0x3E00 + (index - 1) * page_size
```

**Packet 1 must be exactly 512 bytes.** It is the boot-info page. An image shorter than
that has no application at all.

**Response payload: 3 bytes.**

| offset | size | field |
|---|---|---|
| `[0:2]` | u16 BE | `index`, echoed |
| `[2]` | 1 | `0x01` = written, `0x00` = refused |

Full response frame, 18 bytes.

**Ordering.** On packet 1 the boot-info sector at `0x3E00` is erased **first**, before
anything else happens. The packet's contents are then buffered in RAM and are **not**
programmed until finalize. Packets 2..N erase and program their own sector as they
arrive, and each is read back and CRC-checked immediately.

**A retransmission of the packet just accepted is re-acknowledged, not rewritten** —
same index, same length, same CRC. Rewriting would double-count the streaming CRC and
corrupt the finalize comparison.

A packet is refused (`0x00`) when: the declared length and the payload length disagree;
`chunk_len` is 0 or exceeds `page_size`; the chunk CRC does not match; the session is
already finalized; the index is not the expected one; the image already ended with a
short chunk; the destination would run past the image bound; the erase or the program
failed; or the read-back did not match.

---

### `0x23` — finalise

**Request payload: 4 bytes.**

| offset | size | field |
|---|---|---|
| `[0:2]` | u16 BE | CRC16 of the whole image, every byte of every accepted chunk |
| `[2:4]` | u16 BE | the bitwise inverse of that CRC |

The two must XOR to `0xFFFF`.

**Response payload: 1 byte** — `0x01` success, `0x00` failure. Full response frame, 16
bytes.

Everything the bootloader checks before it answers `0x01`:

1. `crc ^ inverse == 0xFFFF`;
2. the session is not already finalized (a *repeat* of the same CRC is answered `0x01`
   again — it is idempotent, because host retries are not drained);
3. the boot-info page was received and at least one application packet followed;
4. the streaming image CRC matches the CRC in this payload;
5. the buffered boot-info record passes the same gates the boot decision applies — tag,
   record CRC, length window, and the payload CRC field against what actually streamed
   past;
6. the application's initial SP and reset vector, captured off the wire as they streamed
   past, pass the boot decision's vector-table gates;
7. **the application region is read back out of flash** and its CRC recomputed against
   the record's field, and the initial SP re-tested against silicon rather than against
   the wire. About 33 ms for the stock 30,496-byte image;
8. the boot-info record is programmed, tail first, with the header-CRC word withheld;
   everything except that word is read back and compared **while the record is still
   invalid**; then the header-CRC word is written, and that single word write — ~27 µs
   typical, ~36 µs worst case — is the commit.

Only then does the bootloader answer `0x01`.

Step 6 exists because the writer must never answer `SUCCESS` for an image the boot
decision will afterwards refuse forever. A `0x00` answer after packet 1 has already
erased the boot-info sector leaves the device in update mode with no valid application —
recoverable, but only by running the update again.

**After a successful finalize** the bootloader waits for 250 ms of receive silence
(restarted by any further inbound traffic, so a retransmitted finalize is absorbed rather
than interrupted), then validates the freshly written image and issues `SYSRESETREQ`. The
device disconnects cleanly and comes back running the new firmware. See
[DESIGN.md](DESIGN.md) for why this is a reset and not a branch.

---

## Framer behaviour

**Hunting.** Outside a frame, the framer runs a rolling 4-byte window looking for the
intro marker. Anything that is not part of an intro is discarded. This is what makes
stray bytes before a frame harmless, and what absorbs the pad byte trailing every host
frame. Overlapping candidates are handled correctly by the shift register.

**Implausible headers are rejected immediately**, before the declared payload is
swallowed:

| command | accepted payload length |
|---|---|
| `0x21` | exactly 0 |
| `0x23` | exactly 4 |
| `0x24` | at least 6 |
| anything else | not length-checked; the `520`-byte buffer bound still applies |

This matters more than it looks. The motivating case: a 16-byte init frame cut off after
10 bytes leaves the header one byte short, so the *next* frame's leading `0x48` becomes
the low half of the payload length. The command is still `0x21` but the length is now
`0x0048`, and a naive framer would consume 72 more payload bytes plus an outro — five
whole init frames. Both known hosts send only two and then report that no device was
found.

`0x24`'s lower bound is 6 rather than 7 on purpose, so a `chunk_len` of 0 still reaches
the handler and is answered with a refusal rather than with silence.

**A framing failure does not discard the bytes the bad frame consumed.** They are
rescanned for the *last* intro marker in the buffer and the frame is restarted there.
"Last" rather than "first" because it is the frame the host most recently sent — the one
whose answer the host is still waiting for — and because no intro can follow it, so a
replay can complete at most one frame. At most one response is ever produced by one fed
byte, rescan included.

**Idle abandonment is mandatory.** After roughly 50 ms with no byte received while a
frame is part-assembled, the frame is abandoned. Without it, a host killed part-way
through a data frame leaves a legal header with up to 522 bytes outstanding (518 payload
bytes plus the 4-byte outro, for a full 512-byte data frame), and the framer eats
whatever arrives next to satisfy the count — including the reconnecting
host's init frames, which both hosts send exactly two of before giving up. The device
would look dead until unplugged. The rescan cannot see this case, because a frame that
is still *waiting* has not failed.

50 ms sits comfortably between the shortest legitimate mid-frame gap (a host stalling
inside one 534-byte write, about 2.6 ms of wire time at 2 Mbaud) and the 100 ms the hosts
leave between frames.

---

## Where hosts differ

The bootloader tolerates all of the following. Any reimplementation should too.

**FlashGBX sends `0x21` twice**, both with sequence number 1, and throws the first
response away. `getserial.py` sends it once. A repeated sequence number is never an error
here, and `0x21` is answered statelessly, so both work. The second `0x21` also restarts
the session, which is what lets a host that gave up mid-update simply start over with no
power cycle.

**FlashGBX emits stray trigger bytes.** It writes `F1` / `01` before entering update
mode, and **re-sends them afterwards** from its own `WriteFirmware` / `TryConnect` paths
after the bootloader reset. Those bytes arrive with no frame around them. The intro hunt
discards them. A framer that treated unexpected bytes as an error would fail against
FlashGBX and only against FlashGBX.

**Both of those paths are exercised on hardware**, by the FlashGBX GUI update in the
verified set.

**Neither host reads the pad byte.** Do not send an even-length response payload.

**FlashGBX reports success on a failed finalize.** Its error handling returns success
where it should return failure, so its success dialog is not evidence that an update
worked. Verify by reading flash back. The bootloader still answers honestly — it must
not lie just because one host would not notice.

---

## Buffer sizes

For anyone reimplementing the device side:

| | |
|---|---|
| largest inbound payload | `2 + 2 + 512 + 2 = 518` bytes (a data packet) |
| inbound payload buffer | 520 bytes |
| largest outbound frame | 24 bytes |
| outbound frame buffer | 28 bytes |
| rescan buffer | `7 + 520 + 4` bytes — every post-intro byte of the largest acceptable frame |

The advertised `page_size` and the inbound buffer are coupled: a data packet is always
`page_size + 6` bytes of payload, so raising `page_size` past the buffer would make the
framer silently drop every data frame at the length gate and the device would look dead.
This implementation fails the build instead.
