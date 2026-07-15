# Reliable UDP Media Transport

A reliable real-time media transport system implemented in C for unreliable UDP networks. This project was developed as part of a systems programming assignment to deliver fixed-size media frames across a relay that introduces packet loss, delay, duplication, and reordering.

## Overview

The objective is to reliably deliver 160-byte media frames while maintaining low latency and acceptable network overhead. The implementation is designed to tolerate common network impairments using forward error correction and receiver-side buffering without relying on retransmissions.

## Features

- Reliable delivery over unreliable UDP
- Forward Error Correction (FEC) based recovery
- Receiver-side jitter buffer
- Packet reordering support
- Duplicate packet detection
- Low-latency media playback
- Standard C implementation using POSIX sockets

## Project Structure

```
.
├── sender.c
├── receiver.c
├── Makefile
├── RUNLOG.md
├── NOTES.md
├── SUMMARY.html
├── common.py
├── relay.py
├── endpoints.py
├── run.py
├── score.py
└── profiles/
```

## Build

Compile the project using:

```bash
make
```

This generates:

```
./sender
./receiver
```

## Running

Execute the provided testing harness.

Example:

```bash
python3 run.py --profile profiles/A.json --delay_ms 100 --duration 30
```

or

```bash
python3 run.py --profile profiles/B.json --delay_ms 100 --duration 30
```

## Design Summary

The implementation follows a forward error correction based design instead of retransmission to satisfy real-time constraints.

Key components include:

- Forward Error Correction (FEC) for packet recovery
- Receiver-side jitter buffering
- Sequence number based packet ordering
- Duplicate packet filtering
- Deadline-aware frame delivery

This approach minimizes playback interruptions while keeping network overhead within assignment limits.

## Deliverables

- `sender.c`
- `receiver.c`
- `Makefile`
- `RUNLOG.md`
- `NOTES.md`
- `SUMMARY.html`

## Technologies Used

- C
- POSIX Sockets
- UDP Networking
- GNU Make
- Python Test Harness (provided)

## Repository

```text
Reliable UDP Media Transport
Systems Programming Assignment
```

## License

This repository is intended for academic and educational purposes.
