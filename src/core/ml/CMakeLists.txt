# ml — SR/NR ONNX 추론, 모델 매니저, Algorithm/Local/Remote 백엔드

add_library(flexraw_core_ml STATIC)

target_sources(flexraw_core_ml PRIVATE
    ml.cpp
)

target_include_directories(flexraw_core_ml PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_ml PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Network
    ${OpenCV_LIBS}
)

if(ONNXRUNTIME_FOUND)
    target_link_libraries(flexraw_core_ml PUBLIC onnxruntime)
    target_compile_definitions(flexraw_core_ml PUBLIC FLEXRAW_ONNX_ENABLED)
endif()
