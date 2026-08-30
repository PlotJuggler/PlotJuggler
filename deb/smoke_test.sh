#!/bin/sh
#
# Install the built .deb in a clean distro image and prove it actually runs.
#
# Meant to be run INSIDE a bare container, with the directory holding the .deb
# mounted at /pkg:
#
#   docker run --rm -v "$PWD/deb:/pkg:ro" ubuntu:22.04 sh /pkg/smoke_test.sh
#
# This is the only check in the Linux release flow that exercises the package on
# a machine without build dependencies. `apt install` succeeding proves the
# declared Depends resolve; it does NOT prove the payload runs, because a
# too-new glibc/libstdc++ version symbol is burned into the ELF and no package
# name expresses it. Only running the binary catches that.
set -e
export DEBIAN_FRONTEND=noninteractive

# This automation deliberately launches the real GUI. Suppress its anonymous
# launch ping even when the caller does not forward CI into the container.
export PJ_DISABLE_TELEMETRY=1

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT HUP INT TERM

echo "### $(. /etc/os-release; echo "$PRETTY_NAME")"
apt-get update -qq
apt-get install -y -qq /pkg/plotjuggler4_*.deb >/dev/null
echo "--- installed ---"
dpkg -l plotjuggler4 | tail -1
echo "--- wrapper ---"
command -v plotjuggler4

# Declared via Depends for the wrapper's gRPC CA export and Qt TLS; assert the
# bundle actually landed where the wrapper looks first.
echo "--- CA bundle ---"
test -f /etc/ssl/certs/ca-certificates.crt || { echo "FAIL: ca-certificates bundle missing"; exit 1; }
echo "OK: /etc/ssl/certs/ca-certificates.crt present"

# Linkable-closure gate, BEFORE xvfb is installed: every DT_NEEDED of every
# shipped ELF must resolve with only the declared Depends present. Installing
# xvfb first would mask a missing X11 dependency (it pulls those libraries in
# itself), so this ordering is load-bearing. The ROS dist/ inners are exempt —
# they bind to the user's sourced ROS by design (same carve-out as the
# packaging scan and the release glibc audit). dlopen'ed libraries are outside
# ldd's view; the GUI phase below covers those.
echo "--- ldd closure against declared Depends only ---"
find /opt/plotjuggler4 -path '*/plugins/ros2-topic-subscriber/dist' -prune -o \
     -type f \( -name '*.so*' -o -path '*/bin/plotjuggler4' \) -print \
  | while IFS= read -r elf; do
      LD_LIBRARY_PATH=/opt/plotjuggler4/lib ldd "$elf" 2>/dev/null \
        | grep "not found" | sed "s|^|$elf: |" || true
    done > "${WORK}/ldd_missing.txt"
if [ -s "${WORK}/ldd_missing.txt" ]; then
  echo "FAIL: unresolved libraries with only the declared Depends installed:"
  cat "${WORK}/ldd_missing.txt"
  exit 1
fi
echo "OK: every shipped ELF resolves"

# The retro helper is launched as a child process and inherits the wrapper's
# LD_LIBRARY_PATH, but it is packaged to stand on its own RUNPATH — so inspect
# it with none set. The gate above skips it (neither a *.so* nor
# bin/plotjuggler4). Absent from a package built without --retro-wad, hence the
# presence test rather than a hard requirement; release CI asserts it shipped.
RETRO=/opt/plotjuggler4/bin/3rdparty/retro
if [ -d "${RETRO}" ]; then
  echo "--- retro payload ---"
  for f in pj-raster-helper base.wad COPYING SOURCE-OFFER.txt SHAREWARE-LICENSE.txt README.md; do
    test -f "${RETRO}/${f}" || { echo "FAIL: ${RETRO}/${f} missing"; exit 1; }
  done
  if ldd "${RETRO}/pj-raster-helper" | grep "not found"; then
    echo "FAIL: pj-raster-helper does not resolve without LD_LIBRARY_PATH"
    exit 1
  fi
  echo "OK: helper, data and licenses present; helper resolves standalone"
fi

# Display-free by design: main() short-circuits this before any QApplication
# exists, so it also proves the wrapper's PYTHONHOME reaches the bundled stdlib.
echo "--- selftest-python ---"
plotjuggler4 --selftest-python

# The bundled Qt ships only the xcb platform plugin, so constructing a
# QApplication needs a real X server. Xvfb provides one without a GPU or a seat,
# and exercises the shipped code path rather than a substitute platform plugin.
echo "--- GUI startup under Xvfb ---"
apt-get install -y -qq xvfb >/dev/null
set +e
xvfb-run -a --server-args="-screen 0 1280x800x24" \
  timeout 25 plotjuggler4 --test-data --nosplash >"${WORK}/gui.log" 2>&1
rc=$?
set -e
# 124 is timeout(1) reporting that it had to kill a still-healthy process.
if [ "$rc" = "124" ]; then
  echo "OK: ran 25s with a real window under Xvfb"
else
  echo "FAIL: exited early with rc=$rc"
  tail -60 "${WORK}/gui.log"
  exit 1
fi

echo "--- removal ---"
apt-get remove -y -qq plotjuggler4 >/dev/null
test ! -e /usr/bin/plotjuggler4 || { echo "FAIL: wrapper survived removal"; exit 1; }
test ! -d /opt/plotjuggler4     || { echo "FAIL: /opt tree survived removal"; exit 1; }
echo "OK: clean removal"
