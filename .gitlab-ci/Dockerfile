FROM fedora:rawhide

RUN dnf install -y avahi-devel avahi-glib-devel dbus-glib-devel docbook-style-xsl fuse-devel gcc gcr-devel gettext-devel glib2-devel gnome-online-accounts-devel libarchive-devel libbluray-devel libcap-devel libcdio-paranoia-devel libexif-devel libgcrypt-devel libgdata-devel libgphoto2-devel libgudev-devel libimobiledevice-devel libmtp-devel libnfs-devel libplist-devel libsecret-devel libsmbclient-devel libsoup-devel libtalloc-devel libudisks2-devel libusb-devel libxslt-devel meson openssh-clients pkgconf-pkg-config polkit-devel systemd-devel \
 && dnf clean all

RUN dnf install -y --best elfutils-libelf-devel gamin-devel gcc gcc-c++ gdbm gettext git glibc-devel glibc-headers gtk-doc libattr-devel libffi-devel libmount-devel libselinux-devel ninja-build pcre-devel python3-devel systemtap-sdt-devel zlib-devel \
 && dnf clean all \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/glib.git \
 && cd glib \
 && meson . _build --prefix=/usr \
 && ninja -C _build \
 && ninja -C _build install \
 && cd .. \
 && rm -rf glib

# https://gitlab.com/BuildStream/buildstream-docker-images/blob/master/buildstream-fedora/prepare.sh
# https://gitlab.gnome.org/GNOME/gnome-build-meta/blob/master/.gitlab-ci.yml
RUN dnf install -y python3 fuse bubblewrap python3-pip python-setuptools python3-devel gcc-c++ bzr git lzip patch ostree python3-gobject quilt make findutils \
 && dnf clean all \
 && pip3 install arpy \
 && git clone https://gitlab.com/BuildStream/buildstream.git \
 && git -C buildstream/ checkout 10abe77fe8d77385d86f225b503d9185f4ef7f3a \
 && pip3 install buildstream/ \
 && git clone https://gitlab.com/BuildStream/bst-external.git \
 && git -C bst-external/ checkout 0.8.0-0-g762ea216b751bf89ac15350e1cb98542963bbf33 \
 && pip3 install bst-external/ \
 && git clone --depth 1 https://gitlab.gnome.org/GNOME/gnome-build-meta.git
