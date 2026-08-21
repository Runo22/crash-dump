from conans import ConanFile, CMake, tools
from conans.errors import ConanInvalidConfiguration


class CrashHandlerConan(ConanFile):
    name = "crash_handler"
    version = "1.0.0"
    license = "Proprietary"
    description = "Offline Windows/MSVC crash reporter: minidump + symbolized text log."
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False]}
    default_options = {"shared": False}
    exports_sources = "CMakeLists.txt", "cmake/*", "*.cpp", "*.hpp", "README.md", "USAGE.md"

    def validate(self):
        # MSVC intrinsics, dbghelp/psapi and /Z7 make this Windows/MSVC only.
        if self.settings.os != "Windows":
            raise ConanInvalidConfiguration("crash_handler is Windows / MSVC only")
        # The shared build passes std::wstring across the DLL boundary, which is
        # only safe with the dynamic runtime (one shared heap).
        runtime = str(self.settings.get_safe("compiler.runtime"))
        if str(self.options.shared) == "True" and runtime in ("MT", "MTd", "static"):
            raise ConanInvalidConfiguration(
                "shared crash_handler requires the dynamic runtime (MD): STL "
                "types cross the DLL boundary")

    def _configure_cmake(self):
        # The classic CMake helper doesn't map compiler=msvc to a generator and
        # falls back to "MinGW Makefiles". Force Ninja. Pass an explicit ON/OFF --
        # assigning the option object can serialize wrong and leave the DLL
        # without its export defines (hence no import .lib).
        cmake = CMake(self, generator="Ninja")
        cmake.definitions["BUILD_SHARED_LIBS"] = \
            "ON" if str(self.options.shared) == "True" else "OFF"
        cmake.configure()
        return cmake

    def build(self):
        with tools.vcvars(self.settings):
            self._configure_cmake().build()

    def package(self):
        # cmake --install lays out include/, lib/, bin/ AND the relocatable
        # crash_handlerConfig.cmake + crash_handlerTargets.cmake under
        # lib/cmake/crash_handler.
        with tools.vcvars(self.settings):
            self._configure_cmake().install()

    def package_id(self):
        # RelWithDebInfo and MinSizeRel use the release CRT (MD), build from the
        # same source, and are ABI-compatible with Release. Collapse them so one
        # optimized binary serves all three. Debug (MDd) stays separate -- its
        # CRT cannot be mixed with the others.
        if self.settings.build_type in ("RelWithDebInfo", "MinSizeRel"):
            self.info.settings.build_type = "Release"

    def package_info(self):
        # Use the config shipped in the package instead of a Conan-generated one,
        # so a consumer just does find_package(crash_handler CONFIG) and gets the
        # imported target -- with the DLL location, so $<TARGET_RUNTIME_DLLS>
        # deploys it. cmake_find_mode "none" keeps Conan from generating a
        # competing config; builddirs points find_package at ours.
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = ["lib/cmake/crash_handler"]
