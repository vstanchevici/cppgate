:: Tell Conan this folder IS the package
conan editable add . cppgate/0.1.0

:: Install dependencies for CppGate itself
conan install . --build=missing

:: Build CppGate locally
cmake --preset conan-release
cmake --build --preset conan-release