FROM fedora:latest

RUN dnf install --nogpg -y dnf-plugins-core git gnome-desktop-testing dbus-daemon python3-twisted python3-gobject procps-ng bzip2 httpd mod_ssl openssh-server passwd gcc-c++ gcr-devel \
 && dnf builddep --nogpg -y gvfs \
 && dnf clean all

RUN dnf install --nogpg -y rest0.7-devel uhttpmock-devel gobject-introspection-devel \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/jbrummer/msgraph \
 && cd msgraph \
 && meson . _msgraph --prefix=/usr -Dgtk_doc=false \
 && ninja -C _msgraph \
 && ninja -C _msgraph install \
 && cd .. \
 && rm -rf msgraph

RUN sed -i -e 's/# %wheel/%wheel/' /etc/sudoers
RUN useradd -G wheel -m user
RUN passwd -d user
USER user
WORKDIR /home/user
ENV USER user
ENV XDG_RUNTIME_DIR /home/user

RUN ssh-keygen -t rsa -q -N "" -f ~/.ssh/id_rsa
