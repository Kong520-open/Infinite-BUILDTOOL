import ast
import re
import struct
import sys
from pathlib import Path


def load_embedded_source():
    header_path = Path(__file__).resolve().parent.parent / "PyRpcEnvelopeBridge.h"
    header = header_path.read_text(encoding="utf-8")
    function = header.split("inline const char* rpcReusableEnvelopePythonSource()", 1)[1]
    returned = function.split("return", 1)[1].split(";\n}", 1)[0]
    literals = re.findall(r'"(?:\\.|[^"\\])*"', returned)
    return "".join(ast.literal_eval(literal) for literal in literals)


def pack_string(value):
    if isinstance(value, str):
        raw = value.encode("utf-8")
        size = len(raw)
        if size <= 31:
            return struct.pack(">B", 0xA0 | size) + raw
        if size <= 0xFF:
            return struct.pack(">BB", 0xD9, size) + raw
        if size <= 0xFFFF:
            return struct.pack(">BH", 0xDA, size) + raw
        return struct.pack(">BI", 0xDB, size) + raw
    if isinstance(value, bytes):
        size = len(value)
        if size <= 0xFF:
            return struct.pack(">BB", 0xC4, size) + value
        if size <= 0xFFFF:
            return struct.pack(">BH", 0xC5, size) + value
        return struct.pack(">BI", 0xC6, size) + value
    raise TypeError("unsupported string type")


def pack_integer(value):
    if value >= 0:
        if value <= 0x7F:
            return struct.pack(">B", value)
        if value <= 0xFF:
            return struct.pack(">BB", 0xCC, value)
        if value <= 0xFFFF:
            return struct.pack(">BH", 0xCD, value)
        if value <= 0xFFFFFFFF:
            return struct.pack(">BI", 0xCE, value)
        return struct.pack(">BQ", 0xCF, value)
    if value >= -32:
        return struct.pack(">B", 0x100 + value)
    if value >= -128:
        return struct.pack(">Bb", 0xD0, value)
    if value >= -32768:
        return struct.pack(">Bh", 0xD1, value)
    if value >= -2147483648:
        return struct.pack(">Bi", 0xD2, value)
    return struct.pack(">Bq", 0xD3, value)


def pack_container_header(count, fix_base, code16, code32):
    if count <= 15:
        return struct.pack(">B", fix_base | count)
    if count <= 0xFFFF:
        return struct.pack(">BH", code16, count)
    return struct.pack(">BI", code32, count)


def reference_pack(value):
    if isinstance(value, tuple):
        value = {"__type__": "tuple", "value": list(value)}
    if value is None:
        return b"\xC0"
    if value is True:
        return b"\xC3"
    if value is False:
        return b"\xC2"
    if isinstance(value, int):
        return pack_integer(value)
    if isinstance(value, (str, bytes)):
        return pack_string(value)
    if isinstance(value, list):
        return pack_container_header(len(value), 0x90, 0xDC, 0xDD) + b"".join(
            reference_pack(item) for item in value
        )
    if isinstance(value, dict):
        encoded = [pack_container_header(len(value), 0x80, 0xDE, 0xDF)]
        for key, item in value.items():
            encoded.append(reference_pack(key))
            encoded.append(reference_pack(item))
        return b"".join(encoded)
    raise TypeError("unsupported reference type: " + type(value).__name__)


def main():
    namespace = {
        "sys": sys,
        "struct": struct,
        "_build_import_rpc_pack": reference_pack,
    }
    exec(load_embedded_source(), namespace)
    fast = namespace["_build_import_rpc_pack_event"]
    legacy = namespace["_build_import_rpc_pack_legacy"]

    binary_uuid = b"infinitecz_build_silent"
    text_uuid = "infinitecz_build_tracked"
    cases = [
        (b"player", b"/fill 0 0 0 1 1 1 stone", binary_uuid),
        (b"player", b"x" * 255, binary_uuid),
        (b"player", b"x" * 256, binary_uuid),
        (b"player", b"same uuid text", "infinitecz_build_silent"),
        (b"player", "x" * 31, text_uuid),
        (b"player", "x" * 32, text_uuid),
        (b"player", "x" * 255, text_uuid),
        (b"player", "x" * 256, text_uuid),
        (b"player", "/say " + "\u4f60\u597d" * 100, text_uuid),
        (b"player", "x" * 65536, text_uuid),
    ]
    for player_id, command, command_uuid in cases:
        assert fast(player_id, command, command_uuid) == legacy(
            player_id, command, command_uuid
        )

    state = namespace["_build_import_rpc_template_state"][0]
    assert state is not False
    assert (4, 4) in state[5]
    assert (4, 0) in state[5]
    assert (5, 4) in state[5]
    assert (0, 0) in state[5]
    assert (1, 0) in state[5]
    assert (2, 0) in state[5]
    assert (3, 0) in state[5]

    # Python 2 considers some str/unicode pairs equal even though msgpack uses
    # different wire types. Equal content with a different Python type must
    # rebuild the playerId template instead of reusing fixed bytes.
    assert fast("player", "type transition", text_uuid) == legacy(
        "player", "type transition", text_uuid
    )
    assert type(namespace["_build_import_rpc_template_state"][0][0]) is str

    # A changed local-player identifier rebuilds the fixed portion of the
    # template and validates the new player type before reuse.
    assert fast(-4294967295, "/setblock 1 2 3 stone", text_uuid) == legacy(
        -4294967295, "/setblock 1 2 3 stone", text_uuid
    )

    # Unsupported dynamic types use the legacy packer without poisoning an
    # already calibrated string template.
    assert fast(-4294967295, 7, text_uuid) == legacy(-4294967295, 7, text_uuid)
    state = namespace["_build_import_rpc_template_state"][0]
    assert state is not False
    assert (0, 0) in state[5]


if __name__ == "__main__":
    main()
