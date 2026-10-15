add_executable(testsubject main.cpp)
target_link_libraries(testsubject PRIVATE ole32 user32 gdi32 mmdevapi)
