from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class PjScriptingCoreConan(ConanFile):
    """The Luau scripting engine + whole-series marker analysis, packaged so the
    standalone official-plugins repo (the GUI Anomaly Detector + the headless
    anomaly_runner) can link the SAME engine the app uses — one engine, GUI ==
    headless. Depends only on pj_base (via plotjuggler_sdk) + Luau + kissfft; the
    SISO DataProcessor adapter (which needs pj_datastore) is NOT part of this
    package and is built only in-tree by the app."""

    name = "pj_scripting_core"
    version = "0.2.0"
    license = "MPL-2.0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # Consumers that link the engine into a shared lib (the GUI plugin .so) need a PIC
    # Luau; default it here so the package's own Luau binary id matches theirs.
    default_options = {"luau/*:fPIC": True}
    exports_sources = (
        "CMakeLists.txt",
        "src/luau_engine.cpp",
        "src/marker_engine.cpp",
        "src/luau_sandbox_state.h",
        "include/*",
    )

    def requirements(self):
        # pj_base (PlotMarker) is exposed by marker_engine.h, so consumers need its
        # headers transitively.
        self.requires("plotjuggler_sdk/0.20.0", transitive_headers=True)
        self.requires("nlohmann_json/3.12.0", transitive_headers=True)
        # A static lib does NOT absorb its static deps — pj_scripting_core.a only
        # *references* the Luau/kissfft symbols, so consumers must link them at the
        # final link. They are therefore propagated (visible), even though they are
        # not in any public header.
        self.requires("luau/0.700")
        self.requires("kissfft/131.1.0")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE*", src=self.source_folder, dst=self.package_folder)

    def package_info(self):
        self.cpp_info.libs = ["pj_scripting_core"]
        self.cpp_info.set_property("cmake_file_name", "pj_scripting_core")
        self.cpp_info.set_property("cmake_target_name", "pj_scripting_core::pj_scripting_core")
        # No explicit cpp_info.requires: default to requiring ALL direct deps so Luau +
        # kissfft propagate to consumers (a static lib doesn't absorb its static deps).
