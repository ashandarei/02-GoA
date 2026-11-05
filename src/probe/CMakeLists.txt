option(KH02_ENABLE_DIAGNOSTICS "Compile opt-in KH0.2 gameplay diagnostics" OFF)

add_library(KH02APProbeCpp SHARED dllmain.cpp asset_reader.cpp asset_index_provider.cpp pak_read_profile.cpp asset_service.cpp movie_service.cpp movie_frames.cpp movie_audio.cpp
    movie_control_service.cpp
    ../shared/kh02/kh02_control_pipe.cpp
    ../shared/kh02/kh02_executable_verification.cpp
    ../shared/kh02/kh02_memory_signature.cpp
    ../shared/kh02/kh02_owner_pipe.cpp
    ../shared/kh02/kh02_movie_shared.cpp
    ../support/kh3ap/features/kh02/transport/kh02_index_state.cpp
    ../support/kh3ap/core/uobject_validity.cpp)
if(KH02_ENABLE_DIAGNOSTICS)
    target_sources(KH02APProbeCpp PRIVATE bridge.cpp engine_tick.cpp companion_audio.cpp
        ../support/kh3ap/bridge/json.cpp
        ../support/kh3ap/bridge/json_fields.cpp)
endif()
target_compile_definitions(KH02APProbeCpp PRIVATE
    KH02_ENABLE_DIAGNOSTICS=$<BOOL:${KH02_ENABLE_DIAGNOSTICS}>)
target_include_directories(KH02APProbeCpp PRIVATE
    ../support .. ../metadata)
target_link_libraries(KH02APProbeCpp PRIVATE UE4SS PolyHook_2 kh3ap_json_headers ole32 bcrypt advapi32 shell32)
set_target_properties(KH02APProbeCpp PROPERTIES OUTPUT_NAME "main")
