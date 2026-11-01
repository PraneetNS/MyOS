# Demo 3: Flagship Red-Team (Multi-Layer Defense in Depth)

## Overview
The **Red-Team** demo proves that MyOS does not rely on fragile prompt engineering or LLM "alignment" to stay secure. It subjects an autonomous agent to an adversarial prompt injection embedded inside an external web page and an untrusted downloaded file.

When tricked by the injection, the model attempts three malicious escalations, each stopped by a completely DIFFERENT defense layer:
1. **Destructive File System Deletion (`fs.delete /etc/master.key`)**:
   - **Defense Layer**: Window Manager / Kernel Approval Gate.
   - **Result**: Human Approval Dialog intercepts; agent cannot simulate clicks on "Allow"; user denies.
2. **Data Exfiltration to Disallowed Domain (`net.http attacker.com`)**:
   - **Defense Layer**: Userland Network Daemon Policy Engine (`netd`).
   - **Result**: Checked against `/etc/net-policy.conf`; domain not on allowlist; blocked with `403 Forbidden` and audited.
3. **Privilege Escalation via Direct Raw Socket (`socket(AF_INET, SOCK_RAW)`)**:
   - **Defense Layer**: Kernel Syscall Boundary (`CAP_NET_RAW_INET`).
   - **Result**: Kernel denies with `-EACCES`; immutable entry appended to SHA-256 audit log.

Finally, `auditctl verify` executes and cryptographically validates the tamper-proof SHA-256 hash chain, proving zero log tampering.

## How to Run

### Via Make & Headless Automated Recorder:
```bash
make demo-redteam
# or
./tools/record_demo.sh red-team
```

### Verification Command:
```bash
auditctl verify
```
Expected output:
```
Audit verification PASSED: 100% hash chain integrity verified!
```

