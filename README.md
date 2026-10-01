# UDP `recv()` vs `io_uring` benchmark

Linux/C++20 benchmark for comparing UDP receive throughput and CPU cost between a conventional `recv()` loop and `io_uring` on two computers on the same LAN.

The project includes:

- IPv4 UDP broadcast discovery, so the sender can find a receiver automatically.
- `recv` backend: one blocking `recv()` per datagram.
- `uring` backend: multishot `IORING_OP_RECV` with a provided-buffer ring.
- `uring-oneshot` backend: multiple ordinary one-shot io_uring receives, retained as a useful comparison.
- `sendmmsg()` sender shared by every receiver backend.
- `--pps` sender pacing so both backends can be tested at the **same offered packet rate**.
- `getrusage(RUSAGE_SELF)` CPU accounting split into user and system CPU.
- user/system/total nanoseconds per successfully received packet.
- voluntary/involuntary context-switch counts.
- `--once` and `--quiet` modes intended for `perf stat` runs.
- optional CPU affinity with `--cpu`.
- helper scripts for process-only and system-wide `perf` measurements.

## Why there are several CPU measurements

The built-in receiver statistics use `getrusage(RUSAGE_SELF)`:

- **user CPU**: time the benchmark spends executing in userspace.
- **system CPU**: kernel time charged to the benchmark process.
- **ns/packet**: CPU time divided by successfully received packets. This is usually a better efficiency metric than raw CPU percentage.

That is not the complete cost of receiving a network packet. NIC interrupt handling, NAPI and `NET_RX` softirq work may execute outside CPU time directly charged to `udp_bench`. For that reason the repository also includes `perf_system.sh`, and the README shows `mpstat`/`/proc/softirqs` checks below.

## Dependencies

### Arch / CachyOS / SteamOS development environment

```bash
sudo pacman -S --needed cmake ninja gcc liburing perf sysstat
```

Depending on your SteamOS environment, `perf` may be packaged separately or already available.

### Debian / Ubuntu

```bash
sudo apt install cmake ninja-build g++ liburing-dev linux-perf sysstat
```

The multishot backend requires a kernel and liburing with multishot recv and provided-buffer-ring support. The project requires liburing 2.4 or newer when its version can be detected through pkg-config.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Executable:

```bash
./build/udp_bench
```

## Basic throughput test

Computer B, normal `recv()`:

```bash
./build/udp_bench receiver --backend recv
```

Computer A:

```bash
./build/udp_bench sender --duration 15 --packet-size 1400
```

Then on Computer B:

```bash
./build/udp_bench receiver --backend uring --queue-depth 256
```

Run the identical sender command again.

`--backend uring` uses one multishot receive request with a provided-buffer ring. `--queue-depth` is the number of provided receive buffers and must be a power of two.

The older one-shot implementation is available as:

```bash
./build/udp_bench receiver --backend uring-oneshot --queue-depth 32
```

## Broadcast discovery

With a receiver running:

```bash
./build/udp_bench discover
```

The discovery client broadcasts on every active IPv4 broadcast-capable interface and also sends to `255.255.255.255`.

If VPN/firewall/VLAN configuration prevents discovery, specify the receiver directly:

```bash
./build/udp_bench sender --target 192.168.1.42
```

## CPU-efficiency test: use a fixed offered PPS

Do **not** compare CPU percentages if one backend is dropping substantially more packets than the other. A backend processing fewer packets is doing less work.

Instead choose a packet rate both implementations can sustain and keep the sender parameters identical.

For example, on the receiver:

```bash
./build/udp_bench receiver \
    --backend recv \
    --once \
    --quiet \
    --cpu 4
```

On the sender:

```bash
./build/udp_bench sender \
    --packet-size 64 \
    --pps 400000 \
    --duration 30 \
    --send-batch 16
```

Then repeat the receiver test with:

```bash
./build/udp_bench receiver \
    --backend uring \
    --queue-depth 256 \
    --once \
    --quiet \
    --cpu 4
```

and run **exactly the same sender command**.

The most useful receiver fields are:

```text
user CPU
system CPU
total process CPU
user ns/packet
system ns/packet
total ns/packet
```

`system ns/packet` is particularly useful when looking for a reduction in syscall/kernel work charged to the receiver process.

## Recommended PPS sweep

1400-byte datagrams are large enough that a 1 GbE link becomes the bottleneck at only roughly 85k packets/s. That can hide differences in receive overhead.

Use small packets and sweep the rate, for example:

```text
100000 pps
200000 pps
300000 pps
400000 pps
500000 pps
600000 pps
700000 pps
800000 pps
900000 pps
```

For each point:

1. Run `recv` for 20–30 seconds.
2. Run `uring` with identical sender settings.
3. Repeat each configuration several times.
4. Compare median CPU ns/packet only at rates where both runs have negligible packet loss.

A 64-byte benchmark UDP payload still contains the 32-byte benchmark header.

### Sender pacing and `--send-batch`

When `--pps` is specified, the sender paces the end of each `sendmmsg()` batch against an absolute schedule. Packets inside a batch are therefore a small burst.

Keep `--send-batch` the same for every comparison. `16` is a reasonable starting point. Use `--send-batch 1` if you want the smoothest offered traffic and the sender CPU can keep up.

## Process CPU with `perf stat`

On the receiver:

```bash
./scripts/perf_process.sh recv --cpu 4
```

Then start the sender on the other machine.

Repeat with:

```bash
./scripts/perf_process.sh uring --queue-depth 256 --cpu 4
```

The script records:

```text
task-clock
cycles:u
cycles:k
instructions:u
instructions:k
context-switches
cpu-migrations
```

Divide `cycles:k` and total cycles by the receiver's reported packet count if you want kernel cycles/packet and total cycles/packet.

## Kernel work outside the process

Some receive work can happen in interrupt/NAPI/softirq context and therefore isn't fully represented by process `ru_stime` or process-attached perf counters.

For a system-wide comparison on an otherwise idle receiver machine:

```bash
./scripts/perf_system.sh recv --cpu 4
```

then:

```bash
./scripts/perf_system.sh uring --queue-depth 256 --cpu 4
```

This runs `perf stat -a` and records kernel cycles across the receiver system while the one-shot benchmark process is alive. Because it is system-wide, unrelated activity is noise; keep the host idle and repeat runs.

For a profile showing where those kernel cycles go:

```bash
sudo perf record -a -g -e cycles:k -- ./build/udp_bench receiver --backend recv --once --quiet --cpu 4
# run the sender, then:
sudo perf report
```

Repeat with `--backend uring`.

Useful functions/stacks may include NIC driver/NAPI paths, UDP/IP receive processing, syscall receive paths, and io_uring internals.

## Softirq monitoring

Install `sysstat`, then in another terminal on the receiver:

```bash
mpstat -P ALL 1
```

Pay attention to `%sys`, `%irq`, `%soft` and `%idle`.

You can also inspect receive softirq activity:

```bash
watch -n1 'grep NET_RX /proc/softirqs'
```

And UDP receive-buffer drops:

```bash
nstat -az UdpRcvbufErrors UdpInErrors
```

Check those immediately before and after a run.

## CPU affinity

The receiver can pin its receive thread to a CPU:

```bash
./build/udp_bench receiver --backend recv --cpu 4
```

and the sender has the same option:

```bash
./build/udp_bench sender --target 192.168.1.42 --cpu 2
```

This makes repeated process measurements less noisy. It does **not** automatically move NIC IRQs, NAPI or RPS processing to that CPU. If you are doing detailed system-level profiling, inspect your NIC IRQ affinity and RPS/XPS settings as well.

## Socket buffers

The program requests a 16 MiB socket buffer by default. Linux may cap it according to the system settings. The receiver prints the actual `SO_RCVBUF` value returned by the kernel.

```bash
sysctl net.core.rmem_max net.core.wmem_max
```

Override the requested size with:

```bash
--socket-buffer 33554432
```

## Receiver backends

### `recv`

A conventional blocking loop:

```text
recv()
process datagram
recv()
process datagram
...
```

It intentionally does not use `recvmmsg()` because the test is intended to compare ordinary `recv()` against io_uring.

### `uring`

Uses `io_uring_prep_recv_multishot()` and a registered provided-buffer ring. One receive SQE can generate many CQEs. Consumed buffers are returned to the buffer ring in CQ batches.

### `uring-oneshot`

Keeps N independent ordinary `IORING_OP_RECV` operations outstanding and resubmits completed receives. This is retained to illustrate the cost of a naive io_uring design and is not the primary optimized comparison.

## Benchmark hygiene

For repeatable CPU results:

- Use wired Ethernet where possible.
- Keep packet size, PPS, `sendmmsg` batch size and test duration identical.
- Use `--quiet` for measured runs.
- Use `--once` when wrapping the receiver in `perf`.
- Pin the receiver to the same CPU each run.
- Keep the receiver machine otherwise idle.
- Run each point several times and compare medians.
- Ensure both backends receive approximately the same number of packets with negligible loss before comparing CPU/packet.
- Consider fixing CPU frequency/governor if you are comparing hardware cycles very closely.
