from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, cmake_layout


class Recipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    def generate(self):
        tc = CMakeToolchain(self)
        tc.user_presets_path = False
        tc.generate()

    def requirements(self):
        if self.settings.os == "Windows":
            self.requires("hunspell/[*]")
            self.requires("libjxl/[~0.10]")

    def layout(self):
        cmake_layout(self)
