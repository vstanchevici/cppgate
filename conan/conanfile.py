from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
import os

class CppGateConan(ConanFile):
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
        os_name = str(self.settings.os)
        custom_name = self.conf.get("user.build:folder_name", os.getenv("CONAN_FOLDER_NAME", "common"))

        base_path = os.path.dirname(os.path.dirname(__file__))
        build_path = os.path.join(base_path, "build", custom_name)
        #gen = self.conf.get("tools.cmake.cmaketoolchain:generator")
        #cmake_layout(self, generator=gen, build_folder=my_custom_path, src_folder="..")

        # Standard Conan layout anchored to your custom path
        cmake_layout(self, build_folder=build_path, src_folder="..")

        # Detect the Build Type (Debug/Release)
        bt = str(self.settings.build_type)
        
        # Logic for binary discovery
        # If using Xcode/MSVC, we need to point to the sub-folder
        # self.conf.get("tools.cmake.cmaketoolchain:generator") is how we check
        gen = str(self.conf.get("tools.cmake.cmaketoolchain:generator", ""))
        
        lib_path = lib_path = self.folders.build  # Look in build/common (for Make/Ninja)

        if "Visual" in gen or "Xcode" in gen:
            # Look in build/xcode/Debug
            lib_path = os.path.join(self.folders.build, bt)                      

        if os_name == "Windows":
            lib_path = lib_path.replace("\\", "/")
        
        self.cpp.build.libdirs = [lib_path]

        self.cpp.build.libs = ["cppgate"]
        self.cpp_info.libdirs = self.cpp.build.libdirs

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
            self.cpp_info.defines = ["CPPGATE_API="]


