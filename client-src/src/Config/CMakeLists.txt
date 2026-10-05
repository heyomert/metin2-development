file(GLOB_RECURSE FILE_SOURCES "*.h" "*.cpp")

add_executable(Config WIN32 ${FILE_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/Config.rc)
set_target_properties(Config PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
	OUTPUT_NAME "config"
)
target_compile_definitions(Config PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(Config
	DirectX
	comctl32
)

GroupSourcesByFolder(Config)
