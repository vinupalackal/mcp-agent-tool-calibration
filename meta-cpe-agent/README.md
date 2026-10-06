# meta-cpe-agent

Yocto layer with the CPE Agent's lab tooling for RDK-B platforms. It currently provides one recipe, **toolcal**, the tool resource calibration framework.

toolcal is a **lab and debug** tool: it runs commands as root and deliberately stresses the device. Add it to lab or debug images only, never to production images.

## Contents

```text
meta-cpe-agent/
├── conf/layer.conf
├── recipes-tools/toolcal/
│   ├── toolcal_1.0.0.bb                  recipe
│   └── files/toolcal/                    sources (C, wrapper, catalogs, README)
└── kernel-config/
    ├── toolcal-cgroups.cfg               kernel options toolcal uses
    └── linux-%.bbappend.sample           how to apply them to a kernel recipe
```

## What gets installed

| Path on the device | What it is |
| --- | --- |
| `/usr/bin/toolcal` | Calibration binary, linked against the platform `libcjson` |
| `/usr/bin/run_calibration.sh` | Wrapper for idle, typical and stressed runs |
| `/usr/share/toolcal/tools.example.json` | Example RDK-B catalog to copy and edit |
| `/usr/share/toolcal/tests/selftest.json` | Self-test catalog |
| `/usr/share/toolcal/README.md` | Usage reference |

## 1. Add the layer

Copy `meta-cpe-agent` next to your other layers, then either:

```sh
bitbake-layers add-layer ../meta-cpe-agent
```

or add it to `conf/bblayers.conf`:

```text
BBLAYERS += "${TOPDIR}/../meta-cpe-agent"
```

RDK-B builds set their layers from the repo manifest. To make the layer permanent, add it to your manifest and to the `bblayers.conf` template your setup script uses.

Requirements: `meta-oe` (for `cjson`), which RDK-B builds already include. Compatible with dunfell, kirkstone and scarthgap.

## 2. Build

```sh
bitbake toolcal
```

## 3. Put it in a lab image

**Option A: one-off lab build.** In `conf/local.conf`:

```text
IMAGE_INSTALL:append = " toolcal"
```

**Option B: controlled by a distro feature,** so production builds can never pick it up. In a bbappend for your image recipe (for example `rdk-generic-broadband-image.bbappend`):

```text
IMAGE_INSTALL:append = " ${@bb.utils.contains('DISTRO_FEATURES', 'toolcal', 'toolcal', '', d)}"
```

Then turn it on only in lab builds, in `local.conf`:

```text
DISTRO_FEATURES:append = " toolcal"
```

**Option C: no reflash.** Build the recipe and copy the two files to a running device:

```sh
scp tmp/work/<arch>/toolcal/1.0.0-r0/image/usr/bin/toolcal \
    tmp/work/<arch>/toolcal/1.0.0-r0/image/usr/bin/run_calibration.sh  root@<device>:/tmp/
```

If the image has `opkg`, you can install the package from `tmp/deploy/ipk/<arch>/` instead.

## 4. Kernel support

toolcal measures most accurately with cgroups and PSI. Check a device with:

```sh
cat /proc/mounts | grep cgroup ; ls /proc/pressure
```

If they are missing, enable the options in `kernel-config/toolcal-cgroups.cfg`:

- **kernel-yocto style kernels:** use `linux-%.bbappend.sample` as described in the file.
- **SoC vendor kernels with a fixed defconfig:** add the options to that defconfig.

Without them, toolcal still works but falls back to rusage: memory covers only the largest single process and task counts are missing.

## 5. Check it on the device

```sh
toolcal list -c /usr/share/toolcal/tests/selftest.json
run_calibration.sh -c /usr/share/toolcal/tests/selftest.json -o /tmp/selftest -n 5 -m 20 -s 3
cat /tmp/selftest/threshold_report.md
```

Expected: three tools pass, four are flagged for review and two fail (an infinite loop and endless output, both stopped by toolcal). `/tmp` is usually RAM-backed on RDK-B, so copy the results off the device before rebooting.

## Notes

- **Override syntax.** The recipe uses the `:` override syntax (`FILES:${PN}`, `RDEPENDS:${PN}`). Dunfell supports it from 3.1.11. On older dunfell releases, change `:${PN}` to `_${PN}` and `:append` to `_append`.
- **Yocto 5.1 (styhead) and later:** set `S = "${UNPACKDIR}/toolcal"` in the recipe.
- **Building from git:** the recipe has a commented `SRC_URI` for the RDK CMF repository layout (`CMF_GIT_ROOT`, `CMF_GIT_BRANCH`).
- **Licence.** The recipe declares `LICENSE = "CLOSED"`. To publish the component, add a `LICENSE` file and switch to `Apache-2.0` with `LIC_FILES_CHKSUM`, as the recipe comments show.
- **stress-ng.** Optional. Uncomment `RRECOMMENDS:${PN} = "stress-ng"` to use it for the stressed condition. Otherwise the built-in `toolcal hog` creates the load.
