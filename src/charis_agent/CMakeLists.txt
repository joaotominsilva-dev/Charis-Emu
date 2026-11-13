# SPDX-FileCopyrightText: 2026 Charis
# SPDX-License-Identifier: GPL-3.0-or-later

# Charis Switch helper (Windows): opens Charis Switch when the Switch asks while it is closed
add_executable(charis_agent WIN32
    main.cpp
    ../charis_stream/crypto.cpp
    ../charis_stream/net.cpp
    ../charis_stream/fec.cpp
    ../charis_stream/host.cpp
    ../charis_stream/account.cpp
    ../charis_stream/presence.cpp
    ../charis_stream/upnp.cpp
    ../charis_stream/windows_http.cpp
    ../charis_stream/third_party/monocypher.c
)
set_target_properties(charis_agent PROPERTIES OUTPUT_NAME "CharisSwitchAgent")
target_include_directories(charis_agent PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(charis_agent PRIVATE ws2_32 winhttp crypt32 bcrypt shell32)
if (MINGW)
    target_link_options(charis_agent PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
