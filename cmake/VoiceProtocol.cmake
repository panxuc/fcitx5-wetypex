# Invalidate an idle resident controller when its implementation/model contract
# changes, including upgrades that retain the same public protocol number.
set(wetype_voice_sources scripts/voice_stream.py scripts/local_voice.py scripts/optional_voice.py data/local-voice-model.json data/optional-voice-models.json src/sherpavoice/main.cpp src/sherpavoice/c-api.h)
file(GLOB wetype_voice_rust RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/localvoice/*.rs")
list(SORT wetype_voice_rust)
list(APPEND wetype_voice_sources ${wetype_voice_rust})
set(wetype_voice_hashes "")
foreach(source IN LISTS wetype_voice_sources)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
  string(APPEND wetype_voice_hashes "${source_hash}\n")
endforeach()
string(SHA256 WETYPE_VOICE_REVISION "${wetype_voice_hashes}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${wetype_voice_sources})
target_compile_definitions(wetypex PRIVATE WETYPE_VOICE_REVISION="${WETYPE_VOICE_REVISION}")
