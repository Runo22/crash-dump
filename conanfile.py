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
    exports_sources = "CMakeLists.txt", "*.cpp", "*.hpp", "README.md", "USAGE.md"

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

    def build(self):
        # The classic CMake helper doesn't map compiler=msvc to a generator and
        # falls back to "MinGW Makefiles". Force Ninja, and bring in the MSVC
        # environment so cl.exe is on PATH for the Ninja build.
        cmake = CMake(self, generator="Ninja")
        # Pass an explicit ON/OFF. Assigning the option object (or relying on its
        # truthiness) can serialize wrong, leaving BUILD_SHARED_LIBS off so the
        # DLL never gets its export defines -> no import .lib.
        shared = str(self.options.shared) == "True"
        cmake.definitions["BUILD_SHARED_LIBS"] = "ON" if shared else "OFF"
        with tools.vcvars(self.settings):
            cmake.configure()
            cmake.build()

    def package(self):
        self.copy("crash_handler.hpp", dst="include")   # only the public header
        self.copy("*.lib", dst="lib", keep_path=False)
        self.copy("*.dll", dst="bin", keep_path=False)  # present only for the shared build

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
        # Consumers of the DLL need dllimport on the public API.
        if str(self.options.shared) == "True":
            self.cpp_info.defines.append("CRASH_HANDLER_SHARED")
