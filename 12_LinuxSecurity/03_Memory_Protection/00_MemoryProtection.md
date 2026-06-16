## 🔑 3. Memory Protection Features

* **ASLR (Address Space Layout Randomization)** → makes exploits harder.
* **NX / DEP (No Execute bit)** → prevents execution from data pages.
* **Stack canaries** → detect stack buffer overflows.
* **Fortify Source / Kernel hardening configs** → (`CONFIG_FORTIFY_SOURCE`, `CONFIG_STACKPROTECTOR`).
* **Read-Only Kernel Memory** → certain kernel regions locked after boot.