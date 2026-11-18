
add_library(munit "${CMAKE_CURRENT_SOURCE_DIR}/munit/munit.c")
target_include_directories(munit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/munit")

set(CHIAKI_UNIT_SOURCES
				main.c
				http.c
				rpcrypt.c
				gkcrypt.c
				takion.c
				seqnum.c
				keystate.c
				reorderqueue.c
				fec.c
				test_log.c
				test_log.h
				bitstream.c
				regist.c)

if(CHIAKI_ENABLE_FFMPEG_DECODER)
	list(APPEND CHIAKI_UNIT_SOURCES ffmpegdecoder.c)
endif()

add_executable(chiaki-unit ${CHIAKI_UNIT_SOURCES})
add_test(NAME unit COMMAND chiaki-unit)

target_link_libraries(chiaki-unit chiaki-lib munit)

if(CHIAKI_ENABLE_FFMPEG_DECODER)
	target_link_libraries(chiaki-unit FFMPEG::avcodec FFMPEG::avutil)
endif()

# Exercise the production sender with deterministic test-only I/O and time.
add_executable(rcp-feedback-replay
    feedback-replay.c
    ../lib/src/feedbacksender.c
    ../lib/src/feedback.c
    ../lib/src/controller.c)
target_include_directories(rcp-feedback-replay PRIVATE ../lib/include)
target_compile_features(rcp-feedback-replay PRIVATE c_std_99)
if(WIN32)
    target_link_libraries(rcp-feedback-replay PRIVATE ws2_32)
else()
    target_link_libraries(rcp-feedback-replay PRIVATE m)
endif()
add_test(NAME rcp-feedback-replay COMMAND rcp-feedback-replay)
set_tests_properties(rcp-feedback-replay PROPERTIES TIMEOUT 10)
