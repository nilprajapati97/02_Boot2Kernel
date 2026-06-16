Perfect 👌 — yes, that’s exactly what we should do.
Let’s build a **tiny C daemon** that tries to write to `/var/log/hello.log`, and then confine it with SELinux so you can see enforcement in action.

---

# 🔹 Step 1: Write the C daemon

Create `mydaemon.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

int main(void) {
    FILE *fp;
    int i = 0;

    while (1) {
        fp = fopen("/var/log/hello.log", "a");
        if (fp == NULL) {
            perror("fopen");
            exit(1);
        }

        time_t now = time(NULL);
        fprintf(fp, "mydaemon: log entry %d at %s", i++, ctime(&now));
        fclose(fp);

        sleep(5); // log every 5 seconds
    }

    return 0;
}
```

Compile it on your build host:

```bash
gcc -static -o mydaemon mydaemon.c
```

Copy it to your rootfs:

```bash
cp mydaemon rootfs/usr/sbin/
```

---

# 🔹 Step 2: Label the binary

Give it a unique SELinux label:

```bash
chcon u:object_r:mydaemon_exec_t:s0 rootfs/usr/sbin/mydaemon
ls -Z rootfs/usr/sbin/mydaemon
```

---

# 🔹 Step 3: Create SELinux Policy

File: `mydaemon.te`

```te
policy_module(mydaemon, 1.0)

# Define new types
type mydaemon_t;
type mydaemon_exec_t;

# Tell SELinux this is a daemon domain
init_daemon_domain(mydaemon_t, mydaemon_exec_t)

# Allow daemon to create/write to log files
allow mydaemon_t var_log_t:file { create append write };
```

---

# 🔹 Step 4: Compile and install policy

```bash
checkmodule -M -m -o mydaemon.mod mydaemon.te
semodule_package -o mydaemon.pp -m mydaemon.mod
semodule -i mydaemon.pp
```

Verify:

```bash
semodule -l | grep mydaemon
```

---

# 🔹 Step 5: Run and test

Start daemon:

```bash
/usr/sbin/mydaemon &
```

### Case A: In **permissive mode**

```bash
setenforce 0
```

* The daemon will write to `/var/log/hello.log`.
* You’ll see denials in `dmesg` but not blocked:

  ```
  avc: denied { write } for pid=123 comm="mydaemon" ...
  ```

### Case B: In **enforcing mode**

```bash
setenforce 1
```

* If policy is correct → daemon writes normally.
* If policy is missing a rule → daemon fails (`fopen: Permission denied`), AVC denial logged.

---

✅ Now you have a **real, confined daemon** running in its own SELinux domain (`mydaemon_t`), only allowed to write logs.

---

Do you want me to also show you how to **auto-start this daemon via init/systemd in your BusyBox rootfs with correct SELinux domain transition**, so you don’t have to run it manually?
