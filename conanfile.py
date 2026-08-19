from conans import ConanFile, CMake
from conans.errors import ConanInvalidConfiguration


class CrashHandlerConan(ConanFile):
    name = "crash_handler"
    version = "1.0.0"
    license = "Proprietary"
    description = "Offline Windows/MSVC crash reporter: minidump + symbolized text log."
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "CMakeLists.txt", "*.cpp", "*.hpp", "README.md", "USAGE.md"

    def validate(self):
        # MSVC intrinsics, dbghelp/psapi and /Z7 make this Windows/MSVC only.
        if self.settings.os != "Windows":
            raise ConanInvalidConfiguration("crash_handler is Windows / MSVC only")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        self.copy("crash_handler.hpp", dst="include")   # only the public header
        self.copy("*.lib", dst="lib", keep_path=False)

    def package_id(self):
        # RelWithDebInfo and MinSizeRel use the release CRT (MD), build from the
        # same source, and are ABI-compatible with Release. Collapse them so one
        # optimized binary serves all three. Debug (MDd) stays separate -- its
        # CRT cannot be mixed with the others.
        if self.settings.build_type in ("RelWithDebInfo", "MinSizeRel"):
            self.info.settings.build_type = "Release"

    def package_info(self):
        self.cpp_info.libs = ["crash_handler"]
        self.cpp_info.system_libs = ["dbghelp", "psapi"]
