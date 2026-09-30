if(NOT DEFINED SOURCE_FILE OR NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "BuildImportRuntime source is unavailable")
endif()

file(READ "${SOURCE_FILE}" runtime_source)

set(reset_probe_start_marker "void BuildImportRuntime::resetSignShellProbe()")
set(reset_probe_end_marker "void BuildImportRuntime::resetSignRuntime()")
string(FIND "${runtime_source}" "${reset_probe_start_marker}" reset_probe_start)
string(FIND "${runtime_source}" "${reset_probe_end_marker}" reset_probe_end)
if(reset_probe_start EQUAL -1 OR reset_probe_end EQUAL -1 OR
   reset_probe_end LESS_EQUAL reset_probe_start)
    message(FATAL_ERROR "Cannot isolate sign shell probe cleanup")
endif()
math(EXPR reset_probe_length "${reset_probe_end} - ${reset_probe_start}")
string(SUBSTRING "${runtime_source}" ${reset_probe_start}
       ${reset_probe_length} reset_probe_source)
foreach(token IN ITEMS
        "uuid.swap(sign_shell_probe_uuid_);"
        "sign_shell_probe_poll_at_ = {};"
        "sign_shell_probe_deadline_ = {};"
        "received_rpc_acks_.erase(uuid);")
    string(FIND "${reset_probe_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "Sign shell probe cleanup is incomplete: ${token}")
    endif()
endforeach()

set(sign_start_marker "void BuildImportRuntime::tickSignWrite(")
set(sign_end_marker "void BuildImportRuntime::resetContainerRuntime()")
string(FIND "${runtime_source}" "${sign_start_marker}" sign_start)
string(FIND "${runtime_source}" "${sign_end_marker}" sign_end)
if(sign_start EQUAL -1 OR sign_end EQUAL -1 OR sign_end LESS_EQUAL sign_start)
    message(FATAL_ERROR "Cannot isolate the deferred sign restore state machine")
endif()
math(EXPR sign_length "${sign_end} - ${sign_start}")
string(SUBSTRING "${runtime_source}" ${sign_start} ${sign_length} sign_source)

set(required_tokens
    "containerItemPrepareCommands("
    "sign_cell_bounds_, record.x, record.y, record.z, !reuse_loaded_cell);"
    "sign_target_ready_at_ = now + kSignTargetSettleDelay;"
    "deferredSignShellMatches(record.expected_sign_id"
    "const CanonicalBlock expected_shell = placementBlock("
    "formatVerificationBlockArgument("
    "executeTrackedCommands("
    "{shell_probe_command}, {sign_shell_probe_uuid_}, &sent_count)"
    "pollTrackedCommands({sign_shell_probe_uuid_})"
    "state != RpcResultState::Accepted"
    "resetSignShellProbe();"
    "signRecordMatchesWithLightingNormalization("
    "ArmSignEditSession(token, record.x, record.y, record.z);"
    "ContainerOpenPacketSender::send(record.x, record.y, record.z,"
    "stage_ = ExecuteStage::SignOpenWait;"
    "PollSignEditSession(sign_edit_session_token_, &edit_result);"
    "SignBlockActorPacketSender::sendFace("
    "stage_ = ExecuteStage::SignFaceVerify;"
    "sign_face_includes_waxed_ = !other_face_pending;"
    "bool lighting_normalized = false;"
    "sign_verify_poll_at_ = now + kSignVerificationPollInterval;"
    "sign_verify_deadline_ = now + kSignVerificationTimeout;"
    "NativeWorldAccess::getBlockSnapshot(record.x, record.y, record.z"
    "matched = signRecordMatchesWithLightingNormalization("
    "retryOrPauseSignWrite(sign_last_verification_error_);"
)
foreach(token IN LISTS required_tokens)
    string(FIND "${sign_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "Sign restore safety invariant is missing: ${token}")
    endif()
endforeach()

# A legacy spool record has no expected Aux and must retain the old direct
# path. Modern records send exactly one tracked probe, wait without opening
# the editor, and clear its UUID before any block-actor write can begin.
string(FIND "${sign_source}" "if (record.has_expected_aux) {" aux_guard_position)
string(FIND "${sign_source}" "if (sign_shell_probe_uuid_.empty()) {"
       probe_dispatch_position)
string(FIND "${sign_source}"
       "if (state == RpcResultState::Pending &&" pending_position)
string(FIND "${sign_source}" "if (state != RpcResultState::Accepted) {"
       terminal_position)
string(FIND "${sign_source}" "NativeBlockSnapshot existing_snapshot;"
       block_actor_position)
if(aux_guard_position EQUAL -1 OR
   probe_dispatch_position LESS_EQUAL aux_guard_position OR
   pending_position LESS_EQUAL probe_dispatch_position OR
   terminal_position LESS_EQUAL pending_position OR
   block_actor_position LESS_EQUAL terminal_position)
    message(FATAL_ERROR
        "Tracked sign shell probe can be bypassed or leak into block-actor restore")
endif()
math(EXPR accepted_path_length "${block_actor_position} - ${terminal_position}")
string(SUBSTRING "${sign_source}" ${terminal_position}
       ${accepted_path_length} accepted_path_source)
string(FIND "${accepted_path_source}" "resetSignShellProbe();"
       accepted_reset_position)
if(accepted_reset_position EQUAL -1)
    message(FATAL_ERROR
        "Accepted sign shell probe is not cleared before block-actor restore")
endif()

string(FIND "${sign_source}"
       "block.aux != record.expected_aux" legacy_aux_comparison)
if(NOT legacy_aux_comparison EQUAL -1)
    message(FATAL_ERROR
        "Sign restore must not compare modern BlockState against legacy native Aux")
endif()

string(FIND "${sign_source}" "if (matched) {" matched_position)
string(FIND "${sign_source}" "++sign_cursor_;" cursor_position)
string(FIND "${sign_source}" "persistSignCursor(true, &state_error)" persist_position)
if(matched_position EQUAL -1 OR cursor_position LESS_EQUAL matched_position OR
   persist_position LESS_EQUAL cursor_position)
    message(FATAL_ERROR "Sign cursor can advance before verified readback")
endif()
string(REGEX MATCHALL "[+][+]sign_cursor_|sign_cursor_[+][+]"
       cursor_advances "${sign_source}")
list(LENGTH cursor_advances cursor_advance_count)
if(NOT cursor_advance_count EQUAL 1)
    message(FATAL_ERROR "Sign cursor must advance exactly once after a match")
endif()

set(final_start_marker "void BuildImportRuntime::finishFinalVerification(")
set(final_end_marker "void BuildImportRuntime::pauseFinalVerification(")
string(FIND "${runtime_source}" "${final_start_marker}" final_start)
string(FIND "${runtime_source}" "${final_end_marker}" final_end)
if(final_start EQUAL -1 OR final_end EQUAL -1 OR final_end LESS_EQUAL final_start)
    message(FATAL_ERROR "Cannot isolate deferred writer ordering")
endif()
math(EXPR final_length "${final_end} - ${final_start}")
string(SUBSTRING "${runtime_source}" ${final_start} ${final_length} final_source)
string(FIND "${final_source}" "beginCommandBlockWrite" command_position)
string(FIND "${final_source}" "beginSignWrite" sign_position)
string(FIND "${final_source}" "beginContainerWrite" container_position)
string(FIND "${final_source}" "beginEntityWrite" entity_position)
if(command_position EQUAL -1 OR sign_position LESS_EQUAL command_position OR
   container_position LESS_EQUAL sign_position OR entity_position LESS_EQUAL container_position)
    message(FATAL_ERROR
        "Deferred writer order must remain command block, sign, container, entity")
endif()
