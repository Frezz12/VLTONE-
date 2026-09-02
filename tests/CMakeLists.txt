# A real CLAP plugin, built here and loaded by the host tests. Defined first so
# DAW_TEST_CLAP_PATH is available to the targets below.
if(DAW_ENABLE_CLAP)
    add_subdirectory(fixtures/test_clap)
endif()
if(DAW_ENABLE_VST3)
    add_subdirectory(fixtures/test_vst3)
endif()
if(DAW_ENABLE_VST)
    add_subdirectory(fixtures/test_vst)
endif()

# Phase-1: the portable device/decode paths (libsndfile + PortAudio).
add_executable(platform_test platform_test.cpp)
target_link_libraries(platform_test PRIVATE daw_core daw_engine)

add_test(NAME platform_test COMMAND platform_test)
set_tests_properties(platform_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Musical content analysis: multifeature tempo, chroma key, uncertainty and
# playback transforms, all from deterministic generated audio.
add_executable(audio_musical_analysis_test audio_musical_analysis_test.cpp)
target_link_libraries(audio_musical_analysis_test PRIVATE daw_controller)

add_test(NAME audio_musical_analysis_test COMMAND audio_musical_analysis_test)
set_tests_properties(audio_musical_analysis_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Corpus evaluator for licensed/local datasets. Kept out of the default build:
# it is a quality/calibration instrument, not a deterministic unit test.
add_executable(audio_analysis_bench EXCLUDE_FROM_ALL audio_analysis_bench.cpp)
target_link_libraries(audio_analysis_bench PRIVATE daw_controller)

# Manual hardware smoke — built but NOT registered as a ctest, since it opens a
# real audio device. Run ./bin/device_smoke by hand.
add_executable(device_smoke device_smoke.cpp)
target_link_libraries(device_smoke PRIVATE daw_controller)

# Phase-2: headless controller round-trip (model, export, save/reload).
add_executable(controller_test controller_test.cpp)
target_link_libraries(controller_test PRIVATE daw_controller)

add_test(NAME controller_test COMMAND controller_test)
set_tests_properties(controller_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Narrow seam between legacy EngineController entry points and the typed cloud
# command path: submitted/blocked edits never mutate locally, while an explicit
# fallback retains the pre-collaboration mutation and undo behaviour.
add_executable(shared_mutation_sink_test shared_mutation_sink_test.cpp)
target_link_libraries(shared_mutation_sink_test PRIVATE daw_controller)

add_test(NAME shared_mutation_sink_test COMMAND shared_mutation_sink_test)
set_tests_properties(shared_mutation_sink_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Collaboration document protocol: v5->v6 migration, canonical command wire,
# pure reducer convergence, conditional undo, and optimistic/confirmed replay.
add_executable(collaboration_protocol_test collaboration_protocol_test.cpp)
target_link_libraries(collaboration_protocol_test PRIVATE daw_controller)
target_compile_definitions(collaboration_protocol_test PRIVATE
    DAW_PROJECT_COMMAND_SCHEMA="${CMAKE_SOURCE_DIR}/protocol/schema/project-command-v2.schema.json"
    DAW_TAKE_MOVE_GOLDEN="${CMAKE_SOURCE_DIR}/tests/fixtures/collaboration_take_move_v2.json"
    DAW_COMMAND_V2_GOLDEN="${CMAKE_SOURCE_DIR}/tests/fixtures/collaboration_command_v2_golden.json")

add_test(NAME collaboration_protocol_test COMMAND collaboration_protocol_test)
set_tests_properties(collaboration_protocol_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Pure fail-closed landing of ready cloud captures into the narrow
# recording.commit whitelist. Kept separate from the broad wire/reducer suite
# so recovery/asset/overlap semantics remain quick to exercise in isolation.
add_executable(recording_commit_planner_test recording_commit_planner_test.cpp)
target_link_libraries(recording_commit_planner_test PRIVATE daw_controller)

add_test(NAME recording_commit_planner_test COMMAND recording_commit_planner_test)
set_tests_properties(recording_commit_planner_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Cloud publication stays pure and headless: plugin compatibility, AssetRef
# requirements, and the guarantee that canonical snapshots contain no local
# paths, transport/capture choices, or editor geometry.
add_executable(cloud_project_test cloud_project_test.cpp)
target_link_libraries(cloud_project_test PRIVATE daw_controller)

add_test(NAME cloud_project_test COMMAND cloud_project_test)
set_tests_properties(cloud_project_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# The windowed-sinc sample-rate converter every clip is decoded through.
add_executable(resampler_test resampler_test.cpp)
target_link_libraries(resampler_test PRIVATE daw_engine)

add_test(NAME resampler_test COMMAND resampler_test)
set_tests_properties(resampler_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Offline render: formats, ranges, tails, stems, and the state a render borrows.
add_executable(render_test render_test.cpp)
target_link_libraries(render_test PRIVATE daw_controller)

add_test(NAME render_test COMMAND render_test)
set_tests_properties(render_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Portable Channel Strip templates: compact VLTS round-trip, cross-channel
# application, and the guarantee that routing/sends are never part of one.
add_executable(channel_strip_preset_test channel_strip_preset_test.cpp)
target_link_libraries(channel_strip_preset_test PRIVATE daw_controller)

add_test(NAME channel_strip_preset_test COMMAND channel_strip_preset_test)
set_tests_properties(channel_strip_preset_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Portable project templates: clip-free round-trip, new-project loading,
# append-only import with id/routing remap, and one-step undo/redo.
add_executable(project_template_test project_template_test.cpp)
target_link_libraries(project_template_test PRIVATE daw_controller)

add_test(NAME project_template_test COMMAND project_template_test)
set_tests_properties(project_template_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# The built-in sampler: parameter surface, voice DSP, the precomputed stage,
# its state chunk, and one pass through a real project. No fixture plugin is
# involved — the sampler is part of this build.
add_executable(sampler_test sampler_test.cpp)
target_link_libraries(sampler_test PRIVATE daw_controller)

add_test(NAME sampler_test COMMAND sampler_test)
set_tests_properties(sampler_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Gravity 2: stable state/parameters, the allocation-free granular cloud,
# six trajectories, sidechain detector, A/B attractors and feedback safety.
add_executable(gravity_test gravity_test.cpp)
target_link_libraries(gravity_test PRIVATE daw_controller)

add_test(NAME gravity_test COMMAND gravity_test)
set_tests_properties(gravity_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# VLT Equalizer: stable registration/state, IIR and WOLA paths, dynamic
# sidechain, PDC notifications, sample-offset events and realtime safety.
add_executable(equalizer_test equalizer_test.cpp)
target_link_libraries(equalizer_test PRIVATE daw_controller)

add_test(NAME equalizer_test COMMAND equalizer_test)
set_tests_properties(equalizer_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# The rebuilt engine: graph compilation, latency compensation, and the
# dependency-driven parallel scheduler (correctness, determinism, scaling).
add_executable(engine_graph_test engine_graph_test.cpp)
target_link_libraries(engine_graph_test PRIVATE daw_engine)

add_test(NAME engine_graph_test COMMAND engine_graph_test)
set_tests_properties(engine_graph_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Meter aggregation and peak reduction share one SIMD pass. Keep its numerical
# equivalence and the representative callback cost visible independently of the
# full graph benchmark.
add_executable(meter_node_test meter_node_test.cpp)
target_link_libraries(meter_node_test PRIVATE daw_engine)

add_test(NAME meter_node_test COMMAND meter_node_test)
set_tests_properties(meter_node_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# The piano roll's note maths: every Tools/Edit transform as a pure function,
# plus the undoable setClipNotes they all land through.
add_executable(miditools_test miditools_test.cpp)
target_link_libraries(miditools_test PRIVATE daw_controller)

add_test(NAME miditools_test COMMAND miditools_test)
set_tests_properties(miditools_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# The automation editor's curve maths: every generator and shaping command as a
# pure function, plus the live-edit/commit split the editor drives them through.
add_executable(automation_test automation_test.cpp)
target_link_libraries(automation_test PRIVATE daw_controller)

add_test(NAME automation_test COMMAND automation_test)
set_tests_properties(automation_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Standard MIDI File parsing and the import that lands one in the document.
add_executable(midifile_test midifile_test.cpp)
target_link_libraries(midifile_test PRIVATE daw_controller)

add_test(NAME midifile_test COMMAND midifile_test)
set_tests_properties(midifile_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# The tool surface a language model drives the program through, and the agent
# loop over it. Scripted JSON only — no provider is contacted.
add_executable(ai_tools_test ai_tools_test.cpp)
target_link_libraries(ai_tools_test PRIVATE daw_controller)

add_test(NAME ai_tools_test COMMAND ai_tools_test)
set_tests_properties(ai_tools_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

add_executable(project_music_context_test project_music_context_test.cpp)
target_link_libraries(project_music_context_test PRIVATE daw_controller)

add_test(NAME project_music_context_test COMMAND project_music_context_test)
set_tests_properties(project_music_context_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

add_executable(content_catalog_test content_catalog_test.cpp)
target_link_libraries(content_catalog_test PRIVATE daw_controller)

add_test(NAME content_catalog_test COMMAND content_catalog_test)
set_tests_properties(content_catalog_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

add_executable(composition_engine_test composition_engine_test.cpp)
target_link_libraries(composition_engine_test PRIVATE daw_controller)

add_test(NAME composition_engine_test COMMAND composition_engine_test)
set_tests_properties(composition_engine_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Reproducible quality baseline for contextual melody/bass/chords/drums.
# Run with --ratings-template <file.tsv> to prepare a human listening sheet.
add_executable(composition_eval composition_eval.cpp)
target_link_libraries(composition_eval PRIVATE daw_controller)
target_compile_definitions(composition_eval PRIVATE
    DAW_COMPOSITION_EVAL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/composition_eval_golden.tsv")

add_test(NAME composition_eval COMMAND composition_eval)
set_tests_properties(composition_eval PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# A hand-run diagnostic, deliberately not a ctest target: it loads a real
# plugin from the machine it runs on and prints what that plugin did with the
# host's transport, buses and callbacks. Built with the tests because that is
# where its dependencies already are.
add_executable(plugin_probe EXCLUDE_FROM_ALL plugin_probe.cpp)
target_link_libraries(plugin_probe PRIVATE daw_pluginhost)

# The same idea one layer up: a real plugin in a real slot, through the
# controller — does the slot end up with a live instance, and does that survive
# a Replace. Also hand-run only.
add_executable(plugin_slot_probe EXCLUDE_FROM_ALL plugin_slot_probe.cpp)
target_link_libraries(plugin_slot_probe PRIVATE daw_controller)

# What the graph costs on the machine it is running on: a session of N tracks
# with real plugins, played through the real device, with the DSP meter read
# at several block sizes. Hand-run — the plugins and the answer are both local.
add_executable(load_bench EXCLUDE_FROM_ALL load_bench.cpp)
target_link_libraries(load_bench PRIVATE daw_controller)

# Open and close every installed plugin's editor, repeatedly, in a real window.
# Under Guard Malloc this is what turns "the program crashes sometimes" into a
# stack: a plugin that leaves a run-loop timer or a worker thread behind after
# its editor closes faults on the instruction that touches the freed object.
#
#   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./build/bin/editor_churn --au
#
# Hand-run, macOS only — it needs AppKit and the machine's own plugins.
if(APPLE)
    add_executable(editor_churn EXCLUDE_FROM_ALL editor_churn.mm)
    target_link_libraries(editor_churn PRIVATE daw_pluginhost "-framework AppKit")
    target_compile_options(editor_churn PRIVATE -fobjc-arc)
endif()

# Every edit a session makes to the engine while it renders, in a loop, so an
# intermittent heap corruption becomes reproducible under a heap checker.
# Hand-run — it wants minutes and the machine's own plugins.
add_executable(stress_soak EXCLUDE_FROM_ALL stress_soak.cpp)
target_link_libraries(stress_soak PRIVATE daw_controller)

# The music-generation mode: the brief read off the project, the request body,
# and the tolerant reply parser. Hand-written JSON only — no server is called.
add_executable(music_gen_test music_gen_test.cpp)
target_link_libraries(music_gen_test PRIVATE daw_controller)

add_test(NAME music_gen_test COMMAND music_gen_test)
set_tests_properties(music_gen_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Crash recovery: the document-only serializer, the journal, and the session
# protocol the watchdog reads.
add_executable(recovery_test recovery_test.cpp)
target_link_libraries(recovery_test PRIVATE daw_controller)

target_compile_definitions(recovery_test PRIVATE
    DAW_GUARD_PATH="$<TARGET_FILE:daw_guard>")
add_dependencies(recovery_test daw_guard)

add_test(NAME recovery_test COMMAND recovery_test)
set_tests_properties(recovery_test PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

# Plugin hosting against the real CLAP fixture above: scanning, instantiation,
# parameters, state, and a PluginNode inside the graph.
if(DAW_ENABLE_CLAP)
    add_executable(plugin_host_test plugin_host_test.cpp)
    target_link_libraries(plugin_host_test PRIVATE daw_pluginhost)
    target_compile_definitions(plugin_host_test
        PRIVATE DAW_TEST_CLAP_PATH="${DAW_TEST_CLAP_PATH}")
    add_dependencies(plugin_host_test daw_test_clap)

    add_test(NAME plugin_host_test COMMAND plugin_host_test)
    set_tests_properties(plugin_host_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

    # The out-of-process scanner, the cache, and the crash/timeout handling —
    # driven against the real daw_scan binary, including its deliberate crash
    # and hang probes.
    add_executable(plugin_scan_test plugin_scan_test.cpp)
    target_link_libraries(plugin_scan_test PRIVATE daw_controller)
    target_compile_definitions(plugin_scan_test PRIVATE
        DAW_TEST_CLAP_PATH="${DAW_TEST_CLAP_PATH}"
        DAW_SCAN_PATH="$<TARGET_FILE:daw_scan>")
    add_dependencies(plugin_scan_test daw_test_clap daw_scan)

    add_test(NAME plugin_scan_test COMMAND plugin_scan_test)
    set_tests_properties(plugin_scan_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")

    # Plugins in a real project: insert slots, the mixdown, save/reload with
    # plugin state, and loading a project written before hosting existed.
    add_executable(plugin_insert_test plugin_insert_test.cpp)
    target_link_libraries(plugin_insert_test PRIVATE daw_controller)
    target_compile_definitions(plugin_insert_test PRIVATE
        DAW_TEST_CLAP_PATH="${DAW_TEST_CLAP_PATH}"
        DAW_SCAN_PATH="$<TARGET_FILE:daw_scan>")
    add_dependencies(plugin_insert_test daw_test_clap daw_scan)

    add_test(NAME plugin_insert_test COMMAND plugin_insert_test)
    set_tests_properties(plugin_insert_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")
endif()

# Phase-7: VST3 hosting, against a real VST3 bundle built in-tree.
if(DAW_ENABLE_VST3)
    add_executable(plugin_vst3_test plugin_vst3_test.cpp)
    target_link_libraries(plugin_vst3_test PRIVATE daw_pluginhost)
    target_compile_definitions(plugin_vst3_test
        PRIVATE DAW_TEST_VST3_PATH="${DAW_TEST_VST3_PATH}")
    add_dependencies(plugin_vst3_test daw_test_vst3)
    add_test(NAME plugin_vst3_test COMMAND plugin_vst3_test)
    set_tests_properties(plugin_vst3_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")
endif()

# Legacy VST1/VST2 hosting, including shell components and both state modes.
if(DAW_ENABLE_VST)
    add_executable(plugin_vst_test plugin_vst_test.cpp)
    target_link_libraries(plugin_vst_test PRIVATE daw_controller)
    target_compile_definitions(plugin_vst_test PRIVATE
        DAW_TEST_VST_SHELL_PATH="${DAW_TEST_VST_SHELL_PATH}"
        DAW_TEST_VST1_PATH="${DAW_TEST_VST1_PATH}"
        DAW_SCAN_PATH="$<TARGET_FILE:daw_scan>")
    add_dependencies(plugin_vst_test daw_test_vst_shell daw_test_vst1 daw_scan)
    add_test(NAME plugin_vst_test COMMAND plugin_vst_test)
    set_tests_properties(plugin_vst_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")
endif()

# Phase-8: AU hosting. The fixture is macOS itself — Apple's own units in
# /System/Library/Components — so nothing has to be built or installed.
if(DAW_ENABLE_AU)
    add_executable(plugin_au_test plugin_au_test.cpp)
    target_link_libraries(plugin_au_test PRIVATE daw_pluginhost)
    add_test(NAME plugin_au_test COMMAND plugin_au_test)
    set_tests_properties(plugin_au_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")
endif()

# Phase-9: MIDI through the graph and into an instrument.
if(DAW_ENABLE_CLAP)
    add_executable(plugin_midi_test plugin_midi_test.cpp)
    target_link_libraries(plugin_midi_test PRIVATE daw_pluginhost)
    target_compile_definitions(plugin_midi_test
        PRIVATE DAW_TEST_CLAP_PATH="${DAW_TEST_CLAP_PATH}")
    add_dependencies(plugin_midi_test daw_test_clap)
    add_test(NAME plugin_midi_test COMMAND plugin_midi_test)
    set_tests_properties(plugin_midi_test PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL;FAILURES PRESENT")
endif()
