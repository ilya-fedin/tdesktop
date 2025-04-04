from conan import ConanFile
from conan.tools.cmake import cmake_layout
import os


class Recipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def requirements(self):
        self.requires("ada/[*]")
        self.requires("boost/[*]")
        if self.settings.os == "Macos":
            self.requires("crashpad/[*]")
        else:
            self.requires("sentry-breakpad/[*]")
        self.requires("ffmpeg/[*]")
        if self.settings.os != "Macos":
            self.requires("hunspell/[*]")
        if self.settings.os == "Linux":
            self.requires("libdispatch/[*]")
        self.requires("libjpeg-turbo/[*]")
        self.requires("lz4/[*]")
        self.requires("minizip/[*]")
        self.requires("ms-gsl/[*]")
        self.requires("openal-soft/[*]")
        self.requires("openssl/[*]")
        if self.settings.os == "Linux":
            self.requires("protobuf/[~3.21]")
        elif self.settings.os != "Macos":
            self.requires("protobuf/[*]")
        self.requires("qr-code-generator/[*]")
        if self.settings.os == "Windows" and self.settings.arch != "armv8":
            self.requires("qt/[~5]")
        else:
            self.requires("qt/[*]")
        self.requires("range-v3/[*]")
        self.requires("tl-expected/[*]")
        self.requires("xxhash/[*]")
        self.requires("xz_utils/[*]")
        self.requires("zlib/[*]")

    def configure(self):
        self.options["protobuf/*"].lite = True
        self.options["qt/*"].qtimageformats = True
        self.options["qt/*"].qtsvg = True
        self.options["qt/*"].with_harfbuzz = True
        self.options["qt/*"].with_icu = False
        self.options["qt/*"].with_libjpeg = "libjpeg-turbo"
        self.options["qt/*"].with_mysql = False
        self.options["qt/*"].with_odbc = False
        self.options["qt/*"].with_pq = False
        self.options["qt/*"].with_sqlite3 = False

        if self.settings.os == "Linux":
            self.options["glib/*"].shared = True
            self.options["qt/*"].qtdeclarative = True
            self.options["qt/*"].qtshadertools = True
            self.options["qt/*"].qtwayland = True
            self.options["qt/*"].with_dbus = True
            self.options["qt/*"].with_egl = True
            self.options["qt/*"].with_glib = True
            self.options["qt/*"].with_vulkan = True

    def layout(self):
        cmake_layout(self)
        self.folders.generators = os.path.join("out", "generators")
        self.folders.build = "out"
