SUMMARY = "toolcal - tool resource calibration for the CPE Agent (lab use)"
DESCRIPTION = "Runs each CPE Agent diagnostic tool in its own cgroup and measures \
CPU, peak memory, tasks, disk IO, wall time, output size and the CPU it induces \
in CCSP daemons, under idle, typical and stressed load. Writes a threshold report \
with a budget, class and verdict per tool. Intended for lab and debug images only."
SECTION = "devel"

# Proprietary by default. To publish the component (for example to RDK Central),
# add a LICENSE file to the source tree and switch to:
#   LICENSE = "Apache-2.0"
#   LIC_FILES_CHKSUM = "file://LICENSE;md5=<md5sum of that file>"
LICENSE = "CLOSED"

# --- Source -------------------------------------------------------------------
# Default: the sources bundled next to this recipe (files/toolcal/).
SRC_URI = "file://toolcal/"
S = "${WORKDIR}/toolcal"
# Yocto 5.1 (styhead) and later unpack local files into UNPACKDIR instead:
#   S = "${UNPACKDIR}/toolcal"
#
# Alternative: build from your component repository in the RDK CMF layout.
#   SRC_URI = "${CMF_GIT_ROOT}/rdkb/components/generic/toolcal;protocol=${CMF_GIT_PROTOCOL};branch=${CMF_GIT_BRANCH};name=toolcal"
#   SRCREV_toolcal = "${AUTOREV}"
#   SRCREV_FORMAT = "toolcal"
#   S = "${WORKDIR}/git"

# --- Build --------------------------------------------------------------------
# Link the platform cJSON instead of the vendored copy. CFLAGS, LDFLAGS and
# CPPFLAGS come from the BitBake environment, so hardening and GNU_HASH apply.
DEPENDS = "cjson"
EXTRA_OEMAKE = "'CC=${CC}' CJSON=system"

do_compile() {
    oe_runmake toolcal
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/toolcal            ${D}${bindir}/toolcal
    install -m 0755 ${S}/run_calibration.sh ${D}${bindir}/run_calibration.sh

    install -d ${D}${datadir}/toolcal/tests
    install -m 0644 ${S}/examples/tools.example.json ${D}${datadir}/toolcal/
    install -m 0644 ${S}/tests/selftest.json         ${D}${datadir}/toolcal/tests/
    install -m 0644 ${S}/README.md                   ${D}${datadir}/toolcal/
}

# --- Packaging ----------------------------------------------------------------
FILES:${PN} += "${datadir}/toolcal"

# run_calibration.sh needs /bin/sh plus awk, sed, grep, id, date and sleep.
# BusyBox (the usual base-utils provider on RDK-B) supplies all of them;
# setsid and pkill are used when present but are not required.
RDEPENDS:${PN} = "${VIRTUAL-RUNTIME_base-utils}"

# Optional: real stress-ng (meta-oe) for the stressed condition.
# Without it, the built-in "toolcal hog" generates the load.
# RRECOMMENDS:${PN} = "stress-ng"
