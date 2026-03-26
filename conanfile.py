from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
import os

class CppGateRecipe(ConanFile):
    name = "cppgate"
    version = "0.1.0"
    package_type = "library"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    
    # Declare the options
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    
    # Set the defaults
    default_options = {
        "shared": False,
        "fPIC": True,

        # Set spdlog to header-only
        "spdlog/*:header_only": True,

        # Strip down Boost to the bare essentials
        "boost/*:without_log": True,
        "boost/*:without_locale": True,
        "boost/*:without_python": True,
        "boost/*:without_process": True,
        "boost/*:without_stacktrace": True,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

        # Use the current build_type setting to point to the right folder
        bt = str(self.settings.build_type) # Usually "Debug" or "Release"

        # This creates the absolute path:
        lib_path = os.path.join(self.recipe_folder, "build", bt)

        self.cpp.source.includedirs = ["include"]

        # Set the search path for editable mode
        self.cpp.build.libdirs = [lib_path]
        self.cpp.build.libs = ["cppgate"]

        # This is what the consumer sees
        self.cpp.package.libdirs = self.cpp.build.libdirs
        self.cpp.package.libs = ["cppgate"]

        '''
        #Handle different folder structures per platform
        if self.settings.os == "Android":
            # Android often puts libs in 'build/android/<arch>/Debug'
            arch = str(self.settings.arch)
            lib_path = os.path.join(self.recipe_folder, "build", "android", arch, bt)
        elif self.settings.os == "Emscripten":
            # Emscripten usually outputs .a or .js files
            lib_path = os.path.join(self.recipe_folder, "build", "wasm", bt)
        else:
            # Default for MSVC/Windows
            lib_path = os.path.join(self.recipe_folder, "build", bt)
        '''

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("re2/20240702")
        self.requires("openssl/3.3.2")
        self.requires("spdlog/1.15.1")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED_LIBS"] = "ON" if self.options.shared else "OFF"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["cppgate"]

        if not self.options.shared:
            self.cpp_info.defines = ["CPPGATE_EXPORT_STATIC"]


