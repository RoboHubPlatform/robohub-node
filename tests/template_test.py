#!/usr/bin/env python3
"""The node template takes the managed layer only when provisioned, on every
board it declares (RNODE-TPL-0001, RNODE-TPL-0003; moved from robohub-drivers'
DRV-TEMP-0005 test with the template, robohub ADR-0025).

Builds the template for each preset in temporary copies, as Mesh copies it: as
it is — no update key, so no managed layer in the image — and as Mesh would
provision it, with robohub_secret.h, robohub_update_key.h and
CONFIG_RH_NODE_MANAGED=y, so the image carries robohub_managed_init and the
Ed25519 check. A preset whose toolchain is not installed is skipped; with none,
the whole test is (77).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(os.path.dirname(HERE), "template")
WORKSPACE = os.environ.get("ROBOHUB_WORKSPACE", os.path.dirname(os.path.dirname(HERE)))

# Each preset, the program name its image takes, and the nm that reads it.
PRESETS = [
    ("pico2w", "arm-none-eabi-nm"),
    ("lolin32-lite", "xtensa-esp32-elf-nm"),
]


def build(root, preset, nm, provisioned):
    copy = os.path.join(root, "node_1")
    shutil.copytree(TEMPLATE, copy, ignore=shutil.ignore_patterns("build"))
    if provisioned:
        with open(os.path.join(copy, "src", "robohub_secret.h"), "w") as f:
            f.write("#pragma once\n#define RH_SECURE_PROVISIONED 1\n#define RH_DEVICE_KEY_EPOCH 0u\n"
                    "static const unsigned char RH_DEVICE_UID[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };\n"
                    "static const unsigned char RH_DEVICE_KEY[16] = { 9 };\n")
        with open(os.path.join(copy, "src", "robohub_update_key.h"), "w") as f:
            f.write("#pragma once\n#define RH_UPDATE_KEY_PROVISIONED 1\n"
                    "static const unsigned char RH_UPDATE_PUBLIC_KEY[32] = { 1 };\n")
        with open(os.path.join(copy, "project.conf"), "a") as f:
            f.write("CONFIG_RH_NODE_MANAGED=y\n")
    done = subprocess.run(["rho", "build", "-target", preset, "."], cwd=copy, capture_output=True, text=True,
                          env=dict(os.environ, ROBOHUB_WORKSPACE=WORKSPACE))
    if done.returncode != 0:
        print(done.stdout[-2000:], done.stderr[-2000:])
        return None
    elf = os.path.join(copy, "build", "output", "node_1.elf")
    if not os.path.exists(elf):
        print(f"{preset}: no image at build/output/node_1.elf")
        return None
    return subprocess.run([nm, elf], capture_output=True, text=True).stdout


def main():
    if not shutil.which("rho"):
        print("skipped: rho is not installed")
        return 77
    failures = []
    ran = 0
    for preset, nm in PRESETS:
        if not shutil.which(nm):
            print(f"skipped {preset}: {nm} is not installed")
            continue
        ran += 1
        with tempfile.TemporaryDirectory() as plain_root, tempfile.TemporaryDirectory() as managed_root:
            plain = build(plain_root, preset, nm, False)
            managed = build(managed_root, preset, nm, True)
        if plain is None or managed is None:
            failures.append(f"{preset}: the template did not build")
            continue
        if " T robohub_managed_init" in plain:
            failures.append(f"{preset}: an unprovisioned build carries the managed layer")
        if " T robohub_managed_init" not in managed or " T crypto_ed25519_check" not in managed:
            failures.append(f"{preset}: a provisioned build lacks the managed layer")
        if " T robohub_transport_report_health" not in plain:
            failures.append(f"{preset}: the image does not report its health")
        # The network contract is the platform's, not robohub-common's weak
        # "not supported" default (ESP-0010 on the LOLIN32 Lite).
        if not re.search(r" [RDrd] rh_net_platform$", plain, re.M) and preset == "lolin32-lite":
            failures.append(f"{preset}: the image links robohub-common's network defaults, not the platform's")
        if not failures:
            print(f"ok {preset}: built, reports its health, the managed layer only when provisioned")
    if ran == 0:
        print("skipped: no board toolchain is installed")
        return 77
    for f in failures:
        print("FAIL:", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
