"""Check the mover's native-call and request-layout gates against an ARM64 ELF.

Usage: python ProjectionPrinterNativeProfileTest.py path/to/libminecraftpe.so
This is read-only and needs no third-party packages or device. It verifies
addresses/bytes, the request packet's relocated vtable and writable request
counter, not C++ ABI semantics or in-game inventory behaviour. Dynamic
relocations are read from PT_DYNAMIC; section headers are not required.
"""

import argparse
import mmap
from pathlib import Path
import re
import struct


def file_offset(segments, address: int, length: int) -> int:
    matches = [
        segment[2] + address - segment[3]
        for segment in segments
        if segment[0] == 1 and segment[3] <= address
        and address + length <= segment[3] + segment[5]
    ]
    if len(matches) != 1:
        raise ValueError(f"{address:#x}: no unique file-backed load segment")
    return matches[0]


def verify_request_layout(image, segments, addresses) -> None:
    expected_slots = {
        addresses["kRequestPacketVtableRva"] + index * 8: addresses[name]
        for index, name in enumerate((
            "kRequestPacketDestructorRva",
            "kRequestPacketDeletingDestructorRva",
            "kRequestPacketIdFunctionRva",
        ))
    }
    dynamic_segments = [segment for segment in segments if segment[0] == 2]
    if len(dynamic_segments) != 1:
        raise ValueError("Expected exactly one PT_DYNAMIC segment")
    dynamic = dynamic_segments[0]
    tags = {}
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<QQ", image, offset)
        if tag == 0:
            break
        tags[tag] = value
    if not all(tag in tags for tag in (7, 8, 9)) or tags[9] != 24:
        raise ValueError("Expected native ELF64 DT_RELA relocation entries")
    if tags[8] % tags[9]:
        raise ValueError("Invalid DT_RELASZ")
    relocation_offset = file_offset(segments, tags[7], tags[8])
    found_slots = {}
    for offset in range(relocation_offset, relocation_offset + tags[8], tags[9]):
        address, info, addend = struct.unpack_from("<QQq", image, offset)
        if address not in expected_slots:
            continue
        if info != 1027:  # R_AARCH64_RELATIVE with symbol index zero.
            raise ValueError(f"Request vtable slot {address:#x}: unexpected relocation {info:#x}")
        if address in found_slots:
            raise ValueError(f"Request vtable slot {address:#x}: duplicate relocation")
        found_slots[address] = addend
    for address, expected in expected_slots.items():
        if found_slots.get(address) != expected:
            actual = found_slots.get(address)
            actual_text = "missing" if actual is None else hex(actual)
            raise ValueError(
                f"Request vtable slot {address:#x}: expected {expected:#x}, got {actual_text}"
            )
        if not any(
            segment[0] == 1 and segment[1] & 1 and segment[3] <= expected
            and expected + 4 <= segment[3] + segment[6]
            for segment in segments
        ):
            raise ValueError(f"Request vtable target {expected:#x}: not executable")

    counter = addresses["kRequestCounterRva"]
    # The counter is in .bss: use p_memsz, not p_filesz. A valid runtime
    # writable field need not have any bytes present in the ELF file.
    if counter % 4 or not any(
        segment[0] == 1 and segment[1] & 2 and segment[3] <= counter
        and counter + 4 <= segment[3] + segment[6]
        for segment in segments
    ):
        raise ValueError(f"Request counter {counter:#x}: not an aligned writable PT_LOAD field")


def verify_profile(image_path: Path, source_path: Path) -> int:
    source = source_path.read_text(encoding="utf-8")
    addresses = {
        name: int(value, 16)
        for name, value in re.findall(
            r"constexpr\s+uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)", source
        )
    }
    fingerprints = {}
    for length, name, body in re.findall(
        r"constexpr\s+std::array<uint8_t,\s*(\d+)U?>\s+(\w+)\s*\{\{(.*?)\}\};",
        source,
        re.S,
    ):
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})U?", body))
        if len(data) != int(length):
            raise ValueError(f"{name}: declared {length} bytes but found {len(data)}")
        fingerprints[name] = data
    pairs = set(re.findall(
        r"resolveAndMatch\(\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*,", source
    ))
    # A sender that this module already hooks compares its saved original
    # prologue instead of current executable bytes at runtime. Its static
    # profile still has an exact, same-named RVA to validate in the input ELF.
    for name in fingerprints:
        if name not in {fingerprint for _, fingerprint in pairs}:
            address_name = name.removesuffix("Fingerprint") + "Rva"
            if address_name in addresses:
                pairs.add((address_name, name))
    if not pairs:
        raise ValueError("No native-call gates found; do not treat an empty check as success")
    unchecked = set(fingerprints) - {fingerprint for _, fingerprint in pairs}
    if unchecked:
        raise ValueError(f"Fingerprints without a checked call gate: {sorted(unchecked)}")

    with image_path.open("rb") as image_file, mmap.mmap(
        image_file.fileno(), 0, access=mmap.ACCESS_READ
    ) as image:
        if image[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", image, 18)[0] != 183:
            raise ValueError("Expected a little-endian ELF64 AArch64 image")
        phoff = struct.unpack_from("<Q", image, 32)[0]
        phentsize, phnum = struct.unpack_from("<HH", image, 54)
        if phentsize < 56 or phoff + phentsize * phnum > len(image):
            raise ValueError("Invalid program-header table")
        segments = []
        executable_segments = []
        for index in range(phnum):
            segment = struct.unpack_from(
                "<IIQQQQQQ", image, phoff + index * phentsize
            )
            segments.append(segment)
            kind, flags, offset, address, _, size, _, _ = segment
            if kind == 1 and flags & 1:
                executable_segments.append((address, offset, size))
        for address_name, fingerprint_name in sorted(pairs):
            address = addresses[address_name]
            expected = fingerprints[fingerprint_name]
            offsets = [
                offset + address - start
                for start, offset, size in executable_segments
                if start <= address and address + len(expected) <= start + size
            ]
            if len(offsets) != 1:
                raise ValueError(f"{address_name}: no unique file-backed executable segment")
            actual = image[offsets[0]:offsets[0] + len(expected)]
            if actual != expected:
                raise ValueError(f"{address_name} at {address:#x}: fingerprint mismatch")
        verify_request_layout(image, segments, addresses)
    return len(pairs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--source", type=Path, default=(
        Path(__file__).resolve().parents[1] / "ProjectionPrinterInventoryMover.cpp"
    ))
    args = parser.parse_args()
    count = verify_profile(args.image, args.source)
    print(
        f"PASS: {count} native-call fingerprints, 3 request vtable slots and "
        "the writable request counter match the supplied ARM64 game image"
    )
