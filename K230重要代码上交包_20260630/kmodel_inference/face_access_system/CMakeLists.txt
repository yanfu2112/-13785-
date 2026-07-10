set(face_common_includes
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${nncase_sdk_root}/riscv64/rvvlib/include
    ${k230_sdk}/src/big/mpp/userapps/api/
    ${k230_sdk}/src/big/mpp/include
    ${k230_sdk}/src/big/mpp/include/comm
    ${k230_sdk}/src/big/mpp/userapps/sample/sample_vo
    ${k230_sdk}/src/big/rt-smart/userapps/sdk/rt-thread/include
)

set(face_mpp_libraries
    -Wl,--start-group
    rvv Nncase.Runtime.Native nncase.rt_modules.k230 functional_k230
    sys vicap vb cam_device cam_engine hal oslayer ebase fpga isp_drv binder
    auto_ctrol common cam_caldb isi 3a buffer_management cameric_drv video_in
    virtual_hal start_engine cmd_buffer switch cameric_reg_drv t_database_c
    t_mxml_c t_json_c t_common_c vo connector sensor atomic dma
    -Wl,--end-group
)

set(face_opencv_libraries
    opencv_imgcodecs opencv_imgproc opencv_core zlib libjpeg-turbo libopenjp2
    libpng libtiff libwebp csi_cv
)

include_directories(${face_common_includes})
link_directories(${nncase_sdk_root}/riscv64/rvvlib/)
link_directories(${k230_sdk}/src/big/rt-smart/userapps/sdk/rt-thread/lib/risc-v/rv64)

add_executable(capture_display.elf
    capture_display_main.cc
)
target_link_libraries(capture_display.elf ${face_mpp_libraries} ${face_opencv_libraries} rtthread)

add_executable(face_ai.elf
    face_ai_main.cc utils.cc ai_base.cc face_detection.cc face_recognition.cc
    anchors_320.cc anchors_640.cc
)
target_link_libraries(face_ai.elf ${face_mpp_libraries} ${face_opencv_libraries} rtthread)

add_executable(access_control.elf
    access_control_main.cc
)
target_link_libraries(access_control.elf -Wl,--start-group sys atomic rtthread -Wl,--end-group)

install(TARGETS capture_display.elf face_ai.elf access_control.elf DESTINATION bin)
