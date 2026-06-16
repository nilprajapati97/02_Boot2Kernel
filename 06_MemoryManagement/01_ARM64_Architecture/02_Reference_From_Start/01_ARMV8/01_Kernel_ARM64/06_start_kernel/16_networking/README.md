# Phase 16: Networking Namespace Initialization

## Overview

Initializes the root network namespace (`init_net`), which provides the foundational infrastructure for all network operations in the kernel.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`net_ns_init()`](net_ns_init/README.md) | `net/core/net_namespace.c` | Root network namespace |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## Network Namespaces and Containers

Each network namespace has its own:
- Network interfaces (lo, eth0, etc.)
- Routing tables
- iptables/nftables rules
- Socket states
- `/proc/net/` subtree
- Port space (TCP/UDP ports)

Container runtimes (Docker, Kubernetes) use `CLONE_NEWNET` to give each container its own private networking.

## Function Index

- [net_ns_init/](net_ns_init/README.md)
