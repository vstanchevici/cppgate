#pragma once

// --- Project Info ---
#ifdef META_PROJECT_NAME
	#define CPPGATE_PROJECT_NAME	META_PROJECT_NAME
#else
	#define CPPGATE_PROJECT_NAME	"unknown"
#endif

#define CPPGATE_PROJECT_DESCRIPTION META_PROJECT_DESCRIPTION
#define CPPGATE_PROJECT_URL         META_PROJECT_URL

#define CPPGATE_VERSION_MAJOR       META_VERSION_MAJOR
#define CPPGATE_VERSION_MINOR       META_VERSION_MINOR
#define CPPGATE_VERSION_PATCH       META_VERSION_PATCH
#define CPPGATE_VERSION_REVISION    META_VERSION_REVISION

#define CPPGATE_VERSION             META_VERSION
#define CPPGATE_NAME_VERSION        META_NAME_VERSION


// --- Export/Import Logic ---
#ifdef CPPGATE_EXPORT_STATIC
#	define CPPGATE_EXPORT
#else
#	if defined(_WIN32) || defined(__CYGWIN__)
#		ifdef cppgate_EXPORTS
#			define CPPGATE_EXPORT __declspec(dllexport)
#		else
#			define CPPGATE_EXPORT __declspec(dllimport)
#		endif
#	else
#		// Linux/Android/macOS
#		define CPPGATE_EXPORT __attribute__((visibility("default")))
#	endif
#endif