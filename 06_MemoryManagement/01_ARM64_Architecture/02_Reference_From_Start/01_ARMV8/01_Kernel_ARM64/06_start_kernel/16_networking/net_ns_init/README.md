# `net_ns_init()` — Root Network Namespace

## Purpose

Initializes the root network namespace (`init_net`) and the net namespace slab cache. All network devices, protocols, and socket infrastructure run within a network namespace.

## Source File

`net/core/net_namespace.c`

```c
void __init net_ns_init(void)
{
    struct net_generic *ng;
    
    // Create slab for network namespaces:
#ifdef CONFIG_NET_NS
    net_cachep = kmem_cache_create("net_namespace", sizeof(struct net),
                                    SMP_CACHE_BYTES,
                                    SLAB_PANIC | SLAB_ACCOUNT, NULL);
    
    setup_net_ns_list_lock();
    INIT_LIST_HEAD(&net_namespace_list);
#endif
    
    // Initialize the initial (root) namespace:
    rcu_assign_pointer(init_net.gen, ng);
    
    // Run all namespace operations (register protocol families, etc.):
    if (setup_net(&init_net, &init_user_ns))
        panic("Could not setup initial network namespace");
    
    list_add_tail_rcu(&init_net.list, &net_namespace_list);
    
    register_pernet_subsys(&loopback_net_ops);
    register_pernet_subsys(&netfilter_net_ops);
}
```

## `struct net`

```c
struct net {
    // Reference counting:
    refcount_t          passive;
    spinlock_t          rules_mod_lock;
    
    // Unique ID:
    unsigned int        ns_capable;
    
    // Core network operations:
    struct list_head    dev_base_head;    // All network devices
    struct hlist_head   *dev_name_head;  // Lookup by name
    struct hlist_head   *dev_index_head; // Lookup by ifindex
    
    // Routing:
    struct netns_ipv4   ipv4;            // IPv4 state
    struct netns_ipv6   ipv6;            // IPv6 state
    
    // Sockets:
    struct sock         *rtnl;           // NETLINK_ROUTE socket
    
    // proc:
    struct proc_dir_entry *proc_net;     // /proc/net/
    struct proc_dir_entry *proc_net_stat;
    
    // Network filter:
    struct netns_nf     nf;
    
    // Per-ns network devices, protocols, etc.:
    struct list_head    list;            // In net_namespace_list
    struct work_struct  work;
};
```

## Network Namespace Operations

Protocols register `pernet_operations` to be notified when namespaces are created/destroyed:

```c
struct pernet_operations {
    struct list_head list;
    int (*init)(struct net *net);    // Called on namespace creation
    void (*exit)(struct net *net);   // Called before destruction
    void (*exit_batch)(struct list_head *net_exit_list);
    unsigned int *id;
    size_t size;
};

// Example: Register TCP for each namespace:
static struct pernet_operations tcp_sk_ops = {
    .init = tcp_sk_init,
    .exit = tcp_sk_exit,
    .exit_batch = tcp_sk_exit_batch,
};
register_pernet_subsys(&tcp_sk_ops);
```

## Loopback Interface

`register_pernet_subsys(&loopback_net_ops)` ensures every namespace gets a `lo` (loopback) interface automatically. The loopback is fundamental for inter-process communication within a namespace.

## `init_net` vs per-Container `net`

```bash
# View network namespaces:
ip netns list

# Create a new namespace:
ip netns add myns

# Run command in namespace:
ip netns exec myns ip addr show
```

Each `ip netns add` creates a new `struct net` using `copy_net_ns()`, initialized by all registered `pernet_operations`.

## Cross-references

- [Phase overview](../README.md)
- `rest_init()` → `kthreadd`: [../../19_rest_init/kthreadd/README.md](../../19_rest_init/kthreadd/README.md)
