"""Conan 2 recipe. `conan build . -pr:a=<profile-from-profiles/>` is the entry point."""

import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain


class MatchingConan(ConanFile):
    name = "matching"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"

    def layout(self):
        self.sanitizer = self.conf.get("user.matching:sanitizer")
        slug = self.sanitizer or str(self.settings.build_type).lower()
        self.folders.source = "."
        self.folders.build = os.path.join(
            "build",
            f"{self.settings.compiler}-{self.settings.compiler.version}-{slug}",
        )
        self.folders.generators = os.path.join(self.folders.build, "generators")
        if self.sanitizer:
            self.folders.build_folder_vars = ["self.sanitizer"]

    def requirements(self):
        self.requires("cxxopts/3.3.1")

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["CMAKE_EXPORT_COMPILE_COMMANDS"] = "ON"
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
