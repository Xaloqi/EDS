# MISRA analysis stubs

Declarations only. `misra_analysis.py` puts this directory on cppcheck's include
path so that the lwIP, FreeRTOS and Zephyr calls in `transport/doip/freertos_lwip.c`
and `transport/doip/zephyr_lwip.c` resolve to a declaration. Without them cppcheck
2.19 reports every such call as Rule 17.3 (implicit function declaration), a
mandatory rule that cannot carry a deviation (EDS#583).

These files are never compiled or shipped in a firmware build. The prototypes
mirror the upstream signatures; keep them to what the two files call.
