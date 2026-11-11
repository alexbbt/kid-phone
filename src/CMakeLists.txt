FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
# setup_page.html is the phone's setup web page; voice.js is the Twilio
# Function source, uploaded to the parent's account during provisioning
# (one source of truth: twilio/voice.js). prompts/*.pcm are the spoken
# prompts (16 kHz mono s16le, made by scripts/gen_prompts.py).
# This build embeds English only (other <name>_<lang>.pcm files stay in the
# tree for multi-language builds). twilio/no_answer.wav is the English clip
# for Twilio <Play>. Both lists must match board_build.embed_* in platformio.ini.
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "setup_page.html" "favicon.svg" "${CMAKE_SOURCE_DIR}/twilio/voice.js"
                       EMBED_FILES "prompts/time_up.pcm" "prompts/unavailable.pcm" "prompts/no_answer.pcm"
                                   "prompts/no_wifi.pcm" "prompts/not_ready.pcm" "prompts/cant_call.pcm"
                                   "prompts/sleep_time.pcm" "prompts/secret_start.pcm" "prompts/secret_sent.pcm"
                                   "${CMAKE_SOURCE_DIR}/twilio/no_answer.wav"
                       REQUIRES usb esp_timer driver g711 resampler
                                esp_wifi esp_netif esp_event nvs_flash lwip mbedtls
                                esp_http_client esp_http_server esp_https_ota app_update json
                                esp_driver_uart esp_app_format esp-tls mqtt littlefs)
