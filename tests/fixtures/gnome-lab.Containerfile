FROM docker.io/library/ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    gnome-shell gnome-session-bin gnome-settings-daemon xwayland xvfb xauth xdotool x11-utils \
    dbus-daemon dbus-x11 python3-gi gir1.2-gtk-4.0 libgl1-mesa-dri \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
# No host display, session bus, runtime directory or device mount is needed.
# Ubuntu's packages may create an empty seats marker although logind is not running here.
CMD ["/bin/sh", "-c", "if [ -d /run/systemd/seats ]; then rmdir /run/systemd/seats; fi; mkdir -p /run/dbus; dbus-daemon --system --fork; exec sleep infinity"]
