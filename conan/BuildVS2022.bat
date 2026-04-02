@echo OFF

if "%~1"=="" (
    echo No BUILD_TYPE have been provided.
    echo BUILD_TYPE can be one of Debug, Release, RelWithDebInfo and MinSizeRel.
    exit /b 1
) else (
    set BUILD_TYPE=%~1
)

set OPTIONS= ^
      --output-folder=../build/vs2022 ^
      --build=missing ^
      --settings=build_type=%BUILD_TYPE% ^
      --profile ./msvc.profile ^
      -c tools.cmake.cmaketoolchain:generator="Visual Studio 17 2022" ^
      -c user.build:folder_name=vs2022 ^
      conanfile.py

conan editable add ./

conan install %OPTIONS%
