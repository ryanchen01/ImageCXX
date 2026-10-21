cmake_minimum_required(VERSION 3.24)
project(ImageCXX VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)
include(CTest)
option(IMAGECXX_BUILD_BENCHMARKS "Build image-model and analysis benchmarks" OFF)
option(IMAGECXX_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Core Gui GuiPrivate Widgets PrintSupport ShaderTools Svg QuickWidgets Graphs)
find_package(ITK 5.4 REQUIRED COMPONENTS ITKCommon ITKIOImageBase ITKIOMeta ITKIONIFTI ITKIONRRD
    ITKSmoothing ITKImageIntensity ITKMathematicalMorphology ITKImageGrid)
list(FILTER ITK_INCLUDE_DIRS EXCLUDE REGEX "^/Library/Developer/CommandLineTools/SDKs/.*")
# ITK I/O adapters select their readers explicitly; keep ITK include paths target-local.
find_package(TIFF REQUIRED)
find_package(DCMTK CONFIG REQUIRED)
qt_standard_project_setup(REQUIRES 6.12)

add_library(imagecxx_core STATIC core/image.cpp core/roi.cpp core/history.cpp core/memory.cpp)
target_include_directories(imagecxx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(imagecxx_core PUBLIC Qt6::Core Qt6::Gui)

add_library(imagecxx_processing STATIC processing/analysis.cpp processing/processing.cpp processing/reslice.cpp)
target_link_libraries(imagecxx_processing PUBLIC imagecxx_core PRIVATE ${ITK_LIBRARIES})
target_include_directories(imagecxx_processing SYSTEM PRIVATE ${ITK_INCLUDE_DIRS})
target_link_directories(imagecxx_processing INTERFACE ${ITK_LIBRARY_DIRS})

add_library(imagecxx_io STATIC io/image_io.cpp io/image_import.cpp io/itk_io.cpp io/dicom_io.cpp)
target_link_libraries(imagecxx_io PUBLIC imagecxx_core PRIVATE TIFF::TIFF ${ITK_LIBRARIES} DCMTK::dcmdata DCMTK::dcmjpeg DCMTK::dcmjpls)
target_include_directories(imagecxx_io SYSTEM PRIVATE ${ITK_INCLUDE_DIRS} ${DCMTK_INCLUDE_DIRS})
target_link_directories(imagecxx_io INTERFACE ${ITK_LIBRARY_DIRS})

add_library(imagecxx_app STATIC app/command_registry.cpp app/processing_commands.cpp app/job_scheduler.cpp
    app/batch.cpp)
target_link_libraries(imagecxx_app PUBLIC imagecxx_core imagecxx_processing imagecxx_io)

add_library(imagecxx_viewer STATIC viewer/image_viewer.cpp)
target_link_libraries(imagecxx_viewer PUBLIC imagecxx_core Qt6::Widgets PRIVATE Qt6::GuiPrivate)
qt_add_shaders(imagecxx_viewer imagecxx_viewer_shaders PREFIX "/imagecxx"
    BASE "viewer/shaders" FILES viewer/shaders/slice.vert viewer/shaders/slice.frag)

add_library(imagecxx_plots STATIC plots/plot_widget.cpp)
target_link_libraries(imagecxx_plots PUBLIC Qt6::Widgets PRIVATE Qt6::QuickWidgets Qt6::Graphs)
set_source_files_properties(plots/Plot.qml PROPERTIES QT_RESOURCE_ALIAS Plot.qml)
qt_add_qml_module(imagecxx_plots URI ImageCXX.Plots VERSION 1.0 NO_PLUGIN
    RESOURCE_PREFIX "/qt/qml" OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/ImageCXX/Plots" QML_FILES plots/Plot.qml)

add_library(imagecxx_ui STATIC mainwindow.cpp ui/document_widget.cpp)
target_link_libraries(imagecxx_ui PUBLIC imagecxx_app imagecxx_viewer imagecxx_plots Qt6::Widgets Qt6::Svg Qt6::PrintSupport)
qt_add_resources(imagecxx_ui imagecxx_icons PREFIX "/" BASE "resources" FILES
    resources/icons/open.svg resources/icons/pan.svg resources/icons/rectangle.svg resources/icons/ellipse.svg
    resources/icons/polygon.svg resources/icons/line.svg resources/icons/measure.svg
    resources/icons/histogram.svg resources/icons/undo.svg resources/icons/search.svg)
qt_add_executable(ImageCXX WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(ImageCXX PRIVATE imagecxx_ui)
set_target_properties(ImageCXX PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER org.imagecxx.ImageCXX
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION} MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

foreach(target imagecxx_core imagecxx_io imagecxx_processing imagecxx_app imagecxx_viewer imagecxx_plots imagecxx_ui ImageCXX)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(IMAGECXX_SANITIZERS)
            target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PUBLIC -fsanitize=address,undefined)
        endif()
    endif()
endforeach()

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(imagecxx_tests tests/core_tests.cpp tests/processing_tests.cpp tests/io_tests.cpp tests/app_tests.cpp)
    target_link_libraries(imagecxx_tests PRIVATE imagecxx_app TIFF::TIFF DCMTK::dcmdata GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(imagecxx_tests DISCOVERY_MODE PRE_TEST)
    add_executable(imagecxx_ui_tests tests/ui_tests.cpp tests/viewer_tests.cpp tests/scheduler_tests.cpp tests/plot_tests.cpp)
    target_link_libraries(imagecxx_ui_tests PRIVATE imagecxx_ui DCMTK::dcmdata GTest::gtest Qt6::QuickWidgets Qt6::Graphs)
    add_test(NAME ui_integration COMMAND imagecxx_ui_tests)
    set_tests_properties(ui_integration PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(IMAGECXX_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    add_executable(imagecxx_benchmarks benchmarks/image_benchmarks.cpp)
    target_link_libraries(imagecxx_benchmarks PRIVATE imagecxx_processing benchmark::benchmark_main)
endif()

install(TARGETS ImageCXX BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_qml_app_script(TARGET ImageCXX OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
