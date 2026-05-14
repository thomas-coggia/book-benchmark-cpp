"""Conan 2 recipe for the matching engine.

Production code carries zero third-party dependencies; the only external library that ever
reaches a translation unit is GoogleTest, declared here as a `test_requires` so it stays
out of the consumer-visible interface and out of the matching::matching INTERFACE library.
"""

import os

from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class MatchingConan(ConanFile):
    name = "matching"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"

    options = {"matching_preset": ["standard", "asan", "ubsan", "tsan"]}
    default_options = {"matching_preset": "standard"}

    def layout(self):
        # cmake_layout(): toolchain + Conan CMakePresets under <binaryDir>/generators/.
        # Standard presets: directories clang-19-debug / clang-19-release (matches CMakePresets names).
        # Sanitizer presets: flat clang-19-<slug>/ without an extra Debug/ leaf (manual folders.build).
        self.folders.source = "."
        if self.options.matching_preset == "standard":
            self.folders.build_folder_vars = [
                "settings.compiler",
                "settings.compiler.version",
                "settings.build_type",
            ]
            cmake_layout(self)
            return

        slug = str(self.options.matching_preset)
        compiler = self.settings.get_safe("compiler")
        ver = self.settings.get_safe("compiler.version")
        self.folder_slug = slug
        self.folders.build = os.path.join("build", f"{compiler}-{ver}-{slug}")
        self.folders.generators = os.path.join(self.folders.build, "generators")
        self.folders.build_folder_vars = [
            "settings.compiler",
            "settings.compiler.version",
            "self.folder_slug",
            "settings.build_type",
        ]

    def requirements(self):
        # Production code carries zero third-party dependencies.
        pass

    def build_requirements(self):
        self.test_requires("gtest/1.14.0")

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = "ConanPresets.json"
        toolchain.generate()
        CMakeDeps(self).generate()
