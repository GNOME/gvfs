FROM fedora:rawhide

RUN dnf install -y --nogpg avahi-devel avahi-glib-devel dbus-glib-devel docbook-style-xsl fuse3-devel gcc gcr-devel gettext-devel glib2-devel gnome-online-accounts-devel gsettings-desktop-schemas-devel libarchive-devel libbluray-devel libcap-devel libcdio-paranoia-devel libexif-devel libgcrypt-devel libgphoto2-devel libgudev-devel libimobiledevice-devel libmtp-devel libnfs-devel libplist-devel libsecret-devel libsmbclient-devel libsoup-devel libtalloc-devel libudisks2-devel libusb-devel libxslt-devel meson openssh-clients pkgconf-pkg-config polkit-devel systemd-devel gnome-desktop-testing dbus-daemon python3-twisted python3-gobject procps-ng bzip2 httpd mod_ssl openssh-server passwd \
 && dnf clean all

RUN dnf install -y --nogpg --best elfutils-libelf-devel gcc gcc-c++ gdbm gettext git glibc-devel glibc-headers gtk-doc libattr-devel libffi-devel libmount-devel libselinux-devel ninja-build pcre-devel python3-devel systemtap-sdt-devel zlib-devel \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/glib.git \
 && cd glib \
 && meson . _build --prefix=/usr \
 && ninja -C _build \
 && ninja -C _build install \
 && cd .. \
 && rm -rf glib

RUN dnf install -y --nogpg uhttpmock-devel gobject-introspection-devel liboauth-devel vala json-glib-devel \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/libgdata.git \
 && cd libgdata \
 && meson . _build --prefix=/usr \
 && ninja -C _build \
 && ninja -C _build install \
 && cd .. \
 && rm -rf libgdata

RUN sed -i -e 's/# %wheel/%wheel/' /etc/sudoers
RUN useradd -G wheel -m user
RUN passwd -d user
USER user
WORKDIR /home/user
ENV USER user
ENV XDG_RUNTIME_DIR /home/user

RUN ssh-keygen -t rsa -q -N "" -f ~/.ssh/id_rsa
