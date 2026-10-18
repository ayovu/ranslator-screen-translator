FROM docker.io/library/ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    sway wl-clipboard grim xdg-desktop-portal xdg-desktop-portal-wlr xdg-desktop-portal-gtk \
    pipewire dbus-daemon dbus-x11 python3 python3-gi gir1.2-gtk-4.0 \
    gir1.2-atspi-2.0 at-spi2-core fonts-dejavu-core \
    libegl1 libgl1 libglx0 libglvnd0 libgl1-mesa-dri \
    && rm -rf /var/lib/apt/lists/*
# Deliberately omit libopengl0: the portable bundle must provide its generic loader.
WORKDIR /work
# Use a private bus, headless outputs and pixman; never mount host session sockets/devices.
CMD ["sleep", "infinity"]
