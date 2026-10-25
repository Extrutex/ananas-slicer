# Virtual Klipper + Moonraker test rig for ANANAS SLICER G-code.
#
# One image serves three roles (see docker-compose.yml):
#   - Klipper batch mode: klippy.py <printer.cfg> -i <gcode> -o /dev/null -d <dict>
#   - Klipper live mode: the same file-output MCU, kept alive on an idle input
#     FIFO with the API socket open, so Moonraker sees a "ready" printer
#   - Moonraker, connected to that live Klippy
#
# The MCU data dictionary comes from building upstream Klipper for the Linux
# host-MCU target. No MCU process runs: Klippy writes its MCU command stream to
# /dev/null, which is upstream's own batch-mode mechanism.
#
# Versions are fetched by explicit ref and verified against a commit hash.
# A moved tag or branch fails the build instead of silently changing the rig.

ARG PYTHON_IMAGE=python:3.11.13-slim-bookworm@sha256:86adf8dbadc3d6e82ee5dd2c74bec2e1c2467cdad47886280501df722372d2e1

FROM ${PYTHON_IMAGE} AS builder

ARG KLIPPER_REPOSITORY=https://github.com/Klipper3d/klipper.git
ARG KLIPPER_TAG=v0.13.0
ARG KLIPPER_REF=${KLIPPER_TAG}
ARG KLIPPER_COMMIT=61c0c8d2ef40340781835dd53fb04cc7a454e37a
ARG MOONRAKER_TAG=v0.11.0
ARG MOONRAKER_COMMIT=985c1d0bbeb90bc057d34a232c9dc3b05e0c6c8d

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git libffi-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Klipper: pinned source, host-MCU dictionary, Klippy virtualenv, prebuilt chelper.
# Firmware sources, vendor libraries, docs and tests are dropped once the
# dictionary is built; Klippy needs only klippy/ at runtime.
RUN git init /opt/klipper \
    && git -C /opt/klipper remote add origin "${KLIPPER_REPOSITORY}" \
    && git -C /opt/klipper fetch --depth 1 origin "${KLIPPER_REF}" \
    && git -C /opt/klipper -c advice.detachedHead=false checkout --detach FETCH_HEAD \
    && test "$(git -C /opt/klipper rev-parse HEAD)" = "${KLIPPER_COMMIT}" \
    && git -C /opt/klipper describe --tags --always > /opt/klipper/klippy/.version \
    && printf 'CONFIG_MACH_LINUX=y\n' > /opt/klipper/.config \
    && make -C /opt/klipper olddefconfig \
    && make -C /opt/klipper -j"$(nproc)" \
    && mkdir -p /opt/klipper-dict \
    && cp /opt/klipper/out/klipper.dict /opt/klipper-dict/linux-host.dict \
    && rm -rf /opt/klipper/out /opt/klipper/.git /opt/klipper/.config /opt/klipper/.config.old \
    && rm -rf /opt/klipper/lib /opt/klipper/src /opt/klipper/docs /opt/klipper/test

# Exact versions of every Python package, transitive ones included.
COPY klippy-constraints.txt moonraker-constraints.txt /tmp/constraints/

RUN python -m venv /opt/klippy-env \
    && /opt/klippy-env/bin/pip install --no-cache-dir \
        -c /tmp/constraints/klippy-constraints.txt -r /opt/klipper/scripts/klippy-requirements.txt \
    && /opt/klippy-env/bin/python -c \
        "import sys; sys.path.insert(0, '/opt/klipper/klippy'); import chelper; chelper.get_ffi()"

# Moonraker: pinned source and virtualenv.
RUN git -c advice.detachedHead=false clone --depth 1 --branch "${MOONRAKER_TAG}" \
        https://github.com/Arksine/moonraker.git /opt/moonraker \
    && test "$(git -C /opt/moonraker rev-parse HEAD)" = "${MOONRAKER_COMMIT}" \
    && git -C /opt/moonraker describe --tags --always > /opt/moonraker/moonraker/.version \
    && rm -rf /opt/moonraker/.git \
    && python -m venv /opt/moonraker-env \
    && cd /opt/moonraker/scripts \
    && /opt/moonraker-env/bin/pip install --no-cache-dir \
        -c /tmp/constraints/moonraker-constraints.txt -r moonraker-requirements.txt

FROM ${PYTHON_IMAGE}

# libsodium: Moonraker's libnacl loads it at runtime. iproute2: Moonraker's
# machine component reads network interfaces with `ip`.
RUN apt-get update \
    && apt-get install -y --no-install-recommends iproute2 libsodium23 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt /opt

# Fails the build if Klippy would try to recompile its C helper at runtime
# (there is no compiler in this stage).
RUN /opt/klippy-env/bin/python -c \
        "import sys; sys.path.insert(0, '/opt/klipper/klippy'); import chelper; chelper.get_ffi()"

# Shared data directory for Klippy (virtual_sdcard, API socket, logs) and
# Moonraker. World-writable because the rig runs as the invoking host user so
# files written to bind mounts stay owned by that user.
RUN mkdir -p /printer_data/config /printer_data/gcodes /printer_data/logs \
        /printer_data/comms /printer_data/database \
    && chmod -R 1777 /printer_data

ENV KLIPPY_DICT=/opt/klipper-dict/linux-host.dict \
    PYTHONDONTWRITEBYTECODE=1
