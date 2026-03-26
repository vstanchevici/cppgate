# === Functions ===

# Filters sources files with a variadic argument for all directores:
# Example:
#   find_source_files(OutputFiles "*.h;*.cpp" ${MyProjectDir}/Base ${MyProjectDir}/Extensions)
function(find_source_files OUTPUT_LIST FILTERS)
    # Substitue predefined filters
    if("${FILTERS}" STREQUAL "C")
        set(FILTERS "*.c;*.h;*.inl")
    elseif("${FILTERS}" STREQUAL "CXX")
        set(FILTERS "*.c;*.cpp;*.h;*.inl")
    elseif("${FILTERS}" STREQUAL "OBJC")
        set(FILTERS "*.c;*.cpp;*.h;*.inl;*.m;*.mm")
    elseif("${FILTERS}" STREQUAL "RES")
        set(FILTERS "*.c;*.cpp;*.h;*.inl;*.m;*.mm;*.metal;*.vert;*.frag")
    elseif("${FILTERS}" STREQUAL "INC")
        set(FILTERS "*.h;*.hpp;*.inl")
    endif()

    # Collect all input directories for each filter
    set(FilteredProjectDirs "")
    set(ProjectDirs "${ARGN}")
    foreach(ProjectSubdir ${ProjectDirs})
        foreach(Filter ${FILTERS})
            list(APPEND FilteredProjectDirs "${ProjectSubdir}/${Filter}")
        endforeach()
    endforeach()
    
    # Find all source files
    file(GLOB FilteredProjectFiles ${FilteredProjectDirs})
    
    # Write filtered files to output variable
    set(${OUTPUT_LIST} ${${OUTPUT_LIST}} ${FilteredProjectFiles} PARENT_SCOPE)
endfunction()


macro(ADD_DEBUG_DEFINE IDENT)
    if(MSVC)
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D${IDENT}")
    else(MSVC)
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D${IDENT}")
    endif(MSVC)
endmacro()


macro(ADD_DEFINE IDENT)
    add_definitions("-D${IDENT}")
endmacro()

function(set_llgl_module_properties MODULE_NAME)
    set_target_properties(
        ${MODULE_NAME} PROPERTIES
            LINKER_LANGUAGE CXX
            DEBUG_POSTFIX   "D"
            FOLDER          "LLGL"
    )
endfunction()


### get_all_targets(all_targets)
function(get_all_targets var)
    set(targets)
    get_all_targets_recursive(targets ${CMAKE_CURRENT_SOURCE_DIR})
    set(${var} ${targets} PARENT_SCOPE)
endfunction()

macro(get_all_targets_recursive targets dir)
    get_property(subdirectories DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(subdir ${subdirectories})
        get_all_targets_recursive(${targets} ${subdir})
    endforeach()

    get_property(current_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND ${targets} ${current_targets})
endmacro()
### end all get_all_targets


### group_targets_from_dir(directory filter_name)
function(group_targets_from_dir DIR FILTER_NAME)
    get_property(TGTS DIRECTORY "${DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(TGT IN LISTS TGTS)
        #message(STATUS "Target: ${TGT}")
        set_property(TARGET ${TGT} PROPERTY FOLDER ${FILTER_NAME})
    endforeach()

    get_property(SUBDIRS DIRECTORY "${DIR}" PROPERTY SUBDIRECTORIES)
    foreach(SUBDIR IN LISTS SUBDIRS)
        group_targets_from_dir("${SUBDIR}" "${FILTER_NAME}")
    endforeach()
endfunction()
### end group_targets_from_dir