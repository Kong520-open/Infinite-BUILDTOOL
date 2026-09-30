if(NOT DEFINED SOURCE_FILE)
    message(FATAL_ERROR "SOURCE_FILE is required")
endif()

file(READ "${SOURCE_FILE}" build_import_runtime_source)

set(tick_start_marker "void BuildImportRuntime::tickContainerWrite(")
set(tick_end_marker "void BuildImportRuntime::resetEntityRuntime(")
string(FIND "${build_import_runtime_source}" "${tick_start_marker}" tick_start)
string(FIND "${build_import_runtime_source}" "${tick_end_marker}" tick_end)
if(tick_start EQUAL -1 OR tick_end EQUAL -1 OR tick_end LESS_EQUAL tick_start)
    message(FATAL_ERROR "Cannot isolate BuildImportRuntime::tickContainerWrite")
endif()
math(EXPR tick_length "${tick_end} - ${tick_start}")
string(SUBSTRING "${build_import_runtime_source}" ${tick_start} ${tick_length}
       container_restore_source)

# Container restoration must settle for a bounded interval, then rely on the
# tracked server command result. Native position/block reads are not reliable
# readiness gates after packet-side teleports.
set(required_wait "container_item_cell_ready_at_ = now + kDeferredCellMinimumWait;")
string(FIND "${container_restore_source}" "${required_wait}" required_wait_position)
if(required_wait_position EQUAL -1)
    message(FATAL_ERROR "Container restore no longer applies the fixed settling delay")
endif()

set(required_inter_target_delay
    "constexpr auto kContainerInterTargetDelay = std::chrono::milliseconds(0);")
string(FIND "${build_import_runtime_source}" "${required_inter_target_delay}"
       inter_target_delay_position)
if(inter_target_delay_position EQUAL -1)
    message(FATAL_ERROR "Container-to-container delay is not exactly 0 ms")
endif()

set(required_prepare_delay_gate
    "now < container_item_next_target_ready_at_")
string(FIND "${container_restore_source}" "${required_prepare_delay_gate}"
       prepare_delay_gate_position)
if(prepare_delay_gate_position EQUAL -1)
    message(FATAL_ERROR "Container prepare does not wait before the next teleport")
endif()
set(prepare_dispatch_marker
    "const std::vector<std::string> commands = containerItemPrepareCommands(")
string(FIND "${container_restore_source}" "${prepare_dispatch_marker}"
       prepare_dispatch_position)
if(prepare_dispatch_position EQUAL -1 OR
   prepare_delay_gate_position GREATER prepare_dispatch_position)
    message(FATAL_ERROR "The inter-container delay gate must run before teleport preparation")
endif()

set(wait_start_marker "if (stage_ == ExecuteStage::ContainerWait) {")
set(wait_end_marker "if (stage_ != ExecuteStage::ContainerWrite) return;")
string(FIND "${container_restore_source}" "${wait_start_marker}" wait_start)
string(FIND "${container_restore_source}" "${wait_end_marker}" wait_end)
if(wait_start EQUAL -1 OR wait_end EQUAL -1 OR wait_end LESS_EQUAL wait_start)
    message(FATAL_ERROR "Cannot isolate the container wait stage")
endif()
math(EXPR wait_length "${wait_end} - ${wait_start}")
string(SUBSTRING "${container_restore_source}" ${wait_start} ${wait_length}
       container_wait_source)

set(forbidden_wait_tokens
    "pollServerChunkProbe"
    "containerItemTargetMatches"
    "localPlayerIsNearDeferredTarget"
    "NativeWorldReader"
    "NativeWorldAccess::getBlock"
    "NativeWorldAccess::getLocalPlayerBlockPosition"
    "NativeWorldAccess::isChunkReadable"
)
foreach(token IN LISTS forbidden_wait_tokens)
    string(FIND "${container_wait_source}" "${token}" token_position)
    if(NOT token_position EQUAL -1)
        message(FATAL_ERROR "Container wait stage restored an unreliable readiness gate: ${token}")
    endif()
endforeach()

# Keep target validation out of the entire pre-dispatch state machine. The
# destination comes from the spool record and the server ACK is authoritative.
set(forbidden_predispatch_tokens
    "pollServerChunkProbe"
    "containerItemTargetMatches"
    "localPlayerIsNearDeferredTarget"
    "NativeWorldReader"
)
foreach(token IN LISTS forbidden_predispatch_tokens)
    string(FIND "${container_restore_source}" "${token}" token_position)
    if(NOT token_position EQUAL -1)
        message(FATAL_ERROR "Container restore depends on a native/exact-position gate: ${token}")
    endif()
endforeach()

set(ack_start_marker "if (!container_item_pending_uuid_.empty()) {")
set(dispatch_start_marker "const std::string uuid = nextRpcUuid();")
string(FIND "${container_restore_source}" "${ack_start_marker}" ack_start)
string(FIND "${container_restore_source}" "${dispatch_start_marker}" dispatch_start)
if(ack_start EQUAL -1 OR dispatch_start EQUAL -1 OR dispatch_start LESS_EQUAL ack_start)
    message(FATAL_ERROR "Cannot isolate container command acknowledgement handling")
endif()
math(EXPR ack_length "${dispatch_start} - ${ack_start}")
string(SUBSTRING "${container_restore_source}" ${ack_start} ${ack_length}
       container_ack_source)

set(nonaccepted_marker "if (result != RpcResultState::Accepted) {")
set(accepted_marker "container_item_target_retries_ = 0;")
string(FIND "${container_ack_source}" "${nonaccepted_marker}" nonaccepted_start)
string(FIND "${container_ack_source}" "${accepted_marker}" accepted_start)
if(nonaccepted_start EQUAL -1 OR accepted_start EQUAL -1 OR
   accepted_start LESS_EQUAL nonaccepted_start)
    message(FATAL_ERROR "Container ACK success/failure branches are missing or reordered")
endif()
math(EXPR nonaccepted_length "${accepted_start} - ${nonaccepted_start}")
string(SUBSTRING "${container_ack_source}" ${nonaccepted_start} ${nonaccepted_length}
       container_nonaccepted_source)
string(SUBSTRING "${container_ack_source}" ${accepted_start} -1
       container_accepted_source)

set(required_retry_tokens
    "++container_item_target_retries_"
    "stage_ = ExecuteStage::ContainerPrepare;"
)
foreach(token IN LISTS required_retry_tokens)
    string(FIND "${container_nonaccepted_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "Non-accepted container command no longer retries safely: ${token}")
    endif()
endforeach()
string(REGEX MATCHALL "return" nonaccepted_returns "${container_nonaccepted_source}")
list(LENGTH nonaccepted_returns nonaccepted_return_count)
if(nonaccepted_return_count LESS 2)
    message(FATAL_ERROR "Non-accepted container command can fall through to cursor advancement")
endif()
string(FIND "${container_nonaccepted_source}" "++container_item_cursor_" rejected_cursor_position)
if(NOT rejected_cursor_position EQUAL -1)
    message(FATAL_ERROR "Rejected container command advances the durable cursor")
endif()

string(FIND "${container_accepted_source}" "++container_item_cursor_" accepted_cursor_position)
if(accepted_cursor_position EQUAL -1)
    message(FATAL_ERROR "Accepted container command no longer advances the durable cursor")
endif()

set(required_transition_tokens
    "container_item_pending_record_ = container_item_reader_->next();"
    "container_item_pending_record_->x != container_item_target_x_"
    "container_item_pending_record_->y != container_item_target_y_"
    "container_item_pending_record_->z != container_item_target_z_"
    "container_item_next_target_ready_at_ = now + kContainerInterTargetDelay;"
)
foreach(token IN LISTS required_transition_tokens)
    string(FIND "${container_accepted_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "Accepted final slot no longer schedules the next container: ${token}")
    endif()
endforeach()
string(FIND "${container_accepted_source}"
       "container_item_pending_record_ = container_item_reader_->next();"
       next_record_position)
string(FIND "${container_accepted_source}"
       "container_item_next_target_ready_at_ = now + kContainerInterTargetDelay;"
       transition_delay_position)
if(accepted_cursor_position GREATER next_record_position OR
   next_record_position GREATER transition_delay_position)
    message(FATAL_ERROR
        "Inter-container delay must be scheduled after ACK cursor persistence and next-record read")
endif()
string(REGEX MATCHALL "[+][+]container_item_cursor_|container_item_cursor_[+][+]"
       cursor_advances "${container_restore_source}")
list(LENGTH cursor_advances cursor_advance_count)
if(NOT cursor_advance_count EQUAL 1)
    message(FATAL_ERROR "Container cursor must advance exactly once, after an accepted ACK")
endif()

set(required_dispatch
    "executeTrackedCommands({formatContainerReplaceItemCommand(record)}, {uuid}, &sent_count)")
string(FIND "${container_restore_source}" "${required_dispatch}" dispatch_position)
if(dispatch_position EQUAL -1)
    message(FATAL_ERROR "Container /replaceitem is no longer sent as a tracked command")
endif()
