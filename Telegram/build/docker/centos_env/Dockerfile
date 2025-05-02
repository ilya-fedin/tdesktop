# syntax=docker/dockerfile:1

FROM rockylinux:8 AS builder
ENV LANG=C.UTF-8
ENV TOOLSET=gcc-toolset-14
ENV PATH=/opt/rh/$TOOLSET/root/usr/bin:$PATH
ENV LIBRARY_PATH=/opt/rh/$TOOLSET/root/usr/lib64:/opt/rh/$TOOLSET/root/usr/lib:/usr/local/lib64:/usr/local/lib:/lib64:/lib:/usr/lib64:/usr/lib
ENV LD_LIBRARY_PATH=$LIBRARY_PATH

RUN dnf -y install epel-release \
	&& dnf config-manager --set-enabled powertools \
	&& dnf -y install cmake autoconf autoconf-archive automake libtool pkgconfig \
		make patch git python3.11-pip python3.11-devel gperf flex bison clang \
		clang-tools-extra lld nasm yasm file which zip perl-open perl-XML-Parser \
		perl-IPC-Cmd perl-Time-Piece xorg-x11-util-macros $TOOLSET-gcc $TOOLSET-gcc-c++ \
		$TOOLSET-binutils $TOOLSET-gdb $TOOLSET-libasan-devel libffi-devel \
		fontconfig-devel freetype-devel libX11-devel wayland-devel alsa-lib-devel \
		pulseaudio-libs-devel mesa-libGL-devel mesa-libEGL-devel mesa-libgbm-devel \
		libdrm-devel vulkan-devel libva-devel libvdpau-devel libselinux-devel \
		libmount-devel systemd-devel glib2-devel gobject-introspection-devel \
		at-spi2-core-devel gtk3-devel \
	&& dnf clean all

RUN alternatives --set python3 /usr/bin/python3.11
RUN python3 -m pip install conan meson ninja
RUN ls /opt/rh/$TOOLSET/root/usr/bin | xargs -i rm -rf /usr/bin/{}
RUN cat <<EOF > /usr/local/bin/pkg-config && chmod +x /usr/local/bin/pkg-config
#!/bin/sh
for i in "\$@"; do
	[ "\$i" = "--version" ] && exec /usr/bin/pkg-config "\$i"
done
export PKG_CONFIG_PATH="\${PKG_CONFIG_PATH:+\$PKG_CONFIG_PATH:}/opt/rh/\$TOOLSET/root/usr/lib64/pkgconfig:/opt/rh/\$TOOLSET/root/usr/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig"
exec /usr/bin/pkg-config --static "\$@"
EOF
RUN sed -i '/CMAKE_${lang}_FLAGS_DEBUG_INIT/s/")/ -O0 {% if LTO %}-fno-lto -fno-use-linker-plugin{% endif %} -fuse-ld=lld")/' /usr/share/cmake/Modules/Compiler/GNU.cmake
RUN sed -i 's/NO_DEFAULT_PATH//g; s/PKG_CONFIG_ALLOW_SYSTEM_LIBS/PKG_CONFIG_IS_DUMB/g' /usr/share/cmake/Modules/FindPkgConfig.cmake
RUN sed -i 's/set(OpenGL_GL_PREFERENCE "")/set(OpenGL_GL_PREFERENCE "LEGACY")/' /usr/share/cmake/Modules/FindOpenGL.cmake
RUN sed -i '/Requires.private: valgrind/d' /usr/lib64/pkgconfig/libdrm.pc
RUN sed -i 's/-lharfbuzz//' /usr/lib64/pkgconfig/harfbuzz.pc
RUN sed -i 's/-lpng16//' /usr/lib64/pkgconfig/libpng16.pc
RUN curl -sSL https://raw.githubusercontent.com/desktop-app/patches/{{ PATCHES }}/gobject-introspection.patch | patch -p1 -d /usr/lib64/gobject-introspection
RUN echo set debuginfod enabled on > /opt/rh/$TOOLSET/root/etc/gdbinit.d/00-debuginfod.gdb
RUN adduser user

WORKDIR /usr/src
ENV PKG_CONFIG=/usr/local/bin/pkg-config
ENV AR=gcc-ar
ENV RANLIB=gcc-ranlib
ENV NM=gcc-nm
ENV CFLAGS='{% if DEBUG %}-g{% endif %} -O3 {% if LTO %}-flto=auto -ffat-lto-objects{% endif %} -pipe -fPIC -fno-strict-aliasing -fexceptions -fasynchronous-unwind-tables -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fhardened -Wno-hardened'
ENV CXXFLAGS=$CFLAGS
ENV LDFLAGS='-static-libstdc++ -static-libgcc -static-libasan -pthread -Wl,--push-state,--no-as-needed,-ldl,--pop-state -Wl,--as-needed -Wl,-z,muldefs'

ENV CMAKE_GENERATOR=Ninja
ENV CMAKE_BUILD_TYPE=None
ENV CMAKE_BUILD_PARALLEL_LEVEL='{{ JOBS }}'

RUN git init Implib.so \
	&& cd Implib.so \
	&& git remote add origin https://github.com/yugr/Implib.so.git \
	&& git fetch --depth=1 origin ecf7bb51a92a0fb16834c5b698570ab25f9f1d21 \
	&& git reset --hard FETCH_HEAD \
	&& mkdir build \
	&& cd build \
	&& implib() { \
		LIBFILE=$(basename $1); \
		LIBNAME=$(basename $1 .so); \
		../implib-gen.py -q $1; \
		gcc $CFLAGS -c -o $LIBFILE.tramp.o $LIBFILE.tramp.S; \
		gcc $CFLAGS -c -o $LIBFILE.init.o $LIBFILE.init.c; \
		ar rcs /usr/local/lib64/$LIBNAME.a $LIBFILE.tramp.o $LIBFILE.init.o; \
	} \
	&& implib /usr/lib64/libgtk-3.so \
	&& implib /usr/lib64/libgdk-3.so \
	&& implib /usr/lib64/libgdk_pixbuf-2.0.so \
	&& implib /usr/lib64/libpango-1.0.so \
	&& implib /usr/lib64/libvdpau.so \
	&& implib /usr/lib64/libva-x11.so \
	&& implib /usr/lib64/libva-drm.so \
	&& implib /usr/lib64/libva.so \
	&& implib /usr/lib64/libEGL.so \
	&& implib /usr/lib64/libGL.so \
	&& implib /usr/lib64/libdrm.so \
	&& implib /usr/lib64/libwayland-egl.so \
	&& implib /usr/lib64/libwayland-cursor.so \
	&& implib /usr/lib64/libwayland-client.so \
	&& implib /usr/lib64/libwayland-server.so \
	&& implib /usr/lib64/libX11-xcb.so \
	&& implib /usr/lib64/libxcb.so \
	&& cd ../.. \
	&& rm -rf Implib.so

FROM builder AS xcb-proto
RUN git clone -b xcb-proto-1.16.0 --depth=1 https://github.com/gitlab-freedesktop-mirrors/xcbproto.git \
	&& cd xcbproto \
	&& ./autogen.sh \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-proto-cache install \
	&& cd .. \
	&& rm -rf xcbproto

FROM builder AS xcb
COPY --link --from=xcb-proto /usr/src/xcb-proto-cache /

RUN git clone -b libxcb-1.16 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxcb.git \
	&& cd libxcb \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& export DESTDIR=/usr/src/xcb-cache \
	&& make install \
	&& rm $DESTDIR/usr/local/lib/{libxcb.{,l}a,pkgconfig/xcb.pc} \
	&& cd .. \
	&& rm -rf libxcb

FROM builder AS xcb-wm
RUN git clone -b xcb-util-wm-0.4.2 --depth=1 --recursive --shallow-submodules https://github.com/gitlab-freedesktop-mirrors/libxcb-wm.git \
	&& cd libxcb-wm \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-wm-cache install \
	&& cd .. \
	&& rm -rf libxcb-wm

FROM builder AS xcb-util
RUN git clone -b xcb-util-0.4.1-gitlab --depth=1 --recursive --shallow-submodules https://github.com/gitlab-freedesktop-mirrors/libxcb-util.git \
	&& cd libxcb-util \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-util-cache install \
	&& cd .. \
	&& rm -rf libxcb-util

FROM builder AS xcb-image
COPY --link --from=xcb-util /usr/src/xcb-util-cache /

RUN git clone -b xcb-util-image-0.4.1-gitlab --depth=1 --recursive --shallow-submodules https://github.com/gitlab-freedesktop-mirrors/libxcb-image.git \
	&& cd libxcb-image \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-image-cache install \
	&& cd .. \
	&& rm -rf libxcb-image

FROM builder AS xcb-keysyms
RUN git init libxcb-keysyms \
	&& cd libxcb-keysyms \
	&& git remote add origin https://github.com/gitlab-freedesktop-mirrors/libxcb-keysyms.git \
	&& git fetch --depth=1 origin ef5cb393d27511ba511c68a54f8ff7b9aab4a384 \
	&& git reset --hard FETCH_HEAD \
	&& git submodule update --init --recursive --depth=1 \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-keysyms-cache install \
	&& cd .. \
	&& rm -rf libxcb-keysyms

FROM builder AS xcb-render-util
RUN git init libxcb-render-util \
	&& cd libxcb-render-util \
	&& git remote add origin https://github.com/gitlab-freedesktop-mirrors/libxcb-render-util.git \
	&& git fetch --depth=1 origin 5ad9853d6ddcac394d42dd2d4e34436b5db9da39 \
	&& git reset --hard FETCH_HEAD \
	&& git submodule update --init --recursive --depth=1 \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-render-util-cache install \
	&& cd .. \
	&& rm -rf libxcb-render-util

FROM builder AS xcb-cursor
COPY --link --from=xcb-util /usr/src/xcb-util-cache /
COPY --link --from=xcb-image /usr/src/xcb-image-cache /
COPY --link --from=xcb-render-util /usr/src/xcb-render-util-cache /

RUN git init libxcb-cursor \
	&& cd libxcb-cursor \
	&& git remote add origin https://github.com/gitlab-freedesktop-mirrors/libxcb-cursor.git \
	&& git fetch --depth=1 origin 4929f6051658ba5424b41703a1fb63f9db896065 \
	&& git reset --hard FETCH_HEAD \
	&& git submodule update --init --recursive --depth=1 \
	&& ./autogen.sh --enable-static --disable-shared --with-cursorpath='~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps' \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcb-cursor-cache install \
	&& cd .. \
	&& rm -rf libxcb-cursor

FROM builder AS xext
RUN git clone -b libXext-1.3.5 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxext.git \
	&& cd libxext \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xext-cache install \
	&& cd .. \
	&& rm -rf libxext

FROM builder AS xtst
RUN git clone -b libXtst-1.2.4 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxtst.git \
	&& cd libxtst \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xtst-cache install \
	&& cd .. \
	&& rm -rf libxtst

FROM builder AS xfixes
RUN git clone -b libXfixes-5.0.3 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxfixes.git \
	&& cd libxfixes \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xfixes-cache install \
	&& cd .. \
	&& rm -rf libxfixes

FROM builder AS xv
COPY --link --from=xext /usr/src/xext-cache /

RUN git clone -b libXv-1.0.12 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxv.git \
	&& cd libxv \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xv-cache install \
	&& cd .. \
	&& rm -rf libxv

FROM builder AS xrandr
RUN git clone -b libXrandr-1.5.3 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxrandr.git \
	&& cd libxrandr \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xrandr-cache install \
	&& cd .. \
	&& rm -rf libxrandr

FROM builder AS xrender
RUN git clone -b libXrender-0.9.11 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxrender.git \
	&& cd libxrender \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xrender-cache install \
	&& cd .. \
	&& rm -rf libxrender

FROM builder AS xdamage
RUN git clone -b libXdamage-1.1.6 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxdamage.git \
	&& cd libxdamage \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xdamage-cache install \
	&& cd .. \
	&& rm -rf libxdamage

FROM builder AS xcomposite
RUN git clone -b libXcomposite-0.4.6 --depth=1 https://github.com/gitlab-freedesktop-mirrors/libxcomposite.git \
	&& cd libxcomposite \
	&& ./autogen.sh --enable-static --disable-shared \
	&& make -j$(nproc) \
	&& make DESTDIR=/usr/src/xcomposite-cache install \
	&& cd .. \
	&& rm -rf libxcomposite

FROM builder AS pipewire
RUN git clone -b 0.3.62 --depth=1 https://github.com/PipeWire/pipewire.git \
	&& cd pipewire \
	&& meson build \
		--buildtype=plain \
		-Dtests=disabled \
		-Dexamples=disabled \
		-Dspa-plugins=disabled \
		-Dsession-managers= \
	&& meson compile -C build \
	&& DESTDIR=/usr/src/pipewire-cache meson install -C build \
	&& cd .. \
	&& rm -rf pipewire

FROM builder AS xkbcommon
COPY --link --from=xcb /usr/src/xcb-cache /

RUN git clone -b xkbcommon-1.6.0 --depth=1 https://github.com/xkbcommon/libxkbcommon.git \
	&& cd libxkbcommon \
	&& meson build \
		--buildtype=plain \
		--default-library=static \
		-Denable-docs=false \
		-Denable-wayland=false \
		-Denable-xkbregistry=false \
		-Dxkb-config-root=/usr/share/X11/xkb \
		-Dxkb-config-extra-path=/etc/xkb \
		-Dx-locale-root=/usr/share/X11/locale \
	&& meson compile -C build \
	&& DESTDIR=/usr/src/xkbcommon-cache meson install -C build \
	&& cd .. \
	&& rm -rf libxkbcommon

FROM builder
COPY --link --from=xcb /usr/src/xcb-cache /
COPY --link --from=xcb-wm /usr/src/xcb-wm-cache /
COPY --link --from=xcb-util /usr/src/xcb-util-cache /
COPY --link --from=xcb-image /usr/src/xcb-image-cache /
COPY --link --from=xcb-keysyms /usr/src/xcb-keysyms-cache /
COPY --link --from=xcb-render-util /usr/src/xcb-render-util-cache /
COPY --link --from=xcb-cursor /usr/src/xcb-cursor-cache /
COPY --link --from=xext /usr/src/xext-cache /
COPY --link --from=xfixes /usr/src/xfixes-cache /
COPY --link --from=xv /usr/src/xv-cache /
COPY --link --from=xtst /usr/src/xtst-cache /
COPY --link --from=xrandr /usr/src/xrandr-cache /
COPY --link --from=xrender /usr/src/xrender-cache /
COPY --link --from=xdamage /usr/src/xdamage-cache /
COPY --link --from=xcomposite /usr/src/xcomposite-cache /
COPY --link --from=pipewire /usr/src/pipewire-cache /
COPY --link --from=xkbcommon /usr/src/xkbcommon-cache /

ENV XDG_CACHE_HOME=/usr/src/tdesktop/.cache
ENV CONAN_HOME=/usr/src/tdesktop/.cache/conan
ENV VCPKG_ROOT=/usr/src/tdesktop/Telegram/ThirdParty/vcpkg
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
WORKDIR /usr/src/tdesktop
USER user
VOLUME [ "/usr/src/tdesktop" ]
CMD [ "/usr/src/tdesktop/Telegram/build/docker/centos_env/build.sh" ]
