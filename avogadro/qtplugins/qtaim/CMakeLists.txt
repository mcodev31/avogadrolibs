find_package(Qt5Concurrent REQUIRED)
include_directories(SYSTEM ${Qt5Concurrent_INCLUDE_DIRS})
add_definitions(${Qt5Concurrent_DEFINITIONS})

set(qtaimextension_SRCS
    qtaimextension.cpp
    qtaimwavefunction.cpp
    qtaimwavefunctionevaluator.cpp
    qtaimodeintegrator.cpp
    qtaimcriticalpointlocator.cpp
    qtaimmathutilities.cpp
    qtaimodeintegrator.cpp
    qtaimlsodaintegrator.cpp
    qtaimcubature.cpp
)

avogadro_plugin(QTAIMExtension
  "QTAIM extension"
  ExtensionPlugin
  qtaimextension.h
  QTAIMExtension
  "${qtaimextension_SRCS}"
)

# The settings widget is not built -- its settings weren't actually used by the
# engine in Avogadro 1. The sources are kept for later if we decide to use it.
avogadro_plugin(QTAIMScenePlugin
  "QTAIM scene plugin"
  ScenePlugin
  qtaimengine.h
  QTAIMEngine
  qtaimengine.cpp
)

target_link_libraries(QTAIMScenePlugin
  LINK_PRIVATE AvogadroRendering ${Qt5Concurrent_LIBRARIES})
