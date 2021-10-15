FROM fedora:latest

RUN dnf install --nogpg -y dnf-plugins-core git gnome-desktop-testing dbus-daemon python3-twisted python3-gobject procps-ng bzip2 httpd mod_ssl openssh-server passwd gcc-c++ \
 && dnf builddep --nogpg -y gvfs \
 && dnf clean all

RUN dnf builddep --nogpg -y glib \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/glib.git \
 && cd glib \
 && meson . _build --prefix=/usr \
 && ninja -C _build \
 && ninja -C _build install \
 && cd .. \
 && rm -rf glib

RUN dnf builddep --nogpg -y glib-networking \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/glib-networking.git \
 && cd glib-networking \
 && meson . _build --prefix=/usr \
 && ninja -C _build \
 && ninja -C _build install \
 && cd .. \
 && rm -rf glib-networking

RUN dnf builddep --nogpg -y libsoup \
 && dnf install -y --nogpg libnghttp2-devel \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/libsoup.git \
 && cd libsoup \
 && meson . _build --prefix=/usr \
 && ninja -C _build \
 && ninja -C _build install \
 && cd .. \
 && rm -rf libsoup

RUN sed -i -e 's/# %wheel/%wheel/' /etc/sudoers
RUN useradd -G wheel -m user
RUN passwd -d user
USER user
WORKDIR /home/user
ENV USER user
ENV XDG_RUNTIME_DIR /home/user

RUN ssh-keygen -t rsa -q -N "" -f ~/.ssh/id_rsa
