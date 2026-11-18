cmake_minimum_required(VERSION 3.25)
project(cheat-indicator)

# Load Geode SDK
find_package(Geode REQUIRED)

add_geode_mod(cheat-indicator)

target_sources(cheat-indicator PRIVATE
    src/main.cpp
    src/Detectors/SpeedhackDetector.cpp
    src/Detectors/PhysicsDetector.cpp
    src/Detectors/ModDetector.cpp
    src/UI/IndicatorHUD.cpp
)

target_include_directories(cheat-indicator PRIVATE include)

# Platform-specific handling for iOS / Android / Windows
if(GEODE_IS_IOS)
    # iOS specific linker flags or frameworks if needed
    target_link_libraries(cheat-indicator PRIVATE)
elseif(GEODE_IS_ANDROID)
    # Android specific configurations
elseif(GEODE_IS_WINDOWS)
    # Windows specific configurations
endif()
