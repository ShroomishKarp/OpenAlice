cmake_minimum_required(VERSION 4.4 FATAL_ERROR)

project(OpenAlice LANGUAGES C)


set(CMAKE_C_STANDARD          23)

set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_EXTENSIONS        ON)


if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)

	set(CMAKE_BUILD_TYPE RelWithDebInfo)

endif()


find_package(SDL3 REQUIRED CONFIG)

find_path(GL_INCLUDE_DIR GL/gl.h REQUIRED)



set(SOURCE_DIR "${CMAKE_SOURCE_DIR}/Sources/")


file(GLOB CLIENT_SRC   "${SOURCE_DIR}/client/*.c")

file(GLOB JPEG_SRC     "${SOURCE_DIR}/jpeg-6/*.c")

file(GLOB QCOMMON_SRC  "${SOURCE_DIR}/qcommon/*.c")

file(GLOB RENDERER_SRC "${SOURCE_DIR}/renderer/*.c")

file(GLOB SERVER_SRC   "${SOURCE_DIR}/server/*.c")

file(GLOB SDL_SRC      "${SOURCE_DIR}/sdl/*.c")

file(GLOB UNIX_SRC     "${SOURCE_DIR}/unix/*.c")

set(SHARED_SRC "${SOURCE_DIR}/game/q_math.c" "${SOURCE_DIR}/game/q_shared.c")


add_compile_options(-fsigned-char -fno-strict-aliasing -w)

add_executable(${PROJECT_NAME} ${CLIENT_SRC} ${JPEG_SRC} ${QCOMMON_SRC} ${RENDERER_SRC} ${SERVER_SRC} ${SDL_SRC} ${UNIX_SRC} ${SHARED_SRC})


target_include_directories(${PROJECT_NAME} PRIVATE "${SOURCE_DIR}/qcommon" ${GL_INCLUDE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3 m ${CMAKE_DL_LIBS})


if(CMAKE_SIZEOF_VOID_P EQUAL 4)

	set(_q3_modules_default ON)

else()

	set(_q3_modules_default OFF)

endif()


option(OPENALICE_BUILD_Q3_MODULES "Build the Quake 3 game, cgame and ui modules" ${_q3_modules_default})


if(OPENALICE_BUILD_Q3_MODULES)

	if(CMAKE_SIZEOF_VOID_P EQUAL 4)

		set(OPENALICE_ARCH i386)

	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")

		set(OPENALICE_ARCH aarch64)

	else()

		set(OPENALICE_ARCH x86_64)

	endif()


	function(openalice_module name)

		add_library(${name} MODULE ${ARGN})

		target_include_directories(${name} PRIVATE "${SOURCE_DIR}/qcommon" ${GL_INCLUDE_DIR})

		target_link_libraries(${name} PRIVATE m)

		set_target_properties(${name} PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/baseq3)

	endfunction()


	file(GLOB CGAME_SRC "${SOURCE_DIR}/cgame/*.c")

	openalice_module(cgame${OPENALICE_ARCH} ${CGAME_SRC} "${SOURCE_DIR}/game/q_math.c" "${SOURCE_DIR}/game/q_shared.c" "${SOURCE_DIR}/game/bg_misc.c" "${SOURCE_DIR}/game/bg_pmove.c" "${SOURCE_DIR}/game/bg_slidemove.c")


	file(GLOB GAME_SRC "${SOURCE_DIR}/game/*.c")

	openalice_module(qagame${OPENALICE_ARCH} ${GAME_SRC})


	file(GLOB Q3UI_SRC "${SOURCE_DIR}/q3_ui/*.c")

	openalice_module(ui${OPENALICE_ARCH} ${Q3UI_SRC} "${SOURCE_DIR}/ui/ui_syscalls.c" "${SOURCE_DIR}/game/bg_misc.c" "${SOURCE_DIR}/game/q_math.c" "${SOURCE_DIR}/game/q_shared.c")

endif()