if(NOT DEFINED SOURCE_FILE)
    message(FATAL_ERROR "SOURCE_FILE is required")
endif()

file(READ "${SOURCE_FILE}" native_world_access_source)

# Server Item components are not valid in the embedded client interpreter.
# Their native bindings can SIGSEGV before a Python exception is raised.
set(forbidden_tokens
    "mod.server.extraServerApi"
    "_infinitecz_snapshot_server_api"
    "_server_factory"
    "_server_item"
)
foreach(token IN LISTS forbidden_tokens)
    string(FIND "${native_world_access_source}" "${token}" token_position)
    if(NOT token_position EQUAL -1)
        message(FATAL_ERROR "Unsafe server-side container probe found: ${token}")
    endif()
endforeach()

set(required_tokens
    "hasattr(_component, 'GetContainerSize')"
    "hasattr(_component, 'GetContainerItem')"
    "_infinitecz_snapshot_factory.CreateItem(_client_player)"
    "_infinitecz_snapshot_client_inventory(_client_item)"
)
foreach(token IN LISTS required_tokens)
    string(FIND "${native_world_access_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "Client capability-gated container probe missing: ${token}")
    endif()
endforeach()
